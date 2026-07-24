#ifndef NEKOS_IMAGE_H
#define NEKOS_IMAGE_H

#include <cairo.h>

/* Loads `path` via imlib2, decoded/scaled into a new CAIRO_FORMAT_ARGB32
 * surface sized target_w x target_h. If `cover`, scales to fill exactly that
 * size (aspect-preserving, cropping overflow -- for wallpaper). If not,
 * scales to fit *within* that size preserving aspect (for frames), so the
 * returned surface's actual dimensions may be smaller than target_w/target_h
 * in one axis -- check cairo_image_surface_get_width/height on the result.
 * Caller owns the returned surface (cairo_surface_destroy). Returns NULL on
 * a missing/corrupt/unreadable file -- never a partial surface. */
cairo_surface_t *image_load_scaled(const char *path, int target_w, int target_h, int cover);

#endif
