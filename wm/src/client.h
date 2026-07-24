#ifndef NEKOS_CLIENT_H
#define NEKOS_CLIENT_H

#include <xcb/xcb.h>

void client_handle_map_request(xcb_map_request_event_t *ev);

/* Scans root's existing children at startup and adopts any already mapped --
 * otherwise, restarting nekos-wm while windows are open orphans them: their
 * MapRequest fired for the *previous* WM instance, which this one never saw,
 * so it never calls compositor_manage() on them and they become invisible
 * (covered by the fresh COMPOSITE overlay window). Call once after
 * compositor_init()/ewmh_workarea_init(), before the event loop starts. */
void client_adopt_existing_windows(void);
void client_handle_configure_request(xcb_configure_request_event_t *ev);
void client_handle_unmap_notify(xcb_unmap_notify_event_t *ev);
/* Tears down a client whose window was destroyed without a clean unmap first
 * (crash/exit), so its frame doesn't linger as a compositor ghost. No-op if
 * the client already unmapped. */
void client_handle_destroy_notify(xcb_destroy_notify_event_t *ev);
void client_handle_button_press(xcb_button_press_event_t *ev);
void client_handle_motion_notify(xcb_motion_notify_event_t *ev);
void client_handle_button_release(xcb_button_release_event_t *ev);
void client_handle_client_message(xcb_client_message_event_t *ev);
/* Pointer left a frame: clears its paw-button hover highlight. */
void client_handle_leave_notify(xcb_leave_notify_event_t *ev);
/* A client's _NET_WM_NAME/WM_NAME changed: repaints its titlebar text. */
void client_handle_property_notify(xcb_property_notify_event_t *ev);

/* Advances focus to the next non-dock window (wrapping), transparently
 * un-minimizing it first if needed. No-op if no normal windows are open. */
void client_cycle_focus(void);

/* Reflows screen-anchored windows (maximized fills, centered frame images)
 * after a RandR root resize. Call after screen->width_in_pixels/height_in_pixels
 * are updated and ewmh_recompute_workarea() has run. */
void client_handle_screen_resize(void);

#endif
