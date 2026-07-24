/*
 * nekos-launch: a dmenu-style application launcher.
 *
 * Scans .desktop files (user's ~/.local/share/applications overriding the
 * system /usr/share/applications), shows them in a searchable list, and
 * launches the picked one. Opened by nekos-bar's "Menu" button. An
 * override-redirect window that grabs the keyboard (like rofi/dmenu), so it's
 * modal and disappears on Escape or a click outside.
 *
 * Type to filter (substring match on the app name), Up/Down to move, Enter to
 * launch the selection, Escape to cancel. A single click on a row launches it.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "image.h"
#include "theme.h"

#define WIN_W        560
#define WIN_H        440
#define SEARCH_H     46
#define ROW_H        32
#define ICON_PX      22
#define MAX_APPS     1024
#define NAME_LEN     128
#define EXEC_LEN     512
#define ICON_LEN     256
#define QUERY_LEN    128
#define MAX_SEEN     2048

typedef struct {
    char name[NAME_LEN];
    char exec[EXEC_LEN];
    char icon_name[ICON_LEN]; /* raw Icon= value (theme name or absolute path) */
    cairo_surface_t *icon;    /* resolved+loaded, or NULL (paw-dot fallback) */
    int terminal;
} app_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;
static xcb_key_symbols_t *keysyms;
static int win_x, win_y;

static app_t apps[MAX_APPS];
static int app_count = 0;

static int filtered[MAX_APPS];
static int filtered_count = 0;
static int sel = 0;       /* index into filtered[] */
static int scroll_off = 0;
static char query[QUERY_LEN] = "";

/* ---- .desktop scanning --------------------------------------------------- */

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

/* Strips .desktop Exec field codes (%f %F %u %U %i %c %k %d %D %n %N %v %m)
 * in place -- we launch via sh so they'd just be stray literals otherwise. */
static void strip_field_codes(char *exec) {
    char *w = exec;
    for (char *r = exec; *r; r++) {
        if (r[0] == '%' && r[1]) {
            if (r[1] == '%') { *w++ = '%'; r++; continue; } /* %% -> literal % */
            r++; /* skip the code letter */
            continue;
        }
        *w++ = *r;
    }
    *w = '\0';
    /* trim trailing spaces */
    while (w > exec && w[-1] == ' ') *--w = '\0';
}

/* Parses one .desktop file. Returns 1 and fills *out on a launchable
 * Application entry (has Name+Exec, Type=Application, not NoDisplay/Hidden). */
static int parse_desktop(const char *path, app_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int in_entry = 0, is_app = 0, no_display = 0;
    out->name[0] = out->exec[0] = out->icon_name[0] = '\0';
    out->icon = NULL;
    out->terminal = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        if (line[0] == '[') {
            in_entry = (strcmp(line, "[Desktop Entry]") == 0);
            continue;
        }
        if (!in_entry) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line, *val = eq + 1;

        if (strcmp(key, "Type") == 0) is_app = (strcmp(val, "Application") == 0);
        else if (strcmp(key, "Name") == 0 && out->name[0] == '\0') {
            strncpy(out->name, val, NAME_LEN - 1);
            out->name[NAME_LEN - 1] = '\0';
        } else if (strcmp(key, "Exec") == 0 && out->exec[0] == '\0') {
            strncpy(out->exec, val, EXEC_LEN - 1);
            out->exec[EXEC_LEN - 1] = '\0';
        } else if (strcmp(key, "Icon") == 0 && out->icon_name[0] == '\0') {
            strncpy(out->icon_name, val, ICON_LEN - 1);
            out->icon_name[ICON_LEN - 1] = '\0';
        } else if (strcmp(key, "NoDisplay") == 0 || strcmp(key, "Hidden") == 0) {
            if (strcasecmp(val, "true") == 0) no_display = 1;
        } else if (strcmp(key, "Terminal") == 0) {
            out->terminal = (strcasecmp(val, "true") == 0);
        }
    }
    fclose(f);

    if (!is_app || no_display || out->name[0] == '\0' || out->exec[0] == '\0') return 0;
    strip_field_codes(out->exec);
    return out->exec[0] != '\0';
}

static int seen_count = 0;
static char seen[MAX_SEEN][NAME_LEN];

static int already_seen(const char *basename) {
    for (int i = 0; i < seen_count; i++)
        if (strcmp(seen[i], basename) == 0) return 1;
    if (seen_count < MAX_SEEN) {
        strncpy(seen[seen_count], basename, NAME_LEN - 1);
        seen[seen_count][NAME_LEN - 1] = '\0';
        seen_count++;
    }
    return 0;
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while (app_count < MAX_APPS && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 8, ".desktop") != 0) continue;
        if (already_seen(e->d_name)) continue; /* ~/.local overrides /usr/share */

        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (parse_desktop(path, &apps[app_count])) app_count++;
    }
    closedir(d);
}

static int cmp_apps(const void *a, const void *b) {
    return strcasecmp(((const app_t *)a)->name, ((const app_t *)b)->name);
}

/* Resolves a .desktop Icon= value to a loadable file. Absolute paths are used
 * as-is; bare names are looked up first in Papirus-Dark (the icon theme nekOS
 * ships for third-party apps -- SVGs, which imlib2's svg loader handles), then
 * the usual fixed-size hicolor dirs and /usr/share/pixmaps (no full
 * icon-theme spec walk -- this fixed list covers what packages actually
 * install). */
static int resolve_icon_path(const char *icon_name, char *out, size_t out_size) {
    if (icon_name[0] == '\0') return 0;
    if (icon_name[0] == '/') {
        snprintf(out, out_size, "%s", icon_name);
        return access(out, R_OK) == 0;
    }
    static const char *const patterns[] = {
        "/usr/share/icons/Papirus-Dark/24x24/apps/%s.svg",
        "/usr/share/icons/Papirus-Dark/48x48/apps/%s.svg",
        "/usr/share/icons/Papirus-Dark/32x32/apps/%s.svg",
        "/usr/share/icons/hicolor/48x48/apps/%s.png",
        "/usr/share/icons/hicolor/64x64/apps/%s.png",
        "/usr/share/icons/hicolor/32x32/apps/%s.png",
        "/usr/share/icons/hicolor/128x128/apps/%s.png",
        "/usr/share/icons/hicolor/scalable/apps/%s.png",
        "/usr/share/pixmaps/%s.png",
        "/usr/share/pixmaps/%s.xpm",
        NULL,
    };
    for (int i = 0; patterns[i]; i++) {
        snprintf(out, out_size, patterns[i], icon_name);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

static void load_icons(void) {
    for (int i = 0; i < app_count; i++) {
        char path[512];
        if (resolve_icon_path(apps[i].icon_name, path, sizeof(path))) {
            apps[i].icon = image_load_scaled(path, ICON_PX, ICON_PX, /*cover=*/0);
        }
    }
}

static void scan_all(void) {
    app_count = 0;
    seen_count = 0;
    const char *home = getenv("HOME");
    if (home) {
        char user_dir[1024];
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications", home);
        scan_dir(user_dir); /* first, so it wins the dedup */
    }
    scan_dir("/usr/share/applications");
    qsort(apps, app_count, sizeof(app_t), cmp_apps);
    load_icons();
}

/* ---- filtering / launching ---------------------------------------------- */

static void refilter(void) {
    filtered_count = 0;
    for (int i = 0; i < app_count; i++) {
        if (query[0] == '\0' || strcasestr(apps[i].name, query))
            filtered[filtered_count++] = i;
    }
    if (sel >= filtered_count) sel = filtered_count - 1;
    if (sel < 0) sel = 0;
    scroll_off = 0;
}

static void launch_selected(void) {
    if (sel < 0 || sel >= filtered_count) return;
    app_t *a = &apps[filtered[sel]];

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (a->terminal)
            execlp("xterm", "xterm", "-e", "sh", "-c", a->exec, (char *)NULL);
        else
            execlp("sh", "sh", "-c", a->exec, (char *)NULL);
        _exit(127);
    }
}

/* ---- rendering ----------------------------------------------------------- */

static int visible_rows(void) { return (WIN_H - SEARCH_H) / ROW_H; }

static void keep_sel_visible(void) {
    int rows = visible_rows();
    if (sel < scroll_off) scroll_off = sel;
    if (sel >= scroll_off + rows) scroll_off = sel - rows + 1;
    if (scroll_off < 0) scroll_off = 0;
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, WIN_W, WIN_H);
    cairo_t *cr = cairo_create(surface);

    theme_rgb(cr, THEME_BG);
    cairo_paint(cr);

    /* Search box, with a results count tucked into its right edge. */
    theme_rgba(cr, THEME_ACCENT, 0.10);
    theme_rounded_rect(cr, 8, 8, WIN_W - 16, SEARCH_H - 16, THEME_RADIUS);
    cairo_fill(cr);
    theme_font(cr, THEME_FONT_XL, 0);
    if (query[0]) {
        theme_rgb(cr, THEME_FG);
        cairo_move_to(cr, 18, SEARCH_H / 2.0 + 5.0);
        cairo_show_text(cr, query);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, query, &ext);
        cairo_rectangle(cr, 18 + ext.x_advance + 2, 13, 2, SEARCH_H - 26); /* cursor */
        cairo_fill(cr);
    } else {
        theme_rgba(cr, THEME_FG, 0.4);
        cairo_move_to(cr, 18, SEARCH_H / 2.0 + 5.0);
        cairo_show_text(cr, "Search apps...");
    }

    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d app%s", filtered_count,
             filtered_count == 1 ? "" : "s");
    theme_font(cr, THEME_FONT_XS, 0);
    theme_rgba(cr, THEME_FG, 0.35);
    cairo_text_extents_t cext;
    cairo_text_extents(cr, count_txt, &cext);
    cairo_move_to(cr, WIN_W - 18 - cext.x_advance, SEARCH_H / 2.0 + 4.0);
    cairo_show_text(cr, count_txt);

    theme_rgba(cr, THEME_ACCENT, THEME_BORDER_ALPHA);
    cairo_rectangle(cr, 0, SEARCH_H - 1, WIN_W, 1);
    cairo_fill(cr);

    if (filtered_count == 0) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.45);
        cairo_move_to(cr, 18, SEARCH_H + 30);
        cairo_show_text(cr, "no app by that name... maybe it's napping?");
    }

    int rows = visible_rows();
    for (int r = 0; r < rows; r++) {
        int idx = scroll_off + r;
        if (idx >= filtered_count) break;
        app_t *a = &apps[filtered[idx]];
        double y = SEARCH_H + r * ROW_H;

        if (idx == sel) {
            theme_rgba(cr, THEME_ACCENT, THEME_SELECT_ALPHA);
            theme_rounded_rect(cr, 4, y + 1, WIN_W - 8, ROW_H - 2, THEME_RADIUS);
            cairo_fill(cr);
        }

        /* App icon (from Icon=), or a paw-pad dot placeholder. */
        double icon_y = y + (ROW_H - ICON_PX) / 2.0;
        if (a->icon) {
            int iw = cairo_image_surface_get_width(a->icon);
            int ih = cairo_image_surface_get_height(a->icon);
            cairo_set_source_surface(cr, a->icon,
                                     14 + (ICON_PX - iw) / 2.0, icon_y + (ICON_PX - ih) / 2.0);
            cairo_paint(cr);
        } else {
            theme_rgba(cr, THEME_ACCENT, 0.5);
            cairo_arc(cr, 14 + ICON_PX / 2.0, y + ROW_H / 2.0, 4.0, 0, 6.2831853);
            cairo_fill(cr);
        }

        theme_font(cr, THEME_FONT_LG, 0);
        theme_rgb(cr, THEME_FG);
        theme_text_ellipsized(cr, 14 + ICON_PX + 10,
                               theme_baseline(y, ROW_H, THEME_FONT_LG),
                               WIN_W - (14 + ICON_PX + 10) - 14, a->name);
    }

    /* Outer border. */
    theme_rgba(cr, THEME_ACCENT, THEME_BORDER_ALPHA);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, WIN_W - 1, WIN_H - 1);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- input --------------------------------------------------------------- */

/* Returns 1 to keep running, 0 to exit (launch already happened or cancel). */
static int handle_key(xcb_key_press_event_t *ev) {
    xcb_keysym_t ks = xcb_key_press_lookup_keysym(keysyms, ev, (ev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0);

    if (ks == XK_Escape) return 0;
    if (ks == XK_Return || ks == XK_KP_Enter) { launch_selected(); return 0; }
    if (ks == XK_Up) { if (sel > 0) sel--; keep_sel_visible(); paint(); return 1; }
    if (ks == XK_Down) { if (sel < filtered_count - 1) sel++; keep_sel_visible(); paint(); return 1; }
    if (ks == XK_BackSpace) {
        size_t l = strlen(query);
        if (l > 0) { query[l - 1] = '\0'; refilter(); keep_sel_visible(); paint(); }
        return 1;
    }
    if (ks >= 0x20 && ks <= 0x7e) { /* printable Latin-1 */
        size_t l = strlen(query);
        if (l < QUERY_LEN - 1) { query[l] = (char)ks; query[l + 1] = '\0'; refilter(); keep_sel_visible(); paint(); }
        return 1;
    }
    return 1;
}

/* Row under a pointer at window-relative (x,y), or -1. */
static int row_at(int16_t x, int16_t y) {
    if (x < 0 || x >= WIN_W || y < SEARCH_H) return -1;
    int idx = scroll_off + (y - SEARCH_H) / ROW_H;
    if (idx < 0 || idx >= filtered_count) return -1;
    return idx;
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-launch: could not connect to X server\n");
        return 1;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    keysyms = xcb_key_symbols_alloc(conn);

    scan_all();
    refilter();

    win_x = (screen->width_in_pixels - WIN_W) / 2;
    win_y = (screen->height_in_pixels - WIN_H) / 3;
    if (win_x < 0) win_x = 0;
    if (win_y < 0) win_y = 0;

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[3] = {
        screen->black_pixel, 1,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       (int16_t)win_x, (int16_t)win_y, WIN_W, WIN_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    xcb_map_window(conn, win);
    xcb_flush(conn);

    /* Modal like dmenu: grab the keyboard for all typing, and the pointer so a
     * click outside dismisses us. */
    xcb_grab_keyboard(conn, 1, screen->root, XCB_CURRENT_TIME,
                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    /* The pointer grab can transiently fail with AlreadyGrabbed: we're spawned
     * by a click on nekos-bar's "Menu" button, and that press holds an implicit
     * pointer grab on the bar until the button is released -- which can still be
     * in effect when we start up and try to grab. If we gave up after one try
     * (the reply was previously ignored) we'd come up with no pointer grab, so
     * clicks outside the launcher would never reach us and it wouldn't close
     * when you click away -- even though the keyboard grab (nothing competes for
     * it) succeeded, leaving search still working. Retry until it lands; the
     * implicit grab clears the moment the mouse button comes up (a few ms). */
    for (int i = 0; i < 1000; i++) {
        xcb_grab_pointer_reply_t *pr = xcb_grab_pointer_reply(conn,
            xcb_grab_pointer(conn, 0, screen->root,
                             XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                             XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
        int ok = pr && pr->status == XCB_GRAB_STATUS_SUCCESS;
        free(pr);
        if (ok) break;
        poll(NULL, 0, 1); /* wait 1ms for the bar's implicit grab to release, then retry */
    }
    xcb_flush(conn);

    paint();

    /* Compositor damage-tracking for this override-redirect window races with
     * the first paint (same issue menu_show handles): poll with a short
     * timeout and repaint once ~40ms in so we appear promptly without needing
     * a mouse move first. */
    int xfd = xcb_get_file_descriptor(conn);
    int running = 1, settled = 0;
    while (running) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, settled ? -1 : 40);
        if (!settled) { paint(); settled = 1; }

        xcb_generic_event_t *event;
        while (running && (event = xcb_poll_for_event(conn))) {
            switch (event->response_type & ~0x80) {
            case XCB_EXPOSE:
                paint();
                break;
            case XCB_KEY_PRESS:
                running = handle_key((xcb_key_press_event_t *)event);
                break;
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
                int r = row_at((int16_t)(me->root_x - win_x), (int16_t)(me->root_y - win_y));
                if (r >= 0 && r != sel) { sel = r; paint(); }
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *be = (xcb_button_press_event_t *)event;
                int r = row_at((int16_t)(be->root_x - win_x), (int16_t)(be->root_y - win_y));
                if (r >= 0) { sel = r; launch_selected(); running = 0; }
                else if (be->root_x < win_x || be->root_x >= win_x + WIN_W ||
                         be->root_y < win_y || be->root_y >= win_y + WIN_H) {
                    running = 0; /* click outside -> cancel */
                }
                break;
            }
            default:
                break;
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;
    }

    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_key_symbols_free(keysyms);
    xcb_disconnect(conn);
    return 0;
}
