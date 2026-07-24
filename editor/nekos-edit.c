/*
 * nekos-edit: nekOS's plain-text editor.
 *
 * A normal (non-dock) window -- gets nekos-wm's titlebar/close/move for free,
 * same convention as nekos-settings. `nekos-edit [path]` opens `path` (or
 * starts a blank untitled buffer if omitted or the file doesn't exist yet).
 * Ctrl+S saves; if the buffer has no path yet, that's the same as Ctrl+Shift+S
 * (always prompts) -- a blocking "Save As" text-input window modeled on
 * nekos-files' prompt_text/paint_prompt (but this one allows '/', since it
 * takes a full path rather than a bare filename).
 *
 * Monospace font, so cursor/column math is a fixed char-width multiply
 * instead of measuring text_extents per keystroke. This assumes one column
 * per byte, which is exactly right for ASCII and only an approximation for
 * multi-byte UTF-8 (a wide/CJK glyph still advances the cursor by one
 * column) -- an accepted simplification, not a correctness goal for v1.
 * Tabs are expanded to spaces (on load and on keypress) rather than given
 * their own variable-width column, for the same reason.
 *
 * Selection: Shift+movement or a mouse drag extends a selection from an
 * anchor to the cursor; typing/Backspace/Delete replace it. Copy/Cut/Paste
 * (Ctrl+C/X/V) go through the real X11 CLIPBOARD selection -- we become the
 * selection owner on copy/cut and answer other apps' SelectionRequest events
 * from an internal buffer, and on paste we either read that buffer directly
 * (if we still own it) or ask whoever does via xcb_convert_selection and wait
 * for the SelectionNotify. This interops with Chromium/Firefox/xterm, not
 * just within nekos-edit. Simplification: paste doesn't handle the INCR
 * protocol for huge (>~256KB) clipboard contents -- fine for ordinary text
 * snippets, which is all a clipboard usually carries.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "theme.h"

#define INIT_W       760
#define INIT_H       520
#define STATUS_H     24
#define LINE_H       19
#define TAB_WIDTH    4
#define PATH_LEN     1024
#define GUTTER_PAD   14.0

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;
static xcb_key_symbols_t *keysyms;

static xcb_atom_t net_wm_name;
static xcb_atom_t utf8_string;
static xcb_atom_t atom_clipboard;
static xcb_atom_t atom_targets;
static xcb_atom_t atom_paste_prop;

static xcb_atom_t intern(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
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

/* xcb_key_press_lookup_keysym(..., 1) asks for the keycode's shift-level-1
 * (shifted) keysym. Letter keys always define one (lowercase/uppercase), but
 * arrows/Home/End/Backspace/etc. are single-keysym-per-keycode and have
 * nothing at level 1 -- asking for it returns NoSymbol (0), not a level-0
 * fallback, so a naive shift-aware lookup silently eats e.g. Shift+Right.
 * Replicate the standard X11 "one keysym per keycode means shift doesn't
 * change it" fallback ourselves. */
static xcb_keysym_t lookup_keysym(xcb_key_press_event_t *ev, int shift) {
    xcb_keysym_t k0 = xcb_key_press_lookup_keysym(keysyms, ev, 0);
    if (!shift) return k0;
    xcb_keysym_t k1 = xcb_key_press_lookup_keysym(keysyms, ev, 1);
    return k1 ? k1 : k0;
}

static int win_w = INIT_W;
static int win_h = INIT_H;
static double char_w = 8.0; /* monospace advance width, measured once at startup */

static char notify_tool[PATH_LEN + 32];

/* ---- buffer ---------------------------------------------------------- */

typedef struct {
    char *buf;
    int len;
    int cap;
} line_t;

static line_t *lines = NULL;
static int line_count = 0;
static int line_cap = 0;

static int cur_row = 0, cur_col = 0;
static int desired_col = 0; /* preferred column across Up/Down */
static int top_line = 0;    /* first visible row */
static int scroll_x = 0;    /* horizontal scroll, in columns */

static char file_path[PATH_LEN] = "";
static int has_path = 0;
static int modified = 0;

/* Selection: active whenever has_sel and (sel_row,sel_col) != (cur_row,cur_col).
 * sel_row/sel_col is the anchor set when a shift-move or mouse-drag begins;
 * cur_row/cur_col (the ordinary cursor) is the moving end. */
static int has_sel = 0;
static int sel_row = 0, sel_col = 0;
static int mouse_selecting = 0;

/* Last copied/cut text, and whether we currently own CLIPBOARD (so our own
 * paste can skip the X round-trip and read straight from here). */
static char *clip_buf = NULL;
static size_t clip_len = 0;
static int own_clipboard = 0;

static void line_ensure(line_t *l, int extra) {
    if (l->len + extra + 1 <= l->cap) return;
    int cap = l->cap ? l->cap : 16;
    while (cap < l->len + extra + 1) cap *= 2;
    l->buf = realloc(l->buf, (size_t)cap);
    l->cap = cap;
}

static void line_init(line_t *l, const char *text, int len) {
    l->cap = 0;
    l->buf = NULL;
    l->len = 0;
    line_ensure(l, len);
    if (len > 0) memcpy(l->buf, text, (size_t)len);
    l->len = len;
    l->buf[l->len] = '\0';
}

static void line_free(line_t *l) {
    free(l->buf);
    l->buf = NULL;
    l->len = l->cap = 0;
}

static void line_insert_char(line_t *l, int at, char c) {
    line_ensure(l, 1);
    memmove(l->buf + at + 1, l->buf + at, (size_t)(l->len - at));
    l->buf[at] = c;
    l->len++;
    l->buf[l->len] = '\0';
}

static void line_insert_str(line_t *l, int at, const char *s, int slen) {
    line_ensure(l, slen);
    memmove(l->buf + at + slen, l->buf + at, (size_t)(l->len - at));
    memcpy(l->buf + at, s, (size_t)slen);
    l->len += slen;
    l->buf[l->len] = '\0';
}

static void line_delete_char(line_t *l, int at) {
    memmove(l->buf + at, l->buf + at + 1, (size_t)(l->len - at - 1));
    l->len--;
    l->buf[l->len] = '\0';
}

static void line_delete_range(line_t *l, int from, int to) {
    memmove(l->buf + from, l->buf + to, (size_t)(l->len - to));
    l->len -= (to - from);
    l->buf[l->len] = '\0';
}

static void lines_ensure(int extra) {
    if (line_count + extra <= line_cap) return;
    int cap = line_cap ? line_cap : 32;
    while (cap < line_count + extra) cap *= 2;
    lines = realloc(lines, (size_t)cap * sizeof(line_t));
    line_cap = cap;
}

static void lines_insert_at(int idx, const char *text, int len) {
    lines_ensure(1);
    memmove(&lines[idx + 1], &lines[idx], (size_t)(line_count - idx) * sizeof(line_t));
    line_init(&lines[idx], text, len);
    line_count++;
}

static void lines_remove_at(int idx) {
    line_free(&lines[idx]);
    memmove(&lines[idx], &lines[idx + 1], (size_t)(line_count - idx - 1) * sizeof(line_t));
    line_count--;
}

static void lines_clear(void) {
    for (int i = 0; i < line_count; i++) line_free(&lines[i]);
    line_count = 0;
}

/* Appends `text` (len bytes, no embedded newline) to the buffer as a new
 * line, expanding tabs to spaces and dropping other control bytes so a
 * stray non-text file can't wreck rendering (see file header). */
static void push_expanded_line(const char *text, int len) {
    char tmp[4096];
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof(tmp) - TAB_WIDTH - 1; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\t') {
            int spaces = TAB_WIDTH - (n % TAB_WIDTH);
            for (int s = 0; s < spaces && n < (int)sizeof(tmp) - 1; s++) tmp[n++] = ' ';
        } else if (c < 0x20) {
            continue; /* drop other control bytes */
        } else {
            tmp[n++] = (char)c;
        }
    }
    lines_insert_at(line_count, tmp, n);
}

static void open_file(const char *path) {
    lines_clear();
    cur_row = cur_col = desired_col = top_line = scroll_x = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char *raw = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&raw, &cap, f)) != -1) {
            if (n > 0 && raw[n - 1] == '\n') n--;
            if (n > 0 && raw[n - 1] == '\r') n--;
            push_expanded_line(raw, (int)n);
        }
        free(raw);
        fclose(f);
    }
    if (line_count == 0) lines_insert_at(0, "", 0);
    modified = 0;
}

static int save_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = 1;
    for (int i = 0; i < line_count; i++) {
        if (fwrite(lines[i].buf, 1, (size_t)lines[i].len, f) != (size_t)lines[i].len) ok = 0;
        if (fputc('\n', f) == EOF) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* ---- title / notify ---------------------------------------------------- */

static void resolve_notify_tool(void) {
    char exe[PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        snprintf(notify_tool, sizeof(notify_tool), "%s/notify/nekos-notify", exe);
    } else {
        snprintf(notify_tool, sizeof(notify_tool), "notify/nekos-notify");
    }
}

static void notify_error(const char *title, const char *body) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(notify_tool, "nekos-notify", title, body, (char *)NULL);
        _exit(127);
    }
}

static void update_title(void) {
    const char *base = "Untitled";
    if (has_path) {
        const char *slash = strrchr(file_path, '/');
        base = slash ? slash + 1 : file_path;
    }
    char title[PATH_LEN + 8];
    snprintf(title, sizeof(title), "%s%s", modified ? "\xE2\x97\x8F " : "", base);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name,
                         utf8_string, 8, (uint32_t)strlen(title), title);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
    xcb_flush(conn);
}

static void mark_modified(void) {
    if (!modified) { modified = 1; update_title(); }
}

/* ---- layout / scrolling ------------------------------------------------ */

static double gutter_w(void) {
    int digits = 2;
    for (int n = line_count; n >= 100; n /= 10) digits++;
    return GUTTER_PAD + digits * char_w + GUTTER_PAD;
}

static int rows_visible(void) { return (win_h - STATUS_H) / LINE_H; }
static int cols_visible(void) { return (int)((win_w - gutter_w()) / char_w); }

static void keep_cursor_visible(void) {
    int rv = rows_visible();
    if (cur_row < top_line) top_line = cur_row;
    if (cur_row >= top_line + rv) top_line = cur_row - rv + 1;
    if (top_line < 0) top_line = 0;

    int cv = cols_visible();
    if (cv < 1) cv = 1;
    if (cur_col < scroll_x) scroll_x = cur_col;
    if (cur_col >= scroll_x + cv) scroll_x = cur_col - cv + 1;
    if (scroll_x < 0) scroll_x = 0;
}

/* ---- cursor movement / editing ------------------------------------------ */

static void clamp_col(void) {
    if (cur_col > lines[cur_row].len) cur_col = lines[cur_row].len;
    if (cur_col < 0) cur_col = 0;
}

static void move_left(void) {
    if (cur_col > 0) cur_col--;
    else if (cur_row > 0) { cur_row--; cur_col = lines[cur_row].len; }
    desired_col = cur_col;
}

static void move_right(void) {
    if (cur_col < lines[cur_row].len) cur_col++;
    else if (cur_row < line_count - 1) { cur_row++; cur_col = 0; }
    desired_col = cur_col;
}

static void move_up(void) {
    if (cur_row == 0) return;
    cur_row--;
    cur_col = desired_col;
    clamp_col();
}

static void move_down(void) {
    if (cur_row >= line_count - 1) return;
    cur_row++;
    cur_col = desired_col;
    clamp_col();
}

static void insert_char(char c) {
    line_insert_char(&lines[cur_row], cur_col, c);
    cur_col++;
    desired_col = cur_col;
    mark_modified();
}

static void insert_newline(void) {
    line_t *l = &lines[cur_row];
    int tail_len = l->len - cur_col;
    lines_insert_at(cur_row + 1, l->buf + cur_col, tail_len);
    l->len = cur_col;
    l->buf[l->len] = '\0';
    cur_row++;
    cur_col = 0;
    desired_col = 0;
    mark_modified();
}

static void backspace(void) {
    if (cur_col > 0) {
        line_delete_char(&lines[cur_row], cur_col - 1);
        cur_col--;
    } else if (cur_row > 0) {
        line_t *prev = &lines[cur_row - 1];
        line_t *cur = &lines[cur_row];
        int join_at = prev->len;
        line_insert_str(prev, prev->len, cur->buf, cur->len);
        lines_remove_at(cur_row);
        cur_row--;
        cur_col = join_at;
    } else {
        return;
    }
    desired_col = cur_col;
    mark_modified();
}

static void delete_forward(void) {
    line_t *l = &lines[cur_row];
    if (cur_col < l->len) {
        line_delete_char(l, cur_col);
    } else if (cur_row < line_count - 1) {
        line_t *next = &lines[cur_row + 1];
        line_insert_str(l, l->len, next->buf, next->len);
        lines_remove_at(cur_row + 1);
    } else {
        return;
    }
    mark_modified();
}

static void insert_tab(void) {
    int spaces = TAB_WIDTH - (cur_col % TAB_WIDTH);
    for (int i = 0; i < spaces; i++) insert_char(' ');
}

/* ---- selection / clipboard ------------------------------------------------ */

static void clear_selection(void) { has_sel = 0; }

/* Normalizes the anchor/cursor pair into an ordered (r0,c0)-(r1,c1) range. */
static void selection_bounds(int *r0, int *c0, int *r1, int *c1) {
    if (sel_row < cur_row || (sel_row == cur_row && sel_col <= cur_col)) {
        *r0 = sel_row; *c0 = sel_col; *r1 = cur_row; *c1 = cur_col;
    } else {
        *r0 = cur_row; *c0 = cur_col; *r1 = sel_row; *c1 = sel_col;
    }
}

/* Deletes [r0,c0)-(r1,c1) (r0,c0 <= r1,c1) and leaves the cursor at the cut
 * point. Multi-line case joins the first line's head to the last line's
 * tail, then removes the fully-consumed lines in between. */
static void delete_range(int r0, int c0, int r1, int c1) {
    if (r0 == r1) {
        line_delete_range(&lines[r0], c0, c1);
    } else {
        line_t *first = &lines[r0];
        line_t *last = &lines[r1];
        int tail_len = last->len - c1;
        first->len = c0;
        first->buf[first->len] = '\0';
        line_insert_str(first, c0, last->buf + c1, tail_len);
        for (int r = r0 + 1; r <= r1; r++) line_free(&lines[r]);
        memmove(&lines[r0 + 1], &lines[r1 + 1], (size_t)(line_count - r1 - 1) * sizeof(line_t));
        line_count -= (r1 - r0);
    }
    cur_row = r0;
    cur_col = c0;
    desired_col = c0;
    has_sel = 0;
    mark_modified();
}

static void delete_selection(void) {
    if (!has_sel) return;
    int r0, c0, r1, c1;
    selection_bounds(&r0, &c0, &r1, &c1);
    delete_range(r0, c0, r1, c1);
}

/* Extracts the selected text as a fresh malloc'd, NUL-terminated buffer
 * (embedded newlines joining multi-line selections); *out_len excludes the
 * terminator. Caller frees. */
static char *get_selected_text(size_t *out_len) {
    int r0, c0, r1, c1;
    selection_bounds(&r0, &c0, &r1, &c1);

    if (r0 == r1) {
        int n = c1 - c0;
        char *buf = malloc((size_t)n + 1);
        memcpy(buf, lines[r0].buf + c0, (size_t)n);
        buf[n] = '\0';
        *out_len = (size_t)n;
        return buf;
    }

    size_t total = (size_t)(lines[r0].len - c0) + 1;
    for (int r = r0 + 1; r < r1; r++) total += (size_t)lines[r].len + 1;
    total += (size_t)c1;

    char *buf = malloc(total + 1);
    size_t pos = 0;
    memcpy(buf + pos, lines[r0].buf + c0, (size_t)(lines[r0].len - c0));
    pos += (size_t)(lines[r0].len - c0);
    buf[pos++] = '\n';
    for (int r = r0 + 1; r < r1; r++) {
        memcpy(buf + pos, lines[r].buf, (size_t)lines[r].len);
        pos += (size_t)lines[r].len;
        buf[pos++] = '\n';
    }
    memcpy(buf + pos, lines[r1].buf, (size_t)c1);
    pos += (size_t)c1;
    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

/* Inserts arbitrary text (e.g. pasted clipboard content) at the cursor,
 * splitting on '\n' into real line breaks and applying the same tab-expand
 * / control-byte-drop treatment as loading a file (see push_expanded_line). */
static void insert_text_at_cursor(const char *text, int len) {
    int i = 0;
    while (i < len) {
        int j = i;
        while (j < len && text[j] != '\n') j++;
        for (int k = i; k < j; k++) {
            unsigned char c = (unsigned char)text[k];
            if (c == '\t') {
                int spaces = TAB_WIDTH - (cur_col % TAB_WIDTH);
                for (int s = 0; s < spaces; s++) insert_char(' ');
            } else if (c == '\r' || c < 0x20) {
                continue;
            } else {
                insert_char((char)c);
            }
        }
        if (j < len && text[j] == '\n') {
            insert_newline();
            j++;
        }
        i = j;
    }
}

static void do_copy(void) {
    if (!has_sel) return;
    size_t len;
    char *text = get_selected_text(&len);
    free(clip_buf);
    clip_buf = text;
    clip_len = len;
    own_clipboard = 1;
    xcb_set_selection_owner(conn, win, atom_clipboard, XCB_CURRENT_TIME);
}

static void do_cut(void) {
    if (!has_sel) return;
    do_copy();
    delete_selection();
}

static void do_paste(void) {
    if (has_sel) delete_selection();
    if (own_clipboard && clip_buf) {
        insert_text_at_cursor(clip_buf, (int)clip_len);
        keep_cursor_visible();
    } else {
        /* Async: the requested content lands as a SelectionNotify handled in
         * the main loop, which inserts it there once it arrives. */
        xcb_convert_selection(conn, win, atom_clipboard, utf8_string, atom_paste_prop, XCB_CURRENT_TIME);
    }
}

static void select_all(void) {
    sel_row = 0;
    sel_col = 0;
    cur_row = line_count - 1;
    cur_col = lines[cur_row].len;
    desired_col = cur_col;
    has_sel = 1;
}

/* ---- painting ------------------------------------------------------------ */

static void set_mono_font(cairo_t *cr, double size) {
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    theme_rgb(cr, THEME_BG);
    cairo_paint(cr);

    set_mono_font(cr, THEME_FONT_MD);
    double gw = gutter_w();

    /* Gutter. */
    theme_rgba(cr, THEME_ACCENT, 0.06);
    cairo_rectangle(cr, 0, 0, gw, win_h - STATUS_H);
    cairo_fill(cr);
    theme_rgba(cr, THEME_ACCENT, 0.2);
    cairo_rectangle(cr, gw - 1, 0, 1, win_h - STATUS_H);
    cairo_fill(cr);

    int sel_r0 = -1, sel_c0 = 0, sel_r1 = -1, sel_c1 = 0;
    if (has_sel) selection_bounds(&sel_r0, &sel_c0, &sel_r1, &sel_c1);

    int rv = rows_visible();
    for (int r = 0; r < rv; r++) {
        int row = top_line + r;
        if (row >= line_count) break;
        double y = r * LINE_H;

        char num[16];
        snprintf(num, sizeof(num), "%d", row + 1);
        cairo_text_extents_t next;
        cairo_text_extents(cr, num, &next);
        theme_rgba(cr, THEME_FG, row == cur_row ? 0.7 : 0.32);
        cairo_move_to(cr, gw - GUTTER_PAD - next.x_advance, theme_baseline(y, LINE_H, THEME_FONT_MD));
        cairo_show_text(cr, num);

        if (sel_r0 >= 0 && row >= sel_r0 && row <= sel_r1) {
            int hs = (row == sel_r0) ? sel_c0 : 0;
            int he = (row == sel_r1) ? sel_c1 : lines[row].len;
            /* Rows before the last selected one show one extra column of
             * highlight past EOL, so the selection visibly continues through
             * the newline instead of stopping dead at the last character. */
            double hx0 = gw + GUTTER_PAD + (hs - scroll_x) * char_w;
            double hx1 = gw + GUTTER_PAD + ((row < sel_r1 ? he + 1 : he) - scroll_x) * char_w;
            if (hx1 > hx0) {
                theme_rgba(cr, THEME_ACCENT, THEME_SELECT_ALPHA);
                cairo_rectangle(cr, hx0, y, hx1 - hx0, LINE_H);
                cairo_fill(cr);
            }
        }

        line_t *l = &lines[row];
        int start = scroll_x < l->len ? scroll_x : l->len;
        theme_rgba(cr, THEME_FG, 0.9);
        cairo_move_to(cr, gw + GUTTER_PAD - scroll_x * char_w + start * char_w,
                      theme_baseline(y, LINE_H, THEME_FONT_MD));
        if (start < l->len) cairo_show_text(cr, l->buf + start);
    }

    /* Cursor. */
    if (cur_row >= top_line && cur_row < top_line + rv) {
        double cy = (cur_row - top_line) * LINE_H;
        double cx = gw + GUTTER_PAD + (cur_col - scroll_x) * char_w;
        theme_rgb(cr, THEME_ACCENT);
        cairo_rectangle(cr, cx, cy + 2, 2, LINE_H - 4);
        cairo_fill(cr);
    }

    /* Status bar. */
    theme_panel_gradient(cr, 0, win_h - STATUS_H, win_w, STATUS_H, 1);
    theme_font(cr, THEME_FONT_XS, 0);
    theme_rgba(cr, THEME_FG, 0.7);
    char status[64];
    snprintf(status, sizeof(status), "Ln %d, Col %d", cur_row + 1, cur_col + 1);
    cairo_move_to(cr, THEME_PAD, win_h - STATUS_H / 2.0 + 3.0);
    cairo_show_text(cr, status);

    const char *shown_path = has_path ? file_path : "Untitled";
    theme_rgba(cr, THEME_FG, 0.45);
    cairo_text_extents_t pext;
    cairo_text_extents(cr, shown_path, &pext);
    cairo_move_to(cr, win_w - THEME_PAD - pext.x_advance, win_h - STATUS_H / 2.0 + 3.0);
    cairo_show_text(cr, shown_path);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- Save As prompt -------------------------------------------------------
 * Blocking text-input window, same lifecycle as nekos-files' prompt_text
 * (override-redirect, keyboard-grabbed, dismissed by Enter/Escape/click
 * outside) but '/' is allowed through since this takes a full path. */

#define PROMPT_W 420
#define PROMPT_H 92

static void paint_prompt(xcb_window_t pwin, const char *buf) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, pwin, visual, PROMPT_W, PROMPT_H);
    cairo_t *cr = cairo_create(surface);

    theme_panel_gradient(cr, 0, 0, PROMPT_W, PROMPT_H, 1);

    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, 0.65);
    cairo_move_to(cr, THEME_PAD, 24);
    cairo_show_text(cr, "Save as:");

    theme_rgba(cr, THEME_ACCENT, 0.12);
    theme_rounded_rect(cr, THEME_PAD, 34, PROMPT_W - 2 * THEME_PAD, 36, THEME_RADIUS);
    cairo_fill(cr);

    set_mono_font(cr, THEME_FONT_SM);
    theme_rgb(cr, THEME_FG);
    double tx = THEME_PAD + 8, ty = 34 + 23;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, buf);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_rectangle(cr, tx + ext.x_advance + 2, 34 + 7, 2, 22);
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

static int prompt_save_path(char *out, size_t out_size) {
    char buf[PATH_LEN];
    if (has_path) snprintf(buf, sizeof(buf), "%s", file_path);
    else {
        const char *home = getenv("HOME");
        snprintf(buf, sizeof(buf), "%s/untitled.txt", home ? home : ".");
    }

    int16_t x = (int16_t)((screen->width_in_pixels - PROMPT_W) / 2);
    int16_t y = (int16_t)((screen->height_in_pixels - PROMPT_H) / 3);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    xcb_window_t pwin = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    uint32_t values[3] = { screen->black_pixel, 1, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, pwin, screen->root,
                       x, y, PROMPT_W, PROMPT_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    xcb_map_window(conn, pwin);
    xcb_flush(conn);

    xcb_grab_keyboard_reply_t *kr = xcb_grab_keyboard_reply(conn,
        xcb_grab_keyboard(conn, 1, screen->root, XCB_CURRENT_TIME,
                           XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    int kbd_ok = kr && kr->status == XCB_GRAB_STATUS_SUCCESS;
    free(kr);
    if (!kbd_ok) {
        xcb_destroy_window(conn, pwin);
        xcb_flush(conn);
        return 0;
    }
    xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(conn,
        xcb_grab_pointer(conn, 0, screen->root, XCB_EVENT_MASK_BUTTON_PRESS,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                          XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
    free(gr);

    paint_prompt(pwin, buf);

    int result = 0;
    int xfd = xcb_get_file_descriptor(conn);
    int settled = 0, done = 0;
    while (!done) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, settled ? -1 : 40);
        if (!settled) { paint_prompt(pwin, buf); settled = 1; }

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t rt = event->response_type & ~0x80;
            if (rt == XCB_EXPOSE) {
                paint_prompt(pwin, buf);
            } else if (rt == XCB_KEY_PRESS) {
                xcb_key_press_event_t *ke = (xcb_key_press_event_t *)event;
                xcb_keysym_t ks = lookup_keysym(ke, (ke->state & XCB_MOD_MASK_SHIFT) != 0);
                if (ks == XK_Escape) {
                    done = 1;
                } else if (ks == XK_Return || ks == XK_KP_Enter) {
                    if (buf[0]) result = 1;
                    done = 1;
                } else if (ks == XK_BackSpace) {
                    size_t l = strlen(buf);
                    if (l > 0) { buf[l - 1] = '\0'; paint_prompt(pwin, buf); }
                } else if (ks >= 0x20 && ks <= 0x7e) {
                    size_t l = strlen(buf);
                    if (l < sizeof(buf) - 1) {
                        buf[l] = (char)ks;
                        buf[l + 1] = '\0';
                        paint_prompt(pwin, buf);
                    }
                }
            } else if (rt == XCB_BUTTON_PRESS) {
                xcb_button_press_event_t *be = (xcb_button_press_event_t *)event;
                if (be->root_x < x || be->root_x >= x + PROMPT_W ||
                    be->root_y < y || be->root_y >= y + PROMPT_H) {
                    done = 1;
                }
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;
    }

    xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
    xcb_destroy_window(conn, pwin);
    xcb_flush(conn);

    if (result) snprintf(out, out_size, "%s", buf);
    return result;
}

static void do_save(int force_prompt) {
    char target[PATH_LEN];
    if (force_prompt || !has_path) {
        if (!prompt_save_path(target, sizeof(target))) return;
    } else {
        snprintf(target, sizeof(target), "%s", file_path);
    }

    if (!save_file(target)) {
        notify_error("Save failed", strerror(errno));
        return;
    }
    snprintf(file_path, sizeof(file_path), "%s", target);
    has_path = 1;
    modified = 0;
    update_title();
}

/* ---- input --------------------------------------------------------------- */

static int is_movement_key(xcb_keysym_t ks) {
    switch (ks) {
    case XK_Left: case XK_Right: case XK_Up: case XK_Down:
    case XK_Home: case XK_End: case XK_Prior: case XK_Next:
        return 1;
    default:
        return 0;
    }
}

static void handle_key(xcb_key_press_event_t *ev) {
    int ctrl = (ev->state & XCB_MOD_MASK_CONTROL) != 0;
    int shift = (ev->state & XCB_MOD_MASK_SHIFT) != 0;
    xcb_keysym_t ks = lookup_keysym(ev, shift);

    if (ctrl && (ks == 's' || ks == 'S')) { do_save(shift); paint(); return; }
    if (ctrl && (ks == 'c' || ks == 'C')) { do_copy(); paint(); return; }
    if (ctrl && (ks == 'x' || ks == 'X')) { do_cut(); keep_cursor_visible(); paint(); return; }
    if (ctrl && (ks == 'v' || ks == 'V')) { do_paste(); paint(); return; }
    if (ctrl && (ks == 'a' || ks == 'A')) { select_all(); paint(); return; }
    if (ks == XK_Escape && has_sel) { clear_selection(); paint(); return; }

    if (is_movement_key(ks)) {
        /* Shift starts (or continues) a selection anchored where the cursor
         * was before this move; plain movement drops any existing one. */
        if (shift) {
            if (!has_sel) { sel_row = cur_row; sel_col = cur_col; has_sel = 1; }
        } else {
            has_sel = 0;
        }
    } else if (ks == XK_BackSpace || ks == XK_Delete) {
        if (has_sel) { delete_selection(); keep_cursor_visible(); paint(); return; }
    } else if (has_sel && ((!ctrl && ks >= 0x20 && ks <= 0x7e) ||
                           ks == XK_Return || ks == XK_KP_Enter || ks == XK_Tab)) {
        delete_selection(); /* typing over a selection replaces it */
    }

    switch (ks) {
    case XK_Left:      move_left(); break;
    case XK_Right:     move_right(); break;
    case XK_Up:        move_up(); break;
    case XK_Down:      move_down(); break;
    case XK_Home:
        if (ctrl) cur_row = 0;
        cur_col = 0;
        desired_col = 0;
        break;
    case XK_End:
        if (ctrl) { cur_row = line_count - 1; }
        cur_col = lines[cur_row].len;
        desired_col = cur_col;
        break;
    case XK_Prior: /* Page Up */
        cur_row -= rows_visible();
        if (cur_row < 0) cur_row = 0;
        clamp_col();
        break;
    case XK_Next: /* Page Down */
        cur_row += rows_visible();
        if (cur_row >= line_count) cur_row = line_count - 1;
        clamp_col();
        break;
    case XK_Return:
    case XK_KP_Enter:
        insert_newline();
        break;
    case XK_BackSpace:
        backspace();
        break;
    case XK_Delete:
        delete_forward();
        break;
    case XK_Tab:
        insert_tab();
        break;
    default:
        if (!ctrl && ks >= 0x20 && ks <= 0x7e) insert_char((char)ks);
        break;
    }

    keep_cursor_visible();
    paint();
}

/* Text-area row/col under window-relative (x,y), clamped to valid buffer
 * positions. Shared by click-to-position and drag-select. */
static void pos_at(int16_t x, int16_t y, int *out_row, int *out_col) {
    double gw = gutter_w();
    int row = top_line + y / LINE_H;
    if (row >= line_count) row = line_count - 1;
    if (row < 0) row = 0;
    int col = scroll_x + (int)((x - gw - GUTTER_PAD) / char_w + 0.5);
    if (col > lines[row].len) col = lines[row].len;
    if (col < 0) col = 0;
    *out_row = row;
    *out_col = col;
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    if (ev->event_y >= win_h - STATUS_H) return;

    if (ev->detail == 4) { top_line -= 3; if (top_line < 0) top_line = 0; paint(); return; }
    if (ev->detail == 5) {
        top_line += 3;
        int max_top = line_count - 1;
        if (top_line > max_top) top_line = max_top < 0 ? 0 : max_top;
        paint();
        return;
    }
    if (ev->detail != 1) return;

    /* A plain click starts a fresh (empty) selection anchor here; if it
     * turns into a drag, handle_motion_notify grows it from this point.
     * Shift+click instead extends whatever selection already existed, and
     * shows that extension immediately rather than waiting for a drag. */
    int shift_extend = (ev->state & XCB_MOD_MASK_SHIFT) && has_sel;
    pos_at(ev->event_x, ev->event_y, &cur_row, &cur_col);
    desired_col = cur_col;
    if (!shift_extend) {
        sel_row = cur_row;
        sel_col = cur_col;
    }
    has_sel = shift_extend && (cur_row != sel_row || cur_col != sel_col);
    mouse_selecting = 1;
    paint();
}

static void handle_motion_notify(xcb_motion_notify_event_t *ev) {
    if (!mouse_selecting) return;
    int row, col;
    int16_t y = ev->event_y;
    if (y >= win_h - STATUS_H) y = (int16_t)(win_h - STATUS_H - 1);
    if (y < 0) y = 0;
    pos_at(ev->event_x, y, &row, &col);
    if (row != cur_row || col != cur_col) {
        cur_row = row;
        cur_col = col;
        desired_col = col;
        has_sel = (row != sel_row || col != sel_col);
        keep_cursor_visible();
        paint();
    }
}

static void handle_button_release(xcb_button_release_event_t *ev) {
    if (ev->detail == 1) mouse_selecting = 0;
}

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_IGN);

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-edit: could not connect to X server\n");
        return 1;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    keysyms = xcb_key_symbols_alloc(conn);
    resolve_notify_tool();

    if (argc >= 2) {
        /* A path given on the command line is the save target whether or not
         * it exists yet -- open_file() starts a blank buffer for a missing
         * file, and Ctrl+S then writes straight to it, no Save As needed. */
        snprintf(file_path, sizeof(file_path), "%s", argv[1]);
        has_path = 1;
        open_file(file_path);
    } else {
        lines_insert_at(0, "", 0);
    }

    net_wm_name = intern("_NET_WM_NAME");
    utf8_string = intern("UTF8_STRING");
    atom_clipboard = intern("CLIPBOARD");
    atom_targets = intern("TARGETS");
    atom_paste_prop = intern("_NEKOS_EDIT_PASTE");

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_1_MOTION |
            XCB_EVENT_MASK_STRUCTURE_NOTIFY,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, INIT_W, INIT_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    xcb_map_window(conn, win);
    xcb_flush(conn);

    /* Measure the monospace advance width once now that the window (and a
     * cairo surface to measure against) exists. */
    {
        cairo_surface_t *msurf = cairo_xcb_surface_create(conn, win, visual, 1, 1);
        cairo_t *mcr = cairo_create(msurf);
        set_mono_font(mcr, THEME_FONT_MD);
        cairo_text_extents_t ext;
        cairo_text_extents(mcr, "M", &ext);
        if (ext.x_advance > 0) char_w = ext.x_advance;
        cairo_destroy(mcr);
        cairo_surface_destroy(msurf);
    }

    update_title();
    keep_cursor_visible();
    paint();

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn))) {
        switch (event->response_type & ~0x80) {
        case XCB_EXPOSE:
            paint();
            break;
        case XCB_CONFIGURE_NOTIFY: {
            xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)event;
            if (ce->width > 0 && ce->height > 0 && (ce->width != win_w || ce->height != win_h)) {
                win_w = ce->width;
                win_h = ce->height;
                keep_cursor_visible();
                paint();
            }
            break;
        }
        case XCB_KEY_PRESS:
            handle_key((xcb_key_press_event_t *)event);
            break;
        case XCB_BUTTON_PRESS:
            handle_button_press((xcb_button_press_event_t *)event);
            break;
        case XCB_MOTION_NOTIFY:
            handle_motion_notify((xcb_motion_notify_event_t *)event);
            break;
        case XCB_BUTTON_RELEASE:
            handle_button_release((xcb_button_release_event_t *)event);
            break;
        case XCB_SELECTION_REQUEST: {
            /* Another app wants data from the CLIPBOARD we own -- answer
             * TARGETS with what we can provide, and UTF8_STRING/STRING with
             * the actual bytes (X11 no longer distinguishes latin1 STRING
             * from UTF8_STRING content-wise for our purposes; a plain-text
             * editor's buffer bytes work as either). ICCCM: pre-2000 clients
             * may pass property==None, meaning "use target as the property". */
            xcb_selection_request_event_t *sr = (xcb_selection_request_event_t *)event;
            xcb_atom_t prop = (sr->property != XCB_ATOM_NONE) ? sr->property : sr->target;
            xcb_selection_notify_event_t note;
            memset(&note, 0, sizeof(note));
            note.response_type = XCB_SELECTION_NOTIFY;
            note.time = sr->time;
            note.requestor = sr->requestor;
            note.selection = sr->selection;
            note.target = sr->target;
            note.property = XCB_ATOM_NONE;

            if (sr->target == atom_targets) {
                xcb_atom_t targets[3] = { atom_targets, utf8_string, XCB_ATOM_STRING };
                xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sr->requestor, prop,
                                     XCB_ATOM_ATOM, 32, 3, targets);
                note.property = prop;
            } else if (sr->target == utf8_string || sr->target == XCB_ATOM_STRING) {
                xcb_change_property(conn, XCB_PROP_MODE_REPLACE, sr->requestor, prop,
                                     sr->target, 8, (uint32_t)clip_len, clip_buf ? clip_buf : "");
                note.property = prop;
            }
            xcb_send_event(conn, 0, sr->requestor, 0, (const char *)&note);
            xcb_flush(conn);
            break;
        }
        case XCB_SELECTION_CLEAR:
            /* Another app took CLIPBOARD ownership; our own paste must now
             * go through X instead of assuming clip_buf is still current. */
            own_clipboard = 0;
            break;
        case XCB_SELECTION_NOTIFY: {
            xcb_selection_notify_event_t *sn = (xcb_selection_notify_event_t *)event;
            if (sn->property != XCB_ATOM_NONE) {
                xcb_get_property_reply_t *rep = xcb_get_property_reply(conn,
                    xcb_get_property(conn, 0, win, sn->property, XCB_ATOM_ANY, 0, 0x1fffffff), NULL);
                if (rep) {
                    int len = xcb_get_property_value_length(rep);
                    if (len > 0) {
                        insert_text_at_cursor((char *)xcb_get_property_value(rep), len);
                        keep_cursor_visible();
                    }
                    free(rep);
                }
                xcb_delete_property(conn, win, sn->property);
                paint();
            }
            break;
        }
        default:
            break;
        }
        free(event);
    }

    xcb_key_symbols_free(keysyms);
    xcb_disconnect(conn);
    return 0;
}
