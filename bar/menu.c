#include <stdlib.h>
#include <poll.h>

#include <xcb/xcb.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "menu.h"
#include "theme.h"

#define MENU_WIDTH   140
#define ROW_HEIGHT   28

/* Draws into the off-screen `surface` (backed by `pixmap`, not the visible
 * popup window), then blits the finished frame to `popup` in one
 * xcb_copy_area. Building the frame directly on the window -- what nekos-wm's
 * compositor names as its pixmap -- risked the compositor's copy landing
 * mid-draw (half-painted rows/highlight), a real flicker on hover/hover-away.
 * Mirrors nekos-wm's own backbuffer_pixmap -> overlay copy in compositor.c. */
static void paint(xcb_connection_t *conn, xcb_window_t popup, xcb_pixmap_t pixmap,
                   xcb_gcontext_t gc, cairo_surface_t *surface, uint16_t width, uint16_t height,
                   const menu_item_t *items, int item_count, int hover_idx) {
    cairo_t *cr = cairo_create(surface);

    theme_panel_gradient(cr, 0, 0, width, height, 1);

    theme_font(cr, THEME_FONT_SM, 0);

    for (int i = 0; i < item_count; i++) {
        double y = i * ROW_HEIGHT;
        if (i == hover_idx) {
            theme_rgba(cr, THEME_ACCENT, 0.18);
            theme_rounded_rect(cr, 2, y + 1, width - 4, ROW_HEIGHT - 2, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgb(cr, THEME_FG);
        theme_text_ellipsized(cr, THEME_PAD, theme_baseline(y, ROW_HEIGHT, THEME_FONT_SM),
                               width - 2 * THEME_PAD, items[i].label);
    }

    theme_rgba(cr, THEME_ACCENT, THEME_BORDER_ALPHA);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, width - 1, height - 1);
    cairo_stroke(cr);

    cairo_destroy(cr);
    xcb_copy_area(conn, pixmap, popup, gc, 0, 0, 0, 0, width, height);
    xcb_flush(conn);
}

/* Item index at root-relative (rx, ry), or -1 if outside the popup or not
 * over any row. */
static int hit_test(int16_t px, int16_t py, uint16_t width, uint16_t height,
                     int item_count, int16_t rx, int16_t ry) {
    if (rx < px || rx >= px + (int16_t)width || ry < py || ry >= py + (int16_t)height) return -1;
    int idx = (ry - py) / ROW_HEIGHT;
    if (idx < 0 || idx >= item_count) return -1;
    return idx;
}

void menu_show(xcb_connection_t *conn, xcb_screen_t *screen, xcb_visualtype_t *visual,
               int16_t x, int16_t y, const menu_item_t *items, int item_count) {
    if (item_count <= 0) return;

    uint16_t width = MENU_WIDTH;
    uint16_t height = (uint16_t)(item_count * ROW_HEIGHT);

    if (x + width > screen->width_in_pixels) x = (int16_t)(screen->width_in_pixels - width);
    if (y + height > screen->height_in_pixels) y = (int16_t)(screen->height_in_pixels - height);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    xcb_window_t popup = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[3] = { screen->black_pixel, 1, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, popup, screen->root,
                       x, y, width, height, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);
    xcb_map_window(conn, popup);
    xcb_flush(conn);

    /* Active grab so we see every click on screen while the menu is open --
     * same technique nekos-wm's client.c uses for interactive move/resize.
     * owner_events=0 means motion/button events are reported to us using
     * this mask regardless of the popup window's own selected mask. */
    xcb_grab_pointer_cookie_t grab_cookie = xcb_grab_pointer(
        conn, 0, screen->root,
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(conn, grab_cookie, NULL);
    int grabbed = gr && gr->status == XCB_GRAB_STATUS_SUCCESS;
    free(gr);

    xcb_pixmap_t pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, pixmap, popup, width, height);
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, popup, 0, NULL);
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, pixmap, visual, width, height);
    int hover_idx = -1;
    int selected_idx = -1;

    if (!grabbed) {
        /* Can't reliably detect outside clicks without the grab -- close
         * immediately rather than leaving a menu stuck open forever. */
        goto teardown;
    }

    paint(conn, popup, pixmap, gc, surface, width, height, items, item_count, hover_idx);

    /* The compositor only sets up damage tracking for this override-redirect
     * popup once nekos-wm processes its MapNotify -- which races with the
     * initial paint above and can miss it, leaving the menu invisible until
     * some later repaint (e.g. a mouse-move hover). Poll with a short timeout
     * and repaint once ~40ms in (by which point the WM has surely processed
     * the map) so the menu reliably appears immediately, mouse-move or not. */
    int xfd = xcb_get_file_descriptor(conn);
    int settled = 0;
    int done = 0;
    while (!done) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, settled ? -1 : 40);
        if (!settled) {
            paint(conn, popup, pixmap, gc, surface, width, height, items, item_count, hover_idx);
            settled = 1;
        }

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t rt = event->response_type & ~0x80;
            if (rt == XCB_EXPOSE) {
                xcb_expose_event_t *ee = (xcb_expose_event_t *)event;
                if (ee->window == popup) {
                    paint(conn, popup, pixmap, gc, surface, width, height, items, item_count, hover_idx);
                }
            } else if (rt == XCB_MOTION_NOTIFY) {
                xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
                int idx = hit_test(x, y, width, height, item_count, me->root_x, me->root_y);
                if (idx != hover_idx) {
                    hover_idx = idx;
                    paint(conn, popup, pixmap, gc, surface, width, height, items, item_count, hover_idx);
                }
            } else if (rt == XCB_BUTTON_PRESS) {
                xcb_button_press_event_t *be = (xcb_button_press_event_t *)event;
                selected_idx = hit_test(x, y, width, height, item_count, be->root_x, be->root_y);
                free(event);
                done = 1;
                break;
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;
    }

    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);

teardown:
    cairo_surface_destroy(surface);
    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, pixmap);
    xcb_destroy_window(conn, popup);
    xcb_flush(conn);

    if (selected_idx >= 0 && items[selected_idx].action) {
        items[selected_idx].action(items[selected_idx].userdata);
    }
}
