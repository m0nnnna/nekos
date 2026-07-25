#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>

#include "wm.h"
#include "anim.h"
#include "atoms.h"
#include "compositor.h"
#include "decor.h"
#include "ewmh.h"
#include "image.h"
#include "client.h"

#define CASCADE_STEP     32
#define CASCADE_WRAP     240
#define MAX_CLIENTS      64
#define MIN_CLIENT_SIZE  50 /* floor on client width/height during an interactive resize */
/* Throttle how often an interactive resize actually reconfigures the client
 * and re-fetches its compositor pixmap -- mirrors compositor.c's
 * FRAME_INTERVAL_MS, since there's no point tearing down and recreating the
 * named pixmap (xcb_composite_name_window_pixmap + a fresh cairo_xcb_surface)
 * faster than the compositor can paint a new frame from it. Uncapped, a fast
 * resize drag over VNC was issuing that teardown/recreate on every single
 * POINTER_MOTION -- easily 100+/sec -- which is what made resizing feel
 * janky; the pointer coordinates are still tracked every event (cheap), just
 * not applied to the real window that often. */
#define RESIZE_THROTTLE_MS 11
/* Interactive move gets the identical throttle treatment, for the identical
 * reason: unthrottled, a fast titlebar drag was issuing xcb_configure_window
 * + send_synthetic_configure + xcb_flush on every single POINTER_MOTION --
 * again easily 100+/sec -- which is what made dragging feel janky. See
 * client_handle_motion_notify's INTERACT_MOVE branch. */
#define MOVE_THROTTLE_MS RESIZE_THROTTLE_MS
/* How close the pointer must get to a workarea edge, in root pixels, for a
 * titlebar drag to preview/apply an edge-snap. Generous enough to forgive a
 * little pointer imprecision without triggering from merely dragging near an
 * edge. */
#define SNAP_EDGE_PX 8

/* Edge-snap target: which half of the workarea a drag would land the window
 * in. SNAP_MAXIMIZE (drag to the top edge) is only ever a transient
 * interaction.pending_snap value -- applying it sets managed_client_t's
 * existing `maximized` flag instead of `snapped`, since a full-workarea
 * snap-via-drag is exactly the same state as the maximize button. */
typedef enum {
    SNAP_NONE,
    SNAP_LEFT,
    SNAP_RIGHT,
    SNAP_MAXIMIZE,
} snap_state_t;

typedef struct {
    xcb_window_t client;
    xcb_window_t frame;
    int16_t frame_x, frame_y;
    uint16_t client_width;  /* content size, i.e. sans decoration insets */
    uint16_t client_height;
    int is_dock;
    /* The single _NET_WM_WINDOW_TYPE_DESKTOP window (nekos-desktop): like a
     * dock it's undecorated and never focused, but it lives at the BOTTOM of
     * the stack instead of the top, and reserves no strut. */
    int is_desktop;
    int maximized;
    /* Which half of the workarea the window is snapped to, or SNAP_NONE.
     * Mutually exclusive with `maximized` (SNAP_MAXIMIZE, the zone value for
     * a drag-to-top-edge snap, is never stored here -- that case sets
     * `maximized` instead, same as the maximize button/double-click). */
    snap_state_t snapped;
    int16_t saved_frame_x, saved_frame_y;
    uint16_t saved_client_width, saved_client_height;
    int minimized;
    /* Decorative "frame" image windows (see frame_create()) -- client==frame
     * for these (nekos-wm owns and draws the single window directly, no
     * external app/reparenting involved). Never appear in _NET_CLIENT_LIST,
     * never take focus, never resize. */
    int is_frame;
    int frame_locked;
    cairo_surface_t *frame_image;
    /* Paw button currently under the pointer (hover feedback), or
     * DECOR_BUTTON_NONE. */
    decor_button_t hover_button;
    /* Count of UnmapNotifys to ignore because *we* caused them, not the client.
     * Reparenting an already-mapped window (only client_adopt_existing_windows
     * does this -- a live MapRequest's window is still unmapped) makes the
     * server emit a synthetic UnmapNotify that would otherwise be misread as a
     * voluntary withdraw/close. See client_handle_unmap_notify. */
    int pending_unmaps;
} managed_client_t;

/* ICCCM WM_STATE property values (not provided by any xcb header). */
#define WM_STATE_NORMAL  1
#define WM_STATE_ICONIC  3

static managed_client_t clients[MAX_CLIENTS];
static int client_count = 0;
static xcb_window_t focused_frame = XCB_NONE;

/* Set only while client_adopt_existing_windows() is framing a window that was
 * already mapped before nekos-wm took over; every managed-window map path reads
 * it to seed pending_unmaps (see the managed_client_t field). Live MapRequests
 * always carry an unmapped window, so this stays 0 for them. */
static int adopting_mapped_window = 0;

typedef enum {
    INTERACT_NONE,
    INTERACT_MOVE,
    INTERACT_RESIZE,
} interact_kind_t;

typedef enum {
    RESIZE_NONE,
    RESIZE_RIGHT,
    RESIZE_BOTTOM,
    RESIZE_BOTTOM_RIGHT,
    RESIZE_LEFT,
    RESIZE_TOP,
    RESIZE_TOP_LEFT,
    RESIZE_TOP_RIGHT,
    RESIZE_BOTTOM_LEFT,
} resize_edge_t;

/* Drag state for an in-progress move/resize. Keyed by frame window id (not a
 * pointer into `clients[]`) since that array removes entries by swapping the
 * last element into the gap -- a raw pointer could silently go stale if some
 * other client happened to unmap mid-drag. */
static struct {
    interact_kind_t kind;
    xcb_window_t frame;
    resize_edge_t edge;
    int16_t start_pointer_x, start_pointer_y;
    int16_t start_frame_x, start_frame_y;
    uint16_t start_frame_w, start_frame_h;
    /* RESIZE_THROTTLE_MS coalescing: has_pending_resize marks that motion
     * moved the pointer since the last size actually applied to the client;
     * pending_w/h is where it should end up next. last_resize_ms is when
     * that last apply happened. end_interaction() flushes a still-pending
     * resize on button-release so the window always lands exactly where the
     * pointer let go, never one throttle tick behind. */
    int has_pending_resize;
    uint16_t pending_w, pending_h;
    /* Only differ from start_frame_x/y for a left/top (or their corner)
     * resize, which has to move the frame's origin to keep the opposite
     * edge anchored while the near edge follows the pointer -- see
     * client_handle_motion_notify's INTERACT_RESIZE branch. */
    int16_t pending_x, pending_y;
    int64_t last_resize_ms;
    /* MOVE_THROTTLE_MS coalescing, mirroring has_pending_resize/pending_w/h/
     * last_resize_ms above: pending_move_x/y is where the frame should end up
     * next; end_interaction() flushes a still-pending move on button-release
     * so the window always lands exactly where the pointer let go. */
    int has_pending_move;
    int16_t pending_move_x, pending_move_y;
    int64_t last_move_ms;
    /* Edge-snap: which zone (if any) an INTERACT_MOVE drag currently has the
     * pointer over. Updated every motion event; applied (and cleared) in
     * end_interaction() on button-release. */
    snap_state_t pending_snap;
} interaction = { .kind = INTERACT_NONE, .frame = XCB_NONE, .edge = RESIZE_NONE, .pending_snap = SNAP_NONE };

/* Double-click-titlebar-to-toggle-maximize state. The maximize paw button is
 * a small target that ends up at the extreme edge of a wide/maximized
 * titlebar (observed in practice to be an unreliable click target), so this
 * is the more reliable primary way to toggle maximize; the button still
 * works too. */
#define DOUBLE_CLICK_MS 400
static xcb_timestamp_t last_titlebar_click_time = 0;
static xcb_window_t last_titlebar_click_frame = XCB_NONE;

static managed_client_t *find_by_client(xcb_window_t win) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].client == win) return &clients[i];
    }
    return NULL;
}

static managed_client_t *find_by_frame(xcb_window_t frame) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame == frame) return &clients[i];
    }
    return NULL;
}

/* Publishes _NET_CLIENT_LIST (ordered non-dock client XIDs) so nekos-bar (or
 * any other EWMH-aware tool) can discover open windows. Client windows are
 * never unmapped/reparented by minimize, only their frame, so list
 * membership is unaffected by minimize/restore either way. */
static void publish_client_list(void) {
    xcb_window_t list[MAX_CLIENTS];
    int n = 0;
    for (int i = 0; i < client_count; i++) {
        if (!clients[i].is_dock && !clients[i].is_frame && !clients[i].is_desktop)
            list[n++] = clients[i].client;
    }
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_CLIENT_LIST, XCB_ATOM_WINDOW, 32, (uint32_t)n, list);
}

/* Cascade placement counts only normal (non-dock) clients, so adding a dock
 * doesn't skew the spacing of subsequently-opened normal windows. */
static int count_normal_clients(void) {
    int n = 0;
    for (int i = 0; i < client_count; i++) {
        if (!clients[i].is_dock && !clients[i].is_frame && !clients[i].is_desktop) n++;
    }
    return n;
}

/* Fetches the client's title (_NET_WM_NAME preferred, core WM_NAME fallback;
 * same naive ASCII/Latin-1 passthrough nekos-bar uses). */
static void fetch_title(xcb_window_t client, char *out, size_t out_size) {
    out[0] = '\0';

    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, client, atoms._NET_WM_NAME, atoms.UTF8_STRING, 0, (uint32_t)(out_size - 1));
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (reply && xcb_get_property_value_length(reply) > 0) {
        int len = xcb_get_property_value_length(reply);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, xcb_get_property_value(reply), (size_t)len);
        out[len] = '\0';
        free(reply);
        return;
    }
    free(reply);

    cookie = xcb_get_property(conn, 0, client, XCB_ATOM_WM_NAME, XCB_ATOM_STRING,
                              0, (uint32_t)(out_size - 1));
    reply = xcb_get_property_reply(conn, cookie, NULL);
    if (reply && xcb_get_property_value_length(reply) > 0) {
        int len = xcb_get_property_value_length(reply);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, xcb_get_property_value(reply), (size_t)len);
        out[len] = '\0';
    }
    free(reply);
}

static void repaint_decor(managed_client_t *mc, int focused) {
    char title[128];
    fetch_title(mc->client, title, sizeof(title));
    decor_paint(mc->frame, mc->client_width + 2 * DECOR_BORDER,
                mc->client_height + TITLEBAR_HEIGHT + 2 * DECOR_BORDER, focused,
                title, mc->hover_button);
}

/* Reads a STRING property off root into `out` (NUL-terminated, truncated to
 * fit). Used for the *_PATH properties nekos-settings sets before sending
 * _NEKOS_SET_WALLPAPER/_NEKOS_ADD_FRAME -- a ClientMessage's 20-byte payload
 * can't hold an arbitrary file path, so the path travels via property
 * instead. Returns 0 (out left untouched) if the property is absent/empty. */
static int read_root_string_property(xcb_atom_t atom, char *out, size_t out_size) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, screen->root, atom, XCB_ATOM_STRING, 0, (uint32_t)(out_size - 1));
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) return 0;

    int len = xcb_get_property_value_length(reply);
    if (len <= 0) {
        free(reply);
        return 0;
    }
    if ((size_t)len >= out_size) len = (int)out_size - 1;
    memcpy(out, xcb_get_property_value(reply), (size_t)len);
    out[len] = '\0';
    free(reply);
    return 1;
}

/* Passive click-to-focus grab on a client's own window, active only while
 * it's unfocused. owner_events=0 (not 1) is deliberate: with 1, a toolkit
 * that already selected ButtonPress on one of its own subwidgets could
 * receive the notification instead of us, defeating the grab. SYNC pointer
 * mode freezes the pointer so we see the press before the client does; the
 * handler must call xcb_allow_events to release it again. */
static void grab_focus_click(xcb_window_t client) {
    xcb_grab_button(conn, 0, client, XCB_EVENT_MASK_BUTTON_PRESS,
                     XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
                     XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
}

static void ungrab_focus_click(xcb_window_t client) {
    xcb_ungrab_button(conn, XCB_BUTTON_INDEX_ANY, client, XCB_MOD_MASK_ANY);
}

/* ICCCM 4.1.5: tell the client its true root-relative geometry after any
 * WM-driven move/resize, since we own its position/size post-reparent and
 * it otherwise has no other way to find out. Uses frame_x/frame_y directly
 * (tracked on every move) rather than a GetGeometry round-trip. */
static void send_synthetic_configure(managed_client_t *mc) {
    xcb_configure_notify_event_t note;
    memset(&note, 0, sizeof(note));
    note.response_type = XCB_CONFIGURE_NOTIFY;
    note.event = mc->client;
    note.window = mc->client;
    note.above_sibling = XCB_NONE;
    note.x = (int16_t)(mc->frame_x + DECOR_BORDER);
    note.y = (int16_t)(mc->frame_y + TITLEBAR_HEIGHT);
    note.width = mc->client_width;
    note.height = mc->client_height;
    note.border_width = 0;
    note.override_redirect = 0;
    xcb_send_event(conn, 0, mc->client, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&note);
}

/* True if `client` advertises WM_DELETE_WINDOW support via WM_PROTOCOLS. */
static int client_supports_wm_delete(xcb_window_t client) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        conn, 0, client, atoms.WM_PROTOCOLS, XCB_ATOM_ATOM, 0, 20);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(conn, cookie, NULL);
    if (!reply) return 0;

    int supports = 0;
    int count = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
    xcb_atom_t *values = (xcb_atom_t *)xcb_get_property_value(reply);
    for (int i = 0; i < count; i++) {
        if (values[i] == atoms.WM_DELETE_WINDOW) {
            supports = 1;
            break;
        }
    }
    free(reply);
    return supports;
}

/* Asks the client to close itself gracefully (ICCCM WM_DELETE_WINDOW) if it
 * advertises support; force-terminates its connection otherwise. */
static void close_client_gracefully(managed_client_t *mc) {
    if (client_supports_wm_delete(mc->client)) {
        xcb_client_message_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.format = 32;
        ev.window = mc->client;
        ev.type = atoms.WM_PROTOCOLS;
        ev.data.data32[0] = atoms.WM_DELETE_WINDOW;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        /* Empty event mask is required by ICCCM 4.2.8 for WM_PROTOCOLS
         * messages -- a nonzero mask changes delivery semantics entirely. */
        xcb_send_event(conn, 0, mc->client, 0, (const char *)&ev);
    } else {
        xcb_kill_client(conn, mc->client);
    }
    xcb_flush(conn);
}

/* Applies a new content size: reconfigures the frame and the client
 * sub-window to match, notifies the client of its true geometry, and
 * refreshes the compositor pixmap + decorations. Shared by client-initiated
 * resizes (ConfigureRequest) and WM-driven interactive border-drag resizes. */
static void apply_client_size(managed_client_t *mc, uint16_t new_width, uint16_t new_height) {
    mc->client_width = new_width;
    mc->client_height = new_height;

    uint16_t frame_width = new_width + 2 * DECOR_BORDER;
    uint16_t frame_height = new_height + TITLEBAR_HEIGHT + 2 * DECOR_BORDER;

    uint32_t frame_values[2] = { frame_width, frame_height };
    xcb_configure_window(conn, mc->frame,
                          XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, frame_values);

    uint32_t client_values[4] = { DECOR_BORDER, TITLEBAR_HEIGHT, new_width, new_height };
    xcb_configure_window(conn, mc->client,
                          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                          client_values);

    send_synthetic_configure(mc);

    compositor_resize(mc->frame, frame_width, frame_height);
    repaint_decor(mc, focused_frame == mc->frame);

    xcb_flush(conn);
}

/* Superset of apply_client_size that also repositions the frame's origin --
 * needed for a left/top (or corner) interactive resize, where the edge under
 * the pointer moves but the opposite edge must stay anchored, so the frame's
 * x/y has to change alongside its width/height. A no-op position change
 * (new_x/new_y equal to the frame's current origin) behaves identically to
 * apply_client_size, so this is safe to use for every interactive-resize
 * edge, not just the ones that actually move the origin. Only used by the
 * interactive-resize path (client_handle_motion_notify/end_interaction) --
 * client-initiated ConfigureRequests and other apply_client_size callers
 * never need to move the frame. */
static void apply_client_resize_geometry(managed_client_t *mc, int16_t new_x, int16_t new_y,
                                          uint16_t new_width, uint16_t new_height) {
    mc->frame_x = new_x;
    mc->frame_y = new_y;
    mc->client_width = new_width;
    mc->client_height = new_height;

    uint16_t frame_width = new_width + 2 * DECOR_BORDER;
    uint16_t frame_height = new_height + TITLEBAR_HEIGHT + 2 * DECOR_BORDER;

    uint32_t frame_values[4] = { (uint32_t)new_x, (uint32_t)new_y, frame_width, frame_height };
    xcb_configure_window(conn, mc->frame,
                          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                          frame_values);

    uint32_t client_values[4] = { DECOR_BORDER, TITLEBAR_HEIGHT, new_width, new_height };
    xcb_configure_window(conn, mc->client,
                          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                          client_values);

    send_synthetic_configure(mc);

    compositor_move(mc->frame, new_x, new_y);
    compositor_resize(mc->frame, frame_width, frame_height);
    repaint_decor(mc, focused_frame == mc->frame);

    xcb_flush(conn);
}

/* Applies a new size to a dock (nekos-bar). Docks carry no decoration -- the
 * frame is exactly the client size and the client sits at the frame origin --
 * so they can't go through apply_client_size (which insets by the titlebar/
 * border). Used when a dock re-spans itself after a screen-size change. */
static void apply_dock_size(managed_client_t *mc, uint16_t new_width, uint16_t new_height) {
    mc->client_width = new_width;
    mc->client_height = new_height;

    uint32_t frame_values[2] = { new_width, new_height };
    xcb_configure_window(conn, mc->frame,
                          XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, frame_values);

    uint32_t client_values[4] = { 0, 0, new_width, new_height };
    xcb_configure_window(conn, mc->client,
                          XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                          client_values);

    compositor_resize(mc->frame, new_width, new_height);
    xcb_flush(conn);
}

/* Restores frame_x/frame_y/client_width/client_height from the saved_*
 * fields -- shared by un-maximizing, un-snapping, and drag-away-to-restore.
 * Does not touch mc->maximized/mc->snapped; callers clear whichever applies. */
static void restore_saved_geometry(managed_client_t *mc) {
    mc->frame_x = mc->saved_frame_x;
    mc->frame_y = mc->saved_frame_y;
    uint32_t pos[2] = { (uint32_t)mc->frame_x, (uint32_t)mc->frame_y };
    xcb_configure_window(conn, mc->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
    compositor_move(mc->frame, mc->frame_x, mc->frame_y);
    apply_client_size(mc, mc->saved_client_width, mc->saved_client_height);
}

static void save_current_geometry(managed_client_t *mc) {
    mc->saved_frame_x = mc->frame_x;
    mc->saved_frame_y = mc->frame_y;
    mc->saved_client_width = mc->client_width;
    mc->saved_client_height = mc->client_height;
}

/* Frame-coordinate geometry (outer bounds, including decoration) for a snap
 * zone within `wa`. SNAP_NONE is treated the same as SNAP_MAXIMIZE (the
 * whole workarea) since callers only ever pass one of the three real zones. */
static void snap_zone_geometry(workarea_t wa, snap_state_t zone,
                                int16_t *x, int16_t *y, uint16_t *w, uint16_t *h) {
    uint16_t half_w = wa.width / 2;
    switch (zone) {
    case SNAP_LEFT:
        *x = wa.x;
        *y = wa.y;
        *w = half_w;
        *h = wa.height;
        break;
    case SNAP_RIGHT:
        *x = (int16_t)(wa.x + half_w);
        *y = wa.y;
        *w = (uint16_t)(wa.width - half_w);
        *h = wa.height;
        break;
    case SNAP_MAXIMIZE:
    case SNAP_NONE:
    default:
        *x = wa.x;
        *y = wa.y;
        *w = wa.width;
        *h = wa.height;
        break;
    }
}

/* Applies a snap zone (left half / right half / full workarea maximize) to
 * `mc`, saving its current geometry first so it can be restored later.
 * Position is applied before apply_client_size(), since that call's
 * synthetic-ConfigureNotify reads mc->frame_x/frame_y -- doing it in this
 * order means the client gets one correct notification reflecting the final
 * geometry, not two. Shared by the maximize button/double-click, drag-to-
 * top-edge, and drag-to-side-edge. */
static void apply_snap_geometry(managed_client_t *mc, snap_state_t zone) {
    save_current_geometry(mc);

    workarea_t wa = ewmh_workarea();
    int16_t x, y;
    uint16_t w, h;
    snap_zone_geometry(wa, zone, &x, &y, &w, &h);

    mc->frame_x = x;
    mc->frame_y = y;
    uint32_t pos[2] = { (uint32_t)x, (uint32_t)y };
    xcb_configure_window(conn, mc->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
    compositor_move(mc->frame, x, y);

    uint16_t cw = (uint16_t)(w - 2 * DECOR_BORDER);
    uint16_t ch = (uint16_t)(h - TITLEBAR_HEIGHT - 2 * DECOR_BORDER);
    apply_client_size(mc, cw, ch);

    if (zone == SNAP_MAXIMIZE) {
        mc->maximized = 1;
        mc->snapped = SNAP_NONE;
    } else {
        mc->maximized = 0;
        mc->snapped = zone;
    }
}

/* Toggles between the window's normal floating geometry and filling the
 * current EWMH workarea. */
static void toggle_maximize(managed_client_t *mc) {
    if (mc->maximized) {
        restore_saved_geometry(mc);
        mc->maximized = 0;
    } else {
        apply_snap_geometry(mc, SNAP_MAXIMIZE);
    }
}

/* Restores a maximized or edge-snapped window to its pre-snap geometry right
 * as a titlebar drag begins on it, repositioning so the pointer stays at the
 * same fractional offset along the titlebar it grabbed -- otherwise the
 * titlebar would teleport out from under the cursor the instant the drag
 * starts. No-op if `mc` isn't currently maximized or snapped. */
static void unsnap_for_drag(managed_client_t *mc, int16_t pointer_root_x) {
    if (!mc->maximized && mc->snapped == SNAP_NONE) return;

    uint16_t old_frame_w = mc->client_width + 2 * DECOR_BORDER;
    double frac = old_frame_w ? (double)(pointer_root_x - mc->frame_x) / old_frame_w : 0.5;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    restore_saved_geometry(mc);
    mc->maximized = 0;
    mc->snapped = SNAP_NONE;

    uint16_t new_frame_w = mc->client_width + 2 * DECOR_BORDER;
    mc->frame_x = (int16_t)(pointer_root_x - frac * new_frame_w);
    uint32_t pos[2] = { (uint32_t)mc->frame_x, (uint32_t)mc->frame_y };
    xcb_configure_window(conn, mc->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
    compositor_move(mc->frame, mc->frame_x, mc->frame_y);
    send_synthetic_configure(mc);
    xcb_flush(conn);
}

/* Click-to-focus works both from the titlebar/border chrome (plain event
 * mask on the frame, since no child covers that area) and from anywhere in
 * a window's content area (a passive grab-and-replay on the client window --
 * see grab_focus_click). This function maintains the grab invariant: exactly
 * the focused client has no grab, every other normal (non-dock) one does. */
static void focus_window(xcb_window_t frame) {
    if (focused_frame == frame) return;

    xcb_window_t old_focused = focused_frame;
    focused_frame = frame;

    if (old_focused != XCB_NONE) {
        managed_client_t *old_mc = find_by_frame(old_focused);
        if (old_mc) {
            repaint_decor(old_mc, 0);
            if (!old_mc->is_dock) grab_focus_click(old_mc->client);
        }
    }

    xcb_window_t active = XCB_NONE;
    if (frame != XCB_NONE) {
        managed_client_t *mc = find_by_frame(frame);
        if (mc) {
            repaint_decor(mc, 1);
            if (!mc->is_dock) ungrab_focus_click(mc->client);
            xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, mc->client, XCB_CURRENT_TIME);
            uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
            xcb_configure_window(conn, frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
            compositor_raise(frame);
            active = mc->client;
        }
    }

    /* Publish _NET_ACTIVE_WINDOW so the bar can highlight the active task
     * (and any EWMH tool can query it). */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                         atoms._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 32, 1, &active);

    /* Docks must always stay on top -- both visually (compositor_raise, our
     * own independent paint order) and in real X stacking (xcb_configure_window
     * STACK_MODE_ABOVE), or a later-created/focused window can cover the bar
     * on screen while also sitting above it in real stacking, meaning clicks
     * meant for the bar would land on the window underneath instead even if
     * compositor_raise() alone made the bar paint on top visually. Re-asserted
     * on every focus change since new/refocused windows always start above
     * whatever came before otherwise. */
    for (int i = 0; i < client_count; i++) {
        if (clients[i].is_dock) {
            uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
            xcb_configure_window(conn, clients[i].frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
            compositor_raise(clients[i].frame);
        }
    }

    xcb_flush(conn);
}

/* Hit-test margin for resize edges/corners, in frame-relative pixels --
 * deliberately more generous than the drawn DECOR_BORDER (decor.h) so edges,
 * and especially corners (which need to satisfy two edges' margins at once),
 * are actually easy to grab regardless of how thin the visual border is
 * drawn. Same "forgive a little pointer imprecision" reasoning as
 * SNAP_EDGE_PX above. */
#define RESIZE_MARGIN 8

/* All 8 edges/corners, tested against the full frame rect -- including the
 * titlebar strip at the top, since that's the only place a top/top-corner
 * resize can be grabbed at all (frame geometry has no room above the
 * titlebar; see decor.c's cat-ear-accents comment). Safe to call
 * unconditionally from the titlebar-click path (client_handle_button_press)
 * as long as decor button hits are checked first there: a close/maximize/
 * minimize click landing near the top-right corner must never be swallowed
 * by a resize instead, and button-hit-testing runs and returns early before
 * this ever gets called for those pixels. */
static resize_edge_t hit_test_resize_edge(uint16_t frame_width, uint16_t frame_height,
                                           int16_t x, int16_t y) {
    int on_left   = x < RESIZE_MARGIN;
    int on_right  = x >= frame_width - RESIZE_MARGIN;
    int on_top    = y < RESIZE_MARGIN;
    int on_bottom = y >= frame_height - RESIZE_MARGIN;

    if (on_top && on_left) return RESIZE_TOP_LEFT;
    if (on_top && on_right) return RESIZE_TOP_RIGHT;
    if (on_bottom && on_left) return RESIZE_BOTTOM_LEFT;
    if (on_bottom && on_right) return RESIZE_BOTTOM_RIGHT;
    if (on_left) return RESIZE_LEFT;
    if (on_right) return RESIZE_RIGHT;
    if (on_top) return RESIZE_TOP;
    if (on_bottom) return RESIZE_BOTTOM;
    return RESIZE_NONE;
}

static void begin_interaction(managed_client_t *mc, interact_kind_t kind, resize_edge_t edge,
                               int16_t pointer_root_x, int16_t pointer_root_y) {
    xcb_grab_pointer_cookie_t cookie = xcb_grab_pointer(
        conn, 0, screen->root,
        XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
    xcb_grab_pointer_reply_t *reply = xcb_grab_pointer_reply(conn, cookie, NULL);
    int grabbed = reply && reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(reply);
    if (!grabbed) return;

    interaction.kind = kind;
    interaction.frame = mc->frame;
    interaction.edge = edge;
    interaction.start_pointer_x = pointer_root_x;
    interaction.start_pointer_y = pointer_root_y;
    interaction.start_frame_x = mc->frame_x;
    interaction.start_frame_y = mc->frame_y;
    interaction.start_frame_w = mc->client_width + 2 * DECOR_BORDER;
    interaction.start_frame_h = mc->client_height + TITLEBAR_HEIGHT + 2 * DECOR_BORDER;
    interaction.has_pending_resize = 0;
    interaction.last_resize_ms = anim_now_ms();
    interaction.has_pending_move = 0;
    interaction.last_move_ms = anim_now_ms();
    interaction.pending_snap = SNAP_NONE;
}

/* Actually reconfigures the frame to a new position and tells the client
 * (mirrors apply_client_size's shape) -- only called at MOVE_THROTTLE_MS
 * cadence from client_handle_motion_notify, plus once more from
 * end_interaction() to flush a final still-pending position. */
static void apply_client_move(managed_client_t *mc, int16_t new_x, int16_t new_y) {
    uint32_t values[2] = { (uint32_t)new_x, (uint32_t)new_y };
    xcb_configure_window(conn, mc->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
    mc->frame_x = new_x;
    mc->frame_y = new_y;
    compositor_move(mc->frame, new_x, new_y);
    send_synthetic_configure(mc);
    xcb_flush(conn);
}

static void end_interaction(void) {
    if (interaction.kind == INTERACT_NONE) return;
    /* A throttled-away resize (see RESIZE_THROTTLE_MS) may still be pending
     * when the button comes up -- apply it now so the window always ends at
     * exactly the size the pointer last indicated, not wherever the last
     * throttle tick happened to land. */
    if (interaction.kind == INTERACT_RESIZE && interaction.has_pending_resize) {
        managed_client_t *mc = find_by_frame(interaction.frame);
        if (mc) apply_client_resize_geometry(mc, interaction.pending_x, interaction.pending_y,
                                              interaction.pending_w, interaction.pending_h);
        interaction.has_pending_resize = 0;
    }
    /* A throttled-away move (see MOVE_THROTTLE_MS) may still be pending when
     * the button comes up -- apply it now so the window always ends exactly
     * where the pointer let go, not wherever the last throttle tick landed. */
    if (interaction.kind == INTERACT_MOVE && interaction.has_pending_move) {
        managed_client_t *mc = find_by_frame(interaction.frame);
        if (mc) apply_client_move(mc, interaction.pending_move_x, interaction.pending_move_y);
        interaction.has_pending_move = 0;
    }
    /* Edge-snap: if the drag ended over a snap zone, commit it now. */
    if (interaction.kind == INTERACT_MOVE && interaction.pending_snap != SNAP_NONE) {
        managed_client_t *mc = find_by_frame(interaction.frame);
        if (mc) apply_snap_geometry(mc, interaction.pending_snap);
        interaction.pending_snap = SNAP_NONE;
    }
    compositor_clear_snap_preview();
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    interaction.kind = INTERACT_NONE;
    interaction.frame = XCB_NONE;
    xcb_flush(conn);
}

static void set_wm_state(xcb_window_t client, uint32_t state) {
    uint32_t value[2] = { state, XCB_NONE }; /* [state, icon_window] per ICCCM */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client, atoms.WM_STATE,
                         atoms.WM_STATE, 32, 2, value);
}

/* Minimizes by unmapping the FRAME directly (not the client) -- this is the
 * key correctness decision. Frame-unmap generates an UnmapNotify with
 * window==frame, event==root, which client_handle_unmap_notify's
 * find_by_frame guard distinguishes from a real client close before it ever
 * reaches the find_by_client lookup. Unmapping a window doesn't change its
 * children's mapped-state bit, so the reparented client itself never
 * generates an UnmapNotify from this -- zero ambiguity with a close. */
static void minimize_client(managed_client_t *mc) {
    if (mc->minimized) return;
    if (interaction.frame == mc->frame) end_interaction();
    if (focused_frame == mc->frame) focus_window(XCB_NONE);

    mc->minimized = 1;
    /* The compositor animates the fade+shrink and unmaps the frame itself
     * when it finishes -- unmapping here would invalidate the named pixmap
     * mid-animation (see compositor_start_minimize). */
    compositor_start_minimize(mc->frame);
    set_wm_state(mc->client, WM_STATE_ICONIC);

    uint32_t hidden = atoms._NET_WM_STATE_HIDDEN;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, mc->client,
                         atoms._NET_WM_STATE, XCB_ATOM_ATOM, 32, 1, &hidden);
    xcb_flush(conn);
}

/* Makes `mc` the active window regardless of its current state -- un-
 * minimizing first if needed -- the single entry point _NET_ACTIVE_WINDOW
 * handling and Alt+Tab cycling both use. */
static void activate_client(managed_client_t *mc) {
    if (mc->minimized) {
        mc->minimized = 0;
        xcb_map_window(conn, mc->frame);
        compositor_show(mc->frame);
        set_wm_state(mc->client, WM_STATE_NORMAL);
        xcb_delete_property(conn, mc->client, atoms._NET_WM_STATE);
    }
    focus_window(mc->frame);
    xcb_flush(conn);
}

static void map_dock(xcb_map_request_event_t *ev, uint16_t client_width, uint16_t client_height,
                      int16_t client_x, int16_t client_y) {
    /* Docks/panels get no decoration and no cascade placement -- the client
     * (nekos-bar) sizes and positions itself; we just make it visible
     * through the same compositor pipeline as everything else. */
    xcb_window_t frame = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
    };

    xcb_create_window(conn, XCB_COPY_FROM_PARENT, frame, screen->root,
                       client_x, client_y, client_width, client_height, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    compositor_manage(frame, client_x, client_y, client_width, client_height);

    xcb_reparent_window(conn, ev->window, frame, 0, 0);
    xcb_map_window(conn, frame);
    xcb_map_window(conn, ev->window);
    xcb_flush(conn);
    compositor_notify_mapped(frame);

    clients[client_count].client = ev->window;
    clients[client_count].frame = frame;
    clients[client_count].frame_x = client_x;
    clients[client_count].frame_y = client_y;
    clients[client_count].client_width = client_width;
    clients[client_count].client_height = client_height;
    clients[client_count].is_dock = 1;
    clients[client_count].is_desktop = 0;
    clients[client_count].maximized = 0; /* array slots are reused (swap-remove), never leave stale state */
    clients[client_count].snapped = SNAP_NONE;
    clients[client_count].minimized = 0;
    clients[client_count].is_frame = 0;
    clients[client_count].frame_locked = 0;
    clients[client_count].frame_image = NULL;
    clients[client_count].hover_button = DECOR_BUTTON_NONE;
    clients[client_count].pending_unmaps = adopting_mapped_window ? 1 : 0;
    client_count++;

    ewmh_add_dock(ev->window);

    xcb_flush(conn);
}

/* Maps the single desktop window (nekos-desktop): undecorated and never
 * focused like a dock, but placed at the BOTTOM of the stack -- both in real X
 * stacking and the compositor's paint order -- and reserving no strut. Clicks
 * on empty desktop reach it because nothing is ever stacked below it. */
static void map_desktop(xcb_map_request_event_t *ev, uint16_t client_width, uint16_t client_height,
                         int16_t client_x, int16_t client_y) {
    xcb_window_t frame = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
    };

    xcb_create_window(conn, XCB_COPY_FROM_PARENT, frame, screen->root,
                       client_x, client_y, client_width, client_height, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    /* Background paint order == always behind every real window (same path the
     * decorative frame images use). */
    compositor_manage_background(frame, client_x, client_y, client_width, client_height);

    xcb_reparent_window(conn, ev->window, frame, 0, 0);
    xcb_map_window(conn, frame);
    xcb_map_window(conn, ev->window);

    uint32_t below = XCB_STACK_MODE_BELOW;
    xcb_configure_window(conn, frame, XCB_CONFIG_WINDOW_STACK_MODE, &below);

    xcb_flush(conn);
    compositor_notify_mapped(frame);

    clients[client_count].client = ev->window;
    clients[client_count].frame = frame;
    clients[client_count].frame_x = client_x;
    clients[client_count].frame_y = client_y;
    clients[client_count].client_width = client_width;
    clients[client_count].client_height = client_height;
    clients[client_count].is_dock = 0;
    clients[client_count].is_desktop = 1;
    clients[client_count].maximized = 0;
    clients[client_count].snapped = SNAP_NONE;
    clients[client_count].minimized = 0;
    clients[client_count].is_frame = 0;
    clients[client_count].frame_locked = 0;
    clients[client_count].frame_image = NULL;
    clients[client_count].hover_button = DECOR_BUTTON_NONE;
    clients[client_count].pending_unmaps = adopting_mapped_window ? 1 : 0;
    client_count++;

    xcb_flush(conn);
}

#define MAX_FRAME_DIM 300

/* Creates a decorative "frame" image window from `path`: loads it fit-within
 * MAX_FRAME_DIM x MAX_FRAME_DIM (aspect-preserving, no crop), centers it on
 * screen, and registers it via compositor_manage_background() so it always
 * paints behind real app windows (but above the wallpaper). Unlike a normal
 * window/dock, nekos-wm owns and draws this window directly -- there's no
 * external app to reparent, so client==frame (see managed_client_t's
 * is_frame comment). Silently does nothing if the image can't be loaded or
 * the client limit is reached. */
static void frame_create(const char *path) {
    if (client_count >= MAX_CLIENTS) {
        fprintf(stderr, "nekos-wm: client limit reached, ignoring add-frame request\n");
        return;
    }

    cairo_surface_t *image = image_load_scaled(path, MAX_FRAME_DIM, MAX_FRAME_DIM, /*cover=*/0);
    if (!image) {
        fprintf(stderr, "nekos-wm: could not load frame image %s\n", path);
        return;
    }

    uint16_t w = (uint16_t)cairo_image_surface_get_width(image);
    uint16_t h = (uint16_t)cairo_image_surface_get_height(image);
    int16_t x = (int16_t)((screen->width_in_pixels - w) / 2);
    int16_t y = (int16_t)((screen->height_in_pixels - h) / 2);

    xcb_window_t window = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = { screen->black_pixel, XCB_EVENT_MASK_BUTTON_PRESS };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root,
                       x, y, w, h, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    compositor_manage_background(window, x, y, w, h);
    xcb_map_window(conn, window);
    xcb_flush(conn);
    /* Deliberately NOT calling compositor_notify_mapped() here -- same
     * reasoning as compositor_manage_override's comment: fetching the named
     * pixmap this early (window just mapped, nothing drawn into it yet)
     * risks a compositable that never picks up later drawing (see that
     * comment for the mechanism). Painting the real image now that the
     * window is mapped, and letting the DamageNotify it generates drive the
     * first (lazily-timed) fetch in compositor_handle_damage_notify, is the
     * same proven-safe sequencing override-redirect popups already use --
     * verified by dumping the fetched surface to a PNG mid-debug and
     * confirming it holds the real image. Frames lose the open-fade
     * animation eager notify_mapped would have given them; a decorative
     * picture popping in flat is a fine trade for correctness. */
    frame_paint(window, image, w, h, 0);

    clients[client_count].client = window;
    clients[client_count].frame = window;
    clients[client_count].frame_x = x;
    clients[client_count].frame_y = y;
    clients[client_count].client_width = w;
    clients[client_count].client_height = h;
    clients[client_count].is_dock = 0;
    clients[client_count].is_desktop = 0;
    clients[client_count].maximized = 0;
    clients[client_count].snapped = SNAP_NONE;
    clients[client_count].minimized = 0;
    clients[client_count].is_frame = 1;
    clients[client_count].frame_locked = 0;
    clients[client_count].frame_image = image;
    clients[client_count].pending_unmaps = 0; /* client==frame, nothing external was reparented */
    client_count++;
}

void client_handle_map_request(xcb_map_request_event_t *ev) {
    /* A MapRequest for a window we already manage: some toolkits (GTK, when
     * realizing a dialog) issue a second MapWindow for the same window. Framing
     * it again would build a duplicate frame and reparent the client a second
     * time -- and that second reparent of an already-mapped window generates a
     * synthetic UnmapNotify that client_handle_unmap_notify then misreads as a
     * close, tearing the client down. The two bugs together thrash the window
     * (repeated map/unmap) until it ends up mapped at root with no frame and
     * uncomposited (invisible). Just re-assert the existing frame instead. */
    managed_client_t *existing = find_by_client(ev->window);
    if (existing) {
        xcb_map_window(conn, existing->frame);
        xcb_map_window(conn, ev->window);
        if (!existing->is_dock && !existing->is_desktop && !existing->is_frame)
            focus_window(existing->frame);
        xcb_flush(conn);
        return;
    }

    if (client_count >= MAX_CLIENTS) {
        fprintf(stderr, "nekos-wm: client limit reached, mapping unmanaged\n");
        xcb_map_window(conn, ev->window);
        xcb_flush(conn);
        return;
    }

    xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(
        conn, xcb_get_geometry(conn, ev->window), NULL);
    uint16_t client_width = geom ? geom->width : 640;
    uint16_t client_height = geom ? geom->height : 480;
    int16_t client_x = geom ? geom->x : 0;
    int16_t client_y = geom ? geom->y : 0;
    free(geom);

    if (ewmh_is_desktop(ev->window)) {
        map_desktop(ev, client_width, client_height, client_x, client_y);
        return;
    }

    if (ewmh_is_dock(ev->window)) {
        map_dock(ev, client_width, client_height, client_x, client_y);
        return;
    }

    workarea_t wa = ewmh_workarea();
    uint16_t wrap = wa.width > CASCADE_WRAP ? CASCADE_WRAP : wa.width;
    int16_t offset = (int16_t)((count_normal_clients() * CASCADE_STEP) % (wrap > 0 ? wrap : 1));
    int16_t x = wa.x + 40 + offset;
    int16_t y = wa.y + 40 + offset;

    uint16_t frame_width = client_width + 2 * DECOR_BORDER;
    uint16_t frame_height = client_height + TITLEBAR_HEIGHT + 2 * DECOR_BORDER;

    xcb_window_t frame = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        /* POINTER_MOTION + LEAVE_WINDOW drive the paw-button hover feedback
         * (see client_handle_motion_notify / client_handle_leave_notify). */
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_LEAVE_WINDOW,
    };

    /* Native X border_width is 0: all chrome (titlebar/border/motifs) is
     * Cairo-drawn onto the frame itself in decor.c, not an X border. */
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, frame, screen->root,
                       x, y, frame_width, frame_height, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    compositor_manage(frame, x, y, frame_width, frame_height);

    xcb_reparent_window(conn, ev->window, frame, DECOR_BORDER, TITLEBAR_HEIGHT);
    xcb_map_window(conn, frame);
    xcb_map_window(conn, ev->window);
    xcb_flush(conn);
    compositor_notify_mapped(frame);

    clients[client_count].client = ev->window;
    clients[client_count].frame = frame;
    clients[client_count].frame_x = x;
    clients[client_count].frame_y = y;
    clients[client_count].client_width = client_width;
    clients[client_count].client_height = client_height;
    clients[client_count].is_dock = 0;
    clients[client_count].is_desktop = 0;
    clients[client_count].maximized = 0; /* array slots are reused (swap-remove), never leave stale state */
    clients[client_count].snapped = SNAP_NONE;
    clients[client_count].minimized = 0;
    clients[client_count].is_frame = 0;
    clients[client_count].frame_locked = 0;
    clients[client_count].frame_image = NULL;
    clients[client_count].hover_button = DECOR_BUTTON_NONE;
    clients[client_count].pending_unmaps = adopting_mapped_window ? 1 : 0;
    client_count++;

    /* Watch the client for title changes so the titlebar text stays live.
     * This selects PROPERTY_CHANGE for OUR connection only -- it doesn't
     * disturb whatever event mask the client selected for itself. */
    uint32_t prop_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn, ev->window, XCB_CW_EVENT_MASK, &prop_mask);

    grab_focus_click(ev->window); /* focus_window() ungrabs it immediately below since it's about to be focused */
    set_wm_state(ev->window, WM_STATE_NORMAL);
    focus_window(frame); /* also paints its titlebar as focused */
    publish_client_list();

    xcb_flush(conn);
}

void client_adopt_existing_windows(void) {
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), NULL);
    if (!tree) return;

    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);
    for (int i = 0; i < n; i++) {
        xcb_window_t w = children[i];
        xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
            conn, xcb_get_window_attributes(conn, w), NULL);
        if (!attr) continue;
        int viewable = (attr->map_state == XCB_MAP_STATE_VIEWABLE);
        int override_redirect = attr->override_redirect;
        free(attr);
        if (!viewable || override_redirect) continue;

        xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), NULL);
        uint16_t width = geom ? geom->width : 0;
        uint16_t height = geom ? geom->height : 0;
        free(geom);

        xcb_map_request_event_t fake_ev;
        memset(&fake_ev, 0, sizeof(fake_ev));
        fake_ev.window = w;
        /* This window is already mapped, so the framing reparent inside will
         * emit a synthetic UnmapNotify -- flag it so the new client swallows
         * that one unmap instead of tearing itself straight back down. */
        adopting_mapped_window = 1;
        client_handle_map_request(&fake_ev);
        adopting_mapped_window = 0;

        /* Reparenting an already-mapped window implicitly unmaps+remaps it
         * (ICCCM/X11 semantics) -- the app itself has no reason to think it
         * needs to repaint after that (nothing visually changed from its
         * point of view), so the COMPOSITE-captured content can stay blank
         * until something else prompts a real redraw. A synthetic Expose
         * nudges well-behaved toolkits (which don't distinguish synthetic
         * from real Expose) into repainting immediately. */
        xcb_expose_event_t expose;
        memset(&expose, 0, sizeof(expose));
        expose.response_type = XCB_EXPOSE;
        expose.window = w;
        expose.width = width;
        expose.height = height;
        xcb_send_event(conn, 0, w, XCB_EVENT_MASK_EXPOSURE, (const char *)&expose);
    }
    xcb_flush(conn);
    free(tree);
}

void client_handle_configure_request(xcb_configure_request_event_t *ev) {
    managed_client_t *mc = find_by_client(ev->window);
    if (!mc) {
        /* Not a window we're managing yet (e.g. not-yet-mapped) -- just
         * honor whatever it asked for directly. */
        uint32_t values[7];
        int i = 0;
        uint16_t mask = 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X) {
            mask |= XCB_CONFIG_WINDOW_X;
            values[i++] = (uint32_t)ev->x;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) {
            mask |= XCB_CONFIG_WINDOW_Y;
            values[i++] = (uint32_t)ev->y;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
            mask |= XCB_CONFIG_WINDOW_WIDTH;
            values[i++] = ev->width;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
            mask |= XCB_CONFIG_WINDOW_HEIGHT;
            values[i++] = ev->height;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
            mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
            values[i++] = ev->border_width;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
            mask |= XCB_CONFIG_WINDOW_SIBLING;
            values[i++] = ev->sibling;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
            mask |= XCB_CONFIG_WINDOW_STACK_MODE;
            values[i++] = ev->stack_mode;
        }
        xcb_configure_window(conn, ev->window, mask, values);
        xcb_flush(conn);
        return;
    }

    /* Once reparented, the WM (not the client) owns the frame's screen
     * position and the client's position within it -- only the requested
     * size is honored here, per ICCCM 4.1.5. */
    uint16_t new_width = (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) ? ev->width : mc->client_width;
    uint16_t new_height = (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) ? ev->height : mc->client_height;

    /* Docks (nekos-bar) and the desktop get an undecorated 1:1 frame when they
     * re-span after a screen resize -- apply_client_size would wrongly inset
     * them by the titlebar/border. */
    if (mc->is_dock || mc->is_desktop) {
        apply_dock_size(mc, new_width, new_height);
        return;
    }

    apply_client_size(mc, new_width, new_height);
}

/* Reflows screen-anchored windows after a root resize (RandR): maximized and
 * edge-snapped windows re-fill their slot of the recomputed workarea, and
 * decorative frame images re-center. Docks re-span themselves (they issue
 * their own ConfigureRequest, handled above), and floating windows are
 * intentionally left where they are, matching conventional WM behavior.
 * ewmh_recompute_workarea() must have run first so ewmh_workarea() reflects
 * the new screen size. */
void client_handle_screen_resize(void) {
    workarea_t wa = ewmh_workarea();

    for (int i = 0; i < client_count; i++) {
        managed_client_t *mc = &clients[i];

        if (mc->maximized || mc->snapped != SNAP_NONE) {
            snap_state_t zone = mc->maximized ? SNAP_MAXIMIZE : mc->snapped;
            int16_t x, y;
            uint16_t w, h;
            snap_zone_geometry(wa, zone, &x, &y, &w, &h);

            mc->frame_x = x;
            mc->frame_y = y;
            uint32_t pos[2] = { (uint32_t)x, (uint32_t)y };
            xcb_configure_window(conn, mc->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
            compositor_move(mc->frame, x, y);

            uint16_t cw = (uint16_t)(w - 2 * DECOR_BORDER);
            uint16_t ch = (uint16_t)(h - TITLEBAR_HEIGHT - 2 * DECOR_BORDER);
            apply_client_size(mc, cw, ch);
        } else if (mc->is_frame) {
            int16_t x = (int16_t)((screen->width_in_pixels - mc->client_width) / 2);
            int16_t y = (int16_t)((screen->height_in_pixels - mc->client_height) / 2);
            mc->frame_x = x;
            mc->frame_y = y;
            uint32_t pos[2] = { (uint32_t)x, (uint32_t)y };
            xcb_configure_window(conn, mc->frame, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
            compositor_move(mc->frame, x, y);
        }
    }

    xcb_flush(conn);
}

void client_handle_unmap_notify(xcb_unmap_notify_event_t *ev) {
    /* Minimize unmaps the FRAME directly (see minimize_client) -- that event
     * is reported with window==frame, which find_by_frame matches here,
     * distinguishing it from a real client close before the find_by_client
     * lookup below ever runs. */
    if (find_by_frame(ev->window)) return;

    managed_client_t *mc = find_by_client(ev->window);
    if (!mc) {
        /* Not one of ours -- might be an override-redirect popup/tooltip the
         * compositor is painting (see compositor_manage_override). Safe
         * no-op if it isn't tracked there either. */
        compositor_unmanage_override(ev->window);
        return;
    }

    /* Swallow an UnmapNotify we induced ourselves (by reparenting an already-
     * mapped window while adopting it) rather than treating it as the client
     * withdrawing. Without this, an adopted window tears itself down the instant
     * it's framed. See pending_unmaps in managed_client_t. */
    if (mc->pending_unmaps > 0) {
        mc->pending_unmaps--;
        return;
    }

    if (mc->is_dock) ewmh_remove_dock(ev->window);
    if (interaction.frame == mc->frame) end_interaction();
    if (!mc->is_dock) ungrab_focus_click(mc->client); /* hygiene: grabs survive reparenting */

    xcb_reparent_window(conn, mc->client, screen->root, 0, 0);

    xcb_window_t frame = mc->frame;
    int idx = (int)(mc - clients);
    clients[idx] = clients[client_count - 1];
    client_count--;

    if (focused_frame == frame) {
        focused_frame = XCB_NONE;
        /* Nothing else claims focus automatically yet -- a focus-follows-
         * stacking-order (or most-recently-used) policy is a natural future
         * addition once there's more than one window to fall back to. */
    }

    /* The frame itself stays alive (still composited) through its close
     * animation; compositor.c keeps tracking it and main.c destroys the
     * actual X window once compositor_take_finished_close() returns it. */
    compositor_start_close(frame);
    publish_client_list();

    xcb_flush(conn);
}

void client_handle_destroy_notify(xcb_destroy_notify_event_t *ev) {
    /* A client window destroyed outright -- the app exited or crashed without
     * a clean unmap first (e.g. a process that maps a window then dies).
     * Without this, the frame would never be torn down and the compositor
     * would keep painting a ghost of it. Mirrors the unmap teardown, minus
     * anything that would touch the now-destroyed client window: no
     * reparent-to-root and no ungrab_focus_click (both would just raise
     * BadWindow on a window that no longer exists). If the client already
     * unmapped cleanly first, it's gone from clients[] and find_by_client
     * returns NULL here, making this a no-op.
     *
     * Note the frame itself (a separate window we own) is still alive, so
     * compositor_start_close + main.c's eventual xcb_destroy_window operate on
     * a valid window -- only the reparented client child was destroyed. */
    managed_client_t *mc = find_by_client(ev->window);
    if (!mc) {
        /* Could be a tracked override-redirect popup destroyed without an
         * unmap. Safe no-op if it isn't tracked there either. */
        compositor_unmanage_override(ev->window);
        return;
    }

    if (mc->is_dock) ewmh_remove_dock(ev->window);
    if (interaction.frame == mc->frame) end_interaction();
    /* Decorative frame images are our own windows (client==frame) and own a
     * cairo surface the normal delete path frees -- free it here too so an
     * externally-destroyed one doesn't leak. */
    if (mc->is_frame && mc->frame_image) cairo_surface_destroy(mc->frame_image);

    xcb_window_t frame = mc->frame;
    int idx = (int)(mc - clients);
    clients[idx] = clients[client_count - 1];
    client_count--;

    if (focused_frame == frame) {
        focused_frame = XCB_NONE;
    }

    compositor_start_close(frame);
    publish_client_list();

    xcb_flush(conn);
}

void client_handle_button_press(xcb_button_press_event_t *ev) {
    managed_client_t *mc = find_by_frame(ev->event);
    if (!mc) {
        /* Not a frame-chrome click -- check if it's a passive-grab focus
         * click landing on a client's own content area (see
         * grab_focus_click). Replay it so the app still receives the press
         * normally afterward, same as if we weren't grabbing at all. */
        mc = find_by_client(ev->event);
        if (!mc) return;
        focus_window(mc->frame);
        xcb_allow_events(conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        xcb_flush(conn);
        return;
    }

    if (mc->is_frame) {
        /* No titlebar -- the whole window is a drag handle when unlocked,
         * except the two small corner buttons. Never focuses (frames don't
         * take focus at all) and never resizes. */
        frame_hit_t hit = frame_hit_test(mc->client_width, mc->client_height, ev->event_x, ev->event_y);
        if (hit == FRAME_HIT_DELETE) {
            /* Immediate synchronous cleanup (clients[] removal + our own
             * frame_image surface) before the fade visually finishes --
             * exactly mirrors how client_handle_unmap_notify already does
             * clients[] removal alongside compositor_start_close() for a
             * real window close, just triggered from a click instead of an
             * UnmapNotify. */
            cairo_surface_destroy(mc->frame_image);
            xcb_window_t window = mc->frame;
            int idx = (int)(mc - clients);
            clients[idx] = clients[client_count - 1];
            client_count--;
            compositor_start_close(window);
            return;
        }
        if (hit == FRAME_HIT_LOCK) {
            mc->frame_locked = !mc->frame_locked;
            frame_paint(mc->frame, mc->frame_image, mc->client_width, mc->client_height, mc->frame_locked);
            return;
        }
        if (!mc->frame_locked) {
            begin_interaction(mc, INTERACT_MOVE, RESIZE_NONE, ev->root_x, ev->root_y);
        }
        return;
    }

    uint16_t frame_width = mc->client_width + 2 * DECOR_BORDER;
    uint16_t frame_height = mc->client_height + TITLEBAR_HEIGHT + 2 * DECOR_BORDER;

    if (ev->event_y < TITLEBAR_HEIGHT) {
        decor_button_t button = decor_hit_test(frame_width, ev->event_x, ev->event_y);

        if (button == DECOR_BUTTON_CLOSE) {
            close_client_gracefully(mc);
            return;
        }
        if (button == DECOR_BUTTON_MAXIMIZE) {
            focus_window(mc->frame);
            toggle_maximize(mc);
            return;
        }
        if (button == DECOR_BUTTON_MINIMIZE) {
            minimize_client(mc);
            return;
        }

        /* Top edge/corner resize: only reachable here once no button was
         * hit above, so a close/maximize/minimize click near the top-right
         * corner always wins over this. This is the only place a top-edge
         * or top-corner resize can start at all -- the titlebar strip is the
         * only frame real estate above the client (see hit_test_resize_edge's
         * comment) -- while left/right/bottom-only edges reaching into this
         * same strip (e.g. the thin column beside the titlebar) are also
         * caught here rather than falling through to the move/focus logic
         * below. */
        resize_edge_t top_edge = hit_test_resize_edge(frame_width, frame_height,
                                                        ev->event_x, ev->event_y);
        if (top_edge != RESIZE_NONE && !mc->maximized) {
            focus_window(mc->frame);
            begin_interaction(mc, INTERACT_RESIZE, top_edge, ev->root_x, ev->root_y);
            return;
        }

        focus_window(mc->frame);

        if (last_titlebar_click_frame == mc->frame &&
            (ev->time - last_titlebar_click_time) < DOUBLE_CLICK_MS) {
            last_titlebar_click_frame = XCB_NONE; /* consume it -- don't chain into a triple-click */
            toggle_maximize(mc);
            return;
        }
        last_titlebar_click_time = ev->time;
        last_titlebar_click_frame = mc->frame;

        /* Dragging a maximized or edge-snapped window's titlebar restores it
         * to its pre-snap size first (keeping the pointer at the same
         * relative spot along the titlebar), then starts the move normally
         * -- resize drags still stay disabled on a maximized window (below),
         * since there's no sensible "resize" of a window filling the whole
         * workarea. */
        unsnap_for_drag(mc, ev->root_x);
        begin_interaction(mc, INTERACT_MOVE, RESIZE_NONE, ev->root_x, ev->root_y);
        return;
    }

    resize_edge_t edge = hit_test_resize_edge(frame_width, frame_height, ev->event_x, ev->event_y);
    focus_window(mc->frame);
    if (edge != RESIZE_NONE && !mc->maximized) {
        begin_interaction(mc, INTERACT_RESIZE, edge, ev->root_x, ev->root_y);
    }
    /* A border click below the titlebar that missed every edge/corner
     * margin just focuses, same as clicking anywhere else. */
}

/* Hover tracking for the paw buttons: repaints the decor only when the
 * hovered button actually changes, so idle mouse travel costs nothing. */
static void update_hover(managed_client_t *mc, int16_t x, int16_t y) {
    decor_button_t hover = DECOR_BUTTON_NONE;
    if (y >= 0 && y < TITLEBAR_HEIGHT) {
        hover = decor_hit_test((uint16_t)(mc->client_width + 2 * DECOR_BORDER), x, y);
    }
    if (hover != mc->hover_button) {
        mc->hover_button = hover;
        repaint_decor(mc, focused_frame == mc->frame);
        xcb_flush(conn);
    }
}

void client_handle_leave_notify(xcb_leave_notify_event_t *ev) {
    managed_client_t *mc = find_by_frame(ev->event);
    if (!mc || mc->is_frame || mc->is_dock || mc->is_desktop) return;
    if (mc->hover_button != DECOR_BUTTON_NONE) {
        mc->hover_button = DECOR_BUTTON_NONE;
        repaint_decor(mc, focused_frame == mc->frame);
        xcb_flush(conn);
    }
}

void client_handle_property_notify(xcb_property_notify_event_t *ev) {
    if (ev->atom != atoms._NET_WM_NAME && ev->atom != XCB_ATOM_WM_NAME) return;
    managed_client_t *mc = find_by_client(ev->window);
    if (!mc || mc->is_frame || mc->is_dock || mc->is_desktop) return;
    repaint_decor(mc, focused_frame == mc->frame);
    xcb_flush(conn);
}

void client_handle_motion_notify(xcb_motion_notify_event_t *ev) {
    if (interaction.kind == INTERACT_NONE) {
        managed_client_t *mc = find_by_frame(ev->event);
        if (mc && !mc->is_frame && !mc->is_dock && !mc->is_desktop) {
            update_hover(mc, ev->event_x, ev->event_y);
        }
        return;
    }

    managed_client_t *mc = find_by_frame(interaction.frame);
    if (!mc) {
        end_interaction();
        return;
    }

    int16_t dx = (int16_t)(ev->root_x - interaction.start_pointer_x);
    int16_t dy = (int16_t)(ev->root_y - interaction.start_pointer_y);

    if (interaction.kind == INTERACT_MOVE) {
        int16_t new_x = (int16_t)(interaction.start_frame_x + dx);
        int16_t new_y = (int16_t)(interaction.start_frame_y + dy);

        /* Track where the pointer says the frame should be on every motion
         * event (cheap: just arithmetic), but only actually reconfigure the
         * frame at MOVE_THROTTLE_MS intervals -- see the constant's comment.
         * end_interaction() applies any still-pending position on
         * button-release, so this never leaves the window visibly lagging
         * behind a stationary pointer. */
        interaction.pending_move_x = new_x;
        interaction.pending_move_y = new_y;
        interaction.has_pending_move = 1;

        int64_t move_now = anim_now_ms();
        if (move_now - interaction.last_move_ms >= MOVE_THROTTLE_MS) {
            interaction.last_move_ms = move_now;
            interaction.has_pending_move = 0;
            apply_client_move(mc, new_x, new_y);
        }

        /* Edge-snap preview: the pointer (not the window edge) reaching a
         * workarea edge previews snapping to that half/full-screen slot;
         * end_interaction() applies it if the drag ends while still there.
         * Checked in top-then-sides order so a drag into the top-left corner
         * previews maximize, not a left-half snap. */
        workarea_t wa = ewmh_workarea();
        snap_state_t zone = SNAP_NONE;
        if (ev->root_y <= wa.y + SNAP_EDGE_PX) zone = SNAP_MAXIMIZE;
        else if (ev->root_x <= wa.x + SNAP_EDGE_PX) zone = SNAP_LEFT;
        else if (ev->root_x >= wa.x + wa.width - SNAP_EDGE_PX) zone = SNAP_RIGHT;

        if (zone != interaction.pending_snap) {
            interaction.pending_snap = zone;
            if (zone == SNAP_NONE) {
                compositor_clear_snap_preview();
            } else {
                int16_t px, py;
                uint16_t pw, ph;
                snap_zone_geometry(wa, zone, &px, &py, &pw, &ph);
                compositor_set_snap_preview(px, py, pw, ph);
            }
        }
        return;
    }

    /* INTERACT_RESIZE */
    int new_client_w = mc->client_width;
    int new_client_h = mc->client_height;
    int16_t new_frame_x = interaction.start_frame_x;
    int16_t new_frame_y = interaction.start_frame_y;

    int grows_right  = interaction.edge == RESIZE_RIGHT || interaction.edge == RESIZE_TOP_RIGHT ||
                        interaction.edge == RESIZE_BOTTOM_RIGHT;
    int grows_left   = interaction.edge == RESIZE_LEFT || interaction.edge == RESIZE_TOP_LEFT ||
                        interaction.edge == RESIZE_BOTTOM_LEFT;
    int grows_bottom = interaction.edge == RESIZE_BOTTOM || interaction.edge == RESIZE_BOTTOM_LEFT ||
                        interaction.edge == RESIZE_BOTTOM_RIGHT;
    int grows_top    = interaction.edge == RESIZE_TOP || interaction.edge == RESIZE_TOP_LEFT ||
                        interaction.edge == RESIZE_TOP_RIGHT;

    if (grows_right) {
        new_client_w = (int)(interaction.start_frame_w - 2 * DECOR_BORDER) + dx;
        if (new_client_w < MIN_CLIENT_SIZE) new_client_w = MIN_CLIENT_SIZE;
    }
    if (grows_left) {
        /* The right edge has to stay anchored, so frame_x moves by however
         * much the width actually changed -- which may be less than dx once
         * MIN_CLIENT_SIZE clamps it -- not by dx itself, or the right edge
         * would keep drifting right even after the drag clamps. */
        int start_client_w = (int)interaction.start_frame_w - 2 * DECOR_BORDER;
        int want_w = start_client_w - dx;
        new_client_w = want_w < MIN_CLIENT_SIZE ? MIN_CLIENT_SIZE : want_w;
        new_frame_x = (int16_t)(interaction.start_frame_x + (start_client_w - new_client_w));
    }
    if (grows_bottom) {
        new_client_h = (int)(interaction.start_frame_h - TITLEBAR_HEIGHT - 2 * DECOR_BORDER) + dy;
        if (new_client_h < MIN_CLIENT_SIZE) new_client_h = MIN_CLIENT_SIZE;
    }
    if (grows_top) {
        /* Mirrors grows_left: keeps the bottom edge anchored. */
        int start_client_h = (int)interaction.start_frame_h - TITLEBAR_HEIGHT - 2 * DECOR_BORDER;
        int want_h = start_client_h - dy;
        new_client_h = want_h < MIN_CLIENT_SIZE ? MIN_CLIENT_SIZE : want_h;
        new_frame_y = (int16_t)(interaction.start_frame_y + (start_client_h - new_client_h));
    }

    /* Track where the pointer says the edge should be on every motion event
     * (cheap: just arithmetic), but only actually reconfigure the client and
     * re-fetch its compositor pixmap at RESIZE_THROTTLE_MS intervals -- see
     * the constant's comment. end_interaction() applies any still-pending
     * size/position on button-release, so this never leaves the window
     * visibly lagging behind a stationary pointer. */
    interaction.pending_w = (uint16_t)new_client_w;
    interaction.pending_h = (uint16_t)new_client_h;
    interaction.pending_x = new_frame_x;
    interaction.pending_y = new_frame_y;
    interaction.has_pending_resize = 1;

    int64_t now = anim_now_ms();
    if (now - interaction.last_resize_ms < RESIZE_THROTTLE_MS) return;
    interaction.last_resize_ms = now;
    interaction.has_pending_resize = 0;
    apply_client_resize_geometry(mc, interaction.pending_x, interaction.pending_y,
                                  interaction.pending_w, interaction.pending_h);
}

void client_handle_button_release(xcb_button_release_event_t *ev) {
    (void)ev;
    end_interaction();
}

void client_handle_client_message(xcb_client_message_event_t *ev) {
    if (ev->type == atoms._NET_ACTIVE_WINDOW) {
        managed_client_t *mc = find_by_client(ev->window);
        if (!mc || mc->is_dock) return;
        activate_client(mc);
        return;
    }

    if (ev->type == atoms._NET_CLOSE_WINDOW) {
        managed_client_t *mc = find_by_client(ev->window);
        if (!mc || mc->is_dock) return;
        close_client_gracefully(mc);
        return;
    }

    if (ev->type == atoms._NEKOS_SET_WALLPAPER) {
        char path[1024];
        if (read_root_string_property(atoms._NEKOS_WALLPAPER_PATH, path, sizeof(path))) {
            compositor_set_wallpaper(path);
        }
        return;
    }

    if (ev->type == atoms._NEKOS_ADD_FRAME) {
        char path[1024];
        if (read_root_string_property(atoms._NEKOS_FRAME_PATH, path, sizeof(path))) {
            frame_create(path);
        }
        return;
    }

    if (ev->type == atoms._NET_WM_STATE) {
        managed_client_t *mc = find_by_client(ev->window);
        if (!mc || mc->is_dock) return;

        /* EWMH's generic state-change message: data32[0] is add(1)/remove(0)/
         * toggle(2), data32[1]/[2] are up to two state atoms. Accepted
         * simplification: any request touching these two properties is
         * treated as a toggle regardless of the add/remove/toggle value --
         * nekos-bar (the only sender so far) always sends toggle(2), and
         * add/remove aren't separately honored this pass. */
        xcb_atom_t prop1 = (xcb_atom_t)ev->data.data32[1];
        xcb_atom_t prop2 = (xcb_atom_t)ev->data.data32[2];

        if (prop1 == atoms._NET_WM_STATE_HIDDEN || prop2 == atoms._NET_WM_STATE_HIDDEN) {
            if (mc->minimized) activate_client(mc);
            else minimize_client(mc);
        }
        if (prop1 == atoms._NET_WM_STATE_MAXIMIZED_VERT || prop2 == atoms._NET_WM_STATE_MAXIMIZED_VERT ||
            prop1 == atoms._NET_WM_STATE_MAXIMIZED_HORZ || prop2 == atoms._NET_WM_STATE_MAXIMIZED_HORZ) {
            toggle_maximize(mc);
        }
    }
}

void client_cycle_focus(void) {
    if (client_count == 0) return;

    int start = -1;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].frame == focused_frame) {
            start = i;
            break;
        }
    }

    int idx = start;
    for (int step = 0; step < client_count; step++) {
        idx = (idx + 1) % client_count;
        if (!clients[idx].is_dock && !clients[idx].is_frame && !clients[idx].is_desktop) {
            activate_client(&clients[idx]);
            return;
        }
    }
}
