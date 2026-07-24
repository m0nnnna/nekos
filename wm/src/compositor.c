#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/xfixes.h>
#include <xcb/render.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "wm.h"
#include "anim.h"
#include "compositor.h"
#include "image.h"
#include "theme.h"

#define MAX_COMPOSITABLES 64
#define OPEN_ANIM_MS 220
#define CLOSE_ANIM_MS 160
#define OPEN_SCALE_START 0.75  /* opens grow in from 75% with a slight overshoot past 100% */
#define CLOSE_SCALE_END 0.75   /* closes shrink toward 75%, "poof inward" */
#define FRAME_INTERVAL_MS 4    /* ~240fps ceiling while animating -- effectively "as fast
                                 * as the session can actually present," not a real target.
                                 * This used to be 11 (~90fps), deliberately capped low
                                 * because the session ran over Xvnc and every extra frame
                                 * was extra x11vnc re-encode work (see compositor_paint()'s
                                 * old comment, now stale). That bottleneck is gone: the
                                 * session is Xephyr -glamor nested in WSLg (2026-07-23, see
                                 * project memory display-server-and-browser), GPU-rendered
                                 * and forwarded via shared memory, not re-encoded. The real
                                 * pacing now happens downstream (Xephyr/Weston's own vsync),
                                 * so this just needs to not be the bottleneck itself. */

typedef struct {
    xcb_window_t frame;
    xcb_damage_damage_t damage;
    xcb_pixmap_t pixmap;
    cairo_surface_t *surface;
    int16_t x, y;
    uint16_t width, height;
    /* Last rect this compositable was actually painted at, used to compute
     * each frame's dirty region -- both where it is now AND where it used to
     * be need repainting (the vacated area doesn't clear itself). */
    int16_t last_x, last_y;
    uint16_t last_width, last_height;
    int dirty;
    int hidden;
    anim_state_t anim;
    /* Visual the window's named pixmap must be interpreted with. Windows on a
     * 32-bit ARGB visual (nekos-pet) carry per-pixel alpha that the root
     * (24-bit) visual would misread; cairo's OVER operator in compositor_paint
     * then blends it for free. NULL means "use root_visual_type". */
    xcb_visualtype_t *visualtype;
} compositable_t;

uint8_t damage_event_base;

static xcb_window_t overlay;
static xcb_visualtype_t *root_visual_type;
static xcb_pixmap_t backbuffer_pixmap;
static cairo_surface_t *backbuffer_surface;
static xcb_gcontext_t overlay_gc;
static compositable_t compositables[MAX_COMPOSITABLES];
static int compositable_count = 0;
/* Frames whose close animation just finished this paint tick; main.c drains
 * this each loop iteration and is responsible for xcb_destroy_window on each
 * (all compositor-side teardown for them has already happened). */
static xcb_window_t finished_closes[MAX_COMPOSITABLES];
static int finished_close_count = 0;
/* A "structural change" is any add/remove/raise/hide/show that the per-frame,
 * per-compositable dirty scan in compositor_paint() can't infer on its own --
 * most importantly a removed or hidden window's vacated screen rect, which has
 * no live compositable left to report it. Rather than force a whole-screen
 * repaint for these (which makes the VNC server re-encode the entire
 * framebuffer -- the dominant cost once a viewer is attached), each mutator
 * unions just the rect it touched into structural_bbox via mark_structural();
 * compositor_paint() then folds that into the frame's dirty region. structural_
 * change stays set until the next paint consumes it and bypasses the frame-rate
 * cap, so a stale-pixel-clearing repaint is never dropped. structural_full is
 * reserved for the two changes that genuinely touch every pixel -- a wallpaper
 * swap and a root resize -- where scoping to a rect isn't possible. */
static int structural_change = 0;
static int structural_full = 0;
static int32_t structural_x1, structural_y1, structural_x2, structural_y2;
/* Desktop wallpaper, drawn as the backbuffer's base layer instead of the
 * flat-color fallback when set -- see compositor_set_wallpaper(). */
static cairo_surface_t *wallpaper_surface = NULL;
/* Path of the currently-set wallpaper, retained so it can be re-rendered at a
 * new resolution after a root resize (compositor_root_resized()). Empty when
 * no wallpaper is set. */
static char wallpaper_path[1024] = "";

/* Edge-snap preview overlay -- see compositor_set_snap_preview(). */
static int snap_preview_active = 0;
static int16_t snap_preview_x, snap_preview_y;
static uint16_t snap_preview_w, snap_preview_h;

/* Expands the running (x1,y1)-(x2,y2) union bounding box to also cover the rect
 * at (x,y,w,h). int32 so screen-edge int16 coords can't overflow when a width
 * or height is added. */
static void union_rect(int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2,
                       int16_t x, int16_t y, uint16_t w, uint16_t h) {
    if (x < *x1) *x1 = x;
    if (y < *y1) *y1 = y;
    if (x + w > *x2) *x2 = x + w;
    if (y + h > *y2) *y2 = y + h;
}

/* Flags a structural change confined to the given screen rect (see the
 * structural_change comment above). */
static void mark_structural(int16_t x, int16_t y, uint16_t w, uint16_t h) {
    if (!structural_change) { /* first mark since the last paint -- start a fresh bbox */
        structural_x1 = INT32_MAX; structural_y1 = INT32_MAX;
        structural_x2 = INT32_MIN; structural_y2 = INT32_MIN;
    }
    structural_change = 1;
    union_rect(&structural_x1, &structural_y1, &structural_x2, &structural_y2, x, y, w, h);
}

/* Flags a structural change that needs the whole screen repainted (wallpaper
 * swap, root resize) -- the two cases where scoping to a rect isn't possible. */
static void mark_structural_full(void) {
    structural_change = 1;
    structural_full = 1;
}

static xcb_visualtype_t *find_visual(xcb_screen_t *scr, xcb_visualid_t id) {
    xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(scr);
    for (; depth_it.rem; xcb_depth_next(&depth_it)) {
        xcb_visualtype_iterator_t visual_it = xcb_depth_visuals_iterator(depth_it.data);
        for (; visual_it.rem; xcb_visualtype_next(&visual_it)) {
            if (visual_it.data->visual_id == id) return visual_it.data;
        }
    }
    return NULL;
}

static compositable_t *find_compositable(xcb_window_t frame) {
    for (int i = 0; i < compositable_count; i++) {
        if (compositables[i].frame == frame) return &compositables[i];
    }
    return NULL;
}

/* Resolves the visualtype a window was created with, so fetch_pixmap can hand
 * cairo the right pixel interpretation (ARGB windows keep their alpha). Falls
 * back to NULL (-> root visual) if the attributes query fails. */
static xcb_visualtype_t *window_visual_type(xcb_window_t window) {
    xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
        conn, xcb_get_window_attributes(conn, window), NULL);
    if (!attr) return NULL;
    xcb_visualtype_t *vt = find_visual(screen, attr->visual);
    free(attr);
    return vt;
}

static void fetch_pixmap(compositable_t *c) {
    if (c->surface) {
        cairo_surface_destroy(c->surface);
        c->surface = NULL;
    }
    if (c->pixmap != XCB_NONE) {
        xcb_free_pixmap(conn, c->pixmap);
        c->pixmap = XCB_NONE;
    }

    xcb_pixmap_t px = xcb_generate_id(conn);
    xcb_composite_name_window_pixmap(conn, c->frame, px);
    c->pixmap = px;
    c->surface = cairo_xcb_surface_create(conn, px,
                                          c->visualtype ? c->visualtype : root_visual_type,
                                          c->width, c->height);
}

void compositor_init(void) {
    xcb_composite_query_version_cookie_t cc = xcb_composite_query_version(conn, 0, 4);
    xcb_composite_query_version_reply_t *cr = xcb_composite_query_version_reply(conn, cc, NULL);
    if (!cr) {
        fprintf(stderr, "nekos-wm: COMPOSITE extension unavailable\n");
        exit(1);
    }
    free(cr);

    xcb_damage_query_version_cookie_t dc = xcb_damage_query_version(conn, 1, 1);
    xcb_damage_query_version_reply_t *dr = xcb_damage_query_version_reply(conn, dc, NULL);
    if (!dr) {
        fprintf(stderr, "nekos-wm: DAMAGE extension unavailable\n");
        exit(1);
    }
    free(dr);

    const xcb_query_extension_reply_t *damage_ext = xcb_get_extension_data(conn, &xcb_damage_id);
    if (!damage_ext || !damage_ext->present) {
        fprintf(stderr, "nekos-wm: DAMAGE extension unavailable\n");
        exit(1);
    }
    damage_event_base = damage_ext->first_event;

    xcb_xfixes_query_version_cookie_t xc = xcb_xfixes_query_version(conn, 5, 0);
    xcb_xfixes_query_version_reply_t *xr = xcb_xfixes_query_version_reply(conn, xc, NULL);
    if (!xr) {
        fprintf(stderr, "nekos-wm: XFIXES extension unavailable\n");
        exit(1);
    }
    free(xr);

    const xcb_query_extension_reply_t *render_ext = xcb_get_extension_data(conn, &xcb_render_id);
    if (!render_ext || !render_ext->present) {
        fprintf(stderr, "nekos-wm: RENDER extension unavailable\n");
        exit(1);
    }

    root_visual_type = find_visual(screen, screen->root_visual);
    if (!root_visual_type) {
        fprintf(stderr, "nekos-wm: could not find root visual type\n");
        exit(1);
    }

    xcb_composite_get_overlay_window_cookie_t oc = xcb_composite_get_overlay_window(conn, screen->root);
    xcb_composite_get_overlay_window_reply_t *ov = xcb_composite_get_overlay_window_reply(conn, oc, NULL);
    if (!ov) {
        fprintf(stderr, "nekos-wm: could not acquire overlay window\n");
        exit(1);
    }
    overlay = ov->overlay_win;
    free(ov);

    /* Without this, the overlay (topmost, covering the whole screen) silently
     * swallows all mouse/keyboard input meant for real clients. */
    xcb_xfixes_region_t empty_region = xcb_generate_id(conn);
    xcb_xfixes_create_region(conn, empty_region, 0, NULL);
    xcb_xfixes_set_window_shape_region(conn, overlay, XCB_SHAPE_SK_INPUT, 0, 0, empty_region);
    xcb_xfixes_destroy_region(conn, empty_region);

    xcb_pixmap_t bb = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, bb, screen->root,
                       screen->width_in_pixels, screen->height_in_pixels);
    backbuffer_pixmap = bb;
    backbuffer_surface = cairo_xcb_surface_create(conn, bb, root_visual_type,
                                                   screen->width_in_pixels, screen->height_in_pixels);

    overlay_gc = xcb_generate_id(conn);
    xcb_create_gc(conn, overlay_gc, overlay, 0, NULL);

    xcb_flush(conn);
    fprintf(stderr, "nekos-wm: compositor initialized (overlay=0x%x)\n", overlay);

    /* Restore the last-set wallpaper, if any -- see compositor_set_wallpaper(). */
    const char *home = getenv("HOME");
    if (home) {
        char config_path[512];
        snprintf(config_path, sizeof(config_path), "%s/.config/nekos/wallpaper", home);
        FILE *f = fopen(config_path, "r");
        if (f) {
            char path[1024];
            if (fgets(path, sizeof(path), f)) {
                size_t len = strlen(path);
                while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r')) path[--len] = '\0';
                if (len > 0) compositor_set_wallpaper(path);
            }
            fclose(f);
        }
    }
}

xcb_visualtype_t *compositor_root_visual_type(void) {
    return root_visual_type;
}

/* Rebuilds the screen-sized rendering state after the root framebuffer is
 * resized (RandR): the backbuffer pixmap/surface are recreated at the new
 * dimensions and the wallpaper (if any) is re-rendered cover-scaled to fit.
 * The COMPOSITE overlay window is resized by the server to track the root, so
 * it isn't touched here. screen->width_in_pixels/height_in_pixels must already
 * hold the new size. */
void compositor_root_resized(void) {
    uint16_t w = screen->width_in_pixels;
    uint16_t h = screen->height_in_pixels;

    if (backbuffer_surface) {
        cairo_surface_destroy(backbuffer_surface);
        backbuffer_surface = NULL;
    }
    if (backbuffer_pixmap != XCB_NONE) {
        xcb_free_pixmap(conn, backbuffer_pixmap);
    }

    xcb_pixmap_t bb = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, bb, screen->root, w, h);
    backbuffer_pixmap = bb;
    backbuffer_surface = cairo_xcb_surface_create(conn, bb, root_visual_type, w, h);

    if (wallpaper_surface && wallpaper_path[0]) {
        cairo_surface_t *rescaled = image_load_scaled(wallpaper_path, w, h, /*cover=*/1);
        if (rescaled) {
            cairo_surface_destroy(wallpaper_surface);
            wallpaper_surface = rescaled;
        }
    }

    mark_structural_full(); /* whole framebuffer changed size -- repaint it all */
    xcb_flush(conn);
}

/* Shared by compositor_manage() and compositor_manage_background(): redirects
 * `frame`, starts damage tracking, and inserts a new compositable either at
 * the top of paint order (at_front=0, the normal case) or just above index 0
 * (at_front=1, so it always paints behind everything else added since --
 * used for decorative background windows). Index 0 itself is never claimed
 * by at_front: nekos-desktop (the wallpaper+icons window) is a real,
 * fullscreen, opaque compositable that lands at index 0 as the first thing
 * registered at session start, and an at_front insert that displaced it to
 * index 1 would put the desktop's opaque repaint ABOVE the background window
 * in paint order, permanently hiding it (nekos#-frames-invisible). Inserting
 * at index 1 instead keeps whatever is genuinely bottommost (the desktop)
 * there, landing background windows just above it -- still behind every
 * ordinary app window, which is all this ever needed to guarantee. Returns
 * NULL (compositable limit reached) without touching X state beyond what was
 * already done -- callers just propagate that as a no-op, matching the
 * previous behavior. */
static compositable_t *insert_compositable(xcb_window_t frame, int16_t x, int16_t y,
                                            uint16_t width, uint16_t height, int at_front) {
    if (compositable_count >= MAX_COMPOSITABLES) {
        fprintf(stderr, "nekos-wm: compositable limit reached\n");
        return NULL;
    }

    xcb_composite_redirect_window(conn, frame, XCB_COMPOSITE_REDIRECT_MANUAL);

    xcb_damage_damage_t damage = xcb_generate_id(conn);
    xcb_damage_create(conn, damage, frame, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);

    compositable_t *c;
    if (at_front) {
        int pos = compositable_count >= 1 ? 1 : 0;
        for (int i = compositable_count; i > pos; i--) {
            compositables[i] = compositables[i - 1];
        }
        c = &compositables[pos];
    } else {
        c = &compositables[compositable_count];
    }
    compositable_count++;

    memset(c, 0, sizeof(*c));
    c->frame = frame;
    c->damage = damage;
    c->x = x;
    c->y = y;
    c->width = width;
    c->height = height;
    /* No prior on-screen rect to vacate -- start last==current so the first
     * paint's dirty-region calc doesn't manufacture a bogus extra rect. */
    c->last_x = x;
    c->last_y = y;
    c->last_width = width;
    c->last_height = height;
    c->pixmap = XCB_NONE;
    c->surface = NULL;
    c->dirty = 1;
    c->visualtype = window_visual_type(frame);
    mark_structural(x, y, width, height);
    return c;
}

void compositor_manage(xcb_window_t frame, int16_t x, int16_t y, uint16_t width, uint16_t height) {
    insert_compositable(frame, x, y, width, height, 0);
}

void compositor_manage_background(xcb_window_t frame, int16_t x, int16_t y, uint16_t width, uint16_t height) {
    insert_compositable(frame, x, y, width, height, 1);
}

void compositor_manage_override(xcb_window_t window, int16_t x, int16_t y,
                                 uint16_t width, uint16_t height) {
    /* Never composite our own overlay window. It's override-redirect, so the
     * WM's MapNotify path routes it here -- but tracking it means every
     * xcb_copy_area we do TO the overlay each frame damages it, marking it
     * dirty and forcing the *next* frame to repaint full-screen. During any
     * continuous activity (drags, animations) that feedback loop makes every
     * frame full-screen, defeating dirty-region tracking and swamping the VNC
     * encoder. This is the root cause of the drag-outline lag (nekos#2) and
     * choppy animations (nekos#10). */
    if (window == overlay) return;

    if (compositable_count >= MAX_COMPOSITABLES) {
        fprintf(stderr, "nekos-wm: compositable limit reached\n");
        return;
    }

    xcb_composite_redirect_window(conn, window, XCB_COMPOSITE_REDIRECT_MANUAL);

    xcb_damage_damage_t damage = xcb_generate_id(conn);
    xcb_damage_create(conn, damage, window, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);

    compositable_t *c = &compositables[compositable_count++];
    memset(c, 0, sizeof(*c));
    c->frame = window;
    c->damage = damage;
    c->x = x;
    c->y = y;
    c->width = width;
    c->height = height;
    c->last_x = x;
    c->last_y = y;
    c->last_width = width;
    c->last_height = height;
    c->pixmap = XCB_NONE;
    c->surface = NULL;
    c->dirty = 1;
    c->visualtype = window_visual_type(window);
    /* anim.kind stays ANIM_NONE (memset above) -- compositor_paint()'s switch
     * already defaults that to alpha=1.0/scale=1.0, so this appears at full
     * opacity on the very next paint instead of fading/scaling in. */
    mark_structural(x, y, width, height);

    /* Deliberately NOT fetching the pixmap here even though the window is
     * already mapped -- empirically, naming it this early (before the owning
     * app has actually drawn anything real post-map) yields a pixmap that
     * never reflects later drawing, leaving the compositable permanently
     * blank. compositor_handle_damage_notify() fetches it lazily on first
     * real damage instead, which is provably after real content exists. */
}

void compositor_notify_mapped(xcb_window_t frame) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    fetch_pixmap(c);
    anim_start_open(&c->anim, OPEN_ANIM_MS);
    c->dirty = 1;
}

void compositor_raise(xcb_window_t frame) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;

    int idx = (int)(c - compositables);
    if (idx == compositable_count - 1) return; /* already topmost */

    compositable_t tmp = *c;
    for (int i = idx; i < compositable_count - 1; i++) {
        compositables[i] = compositables[i + 1];
    }
    compositables[compositable_count - 1] = tmp;
    /* Only the raised window's own rect restacks (we only ever raise, never
     * lower, so nothing is uncovered elsewhere) -- repaint just that region in
     * the new order rather than the whole screen. */
    mark_structural(tmp.x, tmp.y, tmp.width, tmp.height);
}

void compositor_move(xcb_window_t frame, int16_t x, int16_t y) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    if (c->x == x && c->y == y) return;

    c->x = x;
    c->y = y;
    c->dirty = 1;
}

void compositor_resize(xcb_window_t frame, uint16_t width, uint16_t height) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    if (c->width == width && c->height == height) return;

    c->width = width;
    c->height = height;
    fetch_pixmap(c);
    c->dirty = 1; /* dirty alone is enough now -- see compositor_paint's throttle gate */
}

/* Tears down all compositor-side state for `c` (damage, redirect, pixmap,
 * surface) and removes it from tracking. Does NOT touch the X frame window
 * itself -- callers into this file only reach it once they've already
 * decided the frame is done for (either it was never fully set up, or its
 * close animation just finished). */
static void teardown_compositable(compositable_t *c) {
    xcb_damage_destroy(conn, c->damage);
    xcb_composite_unredirect_window(conn, c->frame, XCB_COMPOSITE_REDIRECT_MANUAL);
    if (c->surface) cairo_surface_destroy(c->surface);
    if (c->pixmap != XCB_NONE) xcb_free_pixmap(conn, c->pixmap);

    /* Once removed there's no live compositable to report its footprint, so
     * union its vacated rect (both where it currently sits and where it was
     * last actually painted) now, while c is still valid, so the next paint
     * clears the stale pixels. */
    mark_structural(c->x, c->y, c->width, c->height);
    mark_structural(c->last_x, c->last_y, c->last_width, c->last_height);

    int idx = (int)(c - compositables);
    compositables[idx] = compositables[compositable_count - 1];
    compositable_count--;
}

void compositor_unmanage_override(xcb_window_t window) {
    compositable_t *c = find_compositable(window);
    if (!c) return;
    teardown_compositable(c);
}

void compositor_hide(xcb_window_t frame) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    c->hidden = 1;
    /* Clear the pixels it last occupied (union current + last-painted rect) --
     * once hidden the paint loop skips it, so nothing else would report them. */
    mark_structural(c->x, c->y, c->width, c->height);
    mark_structural(c->last_x, c->last_y, c->last_width, c->last_height);
}

void compositor_show(xcb_window_t frame) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    c->hidden = 0;
    fetch_pixmap(c); /* COMPOSITE invalidates the named pixmap across unmap->remap, same as on first map/resize */
    anim_start_open(&c->anim, OPEN_ANIM_MS);
    c->dirty = 1;
    mark_structural(c->x, c->y, c->width, c->height);
}

void compositor_start_minimize(xcb_window_t frame) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    anim_start_minimize(&c->anim, CLOSE_ANIM_MS);
    c->dirty = 1;
}

void compositor_start_close(xcb_window_t frame) {
    compositable_t *c = find_compositable(frame);
    if (!c) return;
    /* If `frame` was hidden (minimized), it must still be forced through the
     * paint loop to actually advance and complete its close animation --
     * otherwise compositor_paint()'s `if (c->hidden) continue;` would skip
     * it forever, the animation would never reach t>=1.0, and the frame
     * would leak: never torn down, never reaching finished_closes, never
     * xcb_destroy_window'd. */
    c->hidden = 0;
    anim_start_close(&c->anim, CLOSE_ANIM_MS);
    c->dirty = 1;
}

xcb_window_t compositor_take_finished_close(void) {
    if (finished_close_count == 0) return XCB_NONE;
    xcb_window_t frame = finished_closes[0];
    for (int i = 1; i < finished_close_count; i++) finished_closes[i - 1] = finished_closes[i];
    finished_close_count--;
    return frame;
}

void compositor_handle_damage_notify(xcb_damage_notify_event_t *ev) {
    xcb_damage_subtract(conn, ev->damage, XCB_NONE, XCB_NONE);

    compositable_t *c = find_compositable(ev->drawable);
    if (!c) return;
    if (!c->surface) fetch_pixmap(c);
    c->dirty = 1;
}

void compositor_set_wallpaper(const char *path) {
    cairo_surface_t *surface = image_load_scaled(
        path, screen->width_in_pixels, screen->height_in_pixels, /*cover=*/1);
    if (!surface) {
        fprintf(stderr, "nekos-wm: could not load wallpaper %s\n", path);
        return;
    }

    if (wallpaper_surface) cairo_surface_destroy(wallpaper_surface);
    wallpaper_surface = surface;
    snprintf(wallpaper_path, sizeof(wallpaper_path), "%s", path); /* remembered for re-render on resize */
    mark_structural_full(); /* the whole base layer changed -- repaint everything */

    const char *home = getenv("HOME");
    if (home) {
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s/.config", home);
        (void)mkdir(dir_path, 0755);
        snprintf(dir_path, sizeof(dir_path), "%s/.config/nekos", home);
        (void)mkdir(dir_path, 0755);

        char config_path[512];
        snprintf(config_path, sizeof(config_path), "%s/.config/nekos/wallpaper", home);
        FILE *f = fopen(config_path, "w");
        if (f) {
            fputs(path, f);
            fclose(f);
        }
    }
}

/* mark_structural() with whatever rect the preview currently occupies (if
 * any), so the vacated area repaints instead of leaving a ghost -- same
 * "clear where it used to be, draw where it is now" pattern compositables
 * use via last_x/last_y. */
static void mark_snap_preview_dirty(void) {
    if (snap_preview_active) {
        mark_structural(snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h);
    }
}

void compositor_set_snap_preview(int16_t x, int16_t y, uint16_t w, uint16_t h) {
    mark_snap_preview_dirty(); /* clear the old spot */
    snap_preview_active = 1;
    snap_preview_x = x;
    snap_preview_y = y;
    snap_preview_w = w;
    snap_preview_h = h;
    mark_structural(x, y, w, h); /* draw the new one */
}

void compositor_clear_snap_preview(void) {
    if (!snap_preview_active) return;
    mark_snap_preview_dirty();
    snap_preview_active = 0;
}

static int64_t last_paint_ms = 0;

void compositor_paint(void) {
    int64_t now = anim_now_ms();
    int any_dirty = 0;
    int any_animating = 0;
    for (int i = 0; i < compositable_count; i++) {
        if (compositables[i].dirty) any_dirty = 1;
        if (anim_active(&compositables[i].anim)) any_animating = 1;
    }
    if (!any_dirty && !any_animating && !structural_change) return;

    /* Cap repaint rate per FRAME_INTERVAL_MS -- see its comment. cairo-xcb
     * just issues protocol requests (CopyArea/RENDER Composite); the pixel
     * work happens in Xephyr's glamor backend, GPU-accelerated. There's no
     * downstream re-encode step anymore to protect (that was the whole
     * reason this cap used to matter), so it's set high enough to be a
     * safety ceiling rather than an actual throttle. structural_change
     * (a window was just added/removed/hidden/shown/raised) is the one case
     * that bypasses the cap: it's a rare one-shot event, and throttling it
     * risks dropping the single repaint that clears a removed window's stale
     * pixels. */
    int was_structural_change = structural_change;
    int was_structural_full = structural_full;
    if (!structural_change && (now - last_paint_ms) < FRAME_INTERVAL_MS) return;
    last_paint_ms = now;
    structural_change = 0;
    structural_full = 0;

    /* Dirty-region tracking: X's DAMAGE extension (which x11vnc uses to know
     * what changed) tracks WRITES, not whether pixel values actually
     * differ -- a full-screen xcb_copy_area marks the whole screen "changed"
     * even when 99% of the copied pixels are identical to before, forcing
     * x11vnc to re-scan/re-encode the entire framebuffer on every repaint.
     * This was the actual root cause of a "transparent outline" artifact
     * during fast window drags: full-screen copies far outpaced what x11vnc
     * could diff and encode. Only touching (and only copying to the overlay)
     * the union of what's actually dirty this frame keeps our own writes --
     * and therefore what x11vnc thinks changed -- scoped to reality. */
    int32_t dx1 = INT32_MAX, dy1 = INT32_MAX, dx2 = INT32_MIN, dy2 = INT32_MIN;
    for (int i = 0; i < compositable_count; i++) {
        compositable_t *c = &compositables[i];
        if (!c->dirty && !anim_active(&c->anim)) continue;
        union_rect(&dx1, &dy1, &dx2, &dy2, c->x, c->y, c->width, c->height);
        union_rect(&dx1, &dy1, &dx2, &dy2, c->last_x, c->last_y, c->last_width, c->last_height);
        c->last_x = c->x;
        c->last_y = c->y;
        c->last_width = c->width;
        c->last_height = c->height;
    }
    /* Fold in the region affected by structural changes this cycle. Scoped ones
     * (add/remove/raise/hide/show) contributed a bounding box via
     * mark_structural(); only a wallpaper swap or root resize needs the whole
     * screen. This is what keeps a menu/dialog open-and-close, or a focus-driven
     * raise, from re-encoding the entire framebuffer over the VNC link. */
    if (was_structural_full) {
        dx1 = 0;
        dy1 = 0;
        dx2 = screen->width_in_pixels;
        dy2 = screen->height_in_pixels;
    } else if (was_structural_change && structural_x1 <= structural_x2) {
        if (structural_x1 < dx1) dx1 = structural_x1;
        if (structural_y1 < dy1) dy1 = structural_y1;
        if (structural_x2 > dx2) dx2 = structural_x2;
        if (structural_y2 > dy2) dy2 = structural_y2;
    }
    if (dx1 >= dx2 || dy1 >= dy2) return; /* nothing actually dirty after all */

    if (dx1 < 0) dx1 = 0;
    if (dy1 < 0) dy1 = 0;
    if (dx2 > screen->width_in_pixels) dx2 = screen->width_in_pixels;
    if (dy2 > screen->height_in_pixels) dy2 = screen->height_in_pixels;

    cairo_t *cr = cairo_create(backbuffer_surface);
    cairo_rectangle(cr, dx1, dy1, dx2 - dx1, dy2 - dy1);
    cairo_clip(cr);
    if (wallpaper_surface) {
        cairo_set_source_surface(cr, wallpaper_surface, 0, 0);
        cairo_paint(cr);
    } else {
        theme_rgb(cr, THEME_BG);
        cairo_paint(cr);
    }

    for (int i = 0; i < compositable_count; i++) {
        compositable_t *c = &compositables[i];
        c->dirty = 0;
        if (c->hidden) continue;
        if (!c->surface) continue;

        double t = anim_progress(&c->anim, now);
        double alpha = 1.0;
        double scale = 1.0;
        int close_finished = 0;

        switch (c->anim.kind) {
        case ANIM_OPENING:
            alpha = ease_out_cubic(t);
            scale = OPEN_SCALE_START + (1.0 - OPEN_SCALE_START) * ease_out_back(t);
            if (t >= 1.0) c->anim.kind = ANIM_NONE; /* settle to steady state */
            break;
        case ANIM_CLOSING:
        case ANIM_MINIMIZING:
            alpha = 1.0 - ease_in_cubic(t);
            scale = 1.0 - (1.0 - CLOSE_SCALE_END) * ease_in_cubic(t);
            if (c->anim.kind == ANIM_CLOSING) {
                close_finished = (t >= 1.0);
            } else if (t >= 1.0) {
                /* Minimize finished: hide (keeping all compositor state, see
                 * compositor_hide) and unmap the real X window now -- doing
                 * the unmap only after the animation keeps the named pixmap
                 * valid while it's still being drawn. client.c's unmap
                 * handler ignores frame unmaps (find_by_frame guard). */
                c->anim.kind = ANIM_NONE;
                c->hidden = 1;
                /* Now hidden -- next paint must clear its footprint (this frame
                 * already painted it at its final tiny scale). */
                mark_structural(c->x, c->y, c->width, c->height);
                xcb_unmap_window(conn, c->frame);
            }
            break;
        case ANIM_NONE:
        default:
            break;
        }
        if (alpha < 0.0) alpha = 0.0;
        if (alpha > 1.0) alpha = 1.0;

        cairo_save(cr);
        if (scale != 1.0) {
            double cx = c->x + c->width / 2.0;
            double cy = c->y + c->height / 2.0;
            cairo_translate(cr, cx, cy);
            cairo_scale(cr, scale, scale);
            cairo_translate(cr, -cx, -cy);
        }
        cairo_set_source_surface(cr, c->surface, c->x, c->y);
        cairo_paint_with_alpha(cr, alpha);
        cairo_restore(cr);

        if (close_finished) {
            /* Frame is done animating out -- tear down compositor state now
             * and hand the frame to main.c (via the queue below) to actually
             * destroy the X window. Removing the current index by swapping
             * the last element in means the swapped-in element must be
             * re-visited, hence the i-- before continue. */
            if (finished_close_count < MAX_COMPOSITABLES) {
                finished_closes[finished_close_count++] = c->frame;
            }
            teardown_compositable(c);
            i--;
        }
    }

    /* Edge-snap preview, drawn last so it's above every window -- matches
     * the way a real drag-to-snap gesture previews on top of everything. */
    if (snap_preview_active) {
        cairo_save(cr);
        theme_rgba(cr, THEME_ACCENT, 0.22);
        theme_rounded_rect(cr, snap_preview_x, snap_preview_y, snap_preview_w, snap_preview_h,
                            THEME_RADIUS * 2);
        cairo_fill(cr);
        theme_rgba(cr, THEME_ACCENT, 0.6);
        cairo_set_line_width(cr, 2.0);
        theme_rounded_rect(cr, snap_preview_x + 1.0, snap_preview_y + 1.0,
                            snap_preview_w - 2.0, snap_preview_h - 2.0, THEME_RADIUS * 2);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(backbuffer_surface);

    xcb_copy_area(conn, backbuffer_pixmap, overlay, overlay_gc, dx1, dy1, dx1, dy1,
                  (uint16_t)(dx2 - dx1), (uint16_t)(dy2 - dy1));
    xcb_flush(conn);
}

int compositor_next_wakeup_ms(void) {
    for (int i = 0; i < compositable_count; i++) {
        if (anim_active(&compositables[i].anim)) return FRAME_INTERVAL_MS;
    }
    return -1;
}
