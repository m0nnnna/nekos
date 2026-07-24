#include <math.h>

#include <cairo.h>
#include <cairo-xcb.h>

#include "wm.h"
#include "compositor.h"
#include "decor.h"
#include "theme.h"

#define PI 3.14159265358979323846

static const double CORNER_RADIUS = 7.0;

static double button_cx(uint16_t frame_width, decor_button_t button) {
    switch (button) {
    case DECOR_BUTTON_CLOSE:    return frame_width - 16.0;
    case DECOR_BUTTON_MAXIMIZE: return frame_width - 36.0;
    case DECOR_BUTTON_MINIMIZE: return frame_width - 56.0;
    default:                    return 0.0;
    }
}

static double button_cy(void) {
    return TITLEBAR_HEIGHT / 2.0;
}

static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -0.5 * PI, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, 0.5 * PI);
    cairo_arc(cr, x + r, y + h - r, r, 0.5 * PI, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 1.5 * PI);
    cairo_close_path(cr);
}

static void draw_whiskers(cairo_t *cr, double ox, double oy, int focused) {
    if (focused) theme_rgba(cr, THEME_ACCENT, 0.25);
    else theme_rgba(cr, THEME_MUTED, 0.25);
    cairo_set_line_width(cr, 1.0);

    static const double angles_deg[3] = { -12.0, 0.0, 12.0 };
    for (int i = 0; i < 3; i++) {
        double rad = angles_deg[i] * PI / 180.0;
        double len = 14.0;
        cairo_move_to(cr, ox, oy);
        cairo_line_to(cr, ox + len * cos(rad), oy + len * sin(rad));
        cairo_stroke(cr);
    }
}

/* `grow` scales the paw up slightly (hover feedback); 1.0 is the normal size. */
static void draw_paw(cairo_t *cr, double cx, double cy, double grow) {
    cairo_arc(cr, cx, cy + 2.0 * grow, 5.0 * grow, 0.0, 2.0 * PI);
    cairo_fill(cr);

    static const double toe_offsets[4][2] = {
        { -5.0, -4.0 }, { -1.8, -6.0 }, { 1.8, -6.0 }, { 5.0, -4.0 },
    };
    for (int i = 0; i < 4; i++) {
        cairo_arc(cr, cx + toe_offsets[i][0] * grow, cy + toe_offsets[i][1] * grow,
                  1.6 * grow, 0.0, 2.0 * PI);
        cairo_fill(cr);
    }
}

/* One paw button, with hover feedback: a soft halo behind it plus a slightly
 * enlarged paw. Unfocused windows keep their dimmed paws unless hovered. */
static void draw_paw_button(cairo_t *cr, uint16_t frame_width, decor_button_t button,
                             unsigned color, int focused, int hovered) {
    double cx = button_cx(frame_width, button);
    double cy = button_cy();
    if (hovered) {
        theme_rgba(cr, color, 0.25);
        cairo_arc(cr, cx, cy, 12.0, 0.0, 2.0 * PI);
        cairo_fill(cr);
    }
    theme_rgba(cr, color, hovered ? 1.0 : (focused ? 1.0 : 0.4));
    draw_paw(cr, cx, cy, hovered ? 1.15 : 1.0);
}

void decor_paint(xcb_window_t frame, uint16_t frame_width, uint16_t frame_height, int focused,
                 const char *title, decor_button_t hover) {
    cairo_surface_t *surface = cairo_xcb_surface_create(
        conn, frame, compositor_root_visual_type(), frame_width, frame_height);
    cairo_t *cr = cairo_create(surface);

    /* Clip everything we draw to the frame's rounded outline so corners
     * outside it are left untouched (transparent-looking against whatever
     * the compositor's base fill shows through there). */
    rounded_rect_path(cr, 0, 0, frame_width, frame_height, CORNER_RADIUS);
    cairo_clip(cr);

    /* Titlebar gradient. */
    theme_panel_gradient(cr, 0, 0, frame_width, TITLEBAR_HEIGHT, focused);

    /* Cat-ear corner accents pulled for now: drawing them well needs the
     * frame window itself to extend above the titlebar (a window can't draw
     * above its own top edge, y<0 has no backing pixels), which touches
     * frame geometry math beyond this pass. Revisit as a follow-up. */

    /* Whiskers -- purely ornamental, left side of the titlebar. */
    draw_whiskers(cr, 14.0, 14.0, focused);

    /* Window title, between the whiskers and the paw buttons, ellipsized so
     * it never runs under them. */
    if (title && title[0]) {
        double tx = 34.0;
        double max_w = button_cx(frame_width, DECOR_BUTTON_MINIMIZE) - 14.0 - tx;
        if (max_w > 12.0) {
            theme_font(cr, THEME_FONT_SM, 0);
            if (focused) theme_rgba(cr, THEME_FG, 0.95);
            else theme_rgba(cr, THEME_FG, 0.45);
            theme_text_ellipsized(cr, tx, theme_baseline(0, TITLEBAR_HEIGHT, THEME_FONT_SM),
                                   max_w, title);
        }
    }

    /* Paw-print buttons, right-aligned, with hover halo/grow feedback. */
    draw_paw_button(cr, frame_width, DECOR_BUTTON_CLOSE, THEME_CLOSE,
                    focused, hover == DECOR_BUTTON_CLOSE);
    draw_paw_button(cr, frame_width, DECOR_BUTTON_MAXIMIZE, THEME_MAXIMIZE,
                    focused, hover == DECOR_BUTTON_MAXIMIZE);
    draw_paw_button(cr, frame_width, DECOR_BUTTON_MINIMIZE, THEME_MINIMIZE,
                    focused, hover == DECOR_BUTTON_MINIMIZE);

    /* Border stroke, inset by half its width so it sits fully inside the
     * window (a stroke centered exactly on the edge would render half
     * outside the window and get clipped away, undershooting the intended
     * width). */
    cairo_reset_clip(cr);
    double inset = DECOR_BORDER / 2.0;
    rounded_rect_path(cr, inset, inset, frame_width - 2 * inset, frame_height - 2 * inset,
                       CORNER_RADIUS - inset);
    cairo_set_line_width(cr, DECOR_BORDER);
    if (focused) theme_rgba(cr, THEME_ACCENT, 0.6);
    else theme_rgb(cr, THEME_BORDER_DIM);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

decor_button_t decor_hit_test(uint16_t frame_width, int16_t x, int16_t y) {
    if (y < 0 || y > TITLEBAR_HEIGHT) return DECOR_BUTTON_NONE;

    static const decor_button_t buttons[3] = {
        DECOR_BUTTON_CLOSE, DECOR_BUTTON_MAXIMIZE, DECOR_BUTTON_MINIMIZE,
    };
    for (int i = 0; i < 3; i++) {
        double cx = button_cx(frame_width, buttons[i]);
        double cy = button_cy();
        if (hypot(x - cx, y - cy) <= 14.0) return buttons[i]; /* larger than the drawn size -- easier to actually hit */
    }
    return DECOR_BUTTON_NONE;
}

#define FRAME_ICON_RADIUS 7.0
#define FRAME_ICON_MARGIN 12.0
#define FRAME_ICON_GAP    6.0

static double frame_lock_cx(uint16_t window_w) {
    return window_w - FRAME_ICON_MARGIN - FRAME_ICON_RADIUS;
}

static double frame_delete_cx(uint16_t window_w) {
    return frame_lock_cx(window_w) - 2.0 * FRAME_ICON_RADIUS - FRAME_ICON_GAP;
}

static double frame_icon_cy(void) {
    return FRAME_ICON_MARGIN + FRAME_ICON_RADIUS;
}

void frame_paint(xcb_window_t window, cairo_surface_t *image, uint16_t window_w,
                  uint16_t window_h, int locked) {
    cairo_surface_t *surface = cairo_xcb_surface_create(
        conn, window, compositor_root_visual_type(), window_w, window_h);
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_surface(cr, image, 0, 0);
    cairo_paint(cr);

    /* Low alpha when locked -- still hit-testable (see frame_hit_test) so a
     * locked frame can always be re-selected to unlock, but subtle enough
     * that a locked frame mostly just reads as a plain picture. */
    double alpha = locked ? 0.15 : 0.8;

    theme_rgba(cr, THEME_ACCENT, alpha);
    cairo_arc(cr, frame_lock_cx(window_w), frame_icon_cy(), FRAME_ICON_RADIUS, 0.0, 2.0 * PI);
    cairo_fill(cr);

    theme_rgba(cr, THEME_CLOSE, alpha);
    cairo_arc(cr, frame_delete_cx(window_w), frame_icon_cy(), FRAME_ICON_RADIUS, 0.0, 2.0 * PI);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

frame_hit_t frame_hit_test(uint16_t window_w, uint16_t window_h, int16_t x, int16_t y) {
    (void)window_h;
    if (hypot(x - frame_lock_cx(window_w), y - frame_icon_cy()) <= FRAME_ICON_RADIUS + 4.0) {
        return FRAME_HIT_LOCK;
    }
    if (hypot(x - frame_delete_cx(window_w), y - frame_icon_cy()) <= FRAME_ICON_RADIUS + 4.0) {
        return FRAME_HIT_DELETE;
    }
    return FRAME_HIT_NONE;
}
