/*
 * nekos-desktop: the desktop layer -- wallpaper + ~/Desktop icons + a
 * right-click menu.
 *
 * A single _NET_WM_WINDOW_TYPE_DESKTOP window covering the whole screen, which
 * nekos-wm keeps at the bottom of the stack (undecorated, never focused -- see
 * map_desktop() in wm/src/client.c). Because it's a real bottom-most window,
 * clicks on empty desktop land here directly:
 *   - double-click an icon -> open it via nekos-open (folder/file, same as the
 *     file manager)
 *   - right-click empty space -> Terminal / Files / Settings menu (menu_show,
 *     shared with nekos-bar)
 *
 * It draws the wallpaper itself (reading the same ~/.config/nekos/wallpaper the
 * compositor uses) so the two stay consistent; a poll loop notices wallpaper
 * changes (e.g. from nekos-settings) and new/removed ~/Desktop files and
 * repaints only when something actually changed. The compositor's own
 * wallpaper remains the fallback for when this program isn't running.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "image.h"
#include "menu.h"
#include "theme.h"

#define BAR_H         32
#define TOP_MARGIN    (BAR_H + 20)
#define LEFT_MARGIN   24
#define CELL_W        104
#define CELL_H        100
#define ICON_PX       46
#define LABEL_GAP     6
#define MAX_ICONS     512
#define NAME_LEN      256
#define PATH_LEN      4096
#define REFRESH_MS    1500
#define DOUBLE_CLICK_MS 400

/* Icon categories -- KEEP IN SYNC with files/nekos-files.c (same taxonomy).
 * Consolidating these into a shared source is a fine later cleanup. */
typedef enum {
    CAT_DIR, CAT_IMAGE, CAT_TEXT, CAT_HTML, CAT_EXEC, CAT_GENERIC
} cat_t;

typedef struct {
    char name[NAME_LEN];
    int is_dir;
    cat_t cat;
} entry_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;

static xcb_atom_t net_wm_window_type;
static xcb_atom_t net_wm_window_type_desktop;

static int have_randr = 0;
static uint8_t randr_event_base = 0;

static int win_w, win_h;
static char desktop_dir[PATH_LEN];
static char wp_config[PATH_LEN];
static char open_tool[PATH_LEN + 32];

static entry_t icons[MAX_ICONS];
static int icon_count = 0;
static int sel_index = -1;
static int hover_index = -1;

static cairo_surface_t *wallpaper = NULL;
static long wp_mtime = -1;
static uint64_t icons_hash = 0;

static xcb_timestamp_t last_click_time = 0;
static int last_click_index = -1;

/* ---- helpers ------------------------------------------------------------ */

static xcb_atom_t intern(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

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

static void resolve_open_tool(void) {
    char exe[PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');   /* .../desktop/nekos-desktop -> .../desktop */
        if (slash) *slash = '\0';
        slash = strrchr(exe, '/');         /* .../desktop -> repo root */
        if (slash) *slash = '\0';
        snprintf(open_tool, sizeof(open_tool), "%s/bin/nekos-open", exe);
    } else {
        snprintf(open_tool, sizeof(open_tool), "bin/nekos-open");
    }
}

static void spawn(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
}

static void open_path(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(open_tool, "nekos-open", path, (char *)NULL);
        _exit(127);
    }
}

static int ext_matches(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++) if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static cat_t categorize(const char *name, int is_dir, mode_t mode) {
    if (is_dir) return CAT_DIR;
    static const char *const image_exts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tiff", ".tif",
        ".ppm", ".pnm", ".pgm", ".xpm", ".tga", NULL };
    static const char *const html_exts[] = { ".html", ".htm", ".pdf", ".svg", NULL };
    static const char *const text_exts[] = {
        ".txt", ".md", ".c", ".h", ".cpp", ".cc", ".hpp", ".sh", ".py", ".js",
        ".ts", ".css", ".json", ".xml", ".yaml", ".yml", ".toml", ".ini",
        ".conf", ".cfg", ".log", ".rs", ".go", ".lua", ".rb", ".pl", ".sql", NULL };
    if (ext_matches(name, image_exts)) return CAT_IMAGE;
    if (ext_matches(name, html_exts)) return CAT_HTML;
    if (ext_matches(name, text_exts)) return CAT_TEXT;
    if (mode & S_IXUSR) return CAT_EXEC;
    return CAT_GENERIC;
}

static int cmp_entries(const void *a, const void *b) {
    const entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

/* Rescans ~/Desktop into icons[] (dirs first). Returns a hash of the listing
 * so the poll loop can cheaply tell whether anything changed. */
static uint64_t scan_desktop(void) {
    icon_count = 0;
    DIR *d = opendir(desktop_dir);
    if (d) {
        struct dirent *e;
        while (icon_count < MAX_ICONS && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            entry_t *en = &icons[icon_count];
            strncpy(en->name, e->d_name, NAME_LEN - 1);
            en->name[NAME_LEN - 1] = '\0';

            char full[PATH_LEN + NAME_LEN + 2];
            snprintf(full, sizeof(full), "%s/%s", desktop_dir, en->name);
            struct stat st;
            if (lstat(full, &st) == 0) {
                en->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
                if (S_ISLNK(st.st_mode)) {
                    struct stat t;
                    if (stat(full, &t) == 0) en->is_dir = S_ISDIR(t.st_mode) ? 1 : 0;
                }
                en->cat = categorize(en->name, en->is_dir, st.st_mode);
            } else {
                en->is_dir = 0;
                en->cat = CAT_GENERIC;
            }
            icon_count++;
        }
        closedir(d);
    }
    qsort(icons, icon_count, sizeof(entry_t), cmp_entries);

    uint64_t h = 1469598103934665603ULL; /* FNV-ish over count + names */
    h ^= (uint64_t)icon_count; h *= 1099511628211ULL;
    for (int i = 0; i < icon_count; i++)
        for (const char *p = icons[i].name; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    return h;
}

/* Reads the first line of ~/.config/nekos/wallpaper into `out`. Returns 1 on
 * success (a path was read), 0 if unset/absent. */
static int read_wallpaper_path(char *out, size_t out_size) {
    FILE *f = fopen(wp_config, "r");
    if (!f) return 0;
    int ok = 0;
    if (fgets(out, (int)out_size, f)) {
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
        ok = len > 0;
    }
    fclose(f);
    return ok;
}

static void reload_wallpaper(void) {
    if (wallpaper) { cairo_surface_destroy(wallpaper); wallpaper = NULL; }
    char path[PATH_LEN];
    if (read_wallpaper_path(path, sizeof(path))) {
        wallpaper = image_load_scaled(path, win_w, win_h, /*cover=*/1);
    }
}

static long wallpaper_config_mtime(void) {
    struct stat st;
    return (stat(wp_config, &st) == 0) ? (long)st.st_mtime : -1;
}

/* ---- rendering ---------------------------------------------------------- */

/* Thumbnails for image files, cached by path+mtime so the 1.5s rescan loop
 * doesn't re-decode anything. Failed loads are cached too (as NULL) so a
 * broken image isn't retried on every repaint. */
#define THUMB_CACHE 64

typedef struct {
    char path[PATH_LEN + NAME_LEN + 2];
    long mtime;
    cairo_surface_t *surface; /* may be NULL: cached load failure */
} thumb_t;

static thumb_t thumb_cache[THUMB_CACHE];
static int thumb_used = 0;
static int thumb_evict = 0;

static cairo_surface_t *thumb_get(const char *name) {
    char full[PATH_LEN + NAME_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", desktop_dir, name);
    struct stat st;
    if (stat(full, &st) != 0) return NULL;

    for (int i = 0; i < thumb_used; i++) {
        if (strcmp(thumb_cache[i].path, full) == 0 && thumb_cache[i].mtime == (long)st.st_mtime)
            return thumb_cache[i].surface;
    }

    thumb_t *slot;
    if (thumb_used < THUMB_CACHE) {
        slot = &thumb_cache[thumb_used++];
    } else {
        slot = &thumb_cache[thumb_evict];
        thumb_evict = (thumb_evict + 1) % THUMB_CACHE;
        if (slot->surface) cairo_surface_destroy(slot->surface);
    }
    snprintf(slot->path, sizeof(slot->path), "%s", full);
    slot->mtime = (long)st.st_mtime;
    slot->surface = image_load_scaled(full, ICON_PX, ICON_PX, /*cover=*/1);
    return slot->surface;
}

/* Category icon in a `s`-pixel box at (x, y). KEEP IN SYNC with nekos-files. */
static void draw_icon(cairo_t *cr, double x, double y, double s, cat_t cat) {
    switch (cat) {
    case CAT_DIR:
        theme_rgb(cr, THEME_ICON_FOLDER);
        cairo_rectangle(cr, x, y + s * 0.15, s * 0.45, s * 0.2);
        cairo_fill(cr);
        cairo_rectangle(cr, x, y + s * 0.3, s, s * 0.55);
        cairo_fill(cr);
        break;
    case CAT_IMAGE:
        theme_rgb(cr, THEME_ICON_IMAGE_BG);
        cairo_rectangle(cr, x, y + s * 0.1, s, s * 0.8);
        cairo_fill(cr);
        theme_rgb(cr, THEME_ICON_IMAGE_SUN);
        cairo_arc(cr, x + s * 0.3, y + s * 0.35, s * 0.12, 0, 2 * 3.14159);
        cairo_fill(cr);
        theme_rgb(cr, THEME_ICON_IMAGE_HILL);
        cairo_move_to(cr, x, y + s * 0.9);
        cairo_line_to(cr, x + s * 0.5, y + s * 0.5);
        cairo_line_to(cr, x + s, y + s * 0.9);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case CAT_EXEC:
        theme_rgb(cr, THEME_ICON_EXEC_BG);
        cairo_rectangle(cr, x, y + s * 0.1, s, s * 0.8);
        cairo_fill(cr);
        theme_rgb(cr, THEME_ICON_EXEC_FG);
        cairo_set_line_width(cr, s * 0.08);
        cairo_move_to(cr, x + s * 0.25, y + s * 0.35);
        cairo_line_to(cr, x + s * 0.45, y + s * 0.5);
        cairo_line_to(cr, x + s * 0.25, y + s * 0.65);
        cairo_stroke(cr);
        break;
    default: /* CAT_TEXT / CAT_HTML / CAT_GENERIC */
        theme_rgb(cr, THEME_ICON_PAGE);
        cairo_rectangle(cr, x + s * 0.12, y + s * 0.05, s * 0.76, s * 0.9);
        cairo_fill(cr);
        if (cat == CAT_HTML) {
            theme_rgb(cr, THEME_ICON_HTML);
            cairo_set_line_width(cr, s * 0.07);
            cairo_move_to(cr, x + s * 0.42, y + s * 0.35);
            cairo_line_to(cr, x + s * 0.3, y + s * 0.5);
            cairo_line_to(cr, x + s * 0.42, y + s * 0.65);
            cairo_move_to(cr, x + s * 0.58, y + s * 0.35);
            cairo_line_to(cr, x + s * 0.7, y + s * 0.5);
            cairo_line_to(cr, x + s * 0.58, y + s * 0.65);
            cairo_stroke(cr);
        } else {
            theme_rgb(cr, THEME_ICON_TEXT_LINES);
            for (int i = 0; i < 3; i++) {
                cairo_rectangle(cr, x + s * 0.24, y + s * (0.3 + i * 0.18), s * 0.5, s * 0.06);
                cairo_fill(cr);
            }
        }
        break;
    }
}

static int columns(void) {
    int c = (win_w - LEFT_MARGIN) / CELL_W;
    return c < 1 ? 1 : c;
}

/* Draws one icon cell (selection/hover fill, icon or thumbnail, label). The
 * caller has already set the font. */
static void draw_cell(cairo_t *cr, int i, int cols) {
    int col = i % cols;
    int row = i / cols;
    double cx = LEFT_MARGIN + col * CELL_W;
    double cy = TOP_MARGIN + row * CELL_H;

    if (i == sel_index || i == hover_index) {
        theme_rgba(cr, THEME_ACCENT, i == sel_index ? 0.22 : THEME_HOVER_ALPHA);
        theme_rounded_rect(cr, cx - 2, cy - 2, CELL_W - 8, CELL_H - 10, THEME_RADIUS);
        cairo_fill(cr);
    }

    double ix = cx + (CELL_W - ICON_PX) / 2.0 - 4;
    cairo_surface_t *thumb = (icons[i].cat == CAT_IMAGE) ? thumb_get(icons[i].name) : NULL;
    if (thumb) {
        cairo_save(cr);
        theme_rounded_rect(cr, ix, cy, ICON_PX, ICON_PX, 5.0);
        cairo_clip(cr);
        cairo_set_source_surface(cr, thumb, ix, cy);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        draw_icon(cr, ix, cy, ICON_PX, icons[i].cat);
    }

    /* Label: centered under the icon, ellipsized to the cell, with a dark
     * shadow so it stays readable over any wallpaper. */
    cairo_text_extents_t ext;
    cairo_text_extents(cr, icons[i].name, &ext);
    double avail = CELL_W - 12;
    double tx = (ext.width < avail) ? cx + (CELL_W - 8 - ext.width) / 2.0 : cx + 2;
    double ty = cy + ICON_PX + LABEL_GAP + 10;

    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    theme_text_ellipsized(cr, tx + 1, ty + 1, avail, icons[i].name);
    theme_rgb(cr, THEME_FG);
    theme_text_ellipsized(cr, tx, ty, avail, icons[i].name);
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    if (wallpaper) {
        cairo_set_source_surface(cr, wallpaper, 0, 0);
        cairo_paint(cr);
    } else {
        theme_rgb(cr, THEME_BG); /* matches the compositor's flat fallback */
        cairo_paint(cr);
    }

    int cols = columns();
    theme_font(cr, THEME_FONT_SM, 0);

    for (int i = 0; i < icon_count; i++) {
        double cy = TOP_MARGIN + (i / cols) * CELL_H;
        if (cy > win_h) break;
        draw_cell(cr, i, cols);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* Repaints just one cell's region (hover feedback) so the X damage -- and
 * therefore what the VNC encoder has to re-send -- stays a ~100px rect
 * instead of the whole desktop. */
static void paint_cell(int i) {
    if (i < 0 || i >= icon_count) return;
    int cols = columns();
    double cx = LEFT_MARGIN + (i % cols) * CELL_W;
    double cy = TOP_MARGIN + (i / cols) * CELL_H;
    if (cy > win_h) return;

    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);
    cairo_rectangle(cr, cx - 4, cy - 4, CELL_W, CELL_H + 4);
    cairo_clip(cr);

    if (wallpaper) {
        cairo_set_source_surface(cr, wallpaper, 0, 0);
        cairo_paint(cr);
    } else {
        theme_rgb(cr, THEME_BG);
        cairo_paint(cr);
    }
    theme_font(cr, THEME_FONT_SM, 0);
    draw_cell(cr, i, cols);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- interaction -------------------------------------------------------- */

static int icon_at(int16_t px, int16_t py) {
    if (px < LEFT_MARGIN || py < TOP_MARGIN) return -1;
    int cols = columns();
    int col = (px - LEFT_MARGIN) / CELL_W;
    int row = (py - TOP_MARGIN) / CELL_H;
    if (col >= cols) return -1;
    int idx = row * cols + col;
    if (idx < 0 || idx >= icon_count) return -1;
    return idx;
}

/* -e /bin/bash rather than bare "sakura": root's login shell is /bin/sh
 * (dash on Void), which sakura would otherwise run and which never reads
 * ~/.bashrc (no history/tab-completion either -- dash has no readline at
 * all) -- explicitly running bash here is what makes the nekOS fastfetch
 * splash (installed into ~/.bashrc by install-fastfetch.sh) actually show
 * up in a new terminal. sakura replaced xterm (2026-07-24, nekos#36): real
 * Ctrl+Shift+C/V copy-paste instead of xterm's X-primary-selection-only
 * default, see provision/setup-void.sh's comment for the full reasoning. */
static const char *const argv_terminal[] = { "sakura", "-e", "/bin/bash", NULL };
static const char *const argv_files[]    = { "files/nekos-files", NULL };
static const char *const argv_settings[] = { "settings/nekos-settings", NULL };

static void action_spawn(void *argv) { spawn((const char *const *)argv); }

static const menu_item_t desktop_menu[] = {
    { "Terminal", action_spawn, (void *)(uintptr_t)argv_terminal },
    { "Files",    action_spawn, (void *)(uintptr_t)argv_files },
    { "Settings", action_spawn, (void *)(uintptr_t)argv_settings },
};

static void activate(int idx) {
    if (idx < 0 || idx >= icon_count) return;
    char full[PATH_LEN + NAME_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", desktop_dir, icons[idx].name);
    open_path(full);
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    if (ev->detail == 3) { /* right-click -> desktop menu */
        menu_show(conn, screen, visual, ev->root_x, ev->root_y,
                  desktop_menu, (int)(sizeof(desktop_menu) / sizeof(desktop_menu[0])));
        return;
    }
    if (ev->detail != 1) return;

    int idx = icon_at(ev->event_x, ev->event_y);
    if (idx < 0) { /* click on empty desktop -> deselect */
        if (sel_index != -1) {
            int old = sel_index;
            sel_index = -1;
            paint_cell(old);
        }
        last_click_index = -1;
        return;
    }

    int old_sel = sel_index;
    sel_index = idx;
    int is_double = (last_click_index == idx && (ev->time - last_click_time) < DOUBLE_CLICK_MS);
    last_click_time = ev->time;
    last_click_index = idx;
    if (old_sel != idx) paint_cell(old_sel);
    paint_cell(idx);
    if (is_double) { last_click_index = -1; activate(idx); }
}

/* Requests a self-resize to the full new screen size after a RandR change;
 * nekos-wm applies it (desktop-aware, undecorated) and the ConfigureNotify
 * drives the wallpaper reload + repaint. */
static void handle_screen_change(xcb_randr_screen_change_notify_event_t *ev) {
    if (ev->width == win_w && ev->height == win_h) return;
    uint32_t vals[2] = { ev->width, ev->height };
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, vals);
    xcb_flush(conn);
}

int main(void) {
    signal(SIGCHLD, SIG_IGN);

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-desktop: could not connect to X server\n");
        return 1;
    }

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    win_w = screen->width_in_pixels;
    win_h = screen->height_in_pixels;
    resolve_open_tool();

    const char *home = getenv("HOME");
    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop", home ? home : ".");
    mkdir(desktop_dir, 0755); /* best-effort; ensures there's always a Desktop */
    snprintf(wp_config, sizeof(wp_config), "%s/.config/nekos/wallpaper", home ? home : ".");

    net_wm_window_type = intern("_NET_WM_WINDOW_TYPE");
    net_wm_window_type_desktop = intern("_NET_WM_WINDOW_TYPE_DESKTOP");

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, win_w, win_h, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    /* Must be set before mapping so nekos-wm's map_request sees the type. */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_window_type,
                         XCB_ATOM_ATOM, 32, 1, &net_wm_window_type_desktop);

    /* RandR: re-span on desktop resize (same pattern nekos-bar uses). */
    const xcb_query_extension_reply_t *randr_ext = xcb_get_extension_data(conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        have_randr = 1;
        randr_event_base = randr_ext->first_event;
        free(xcb_randr_query_version_reply(
            conn, xcb_randr_query_version(conn, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION), NULL));
        xcb_randr_select_input(conn, screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }

    xcb_map_window(conn, win);
    xcb_flush(conn);

    wp_mtime = wallpaper_config_mtime();
    reload_wallpaper();
    icons_hash = scan_desktop();
    paint();

    int xfd = xcb_get_file_descriptor(conn);

    for (;;) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, REFRESH_MS);

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t rt = event->response_type & ~0x80;
            if (have_randr && rt == randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
                handle_screen_change((xcb_randr_screen_change_notify_event_t *)event);
                free(event);
                continue;
            }
            switch (rt) {
            case XCB_EXPOSE:
                paint();
                break;
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)event;
                if (ce->window == win && ce->width > 0 && ce->height > 0 &&
                    (ce->width != win_w || ce->height != win_h)) {
                    win_w = ce->width;
                    win_h = ce->height;
                    reload_wallpaper(); /* re-cover-scale to the new size */
                    paint();
                }
                break;
            }
            case XCB_BUTTON_PRESS:
                handle_button_press((xcb_button_press_event_t *)event);
                break;
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
                int idx = icon_at(me->event_x, me->event_y);
                if (idx != hover_index) {
                    int old = hover_index;
                    hover_index = idx;
                    paint_cell(old);
                    paint_cell(idx);
                }
                break;
            }
            case XCB_LEAVE_NOTIFY:
                if (hover_index != -1) {
                    int old = hover_index;
                    hover_index = -1;
                    paint_cell(old);
                }
                break;
            default:
                break;
            }
            free(event);
        }

        if (xcb_connection_has_error(conn)) break;

        /* Periodic refresh: pick up wallpaper changes (nekos-settings) and
         * added/removed ~/Desktop files, repainting only on actual change so
         * an idle desktop generates no VNC traffic. */
        int dirty = 0;
        long m = wallpaper_config_mtime();
        if (m != wp_mtime) { wp_mtime = m; reload_wallpaper(); dirty = 1; }
        uint64_t h = scan_desktop();
        if (h != icons_hash) {
            icons_hash = h;
            if (sel_index >= icon_count) sel_index = -1;
            if (hover_index >= icon_count) hover_index = -1;
            dirty = 1;
        }
        if (dirty) paint();
    }

    xcb_disconnect(conn);
    return 0;
}
