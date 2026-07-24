/*
 * nekos-wm: window manager + software compositor.
 *
 * This file is just orchestration: connect, grab SubstructureRedirect,
 * bring up EWMH/the compositor, then run the event loop dispatching to
 * client.c (window management policy) and compositor.c (rendering).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <xcb/xcb.h>
#include <xcb/damage.h>
#include <xcb/randr.h>
#include <xcb/xcb_cursor.h>

#include "wm.h"
#include "atoms.h"
#include "ewmh.h"
#include "compositor.h"
#include "client.h"
#include "keys.h"

#define XC_LEFT_PTR 68 /* glyph index into the X server's built-in "cursor" font */

xcb_connection_t *conn;
xcb_screen_t *screen;

/* Sets the root cursor. Windows that don't set their own cursor (true of
 * every frame/window this WM creates so far) inherit from their parent, so
 * setting it once on root covers everything.
 *
 * Preferred path: the session's Xcursor theme (XCURSOR_THEME/XCURSOR_SIZE,
 * exported by start-session.sh) via xcb-util-cursor. The theme+size are also
 * published as Xcursor.* resources on the root RESOURCE_MANAGER first --
 * libxcb-cursor reads resources (not env) for its theme lookup, and toolkit
 * clients that consult resources then agree with the ones reading env.
 * Fallback: the classic arrow from the core "cursor" font compiled into
 * every X server (same mechanism twm uses), for sessions with no theme. */
static void set_default_cursor(void) {
    const char *theme = getenv("XCURSOR_THEME");
    const char *size = getenv("XCURSOR_SIZE");
    if (theme && theme[0]) {
        char res[256];
        int n = snprintf(res, sizeof(res), "Xcursor.theme:\t%s\nXcursor.size:\t%s\n",
                         theme, (size && size[0]) ? size : "24");
        if (n > 0 && n < (int)sizeof(res)) {
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root,
                                 XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING,
                                 8, (uint32_t)n, res);
        }

        xcb_cursor_context_t *ctx;
        if (xcb_cursor_context_new(conn, screen, &ctx) >= 0) {
            xcb_cursor_t cursor = xcb_cursor_load_cursor(ctx, "left_ptr");
            xcb_cursor_context_free(ctx);
            if (cursor != XCB_NONE) {
                uint32_t value = cursor;
                xcb_change_window_attributes(conn, screen->root, XCB_CW_CURSOR, &value);
                xcb_free_cursor(conn, cursor);
                return;
            }
        }
        fprintf(stderr, "nekos-wm: cursor theme %s not loadable, using core arrow\n", theme);
    }

    xcb_font_t font = xcb_generate_id(conn);
    xcb_open_font(conn, font, (uint16_t)strlen("cursor"), "cursor");

    xcb_cursor_t cursor = xcb_generate_id(conn);
    xcb_create_glyph_cursor(conn, cursor, font, font, XC_LEFT_PTR, XC_LEFT_PTR + 1,
                             0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF);

    uint32_t value = cursor;
    xcb_change_window_attributes(conn, screen->root, XCB_CW_CURSOR, &value);

    xcb_free_cursor(conn, cursor);
    xcb_close_font(conn, font);
}

/* Handles a RandR ScreenChangeNotify: the root framebuffer was resized (a VNC
 * viewer requested a new desktop size via x0vncserver, or xrandr was run).
 * Updates the cached screen geometry -- every consumer reads through `screen`,
 * so mutating it in place propagates the new size everywhere -- then reflows
 * the workarea, compositor backbuffer/wallpaper, and screen-anchored windows.
 * Rotation can transpose the reported width/height; nekos-wm never rotates, so
 * the values are taken as-is. Refresh-rate/reflection-only notifications (same
 * dimensions) are ignored. */
static void handle_screen_change(xcb_randr_screen_change_notify_event_t *ev) {
    if (ev->width == screen->width_in_pixels && ev->height == screen->height_in_pixels) return;

    screen->width_in_pixels = ev->width;
    screen->height_in_pixels = ev->height;

    ewmh_recompute_workarea();     /* republish _NET_WORKAREA at the new size */
    compositor_root_resized();     /* rebuild backbuffer + rescale wallpaper */
    client_handle_screen_resize(); /* re-fill maximized windows, re-center frames */

    xcb_flush(conn);
}

int main(void) {
    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-wm: could not connect to X server\n");
        return 1;
    }

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;

    uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                          XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(
        conn, screen->root, XCB_CW_EVENT_MASK, &root_mask);
    xcb_generic_error_t *err = xcb_request_check(conn, cookie);
    if (err) {
        fprintf(stderr, "nekos-wm: another window manager is already running\n");
        free(err);
        xcb_disconnect(conn);
        return 1;
    }

    set_default_cursor();
    atoms_init();
    ewmh_init_supporting_wm();
    ewmh_workarea_init();
    compositor_init();
    keys_init();
    client_adopt_existing_windows();

    /* RandR ScreenChangeNotify is how we learn the VNC viewer resized the
     * desktop (x0vncserver drives the framebuffer resize via RandR). Its event
     * code is assigned dynamically per-connection, like DAMAGE's, so it's
     * checked against randr_event_base in the loop rather than as a switch
     * case. The query_version handshake is required before the server will
     * deliver RandR events. Degrades gracefully (fixed size) if RandR is
     * somehow absent. */
    int have_randr = 0;
    uint8_t randr_event_base = 0;
    const xcb_query_extension_reply_t *randr_ext = xcb_get_extension_data(conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        have_randr = 1;
        randr_event_base = randr_ext->first_event;
        free(xcb_randr_query_version_reply(
            conn, xcb_randr_query_version(conn, XCB_RANDR_MAJOR_VERSION, XCB_RANDR_MINOR_VERSION), NULL));
        xcb_randr_select_input(conn, screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    } else {
        fprintf(stderr, "nekos-wm: RANDR extension unavailable -- desktop resize disabled\n");
    }

    xcb_flush(conn);
    fprintf(stderr, "nekos-wm: managing display, waiting for clients\n");

    int xfd = xcb_get_file_descriptor(conn);

    for (;;) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        /* -1 (block indefinitely) when nothing is animating; otherwise wake
         * up in time for the next animation tick even with no new X event. */
        poll(&pfd, 1, compositor_next_wakeup_ms());

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t response_type = event->response_type & ~0x80;

            /* DAMAGE's event code is assigned dynamically per-connection, so
             * it can't be a switch-case constant -- checked separately here. */
            if (response_type == damage_event_base + XCB_DAMAGE_NOTIFY) {
                compositor_handle_damage_notify((xcb_damage_notify_event_t *)event);
            } else if (have_randr && response_type == randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
                handle_screen_change((xcb_randr_screen_change_notify_event_t *)event);
            } else {
                switch (response_type) {
                case XCB_MAP_REQUEST:
                    client_handle_map_request((xcb_map_request_event_t *)event);
                    break;
                case XCB_CONFIGURE_REQUEST:
                    client_handle_configure_request((xcb_configure_request_event_t *)event);
                    break;
                case XCB_UNMAP_NOTIFY:
                    client_handle_unmap_notify((xcb_unmap_notify_event_t *)event);
                    break;
                case XCB_DESTROY_NOTIFY:
                    client_handle_destroy_notify((xcb_destroy_notify_event_t *)event);
                    break;
                case XCB_MAP_NOTIFY: {
                    /* Override-redirect windows (popup menus, tooltips) bypass
                     * SubstructureRedirect entirely -- client.c's map-request
                     * path never sees them. Catch them here instead so the
                     * compositor actually paints them (see
                     * compositor_manage_override's doc comment for why this
                     * matters). Non-OR MapNotifys (our own frames, regular
                     * client windows) are already handled synchronously
                     * elsewhere and skipped here. */
                    xcb_map_notify_event_t *mn = (xcb_map_notify_event_t *)event;
                    if (mn->override_redirect) {
                        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(
                            conn, xcb_get_geometry(conn, mn->window), NULL);
                        if (g) {
                            compositor_manage_override(mn->window, g->x, g->y,
                                                        g->width, g->height);
                            free(g);
                        }
                    }
                    break;
                }
                case XCB_CONFIGURE_NOTIFY: {
                    /* Self-moving override-redirect windows (nekos-pet is the
                     * one that matters: it repositions itself every tick while
                     * chasing the cursor) configure themselves directly, so no
                     * ConfigureRequest ever reaches client.c -- track the move
                     * here or the compositor keeps painting them at their
                     * mapped-time position. The raise mirrors the pet's own
                     * STACK_MODE_ABOVE into the compositor's paint order
                     * (no-op when already topmost). Frame windows are non-OR
                     * and their drags already call compositor_move directly. */
                    xcb_configure_notify_event_t *cn = (xcb_configure_notify_event_t *)event;
                    if (cn->override_redirect) {
                        compositor_move(cn->window, cn->x, cn->y);
                        compositor_resize(cn->window, cn->width, cn->height);
                        compositor_raise(cn->window);
                    }
                    break;
                }
                case XCB_BUTTON_PRESS:
                    client_handle_button_press((xcb_button_press_event_t *)event);
                    break;
                case XCB_MOTION_NOTIFY:
                    client_handle_motion_notify((xcb_motion_notify_event_t *)event);
                    break;
                case XCB_BUTTON_RELEASE:
                    client_handle_button_release((xcb_button_release_event_t *)event);
                    break;
                case XCB_CLIENT_MESSAGE:
                    client_handle_client_message((xcb_client_message_event_t *)event);
                    break;
                case XCB_LEAVE_NOTIFY:
                    client_handle_leave_notify((xcb_leave_notify_event_t *)event);
                    break;
                case XCB_PROPERTY_NOTIFY:
                    client_handle_property_notify((xcb_property_notify_event_t *)event);
                    break;
                case XCB_KEY_PRESS:
                    keys_handle_key_press((xcb_key_press_event_t *)event);
                    break;
                default:
                    break;
                }
            }

            free(event);
        }

        if (xcb_connection_has_error(conn)) break;

        compositor_paint();

        xcb_window_t finished;
        while ((finished = compositor_take_finished_close()) != XCB_NONE) {
            xcb_destroy_window(conn, finished);
        }
        xcb_flush(conn);
    }

    xcb_disconnect(conn);
    return 0;
}
