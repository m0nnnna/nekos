/*
 * nekos-pet: the resident cat.
 *
 * A small override-redirect ARGB window that wanders the desktop: it chases
 * the cursor when it runs off, sits and watches it nearby, dozes off when
 * nothing happens for a while, and shows a heart when clicked (petted).
 *
 * Everything is drawn with cairo from theme.h colors -- no sprite assets.
 * The window is a 32-bit ARGB visual so the cat has a real silhouette over
 * the wallpaper; nekos-wm's compositor picks the matching visual per window
 * (see window_visual_type in compositor.c) and blends the alpha. If the X
 * server offers no 32-bit visual we fall back to the root visual (opaque
 * box) rather than not existing at all.
 *
 * Movement: the pet configures its own x/y (plus STACK_MODE_ABOVE, so it
 * runs over -- not under -- whatever you're doing). The WM mirrors those
 * self-moves into the compositor via its ConfigureNotify handler; without
 * that the cat would smear. Like every override-redirect nekOS window it
 * repaints once ~45ms after mapping (compositor damage-tracking race).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <xcb/xcb.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "theme.h"

#define PET_W        72
#define PET_H        78
#define GROUND_Y     72.0          /* feet line inside the window */
#define BAR_H        32            /* keep out from under the top bar */

#define TICK_ACTIVE_MS   50        /* chasing / petting / settling */
#define TICK_CALM_MS     150       /* sitting / sleeping */
#define SETTLE_MS        45

#define CHASE_START_DIST 240.0     /* cursor further than this -> give chase */
#define CHASE_STOP_DIST  80.0      /* close enough -> sit and watch */
#define CHASE_SPEED      5.5       /* px per active tick */
#define SLEEP_AFTER_MS   25000     /* cursor quiet this long -> doze off */
#define PET_MS           1400      /* heart duration after a click */

typedef enum { STATE_IDLE, STATE_CHASE, STATE_SLEEP, STATE_PET } pet_state_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;     /* ARGB if available, else root */
static int have_argb;
static xcb_window_t win;
/* Off-screen backbuffer: paint() draws here, then blits to `win` in one
 * xcb_copy_area. The cat's drawing is many separate cairo/XRender calls
 * (clear, tail, legs, body, face, heart/zzz); painting them straight onto
 * `win` -- what nekos-wm's compositor names as its pixmap -- let the
 * compositor's copy land mid-draw (e.g. just after the transparent clear,
 * before the cat itself), a real flicker while chasing the cursor every
 * TICK_ACTIVE_MS. Mirrors nekos-wm's own backbuffer_pixmap -> overlay copy
 * in compositor.c. */
static xcb_pixmap_t backbuffer;
static xcb_gcontext_t backbuffer_gc;

static double pos_x, pos_y;          /* window top-left, as doubles for smooth motion */
static uint16_t root_w, root_h;
static pet_state_t state = STATE_IDLE;
static int facing = 1;               /* +1 right, -1 left (art faces right) */
static double run_phase = 0.0;       /* leg cycle while chasing */
static int64_t state_since;          /* when the current state was entered */
static int64_t last_cursor_move_ms;
static int16_t cursor_x, cursor_y;
static int settled;

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static xcb_visualtype_t *find_visual_by_id(xcb_screen_t *scr, xcb_visualid_t id) {
    xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(scr);
    for (; depth_it.rem; xcb_depth_next(&depth_it)) {
        xcb_visualtype_iterator_t visual_it = xcb_depth_visuals_iterator(depth_it.data);
        for (; visual_it.rem; xcb_visualtype_next(&visual_it)) {
            if (visual_it.data->visual_id == id) return visual_it.data;
        }
    }
    return NULL;
}

/* First 32-bit TrueColor visual, or NULL if the server has none. */
static xcb_visualtype_t *find_argb_visual(xcb_screen_t *scr) {
    xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(scr);
    for (; depth_it.rem; xcb_depth_next(&depth_it)) {
        if (depth_it.data->depth != 32) continue;
        xcb_visualtype_iterator_t visual_it = xcb_depth_visuals_iterator(depth_it.data);
        for (; visual_it.rem; xcb_visualtype_next(&visual_it)) {
            if (visual_it.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
                return visual_it.data;
        }
    }
    return NULL;
}

/* ---- drawing ------------------------------------------------------------- */

/* One closed cat silhouette (body loaf + head + ears + tail), so the outline
 * strokes once around the whole cat instead of showing seams between parts.
 * Drawn facing right around a feet line at GROUND_Y; `crouch` (0..1) squashes
 * toward the sleeping loaf pose. */
static void cat_path(cairo_t *cr, double crouch) {
    double body_cx = 32.0, body_rx = 19.0;
    double body_ry = 13.0 - 3.0 * crouch;
    double body_cy = GROUND_Y - body_ry;
    double head_r = 12.5;
    double head_cx = body_cx + 16.0 - 4.0 * crouch;
    double head_cy = body_cy - body_ry - head_r * (0.55 - 0.35 * crouch);

    cairo_new_path(cr);
    cairo_arc(cr, body_cx, body_cy, 1.0, 0, 2 * M_PI); /* seed point, gets unioned over */
    cairo_new_sub_path(cr);
    cairo_save(cr);
    cairo_translate(cr, body_cx, body_cy);
    cairo_scale(cr, body_rx, body_ry);
    cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, head_cx, head_cy, head_r, 0, 2 * M_PI);
    /* Ears: two triangles on the head. */
    double ear_y = head_cy - head_r + 2.0;
    cairo_new_sub_path(cr);
    cairo_move_to(cr, head_cx - 10.0, ear_y);
    cairo_line_to(cr, head_cx - 11.5, ear_y - 9.0);
    cairo_line_to(cr, head_cx - 2.5, ear_y - 3.0);
    cairo_close_path(cr);
    cairo_new_sub_path(cr);
    cairo_move_to(cr, head_cx + 2.0, ear_y - 3.0);
    cairo_line_to(cr, head_cx + 8.5, ear_y - 10.0);
    cairo_line_to(cr, head_cx + 11.0, ear_y + 1.0);
    cairo_close_path(cr);
}

/* Tail: a stroked curve from the body's rear. swish in [-1,1]. */
static void draw_tail(cairo_t *cr, double swish, double crouch) {
    double base_x = 14.0, base_y = GROUND_Y - 10.0 + 4.0 * crouch;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, base_x, base_y);
    cairo_curve_to(cr, base_x - 8.0, base_y - 4.0,
                   base_x - 9.0 + 3.0 * swish, base_y - 16.0 + 2.0 * crouch * 3.0,
                   base_x - 4.0 + 6.0 * swish, base_y - 20.0 + 8.0 * crouch);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    theme_rgb(cr,THEME_PANEL_TOP);
    cairo_set_line_width(cr, 7.0);
    cairo_stroke_preserve(cr);
    theme_rgb(cr,THEME_ACCENT);
    cairo_set_line_width(cr, 4.2);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* Legs while running: little stubs alternating with the run cycle. */
static void draw_run_legs(cairo_t *cr, double phase) {
    double off = sin(phase) * 3.5;
    static const double leg_x[4] = { 20.0, 28.0, 40.0, 48.0 };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 4; i++) {
        double o = (i % 2 == 0) ? off : -off;
        cairo_new_path(cr);
        cairo_move_to(cr, leg_x[i], GROUND_Y - 8.0);
        cairo_line_to(cr, leg_x[i] + o, GROUND_Y - 1.0);
        theme_rgb(cr,THEME_PANEL_TOP);
        cairo_set_line_width(cr, 6.5);
        cairo_stroke_preserve(cr);
        theme_rgb(cr,THEME_ACCENT);
        cairo_set_line_width(cr, 3.8);
        cairo_stroke(cr);
    }
}

static void draw_face(cairo_t *cr, pet_state_t st, int blink, double crouch) {
    double head_cx = 32.0 + 16.0 - 4.0 * crouch;
    double head_r = 12.5;
    double body_ry = 13.0 - 3.0 * crouch;
    double body_cy = GROUND_Y - body_ry;
    double head_cy = body_cy - body_ry - head_r * (0.55 - 0.35 * crouch);

    double eye_y = head_cy - 1.0;
    double lx = head_cx - 5.5, rx = head_cx + 5.5;

    theme_rgb(cr,THEME_BG);
    if (st == STATE_SLEEP || blink) {
        /* Closed eyes: gentle down-arcs. */
        cairo_set_line_width(cr, 1.6);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_new_path(cr);
        cairo_arc(cr, lx, eye_y - 1.0, 2.6, 0.3, M_PI - 0.3);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, rx, eye_y - 1.0, 2.6, 0.3, M_PI - 0.3);
        cairo_stroke(cr);
    } else if (st == STATE_PET) {
        /* Happy ^ ^ eyes. */
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_new_path(cr);
        cairo_arc(cr, lx, eye_y + 1.2, 2.8, M_PI + 0.4, 2 * M_PI - 0.4);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, rx, eye_y + 1.2, 2.8, M_PI + 0.4, 2 * M_PI - 0.4);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
        cairo_arc(cr, lx, eye_y, 2.2, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_arc(cr, rx, eye_y, 2.2, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    /* Tiny nose + ω mouth. */
    double nose_y = eye_y + 4.5;
    cairo_new_path(cr);
    cairo_move_to(cr, head_cx - 1.6, nose_y);
    cairo_line_to(cr, head_cx + 1.6, nose_y);
    cairo_line_to(cr, head_cx, nose_y + 2.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    if (st != STATE_SLEEP) {
        cairo_set_line_width(cr, 1.2);
        cairo_new_path(cr);
        cairo_arc(cr, head_cx - 2.2, nose_y + 3.0, 2.0, 0.15, M_PI * 0.85);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, head_cx + 2.2, nose_y + 3.0, 2.0, 0.15, M_PI * 0.85);
        cairo_stroke(cr);
    }

    /* Inner ears. */
    double ear_y = head_cy - head_r + 2.0;
    theme_rgba(cr, THEME_FG, 0.85);
    cairo_new_path(cr);
    cairo_move_to(cr, head_cx - 9.3, ear_y - 1.0);
    cairo_line_to(cr, head_cx - 10.0, ear_y - 6.0);
    cairo_line_to(cr, head_cx - 5.0, ear_y - 2.6);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, head_cx + 3.6, ear_y - 3.4);
    cairo_line_to(cr, head_cx + 7.4, ear_y - 7.4);
    cairo_line_to(cr, head_cx + 8.8, ear_y - 1.4);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void draw_heart(cairo_t *cr, double t) { /* t in [0,1] over PET_MS */
    double rise = 14.0 * t;
    double alpha = t < 0.7 ? 1.0 : (1.0 - t) / 0.3;
    double cx = 48.0, cy = 18.0 - rise, s = 1.0 + 0.15 * sin(t * M_PI * 3.0);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s, s);
    cairo_new_path(cr);
    cairo_move_to(cr, 0, 3.5);
    cairo_curve_to(cr, -6.0, -2.0, -3.0, -7.0, 0, -3.5);
    cairo_curve_to(cr, 3.0, -7.0, 6.0, -2.0, 0, 3.5);
    cairo_close_path(cr);
    theme_rgba(cr, THEME_CLOSE, alpha);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void draw_zzz(cairo_t *cr, int64_t now) {
    double t = (double)((now / 40) % 90) / 90.0; /* slow loop */
    theme_font(cr, THEME_FONT_SM, 1);
    theme_rgba(cr, THEME_FG, 0.9 * (1.0 - t));
    cairo_move_to(cr, 52.0 + 4.0 * t, 22.0 - 12.0 * t);
    cairo_show_text(cr, "z");
    theme_rgba(cr, THEME_FG, 0.7 * (1.0 - t));
    theme_font(cr, THEME_FONT_XS, 1);
    cairo_move_to(cr, 58.0 + 5.0 * t, 14.0 - 10.0 * t);
    cairo_show_text(cr, "z");
}

static void paint(void) {
    int64_t now = now_ms();
    cairo_surface_t *s = cairo_xcb_surface_create(conn, backbuffer, visual, PET_W, PET_H);
    cairo_t *cr = cairo_create(s);

    /* Clear. With ARGB this leaves true transparency; the opaque fallback
     * gets the flat app background instead of garbage. */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (have_argb) cairo_set_source_rgba(cr, 0, 0, 0, 0);
    else theme_rgb(cr, THEME_BG);
    cairo_paint(cr);
    cairo_restore(cr);

    double crouch = (state == STATE_SLEEP) ? 1.0 : 0.0;
    int blink = (state == STATE_IDLE) && ((now / 200) % 22 == 0);

    /* Ground shadow. */
    cairo_save(cr);
    cairo_translate(cr, 34.0, GROUND_Y + 2.5);
    cairo_scale(cr, 24.0, 4.0);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.20);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_save(cr);
    if (facing < 0) { /* art faces right; mirror for leftward */
        cairo_translate(cr, PET_W, 0);
        cairo_scale(cr, -1.0, 1.0);
    }

    draw_tail(cr, (state == STATE_CHASE) ? sin(run_phase * 0.5)
                                          : sin((double)now / 700.0), crouch);
    if (state == STATE_CHASE) draw_run_legs(cr, run_phase);

    cat_path(cr, crouch);
    theme_rgb(cr,THEME_ACCENT);
    cairo_fill_preserve(cr);
    theme_rgb(cr,THEME_PANEL_TOP);
    cairo_set_line_width(cr, 2.2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);

    /* Chest patch. */
    cairo_save(cr);
    cairo_translate(cr, 40.0, GROUND_Y - 8.0 - 2.0 * crouch);
    cairo_scale(cr, 7.0, 5.0);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
    theme_rgba(cr, THEME_FG, 0.85);
    cairo_fill(cr);
    cairo_restore(cr);

    draw_face(cr, state, blink, crouch);
    cairo_restore(cr); /* un-mirror: heart/zzz read the same either way */

    if (state == STATE_PET) {
        double t = (double)(now - state_since) / PET_MS;
        if (t > 1.0) t = 1.0;
        draw_heart(cr, t);
    }
    if (state == STATE_SLEEP) draw_zzz(cr, now);

    cairo_destroy(cr);
    cairo_surface_destroy(s);
    xcb_copy_area(conn, backbuffer, win, backbuffer_gc, 0, 0, 0, 0, PET_W, PET_H);
    xcb_flush(conn);
}

/* ---- behavior ------------------------------------------------------------ */

static void set_state(pet_state_t st) {
    if (state == st) return;
    state = st;
    state_since = now_ms();
}

static void refresh_root_geometry(void) {
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(
        conn, xcb_get_geometry(conn, screen->root), NULL);
    if (g) {
        root_w = g->width;
        root_h = g->height;
        free(g);
    }
}

static void clamp_pos(void) {
    if (pos_x < 0) pos_x = 0;
    if (pos_y < BAR_H) pos_y = BAR_H;
    if (root_w > PET_W && pos_x > root_w - PET_W) pos_x = root_w - PET_W;
    if (root_h > PET_H && pos_y > root_h - PET_H) pos_y = root_h - PET_H;
}

static void move_window(int raise) {
    clamp_pos();
    if (raise) {
        uint32_t vals[3] = { (uint32_t)(int16_t)pos_x, (uint32_t)(int16_t)pos_y,
                             XCB_STACK_MODE_ABOVE };
        xcb_configure_window(conn, win,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_STACK_MODE, vals);
    } else {
        uint32_t vals[2] = { (uint32_t)(int16_t)pos_x, (uint32_t)(int16_t)pos_y };
        xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
    }
}

/* One behavior tick. Returns the ms until the next one. */
static int tick(void) {
    int64_t now = now_ms();

    /* Where's the cursor? */
    xcb_query_pointer_reply_t *qp = xcb_query_pointer_reply(
        conn, xcb_query_pointer(conn, screen->root), NULL);
    if (qp) {
        if (abs(qp->root_x - cursor_x) > 2 || abs(qp->root_y - cursor_y) > 2)
            last_cursor_move_ms = now;
        cursor_x = qp->root_x;
        cursor_y = qp->root_y;
        free(qp);
    }

    double cat_cx = pos_x + PET_W / 2.0;
    double cat_cy = pos_y + GROUND_Y - 14.0; /* roughly the body center */
    double dx = cursor_x - cat_cx, dy = cursor_y - cat_cy;
    double dist = sqrt(dx * dx + dy * dy);

    switch (state) {
    case STATE_SLEEP:
        /* Any cursor movement in the last few ticks wakes the cat (after a
         * minimum nap so entering sleep can't immediately bounce back out). */
        if ((now - state_since) > 1000 && (now - last_cursor_move_ms) < 300)
            set_state(STATE_IDLE);
        paint(); /* keep the zzz drifting */
        return TICK_CALM_MS;

    case STATE_PET:
        if (now - state_since >= PET_MS) set_state(STATE_IDLE);
        paint();
        return TICK_ACTIVE_MS;

    case STATE_CHASE:
        if (dist <= CHASE_STOP_DIST) {
            set_state(STATE_IDLE);
            paint();
            return TICK_CALM_MS;
        }
        facing = (dx >= 0) ? 1 : -1;
        pos_x += (dx / dist) * CHASE_SPEED;
        pos_y += (dy / dist) * CHASE_SPEED;
        run_phase += 0.9;
        move_window(/*raise=*/1);
        paint();
        return TICK_ACTIVE_MS;

    case STATE_IDLE:
    default:
        if (dist > CHASE_START_DIST) {
            set_state(STATE_CHASE);
            return TICK_ACTIVE_MS;
        }
        if (now - last_cursor_move_ms > SLEEP_AFTER_MS) {
            set_state(STATE_SLEEP);
            paint();
            return TICK_CALM_MS;
        }
        /* Watch the cursor; tail swish + occasional blink keep it alive. */
        facing = (dx >= 0) ? 1 : -1;
        paint();
        return TICK_CALM_MS;
    }
}

int main(void) {
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-pet: could not connect to X server\n");
        return 1;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    refresh_root_geometry();
    if (!root_w) { root_w = screen->width_in_pixels; root_h = screen->height_in_pixels; }

    visual = find_argb_visual(screen);
    have_argb = (visual != NULL);
    uint8_t depth = 32;
    if (!have_argb) {
        fprintf(stderr, "nekos-pet: no 32-bit visual, falling back to opaque\n");
        visual = find_visual_by_id(screen, screen->root_visual);
        depth = screen->root_depth;
    }

    pos_x = root_w - PET_W - 120.0;
    pos_y = root_h - PET_H - 60.0;
    clamp_pos();

    win = xcb_generate_id(conn);
    uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS;
    if (have_argb) {
        /* 32-bit windows need an explicit border pixel + colormap or the
         * create fails with a Match error (defaults come from the parent,
         * which is 24-bit). */
        xcb_colormap_t cmap = xcb_generate_id(conn);
        xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, cmap, screen->root,
                            visual->visual_id);
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
        uint32_t values[5] = { 0, 0, 1, event_mask, cmap };
        xcb_create_window(conn, depth, win, screen->root,
                          (int16_t)pos_x, (int16_t)pos_y, PET_W, PET_H, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, visual->visual_id, mask, values);
    } else {
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
        uint32_t values[3] = { screen->black_pixel, 1, event_mask };
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                          (int16_t)pos_x, (int16_t)pos_y, PET_W, PET_H, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    }

    const char *cls = "nekos-pet\0nekos-pet";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, 20, cls);

    backbuffer = xcb_generate_id(conn);
    xcb_create_pixmap(conn, depth, backbuffer, win, PET_W, PET_H);
    backbuffer_gc = xcb_generate_id(conn);
    xcb_create_gc(conn, backbuffer_gc, win, 0, NULL);

    xcb_map_window(conn, win);
    xcb_flush(conn);

    int64_t start = now_ms();
    state_since = start;
    last_cursor_move_ms = start;

    int xfd = xcb_get_file_descriptor(conn);
    int timeout = TICK_ACTIVE_MS;
    int64_t geom_checked = start;

    for (;;) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, timeout);

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t rt = event->response_type & ~0x80;
            if (rt == XCB_EXPOSE) {
                paint();
            } else if (rt == XCB_BUTTON_PRESS) {
                set_state(STATE_PET); /* petting also wakes a sleeping cat */
                paint();
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;

        int64_t now = now_ms();
        if (!settled && now - start >= SETTLE_MS) {
            /* Post-map settle repaint (compositor damage-tracking race --
             * same workaround as menus/launcher/toasts). */
            paint();
            settled = 1;
        }
        if (now - geom_checked > 5000) {
            /* Track root resizes (the desktop follows the VNC viewer). */
            refresh_root_geometry();
            geom_checked = now;
            clamp_pos();
        }

        timeout = tick();
    }

    xcb_disconnect(conn);
    return 0;
}
