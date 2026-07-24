#include <stdlib.h>

#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

#include "wm.h"
#include "client.h"
#include "keys.h"

/* Verified live: Alt+Tab, Super+Tab, and Ctrl+Alt+Tab are all intercepted by
 * Windows before reaching this VNC session (Windows hooks the Alt key
 * itself). Ctrl+Shift+Tab avoids Alt but was *also* unreliable -- debug
 * logging showed single-modifier states (e.g. plain Control, state=0x4)
 * reliably reaching the grab, but the combined Control+Shift state (0x5)
 * never did, suggesting this VNC pipeline doesn't reliably deliver two
 * simultaneously-held modifiers. Ctrl+Tab (single modifier) is the more
 * robust choice here. Remap if needed. */
#define CYCLE_KEYSYM    XK_Tab
#define CYCLE_MODIFIERS XCB_MOD_MASK_CONTROL

void keys_init(void) {
    xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(conn);
    if (!keysyms) return;

    xcb_keycode_t *codes = xcb_key_symbols_get_keycode(keysyms, CYCLE_KEYSYM);
    if (codes) {
        /* Unlike xcb_grab_button's MOD_MASK_ANY, there's no modifier
         * wildcard for key grabs -- grab across the common NumLock/CapsLock
         * permutations explicitly, or the binding appears to work
         * "randomly" depending on lock-key state. Mod2 is the near-universal
         * Linux/X convention for NumLock. */
        static const uint16_t lock_perms[] = {
            0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
        };
        for (int i = 0; codes[i] != XCB_NO_SYMBOL; i++) {
            for (size_t p = 0; p < sizeof(lock_perms) / sizeof(lock_perms[0]); p++) {
                xcb_grab_key(conn, 1, screen->root, CYCLE_MODIFIERS | lock_perms[p],
                             codes[i], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            }
        }
        free(codes);
    }

    xcb_key_symbols_free(keysyms);
    xcb_flush(conn);
}

void keys_handle_key_press(xcb_key_press_event_t *ev) {
    (void)ev; /* only one combo is ever grabbed, no need to re-decode which key fired */
    client_cycle_focus();
}
