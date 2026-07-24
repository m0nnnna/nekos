#ifndef NEKOS_KEYS_H
#define NEKOS_KEYS_H

#include <xcb/xcb.h>

/* Grabs the window-cycle keybinding on root. Call once after connecting. */
void keys_init(void);

void keys_handle_key_press(xcb_key_press_event_t *ev);

#endif
