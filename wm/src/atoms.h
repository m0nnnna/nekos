#ifndef NEKOS_ATOMS_H
#define NEKOS_ATOMS_H

#include <xcb/xcb.h>

typedef struct {
    xcb_atom_t _NET_SUPPORTED;
    xcb_atom_t _NET_SUPPORTING_WM_CHECK;
    xcb_atom_t _NET_WM_NAME;
    xcb_atom_t UTF8_STRING;
    xcb_atom_t _NET_WM_WINDOW_TYPE;
    xcb_atom_t _NET_WM_WINDOW_TYPE_NORMAL;
    xcb_atom_t _NET_WM_WINDOW_TYPE_DOCK;
    xcb_atom_t _NET_WM_WINDOW_TYPE_DESKTOP;
    xcb_atom_t _NET_WM_STRUT;
    xcb_atom_t _NET_WM_STRUT_PARTIAL;
    xcb_atom_t _NET_WORKAREA;
    xcb_atom_t _NET_NUMBER_OF_DESKTOPS;
    xcb_atom_t _NET_CURRENT_DESKTOP;
    xcb_atom_t _NET_CLIENT_LIST;
    xcb_atom_t _NET_ACTIVE_WINDOW;
    xcb_atom_t _NET_WM_STATE;
    xcb_atom_t _NET_WM_STATE_HIDDEN;
    xcb_atom_t _NET_WM_STATE_MAXIMIZED_VERT;
    xcb_atom_t _NET_WM_STATE_MAXIMIZED_HORZ;
    xcb_atom_t _NET_CLOSE_WINDOW;
    /* nekos-custom (not EWMH, not part of _NET_SUPPORTED). Property-then-
     * message pairs: the sender sets the *_PATH string property on root,
     * then sends the corresponding message so the WM knows to (re)read it --
     * a ClientMessage's 20-byte payload can't hold an arbitrary file path. */
    xcb_atom_t _NEKOS_SET_WALLPAPER;
    xcb_atom_t _NEKOS_WALLPAPER_PATH;
    xcb_atom_t _NEKOS_ADD_FRAME;
    xcb_atom_t _NEKOS_FRAME_PATH;
    /* ICCCM (not _NET_-namespaced, not part of _NET_SUPPORTED). */
    xcb_atom_t WM_PROTOCOLS;
    xcb_atom_t WM_DELETE_WINDOW;
    xcb_atom_t WM_STATE;
} atoms_t;

extern atoms_t atoms;

/* Interns all atoms in `atoms` above. Call once after connecting. */
void atoms_init(void);

/* Creates the _NET_SUPPORTING_WM_CHECK dummy window, publishes _NET_SUPPORTED
 * and the static single-desktop properties. Call once after atoms_init(). */
void ewmh_init_supporting_wm(void);

#endif
