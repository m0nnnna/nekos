/*
 * nekos-files: a lean file manager.
 *
 * Browses a directory as an icon+name list: double-click a folder to enter it
 * (or ".." to go up), double-click a file to open it via nekos-open (nekOS's
 * xdg-open stand-in, which picks the right app by type). A normal (non-dock)
 * window, so it gets nekos-wm's titlebar/close/move/resize for free.
 *
 * Right-click a row for Copy/Cut/Rename/New Folder/Delete (menu_show, shared
 * with the bar and desktop); Paste lands on empty-area right-click once
 * something's on the clipboard. Delete moves to ~/.local/share/nekos/trash
 * (loosely modeled on the freedesktop.org Trash spec) instead of removing
 * outright; "Open Trash" (also empty-area right-click) gets you there to
 * Restore or "Delete Permanently". Multi-select is still left for a later
 * pass -- see ticket nekos#7.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700 /* nftw()/FTW_DEPTH/FTW_PHYS live behind this, not _DEFAULT_SOURCE */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "image.h"
#include "menu.h"
#include "theme.h"

#define INIT_WIDTH   620
#define INIT_HEIGHT  520
#define HEADER_H     42
#define ROW_H        30
#define ICON_SIZE    20
#define ICON_X       12
#define TEXT_X       (ICON_X + ICON_SIZE + 12)
#define UP_X         8   /* the up-button square in the header */
#define UP_SIZE      26
#define PATH_X       (UP_X + UP_SIZE + 10)
#define MAX_SEGS     64
#define SCROLL_STEP  3
#define DOUBLE_CLICK_MS 400
#define MAX_ENTRIES  2048
#define NAME_LEN     256
#define PATH_LEN     4096

typedef enum {
    CAT_DIR, CAT_IMAGE, CAT_TEXT, CAT_HTML, CAT_EXEC, CAT_GENERIC
} cat_t;

typedef struct {
    char name[NAME_LEN];
    int is_dir;
    cat_t cat;
    /* Image rows get a real thumbnail, loaded lazily the first time the row
     * is painted (visible rows only, so entering a huge photo dir doesn't
     * stall on decoding everything). */
    cairo_surface_t *thumb;
    int thumb_tried;
} entry_t;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;

static xcb_atom_t net_wm_name;
static xcb_atom_t utf8_string;

static entry_t entries[MAX_ENTRIES];
static int entry_count = 0;
static char cur_dir[PATH_LEN] = "/";
static int scroll_off = 0;
static int sel_index = -1;
static int win_w = INIT_WIDTH;
static int win_h = INIT_HEIGHT;

/* Hover state (repaint only on change). */
static int hover_row = -1;
static int hover_up = 0;
static int hover_seg = -1;

/* Clickable path segments, rebuilt on every paint: seg_x[i]..seg_x2[i] is
 * segment i's on-screen span, seg_len[i] the cur_dir prefix length it
 * navigates to. */
static double seg_x[MAX_SEGS], seg_x2[MAX_SEGS];
static int seg_len[MAX_SEGS];
static int seg_count = 0;

static char open_tool[PATH_LEN + 32]; /* resolved path to bin/nekos-open (room for the suffix) */
static char notify_tool[PATH_LEN + 32]; /* resolved path to notify/nekos-notify, same trick */

static xcb_timestamp_t last_click_time = 0;
static int last_click_index = -1;

static xcb_key_symbols_t *keysyms;

/* Right-click context menu: which row its actions apply to (-1 for none, e.g.
 * an empty-area click), and where to re-anchor a follow-up confirm popup at
 * the same spot the original menu was opened. */
static int ctx_target_idx = -1;
static int16_t ctx_menu_x, ctx_menu_y;

/* Trash: ~/.local/share/nekos/trash/{files,info}, split the way the
 * freedesktop.org Trash spec does (files/ holds the moved item itself,
 * info/ the sidecar metadata) so info files never show up as clutter when
 * browsing files/. Populated by init_trash_dirs() in main(). */
static char trash_files_dir[PATH_LEN];
static char trash_info_dir[PATH_LEN];

/* Clipboard: a single item (path + its bare name), cut-vs-copy. Copying
 * leaves it in place for repeated pastes; cutting clears it the moment a
 * paste actually moves the item. */
static char clip_path[PATH_LEN + NAME_LEN + 2] = "";
static char clip_name[NAME_LEN] = "";
static int clip_is_cut = 0;
static int clip_has_item = 0;

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

/* Resolves <repo>/bin/nekos-open from this executable's own location
 * (<repo>/files/nekos-files), so opening files works regardless of cwd. */
static void resolve_open_tool(void) {
    char exe[PATH_LEN];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');       /* .../files/nekos-files -> .../files */
        if (slash) *slash = '\0';
        slash = strrchr(exe, '/');             /* .../files -> repo root */
        if (slash) *slash = '\0';
        snprintf(open_tool, sizeof(open_tool), "%s/bin/nekos-open", exe);
    } else {
        snprintf(open_tool, sizeof(open_tool), "bin/nekos-open"); /* cwd fallback */
    }
}

/* Resolves <repo>/notify/nekos-notify the same way, for error toasts on
 * failed file operations. */
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

/* Fire-and-forget error toast. SIGCHLD is SIG_IGN (set in main()), so the
 * child is auto-reaped -- no waitpid needed here. */
static void notify_error(const char *title, const char *body) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(notify_tool, "nekos-notify", title, body, (char *)NULL);
        _exit(127);
    }
}

static int ext_matches(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    }
    return 0;
}

static cat_t categorize(const char *name, int is_dir, mode_t mode) {
    if (is_dir) return CAT_DIR;

    static const char *const image_exts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tiff", ".tif",
        ".ppm", ".pnm", ".pgm", ".xpm", ".tga", NULL };
    static const char *const html_exts[] = { ".html", ".htm", ".pdf", ".svg", NULL };
    static const char *const text_exts[] = {
        ".txt", ".md", ".c", ".h", ".cpp", ".cc", ".hpp", ".sh", ".py", ".js",
        ".ts", ".css", ".json", ".xml", ".yaml", ".yml", ".toml", ".ini",
        ".conf", ".cfg", ".log", ".rs", ".go", ".lua", ".rb", ".pl", ".sql",
        ".mk", ".make", NULL };

    if (ext_matches(name, image_exts)) return CAT_IMAGE;
    if (ext_matches(name, html_exts)) return CAT_HTML;
    if (ext_matches(name, text_exts)) return CAT_TEXT;
    if (mode & S_IXUSR) return CAT_EXEC;
    return CAT_GENERIC;
}

static int cmp_entries(const void *a, const void *b) {
    const entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir; /* dirs first */
    return strcasecmp(ea->name, eb->name);
}

/* Loads `path` (canonicalized) into entries[], dirs first then files, with
 * ".." prepended unless at filesystem root. Hidden dotfiles are skipped for
 * v1. Leaves cur_dir unchanged if `path` can't be resolved. */
static void free_thumbs(void) {
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].thumb) cairo_surface_destroy(entries[i].thumb);
        entries[i].thumb = NULL;
        entries[i].thumb_tried = 0;
    }
}

static void load_dir(const char *path) {
    char resolved[PATH_LEN];
    if (!realpath(path, resolved)) return;

    free_thumbs();

    strncpy(cur_dir, resolved, sizeof(cur_dir) - 1);
    cur_dir[sizeof(cur_dir) - 1] = '\0';

    entry_count = 0;
    DIR *d = opendir(cur_dir);
    if (d) {
        struct dirent *e;
        while (entry_count < MAX_ENTRIES && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue; /* skips ".", "..", and hidden */

            entry_t *en = &entries[entry_count];
            strncpy(en->name, e->d_name, NAME_LEN - 1);
            en->name[NAME_LEN - 1] = '\0';
            en->thumb = NULL;
            en->thumb_tried = 0;

            char full[PATH_LEN + NAME_LEN + 2];
            snprintf(full, sizeof(full), "%s/%s", cur_dir, en->name);

            struct stat st;
            if (lstat(full, &st) == 0) {
                en->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
                if (S_ISLNK(st.st_mode)) {
                    struct stat target;
                    if (stat(full, &target) == 0) en->is_dir = S_ISDIR(target.st_mode) ? 1 : 0;
                }
                en->cat = categorize(en->name, en->is_dir, st.st_mode);
            } else {
                en->is_dir = 0;
                en->cat = CAT_GENERIC;
            }
            entry_count++;
        }
        closedir(d);
    }

    qsort(entries, entry_count, sizeof(entry_t), cmp_entries);

    if (strcmp(cur_dir, "/") != 0 && entry_count < MAX_ENTRIES) {
        memmove(&entries[1], &entries[0], (size_t)entry_count * sizeof(entry_t));
        entry_count++;
        strcpy(entries[0].name, "..");
        entries[0].is_dir = 1;
        entries[0].cat = CAT_DIR;
        entries[0].thumb = NULL;
        entries[0].thumb_tried = 0;
    }

    scroll_off = 0;
    sel_index = -1;
    hover_row = -1;
}

static void open_path(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(open_tool, "nekos-open", path, (char *)NULL);
        _exit(127);
    }
}

/* Activates entry `idx`: enter a directory, or hand a file to nekos-open. */
static void activate(int idx) {
    if (idx < 0 || idx >= entry_count) return;
    entry_t *en = &entries[idx];

    char target[PATH_LEN + NAME_LEN + 2];
    if (strcmp(en->name, "..") == 0) {
        snprintf(target, sizeof(target), "%s/..", cur_dir);
    } else {
        snprintf(target, sizeof(target), "%s/%s", cur_dir, en->name);
    }

    if (en->is_dir) {
        load_dir(target);
    } else {
        open_path(target);
    }
}

static void paint(void); /* defined below; the context-menu actions repaint after changing state */

/* ---- context-menu actions ------------------------------------------------ */

static int rm_visit(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    return (typeflag == FTW_DP) ? rmdir(path) : unlink(path);
}

/* Removes a file or a whole directory tree. FTW_DEPTH visits children before
 * their parent so rmdir() always sees an empty directory by the time it's
 * called; FTW_PHYS doesn't follow symlinks (a symlinked subdirectory gets
 * unlinked itself, not descended into and emptied out). */
static int remove_recursive(const char *path) {
    return nftw(path, rm_visit, 16, FTW_DEPTH | FTW_PHYS);
}

/* State for copy_visit's nftw callback -- classic nftw() has no user-data
 * pointer, so (like the rest of this single-threaded, one-operation-at-a-time
 * file manager) it's file-scope statics rather than a context struct. */
static size_t copy_src_root_len;
static const char *copy_dst_root;
static int copy_had_failure;

static int copy_visit(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    const char *rel = path + copy_src_root_len; /* "" for the root itself, "/child/..." below it */
    char dest[PATH_LEN * 2];
    snprintf(dest, sizeof(dest), "%s%s", copy_dst_root, rel);

    if (typeflag == FTW_D) {
        if (mkdir(dest, sb->st_mode & 07777) != 0 && errno != EEXIST) copy_had_failure = 1;
        return 0;
    }
    if (typeflag != FTW_F) {
        copy_had_failure = 1; /* symlink or other special file -- not handled */
        return 0;
    }

    int in_fd = open(path, O_RDONLY);
    if (in_fd < 0) { copy_had_failure = 1; return 0; }
    int out_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, sb->st_mode & 07777);
    if (out_fd < 0) { close(in_fd); copy_had_failure = 1; return 0; }

    char buf[65536];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
            if (w < 0) { copy_had_failure = 1; n = 0; break; }
            off += w;
        }
    }
    if (n < 0) copy_had_failure = 1;

    close(in_fd);
    close(out_fd);
    return 0;
}

/* Copies a file or a whole directory tree from `src` to `dst`. FTW_PHYS (no
 * FTW_DEPTH) visits pre-order -- a directory is mkdir'd before its contents
 * are copied into it, and for a plain-file `src` the single callback IS the
 * copy, with rel=="" so dest==dst exactly. */
static int copy_recursive(const char *src, const char *dst) {
    copy_src_root_len = strlen(src);
    copy_dst_root = dst;
    copy_had_failure = 0;
    if (nftw(src, copy_visit, 16, FTW_PHYS) != 0) return -1;
    return copy_had_failure ? -1 : 0;
}

/* Creates every path component of `path` that doesn't already exist.
 * Idempotent -- EEXIST from an already-there component is expected, not an
 * error. */
static void mkdir_p(const char *path) {
    char tmp[PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void init_trash_dirs(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/root";

    char raw[PATH_LEN];
    snprintf(raw, sizeof(raw), "%s/.local/share/nekos/trash/files", home);
    mkdir_p(raw);
    if (!realpath(raw, trash_files_dir)) snprintf(trash_files_dir, sizeof(trash_files_dir), "%s", raw);

    snprintf(raw, sizeof(raw), "%s/.local/share/nekos/trash/info", home);
    mkdir_p(raw);
    if (!realpath(raw, trash_info_dir)) snprintf(trash_info_dir, sizeof(trash_info_dir), "%s", raw);
}

/* cur_dir is always realpath()'d by load_dir(), and trash_files_dir was
 * realpath()'d once at startup -- a plain strcmp is enough to tell whether
 * we're currently browsing the trash itself. */
static int in_trash(void) {
    return strcmp(cur_dir, trash_files_dir) == 0;
}

/* Finds a name inside `dir` that doesn't collide with an existing entry,
 * appending " (2)", " (3)", ... until one's free. Used both for trashing
 * same-named files repeatedly and for pasting into a directory that already
 * has something by that name (including pasting back into its own folder). */
static void unique_name_in(const char *dir, const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", name);
    char probe[PATH_LEN + NAME_LEN + 2];
    for (int n = 2; ; n++) {
        snprintf(probe, sizeof(probe), "%s/%s", dir, out);
        struct stat st;
        if (lstat(probe, &st) != 0) return; /* free */
        snprintf(out, out_size, "%s (%d)", name, n);
    }
}

static void unique_trash_name(const char *name, char *out, size_t out_size) {
    unique_name_in(trash_files_dir, name, out, out_size);
}

/* Blocking single-line text prompt, modeled on nekos-launch's search box and
 * menu_show's popup lifecycle: an override-redirect window with the keyboard
 * grabbed (so typing can't leak to whatever's behind it), dismissed by Enter
 * (accepts into `out`), Escape, or a click outside. */
#define PROMPT_W 340
#define PROMPT_H 92

static void paint_prompt(xcb_window_t pwin, const char *label, const char *buf) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, pwin, visual, PROMPT_W, PROMPT_H);
    cairo_t *cr = cairo_create(surface);

    theme_panel_gradient(cr, 0, 0, PROMPT_W, PROMPT_H, 1);

    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, 0.65);
    cairo_move_to(cr, THEME_PAD, 24);
    cairo_show_text(cr, label);

    theme_rgba(cr, THEME_ACCENT, 0.12);
    theme_rounded_rect(cr, THEME_PAD, 34, PROMPT_W - 2 * THEME_PAD, 36, THEME_RADIUS);
    cairo_fill(cr);

    theme_font(cr, THEME_FONT_MD, 0);
    theme_rgb(cr, THEME_FG);
    double tx = THEME_PAD + 8, ty = 34 + 23;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, buf);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_rectangle(cr, tx + ext.x_advance + 2, 34 + 7, 2, 22); /* cursor */
    cairo_fill(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* Returns 1 and fills `out` on a confirmed, non-empty, non-"."/".." result;
 * 0 on cancel. '/' is filtered out of typed input so the result can never
 * smuggle in a path separator. */
static int prompt_text(const char *label, const char *initial, char *out, size_t out_size) {
    char buf[NAME_LEN];
    snprintf(buf, sizeof(buf), "%s", initial ? initial : "");

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

    /* Best-effort: lets a click outside cancel the prompt. Not fatal if it
     * doesn't land -- Enter/Escape still work either way. */
    xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(conn,
        xcb_grab_pointer(conn, 0, screen->root, XCB_EVENT_MASK_BUTTON_PRESS,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                          XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
    free(gr);

    paint_prompt(pwin, label, buf);
    xcb_flush(conn);

    int result = 0;
    int xfd = xcb_get_file_descriptor(conn);
    int settled = 0, done = 0;
    while (!done) {
        struct pollfd pfd = { .fd = xfd, .events = POLLIN, .revents = 0 };
        /* Same settle-repaint trick as menu_show: the compositor only starts
         * damage-tracking this override-redirect window on its MapNotify,
         * which races with the paint above. */
        poll(&pfd, 1, settled ? -1 : 40);
        if (!settled) { paint_prompt(pwin, label, buf); settled = 1; }

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            uint8_t rt = event->response_type & ~0x80;
            if (rt == XCB_EXPOSE) {
                paint_prompt(pwin, label, buf);
            } else if (rt == XCB_KEY_PRESS) {
                xcb_key_press_event_t *ke = (xcb_key_press_event_t *)event;
                xcb_keysym_t ks = xcb_key_press_lookup_keysym(keysyms, ke,
                    (ke->state & XCB_MOD_MASK_SHIFT) ? 1 : 0);
                if (ks == XK_Escape) {
                    done = 1;
                } else if (ks == XK_Return || ks == XK_KP_Enter) {
                    if (buf[0] && strcmp(buf, ".") != 0 && strcmp(buf, "..") != 0) result = 1;
                    done = 1;
                } else if (ks == XK_BackSpace) {
                    size_t l = strlen(buf);
                    if (l > 0) { buf[l - 1] = '\0'; paint_prompt(pwin, label, buf); }
                } else if (ks >= 0x20 && ks <= 0x7e && ks != '/') {
                    size_t l = strlen(buf);
                    if (l < sizeof(buf) - 1) {
                        buf[l] = (char)ks;
                        buf[l + 1] = '\0';
                        paint_prompt(pwin, label, buf);
                    }
                }
            } else if (rt == XCB_BUTTON_PRESS) {
                xcb_button_press_event_t *be = (xcb_button_press_event_t *)event;
                if (be->root_x < x || be->root_x >= x + PROMPT_W ||
                    be->root_y < y || be->root_y >= y + PROMPT_H) {
                    done = 1; /* click outside -> cancel */
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

static void action_ctx_open(void *ud) {
    (void)ud;
    activate(ctx_target_idx);
    paint();
}

static void action_ctx_rename(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count) return;

    char label[64];
    snprintf(label, sizeof(label), "Rename \"%.30s\" to:", entries[idx].name);
    char new_name[NAME_LEN];
    if (!prompt_text(label, entries[idx].name, new_name, sizeof(new_name))) return;
    if (strcmp(new_name, entries[idx].name) == 0) return;

    char old_path[PATH_LEN + NAME_LEN + 2], new_path[PATH_LEN + NAME_LEN + 2];
    snprintf(old_path, sizeof(old_path), "%s/%s", cur_dir, entries[idx].name);
    snprintf(new_path, sizeof(new_path), "%s/%s", cur_dir, new_name);

    struct stat st;
    if (lstat(new_path, &st) == 0) {
        notify_error("Rename failed", "That name is already in use");
        return;
    }
    if (rename(old_path, new_path) != 0) notify_error("Rename failed", strerror(errno));
    load_dir(cur_dir);
    paint();
}

static void action_ctx_new_folder(void *ud) {
    (void)ud;
    char name[NAME_LEN];
    if (!prompt_text("New folder name:", "", name, sizeof(name))) return;

    char path[PATH_LEN + NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", cur_dir, name);
    if (mkdir(path, 0755) != 0) notify_error("Couldn't create folder", strerror(errno));
    load_dir(cur_dir);
    paint();
}

/* Copy/Cut just record the target -- nothing changes on disk until Paste.
 * Copying leaves the clipboard set for repeated pastes; see action_ctx_paste
 * for why cutting clears it only once the move actually happens. */
static void action_ctx_copy(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count || strcmp(entries[idx].name, "..") == 0) return;
    snprintf(clip_path, sizeof(clip_path), "%s/%s", cur_dir, entries[idx].name);
    snprintf(clip_name, sizeof(clip_name), "%s", entries[idx].name);
    clip_is_cut = 0;
    clip_has_item = 1;
}

static void action_ctx_cut(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count || strcmp(entries[idx].name, "..") == 0) return;
    snprintf(clip_path, sizeof(clip_path), "%s/%s", cur_dir, entries[idx].name);
    snprintf(clip_name, sizeof(clip_name), "%s", entries[idx].name);
    clip_is_cut = 1;
    clip_has_item = 1;
}

/* Pastes the clipboard into cur_dir, uniquifying the name if something's
 * already there by that name (including pasting a copy back into its own
 * folder). A cut-paste is a rename() -- same filesystem, so effectively
 * instant, and clears the clipboard since the source is now gone from where
 * it was. A copy-paste walks the source with copy_recursive and leaves the
 * clipboard set so it can be pasted again elsewhere. */
static void action_ctx_paste(void *ud) {
    (void)ud;
    if (!clip_has_item) return;

    struct stat srcst;
    if (lstat(clip_path, &srcst) != 0) {
        notify_error("Paste failed", "That item is gone");
        clip_has_item = 0;
        return;
    }

    char dest_name[NAME_LEN + 16];
    unique_name_in(cur_dir, clip_name, dest_name, sizeof(dest_name));
    char dest_path[PATH_LEN + NAME_LEN + 2];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", cur_dir, dest_name);

    if (clip_is_cut) {
        if (rename(clip_path, dest_path) != 0) notify_error("Move failed", strerror(errno));
        clip_has_item = 0;
    } else if (copy_recursive(clip_path, dest_path) != 0) {
        notify_error("Copy failed", "Some items couldn't be copied");
    }

    load_dir(cur_dir);
    paint();
}

/* Moves an entry to the trash (rename into trash_files_dir + a .trashinfo
 * sidecar recording where it came from) instead of removing it outright. */
static void action_confirm_delete(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count) return;

    char src[PATH_LEN + NAME_LEN + 2];
    snprintf(src, sizeof(src), "%s/%s", cur_dir, entries[idx].name);

    char trash_name[NAME_LEN + 16];
    unique_trash_name(entries[idx].name, trash_name, sizeof(trash_name));

    char dst[PATH_LEN + NAME_LEN + 2];
    snprintf(dst, sizeof(dst), "%s/%s", trash_files_dir, trash_name);

    /* rename() fails across filesystems (EXDEV) -- e.g. a future mounted
     * drive under a different filesystem than ~/.local/share. Left
     * unhandled deliberately: failing with a clear error and leaving the
     * file untouched is safer than silently falling back to a permanent
     * delete. */
    if (rename(src, dst) != 0) {
        notify_error("Delete failed", strerror(errno));
        return;
    }

    char info_path[PATH_LEN + NAME_LEN + 32];
    snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", trash_info_dir, trash_name);
    FILE *f = fopen(info_path, "w");
    if (f) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);
        /* Loosely inspired by the freedesktop.org Trash spec's .trashinfo
         * format -- not full interop (Path here is absolute, not
         * percent-encoded/relative-to-trash), just enough for nekos-files
         * to restore its own deletions. */
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", src, stamp);
        fclose(f);
    }

    load_dir(cur_dir);
    paint();
}

/* Permanently removes an entry -- used both for "Delete Permanently" on an
 * already-trashed item and for Empty Trash. Also drops the matching
 * .trashinfo if there is one; harmless no-op if there isn't (e.g. called on
 * something outside the trash). */
static void action_confirm_delete_permanent(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count) return;

    char path[PATH_LEN + NAME_LEN + 2];
    snprintf(path, sizeof(path), "%s/%s", cur_dir, entries[idx].name);
    if (remove_recursive(path) != 0) notify_error("Delete failed", strerror(errno));

    char info_path[PATH_LEN + NAME_LEN + 32];
    snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", trash_info_dir, entries[idx].name);
    unlink(info_path);

    load_dir(cur_dir);
    paint();
}

static void action_ctx_delete(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count) return;

    /* The confirm menu's own actionable row names the target directly (menu_show
     * has no separate header row) rather than a generic "Delete" -- one popup,
     * unambiguous either way. Inside the trash itself, delete means gone for
     * good, so it's worded and wired differently there. */
    char delete_label[80];
    menu_item_t confirm_menu[2];
    confirm_menu[0] = (menu_item_t){ "Cancel", NULL, NULL };
    if (in_trash()) {
        snprintf(delete_label, sizeof(delete_label), "Delete \"%.30s\" permanently", entries[idx].name);
        confirm_menu[1] = (menu_item_t){ delete_label, action_confirm_delete_permanent, NULL };
    } else {
        snprintf(delete_label, sizeof(delete_label), "Delete \"%.30s\"", entries[idx].name);
        confirm_menu[1] = (menu_item_t){ delete_label, action_confirm_delete, NULL };
    }
    menu_show(conn, screen, visual, ctx_menu_x, ctx_menu_y, confirm_menu, 2);
}

static void action_open_trash(void *ud) {
    (void)ud;
    load_dir(trash_files_dir);
    paint();
}

static void action_confirm_empty_trash(void *ud) {
    (void)ud;
    int failures = 0;
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, "..") == 0) continue;

        char path[PATH_LEN + NAME_LEN + 2];
        snprintf(path, sizeof(path), "%s/%s", cur_dir, entries[i].name);
        if (remove_recursive(path) != 0) failures++;

        char info_path[PATH_LEN + NAME_LEN + 32];
        snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", trash_info_dir, entries[i].name);
        unlink(info_path);
    }
    if (failures) notify_error("Empty Trash", "Some items couldn't be removed");
    load_dir(cur_dir);
    paint();
}

static void action_empty_trash(void *ud) {
    (void)ud;
    int count = entry_count - (strcmp(cur_dir, "/") != 0 ? 1 : 0); /* exclude ".." */
    if (count <= 0) return; /* nothing to confirm */

    char label[48];
    snprintf(label, sizeof(label), "Empty Trash (%d item%s)", count, count == 1 ? "" : "s");
    menu_item_t confirm_menu[2] = {
        { "Cancel", NULL, NULL },
        { label, action_confirm_empty_trash, NULL },
    };
    menu_show(conn, screen, visual, ctx_menu_x, ctx_menu_y, confirm_menu, 2);
}

/* Restores a trashed entry to the original path recorded in its .trashinfo,
 * recreating any parent directories that no longer exist. Refuses (with a
 * toast) rather than clobbering if something's already at that path. */
static void action_ctx_restore(void *ud) {
    (void)ud;
    int idx = ctx_target_idx;
    if (idx < 0 || idx >= entry_count || strcmp(entries[idx].name, "..") == 0) return;

    char info_path[PATH_LEN + NAME_LEN + 32];
    snprintf(info_path, sizeof(info_path), "%s/%s.trashinfo", trash_info_dir, entries[idx].name);

    char original[PATH_LEN] = "";
    FILE *f = fopen(info_path, "r");
    if (f) {
        char line[PATH_LEN + 8];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Path=", 5) == 0) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                snprintf(original, sizeof(original), "%s", line + 5);
                break;
            }
        }
        fclose(f);
    }
    if (original[0] == '\0') {
        notify_error("Restore failed", "No record of where this came from");
        return;
    }

    char *slash = strrchr(original, '/');
    if (slash && slash != original) {
        char parent[PATH_LEN];
        size_t plen = (size_t)(slash - original);
        if (plen >= sizeof(parent)) plen = sizeof(parent) - 1;
        memcpy(parent, original, plen);
        parent[plen] = '\0';
        mkdir_p(parent);
    }

    struct stat st;
    if (lstat(original, &st) == 0) {
        notify_error("Restore failed", "Something's already there");
        return;
    }

    char trashed_path[PATH_LEN + NAME_LEN + 2];
    snprintf(trashed_path, sizeof(trashed_path), "%s/%s", trash_files_dir, entries[idx].name);
    if (rename(trashed_path, original) != 0) {
        notify_error("Restore failed", strerror(errno));
        return;
    }
    unlink(info_path);

    load_dir(cur_dir);
    paint();
}

/* Right-click: an entry row gets Open/Copy/Cut/Rename/New Folder/Delete;
 * empty space (or the ".." row, not a real target) gets New Folder, Paste
 * (only when the clipboard actually has something), and Open Trash. Inside
 * the trash itself the menu swaps to Restore/Delete Permanently (an entry)
 * or Empty Trash (empty space) -- none of the above would mean anything in
 * there. */
static void handle_context_menu(xcb_button_press_event_t *ev) {
    if (ev->event_y < HEADER_H) return; /* no menu over the header */

    ctx_menu_x = ev->root_x;
    ctx_menu_y = ev->root_y;

    int idx = scroll_off + (ev->event_y - HEADER_H) / ROW_H;
    int trash_mode = in_trash();

    if (idx < 0 || idx >= entry_count || strcmp(entries[idx].name, "..") == 0) {
        if (trash_mode) {
            static const menu_item_t trash_empty_menu[] = {
                { "Empty Trash...", action_empty_trash, NULL },
            };
            menu_show(conn, screen, visual, ctx_menu_x, ctx_menu_y,
                      trash_empty_menu, (int)(sizeof(trash_empty_menu) / sizeof(trash_empty_menu[0])));
        } else {
            menu_item_t empty_menu[3];
            int n = 0;
            empty_menu[n++] = (menu_item_t){ "New Folder", action_ctx_new_folder, NULL };
            if (clip_has_item) empty_menu[n++] = (menu_item_t){ "Paste", action_ctx_paste, NULL };
            empty_menu[n++] = (menu_item_t){ "Open Trash", action_open_trash, NULL };
            menu_show(conn, screen, visual, ctx_menu_x, ctx_menu_y, empty_menu, n);
        }
        return;
    }

    ctx_target_idx = idx;
    sel_index = idx;
    paint();

    if (trash_mode) {
        static const menu_item_t trash_entry_menu[] = {
            { "Restore",               action_ctx_restore, NULL },
            { "Delete Permanently...", action_ctx_delete,   NULL },
        };
        menu_show(conn, screen, visual, ctx_menu_x, ctx_menu_y,
                  trash_entry_menu, (int)(sizeof(trash_entry_menu) / sizeof(trash_entry_menu[0])));
    } else {
        static const menu_item_t entry_menu[] = {
            { "Open",       action_ctx_open,       NULL },
            { "Copy",       action_ctx_copy,       NULL },
            { "Cut",        action_ctx_cut,        NULL },
            { "Rename...",  action_ctx_rename,     NULL },
            { "New Folder", action_ctx_new_folder, NULL },
            { "Delete...",  action_ctx_delete,     NULL },
        };
        menu_show(conn, screen, visual, ctx_menu_x, ctx_menu_y,
                  entry_menu, (int)(sizeof(entry_menu) / sizeof(entry_menu[0])));
    }
}

/* ---- rendering ---------------------------------------------------------- */

/* Draws a small category icon in the ICON_SIZE box at (x, y). Simple Cairo
 * primitives -- recognizable at a glance without needing an icon theme. */
static void draw_icon(cairo_t *cr, double x, double y, cat_t cat) {
    double s = ICON_SIZE;
    switch (cat) {
    case CAT_DIR:
        theme_rgb(cr, THEME_ICON_FOLDER);
        cairo_rectangle(cr, x, y + s * 0.15, s * 0.45, s * 0.2); /* tab */
        cairo_fill(cr);
        cairo_rectangle(cr, x, y + s * 0.3, s, s * 0.55);        /* body */
        cairo_fill(cr);
        break;
    case CAT_IMAGE:
        theme_rgb(cr, THEME_ICON_IMAGE_BG);
        cairo_rectangle(cr, x, y + s * 0.1, s, s * 0.8);
        cairo_fill(cr);
        theme_rgb(cr, THEME_ICON_IMAGE_SUN);                     /* sun */
        cairo_arc(cr, x + s * 0.3, y + s * 0.35, s * 0.12, 0, 2 * 3.14159);
        cairo_fill(cr);
        theme_rgb(cr, THEME_ICON_IMAGE_HILL);                    /* hill */
        cairo_move_to(cr, x, y + s * 0.9);
        cairo_line_to(cr, x + s * 0.5, y + s * 0.5);
        cairo_line_to(cr, x + s, y + s * 0.9);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    case CAT_EXEC:
        theme_rgb(cr, THEME_ICON_EXEC_BG);
        cairo_rectangle(cr, x, y + s * 0.1, s, s * 0.8);
        cairo_fill(cr);
        theme_rgb(cr, THEME_ICON_EXEC_FG);                       /* ">" prompt */
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, x + s * 0.25, y + s * 0.35);
        cairo_line_to(cr, x + s * 0.45, y + s * 0.5);
        cairo_line_to(cr, x + s * 0.25, y + s * 0.65);
        cairo_stroke(cr);
        break;
    case CAT_HTML:
    case CAT_TEXT:
    case CAT_GENERIC:
    default:
        theme_rgb(cr, THEME_ICON_PAGE);                          /* page */
        cairo_rectangle(cr, x + s * 0.12, y + s * 0.05, s * 0.76, s * 0.9);
        cairo_fill(cr);
        if (cat == CAT_HTML) {
            theme_rgb(cr, THEME_ICON_HTML);
            cairo_set_line_width(cr, 1.4);
            cairo_move_to(cr, x + s * 0.42, y + s * 0.35);       /* "<" */
            cairo_line_to(cr, x + s * 0.3, y + s * 0.5);
            cairo_line_to(cr, x + s * 0.42, y + s * 0.65);
            cairo_move_to(cr, x + s * 0.58, y + s * 0.35);       /* ">" */
            cairo_line_to(cr, x + s * 0.7, y + s * 0.5);
            cairo_line_to(cr, x + s * 0.58, y + s * 0.65);
            cairo_stroke(cr);
        } else {
            theme_rgb(cr, THEME_ICON_TEXT_LINES);                /* text lines */
            for (int i = 0; i < 3; i++) {
                cairo_rectangle(cr, x + s * 0.24, y + s * (0.3 + i * 0.18), s * 0.5, s * 0.06);
                cairo_fill(cr);
            }
        }
        break;
    }
}

static int visible_rows(void) {
    int rows = (win_h - HEADER_H) / ROW_H;
    return rows < 0 ? 0 : rows;
}

/* Loads an image row's thumbnail on first paint (visible rows only). */
static void ensure_thumb(entry_t *en) {
    if (en->thumb_tried || en->cat != CAT_IMAGE) return;
    en->thumb_tried = 1;
    char full[PATH_LEN + NAME_LEN + 2];
    snprintf(full, sizeof(full), "%s/%s", cur_dir, en->name);
    en->thumb = image_load_scaled(full, ICON_SIZE, ICON_SIZE, /*cover=*/1);
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    theme_rgb(cr, THEME_BG);
    cairo_paint(cr);

    /* Header: up button + clickable path segments. */
    theme_rgba(cr, THEME_ACCENT, hover_up ? 0.30 : 0.12);
    theme_rounded_rect(cr, UP_X, (HEADER_H - UP_SIZE) / 2.0, UP_SIZE, UP_SIZE, THEME_RADIUS);
    cairo_fill(cr);
    theme_rgb(cr, THEME_FG);
    cairo_set_line_width(cr, 1.6);
    {
        double cx = UP_X + UP_SIZE / 2.0, cy = HEADER_H / 2.0;
        cairo_move_to(cr, cx, cy + 5);
        cairo_line_to(cr, cx, cy - 5);
        cairo_move_to(cr, cx - 4, cy - 1);
        cairo_line_to(cr, cx, cy - 5);
        cairo_line_to(cr, cx + 4, cy - 1);
        cairo_stroke(cr);
    }

    /* Path segments: "/" then each component, each a click target that
     * navigates to that prefix of cur_dir. */
    seg_count = 0;
    theme_font(cr, THEME_FONT_MD, 1);
    double px = PATH_X;
    double baseline = theme_baseline(0, HEADER_H, THEME_FONT_MD);
    cairo_save(cr);
    cairo_rectangle(cr, PATH_X, 0, win_w - PATH_X - 8, HEADER_H);
    cairo_clip(cr);
    {
        cairo_text_extents_t ext;
        /* Root segment. */
        seg_x[seg_count] = px;
        theme_rgba(cr, THEME_FG, hover_seg == seg_count ? 1.0 : 0.7);
        cairo_move_to(cr, px, baseline);
        cairo_show_text(cr, "/");
        cairo_text_extents(cr, "/", &ext);
        px += ext.x_advance + 2;
        seg_x2[seg_count] = px;
        seg_len[seg_count] = 1;
        seg_count++;

        const char *p = cur_dir + 1;
        while (*p && seg_count < MAX_SEGS) {
            const char *slash = strchr(p, '/');
            size_t n = slash ? (size_t)(slash - p) : strlen(p);
            char comp[NAME_LEN];
            if (n >= sizeof(comp)) n = sizeof(comp) - 1;
            memcpy(comp, p, n);
            comp[n] = '\0';

            seg_x[seg_count] = px;
            theme_rgba(cr, THEME_FG, hover_seg == seg_count ? 1.0 : 0.7);
            cairo_move_to(cr, px, baseline);
            cairo_show_text(cr, comp);
            cairo_text_extents(cr, comp, &ext);
            px += ext.x_advance;
            seg_x2[seg_count] = px;
            seg_len[seg_count] = (int)((p - cur_dir) + n);
            seg_count++;

            if (!slash) break;
            theme_rgba(cr, THEME_ACCENT, 0.6);
            cairo_move_to(cr, px + 3, baseline);
            cairo_show_text(cr, "/");
            cairo_text_extents(cr, "/", &ext);
            px += ext.x_advance + 6;
            p = slash + 1;
        }
    }
    cairo_restore(cr);

    theme_rgba(cr, THEME_ACCENT, THEME_BORDER_ALPHA);
    cairo_rectangle(cr, 0, HEADER_H - 1, win_w, 1);
    cairo_fill(cr);

    int rows = visible_rows();
    theme_font(cr, THEME_FONT_MD, 0);

    for (int r = 0; r < rows; r++) {
        int idx = scroll_off + r;
        if (idx >= entry_count) break;
        entry_t *en = &entries[idx];
        double y = HEADER_H + r * ROW_H;

        if (idx == sel_index) {
            theme_rgba(cr, THEME_ACCENT, 0.18);
            theme_rounded_rect(cr, 3, y + 1, win_w - 6, ROW_H - 2, THEME_RADIUS);
            cairo_fill(cr);
        } else if (idx == hover_row) {
            theme_rgba(cr, THEME_ACCENT, THEME_HOVER_ALPHA);
            theme_rounded_rect(cr, 3, y + 1, win_w - 6, ROW_H - 2, THEME_RADIUS);
            cairo_fill(cr);
        }

        ensure_thumb(en);
        if (en->thumb) {
            double ix = ICON_X, iy = y + (ROW_H - ICON_SIZE) / 2.0;
            cairo_save(cr);
            theme_rounded_rect(cr, ix, iy, ICON_SIZE, ICON_SIZE, 3.0);
            cairo_clip(cr);
            cairo_set_source_surface(cr, en->thumb, ix, iy);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            draw_icon(cr, ICON_X, y + (ROW_H - ICON_SIZE) / 2.0, en->cat);
        }

        theme_font(cr, THEME_FONT_MD, 0);
        theme_rgb(cr, THEME_FG);
        theme_text_ellipsized(cr, TEXT_X, theme_baseline(y, ROW_H, THEME_FONT_MD),
                               win_w - TEXT_X - 12, en->name);
    }

    /* Empty-folder note (the ".." up-entry doesn't count as contents). */
    if (entry_count - (strcmp(cur_dir, "/") != 0 ? 1 : 0) <= 0) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.45);
        cairo_move_to(cr, TEXT_X, HEADER_H + entry_count * ROW_H + 26);
        cairo_show_text(cr, "nothing in here but dust bunnies");
    }

    /* Scroll hint: a thin bar on the right when the list overflows. */
    if (entry_count > rows && rows > 0) {
        double track_h = win_h - HEADER_H;
        double thumb_h = track_h * ((double)rows / entry_count);
        double thumb_y = HEADER_H + track_h * ((double)scroll_off / entry_count);
        theme_rgba(cr, THEME_ACCENT, 0.35);
        cairo_rectangle(cr, win_w - 4, thumb_y, 3, thumb_h);
        cairo_fill(cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

static void clamp_scroll(void) {
    int max_off = entry_count - visible_rows();
    if (max_off < 0) max_off = 0;
    if (scroll_off > max_off) scroll_off = max_off;
    if (scroll_off < 0) scroll_off = 0;
}

static void go_up(void) {
    char up[PATH_LEN + 4];
    snprintf(up, sizeof(up), "%s/..", cur_dir);
    load_dir(up);
    paint();
}

/* Navigates to the cur_dir prefix of `len` characters (a path-segment click). */
static void go_to_prefix(int len) {
    char target[PATH_LEN];
    if (len <= 0 || len >= (int)sizeof(target)) return;
    memcpy(target, cur_dir, (size_t)len);
    target[len] = '\0';
    load_dir(target);
    paint();
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    /* Wheel up/down. */
    if (ev->detail == 4) { scroll_off -= SCROLL_STEP; clamp_scroll(); paint(); return; }
    if (ev->detail == 5) { scroll_off += SCROLL_STEP; clamp_scroll(); paint(); return; }
    if (ev->detail == 3) { handle_context_menu(ev); return; }
    if (ev->detail != 1) return;
    if (ev->event_y < HEADER_H) {
        if (ev->event_x >= UP_X && ev->event_x < UP_X + UP_SIZE) { go_up(); return; }
        for (int i = 0; i < seg_count; i++) {
            if (ev->event_x >= seg_x[i] && ev->event_x < seg_x2[i]) {
                go_to_prefix(seg_len[i]);
                return;
            }
        }
        return;
    }

    int idx = scroll_off + (ev->event_y - HEADER_H) / ROW_H;
    if (idx < 0 || idx >= entry_count) { sel_index = -1; paint(); return; }

    sel_index = idx;

    int is_double = (last_click_index == idx &&
                     (ev->time - last_click_time) < DOUBLE_CLICK_MS);
    last_click_time = ev->time;
    last_click_index = idx;

    if (is_double) {
        last_click_index = -1; /* consume, don't chain into a triple-click */
        activate(idx);         /* may navigate (reloads entries) */
    }
    paint();
}

int main(int argc, char **argv) {
    signal(SIGCHLD, SIG_IGN); /* reap spawned nekos-open children automatically */

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-files: could not connect to X server\n");
        return 1;
    }

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    keysyms = xcb_key_symbols_alloc(conn);
    resolve_open_tool();
    resolve_notify_tool();
    init_trash_dirs();

    /* Start dir: argv[1] if given (how nekos-open launches us for a folder),
     * else $HOME, else "/". */
    const char *start = (argc > 1) ? argv[1] : getenv("HOME");
    load_dir(start ? start : "/");

    net_wm_name = intern("_NET_WM_NAME");
    utf8_string = intern("UTF8_STRING");

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, INIT_WIDTH, INIT_HEIGHT, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                       mask, values);

    static const char title[] = "Files";
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name,
                         utf8_string, 8, (uint32_t)(sizeof(title) - 1), title);
    /* Legacy WM_NAME too, so ICCCM-era tools (xwininfo -name, xdotool) can
     * find the window by title as well as the taskbar's _NET_WM_NAME read. */
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 8, (uint32_t)(sizeof(title) - 1), title);

    xcb_map_window(conn, win);
    xcb_flush(conn);

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
                clamp_scroll();
                paint();
            }
            break;
        }
        case XCB_BUTTON_PRESS:
            handle_button_press((xcb_button_press_event_t *)event);
            break;
        case XCB_MOTION_NOTIFY: {
            xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
            int row = -1, up = 0, seg = -1;
            if (me->event_y < HEADER_H) {
                if (me->event_x >= UP_X && me->event_x < UP_X + UP_SIZE) up = 1;
                else {
                    for (int i = 0; i < seg_count; i++) {
                        if (me->event_x >= seg_x[i] && me->event_x < seg_x2[i]) { seg = i; break; }
                    }
                }
            } else {
                int idx = scroll_off + (me->event_y - HEADER_H) / ROW_H;
                if (idx >= 0 && idx < entry_count) row = idx;
            }
            if (row != hover_row || up != hover_up || seg != hover_seg) {
                hover_row = row;
                hover_up = up;
                hover_seg = seg;
                paint();
            }
            break;
        }
        case XCB_LEAVE_NOTIFY:
            if (hover_row != -1 || hover_up || hover_seg != -1) {
                hover_row = -1;
                hover_up = 0;
                hover_seg = -1;
                paint();
            }
            break;
        default:
            break;
        }
        free(event);
    }

    xcb_key_symbols_free(keysyms);
    xcb_disconnect(conn);
    return 0;
}
