#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "wm.h"
#include "atoms.h"

atoms_t atoms;

static xcb_atom_t intern(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

void atoms_init(void) {
    atoms._NET_SUPPORTED = intern("_NET_SUPPORTED");
    atoms._NET_SUPPORTING_WM_CHECK = intern("_NET_SUPPORTING_WM_CHECK");
    atoms._NET_WM_NAME = intern("_NET_WM_NAME");
    atoms.UTF8_STRING = intern("UTF8_STRING");
    atoms._NET_WM_WINDOW_TYPE = intern("_NET_WM_WINDOW_TYPE");
    atoms._NET_WM_WINDOW_TYPE_NORMAL = intern("_NET_WM_WINDOW_TYPE_NORMAL");
    atoms._NET_WM_WINDOW_TYPE_DOCK = intern("_NET_WM_WINDOW_TYPE_DOCK");
    atoms._NET_WM_WINDOW_TYPE_DESKTOP = intern("_NET_WM_WINDOW_TYPE_DESKTOP");
    atoms._NET_WM_STRUT = intern("_NET_WM_STRUT");
    atoms._NET_WM_STRUT_PARTIAL = intern("_NET_WM_STRUT_PARTIAL");
    atoms._NET_WORKAREA = intern("_NET_WORKAREA");
    atoms._NET_NUMBER_OF_DESKTOPS = intern("_NET_NUMBER_OF_DESKTOPS");
    atoms._NET_CURRENT_DESKTOP = intern("_NET_CURRENT_DESKTOP");
    atoms._NET_CLIENT_LIST = intern("_NET_CLIENT_LIST");
    atoms._NET_ACTIVE_WINDOW = intern("_NET_ACTIVE_WINDOW");
    atoms._NET_WM_STATE = intern("_NET_WM_STATE");
    atoms._NET_WM_STATE_HIDDEN = intern("_NET_WM_STATE_HIDDEN");
    atoms._NET_WM_STATE_MAXIMIZED_VERT = intern("_NET_WM_STATE_MAXIMIZED_VERT");
    atoms._NET_WM_STATE_MAXIMIZED_HORZ = intern("_NET_WM_STATE_MAXIMIZED_HORZ");
    atoms._NET_CLOSE_WINDOW = intern("_NET_CLOSE_WINDOW");
    atoms._NEKOS_SET_WALLPAPER = intern("_NEKOS_SET_WALLPAPER");
    atoms._NEKOS_WALLPAPER_PATH = intern("_NEKOS_WALLPAPER_PATH");
    atoms._NEKOS_ADD_FRAME = intern("_NEKOS_ADD_FRAME");
    atoms._NEKOS_FRAME_PATH = intern("_NEKOS_FRAME_PATH");
    atoms.WM_PROTOCOLS = intern("WM_PROTOCOLS");
    atoms.WM_DELETE_WINDOW = intern("WM_DELETE_WINDOW");
    atoms.WM_STATE = intern("WM_STATE");
}

void ewmh_init_supporting_wm(void) {
    xcb_window_t check = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, check, screen->root,
                       -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                       screen->root_visual, 0, NULL);

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, check,
                         atoms._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &check);

    static const char wm_name[] = "nekos-wm";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, check,
                         atoms._NET_WM_NAME, atoms.UTF8_STRING, 8,
                         (uint32_t)(sizeof(wm_name) - 1), wm_name);

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &check);

    xcb_atom_t supported[] = {
        atoms._NET_SUPPORTED,
        atoms._NET_SUPPORTING_WM_CHECK,
        atoms._NET_WM_NAME,
        atoms._NET_WM_WINDOW_TYPE,
        atoms._NET_WM_WINDOW_TYPE_NORMAL,
        atoms._NET_WM_WINDOW_TYPE_DOCK,
        atoms._NET_WM_WINDOW_TYPE_DESKTOP,
        atoms._NET_WM_STRUT,
        atoms._NET_WM_STRUT_PARTIAL,
        atoms._NET_WORKAREA,
        atoms._NET_NUMBER_OF_DESKTOPS,
        atoms._NET_CURRENT_DESKTOP,
        atoms._NET_CLIENT_LIST,
        atoms._NET_ACTIVE_WINDOW,
        atoms._NET_WM_STATE,
        atoms._NET_WM_STATE_HIDDEN,
        atoms._NET_WM_STATE_MAXIMIZED_VERT,
        atoms._NET_WM_STATE_MAXIMIZED_HORZ,
        atoms._NET_CLOSE_WINDOW,
    };
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_SUPPORTED, XCB_ATOM_ATOM, 32,
                         (uint32_t)(sizeof(supported) / sizeof(supported[0])), supported);

    uint32_t num_desktops = 1;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1, &num_desktops);

    uint32_t current_desktop = 0;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, &current_desktop);

    xcb_flush(conn);
}
