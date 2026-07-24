#ifndef NEKOS_BAR_MENU_H
#define NEKOS_BAR_MENU_H

#include <xcb/xcb.h>

typedef void (*menu_action_fn)(void *userdata);

typedef struct {
    const char *label;
    menu_action_fn action;
    void *userdata;
} menu_item_t;

/* Shows a small popup menu at (x, y) (clamped on-screen) and blocks until the
 * user picks an item or dismisses it by clicking elsewhere. Deliberately
 * modal -- runs its own local event loop rather than integrating into the
 * caller's main loop, since a menu interaction is brief and nothing else
 * needs to keep updating underneath it. Must be an override-redirect window
 * under the hood (so nekos-wm doesn't try to frame it), which means
 * nekos-wm's compositor must be redirecting override-redirect windows too --
 * see compositor_manage_override() in the WM -- or this will be invisible.
 */
void menu_show(xcb_connection_t *conn, xcb_screen_t *screen, xcb_visualtype_t *visual,
               int16_t x, int16_t y, const menu_item_t *items, int item_count);

#endif
