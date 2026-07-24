#include <stdint.h>

#include <Imlib2.h>

#include "image.h"

cairo_surface_t *image_load_scaled(const char *path, int target_w, int target_h, int cover) {
    Imlib_Image orig = imlib_load_image(path);
    if (!orig) return NULL;

    imlib_context_set_image(orig);
    int orig_w = imlib_image_get_width();
    int orig_h = imlib_image_get_height();
    if (orig_w <= 0 || orig_h <= 0 || target_w <= 0 || target_h <= 0) {
        imlib_free_image();
        return NULL;
    }

    int crop_x, crop_y, crop_w, crop_h, dst_w, dst_h;
    if (cover) {
        /* Scale to fill target_w x target_h exactly, cropping whichever axis
         * overflows -- computed as a source-rect crop before scaling so we
         * never create an oversized intermediate. */
        double target_aspect = (double)target_w / target_h;
        double orig_aspect = (double)orig_w / orig_h;
        if (orig_aspect > target_aspect) {
            crop_h = orig_h;
            crop_w = (int)(orig_h * target_aspect + 0.5);
            crop_y = 0;
            crop_x = (orig_w - crop_w) / 2;
        } else {
            crop_w = orig_w;
            crop_h = (int)(orig_w / target_aspect + 0.5);
            crop_x = 0;
            crop_y = (orig_h - crop_h) / 2;
        }
        dst_w = target_w;
        dst_h = target_h;
    } else {
        /* Fit within target_w x target_h, preserving aspect -- no crop. */
        double scale = (double)target_w / orig_w;
        double scale_h = (double)target_h / orig_h;
        if (scale_h < scale) scale = scale_h;
        crop_x = 0;
        crop_y = 0;
        crop_w = orig_w;
        crop_h = orig_h;
        dst_w = (int)(orig_w * scale + 0.5);
        dst_h = (int)(orig_h * scale + 0.5);
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;
    }

    Imlib_Image scaled = imlib_create_cropped_scaled_image(crop_x, crop_y, crop_w, crop_h, dst_w, dst_h);
    imlib_free_image(); /* done with orig */
    if (!scaled) return NULL;

    imlib_context_set_image(scaled);
    DATA32 *pixels = imlib_image_get_data_for_reading_only();
    if (!pixels) {
        imlib_free_image();
        return NULL;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dst_w, dst_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        imlib_free_image();
        return NULL;
    }

    unsigned char *dst = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < dst_h; y++) {
        uint32_t *dst_row = (uint32_t *)(dst + (size_t)y * stride);
        DATA32 *src_row = pixels + (size_t)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            DATA32 px = src_row[x];
            uint32_t a = (px >> 24) & 0xFF;
            uint32_t r = (px >> 16) & 0xFF;
            uint32_t g = (px >> 8) & 0xFF;
            uint32_t b = px & 0xFF;
            /* Cairo's ARGB32 expects premultiplied alpha; Imlib2's data is
             * straight (unassociated) alpha -- premultiply here or any
             * partially-transparent image (e.g. a PNG frame with a
             * transparent background) would blend incorrectly. */
            r = r * a / 255;
            g = g * a / 255;
            b = b * a / 255;
            dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surface);

    imlib_free_image(); /* done with scaled */
    return surface;
}
