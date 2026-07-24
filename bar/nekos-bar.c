/*
 * nekos-bar: taskbar/launcher/panel client.
 *
 * Proves the EWMH strut mechanism end-to-end: sets _NET_WM_WINDOW_TYPE_DOCK
 * and _NET_WM_STRUT_PARTIAL reserving space at the top of the screen, so
 * nekos-wm leaves that strip alone when placing normal windows. Also is the
 * functional taskbar: a window list (click to focus/restore via
 * _NET_ACTIVE_WINDOW, right-click for a Close/Minimize/Maximize context
 * menu) with per-window icons (_NET_WM_ICON) and an active-window highlight,
 * an application menu button (opens nekos-launch), an xbps updates badge
 * (fed by nekos-software --check via /tmp/nekos-updates), a live clock+date,
 * and a power button. Hover feedback throughout (motion/leave events; the
 * bar repaints only when the hovered target actually changes).
 */

#define _POSIX_C_SOURCE 200809L /* localtime_r */

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "menu.h"
#include "theme.h"

#define BAR_HEIGHT   32
#define MENU_WIDTH   90
#define SLOT_WIDTH   150
#define TRAY_WIDTH   50  /* the power button (was a systray placeholder -- no
                           * XEmbed/_NET_SYSTEM_TRAY protocol; revisit if
                           * anything ever needs a tray icon). */
#define BADGE_WIDTH  56  /* xbps updates badge; occupies space only when updates exist */
#define CLOCK_WIDTH  110
#define MAX_SLOTS    32
#define LABEL_MAX    64
#define SLOT_ICON_PX 16
#define UPDATES_FILE "/tmp/nekos-updates"

typedef struct {
    xcb_window_t window;
    char label[LABEL_MAX];
    int minimized;
    cairo_surface_t *icon; /* _NET_WM_ICON as ARGB32, native size; NULL if none */
    int icon_w, icon_h;
} slot_t;

/* What the pointer is currently over (hover feedback). */
typedef enum {
    HOVER_NONE,
    HOVER_MENU,
    HOVER_SLOT,
    HOVER_BADGE,
    HOVER_POWER,
} hover_zone_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;
static uint16_t bar_width;

static xcb_atom_t net_wm_name;
static xcb_atom_t utf8_string;
static xcb_atom_t net_client_list;
static xcb_atom_t net_active_window;
static xcb_atom_t net_close_window;
static xcb_atom_t net_wm_state;
static xcb_atom_t net_wm_state_hidden;
static xcb_atom_t net_wm_state_maximized_vert;
static xcb_atom_t net_wm_state_maximized_horz;
static xcb_atom_t net_wm_icon;
/* Retained at file scope (not just in main) so set_struts() can re-publish the
 * struts after the bar re-spans on a screen-size change. */
static xcb_atom_t net_wm_strut_partial;
static xcb_atom_t net_wm_strut;

static int have_randr = 0;
static uint8_t randr_event_base = 0;

static slot_t slots[MAX_SLOTS];
static int slot_count = 0;
static xcb_window_t active_window = XCB_NONE;

static hover_zone_t hover_zone = HOVER_NONE;
static int hover_slot = -1;

static int update_count = 0; /* pending xbps updates (from UPDATES_FILE) */
static long updates_mtime = -1;

static char clock_time[16] = "";
static char clock_date[16] = "";
static long last_clock_minute = -1;

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

/* Labels a window via _NET_WM_NAME/UTF8_STRING, falling back to core
 * WM_NAME. Naive ASCII/Latin1 passthrough -- no COMPOUND_TEXT decoding. */
static void label_window(xcb_window_t window, char *out, size_t out_size) {
    out[0] = '\0';

    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, window, net_wm_name, utf8_string, 0, out_size - 1);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (reply && xcb_get_property_value_length(reply) > 0) {
        int len = xcb_get_property_value_length(reply);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, xcb_get_property_value(reply), (size_t)len);
        out[len] = '\0';
        free(reply);
        return;
    }
    free(reply);

    cookie = xcb_get_property(conn, 0, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, out_size - 1);
    reply = xcb_get_property_reply(conn, cookie, NULL);
    if (reply && xcb_get_property_value_length(reply) > 0) {
        int len = xcb_get_property_value_length(reply);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, xcb_get_property_value(reply), (size_t)len);
        out[len] = '\0';
    }
    free(reply);
}

/* True if the window's _NET_WM_STATE currently includes HIDDEN (minimized). */
static int window_minimized(xcb_window_t window) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, window, net_wm_state, XCB_ATOM_ATOM, 0, 16);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) return 0;
    int hidden = 0;
    int count = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
    xcb_atom_t *atoms = (xcb_atom_t *)xcb_get_property_value(reply);
    for (int i = 0; i < count; i++) {
        if (atoms[i] == net_wm_state_hidden) { hidden = 1; break; }
    }
    free(reply);
    return hidden;
}

/* Fetches _NET_WM_ICON: a CARDINAL array of (width, height, ARGB pixels...)
 * blocks. Picks the smallest icon at least SLOT_ICON_PX wide (or the largest
 * available), converts the non-premultiplied ARGB rows into a premultiplied
 * CAIRO_FORMAT_ARGB32 surface. Returns NULL (no icon) quietly. */
static cairo_surface_t *fetch_icon(xcb_window_t window, int *out_w, int *out_h) {
    /* 64x64 ARGB is 16K CARDINALs; cap the fetch generously. */
    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, window, net_wm_icon, XCB_ATOM_CARDINAL, 0, 128 * 1024);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) return NULL;

    int len = xcb_get_property_value_length(reply) / (int)sizeof(uint32_t);
    uint32_t *data = (uint32_t *)xcb_get_property_value(reply);

    uint32_t *best = NULL;
    int best_w = 0, best_h = 0;
    int i = 0;
    while (i + 2 <= len) {
        int w = (int)data[i], h = (int)data[i + 1];
        if (w <= 0 || h <= 0 || i + 2 + w * h > len) break;
        int better;
        if (!best) better = 1;
        else if (best_w < SLOT_ICON_PX) better = (w > best_w);
        else better = (w >= SLOT_ICON_PX && w < best_w);
        if (better) { best = data + i + 2; best_w = w; best_h = h; }
        i += 2 + w * h;
    }
    if (!best) { free(reply); return NULL; }

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        free(reply);
        return NULL;
    }
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < best_h; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < best_w; x++) {
            uint32_t px = best[y * best_w + x];
            uint32_t a = (px >> 24) & 0xFF, r = (px >> 16) & 0xFF,
                     g = (px >> 8) & 0xFF, b = px & 0xFF;
            /* premultiply for cairo */
            row[x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
    }
    cairo_surface_mark_dirty(s);
    free(reply);
    *out_w = best_w;
    *out_h = best_h;
    return s;
}

/* Re-fetches _NET_CLIENT_LIST from root and labels each window. Reuses
 * already-fetched icons for windows that are still present, fetches icons for
 * new ones, and frees icons of departed windows. Also selects PROPERTY_CHANGE
 * on each listed window (our connection only) so title/minimize changes
 * update the bar live. Called on startup and whenever root's
 * _NET_CLIENT_LIST property changes. */
static void refresh_slots(void) {
    slot_t old[MAX_SLOTS];
    int old_count = slot_count;
    memcpy(old, slots, sizeof(slots));

    slot_count = 0;

    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, screen->root, net_client_list, XCB_ATOM_WINDOW, 0, MAX_SLOTS);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (reply) {
        int count = xcb_get_property_value_length(reply) / (int)sizeof(xcb_window_t);
        xcb_window_t *windows = (xcb_window_t *)xcb_get_property_value(reply);
        for (int i = 0; i < count && i < MAX_SLOTS; i++) {
            slot_t *s = &slots[slot_count];
            s->window = windows[i];
            s->icon = NULL;
            s->icon_w = s->icon_h = 0;
            label_window(windows[i], s->label, sizeof(s->label));
            s->minimized = window_minimized(windows[i]);

            /* Steal the icon from the old list if this window was already
             * there (avoids a refetch on every list change). */
            for (int j = 0; j < old_count; j++) {
                if (old[j].window == windows[i] && old[j].icon) {
                    s->icon = old[j].icon;
                    s->icon_w = old[j].icon_w;
                    s->icon_h = old[j].icon_h;
                    old[j].icon = NULL;
                    break;
                }
            }
            if (!s->icon) s->icon = fetch_icon(windows[i], &s->icon_w, &s->icon_h);

            uint32_t prop_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_change_window_attributes(conn, windows[i], XCB_CW_EVENT_MASK, &prop_mask);

            slot_count++;
        }
        free(reply);
    }

    /* Free icons whose windows are gone. */
    for (int j = 0; j < old_count; j++) {
        if (old[j].icon) cairo_surface_destroy(old[j].icon);
    }
}

static void refresh_active_window(void) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, screen->root, net_active_window, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    active_window = XCB_NONE;
    if (reply && xcb_get_property_value_length(reply) >= (int)sizeof(xcb_window_t)) {
        active_window = *(xcb_window_t *)xcb_get_property_value(reply);
    }
    free(reply);
}

/* Reads the pending-updates count from UPDATES_FILE (first line, written by
 * `nekos-software --check`). Returns 1 if the count changed. */
static int refresh_update_count(void) {
    struct stat st;
    long m = (stat(UPDATES_FILE, &st) == 0) ? (long)st.st_mtime : -1;
    if (m == updates_mtime) return 0;
    updates_mtime = m;

    int n = 0;
    FILE *f = fopen(UPDATES_FILE, "r");
    if (f) {
        if (fscanf(f, "%d", &n) != 1) n = 0;
        fclose(f);
    }
    if (n == update_count) return 0;
    update_count = n;
    return 1;
}

/* Recomputes clock_time/clock_date if the wall-clock minute has changed
 * since the last call. Returns 1 if it changed (caller should repaint). */
static int update_clock(void) {
    time_t now = time(NULL);
    long minute = (long)(now / 60);
    if (minute == last_clock_minute) return 0;
    last_clock_minute = minute;

    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(clock_time, sizeof(clock_time), "%I:%M %p", &tmv);
    strftime(clock_date, sizeof(clock_date), "%b %d", &tmv);
    return 1;
}

/* Milliseconds until the top of the next minute -- used as the poll()
 * timeout so the clock redraws right when the displayed minute changes
 * rather than drifting on a fixed interval. */
static int ms_until_next_minute(void) {
    time_t now = time(NULL);
    long rem = 60 - (now % 60);
    if (rem <= 0) rem = 60;
    return (int)(rem * 1000);
}

/* ---- layout -------------------------------------------------------------- */

static int badge_width(void) { return update_count > 0 ? BADGE_WIDTH : 0; }
static int badge_x(void)     { return bar_width - CLOCK_WIDTH - TRAY_WIDTH - badge_width(); }
static int tray_x(void)      { return bar_width - CLOCK_WIDTH - TRAY_WIDTH; }
static int clock_x(void)     { return bar_width - CLOCK_WIDTH; }
static int list_end(void)    { return badge_x(); }

static int slots_shown(void) {
    int list_width = list_end() - MENU_WIDTH;
    int fit = list_width > 0 ? list_width / SLOT_WIDTH : 0;
    return slot_count < fit ? slot_count : fit;
}

/* ---- painting ------------------------------------------------------------ */

static void separator(cairo_t *cr, double x) {
    theme_rgba(cr, THEME_ACCENT, 0.3);
    cairo_rectangle(cr, x, 6, 1, BAR_HEIGHT - 12);
    cairo_fill(cr);
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, bar_width, BAR_HEIGHT);
    cairo_t *cr = cairo_create(surface);

    theme_panel_gradient(cr, 0, 0, bar_width, BAR_HEIGHT, 1);

    /* Menu button: pill with hover feedback. */
    theme_rgba(cr, THEME_ACCENT, hover_zone == HOVER_MENU ? 0.30 : 0.15);
    theme_rounded_rect(cr, 4, 4, MENU_WIDTH - 8, BAR_HEIGHT - 8, THEME_RADIUS);
    cairo_fill(cr);
    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgb(cr, THEME_FG);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "Menu", &ext);
    cairo_move_to(cr, (MENU_WIDTH - ext.x_advance) / 2.0,
                  theme_baseline(0, BAR_HEIGHT, THEME_FONT_SM));
    cairo_show_text(cr, "Menu");

    separator(cr, MENU_WIDTH - 1);

    /* Window list: one pill per window. Active window gets a stronger fill +
     * accent underline; minimized ones are dimmed. Silently truncates past
     * however many slots fit. */
    int shown = slots_shown();
    for (int i = 0; i < shown; i++) {
        double x = MENU_WIDTH + i * SLOT_WIDTH;
        slot_t *s = &slots[i];
        int active = (s->window == active_window);
        int hovered = (hover_zone == HOVER_SLOT && hover_slot == i);

        double fill = active ? 0.22 : (hovered ? 0.14 : 0.07);
        theme_rgba(cr, THEME_ACCENT, fill);
        theme_rounded_rect(cr, x + 3, 4, SLOT_WIDTH - 6, BAR_HEIGHT - 8, THEME_RADIUS);
        cairo_fill(cr);
        if (active) {
            theme_rgba(cr, THEME_ACCENT, 0.9);
            cairo_rectangle(cr, x + 8, BAR_HEIGHT - 6, SLOT_WIDTH - 16, 2);
            cairo_fill(cr);
        }

        /* Icon (native _NET_WM_ICON scaled to 16px), or a small paw-pad dot. */
        double icon_x = x + 10;
        double icon_y = (BAR_HEIGHT - SLOT_ICON_PX) / 2.0;
        if (s->icon) {
            cairo_save(cr);
            cairo_translate(cr, icon_x, icon_y);
            cairo_scale(cr, (double)SLOT_ICON_PX / s->icon_w, (double)SLOT_ICON_PX / s->icon_h);
            cairo_set_source_surface(cr, s->icon, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint_with_alpha(cr, s->minimized ? 0.5 : 1.0);
            cairo_restore(cr);
        } else {
            theme_rgba(cr, THEME_ACCENT, s->minimized ? 0.3 : 0.6);
            cairo_arc(cr, icon_x + SLOT_ICON_PX / 2.0, BAR_HEIGHT / 2.0, 3.5, 0, 6.2831853);
            cairo_fill(cr);
        }

        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, s->minimized ? 0.5 : 1.0);
        theme_text_ellipsized(cr, x + 10 + SLOT_ICON_PX + 8,
                               theme_baseline(0, BAR_HEIGHT, THEME_FONT_SM),
                               SLOT_WIDTH - (10 + SLOT_ICON_PX + 8) - 10,
                               s->label[0] ? s->label : "(untitled)");
    }

    /* Updates badge -- only when updates are pending. Click opens Software. */
    if (update_count > 0) {
        double bx = badge_x();
        separator(cr, bx);
        int hovered = (hover_zone == HOVER_BADGE);
        theme_rgba(cr, THEME_MINIMIZE, hovered ? 0.35 : 0.18);
        theme_rounded_rect(cr, bx + 5, 4, BADGE_WIDTH - 10, BAR_HEIGHT - 8, THEME_RADIUS);
        cairo_fill(cr);

        char txt[16];
        snprintf(txt, sizeof(txt), "%d\xE2\x86\x91", update_count > 99 ? 99 : update_count);
        theme_font(cr, THEME_FONT_SM, 1);
        theme_rgb(cr, THEME_MINIMIZE);
        cairo_text_extents(cr, txt, &ext);
        cairo_move_to(cr, bx + (BADGE_WIDTH - ext.x_advance) / 2.0,
                      theme_baseline(0, BAR_HEIGHT, THEME_FONT_SM));
        cairo_show_text(cr, txt);
    }

    /* Power button (occupies the TRAY_WIDTH slot). Click shuts the session
     * down; hover turns it the danger red so the destructive action is
     * visually announced. Drawn as the standard power glyph: a ring with a
     * gap at the top and a short vertical bar through it. */
    double px_ = tray_x();
    separator(cr, px_);
    double pw_x = px_ + TRAY_WIDTH / 2.0;
    double pw_y = BAR_HEIGHT / 2.0;
    double pw_r = 6.5;
    if (hover_zone == HOVER_POWER) {
        theme_rgba(cr, THEME_CLOSE, 0.20);
        theme_rounded_rect(cr, px_ + 5, 4, TRAY_WIDTH - 10, BAR_HEIGHT - 8, THEME_RADIUS);
        cairo_fill(cr);
        theme_rgb(cr, THEME_CLOSE);
    } else {
        theme_rgba(cr, THEME_FG, 0.8);
    }
    cairo_set_line_width(cr, 1.6);
    cairo_new_sub_path(cr);
    cairo_arc(cr, pw_x, pw_y + 1, pw_r, 1.85, 1.85 + 4.86); /* ~278deg ring, gap at top */
    cairo_stroke(cr);
    cairo_move_to(cr, pw_x, pw_y - pw_r - 1);
    cairo_line_to(cr, pw_x, pw_y + 1);
    cairo_stroke(cr);

    /* Clock, right-aligned: time on top, date beneath. */
    double cx = clock_x();
    separator(cr, cx);
    theme_rgb(cr, THEME_FG);
    theme_font(cr, THEME_FONT_SM, 0);
    cairo_move_to(cr, cx + 12, BAR_HEIGHT / 2.0 - 1.0);
    cairo_show_text(cr, clock_time);
    theme_rgba(cr, THEME_FG, 0.6);
    theme_font(cr, THEME_FONT_XS, 0);
    cairo_move_to(cr, cx + 12, BAR_HEIGHT / 2.0 + 11.0);
    cairo_show_text(cr, clock_date);

    /* Accent rule along the bottom edge. */
    theme_rgb(cr, THEME_ACCENT);
    cairo_rectangle(cr, 0, BAR_HEIGHT - 2, bar_width, 2);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- actions ------------------------------------------------------------- */

/* Forks+execs argv (NULL-terminated), relying on PATH. Used for every
 * application-menu entry, including Shutdown -- it's just another spawned
 * command, not a special-cased code path. */
static void spawn(const char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
}

/* The "Menu" button opens the searchable app launcher (nekos-launch). All
 * relative paths rely on cwd == ~/nekos (start-session.sh launches nekos-bar
 * from there). */
static const char *const argv_launch[]   = { "launch/nekos-launch", NULL };
static const char *const argv_software[] = { "software/nekos-software", NULL };
/* The bar's power button. Shuts down the nekOS session (Xvnc/nekos-wm/
 * nekos-bar/...) only -- not WSL, not the host Windows machine. */
static const char *const argv_shutdown[] = { "sh", "provision/stop-session.sh", NULL };

static void send_active_window(xcb_window_t target) {
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = target;
    ev.type = net_active_window;
    ev.data.data32[0] = 2; /* source indication: pager/taskbar, per EWMH */
    ev.data.data32[1] = XCB_CURRENT_TIME;
    ev.data.data32[2] = XCB_NONE;
    xcb_send_event(conn, 0, screen->root,
                    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                    (const char *)&ev);
    xcb_flush(conn);
}

static void send_close_window(xcb_window_t target) {
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = target;
    ev.type = net_close_window;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = 2; /* source indication: pager/taskbar */
    xcb_send_event(conn, 0, screen->root,
                    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                    (const char *)&ev);
    xcb_flush(conn);
}

/* Generic EWMH _NET_WM_STATE change message: action is always TOGGLE(2) from
 * this bar (nekos-wm treats any request touching these properties as a
 * toggle regardless -- see client_handle_client_message). prop2 may be
 * XCB_ATOM_NONE if only one property is being toggled. */
static void send_wm_state(xcb_window_t target, xcb_atom_t prop1, xcb_atom_t prop2) {
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = target;
    ev.type = net_wm_state;
    ev.data.data32[0] = 2; /* TOGGLE */
    ev.data.data32[1] = prop1;
    ev.data.data32[2] = prop2;
    ev.data.data32[3] = 2; /* source indication: pager/taskbar */
    xcb_send_event(conn, 0, screen->root,
                    XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                    (const char *)&ev);
    xcb_flush(conn);
}

static void action_close(void *userdata) {
    send_close_window((xcb_window_t)(uintptr_t)userdata);
}

static void action_minimize(void *userdata) {
    send_wm_state((xcb_window_t)(uintptr_t)userdata, net_wm_state_hidden, XCB_ATOM_NONE);
}

static void action_maximize(void *userdata) {
    send_wm_state((xcb_window_t)(uintptr_t)userdata, net_wm_state_maximized_vert,
                   net_wm_state_maximized_horz);
}

static void open_window_context_menu(xcb_window_t target, int16_t x, int16_t y) {
    menu_item_t items[3] = {
        { "Close",    action_close,    (void *)(uintptr_t)target },
        { "Minimize", action_minimize, (void *)(uintptr_t)target },
        { "Maximize", action_maximize, (void *)(uintptr_t)target },
    };
    menu_show(conn, screen, visual, x, y, items, 3);
}

/* ---- input --------------------------------------------------------------- */

/* Classifies pointer x into a hover zone (+ slot index for HOVER_SLOT). */
static hover_zone_t zone_at(int16_t x, int *slot_out) {
    *slot_out = -1;
    if (x < MENU_WIDTH) return HOVER_MENU;
    if (x >= clock_x()) return HOVER_NONE; /* clock is inert */
    if (x >= tray_x()) return HOVER_POWER;
    if (update_count > 0 && x >= badge_x()) return HOVER_BADGE;
    if (x < list_end()) {
        int idx = (x - MENU_WIDTH) / SLOT_WIDTH;
        if (idx >= 0 && idx < slots_shown()) {
            *slot_out = idx;
            return HOVER_SLOT;
        }
    }
    return HOVER_NONE;
}

static void handle_motion(int16_t x) {
    int slot;
    hover_zone_t z = zone_at(x, &slot);
    if (z != hover_zone || slot != hover_slot) {
        hover_zone = z;
        hover_slot = slot;
        paint();
    }
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    int slot;
    hover_zone_t z = zone_at(ev->event_x, &slot);

    switch (z) {
    case HOVER_MENU:
        if (ev->detail == 1) spawn(argv_launch);
        break;
    case HOVER_BADGE:
        if (ev->detail == 1) spawn(argv_software);
        break;
    case HOVER_POWER:
        if (ev->detail == 1) spawn(argv_shutdown);
        break;
    case HOVER_SLOT:
        if (ev->detail == 3) {
            open_window_context_menu(slots[slot].window, ev->root_x, ev->root_y);
        } else if (ev->detail == 1) {
            /* Clicking the already-focused window's slot minimizes it
             * (standard taskbar click-to-minimize); otherwise activates it.
             * A minimized window is never the active one, so this can't
             * accidentally re-minimize a window the click is meant to
             * restore. */
            if (slots[slot].window == active_window) {
                send_wm_state(slots[slot].window, net_wm_state_hidden, XCB_ATOM_NONE);
            } else {
                send_active_window(slots[slot].window);
            }
        }
        break;
    default:
        break;
    }
}

/* Publishes _NET_WM_STRUT_PARTIAL (preferred) and the legacy _NET_WM_STRUT so
 * nekos-wm reserves the top BAR_HEIGHT strip. The horizontal extent tracks the
 * current bar_width, so this is re-run whenever the bar re-spans after a
 * screen-size change. */
static void set_struts(void) {
    /* left, right, top, bottom, then the 8 start/end segment fields (unused by
     * nekos-wm's max-reservation-per-edge workarea calc, left zeroed). */
    uint32_t strut_partial[12] = { 0 };
    strut_partial[2] = BAR_HEIGHT;
    strut_partial[8] = 0;
    strut_partial[9] = bar_width;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_strut_partial,
                         XCB_ATOM_CARDINAL, 32, 12, strut_partial);

    uint32_t strut_legacy[4] = { 0, 0, BAR_HEIGHT, 0 };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_strut,
                         XCB_ATOM_CARDINAL, 32, 4, strut_legacy);
}

/* Handles a RandR ScreenChangeNotify: the desktop was resized, so request our
 * own window re-span the new width. nekos-wm applies the (dock-aware) resize
 * and the resulting ConfigureNotify drives the bar_width update, strut
 * refresh, and repaint -- so this only issues the request. */
static void handle_screen_change(xcb_randr_screen_change_notify_event_t *ev) {
    if (ev->width == bar_width) return;
    uint32_t w = ev->width;
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH, &w);
    xcb_flush(conn);
}

int main(void) {
    signal(SIGCHLD, SIG_IGN); /* reap launched terminals/menu commands automatically */

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-bar: could not connect to X server\n");
        return 1;
    }

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    bar_width = screen->width_in_pixels;

    xcb_atom_t net_wm_window_type = intern("_NET_WM_WINDOW_TYPE");
    xcb_atom_t net_wm_window_type_dock = intern("_NET_WM_WINDOW_TYPE_DOCK");
    net_wm_strut_partial = intern("_NET_WM_STRUT_PARTIAL");
    net_wm_strut = intern("_NET_WM_STRUT");
    net_wm_name = intern("_NET_WM_NAME");
    utf8_string = intern("UTF8_STRING");
    net_client_list = intern("_NET_CLIENT_LIST");
    net_active_window = intern("_NET_ACTIVE_WINDOW");
    net_close_window = intern("_NET_CLOSE_WINDOW");
    net_wm_state = intern("_NET_WM_STATE");
    net_wm_state_hidden = intern("_NET_WM_STATE_HIDDEN");
    net_wm_state_maximized_vert = intern("_NET_WM_STATE_MAXIMIZED_VERT");
    net_wm_state_maximized_horz = intern("_NET_WM_STATE_MAXIMIZED_HORZ");
    net_wm_icon = intern("_NET_WM_ICON");

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        /* STRUCTURE_NOTIFY: nekos-wm resizes our window (dock-aware) in
         * response to our re-span request after a screen-size change; the
         * ConfigureNotify it generates is what drives the repaint at the new
         * width. POINTER_MOTION/LEAVE_WINDOW drive hover feedback. */
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, bar_width, BAR_HEIGHT, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_window_type,
                         XCB_ATOM_ATOM, 32, 1, &net_wm_window_type_dock);

    set_struts();

    /* Watch root for _NET_CLIENT_LIST/_NET_ACTIVE_WINDOW changes -- this also
     * delivers every other root property change, so the handler below filters
     * by atom rather than treating every notify as a list change. */
    uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK, &root_mask);

    /* Subscribe to RandR ScreenChangeNotify so the bar re-spans when the VNC
     * viewer resizes the desktop. Event code is dynamic per-connection (like
     * the WM's), so it's compared against randr_event_base in the loop. The
     * query_version handshake is required before events are delivered. */
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

    refresh_slots();
    refresh_active_window();
    refresh_update_count();
    update_clock();
    paint();

    int xfd = xcb_get_file_descriptor(conn);

    for (;;) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, ms_until_next_minute());

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t response_type = event->response_type & ~0x80;

            /* RandR's event code is assigned dynamically per-connection, so it
             * can't be a switch-case constant -- checked separately here. */
            if (have_randr && response_type == randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
                handle_screen_change((xcb_randr_screen_change_notify_event_t *)event);
                free(event);
                continue;
            }

            switch (response_type) {
            case XCB_EXPOSE:
                paint();
                break;
            case XCB_CONFIGURE_NOTIFY: {
                /* nekos-wm applied our re-span request -- adopt the confirmed
                 * width, refresh the struts to match, and repaint. */
                xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)event;
                if (ce->window == win && ce->width != bar_width) {
                    bar_width = ce->width;
                    set_struts();
                    paint();
                }
                break;
            }
            case XCB_BUTTON_PRESS:
                handle_button_press((xcb_button_press_event_t *)event);
                break;
            case XCB_MOTION_NOTIFY:
                handle_motion(((xcb_motion_notify_event_t *)event)->event_x);
                break;
            case XCB_LEAVE_NOTIFY:
                if (hover_zone != HOVER_NONE) {
                    hover_zone = HOVER_NONE;
                    hover_slot = -1;
                    paint();
                }
                break;
            case XCB_PROPERTY_NOTIFY: {
                xcb_property_notify_event_t *pe = (xcb_property_notify_event_t *)event;
                if (pe->window == screen->root) {
                    if (pe->atom == net_client_list) {
                        refresh_slots();
                        paint();
                    } else if (pe->atom == net_active_window) {
                        refresh_active_window();
                        paint();
                    }
                } else if (pe->atom == net_wm_name || pe->atom == net_wm_state) {
                    /* A listed window's title or minimize state changed. */
                    for (int i = 0; i < slot_count; i++) {
                        if (slots[i].window != pe->window) continue;
                        if (pe->atom == net_wm_name) {
                            label_window(pe->window, slots[i].label, sizeof(slots[i].label));
                        } else {
                            slots[i].minimized = window_minimized(pe->window);
                        }
                        paint();
                        break;
                    }
                }
                break;
            }
            default:
                break;
            }
            free(event);
        }

        if (xcb_connection_has_error(conn)) break;

        int dirty = update_clock();
        /* Piggyback the updates-badge check on the minute tick -- the checker
         * (nekos-software --check, looped from start-session.sh) rewrites
         * /tmp/nekos-updates at most every few minutes. */
        dirty |= refresh_update_count();
        if (dirty) paint();
    }

    xcb_disconnect(conn);
    return 0;
}
