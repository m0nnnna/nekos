#ifndef NEKOS_DECOR_H
#define NEKOS_DECOR_H

#include <stdint.h>
#include <cairo.h>
#include <xcb/xcb.h>

/* 5px (not just a visually-thin 2px) so the border is actually big enough to
 * reliably grab for a resize drag -- see client.c's edge hit-testing. */
#define DECOR_BORDER     5
#define TITLEBAR_HEIGHT  28

typedef enum {
    DECOR_BUTTON_NONE,
    DECOR_BUTTON_CLOSE,
    DECOR_BUTTON_MAXIMIZE,
    DECOR_BUTTON_MINIMIZE,
} decor_button_t;

typedef enum {
    FRAME_HIT_NONE,
    FRAME_HIT_LOCK,
    FRAME_HIT_DELETE,
} frame_hit_t;

/* Paints the frame's chrome (titlebar gradient, rounded border, whiskers,
 * window title, paw buttons) directly onto the frame window at its current
 * size. Draws over the whole frame, not just the titlebar strip -- the client
 * sub-window sits on top of the content area regardless. `title` may be NULL/
 * empty (no text drawn); `hover` highlights that paw button (or
 * DECOR_BUTTON_NONE). */
void decor_paint(xcb_window_t frame, uint16_t frame_width, uint16_t frame_height, int focused,
                 const char *title, decor_button_t hover);

/* Hit-tests a click at (x,y) in the frame's own coordinate space against the
 * paw buttons. DECOR_BUTTON_NONE for anywhere else in the titlebar (a plain
 * focus-raise click) or outside the titlebar entirely (caller should ignore). */
decor_button_t decor_hit_test(uint16_t frame_width, int16_t x, int16_t y);

/* Paints a decorative "frame" window: `image` full-bleed at (0,0), then two
 * small circular corner buttons (lock-toggle, delete) at low alpha when
 * `locked` (still hit-testable, just visually subtle so a locked frame reads
 * as a clean picture) or fuller alpha when not. `image` must already be
 * exactly window_w x window_h (see image_load_scaled's fit mode). */
void frame_paint(xcb_window_t window, cairo_surface_t *image, uint16_t window_w,
                  uint16_t window_h, int locked);

/* Hit-tests a click at (x,y) in the frame's own coordinate space against its
 * two corner buttons. FRAME_HIT_NONE for anywhere else (a plain drag-move,
 * if unlocked). */
frame_hit_t frame_hit_test(uint16_t window_w, uint16_t window_h, int16_t x, int16_t y);

#endif
