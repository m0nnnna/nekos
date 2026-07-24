#ifndef NEKOS_EWMH_H
#define NEKOS_EWMH_H

#include <stdint.h>
#include <xcb/xcb.h>

typedef struct {
    int16_t x, y;
    uint16_t width, height;
} workarea_t;

/* Publishes an initial full-screen workarea (no docks reserved yet). Call
 * once at startup, after atoms_init(). */
void ewmh_workarea_init(void);

/* Recomputes and republishes the workarea from the current screen dimensions
 * and registered docks. Call after the root framebuffer is resized (RandR) so
 * _NET_WORKAREA and maximize geometry reflect the new size. */
void ewmh_recompute_workarea(void);

/* True if `window` declares itself as a dock/panel via
 * _NET_WM_WINDOW_TYPE_DOCK. Defaults to false (treated as a normal window)
 * if the property is absent, per the EWMH spec's recommended fallback. */
int ewmh_is_dock(xcb_window_t window);

/* True if `window` declares itself as the desktop via
 * _NET_WM_WINDOW_TYPE_DESKTOP -- nekos-wm manages exactly one such window
 * (nekos-desktop) at the bottom of the stack, undecorated and non-focusable. */
int ewmh_is_desktop(xcb_window_t window);

/* Registers `window` as a dock reserving screen space -- parses its
 * _NET_WM_STRUT_PARTIAL (preferred) or legacy _NET_WM_STRUT -- and
 * recomputes+publishes the workarea. Call once when a dock client maps. */
void ewmh_add_dock(xcb_window_t window);

/* Unregisters a dock (call on unmap) and recomputes+publishes the workarea. */
void ewmh_remove_dock(xcb_window_t window);

/* Current effective work area: screen bounds minus the max reservation per
 * edge across all registered docks (not full segment-aware intersection --
 * a pragmatic simplification fine for a single top bar; see the phase-2 plan). */
workarea_t ewmh_workarea(void);

#endif
