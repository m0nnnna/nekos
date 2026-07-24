/*
 * nekos-notify: a lightweight notification daemon + CLI (nekOS's notify-send).
 *
 * Two modes, one binary:
 *   nekos-notify --daemon        run the daemon (renders toasts)
 *   nekos-notify "Title" "Body"  send a notification to the running daemon
 *
 * Deliberately NOT D-Bus: apps talk to the daemon over a Unix socket
 * (/tmp/nekos-notify.sock). The daemon shows each notification as a toast in
 * the top-right corner -- an override-redirect window the compositor paints --
 * that auto-dismisses after a few seconds or on click. Toasts stack downward.
 *
 * A D-Bus bridge (so third-party apps' org.freedesktop.Notifications reach
 * this) is a documented future add; this covers nekOS-native notifications.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <xcb/xcb.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "theme.h"

#define SOCK_PATH   "/tmp/nekos-notify.sock"
#define BAR_H       32
#define MARGIN      16
#define GAP         10
#define TOAST_W     340
#define TOAST_H     76
#define MAX_TOASTS  6
#define TOAST_MS    5000
#define SETTLE_MS   45
#define TITLE_LEN   128
#define BODY_LEN    512
#define MSG_LEN     (TITLE_LEN + BODY_LEN + 8)

typedef struct {
    xcb_window_t win;
    char title[TITLE_LEN];
    char body[BODY_LEN];
    int64_t expire_at;   /* monotonic ms */
    int settled;         /* has it been repainted post-map (compositor race)? */
    int hovered;         /* pointer inside -> show the dismiss "x" */
} toast_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;

static toast_t toasts[MAX_TOASTS];
static int toast_count = 0;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* ---- CLI (send) mode ---------------------------------------------------- */

static int send_notification(const char *title, const char *body) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "nekos-notify: daemon not running (%s)\n", SOCK_PATH);
        close(fd);
        return 1;
    }

    /* Wire format: title, then a newline, then the (possibly multi-line) body. */
    char msg[MSG_LEN];
    int n = snprintf(msg, sizeof(msg), "%s\n%s", title, body ? body : "");
    if (n > 0) { ssize_t wr = write(fd, msg, (size_t)n); (void)wr; }
    close(fd);
    return 0;
}

/* ---- daemon: toast rendering -------------------------------------------- */

/* Draws `text` wrapped onto up to two lines of `max_w` (baselines y1/y2),
 * preferring to break at a space; the second line is ellipsized if the rest
 * still doesn't fit. */
static void draw_body_wrapped(cairo_t *cr, double x, double y1, double y2,
                               double max_w, const char *text) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    if (ext.x_advance <= max_w) {
        cairo_move_to(cr, x, y1);
        cairo_show_text(cr, text);
        return;
    }

    /* Longest prefix that fits, backed off to a word boundary when there is
     * one. Linear scan is fine at toast lengths. */
    char line[BODY_LEN];
    size_t len = strlen(text);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    size_t fit = len;
    while (fit > 0) {
        memcpy(line, text, fit);
        line[fit] = '\0';
        cairo_text_extents(cr, line, &ext);
        if (ext.x_advance <= max_w) break;
        fit--;
    }
    size_t brk = fit;
    while (brk > 0 && text[brk] != ' ' && text[brk - 1] != ' ') brk--;
    if (brk > 0) fit = brk;
    memcpy(line, text, fit);
    line[fit] = '\0';
    cairo_move_to(cr, x, y1);
    cairo_show_text(cr, line);

    const char *rest = text + fit;
    while (*rest == ' ') rest++;
    if (*rest) theme_text_ellipsized(cr, x, y2, max_w, rest);
}

static void paint_toast(toast_t *t) {
    cairo_surface_t *s = cairo_xcb_surface_create(conn, t->win, visual, TOAST_W, TOAST_H);
    cairo_t *cr = cairo_create(s);

    theme_panel_gradient(cr, 0, 0, TOAST_W, TOAST_H, 1);

    /* Pink accent stripe on the left. */
    theme_rgb(cr, THEME_ACCENT);
    cairo_rectangle(cr, 0, 0, 4, TOAST_H);
    cairo_fill(cr);

    double text_w = TOAST_W - 16 - 24; /* room for the dismiss "x" column */

    theme_rgb(cr, THEME_FG);
    theme_font(cr, THEME_FONT_LG, 1);
    theme_text_ellipsized(cr, 16, 26, text_w, t->title);

    if (t->body[0]) {
        theme_rgba(cr, THEME_FG, 0.8);
        theme_font(cr, THEME_FONT_SM, 0);
        draw_body_wrapped(cr, 16, 46, 63, text_w, t->body);
    }

    /* Dismiss "x", top-right, only while hovered (any click dismisses; this
     * is the affordance that says so). */
    if (t->hovered) {
        theme_rgba(cr, THEME_FG, 0.7);
        cairo_set_line_width(cr, 1.6);
        double cx = TOAST_W - 16, cy = 16, r = 4.0;
        cairo_move_to(cr, cx - r, cy - r);
        cairo_line_to(cr, cx + r, cy + r);
        cairo_move_to(cr, cx + r, cy - r);
        cairo_line_to(cr, cx - r, cy + r);
        cairo_stroke(cr);
    }

    theme_rgba(cr, THEME_ACCENT, THEME_BORDER_ALPHA);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, TOAST_W - 1, TOAST_H - 1);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

static int16_t toast_y(int index) {
    return (int16_t)(BAR_H + MARGIN + index * (TOAST_H + GAP));
}

/* Repositions all toasts to match their array order (after one is removed). */
static void reflow_toasts(void) {
    int16_t x = (int16_t)(screen->width_in_pixels - TOAST_W - MARGIN);
    for (int i = 0; i < toast_count; i++) {
        uint32_t vals[2] = { (uint32_t)x, (uint32_t)toast_y(i) };
        xcb_configure_window(conn, toasts[i].win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
    }
    xcb_flush(conn);
}

static void remove_toast(int idx) {
    xcb_destroy_window(conn, toasts[idx].win);
    for (int i = idx; i < toast_count - 1; i++) toasts[i] = toasts[i + 1];
    toast_count--;
    reflow_toasts();
    xcb_flush(conn);
}

static void add_toast(const char *title, const char *body) {
    if (toast_count >= MAX_TOASTS) remove_toast(0); /* drop the oldest to make room */

    toast_t *t = &toasts[toast_count];
    memset(t, 0, sizeof(*t));
    strncpy(t->title, title, TITLE_LEN - 1);
    strncpy(t->body, body, BODY_LEN - 1);
    t->expire_at = now_ms() + TOAST_MS;
    t->settled = 0;

    int16_t x = (int16_t)(screen->width_in_pixels - TOAST_W - MARGIN);
    int16_t y = toast_y(toast_count);

    t->win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[3] = { screen->black_pixel, 1,
                           XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
                               XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, t->win, screen->root,
                       x, y, TOAST_W, TOAST_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    xcb_map_window(conn, t->win);
    toast_count++;
    xcb_flush(conn);
    paint_toast(t);
    xcb_flush(conn);
}

static toast_t *find_toast(xcb_window_t win) {
    for (int i = 0; i < toast_count; i++) if (toasts[i].win == win) return &toasts[i];
    return NULL;
}

/* ms until the next thing that needs attention (a settle repaint or an
 * expiry), or -1 if nothing is pending. */
static int next_timeout(void) {
    int64_t now = now_ms();
    int64_t soonest = -1;
    for (int i = 0; i < toast_count; i++) {
        int64_t when = toasts[i].settled ? toasts[i].expire_at
                                         : (toasts[i].expire_at - TOAST_MS + SETTLE_MS);
        if (soonest < 0 || when < soonest) soonest = when;
    }
    if (soonest < 0) return -1;
    int64_t d = soonest - now;
    return d < 0 ? 0 : (int)d;
}

static void tick(void) {
    int64_t now = now_ms();
    /* Settle repaints (compositor damage-tracking races with the first paint,
     * same issue menus/launcher handle). */
    for (int i = 0; i < toast_count; i++) {
        if (!toasts[i].settled && now >= toasts[i].expire_at - TOAST_MS + SETTLE_MS) {
            paint_toast(&toasts[i]);
            toasts[i].settled = 1;
        }
    }
    /* Expiries (iterate backwards -- remove_toast shifts the array). */
    for (int i = toast_count - 1; i >= 0; i--) {
        if (now >= toasts[i].expire_at) remove_toast(i);
    }
    xcb_flush(conn);
}

static int run_daemon(void) {
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    unlink(SOCK_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
    if (listen(sock, 8) != 0) { perror("listen"); return 1; }

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-notify: could not connect to X server\n");
        return 1;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);

    int xfd = xcb_get_file_descriptor(conn);

    for (;;) {
        struct pollfd pfds[2] = {
            { .fd = xfd, .events = POLLIN, .revents = 0 },
            { .fd = sock, .events = POLLIN, .revents = 0 },
        };
        poll(pfds, 2, next_timeout());

        /* New notification(s) over the socket. */
        if (pfds[1].revents & POLLIN) {
            int c = accept(sock, NULL, NULL);
            if (c >= 0) {
                char buf[MSG_LEN];
                ssize_t n = read(c, buf, sizeof(buf) - 1);
                close(c);
                if (n > 0) {
                    buf[n] = '\0';
                    char *nl = strchr(buf, '\n');
                    if (nl) { *nl = '\0'; add_toast(buf, nl + 1); }
                    else add_toast(buf, "");
                }
            }
        }

        /* X events: click a toast to dismiss it. */
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t rt = event->response_type & ~0x80;
            if (rt == XCB_EXPOSE) {
                xcb_expose_event_t *ee = (xcb_expose_event_t *)event;
                toast_t *t = find_toast(ee->window);
                if (t) paint_toast(t);
            } else if (rt == XCB_BUTTON_PRESS) {
                xcb_button_press_event_t *be = (xcb_button_press_event_t *)event;
                toast_t *t = find_toast(be->event);
                if (t) remove_toast((int)(t - toasts));
            } else if (rt == XCB_ENTER_NOTIFY || rt == XCB_LEAVE_NOTIFY) {
                xcb_enter_notify_event_t *ce = (xcb_enter_notify_event_t *)event;
                toast_t *t = find_toast(ce->event);
                if (t) {
                    int hovered = (rt == XCB_ENTER_NOTIFY);
                    if (hovered != t->hovered) {
                        t->hovered = hovered;
                        paint_toast(t);
                        xcb_flush(conn);
                    }
                }
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;

        tick();
    }

    close(sock);
    unlink(SOCK_PATH);
    xcb_disconnect(conn);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--daemon") != 0) {
        /* Send mode: argv[1] = title, argv[2] = body (optional). */
        return send_notification(argv[1], argc >= 3 ? argv[2] : "");
    }
    return run_daemon();
}
