/*
 * nekos-software: native xbps front end (updates, search, install/remove).
 *
 * Two modes, one binary:
 *   nekos-software           the GUI (Updates / Installed / Search tabs)
 *   nekos-software --check   headless update check: syncs repodata, writes
 *                            "/tmp/nekos-updates" (first line: count, then one
 *                            pkgver per line) for nekos-bar's badge, and sends
 *                            a toast via nekos-notify when updates first
 *                            appear. Looped from start-session.sh.
 *
 * The session runs as root inside the nekos-void distro, so xbps-install/
 * xbps-remove are called directly -- no sudo/polkit layer.
 *
 * Long operations (install/remove/update/sync) run in a child process with
 * stdout+stderr streamed through a pipe into a log strip at the bottom of the
 * window; the X event loop keeps running (poll() on both fds), so the UI
 * never freezes mid-transaction. List queries (xbps-query -l / -Rs,
 * xbps-install -un) hit the locally cached repodata and are fast enough to
 * run synchronously.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "theme.h"

#define INIT_W       680
#define INIT_H       520
#define HEADER_H     48
#define TABS_H       34
#define ROW_H        34
#define LOG_H        118
#define LOG_LINES    6
#define LOG_LINE_LEN 200
#define BTN_W        84
#define BTN_H        24
#define MAX_PKGS     512
#define NAME_LEN     128
#define VER_LEN      64
#define DESC_LEN     256
#define QUERY_LEN    96
#define UPDATES_FILE "/tmp/nekos-updates"

typedef enum { TAB_UPDATES, TAB_INSTALLED, TAB_SEARCH } tab_t;
static const char *const tab_names[] = { "Updates", "Installed", "Search" };
#define TAB_COUNT 3

typedef struct {
    char name[NAME_LEN];
    char ver[VER_LEN];
    char desc[DESC_LEN];
    int installed;
} pkg_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;
static xcb_key_symbols_t *keysyms;

static int win_w = INIT_W;
static int win_h = INIT_H;

static tab_t tab = TAB_UPDATES;
static pkg_t pkgs[MAX_PKGS];
static int pkg_count = 0;
static int scroll_off = 0;
static char query[QUERY_LEN] = "";
static char status[256] = "";

static int hover_tab = -1;
static int hover_row = -1;      /* row whose action button is hovered */
static int hover_check = 0;     /* header "Check updates" button */
static int hover_update_all = 0;

/* Running operation (child process streaming into the log). */
static pid_t op_pid = 0;
static int op_fd = -1;
static char op_label[64] = "";
static char log_lines[LOG_LINES][LOG_LINE_LEN];
static int log_count = 0;
static char log_partial[LOG_LINE_LEN];
static int have_log = 0; /* show the log strip (op running or output kept) */

static char notify_tool[600];

/* ---- helpers ------------------------------------------------------------- */

static xcb_visualtype_t *find_visual(xcb_screen_t *scr, xcb_visualid_t id) {
    xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(scr);
    for (; depth_it.rem; xcb_depth_next(&depth_it)) {
        xcb_visualtype_iterator_t visual_it = xcb_depth_visuals_iterator(depth_it.data);
        for (; visual_it.rem; xcb_visualtype_next(&visual_it)) {
            if (visual_it.data->visual_id == id) return visual_it.data;
        }
    }
    return NULL;
}

/* Resolves <repo>/notify/nekos-notify from this executable's own location
 * (<repo>/software/nekos-software), so toasts work regardless of cwd. */
static void resolve_notify_tool(void) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        snprintf(notify_tool, sizeof(notify_tool), "%s/notify/nekos-notify", exe);
    } else {
        snprintf(notify_tool, sizeof(notify_tool), "notify/nekos-notify");
    }
}

static void send_toast(const char *title, const char *body) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(notify_tool, "nekos-notify", title, body, (char *)NULL);
        _exit(127);
    }
    /* The notify CLI just writes to a unix socket and exits -- reap it
     * synchronously so it never lingers as a zombie. */
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* Splits "pkgname-1.2_3" at the last '-' into name and version. */
static void split_pkgver(const char *pkgver, char *name, size_t name_size,
                          char *ver, size_t ver_size) {
    const char *dash = strrchr(pkgver, '-');
    if (dash && dash != pkgver) {
        size_t n = (size_t)(dash - pkgver);
        if (n >= name_size) n = name_size - 1;
        memcpy(name, pkgver, n);
        name[n] = '\0';
        snprintf(ver, ver_size, "%s", dash + 1);
    } else {
        snprintf(name, name_size, "%s", pkgver);
        ver[0] = '\0';
    }
}

/* ---- package queries (synchronous, local repodata) ----------------------- */

/* `xbps-install -un` dry run: one "pkgver action ..." line per pending
 * update. Returns the number of update/install lines parsed into pkgs[]
 * (when into_pkgs) or just counted. Optionally appends the pkgver lines to
 * `list` (for the updates file). */
static int query_updates(int into_pkgs, FILE *list) {
    int count = 0;
    if (into_pkgs) pkg_count = 0;
    FILE *p = popen("xbps-install -un 2>/dev/null", "r");
    if (!p) return 0;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        char pkgver[NAME_LEN + VER_LEN], action[32];
        if (sscanf(line, "%191s %31s", pkgver, action) != 2) continue;
        if (strcmp(action, "update") != 0 && strcmp(action, "install") != 0) continue;
        if (list) fprintf(list, "%s\n", pkgver);
        if (into_pkgs && pkg_count < MAX_PKGS) {
            pkg_t *pk = &pkgs[pkg_count++];
            split_pkgver(pkgver, pk->name, sizeof(pk->name), pk->ver, sizeof(pk->ver));
            snprintf(pk->desc, sizeof(pk->desc), "%s", action);
            pk->installed = 1;
        }
        count++;
    }
    pclose(p);
    return count;
}

/* `xbps-query -l`: "ii pkgver short description..." */
static void query_installed(void) {
    pkg_count = 0;
    FILE *p = popen("xbps-query -l 2>/dev/null", "r");
    if (!p) return;
    char line[1024];
    while (fgets(line, sizeof(line), p) && pkg_count < MAX_PKGS) {
        char state[8], pkgver[NAME_LEN + VER_LEN];
        int off = 0;
        if (sscanf(line, "%7s %191s %n", state, pkgver, &off) < 2) continue;
        pkg_t *pk = &pkgs[pkg_count++];
        split_pkgver(pkgver, pk->name, sizeof(pk->name), pk->ver, sizeof(pk->ver));
        snprintf(pk->desc, sizeof(pk->desc), "%s", line + off);
        size_t dl = strlen(pk->desc);
        while (dl > 0 && (pk->desc[dl - 1] == '\n' || pk->desc[dl - 1] == '\r')) pk->desc[--dl] = '\0';
        pk->installed = 1;
    }
    pclose(p);
}

/* `xbps-query -Rs term`: "[*] pkgver short description..." ([*] = installed). */
static void query_search(const char *term) {
    pkg_count = 0;
    if (!term[0]) return;

    /* term is user-typed: pass it via execline-safe single quoting. */
    char safe[QUERY_LEN * 4];
    int si = 0;
    for (const char *c = term; *c && si < (int)sizeof(safe) - 5; c++) {
        if (*c == '\'') { memcpy(safe + si, "'\\''", 4); si += 4; }
        else safe[si++] = *c;
    }
    safe[si] = '\0';

    char cmd[sizeof(safe) + 64];
    snprintf(cmd, sizeof(cmd), "xbps-query -Rs '%s' 2>/dev/null", safe);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    char line[1024];
    while (fgets(line, sizeof(line), p) && pkg_count < MAX_PKGS) {
        char marker[8], pkgver[NAME_LEN + VER_LEN];
        int off = 0;
        if (sscanf(line, "%7s %191s %n", marker, pkgver, &off) < 2) continue;
        pkg_t *pk = &pkgs[pkg_count++];
        split_pkgver(pkgver, pk->name, sizeof(pk->name), pk->ver, sizeof(pk->ver));
        snprintf(pk->desc, sizeof(pk->desc), "%s", line + off);
        size_t dl = strlen(pk->desc);
        while (dl > 0 && (pk->desc[dl - 1] == '\n' || pk->desc[dl - 1] == '\r')) pk->desc[--dl] = '\0';
        pk->installed = (strcmp(marker, "[*]") == 0);
    }
    pclose(p);
}

/* Rewrites UPDATES_FILE from a fresh dry run so nekos-bar's badge tracks
 * reality immediately after any operation. */
static void write_updates_file(void) {
    char tmp[] = UPDATES_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    /* Fixed-width placeholder so the real count can be patched in after the
     * package lines are streamed out (fscanf skips the padding fine). */
    fprintf(f, "%-8d\n", 0);
    int count = query_updates(0, f);
    rewind(f);
    fprintf(f, "%-8d", count);
    fclose(f);
    rename(tmp, UPDATES_FILE);
}

static void reload_tab(void) {
    scroll_off = 0;
    hover_row = -1;
    switch (tab) {
    case TAB_UPDATES:   query_updates(1, NULL); break;
    case TAB_INSTALLED: query_installed(); break;
    case TAB_SEARCH:    query_search(query); break;
    }
}

/* ---- async operations ---------------------------------------------------- */

static void log_append_chunk(const char *buf, ssize_t n) {
    for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        size_t l = strlen(log_partial);
        if (c == '\n' || l >= LOG_LINE_LEN - 1) {
            if (c != '\n' && l < LOG_LINE_LEN - 1) {
                log_partial[l] = c;
                log_partial[l + 1] = '\0';
            }
            if (log_partial[0]) {
                if (log_count == LOG_LINES) {
                    memmove(log_lines[0], log_lines[1], (LOG_LINES - 1) * LOG_LINE_LEN);
                    log_count--;
                }
                snprintf(log_lines[log_count++], LOG_LINE_LEN, "%s", log_partial);
                log_partial[0] = '\0';
            }
        } else if (c != '\r') {
            log_partial[l] = c;
            log_partial[l + 1] = '\0';
        }
    }
}

/* Starts `cmd` (sh -c) with stdout+stderr piped back to us. One at a time. */
static void start_op(const char *label, const char *cmd) {
    if (op_pid > 0) return;

    int fds[2];
    if (pipe(fds) != 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    op_pid = pid;
    op_fd = fds[0];
    snprintf(op_label, sizeof(op_label), "%s", label);
    log_count = 0;
    log_partial[0] = '\0';
    have_log = 1;
    snprintf(status, sizeof(status), "%s...", label);
}

static void paint(void);

static void finish_op(void) {
    int st = 0;
    waitpid(op_pid, &st, 0);
    close(op_fd);
    op_pid = 0;
    op_fd = -1;

    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    snprintf(status, sizeof(status), "%s %s", op_label, ok ? "done" : "FAILED (see log)");

    char body[128];
    snprintf(body, sizeof(body), "%s %s", op_label, ok ? "finished." : "failed.");
    send_toast("Software", body);

    write_updates_file(); /* keep the bar badge honest */
    reload_tab();
    paint();
}

/* ---- painting ------------------------------------------------------------ */

static int list_top(void) { return HEADER_H + TABS_H; }
static int list_bottom(void) { return win_h - (have_log ? LOG_H : 0); }

static int visible_rows(void) {
    int r = (list_bottom() - list_top()) / ROW_H;
    return r < 0 ? 0 : r;
}

/* Header buttons, right-aligned. */
#define HBTN_W 118
static double check_btn_x(void)  { return win_w - 2 * (HBTN_W + 8); }
static double update_btn_x(void) { return win_w - (HBTN_W + 8); }

static void draw_button(cairo_t *cr, double x, double y, double w, double h,
                         const char *text, unsigned color, int hovered, int enabled) {
    theme_rgba(cr, color, enabled ? (hovered ? 0.35 : 0.18) : 0.08);
    theme_rounded_rect(cr, x, y, w, h, THEME_RADIUS);
    cairo_fill(cr);
    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, enabled ? 1.0 : 0.35);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    cairo_move_to(cr, x + (w - ext.x_advance) / 2.0, theme_baseline(y, h, THEME_FONT_SM));
    cairo_show_text(cr, text);
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    theme_rgb(cr, THEME_BG);
    cairo_paint(cr);

    /* Header: search box + action buttons. */
    theme_panel_gradient(cr, 0, 0, win_w, HEADER_H, 1);
    double search_w = check_btn_x() - 24;
    theme_rgba(cr, THEME_ACCENT, 0.10);
    theme_rounded_rect(cr, 8, 9, search_w, HEADER_H - 18, THEME_RADIUS);
    cairo_fill(cr);
    theme_font(cr, THEME_FONT_LG, 0);
    if (query[0]) {
        theme_rgb(cr, THEME_FG);
        cairo_move_to(cr, 18, HEADER_H / 2.0 + 5.0);
        cairo_show_text(cr, query);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, query, &ext);
        cairo_rectangle(cr, 18 + ext.x_advance + 2, 14, 2, HEADER_H - 28);
        cairo_fill(cr);
    } else {
        theme_rgba(cr, THEME_FG, 0.4);
        cairo_move_to(cr, 18, HEADER_H / 2.0 + 5.0);
        cairo_show_text(cr, "Search packages (Enter)...");
    }

    int busy = (op_pid > 0);
    draw_button(cr, check_btn_x(), 10, HBTN_W, HEADER_H - 20, "Check updates",
                THEME_ACCENT, hover_check, !busy);
    char up_label[32];
    int n_updates = (tab == TAB_UPDATES) ? pkg_count : -1;
    if (n_updates >= 0) snprintf(up_label, sizeof(up_label), "Update all (%d)", n_updates);
    else snprintf(up_label, sizeof(up_label), "Update all");
    draw_button(cr, update_btn_x(), 10, HBTN_W, HEADER_H - 20, up_label,
                THEME_MAXIMIZE, hover_update_all, !busy);

    /* Tabs. */
    theme_font(cr, THEME_FONT_MD, 0);
    double tx = 8;
    for (int i = 0; i < TAB_COUNT; i++) {
        cairo_text_extents_t ext;
        cairo_text_extents(cr, tab_names[i], &ext);
        double tw = ext.x_advance + 24;
        if ((int)tab == i) {
            theme_rgba(cr, THEME_ACCENT, 0.18);
            theme_rounded_rect(cr, tx, HEADER_H + 4, tw, TABS_H - 8, THEME_RADIUS);
            cairo_fill(cr);
            theme_rgba(cr, THEME_ACCENT, 0.9);
            cairo_rectangle(cr, tx + 6, HEADER_H + TABS_H - 6, tw - 12, 2);
            cairo_fill(cr);
        } else if (hover_tab == i) {
            theme_rgba(cr, THEME_ACCENT, THEME_HOVER_ALPHA);
            theme_rounded_rect(cr, tx, HEADER_H + 4, tw, TABS_H - 8, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgba(cr, THEME_FG, (int)tab == i ? 1.0 : 0.7);
        cairo_move_to(cr, tx + 12, theme_baseline(HEADER_H, TABS_H, THEME_FONT_MD));
        cairo_show_text(cr, tab_names[i]);
        tx += tw + 4;
    }

    /* Status, right-aligned in the tabs row. */
    if (status[0]) {
        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_FG, 0.5);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, status, &ext);
        cairo_move_to(cr, win_w - ext.x_advance - 12,
                      theme_baseline(HEADER_H, TABS_H, THEME_FONT_XS));
        cairo_show_text(cr, status);
    }

    theme_rgba(cr, THEME_ACCENT, 0.25);
    cairo_rectangle(cr, 0, list_top() - 1, win_w, 1);
    cairo_fill(cr);

    /* Package list. */
    int rows = visible_rows();
    if (pkg_count == 0) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.45);
        const char *msg = (tab == TAB_UPDATES) ? "All patched up \xE2\x99\xA5"
                         : (tab == TAB_SEARCH && !query[0])
                             ? "Type a package name and press Enter."
                             : "Nothing here but paw prints.";
        cairo_move_to(cr, 16, list_top() + 28);
        cairo_show_text(cr, msg);
    }
    for (int r = 0; r < rows; r++) {
        int idx = scroll_off + r;
        if (idx >= pkg_count) break;
        pkg_t *pk = &pkgs[idx];
        double y = list_top() + r * ROW_H;

        if (idx == hover_row) {
            theme_rgba(cr, THEME_ACCENT, 0.06);
            theme_rounded_rect(cr, 3, y + 1, win_w - 6, ROW_H - 2, THEME_RADIUS);
            cairo_fill(cr);
        }

        theme_font(cr, THEME_FONT_MD, 1);
        theme_rgb(cr, THEME_FG);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, pk->name, &ext);
        double name_max = (win_w - BTN_W - 40) * 0.45;
        theme_text_ellipsized(cr, 14, theme_baseline(y, ROW_H, THEME_FONT_MD), name_max, pk->name);
        double nx = 14 + (ext.x_advance < name_max ? ext.x_advance : name_max) + 10;

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_ACCENT, 0.75);
        cairo_move_to(cr, nx, theme_baseline(y, ROW_H, THEME_FONT_XS));
        cairo_show_text(cr, pk->ver);
        cairo_text_extents(cr, pk->ver, &ext);
        nx += ext.x_advance + 12;

        theme_rgba(cr, THEME_FG, 0.55);
        theme_text_ellipsized(cr, nx, theme_baseline(y, ROW_H, THEME_FONT_XS),
                               win_w - BTN_W - 24 - nx, pk->desc);

        /* Action button. */
        const char *action = (tab == TAB_UPDATES) ? "Update"
                              : pk->installed ? "Remove" : "Install";
        unsigned color = (tab == TAB_UPDATES) ? THEME_MINIMIZE
                          : pk->installed ? THEME_CLOSE : THEME_MAXIMIZE;
        draw_button(cr, win_w - BTN_W - 12, y + (ROW_H - BTN_H) / 2.0, BTN_W, BTN_H,
                    action, color, idx == hover_row, !busy);
    }

    /* Scrollbar hint. */
    if (pkg_count > rows && rows > 0) {
        double track_h = list_bottom() - list_top();
        double thumb_h = track_h * ((double)rows / pkg_count);
        double thumb_y = list_top() + track_h * ((double)scroll_off / pkg_count);
        theme_rgba(cr, THEME_ACCENT, 0.35);
        cairo_rectangle(cr, win_w - 4, thumb_y, 3, thumb_h);
        cairo_fill(cr);
    }

    /* Log strip. */
    if (have_log) {
        double ly = win_h - LOG_H;
        theme_rgba(cr, THEME_ICON_EXEC_BG, 1.0);
        cairo_rectangle(cr, 0, ly, win_w, LOG_H);
        cairo_fill(cr);
        theme_rgba(cr, THEME_ACCENT, 0.4);
        cairo_rectangle(cr, 0, ly, win_w, 1);
        cairo_fill(cr);

        theme_font(cr, THEME_FONT_XS, 1);
        theme_rgba(cr, THEME_ACCENT, 0.8);
        cairo_move_to(cr, 10, ly + 16);
        cairo_show_text(cr, op_pid > 0 ? op_label : "Log");

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_ICON_EXEC_FG, 0.9);
        for (int i = 0; i < log_count; i++) {
            theme_text_ellipsized(cr, 10, ly + 32 + i * 14, win_w - 20, log_lines[i]);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- actions ------------------------------------------------------------- */

static void row_action(int idx) {
    if (op_pid > 0 || idx < 0 || idx >= pkg_count) return;
    pkg_t *pk = &pkgs[idx];
    char cmd[NAME_LEN + 64], label[NAME_LEN + 32];

    if (tab == TAB_UPDATES || !pk->installed) {
        snprintf(cmd, sizeof(cmd), "xbps-install -Sy '%s'", pk->name);
        snprintf(label, sizeof(label), "%s %s",
                 tab == TAB_UPDATES ? "Updating" : "Installing", pk->name);
    } else {
        snprintf(cmd, sizeof(cmd), "xbps-remove -Ry '%s'", pk->name);
        snprintf(label, sizeof(label), "Removing %s", pk->name);
    }
    start_op(label, cmd);
    paint();
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    /* Wheel. */
    if (ev->detail == 4 || ev->detail == 5) {
        int rows = visible_rows();
        int max_off = pkg_count - rows;
        if (max_off < 0) max_off = 0;
        scroll_off += (ev->detail == 5) ? 3 : -3;
        if (scroll_off > max_off) scroll_off = max_off;
        if (scroll_off < 0) scroll_off = 0;
        paint();
        return;
    }
    if (ev->detail != 1) return;

    /* Header buttons. */
    if (ev->event_y >= 10 && ev->event_y < HEADER_H - 10) {
        if (ev->event_x >= check_btn_x() && ev->event_x < check_btn_x() + HBTN_W) {
            if (op_pid == 0) {
                start_op("Syncing repositories", "xbps-install -S");
                paint();
            }
            return;
        }
        if (ev->event_x >= update_btn_x() && ev->event_x < update_btn_x() + HBTN_W) {
            if (op_pid == 0) {
                start_op("Updating system", "xbps-install -Suy");
                paint();
            }
            return;
        }
    }

    /* Tabs. */
    if (ev->event_y >= HEADER_H && ev->event_y < list_top()) {
        cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *mc = cairo_create(ms);
        theme_font(mc, THEME_FONT_MD, 0);
        double tx = 8;
        for (int i = 0; i < TAB_COUNT; i++) {
            cairo_text_extents_t ext;
            cairo_text_extents(mc, tab_names[i], &ext);
            double tw = ext.x_advance + 24;
            if (ev->event_x >= tx && ev->event_x < tx + tw) {
                if ((tab_t)i != tab) {
                    tab = (tab_t)i;
                    reload_tab();
                    paint();
                }
                break;
            }
            tx += tw + 4;
        }
        cairo_destroy(mc);
        cairo_surface_destroy(ms);
        return;
    }

    /* Row action buttons. */
    if (ev->event_y >= list_top() && ev->event_y < list_bottom()) {
        int idx = scroll_off + (ev->event_y - list_top()) / ROW_H;
        if (idx >= 0 && idx < pkg_count &&
            ev->event_x >= win_w - BTN_W - 12 && ev->event_x < win_w - 12) {
            row_action(idx);
        }
    }
}

static int handle_key(xcb_key_press_event_t *ev) {
    xcb_keysym_t ks = xcb_key_press_lookup_keysym(keysyms, ev,
                                                   (ev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0);
    if (ks == XK_Return || ks == XK_KP_Enter) {
        tab = TAB_SEARCH;
        reload_tab();
        paint();
        return 1;
    }
    if (ks == XK_BackSpace) {
        size_t l = strlen(query);
        if (l > 0) { query[l - 1] = '\0'; paint(); }
        return 1;
    }
    if (ks >= 0x20 && ks <= 0x7e) {
        size_t l = strlen(query);
        if (l < QUERY_LEN - 1) { query[l] = (char)ks; query[l + 1] = '\0'; paint(); }
        return 1;
    }
    return 1;
}

static void handle_motion(int16_t x, int16_t y) {
    int ht = -1, hr = -1, hc = 0, hu = 0;

    if (y >= 10 && y < HEADER_H - 10) {
        if (x >= check_btn_x() && x < check_btn_x() + HBTN_W) hc = 1;
        else if (x >= update_btn_x() && x < update_btn_x() + HBTN_W) hu = 1;
    } else if (y >= HEADER_H && y < list_top()) {
        cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *mc = cairo_create(ms);
        theme_font(mc, THEME_FONT_MD, 0);
        double tx = 8;
        for (int i = 0; i < TAB_COUNT; i++) {
            cairo_text_extents_t ext;
            cairo_text_extents(mc, tab_names[i], &ext);
            double tw = ext.x_advance + 24;
            if (x >= tx && x < tx + tw) { ht = i; break; }
            tx += tw + 4;
        }
        cairo_destroy(mc);
        cairo_surface_destroy(ms);
    } else if (y >= list_top() && y < list_bottom()) {
        int idx = scroll_off + (y - list_top()) / ROW_H;
        if (idx >= 0 && idx < pkg_count) hr = idx;
    }

    if (ht != hover_tab || hr != hover_row || hc != hover_check || hu != hover_update_all) {
        hover_tab = ht;
        hover_row = hr;
        hover_check = hc;
        hover_update_all = hu;
        paint();
    }
}

/* ---- headless check mode ------------------------------------------------- */

static int run_check(void) {
    resolve_notify_tool();

    /* Previous count (for the 0 -> N toast edge). */
    int old_count = -1;
    FILE *f = fopen(UPDATES_FILE, "r");
    if (f) {
        if (fscanf(f, "%d", &old_count) != 1) old_count = -1;
        fclose(f);
    }

    /* Sync repodata, then dry-run. Sync failures (offline) leave the cached
     * repodata in place; the dry run still reflects the last-known state. */
    int rc = system("xbps-install -S >/dev/null 2>&1");
    (void)rc;

    char tmp[] = UPDATES_FILE ".tmp";
    FILE *out = fopen(tmp, "w");
    if (!out) return 1;
    fprintf(out, "%-8d\n", 0);
    int count = query_updates(0, out);
    rewind(out);
    fprintf(out, "%-8d", count);
    fclose(out);
    rename(tmp, UPDATES_FILE);

    if (count > 0 && old_count <= 0) {
        char body[96];
        snprintf(body, sizeof(body), "%d package%s can be updated.", count, count == 1 ? "" : "s");
        send_toast("Updates available", body);
    }
    printf("nekos-software: %d update(s) pending\n", count);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_DFL); /* every child (ops, toasts) is waitpid()ed explicitly */
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2 && strcmp(argv[1], "--check") == 0) {
        return run_check();
    }

    resolve_notify_tool();

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-software: could not connect to X server\n");
        return 1;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    keysyms = xcb_key_symbols_alloc(conn);

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, INIT_W, INIT_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

    static const char title[] = "Software";
    xcb_atom_t net_wm_name;
    xcb_atom_t utf8_string;
    {
        xcb_intern_atom_reply_t *r1 = xcb_intern_atom_reply(
            conn, xcb_intern_atom(conn, 0, 12, "_NET_WM_NAME"), NULL);
        xcb_intern_atom_reply_t *r2 = xcb_intern_atom_reply(
            conn, xcb_intern_atom(conn, 0, 11, "UTF8_STRING"), NULL);
        net_wm_name = r1 ? r1->atom : XCB_ATOM_NONE;
        utf8_string = r2 ? r2->atom : XCB_ATOM_NONE;
        free(r1);
        free(r2);
    }
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name,
                         utf8_string, 8, (uint32_t)(sizeof(title) - 1), title);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 8, (uint32_t)(sizeof(title) - 1), title);

    xcb_map_window(conn, win);
    xcb_flush(conn);

    reload_tab();
    if (tab == TAB_UPDATES && pkg_count == 0) {
        snprintf(status, sizeof(status), "No cached updates -- try Check updates");
    }
    paint();

    int xfd = xcb_get_file_descriptor(conn);

    for (;;) {
        struct pollfd pfds[2] = {
            { .fd = xfd, .events = POLLIN, .revents = 0 },
            { .fd = op_fd, .events = POLLIN, .revents = 0 },
        };
        poll(pfds, op_fd >= 0 ? 2 : 1, -1);

        /* Streamed op output. */
        if (op_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            ssize_t n;
            int eof = 0;
            while ((n = read(op_fd, buf, sizeof(buf))) != 0) {
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    eof = 1;
                    break;
                }
                log_append_chunk(buf, n);
            }
            if (n == 0) eof = 1;
            if (eof) finish_op();
            else paint();
        }

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            switch (event->response_type & ~0x80) {
            case XCB_EXPOSE:
                paint();
                break;
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)event;
                if (ce->width > 0 && ce->height > 0 &&
                    (ce->width != win_w || ce->height != win_h)) {
                    win_w = ce->width;
                    win_h = ce->height;
                    paint();
                }
                break;
            }
            case XCB_BUTTON_PRESS:
                handle_button_press((xcb_button_press_event_t *)event);
                break;
            case XCB_KEY_PRESS:
                handle_key((xcb_key_press_event_t *)event);
                break;
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
                handle_motion(me->event_x, me->event_y);
                break;
            }
            case XCB_LEAVE_NOTIFY:
                if (hover_tab != -1 || hover_row != -1 || hover_check || hover_update_all) {
                    hover_tab = -1;
                    hover_row = -1;
                    hover_check = 0;
                    hover_update_all = 0;
                    paint();
                }
                break;
            default:
                break;
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;
    }

    xcb_key_symbols_free(keysyms);
    xcb_disconnect(conn);
    return 0;
}
