#ifndef NEKOS_COMPOSITOR_H
#define NEKOS_COMPOSITOR_H

#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/damage.h>

/* DAMAGE extension's event base, set by compositor_init(). An incoming
 * event's (response_type & ~0x80) equals damage_event_base + XCB_DAMAGE_NOTIFY
 * for DamageNotify events -- extension event codes are assigned dynamically
 * per-connection, so this can't be a switch-case constant. */
extern uint8_t damage_event_base;

/* Queries required extensions (COMPOSITE/DAMAGE/RENDER/XFIXES) and acquires
 * the screen overlay window (with an empty input shape so it doesn't eat
 * input). Exits the process on failure: these extensions were verified
 * present in this Xlibre build during Stage 0, so failure here means a
 * broken environment, not a recoverable runtime condition. */
void compositor_init(void);

/* The root visual's xcb_visualtype_t, resolved once during compositor_init().
 * Exposed so decor.c can create its own cairo_xcb_surface directly against a
 * frame window without duplicating the visual-lookup boilerplate. */
xcb_visualtype_t *compositor_root_visual_type(void);

/* Rebuilds screen-sized state (backbuffer + wallpaper) after a RandR root
 * resize. screen->width_in_pixels/height_in_pixels must already reflect the
 * new size when this is called. */
void compositor_root_resized(void);

/* Redirects `frame` off-screen (manual redirect) and starts damage tracking
 * on it. Call right after xcb_create_window, before mapping. Does not fetch
 * a named pixmap yet -- see compositor_notify_mapped(). */
void compositor_manage(xcb_window_t frame, int16_t x, int16_t y, uint16_t width, uint16_t height);

/* Same as compositor_manage(), but inserts at the bottom of the paint order
 * instead of the top, so `frame` stays visually behind every other managed
 * window -- present and future -- without needing per-focus z-order upkeep.
 * Used exclusively for decorative "frame" image windows (see client.c),
 * which should always sit above the wallpaper but below real app windows. */
void compositor_manage_background(xcb_window_t frame, int16_t x, int16_t y,
                                   uint16_t width, uint16_t height);

/* Fetches the frame's named backing pixmap. Must be called after `frame` is
 * actually mapped: a window's composite offscreen storage is not guaranteed
 * valid until then, so naming the pixmap any earlier yields a dead reference
 * that never shows real content. */
void compositor_notify_mapped(xcb_window_t frame);

/* Moves `frame` to the top of the compositor's own paint order (compositables
 * are painted bottom-to-top in array order). Must be called alongside any
 * xcb_configure_window STACK_MODE_ABOVE on the real X window -- raising a
 * window server-side does not by itself reorder how the compositor draws
 * it, since compositor.c tracks paint order independently. No-op if `frame`
 * is already topmost. */
void compositor_raise(xcb_window_t frame);

/* Updates a resized frame's tracked size and re-fetches its named pixmap.
 * No-op if the size is unchanged. */
void compositor_resize(xcb_window_t frame, uint16_t width, uint16_t height);

/* Updates a moved frame's tracked position. No pixmap re-fetch needed --
 * moving doesn't change a window's own content, just where it's painted. */
void compositor_move(xcb_window_t frame, int16_t x, int16_t y);

/* Stops painting `frame` (e.g. for minimize) while keeping its damage/
 * pixmap/redirect state intact, so it can be cheaply resumed later without
 * redoing xcb_composite_redirect_window/xcb_damage_create/etc. */
void compositor_hide(xcb_window_t frame);

/* Resumes painting a previously-hidden `frame`. Re-fetches its named pixmap
 * (COMPOSITE invalidates it across any unmap->remap cycle, not just
 * resizes) and replays the open animation. */
void compositor_show(xcb_window_t frame);

/* Registers an override-redirect window (popup menus, tooltips, dropdowns --
 * anything that bypasses SubstructureRedirect and so never reaches us via
 * compositor_manage()/client.c's map-request path) so it actually gets
 * painted. Without this it would be mapped and receiving input but visually
 * invisible: the COMPOSITE overlay window sits topmost above *everything* by
 * spec, and only windows registered here get drawn into it. Unlike
 * compositor_manage(), this fetches the pixmap immediately (the window is
 * already mapped by its owning app by the time we observe it) and starts at
 * full opacity/scale with no open animation -- these should appear/disappear
 * instantly, not have an app-window's open/close feel. */
void compositor_manage_override(xcb_window_t window, int16_t x, int16_t y,
                                 uint16_t width, uint16_t height);

/* Unregisters a previously-managed override-redirect window (e.g. once it
 * unmaps). Safe no-op if `window` isn't tracked. Never touches the actual X
 * window -- unlike a normal close, we don't own it and never destroy it. */
void compositor_unmanage_override(xcb_window_t window);

/* Starts `frame`'s minimize (fade+shrink out, same curve as close) animation.
 * When it finishes the compositor hides the frame (compositor_hide semantics)
 * and unmaps the actual X window itself -- the caller must NOT unmap the
 * frame up front, or its named pixmap would be invalidated mid-animation.
 * Restore is unchanged: xcb_map_window + compositor_show as before. */
void compositor_start_minimize(xcb_window_t frame);

/* Starts `frame`'s close (fade+shrink out) animation. The frame keeps being
 * composited (and must NOT be xcb_destroy_window'd by the caller) until the
 * animation actually finishes -- drain compositor_take_finished_close() each
 * main-loop iteration and destroy the X window only for frames it returns.
 * NOTE: does not handle a client remapping the same window mid-close-animation
 * (that edge case is unhandled in this stage -- new clients always get a
 * fresh frame, so it only matters for a window unmap+remapping itself). */
void compositor_start_close(xcb_window_t frame);

/* Returns the frame of a compositable whose close animation just finished
 * (all compositor-side state for it has already been torn down internally;
 * the caller still owns and must xcb_destroy_window the actual X window),
 * or XCB_NONE if none are pending. Call once per main-loop iteration. */
xcb_window_t compositor_take_finished_close(void);

/* Handles a DamageNotify event: subtracts the accumulated damage region
 * (required to keep receiving further notifications for that window) and
 * marks the corresponding compositable dirty. */
void compositor_handle_damage_notify(xcb_damage_notify_event_t *ev);

/* Loads `path` (via image_load_scaled(), cover-fit to screen size) as the
 * desktop wallpaper, replacing any previous one, and persists the choice to
 * ~/.config/nekos/wallpaper so it survives a WM restart (compositor_init()
 * reads this back). Silently does nothing visible if the file can't be
 * loaded (no crash, previous wallpaper -- or the flat-color fallback --
 * stays in place). */
void compositor_set_wallpaper(const char *path);

/* Shows (or moves) a translucent accent-colored rounded rect at (x, y, w, h)
 * in frame coordinates, painted above every window -- the edge-snap preview
 * client.c drives during a titlebar drag near a screen edge. Safe to call
 * every time the target zone changes; a no-op call with the same rect is
 * harmless but unnecessary (skip it if the zone hasn't changed). */
void compositor_set_snap_preview(int16_t x, int16_t y, uint16_t w, uint16_t h);

/* Hides the snap preview (no-op if it wasn't showing). Call when the drag
 * leaves every snap zone, and always at drag end whether or not a snap was
 * actually applied. */
void compositor_clear_snap_preview(void);

/* Composites all managed frames onto the overlay window, if anything is
 * dirty or animating. Cheap no-op otherwise. Internally rate-limited to
 * FRAME_INTERVAL_MS (compositor.c) while anything is actively animating. */
void compositor_paint(void);

/* Milliseconds until the next animation frame is due, or -1 if nothing is
 * animating (safe to block indefinitely on the next X event). */
int compositor_next_wakeup_ms(void);

#endif
