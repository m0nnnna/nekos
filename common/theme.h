/*
 * theme.h: the one nekOS look. Every component (wm decor, bar, menu,
 * launcher, notify, desktop, files, settings, software) includes this instead
 * of hardcoding its own copy of the palette/fonts/paddings.
 *
 * Header-only on purpose: colors are macros and the drawing helpers are small
 * static inlines, so there's no extra object file to thread through the
 * per-directory Makefiles -- just add -I../common (or -I../../common from
 * wm/src) and include.
 */

#ifndef NEKOS_THEME_H
#define NEKOS_THEME_H

#include <string.h>
#include <cairo.h>

/* ---- palette (0xRRGGBB) -------------------------------------------------- */

/* Panel/titlebar vertical gradient (focused), shared by the bar, menus,
 * toasts, and window titlebars. */
#define THEME_PANEL_TOP        0x2B2438
#define THEME_PANEL_BOT        0x1B1725
/* Unfocused titlebar variant. */
#define THEME_PANEL_TOP_DIM    0x211D2A
#define THEME_PANEL_BOT_DIM    0x17141F

#define THEME_ACCENT           0xFF8FB1  /* the nekOS pink */
#define THEME_FG               0xEDEAF5  /* primary text */
#define THEME_BG               0x15121C  /* flat app/window background */
#define THEME_MUTED            0x4A4459  /* unfocused/disabled foreground */
#define THEME_BORDER_DIM       0x3A3348  /* unfocused window border */

#define THEME_CLOSE            0xFF6B7A  /* also "danger" (remove, shutdown) */
#define THEME_MAXIMIZE         0x8FE3B0  /* also "ok/success" */
#define THEME_MINIMIZE         0xFFD98F  /* also "warning/busy" */

/* Category-icon glyph palette (files + desktop). */
#define THEME_ICON_FOLDER      0xE8C46A
#define THEME_ICON_IMAGE_BG    0x6AA0E8
#define THEME_ICON_IMAGE_SUN   0xFFE08A
#define THEME_ICON_IMAGE_HILL  0x2E7D4F
#define THEME_ICON_EXEC_BG     0x24202E
#define THEME_ICON_EXEC_FG     0x7CE38B
#define THEME_ICON_PAGE        0xEDEAF5
#define THEME_ICON_HTML        0xC85A9A
#define THEME_ICON_TEXT_LINES  0x9A93A8

/* ---- type scale ---------------------------------------------------------- */

#define THEME_FONT             "sans-serif"
#define THEME_FONT_XS          10.0  /* clock date, fine print */
#define THEME_FONT_SM          12.0  /* bar, menus, labels, list rows */
#define THEME_FONT_MD          13.0  /* app list rows, headers */
#define THEME_FONT_LG          14.0  /* launcher rows, toast titles */
#define THEME_FONT_XL          15.0  /* launcher search box */

/* ---- spacing / shared metrics -------------------------------------------- */

#define THEME_PAD              10.0  /* standard left text inset */
#define THEME_PAD_LG           16.0
#define THEME_ROW_H            30    /* standard list row height */
#define THEME_BORDER_ALPHA     0.4   /* the one popup/window border alpha */
#define THEME_HOVER_ALPHA      0.10  /* accent fill under a hovered row/button */
#define THEME_SELECT_ALPHA     0.20  /* accent fill under a selected row */
#define THEME_RADIUS           5.0   /* rounded corner radius for pills/selections */

/* ---- helpers ------------------------------------------------------------- */

static inline void theme_rgb(cairo_t *cr, unsigned hex) {
    cairo_set_source_rgb(cr, ((hex >> 16) & 0xFF) / 255.0,
                          ((hex >> 8) & 0xFF) / 255.0, (hex & 0xFF) / 255.0);
}

static inline void theme_rgba(cairo_t *cr, unsigned hex, double alpha) {
    cairo_set_source_rgba(cr, ((hex >> 16) & 0xFF) / 255.0,
                           ((hex >> 8) & 0xFF) / 255.0, (hex & 0xFF) / 255.0, alpha);
}

static inline void theme_font(cairo_t *cr, double size, int bold) {
    cairo_select_font_face(cr, THEME_FONT, CAIRO_FONT_SLANT_NORMAL,
                            bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

/* Fills the rect with the shared panel gradient (titlebar/bar/menu/toast). */
static inline void theme_panel_gradient(cairo_t *cr, double x, double y,
                                         double w, double h, int focused) {
    unsigned top = focused ? THEME_PANEL_TOP : THEME_PANEL_TOP_DIM;
    unsigned bot = focused ? THEME_PANEL_BOT : THEME_PANEL_BOT_DIM;
    cairo_pattern_t *grad = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, ((top >> 16) & 0xFF) / 255.0,
                                      ((top >> 8) & 0xFF) / 255.0, (top & 0xFF) / 255.0);
    cairo_pattern_add_color_stop_rgb(grad, 1.0, ((bot >> 16) & 0xFF) / 255.0,
                                      ((bot >> 8) & 0xFF) / 255.0, (bot & 0xFF) / 255.0);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, grad);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);
}

static inline void theme_rounded_rect(cairo_t *cr, double x, double y,
                                       double w, double h, double r) {
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -1.5707963, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, 1.5707963);
    cairo_arc(cr, x + r, y + h - r, r, 1.5707963, 3.1415926);
    cairo_arc(cr, x + r, y + r, r, 3.1415926, 4.7123889);
    cairo_close_path(cr);
}

/* Baseline y that vertically centers text of `size` in a row starting at
 * `row_y` with height `row_h` (toy-API approximation: cap height ~ 0.7em). */
static inline double theme_baseline(double row_y, double row_h, double size) {
    return row_y + (row_h + size * 0.7) / 2.0;
}

/* Draws `text` at (x, baseline y), truncating with "…" if it would exceed
 * max_w. Replaces the old clip-and-hard-cut pattern. Uses the current font;
 * safe with UTF-8 (never splits a multibyte sequence). */
static inline void theme_text_ellipsized(cairo_t *cr, double x, double y,
                                          double max_w, const char *text) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    cairo_move_to(cr, x, y);
    if (ext.x_advance <= max_w) {
        cairo_show_text(cr, text);
        return;
    }

    char buf[512];
    size_t len = strlen(text);
    if (len > sizeof(buf) - 4) len = sizeof(buf) - 4;
    while (len > 0) {
        /* Back off to a UTF-8 boundary, then drop one full character. */
        while (len > 0 && ((unsigned char)text[len - 1] & 0xC0) == 0x80) len--;
        if (len > 0) len--;
        while (len > 0 && ((unsigned char)text[len - 1] & 0xC0) == 0x80) len--;

        memcpy(buf, text, len);
        buf[len] = '\0';
        strcat(buf, "\xE2\x80\xA6"); /* U+2026 ellipsis */
        cairo_text_extents(cr, buf, &ext);
        if (ext.x_advance <= max_w) break;
    }
    cairo_show_text(cr, buf);
}

#endif
