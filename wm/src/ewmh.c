#include <stdlib.h>

#include "wm.h"
#include "atoms.h"
#include "ewmh.h"

#define MAX_DOCKS 16

typedef struct {
    xcb_window_t window;
    uint32_t left, right, top, bottom;
} dock_t;

static dock_t docks[MAX_DOCKS];
static int dock_count = 0;
static workarea_t current_workarea;

static dock_t *find_dock(xcb_window_t window) {
    for (int i = 0; i < dock_count; i++) {
        if (docks[i].window == window) return &docks[i];
    }
    return NULL;
}

/* True if `window`'s _NET_WM_WINDOW_TYPE list contains `type`. */
static int has_window_type(xcb_window_t window, xcb_atom_t type) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, window, atoms._NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 8);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) return 0;

    int found = 0;
    int count = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
    xcb_atom_t *values = (xcb_atom_t *)xcb_get_property_value(reply);
    for (int i = 0; i < count; i++) {
        if (values[i] == type) {
            found = 1;
            break;
        }
    }
    free(reply);
    return found;
}

int ewmh_is_dock(xcb_window_t window) {
    return has_window_type(window, atoms._NET_WM_WINDOW_TYPE_DOCK);
}

int ewmh_is_desktop(xcb_window_t window) {
    return has_window_type(window, atoms._NET_WM_WINDOW_TYPE_DESKTOP);
}

/* Prefers the 12-value _NET_WM_STRUT_PARTIAL over the legacy 4-value
 * _NET_WM_STRUT; only the four edge-reservation values are used from either
 * (the partial variant's per-edge start/end segment fields are not applied --
 * see the max-reservation-per-edge simplification noted in ewmh.h). */
static void parse_strut(xcb_window_t window, dock_t *d) {
    d->left = d->right = d->top = d->bottom = 0;

    xcb_get_property_cookie_t partial_cookie = xcb_get_property(
        conn, 0, window, atoms._NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 12);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, partial_cookie, NULL);
    if (reply && xcb_get_property_value_length(reply) >= (int)(4 * sizeof(uint32_t))) {
        uint32_t *values = (uint32_t *)xcb_get_property_value(reply);
        d->left = values[0];
        d->right = values[1];
        d->top = values[2];
        d->bottom = values[3];
        free(reply);
        return;
    }
    free(reply);

    xcb_get_property_cookie_t legacy_cookie = xcb_get_property(
        conn, 0, window, atoms._NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4);
    reply = xcb_get_property_reply(conn, legacy_cookie, NULL);
    if (reply && xcb_get_property_value_length(reply) >= (int)(4 * sizeof(uint32_t))) {
        uint32_t *values = (uint32_t *)xcb_get_property_value(reply);
        d->left = values[0];
        d->right = values[1];
        d->top = values[2];
        d->bottom = values[3];
    }
    free(reply);
}

static void recompute_workarea(void) {
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    for (int i = 0; i < dock_count; i++) {
        if (docks[i].left > left) left = docks[i].left;
        if (docks[i].right > right) right = docks[i].right;
        if (docks[i].top > top) top = docks[i].top;
        if (docks[i].bottom > bottom) bottom = docks[i].bottom;
    }

    current_workarea.x = (int16_t)left;
    current_workarea.y = (int16_t)top;
    current_workarea.width = (uint16_t)(screen->width_in_pixels - left - right);
    current_workarea.height = (uint16_t)(screen->height_in_pixels - top - bottom);

    uint32_t values[4] = {
        (uint32_t)current_workarea.x,
        (uint32_t)current_workarea.y,
        current_workarea.width,
        current_workarea.height,
    };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_WORKAREA, XCB_ATOM_CARDINAL, 32, 4, values);
    xcb_flush(conn);
}

void ewmh_workarea_init(void) {
    recompute_workarea(); /* dock_count is 0 here -- yields full screen bounds */
}

void ewmh_recompute_workarea(void) {
    recompute_workarea(); /* re-derive from the current screen size + docks (e.g. after a RandR resize) */
}

void ewmh_add_dock(xcb_window_t window) {
    if (dock_count >= MAX_DOCKS) return;
    dock_t *d = &docks[dock_count++];
    d->window = window;
    parse_strut(window, d);
    recompute_workarea();
}

void ewmh_remove_dock(xcb_window_t window) {
    dock_t *d = find_dock(window);
    if (!d) return;
    int idx = (int)(d - docks);
    docks[idx] = docks[dock_count - 1];
    dock_count--;
    recompute_workarea();
}

workarea_t ewmh_workarea(void) {
    return current_workarea;
}
