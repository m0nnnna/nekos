/*
 * nekos-settings: the nekOS settings app.
 *
 * A two-pane window: a sidebar of pages on the left, the active page on the
 * right. Pages:
 *   - Wallpaper: a thumbnail grid of images from the watched folder
 *     (~/nekos-wallpapers/ -- drop files in via \\wsl.localhost from Windows).
 *     Left-click sets the desktop wallpaper, right-click adds a decorative
 *     "frame" (see wm/src/client.c's frame_create()). Both are sent to
 *     nekos-wm via a custom property-then-ClientMessage pair (a
 *     ClientMessage's 20-byte payload can't hold an arbitrary file path).
 *     The currently-set wallpaper is highlighted.
 *   - About: system info (kernel, package count) and a shortcut into
 *     nekos-software.
 * A normal (non-dock) window -- gets nekos-wm's usual titlebar/close/move for
 * free, no special-casing needed anywhere in the WM for this binary.
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "image.h"
#include "theme.h"

#define INIT_W       560
#define INIT_H       460
#define SIDEBAR_W    140
#define SIDE_ROW_H   34
#define THUMB_W      112
#define THUMB_H      70
#define CELL_PAD     12
#define CELL_W       (THUMB_W + CELL_PAD)
#define CELL_H       (THUMB_H + 22)
#define HINT_H       26
#define MAX_FILES    64
#define NAME_MAX_LEN 128
#define PATH_LEN     512

typedef enum { PAGE_WALLPAPER, PAGE_PET, PAGE_ABOUT } page_t;

static const char *const page_names[] = { "Wallpaper", "Pet", "About" };
#define PAGE_COUNT 3

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;

static xcb_atom_t net_wm_name;
static xcb_atom_t utf8_string;
static xcb_atom_t nekos_set_wallpaper;
static xcb_atom_t nekos_wallpaper_path;
static xcb_atom_t nekos_add_frame;
static xcb_atom_t nekos_frame_path;

static int win_w = INIT_W;
static int win_h = INIT_H;

static page_t page = PAGE_WALLPAPER;
static int hover_page = -1;   /* sidebar row under pointer */
static int hover_cell = -1;   /* wallpaper cell under pointer */
static int hover_button = 0;  /* right pane's clickable (About button / Pet toggle) */
static int scroll_off = 0;    /* wallpaper grid scroll, in rows */

static char wallpapers_dir[PATH_LEN];
static char wp_config[PATH_LEN];
static char file_names[MAX_FILES][NAME_MAX_LEN];
static int file_count = 0;
static char current_wp[PATH_LEN] = "";

/* Thumbnails, cached by name+mtime (NULL = cached load failure). */
typedef struct {
    char name[NAME_MAX_LEN];
    long mtime;
    cairo_surface_t *surface;
} thumb_t;
static thumb_t thumbs[MAX_FILES];
static int thumb_used = 0;

/* Pet page: the cat is on unless the marker file exists (start-session.sh
 * checks the same file before launching nekos-pet). */
static char pet_disable_path[PATH_LEN];
static int pet_enabled = 1;

/* About-page facts, gathered once on first visit. */
static int about_loaded = 0;
static char about_kernel[128] = "";
static char about_pkgs[64] = "";

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

static int has_image_extension(const char *name) {
    static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif", NULL };
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    }
    return 0;
}

/* Rescans wallpapers_dir for image files. Truncates silently past MAX_FILES
 * (same accepted simplification nekos-bar's window list already uses). */
static void refresh_files(void) {
    file_count = 0;
    DIR *dir = opendir(wallpapers_dir);
    if (!dir) return;

    struct dirent *entry;
    while (file_count < MAX_FILES && (entry = readdir(dir))) {
        if (!has_image_extension(entry->d_name)) continue;
        strncpy(file_names[file_count], entry->d_name, NAME_MAX_LEN - 1);
        file_names[file_count][NAME_MAX_LEN - 1] = '\0';
        file_count++;
    }
    closedir(dir);
}

/* Reads the currently-set wallpaper path (for the highlight). */
static void refresh_current_wp(void) {
    current_wp[0] = '\0';
    FILE *f = fopen(wp_config, "r");
    if (!f) return;
    if (fgets(current_wp, sizeof(current_wp), f)) {
        size_t len = strlen(current_wp);
        while (len > 0 && (current_wp[len - 1] == '\n' || current_wp[len - 1] == '\r'))
            current_wp[--len] = '\0';
    }
    fclose(f);
}

static cairo_surface_t *thumb_get(const char *name) {
    char full[PATH_LEN + NAME_MAX_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", wallpapers_dir, name);
    struct stat st;
    if (stat(full, &st) != 0) return NULL;

    for (int i = 0; i < thumb_used; i++) {
        if (strcmp(thumbs[i].name, name) == 0 && thumbs[i].mtime == (long)st.st_mtime)
            return thumbs[i].surface;
    }

    thumb_t *slot = NULL;
    for (int i = 0; i < thumb_used; i++) {
        if (strcmp(thumbs[i].name, name) == 0) { slot = &thumbs[i]; break; } /* stale mtime */
    }
    if (!slot) {
        if (thumb_used < MAX_FILES) slot = &thumbs[thumb_used++];
        else slot = &thumbs[0]; /* tiny dir cap; arbitrary eviction is fine */
    }
    if (slot->surface) cairo_surface_destroy(slot->surface);
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->mtime = (long)st.st_mtime;
    slot->surface = image_load_scaled(full, THUMB_W, THUMB_H, /*cover=*/1);
    return slot->surface;
}

static void load_about(void) {
    if (about_loaded) return;
    about_loaded = 1;

    struct utsname un;
    if (uname(&un) == 0) {
        snprintf(about_kernel, sizeof(about_kernel), "%s %s", un.sysname, un.release);
    }

    FILE *p = popen("xbps-query -l 2>/dev/null | wc -l", "r");
    if (p) {
        long n = 0;
        if (fscanf(p, "%ld", &n) == 1 && n > 0) {
            snprintf(about_pkgs, sizeof(about_pkgs), "%ld packages installed", n);
        }
        pclose(p);
    }
}

/* ---- layout -------------------------------------------------------------- */

static int grid_cols(void) {
    int c = (win_w - SIDEBAR_W - CELL_PAD) / CELL_W;
    return c < 1 ? 1 : c;
}

static int grid_rows_visible(void) {
    int r = (win_h - HINT_H - CELL_PAD) / CELL_H;
    return r < 1 ? 1 : r;
}

/* Wallpaper cell index at window coords, or -1. */
static int cell_at(int16_t x, int16_t y) {
    if (page != PAGE_WALLPAPER) return -1;
    int gx = x - SIDEBAR_W - CELL_PAD;
    int gy = y - CELL_PAD;
    if (gx < 0 || gy < 0 || y >= win_h - HINT_H) return -1;
    int col = gx / CELL_W;
    int row = gy / CELL_H;
    if (col >= grid_cols()) return -1;
    int idx = (scroll_off + row) * grid_cols() + col;
    if (idx < 0 || idx >= file_count) return -1;
    return idx;
}

/* About page's button rect. */
#define BTN_W 150
#define BTN_H 32
static double btn_x(void) { return SIDEBAR_W + 24; }
static double btn_y(void) { return 190; }

/* Pet page's toggle row rect (checkbox + label share one click target). */
#define CHK_BOX   18
#define CHK_ROW_W 220
#define CHK_ROW_H 28
static double chk_x(void) { return SIDEBAR_W + 24; }
static double chk_y(void) { return 122; }

/* ---- painting ------------------------------------------------------------ */

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    theme_rgb(cr, THEME_BG);
    cairo_paint(cr);

    /* Sidebar. */
    theme_panel_gradient(cr, 0, 0, SIDEBAR_W, win_h, 1);
    theme_font(cr, THEME_FONT_MD, 0);
    for (int i = 0; i < PAGE_COUNT; i++) {
        double y = 8 + i * SIDE_ROW_H;
        if ((int)page == i) {
            theme_rgba(cr, THEME_ACCENT, 0.22);
            theme_rounded_rect(cr, 6, y, SIDEBAR_W - 12, SIDE_ROW_H - 4, THEME_RADIUS);
            cairo_fill(cr);
            theme_rgba(cr, THEME_ACCENT, 0.9);
            cairo_rectangle(cr, 6, y + 6, 3, SIDE_ROW_H - 16);
            cairo_fill(cr);
        } else if (hover_page == i) {
            theme_rgba(cr, THEME_ACCENT, THEME_HOVER_ALPHA);
            theme_rounded_rect(cr, 6, y, SIDEBAR_W - 12, SIDE_ROW_H - 4, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgba(cr, THEME_FG, (int)page == i ? 1.0 : 0.75);
        cairo_move_to(cr, 20, theme_baseline(y, SIDE_ROW_H - 4, THEME_FONT_MD));
        cairo_show_text(cr, page_names[i]);
    }
    theme_rgba(cr, THEME_ACCENT, 0.25);
    cairo_rectangle(cr, SIDEBAR_W - 1, 0, 1, win_h);
    cairo_fill(cr);

    if (page == PAGE_WALLPAPER) {
        int cols = grid_cols();
        int rows = grid_rows_visible();
        if (file_count == 0) {
            char msg[600];
            snprintf(msg, sizeof(msg), "Drop images into %s", wallpapers_dir);
            theme_font(cr, THEME_FONT_SM, 0);
            theme_rgba(cr, THEME_FG, 0.6);
            theme_text_ellipsized(cr, SIDEBAR_W + CELL_PAD, 40,
                                   win_w - SIDEBAR_W - 2 * CELL_PAD, msg);
        }
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = (scroll_off + r) * cols + c;
                if (idx >= file_count) break;
                double x = SIDEBAR_W + CELL_PAD + c * CELL_W;
                double y = CELL_PAD + r * CELL_H;

                char full[PATH_LEN + NAME_MAX_LEN + 2];
                snprintf(full, sizeof(full), "%s/%s", wallpapers_dir, file_names[idx]);
                int is_current = (current_wp[0] && strcmp(full, current_wp) == 0);

                cairo_surface_t *t = thumb_get(file_names[idx]);
                if (t) {
                    cairo_save(cr);
                    theme_rounded_rect(cr, x, y, THUMB_W, THUMB_H, THEME_RADIUS);
                    cairo_clip(cr);
                    cairo_set_source_surface(cr, t, x, y);
                    cairo_paint(cr);
                    cairo_restore(cr);
                } else {
                    theme_rgba(cr, THEME_MUTED, 0.4);
                    theme_rounded_rect(cr, x, y, THUMB_W, THUMB_H, THEME_RADIUS);
                    cairo_fill(cr);
                }

                if (is_current || idx == hover_cell) {
                    theme_rgba(cr, THEME_ACCENT, is_current ? 0.95 : 0.5);
                    cairo_set_line_width(cr, 2.0);
                    theme_rounded_rect(cr, x + 1, y + 1, THUMB_W - 2, THUMB_H - 2, THEME_RADIUS);
                    cairo_stroke(cr);
                }

                theme_font(cr, THEME_FONT_XS, 0);
                theme_rgba(cr, THEME_FG, 0.7);
                theme_text_ellipsized(cr, x, y + THUMB_H + 14, THUMB_W, file_names[idx]);
            }
        }

        /* Hint bar. */
        theme_rgba(cr, THEME_ACCENT, 0.25);
        cairo_rectangle(cr, SIDEBAR_W, win_h - HINT_H, win_w - SIDEBAR_W, 1);
        cairo_fill(cr);
        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_ACCENT, 0.7);
        cairo_move_to(cr, SIDEBAR_W + CELL_PAD, win_h - HINT_H / 2.0 + 3.0);
        cairo_show_text(cr, "Left-click: set wallpaper    Right-click: add frame");
    } else if (page == PAGE_PET) {
        double x = SIDEBAR_W + 24;

        theme_font(cr, 26.0, 1);
        theme_rgb(cr, THEME_ACCENT);
        cairo_move_to(cr, x, 56);
        cairo_show_text(cr, "Pet");
        theme_font(cr, THEME_FONT_MD, 0);
        theme_rgba(cr, THEME_FG, 0.8);
        cairo_move_to(cr, x, 80);
        cairo_show_text(cr, "The resident cat that chases your cursor.");

        theme_rgba(cr, THEME_ACCENT, 0.25);
        cairo_rectangle(cr, x, 96, win_w - x - 24, 1);
        cairo_fill(cr);

        /* Toggle row. */
        if (hover_button) {
            theme_rgba(cr, THEME_ACCENT, THEME_HOVER_ALPHA);
            theme_rounded_rect(cr, chk_x() - 6, chk_y() - 5, CHK_ROW_W, CHK_ROW_H, THEME_RADIUS);
            cairo_fill(cr);
        }
        double by = chk_y();
        if (pet_enabled) {
            theme_rgb(cr, THEME_ACCENT);
            theme_rounded_rect(cr, chk_x(), by, CHK_BOX, CHK_BOX, 4.0);
            cairo_fill(cr);
            theme_rgb(cr, THEME_BG);
            cairo_set_line_width(cr, 2.4);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_new_path(cr);
            cairo_move_to(cr, chk_x() + 4.0, by + 9.5);
            cairo_line_to(cr, chk_x() + 7.5, by + 13.0);
            cairo_line_to(cr, chk_x() + 14.0, by + 5.0);
            cairo_stroke(cr);
        } else {
            theme_rgba(cr, THEME_MUTED, 0.9);
            cairo_set_line_width(cr, 1.5);
            theme_rounded_rect(cr, chk_x() + 0.5, by + 0.5, CHK_BOX - 1, CHK_BOX - 1, 4.0);
            cairo_stroke(cr);
        }
        theme_font(cr, THEME_FONT_MD, 0);
        theme_rgb(cr, THEME_FG);
        cairo_move_to(cr, chk_x() + CHK_BOX + 10,
                      theme_baseline(by, CHK_BOX, THEME_FONT_MD));
        cairo_show_text(cr, "Show the cat");

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_FG, 0.55);
        cairo_move_to(cr, x, chk_y() + CHK_ROW_H + 18);
        cairo_show_text(cr, pet_enabled
                            ? "The cat naps, gives chase, and accepts pets (click it)."
                            : "The cat is away. It remembers this across sessions.");
    } else { /* PAGE_ABOUT */
        load_about();
        double x = SIDEBAR_W + 24;

        theme_font(cr, 26.0, 1);
        theme_rgb(cr, THEME_ACCENT);
        cairo_move_to(cr, x, 56);
        cairo_show_text(cr, "nekOS");
        theme_font(cr, THEME_FONT_MD, 0);
        theme_rgba(cr, THEME_FG, 0.8);
        cairo_move_to(cr, x, 80);
        cairo_show_text(cr, "neko-void \xC2\xB7 Void Linux in WSL2");

        theme_rgba(cr, THEME_ACCENT, 0.25);
        cairo_rectangle(cr, x, 96, win_w - x - 24, 1);
        cairo_fill(cr);

        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.7);
        double ty = 122;
        if (about_kernel[0]) {
            cairo_move_to(cr, x, ty);
            cairo_show_text(cr, about_kernel);
            ty += 22;
        }
        if (about_pkgs[0]) {
            cairo_move_to(cr, x, ty);
            cairo_show_text(cr, about_pkgs);
            ty += 22;
        }
        cairo_move_to(cr, x, ty);
        cairo_show_text(cr, "Desktop: nekos-wm (C / XCB / Cairo)");

        /* "Open Software" button. */
        theme_rgba(cr, THEME_ACCENT, hover_button ? 0.35 : 0.18);
        theme_rounded_rect(cr, btn_x(), btn_y(), BTN_W, BTN_H, THEME_RADIUS);
        cairo_fill(cr);
        theme_font(cr, THEME_FONT_MD, 0);
        theme_rgb(cr, THEME_FG);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, "Open Software", &ext);
        cairo_move_to(cr, btn_x() + (BTN_W - ext.x_advance) / 2.0,
                      theme_baseline(btn_y(), BTN_H, THEME_FONT_MD));
        cairo_show_text(cr, "Open Software");
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- actions ------------------------------------------------------------- */

/* Sets `path_atom` to the file's full path on root, then sends `msg_atom` as
 * a ClientMessage to root -- nekos-wm reads the property back on receipt.
 * See client_handle_client_message() in wm/src/client.c. */
static void send_path_message(xcb_atom_t path_atom, xcb_atom_t msg_atom, const char *full_path) {
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, path_atom,
                         XCB_ATOM_STRING, 8, (uint32_t)strlen(full_path), full_path);

    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = screen->root;
    ev.type = msg_atom;
    xcb_send_event(conn, 0, screen->root,
                    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                    (const char *)&ev);
    xcb_flush(conn);
}

static void spawn_software(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("software/nekos-software", "nekos-software", (char *)NULL);
        _exit(127);
    }
}

/* Relative path, same cwd-is-~/nekos convention as spawn_software(). */
static void spawn_pet(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("pet/nekos-pet", "nekos-pet", (char *)NULL);
        _exit(127);
    }
}

/* Applies the toggle now (start/kill the running cat) and persists it as the
 * presence/absence of the marker file start-session.sh checks. */
static void set_pet_enabled(int on) {
    pet_enabled = on;
    if (on) {
        unlink(pet_disable_path);
        spawn_pet();
    } else {
        FILE *f = fopen(pet_disable_path, "w");
        if (f) fclose(f);
        system("pkill -x nekos-pet 2>/dev/null");
    }
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    /* Sidebar page switch. */
    if (ev->event_x < SIDEBAR_W) {
        if (ev->detail != 1) return;
        int idx = (ev->event_y - 8) / SIDE_ROW_H;
        if (idx >= 0 && idx < PAGE_COUNT && (page_t)idx != page) {
            page = (page_t)idx;
            scroll_off = 0;
            hover_cell = -1;
            paint();
        }
        return;
    }

    if (page == PAGE_WALLPAPER) {
        /* Wheel scrolls the grid. */
        if (ev->detail == 4 || ev->detail == 5) {
            int cols = grid_cols();
            int total_rows = (file_count + cols - 1) / cols;
            int max_off = total_rows - grid_rows_visible();
            if (max_off < 0) max_off = 0;
            scroll_off += (ev->detail == 5) ? 1 : -1;
            if (scroll_off > max_off) scroll_off = max_off;
            if (scroll_off < 0) scroll_off = 0;
            paint();
            return;
        }

        int idx = cell_at(ev->event_x, ev->event_y);
        if (idx < 0) return;
        char full_path[PATH_LEN + NAME_MAX_LEN + 2];
        snprintf(full_path, sizeof(full_path), "%s/%s", wallpapers_dir, file_names[idx]);
        if (ev->detail == 1) {
            send_path_message(nekos_wallpaper_path, nekos_set_wallpaper, full_path);
            snprintf(current_wp, sizeof(current_wp), "%s", full_path);
            paint();
        } else if (ev->detail == 3) {
            send_path_message(nekos_frame_path, nekos_add_frame, full_path);
        }
    } else if (page == PAGE_PET) {
        if (ev->detail == 1 &&
            ev->event_x >= chk_x() - 6 && ev->event_x < chk_x() - 6 + CHK_ROW_W &&
            ev->event_y >= chk_y() - 5 && ev->event_y < chk_y() - 5 + CHK_ROW_H) {
            set_pet_enabled(!pet_enabled);
            paint();
        }
    } else { /* PAGE_ABOUT */
        if (ev->detail == 1 &&
            ev->event_x >= btn_x() && ev->event_x < btn_x() + BTN_W &&
            ev->event_y >= btn_y() && ev->event_y < btn_y() + BTN_H) {
            spawn_software();
        }
    }
}

static void handle_motion(int16_t x, int16_t y) {
    int hp = -1, hc = -1, hb = 0;
    if (x < SIDEBAR_W) {
        int idx = (y - 8) / SIDE_ROW_H;
        if (idx >= 0 && idx < PAGE_COUNT) hp = idx;
    } else if (page == PAGE_WALLPAPER) {
        hc = cell_at(x, y);
    } else if (page == PAGE_PET) {
        hb = (x >= chk_x() - 6 && x < chk_x() - 6 + CHK_ROW_W &&
              y >= chk_y() - 5 && y < chk_y() - 5 + CHK_ROW_H);
    } else {
        hb = (x >= btn_x() && x < btn_x() + BTN_W && y >= btn_y() && y < btn_y() + BTN_H);
    }
    if (hp != hover_page || hc != hover_cell || hb != hover_button) {
        hover_page = hp;
        hover_cell = hc;
        hover_button = hb;
        paint();
    }
}

int main(void) {
    signal(SIGCHLD, SIG_IGN); /* reap spawned nekos-software automatically */

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-settings: could not connect to X server\n");
        return 1;
    }

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);

    const char *home = getenv("HOME");
    snprintf(wallpapers_dir, sizeof(wallpapers_dir), "%s/nekos-wallpapers", home ? home : ".");
    mkdir(wallpapers_dir, 0755); /* best-effort; ignored if it already exists */
    snprintf(wp_config, sizeof(wp_config), "%s/.config/nekos/wallpaper", home ? home : ".");
    snprintf(pet_disable_path, sizeof(pet_disable_path), "%s/.config/nekos/pet-disabled",
             home ? home : ".");
    /* Usually seeded by start-session.sh's wallpaper default; be safe anyway. */
    char cfg_dir[PATH_LEN];
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config", home ? home : ".");
    mkdir(cfg_dir, 0755);
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config/nekos", home ? home : ".");
    mkdir(cfg_dir, 0755);
    pet_enabled = (access(pet_disable_path, F_OK) != 0);

    net_wm_name = intern("_NET_WM_NAME");
    utf8_string = intern("UTF8_STRING");
    nekos_set_wallpaper = intern("_NEKOS_SET_WALLPAPER");
    nekos_wallpaper_path = intern("_NEKOS_WALLPAPER_PATH");
    nekos_add_frame = intern("_NEKOS_ADD_FRAME");
    nekos_frame_path = intern("_NEKOS_FRAME_PATH");

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, INIT_W, INIT_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    static const char title[] = "Settings";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name,
                         utf8_string, 8, (uint32_t)(sizeof(title) - 1), title);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 8, (uint32_t)(sizeof(title) - 1), title);

    xcb_map_window(conn, win);
    xcb_flush(conn);

    refresh_files();
    refresh_current_wp();
    paint();

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn))) {
        switch (event->response_type & ~0x80) {
        case XCB_EXPOSE:
            refresh_files(); /* cheap enough to just rescan on every expose */
            refresh_current_wp();
            pet_enabled = (access(pet_disable_path, F_OK) != 0);
            paint();
            break;
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)event;
            if (ce->width > 0 && ce->height > 0 && (ce->width != win_w || ce->height != win_h)) {
                win_w = ce->width;
                win_h = ce->height;
                paint();
            }
            break;
        }
        case XCB_BUTTON_PRESS:
            handle_button_press((xcb_button_press_event_t *)event);
            break;
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
            handle_motion(me->event_x, me->event_y);
            break;
        }
        case XCB_LEAVE_NOTIFY:
            if (hover_page != -1 || hover_cell != -1 || hover_button) {
                hover_page = -1;
                hover_cell = -1;
                hover_button = 0;
                paint();
            }
            break;
        default:
            break;
        }
        free(event);
    }

    xcb_disconnect(conn);
    return 0;
}
