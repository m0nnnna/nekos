/*
 * nekos-shot: GUI front end for NekoShot (a separate project, source at
 * /opt/nekoshot -- see provision/install-nekoshot.sh), the neko-powered
 * system-snapshot/backup tool. Mirrors nekoshot's own interactive terminal
 * menu (menu.py) one-to-one: Back Up / Schedule / Jobs / Status / Snapshots /
 * Restore, each building the equivalent `nekoshot ...` command and either
 * running it (streamed into a log strip, exactly like nekos-software's
 * xbps-install/xbps-remove pattern) or reading its `--json` output straight
 * into the page.
 *
 * nekoshot.py already owns every real backup/restore/schedule/query
 * operation; this GUI never reimplements any of that, it only builds
 * commands and parses their `--json` output. No JSON library is linked
 * (nothing in this codebase links one) -- nekoshot's --json shapes are small
 * and entirely our own, so a tiny hand-rolled field extractor is enough.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include "theme.h"

#define INIT_W       860
#define INIT_H       580
#define SIDEBAR_W    150
#define ROW_H        30
#define LOG_H        118
#define LOG_LINES    6
#define LOG_LINE_LEN 200
#define PATH_LEN     512
#define NEKOSHOT_BIN "/opt/nekoshot/nekoshot.py"

/* ---- sidebar pages --------------------------------------------------- */

typedef enum { PAGE_BACKUP, PAGE_SCHEDULE, PAGE_JOBS, PAGE_STATUS, PAGE_SNAPSHOTS, PAGE_RESTORE } page_t;
static const char *const page_names[] = { "Back Up", "Schedule", "Jobs", "Status", "Snapshots", "Restore" };
#define PAGE_COUNT 6

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_visualtype_t *visual;
static xcb_window_t win;
static xcb_key_symbols_t *keysyms;

static int win_w = INIT_W;
static int win_h = INIT_H;
static page_t page = PAGE_BACKUP;
static int hover_page = -1;

static char notify_tool[PATH_LEN + 32];

/* ---- shared backup-kind widget state (Back Up + Schedule pages) -------- */

typedef enum { KIND_SYSTEM, KIND_DOCKER, KIND_SQL } kind_t;
static const char *const kind_names[] = { "The whole system", "Docker only", "Databases only" };
#define KIND_COUNT 3

static kind_t sel_kind = KIND_SYSTEM;
static int docker_pause = 0;
static int docker_prune = 0; /* 0=none, 1=dangling, 2=all */
static char sql_sqlite[256] = "";
static char location[PATH_LEN] = "";
static int run_headless = 0;

/* ---- schedule page state ------------------------------------------------ */

typedef struct {
    const char *key;
    const char *label;
    const char *cron;
} freq_preset_t;
static const freq_preset_t freq_presets[] = {
    { "hourly", "Every hour", "0 * * * *" },
    { "daily", "Every day at 02:00", "0 2 * * *" },
    { "nightly-except-sunday", "Mon-Sat at 02:00", "0 2 * * 1-6" },
    { "weekly", "Every Sunday at 03:00", "0 3 * * 0" },
    { "monthly", "1st of each month at 04:00", "0 4 1 * *" },
    { "custom", "Custom cron expression", NULL },
};
#define FREQ_COUNT 6
static int sel_freq = 1; /* daily */
static char custom_cron[64] = "";
static char job_name[128] = "";

/* ---- jobs page state ----------------------------------------------------- */

typedef struct {
    char name[64];
    char schedule[32];
    char description[64];
    char command[400];
} job_t;
static job_t jobs[64];
static int job_count = 0;
static int cron_is_running = 0;
static char cron_file_path[256] = "";
static int hover_job_remove = -1;

/* ---- status page state ---------------------------------------------------- */

typedef struct {
    char operation[16];
    char mode[16];
    char state[16];
    char hostname[80];
    long pid;
    long elapsed_seconds;
    long overall_percent;
    int has_phase;
    long phase_index, phase_count;
    char phase_detail[160];
    double phase_percent; /* -1 = unknown */
    long phase_current, phase_total;
    int phase_percent_known, phase_total_known;
    double phase_eta_seconds; /* -1 = unknown */
    char archive[300];
    char error[300];
} status_rec_t;
static status_rec_t status_recs[16];
static int status_count = 0;
static struct timespec last_status_poll;
static int status_loaded_once = 0;

/* ---- snapshots page state --------------------------------------------- */

typedef struct {
    char name[160];
    char path[PATH_LEN];
    char created[32];
    char hostname[80];
    char mode[16];
    char size_human[32];
} snap_t;
static snap_t snaps[256];
static int snap_count = 0;
static int snap_kind = 3; /* 0=system,1=docker,2=sql,3=everything */
static int hover_snap_row = -1;

/* ---- restore page state --------------------------------------------- */

static char restore_path[PATH_LEN] = "";
static int restore_armed = 0;

/* ---- text-field focus (one at a time, append/backspace-at-end editing) --- */

static char *focused_field = NULL;
static size_t focused_field_cap = 0;

/* ---- async operation (child process streaming into the log strip) -------- */

static pid_t op_pid = 0;
static int op_fd = -1;
static char op_label[64] = "";
static char log_lines[LOG_LINES][LOG_LINE_LEN];
static int log_count = 0;
static char log_partial[LOG_LINE_LEN];
static int have_log = 0;
static char op_status[200] = "";

/* ---- helpers --------------------------------------------------------- */

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

static void send_toast(const char *title, const char *body) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(notify_tool, "nekos-notify", title, body, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Appends `s` single-quoted (shell-safe against a `sh -c` command line) to
 * `cmd`, followed by a space. */
static void cmd_append_quoted(char *cmd, size_t cmd_size, const char *s) {
    size_t len = strlen(cmd);
    if (len + 1 >= cmd_size) return;
    cmd[len++] = '\'';
    for (const char *c = s; *c && len + 6 < cmd_size; c++) {
        if (*c == '\'') { memcpy(cmd + len, "'\\''", 4); len += 4; }
        else cmd[len++] = *c;
    }
    if (len + 2 < cmd_size) { cmd[len++] = '\''; cmd[len++] = ' '; }
    cmd[len] = '\0';
}

static void cmd_append_raw(char *cmd, size_t cmd_size, const char *s) {
    size_t len = strlen(cmd);
    snprintf(cmd + len, cmd_size - len, "%s ", s);
}

/* Runs `cmd` (sh -c) synchronously, capturing combined stdout into `out`
 * (truncated to fit). Used for fast, read-only queries (status/list/schedule
 * list) -- these hit small JSON files or directory scans, never anything
 * slow enough to need the async streaming path. */
static int run_capture(const char *cmd, char *out, size_t out_size) {
    FILE *p = popen(cmd, "r");
    if (!p) { out[0] = '\0'; return -1; }
    size_t total = fread(out, 1, out_size - 1, p);
    out[total] = '\0';
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* ---- minimal JSON field extraction ---------------------------------------
 * nekoshot's --json output is small, shallow, and entirely produced by our
 * own Python code -- a full parser isn't needed, just enough to pull known
 * fields out of a bounded [start,end) object/array span. Every shape this
 * file actually reads has no field-name collision between an outer object
 * and any object nested inside it, so a plain textual search for `"key":`
 * within the given span (not recursing into further-nested spans on its
 * own) is sufficient -- callers narrow the span first via json_span() when
 * they need a nested object/array's own fields. */

static const char *json_find(const char *start, const char *end, const char *key) {
    char pat[80];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0) return NULL;
    for (const char *p = start; p + n < end; p++) {
        if (memcmp(p, pat, (size_t)n) == 0) {
            p += n;
            while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
            return p;
        }
    }
    return NULL;
}

static int json_string(const char *start, const char *end, const char *key, char *out, size_t out_size) {
    const char *v = json_find(start, end, key);
    out[0] = '\0';
    if (!v || v >= end || *v != '"') return 0;
    v++;
    size_t oi = 0;
    while (v < end && *v != '"' && oi < out_size - 1) {
        if (*v == '\\' && v + 1 < end) {
            v++;
            char c = *v;
            out[oi++] = (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
        } else {
            out[oi++] = *v;
        }
        v++;
    }
    out[oi] = '\0';
    return 1;
}

static int json_int(const char *start, const char *end, const char *key, long *out) {
    const char *v = json_find(start, end, key);
    if (!v || v >= end || strncmp(v, "null", 4) == 0) return 0;
    char *ep;
    long val = strtol(v, &ep, 10);
    if (ep == v) return 0;
    *out = val;
    return 1;
}

static int json_double(const char *start, const char *end, const char *key, double *out) {
    const char *v = json_find(start, end, key);
    if (!v || v >= end || strncmp(v, "null", 4) == 0) return 0;
    char *ep;
    double val = strtod(v, &ep);
    if (ep == v) return 0;
    *out = val;
    return 1;
}

static int json_bool(const char *start, const char *end, const char *key) {
    const char *v = json_find(start, end, key);
    if (!v || v >= end) return 0;
    return strncmp(v, "true", 4) == 0;
}

/* Bounds of the object/array value for `"key":`, found by brace/bracket
 * depth-matching from its opening token. *vstart lands on '{'/'[', *vend one
 * past the matching close. Returns 0 (leaving both output params untouched)
 * if the key is absent, its value is `null`, or it's not an object/array. */
static int json_span(const char *start, const char *end, const char *key,
                      const char **vstart, const char **vend) {
    const char *v = json_find(start, end, key);
    if (!v || v >= end || (*v != '{' && *v != '[')) return 0;
    char open = *v, close = (open == '{') ? '}' : ']';
    int depth = 0;
    const char *p = v;
    for (; p < end; p++) {
        if (*p == open) depth++;
        else if (*p == close) { depth--; if (depth == 0) { p++; break; } }
    }
    *vstart = v;
    *vend = p;
    return 1;
}

/* Iterates the top-level object elements of an array span (as returned by
 * json_span). Call with *cursor == arr_start initially; each call advances
 * *cursor past the element it returns. Returns 0 once exhausted. */
static int json_array_next(const char **cursor, const char *arr_end,
                            const char **elem_start, const char **elem_end) {
    const char *p = *cursor;
    while (p < arr_end && *p != '{') {
        if (*p == ']') return 0;
        p++;
    }
    if (p >= arr_end) return 0;
    int depth = 0;
    const char *start = p;
    for (; p < arr_end; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
    }
    *elem_start = start;
    *elem_end = p;
    *cursor = p;
    return 1;
}

/* ---- data loading (nekoshot --json queries) ------------------------------ */

static char json_buf[65536];

static void parse_status_record(const char *s, const char *e, status_rec_t *r) {
    memset(r, 0, sizeof(*r));
    json_string(s, e, "operation", r->operation, sizeof(r->operation));
    json_string(s, e, "mode", r->mode, sizeof(r->mode));
    json_string(s, e, "state", r->state, sizeof(r->state));
    json_string(s, e, "hostname", r->hostname, sizeof(r->hostname));
    json_int(s, e, "pid", &r->pid);
    json_int(s, e, "elapsed_seconds", &r->elapsed_seconds);
    json_int(s, e, "overall_percent", &r->overall_percent);
    json_string(s, e, "archive", r->archive, sizeof(r->archive));
    json_string(s, e, "error", r->error, sizeof(r->error));

    const char *ps, *pe;
    if (json_span(s, e, "phase", &ps, &pe)) {
        r->has_phase = 1;
        json_int(ps, pe, "index", &r->phase_index);
        json_int(ps, pe, "count", &r->phase_count);
        if (!json_string(ps, pe, "detail", r->phase_detail, sizeof(r->phase_detail)))
            json_string(ps, pe, "description", r->phase_detail, sizeof(r->phase_detail));
        double pct;
        r->phase_percent_known = json_double(ps, pe, "percent", &pct);
        r->phase_percent = r->phase_percent_known ? pct : 0.0;
        r->phase_total_known = json_int(ps, pe, "total", &r->phase_total);
        json_int(ps, pe, "current", &r->phase_current);
        double eta;
        r->phase_eta_seconds = json_double(ps, pe, "eta_seconds", &eta) ? eta : -1.0;
    }
}

static void load_status(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "python3 %s status --all --json 2>/dev/null", NEKOSHOT_BIN);
    run_capture(cmd, json_buf, sizeof(json_buf));

    status_count = 0;
    const char *cursor = json_buf, *end = json_buf + strlen(json_buf);
    /* Top-level value is itself the array -- treat the whole buffer as the span. */
    const char *arr_start = strchr(json_buf, '[');
    if (!arr_start) { clock_gettime(CLOCK_MONOTONIC, &last_status_poll); status_loaded_once = 1; return; }
    cursor = arr_start;
    const char *es, *ee;
    while (status_count < 16 && json_array_next(&cursor, end, &es, &ee)) {
        parse_status_record(es, ee, &status_recs[status_count++]);
    }
    clock_gettime(CLOCK_MONOTONIC, &last_status_poll);
    status_loaded_once = 1;
}

static const char *kind_dir(kind_t k) {
    return k == KIND_SYSTEM ? "system" : k == KIND_DOCKER ? "docker" : "sql";
}

static void load_jobs(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "python3 %s schedule list --json 2>/dev/null", NEKOSHOT_BIN);
    run_capture(cmd, json_buf, sizeof(json_buf));

    job_count = 0;
    cron_is_running = 0;
    cron_file_path[0] = '\0';

    const char *start = json_buf, *end = json_buf + strlen(json_buf);
    json_string(start, end, "cron_file", cron_file_path, sizeof(cron_file_path));
    cron_is_running = json_bool(start, end, "cron_running");

    const char *as, *ae;
    if (json_span(start, end, "jobs", &as, &ae)) {
        const char *cursor = as, *es, *ee;
        while (job_count < 64 && json_array_next(&cursor, ae, &es, &ee)) {
            job_t *j = &jobs[job_count++];
            json_string(es, ee, "name", j->name, sizeof(j->name));
            json_string(es, ee, "schedule", j->schedule, sizeof(j->schedule));
            json_string(es, ee, "description", j->description, sizeof(j->description));
            json_string(es, ee, "command", j->command, sizeof(j->command));
        }
    }
}

static void load_snapshots(void) {
    char path[PATH_LEN];
    if (snap_kind == 3) {
        snprintf(path, sizeof(path), "/backup/nekoshot");
    } else {
        snprintf(path, sizeof(path), "/backup/nekoshot/%s",
                 snap_kind == 0 ? "system" : snap_kind == 1 ? "docker" : "sql");
    }

    char cmd[PATH_LEN + 80];
    snprintf(cmd, sizeof(cmd), "python3 %s list --path ", NEKOSHOT_BIN);
    cmd_append_quoted(cmd, sizeof(cmd), path);
    cmd_append_raw(cmd, sizeof(cmd), "--json 2>/dev/null");
    run_capture(cmd, json_buf, sizeof(json_buf));

    snap_count = 0;
    const char *arr_start = strchr(json_buf, '[');
    if (!arr_start) return;
    const char *end = json_buf + strlen(json_buf);
    const char *cursor = arr_start, *es, *ee;
    while (snap_count < 256 && json_array_next(&cursor, end, &es, &ee)) {
        snap_t *sn = &snaps[snap_count++];
        json_string(es, ee, "name", sn->name, sizeof(sn->name));
        json_string(es, ee, "path", sn->path, sizeof(sn->path));
        json_string(es, ee, "created", sn->created, sizeof(sn->created));
        json_string(es, ee, "hostname", sn->hostname, sizeof(sn->hostname));
        json_string(es, ee, "mode", sn->mode, sizeof(sn->mode));
        json_string(es, ee, "size_human", sn->size_human, sizeof(sn->size_human));
    }
}

/* ---- command building (mirrors menu.py's _backup_kind_menu) -------------- */

static void build_backup_argv(char *cmd, size_t cmd_size, int headless) {
    cmd[0] = '\0';
    cmd_append_raw(cmd, cmd_size, "python3");
    cmd_append_quoted(cmd, cmd_size, NEKOSHOT_BIN);

    if (sel_kind == KIND_SQL) {
        cmd_append_raw(cmd, cmd_size, "sql");
        if (location[0]) { cmd_append_raw(cmd, cmd_size, "--output"); cmd_append_quoted(cmd, cmd_size, location); }
        char paths[256];
        snprintf(paths, sizeof(paths), "%s", sql_sqlite);
        char *save = NULL;
        for (char *tok = strtok_r(paths, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ') tok++;
            if (*tok) { cmd_append_raw(cmd, cmd_size, "--sqlite"); cmd_append_quoted(cmd, cmd_size, tok); }
        }
    } else {
        cmd_append_raw(cmd, cmd_size, "capture");
        if (location[0]) { cmd_append_raw(cmd, cmd_size, "--output"); cmd_append_quoted(cmd, cmd_size, location); }
        if (sel_kind == KIND_DOCKER) {
            cmd_append_raw(cmd, cmd_size, "--docker-only");
            if (docker_pause) cmd_append_raw(cmd, cmd_size, "--pause-docker");
            if (docker_prune == 1) cmd_append_raw(cmd, cmd_size, "--prune-images dangling");
            else if (docker_prune == 2) cmd_append_raw(cmd, cmd_size, "--prune-images all");
        }
    }

    if (headless) cmd_append_raw(cmd, cmd_size, "--headless");
}

/* ---- async operation plumbing (identical shape to nekos-software) -------- */

static void paint(void);

static void log_append_chunk(const char *buf, ssize_t n) {
    for (ssize_t i = 0; i < n; i++) {
        char c = buf[i];
        size_t l = strlen(log_partial);
        if (c == '\n' || l >= LOG_LINE_LEN - 1) {
            if (c != '\n' && l < LOG_LINE_LEN - 1) {
                log_partial[l] = c;
                log_partial[l + 1] = '\0';
            }
            if (log_partial[0]) {
                if (log_count == LOG_LINES) {
                    memmove(log_lines[0], log_lines[1], (LOG_LINES - 1) * LOG_LINE_LEN);
                    log_count--;
                }
                snprintf(log_lines[log_count++], LOG_LINE_LEN, "%s", log_partial);
                log_partial[0] = '\0';
            }
        } else if (c != '\r') {
            log_partial[l] = c;
            log_partial[l + 1] = '\0';
        }
    }
}

static void start_op(const char *label, const char *cmd) {
    if (op_pid > 0) return;

    int fds[2];
    if (pipe(fds) != 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    op_pid = pid;
    op_fd = fds[0];
    snprintf(op_label, sizeof(op_label), "%s", label);
    log_count = 0;
    log_partial[0] = '\0';
    have_log = 1;
    snprintf(op_status, sizeof(op_status), "%s...", label);
}

static void finish_op(void) {
    int st = 0;
    waitpid(op_pid, &st, 0);
    close(op_fd);
    op_pid = 0;
    op_fd = -1;

    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    snprintf(op_status, sizeof(op_status), "%s %s", op_label, ok ? "done" : "FAILED (see log)");

    char body[128];
    snprintf(body, sizeof(body), "%s %s", op_label, ok ? "finished." : "failed.");
    send_toast("NekoShot", body);

    if (page == PAGE_JOBS) load_jobs();
    if (page == PAGE_SNAPSHOTS) load_snapshots();
    if (page == PAGE_STATUS) load_status();
    if (page == PAGE_RESTORE) restore_armed = 0;
    paint();
}

/* ---- page actions ---------------------------------------------------- */

static void run_backup(void) {
    if (op_pid > 0) return;
    char cmd[1024];
    build_backup_argv(cmd, sizeof(cmd), run_headless);
    char label[64];
    snprintf(label, sizeof(label), "%s%s",
             run_headless ? "Starting " : "Running ",
             sel_kind == KIND_SQL ? "database backup" : sel_kind == KIND_DOCKER ? "Docker backup" : "system backup");
    start_op(label, cmd);
    if (run_headless) page = PAGE_STATUS;
    paint();
}

static void save_schedule(void) {
    if (op_pid > 0) return;

    const char *cron = freq_presets[sel_freq].cron ? freq_presets[sel_freq].cron : custom_cron;
    if (!cron[0]) return;

    char name[192];
    if (job_name[0]) snprintf(name, sizeof(name), "%s", job_name);
    else snprintf(name, sizeof(name), "%s-%s", kind_dir(sel_kind), freq_presets[sel_freq].key);

    char argv_part[900];
    build_backup_argv(argv_part, sizeof(argv_part), 0);
    /* build_backup_argv already includes "python3 <bin>"; schedule add wants
     * only the nekoshot subcommand + args after that, matching what
     * schedule.nekoshot_command() prefixes back on when the job actually
     * runs from cron. */
    char *skip = strstr(argv_part, NEKOSHOT_BIN);
    const char *sub_argv = skip ? skip + strlen(NEKOSHOT_BIN) + 1 : argv_part;

    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "python3 %s schedule add ", NEKOSHOT_BIN);
    cmd_append_quoted(cmd, sizeof(cmd), name);
    cmd_append_quoted(cmd, sizeof(cmd), cron);
    cmd_append_raw(cmd, sizeof(cmd), sub_argv);

    char label[sizeof(name) + 16];
    snprintf(label, sizeof(label), "Scheduling \"%s\"", name);
    start_op(label, cmd);
    paint();
}

static void remove_job(const char *name) {
    if (op_pid > 0) return;
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "python3 %s schedule remove ", NEKOSHOT_BIN);
    cmd_append_quoted(cmd, sizeof(cmd), name);
    char label[80];
    snprintf(label, sizeof(label), "Removing \"%s\"", name);
    start_op(label, cmd);
    paint();
}

static void run_restore(void) {
    if (op_pid > 0 || !restore_path[0]) return;
    char cmd[PATH_LEN + 80];
    snprintf(cmd, sizeof(cmd), "python3 %s restore --image ", NEKOSHOT_BIN);
    cmd_append_quoted(cmd, sizeof(cmd), restore_path);
    start_op("Restoring", cmd);
    restore_armed = 0;
    paint();
}

static void reload_page(void) {
    switch (page) {
    case PAGE_JOBS: load_jobs(); break;
    case PAGE_STATUS: load_status(); break;
    case PAGE_SNAPSHOTS: load_snapshots(); break;
    default: break;
    }
}

/* ---- painting -------------------------------------------------------- */

static int content_x(void) { return SIDEBAR_W + 24; }
static int content_top(void) { return 20; }
static int content_bottom(void) { return win_h - (have_log ? LOG_H : 0); }

static void draw_button(cairo_t *cr, double x, double y, double w, double h,
                         const char *text, unsigned color, int hovered, int enabled) {
    theme_rgba(cr, color, enabled ? (hovered ? 0.35 : 0.18) : 0.08);
    theme_rounded_rect(cr, x, y, w, h, THEME_RADIUS);
    cairo_fill(cr);
    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, enabled ? 1.0 : 0.35);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    cairo_move_to(cr, x + (w - ext.x_advance) / 2.0, theme_baseline(y, h, THEME_FONT_SM));
    cairo_show_text(cr, text);
}

/* A single-line text field; draws a focus ring when it's the focused_field. */
static void draw_field(cairo_t *cr, double x, double y, double w, double h,
                        const char *value, const char *placeholder, int focused) {
    theme_rgba(cr, THEME_ACCENT, focused ? 0.16 : 0.10);
    theme_rounded_rect(cr, x, y, w, h, THEME_RADIUS);
    cairo_fill(cr);
    if (focused) {
        theme_rgba(cr, THEME_ACCENT, 0.5);
        cairo_set_line_width(cr, 1.0);
        theme_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, THEME_RADIUS);
        cairo_stroke(cr);
    }
    theme_font(cr, THEME_FONT_SM, 0);
    if (value[0]) {
        theme_rgb(cr, THEME_FG);
        theme_text_ellipsized(cr, x + 8, theme_baseline(y, h, THEME_FONT_SM), w - 16, value);
        if (focused) {
            cairo_text_extents_t ext;
            cairo_text_extents(cr, value, &ext);
            double cx = x + 8 + (ext.x_advance < w - 16 ? ext.x_advance : w - 16);
            theme_rgb(cr, THEME_ACCENT);
            cairo_rectangle(cr, cx + 2, y + 5, 1.5, h - 10);
            cairo_fill(cr);
        }
    } else {
        theme_rgba(cr, THEME_FG, 0.35);
        theme_text_ellipsized(cr, x + 8, theme_baseline(y, h, THEME_FONT_SM), w - 16, placeholder);
    }
}

#define KIND_ROW_W 220

static void paint_kind_selector(cairo_t *cr, double x, double y) {
    theme_font(cr, THEME_FONT_MD, 0);
    theme_rgb(cr, THEME_FG);
    cairo_move_to(cr, x, theme_baseline(y - 22, 22, THEME_FONT_MD));
    cairo_show_text(cr, "What would you like to back up?");

    for (int i = 0; i < KIND_COUNT; i++) {
        double ry = y + i * ROW_H;
        if ((int)sel_kind == i) {
            theme_rgba(cr, THEME_ACCENT, 0.18);
            theme_rounded_rect(cr, x, ry, KIND_ROW_W, ROW_H - 4, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgba(cr, THEME_FG, (int)sel_kind == i ? 1.0 : 0.75);
        theme_font(cr, THEME_FONT_SM, 0);
        cairo_move_to(cr, x + 12, theme_baseline(ry, ROW_H - 4, THEME_FONT_SM));
        cairo_show_text(cr, kind_names[i]);
    }
}

static void paint_backup(cairo_t *cr) {
    double x = content_x();
    double y = content_top();

    paint_kind_selector(cr, x, y + 30);
    double sub_y = y + 30 + KIND_COUNT * ROW_H + 20;

    if (sel_kind == KIND_DOCKER) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, docker_pause ? 1.0 : 0.7);
        theme_rgba(cr, THEME_ACCENT, docker_pause ? 0.9 : 0.3);
        theme_rounded_rect(cr, x, sub_y, 16, 16, 4.0);
        docker_pause ? cairo_fill(cr) : cairo_stroke(cr);
        theme_rgb(cr, THEME_FG);
        cairo_move_to(cr, x + 24, theme_baseline(sub_y, 16, THEME_FONT_SM));
        cairo_show_text(cr, "Pause containers while copying volumes");
        sub_y += 30;

        const char *prune_labels[] = { "Keep every image", "Prune dangling images (safe)", "Prune ALL unused images" };
        theme_rgba(cr, THEME_FG, 0.8);
        cairo_move_to(cr, x, theme_baseline(sub_y - 4, 16, THEME_FONT_SM));
        cairo_show_text(cr, "Prune unused images first?");
        sub_y += 20;
        for (int i = 0; i < 3; i++) {
            if (docker_prune == i) {
                theme_rgba(cr, THEME_ACCENT, 0.18);
                theme_rounded_rect(cr, x, sub_y, 280, 22, THEME_RADIUS);
                cairo_fill(cr);
            }
            theme_rgba(cr, i == 2 ? THEME_CLOSE : THEME_FG, docker_prune == i ? 1.0 : 0.7);
            cairo_move_to(cr, x + 10, theme_baseline(sub_y, 22, THEME_FONT_SM));
            cairo_show_text(cr, prune_labels[i]);
            sub_y += 24;
        }
        if (docker_prune == 2) {
            theme_font(cr, THEME_FONT_XS, 0);
            theme_rgba(cr, THEME_CLOSE, 0.85);
            theme_text_ellipsized(cr, x, sub_y + 10, 340,
                "Deletes locally-built images no container uses -- cannot be re-pulled.");
            sub_y += 24;
        }
        sub_y += 12;
    } else if (sel_kind == KIND_SQL) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.8);
        cairo_move_to(cr, x, theme_baseline(sub_y - 4, 16, THEME_FONT_SM));
        cairo_show_text(cr, "SQLite files to include (comma separated):");
        sub_y += 18;
        draw_field(cr, x, sub_y, 340, 30, sql_sqlite, "blank for none", focused_field == sql_sqlite);
        sub_y += 44;
    }

    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, 0.8);
    cairo_move_to(cr, x, theme_baseline(sub_y - 4, 16, THEME_FONT_SM));
    cairo_show_text(cr, "Where should it go?");
    sub_y += 18;
    draw_field(cr, x, sub_y, 340, 30, location, "(default for this kind)", focused_field == location);
    sub_y += 44;

    theme_rgba(cr, THEME_ACCENT, run_headless ? 0.9 : 0.3);
    theme_rounded_rect(cr, x, sub_y, 16, 16, 4.0);
    run_headless ? cairo_fill(cr) : cairo_stroke(cr);
    theme_rgb(cr, THEME_FG);
    cairo_move_to(cr, x + 24, theme_baseline(sub_y, 16, THEME_FONT_SM));
    cairo_show_text(cr, "Run in background (headless)");
    sub_y += 40;

    draw_button(cr, x, sub_y, 160, 34, "Run backup", THEME_MAXIMIZE, 0, op_pid == 0);
}

static void paint_schedule(cairo_t *cr) {
    double x = content_x();
    double y = content_top();

    paint_kind_selector(cr, x, y + 30);
    double sub_y = y + 30 + KIND_COUNT * ROW_H + 20;

    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, 0.8);
    cairo_move_to(cr, x, theme_baseline(sub_y - 4, 16, THEME_FONT_SM));
    cairo_show_text(cr, "How often?");
    sub_y += 18;
    for (int i = 0; i < FREQ_COUNT; i++) {
        if (sel_freq == i) {
            theme_rgba(cr, THEME_ACCENT, 0.18);
            theme_rounded_rect(cr, x, sub_y, 260, 22, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgba(cr, THEME_FG, sel_freq == i ? 1.0 : 0.7);
        cairo_move_to(cr, x + 10, theme_baseline(sub_y, 22, THEME_FONT_SM));
        cairo_show_text(cr, freq_presets[i].label);
        sub_y += 24;
    }
    if (!freq_presets[sel_freq].cron) {
        draw_field(cr, x, sub_y, 260, 28, custom_cron, "min hour day month weekday", focused_field == custom_cron);
        sub_y += 36;
    }
    sub_y += 8;

    theme_rgba(cr, THEME_FG, 0.8);
    cairo_move_to(cr, x, theme_baseline(sub_y - 4, 16, THEME_FONT_SM));
    cairo_show_text(cr, "Job name:");
    sub_y += 18;
    char default_name[192];
    snprintf(default_name, sizeof(default_name), "%s-%s", kind_dir(sel_kind), freq_presets[sel_freq].key);
    draw_field(cr, x, sub_y, 260, 28, job_name, default_name, focused_field == job_name);
    sub_y += 44;

    draw_button(cr, x, sub_y, 180, 34, "Save schedule", THEME_MAXIMIZE, 0, op_pid == 0);
}

static void paint_jobs(cairo_t *cr) {
    double x = content_x();
    double y = content_top();

    if (cron_file_path[0]) {
        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_FG, 0.5);
        char hdr[300];
        snprintf(hdr, sizeof(hdr), "From %s", cron_file_path);
        cairo_move_to(cr, x, theme_baseline(y, 16, THEME_FONT_XS));
        cairo_show_text(cr, hdr);
        y += 22;
        if (!cron_is_running) {
            theme_rgba(cr, THEME_MINIMIZE, 0.9);
            cairo_move_to(cr, x, theme_baseline(y, 16, THEME_FONT_XS));
            cairo_show_text(cr, "No cron daemon appears to be running -- these will not fire.");
            y += 22;
        }
    }
    y += 8;

    if (job_count == 0) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.45);
        cairo_move_to(cr, x, y + 10);
        cairo_show_text(cr, "No scheduled backups yet.");
        return;
    }

    for (int i = 0; i < job_count && y < content_bottom() - 50; i++) {
        job_t *j = &jobs[i];
        theme_rgba(cr, THEME_ACCENT, 0.06);
        theme_rounded_rect(cr, x - 4, y, win_w - x - 20, 50, THEME_RADIUS);
        cairo_fill(cr);

        theme_font(cr, THEME_FONT_MD, 1);
        theme_rgb(cr, THEME_FG);
        cairo_move_to(cr, x + 8, theme_baseline(y, 22, THEME_FONT_MD));
        cairo_show_text(cr, j->name);

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_ACCENT, 0.8);
        char when[120];
        snprintf(when, sizeof(when), "%s   (%s)", j->description, j->schedule);
        cairo_move_to(cr, x + 8, theme_baseline(y + 20, 16, THEME_FONT_XS));
        cairo_show_text(cr, when);

        theme_rgba(cr, THEME_FG, 0.5);
        theme_text_ellipsized(cr, x + 8, theme_baseline(y + 34, 16, THEME_FONT_XS),
                               win_w - x - 120, j->command);

        draw_button(cr, win_w - 96, y + 10, 74, 26, "Remove", THEME_CLOSE, hover_job_remove == i, op_pid == 0);
        y += 58;
    }
}

static void draw_progress_bar(cairo_t *cr, double x, double y, double w, double h, double frac) {
    theme_rgba(cr, THEME_ACCENT, 0.15);
    theme_rounded_rect(cr, x, y, w, h, h / 2.0);
    cairo_fill(cr);
    if (frac > 0) {
        if (frac > 1.0) frac = 1.0;
        theme_rgb(cr, THEME_ACCENT);
        theme_rounded_rect(cr, x, y, w * frac, h, h / 2.0);
        cairo_fill(cr);
    }
}

static void paint_status(cairo_t *cr) {
    double x = content_x();
    double y = content_top();

    if (!status_loaded_once) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.5);
        cairo_move_to(cr, x, y + 10);
        cairo_show_text(cr, "Loading...");
        return;
    }

    if (status_count == 0) {
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgba(cr, THEME_FG, 0.45);
        cairo_move_to(cr, x, y + 10);
        cairo_show_text(cr, "No backups running, and none finished recently.");
        return;
    }

    for (int i = 0; i < status_count && y < content_bottom() - 90; i++) {
        status_rec_t *r = &status_recs[i];
        double card_h = r->has_phase ? 108 : 66;

        theme_rgba(cr, THEME_ACCENT, 0.06);
        theme_rounded_rect(cr, x - 4, y, win_w - x - 20, card_h, THEME_RADIUS);
        cairo_fill(cr);

        unsigned badge_color = strcmp(r->state, "running") == 0 ? THEME_MAXIMIZE
                              : strcmp(r->state, "completed") == 0 ? THEME_ACCENT
                              : THEME_CLOSE;
        theme_font(cr, THEME_FONT_MD, 1);
        theme_rgb(cr, THEME_FG);
        char title[240];
        snprintf(title, sizeof(title), "%s%s%s%s on %s", r->operation,
                 r->mode[0] ? " (" : "", r->mode[0] ? r->mode : "", r->mode[0] ? ")" : "", r->hostname);
        cairo_move_to(cr, x + 10, theme_baseline(y + 6, 20, THEME_FONT_MD));
        cairo_show_text(cr, title);

        theme_font(cr, THEME_FONT_XS, 1);
        theme_rgba(cr, badge_color, 1.0);
        cairo_text_extents_t ext;
        char state_up[16];
        snprintf(state_up, sizeof(state_up), "%s", r->state);
        for (char *c = state_up; *c; c++) *c = (char)toupper((unsigned char)*c);
        cairo_text_extents(cr, state_up, &ext);
        cairo_move_to(cr, win_w - 24 - ext.x_advance, theme_baseline(y + 6, 20, THEME_FONT_XS));
        cairo_show_text(cr, state_up);

        double ly = y + 30;
        if (r->has_phase) {
            theme_font(cr, THEME_FONT_XS, 0);
            theme_rgba(cr, THEME_FG, 0.7);
            char phase_line[220];
            snprintf(phase_line, sizeof(phase_line), "[%ld/%ld] %s", r->phase_index, r->phase_count, r->phase_detail);
            theme_text_ellipsized(cr, x + 10, theme_baseline(ly, 16, THEME_FONT_XS), win_w - x - 40, phase_line);
            ly += 18;

            draw_progress_bar(cr, x + 10, ly, win_w - x - 40, 8,
                               r->phase_percent_known ? r->phase_percent / 100.0 : -1.0);
            ly += 18;

            char detail_line[160] = "";
            if (r->phase_percent_known) {
                char pct[8]; snprintf(pct, sizeof(pct), "%.0f%%  ", r->phase_percent);
                strncat(detail_line, pct, sizeof(detail_line) - strlen(detail_line) - 1);
            }
            if (r->phase_eta_seconds >= 0) {
                char eta[32];
                snprintf(eta, sizeof(eta), "ETA %ldm%02lds", (long)r->phase_eta_seconds / 60, (long)r->phase_eta_seconds % 60);
                strncat(detail_line, eta, sizeof(detail_line) - strlen(detail_line) - 1);
            }
            theme_rgba(cr, THEME_FG, 0.55);
            cairo_move_to(cr, x + 10, theme_baseline(ly, 16, THEME_FONT_XS));
            cairo_show_text(cr, detail_line);
            ly += 20;
        }

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_FG, 0.55);
        char summary[160];
        snprintf(summary, sizeof(summary), "overall %ld%%   elapsed %ldm%02lds",
                 r->overall_percent, r->elapsed_seconds / 60, r->elapsed_seconds % 60);
        cairo_move_to(cr, x + 10, theme_baseline(ly, 16, THEME_FONT_XS));
        cairo_show_text(cr, summary);

        if (r->error[0]) {
            theme_rgba(cr, THEME_CLOSE, 0.9);
            theme_text_ellipsized(cr, x + 10, theme_baseline(ly + 16, 16, THEME_FONT_XS), win_w - x - 40, r->error);
        } else if (r->archive[0]) {
            theme_rgba(cr, THEME_FG, 0.45);
            theme_text_ellipsized(cr, x + 10, theme_baseline(ly + 16, 16, THEME_FONT_XS), win_w - x - 40, r->archive);
        }

        y += card_h + 12;
    }
}

static double snap_kind_x(int i) { return content_x() + i * 100; }

static void paint_snapshots(cairo_t *cr) {
    double x = content_x();
    double y = content_top();

    const char *kind_labels[] = { "System", "Docker", "Databases", "Everything" };
    theme_font(cr, THEME_FONT_SM, 0);
    for (int i = 0; i < 4; i++) {
        double bx = snap_kind_x(i);
        if (snap_kind == i) {
            theme_rgba(cr, THEME_ACCENT, 0.18);
            theme_rounded_rect(cr, bx, y, 92, 24, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgba(cr, THEME_FG, snap_kind == i ? 1.0 : 0.65);
        cairo_move_to(cr, bx + 10, theme_baseline(y, 24, THEME_FONT_SM));
        cairo_show_text(cr, kind_labels[i]);
    }
    y += 40;

    if (snap_count == 0) {
        theme_rgba(cr, THEME_FG, 0.45);
        cairo_move_to(cr, x, y + 10);
        cairo_show_text(cr, "No snapshots found.");
        return;
    }

    theme_font(cr, THEME_FONT_XS, 1);
    theme_rgba(cr, THEME_FG, 0.5);
    cairo_move_to(cr, x, theme_baseline(y, 18, THEME_FONT_XS));
    cairo_show_text(cr, "NAME");
    cairo_move_to(cr, win_w - 260, theme_baseline(y, 18, THEME_FONT_XS));
    cairo_show_text(cr, "CREATED");
    cairo_move_to(cr, win_w - 130, theme_baseline(y, 18, THEME_FONT_XS));
    cairo_show_text(cr, "MODE");
    cairo_move_to(cr, win_w - 70, theme_baseline(y, 18, THEME_FONT_XS));
    cairo_show_text(cr, "SIZE");
    y += 22;

    for (int i = 0; i < snap_count && y < content_bottom() - 20; i++) {
        snap_t *sn = &snaps[i];
        if (i == hover_snap_row) {
            theme_rgba(cr, THEME_ACCENT, 0.08);
            theme_rounded_rect(cr, x - 4, y, win_w - x - 20, ROW_H - 2, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_font(cr, THEME_FONT_SM, 0);
        theme_rgb(cr, THEME_FG);
        theme_text_ellipsized(cr, x, theme_baseline(y, ROW_H, THEME_FONT_SM), win_w - x - 280, sn->name);

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_FG, 0.6);
        cairo_move_to(cr, win_w - 260, theme_baseline(y, ROW_H, THEME_FONT_XS));
        cairo_show_text(cr, sn->created);
        cairo_move_to(cr, win_w - 130, theme_baseline(y, ROW_H, THEME_FONT_XS));
        cairo_show_text(cr, sn->mode);
        cairo_move_to(cr, win_w - 70, theme_baseline(y, ROW_H, THEME_FONT_XS));
        cairo_show_text(cr, sn->size_human);
        y += ROW_H;
    }

    theme_font(cr, THEME_FONT_XS, 0);
    theme_rgba(cr, THEME_FG, 0.4);
    cairo_move_to(cr, x, content_bottom() - 6);
    cairo_show_text(cr, "Click a row to fill it into Restore.");
}

static void paint_restore(cairo_t *cr) {
    double x = content_x();
    double y = content_top();

    theme_font(cr, THEME_FONT_SM, 0);
    theme_rgba(cr, THEME_FG, 0.8);
    cairo_move_to(cr, x, theme_baseline(y, 16, THEME_FONT_SM));
    cairo_show_text(cr, "Path to the snapshot to restore:");
    y += 24;
    draw_field(cr, x, y, 460, 32, restore_path, "/backup/nekoshot/system/host-20260101-120000.tar.zst",
               focused_field == restore_path);
    y += 40;

    theme_font(cr, THEME_FONT_XS, 0);
    theme_rgba(cr, THEME_FG, 0.5);
    cairo_move_to(cr, x, theme_baseline(y, 16, THEME_FONT_XS));
    cairo_show_text(cr, "Live restore to the running system. Docker-only snapshots are detected automatically.");
    y += 30;

    if (!restore_armed) {
        draw_button(cr, x, y, 160, 34, "Restore", THEME_CLOSE, 0, op_pid == 0 && restore_path[0]);
    } else {
        draw_button(cr, x, y, 190, 34, "Confirm Restore", THEME_CLOSE, 0, op_pid == 0);
        draw_button(cr, x + 200, y, 100, 34, "Cancel", THEME_MUTED, 0, 1);
        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_CLOSE, 0.85);
        theme_text_ellipsized(cr, x, y + 50, 460, "This overwrites the running system. Are you sure?");
    }
}

static void paint(void) {
    cairo_surface_t *surface = cairo_xcb_surface_create(conn, win, visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    theme_rgb(cr, THEME_BG);
    cairo_paint(cr);

    /* Sidebar. */
    theme_panel_gradient(cr, 0, 0, SIDEBAR_W, win_h, 1);
    theme_font(cr, THEME_FONT_MD, 0);
    for (int i = 0; i < PAGE_COUNT; i++) {
        double y = 8 + i * 34;
        if ((int)page == i) {
            theme_rgba(cr, THEME_ACCENT, 0.22);
            theme_rounded_rect(cr, 6, y, SIDEBAR_W - 12, 30, THEME_RADIUS);
            cairo_fill(cr);
            theme_rgba(cr, THEME_ACCENT, 0.9);
            cairo_rectangle(cr, 6, y + 5, 3, 20);
            cairo_fill(cr);
        } else if (hover_page == i) {
            theme_rgba(cr, THEME_ACCENT, THEME_HOVER_ALPHA);
            theme_rounded_rect(cr, 6, y, SIDEBAR_W - 12, 30, THEME_RADIUS);
            cairo_fill(cr);
        }
        theme_rgba(cr, THEME_FG, (int)page == i ? 1.0 : 0.75);
        cairo_move_to(cr, 18, theme_baseline(y, 30, THEME_FONT_MD));
        cairo_show_text(cr, page_names[i]);
    }
    theme_rgba(cr, THEME_ACCENT, 0.25);
    cairo_rectangle(cr, SIDEBAR_W - 1, 0, 1, win_h);
    cairo_fill(cr);

    switch (page) {
    case PAGE_BACKUP:    paint_backup(cr); break;
    case PAGE_SCHEDULE:  paint_schedule(cr); break;
    case PAGE_JOBS:      paint_jobs(cr); break;
    case PAGE_STATUS:    paint_status(cr); break;
    case PAGE_SNAPSHOTS: paint_snapshots(cr); break;
    case PAGE_RESTORE:   paint_restore(cr); break;
    }

    /* Log strip. */
    if (have_log) {
        double ly = win_h - LOG_H;
        theme_rgba(cr, THEME_ICON_EXEC_BG, 1.0);
        cairo_rectangle(cr, SIDEBAR_W, ly, win_w - SIDEBAR_W, LOG_H);
        cairo_fill(cr);
        theme_rgba(cr, THEME_ACCENT, 0.4);
        cairo_rectangle(cr, SIDEBAR_W, ly, win_w - SIDEBAR_W, 1);
        cairo_fill(cr);

        theme_font(cr, THEME_FONT_XS, 1);
        theme_rgba(cr, THEME_ACCENT, 0.8);
        cairo_move_to(cr, SIDEBAR_W + 10, ly + 16);
        cairo_show_text(cr, op_pid > 0 ? op_label : op_status);

        theme_font(cr, THEME_FONT_XS, 0);
        theme_rgba(cr, THEME_ICON_EXEC_FG, 0.9);
        for (int i = 0; i < log_count; i++) {
            theme_text_ellipsized(cr, SIDEBAR_W + 10, ly + 32 + i * 14, win_w - SIDEBAR_W - 20, log_lines[i]);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(conn);
}

/* ---- input --------------------------------------------------------------- */

static void set_focus(char *field, size_t cap) {
    focused_field = field;
    focused_field_cap = cap;
}

static void handle_button_press(xcb_button_press_event_t *ev) {
    if (ev->detail != 1) return;
    double bx = ev->event_x, by = ev->event_y;

    /* Sidebar. */
    if (bx < SIDEBAR_W) {
        int idx = (int)((by - 8) / 34);
        if (idx >= 0 && idx < PAGE_COUNT && (page_t)idx != page) {
            page = (page_t)idx;
            focused_field = NULL;
            reload_page();
            paint();
        }
        return;
    }

    double x = content_x();

    switch (page) {
    case PAGE_BACKUP: case PAGE_SCHEDULE: {
        double y0 = content_top() + 30;
        for (int i = 0; i < KIND_COUNT; i++) {
            double ry = y0 + i * ROW_H;
            if (bx >= x && bx < x + KIND_ROW_W && by >= ry && by < ry + ROW_H - 4) {
                sel_kind = (kind_t)i;
                paint();
                return;
            }
        }
        break;
    }
    default: break;
    }

    if (page == PAGE_BACKUP) {
        double sub_y = content_top() + 30 + KIND_COUNT * ROW_H + 20;
        if (sel_kind == KIND_DOCKER) {
            if (bx >= x && bx < x + 300 && by >= sub_y && by < sub_y + 20) {
                docker_pause = !docker_pause;
                paint();
                return;
            }
            sub_y += 30 + 20;
            for (int i = 0; i < 3; i++) {
                if (bx >= x && bx < x + 280 && by >= sub_y && by < sub_y + 22) {
                    docker_prune = i;
                    paint();
                    return;
                }
                sub_y += 24;
            }
            if (docker_prune == 2) sub_y += 24;
            sub_y += 12;
        } else if (sel_kind == KIND_SQL) {
            sub_y += 18;
            if (bx >= x && bx < x + 340 && by >= sub_y && by < sub_y + 30) {
                set_focus(sql_sqlite, sizeof(sql_sqlite));
                paint();
                return;
            }
            sub_y += 44;
        }
        sub_y += 18;
        if (bx >= x && bx < x + 340 && by >= sub_y && by < sub_y + 30) {
            set_focus(location, sizeof(location));
            paint();
            return;
        }
        sub_y += 44;
        if (bx >= x && bx < x + 300 && by >= sub_y && by < sub_y + 20) {
            run_headless = !run_headless;
            paint();
            return;
        }
        sub_y += 40;
        if (bx >= x && bx < x + 160 && by >= sub_y && by < sub_y + 34) {
            run_backup();
            return;
        }
    } else if (page == PAGE_SCHEDULE) {
        double sub_y = content_top() + 30 + KIND_COUNT * ROW_H + 20 + 18;
        for (int i = 0; i < FREQ_COUNT; i++) {
            if (bx >= x && bx < x + 260 && by >= sub_y && by < sub_y + 22) {
                sel_freq = i;
                paint();
                return;
            }
            sub_y += 24;
        }
        if (!freq_presets[sel_freq].cron) {
            if (bx >= x && bx < x + 260 && by >= sub_y && by < sub_y + 28) {
                set_focus(custom_cron, sizeof(custom_cron));
                paint();
                return;
            }
            sub_y += 36;
        }
        sub_y += 8 + 18;
        if (bx >= x && bx < x + 260 && by >= sub_y && by < sub_y + 28) {
            set_focus(job_name, sizeof(job_name));
            paint();
            return;
        }
        sub_y += 44;
        if (bx >= x && bx < x + 180 && by >= sub_y && by < sub_y + 34) {
            save_schedule();
            return;
        }
    } else if (page == PAGE_JOBS) {
        double y = content_top();
        if (cron_file_path[0]) {
            y += 22;
            if (!cron_is_running) y += 22;
        }
        y += 8;
        for (int i = 0; i < job_count; i++) {
            if (bx >= win_w - 96 && bx < win_w - 22 && by >= y + 10 && by < y + 36) {
                remove_job(jobs[i].name);
                return;
            }
            y += 58;
        }
    } else if (page == PAGE_SNAPSHOTS) {
        double y = content_top();
        for (int i = 0; i < 4; i++) {
            double bx0 = snap_kind_x(i);
            if (bx >= bx0 && bx < bx0 + 92 && by >= y && by < y + 24) {
                snap_kind = i;
                load_snapshots();
                paint();
                return;
            }
        }
        y += 40 + 22;
        int idx = (int)((by - y) / ROW_H);
        if (idx >= 0 && idx < snap_count && bx >= x) {
            snprintf(restore_path, sizeof(restore_path), "%s", snaps[idx].path);
            page = PAGE_RESTORE;
            paint();
            return;
        }
    } else if (page == PAGE_RESTORE) {
        double y = content_top() + 24;
        if (bx >= x && bx < x + 460 && by >= y && by < y + 32) {
            set_focus(restore_path, sizeof(restore_path));
            paint();
            return;
        }
        y += 40 + 30;
        if (!restore_armed) {
            if (bx >= x && bx < x + 160 && by >= y && by < y + 34 && restore_path[0]) {
                restore_armed = 1;
                paint();
            }
        } else {
            if (bx >= x && bx < x + 190 && by >= y && by < y + 34) {
                run_restore();
            } else if (bx >= x + 200 && bx < x + 300 && by >= y && by < y + 34) {
                restore_armed = 0;
                paint();
            }
        }
    }
}

static int handle_key(xcb_key_press_event_t *ev) {
    if (!focused_field) return 1;
    xcb_keysym_t ks = xcb_key_press_lookup_keysym(keysyms, ev, (ev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0);
    if (ks == XK_Escape || ks == XK_Return || ks == XK_KP_Enter || ks == XK_Tab) {
        focused_field = NULL;
        paint();
        return 1;
    }
    if (ks == XK_BackSpace) {
        size_t l = strlen(focused_field);
        if (l > 0) { focused_field[l - 1] = '\0'; paint(); }
        return 1;
    }
    if (ks >= 0x20 && ks <= 0x7e) {
        size_t l = strlen(focused_field);
        if (l < focused_field_cap - 1) {
            focused_field[l] = (char)ks;
            focused_field[l + 1] = '\0';
            paint();
        }
        return 1;
    }
    return 1;
}

static void handle_motion(int16_t x, int16_t y) {
    int hp = -1, hj = -1, hs = -1;

    if (x < SIDEBAR_W) {
        int idx = (int)((y - 8) / 34);
        if (idx >= 0 && idx < PAGE_COUNT) hp = idx;
    } else if (page == PAGE_JOBS) {
        double jy = content_top();
        if (cron_file_path[0]) {
            jy += 22;
            if (!cron_is_running) jy += 22;
        }
        jy += 8;
        for (int i = 0; i < job_count; i++) {
            if (x >= win_w - 96 && x < win_w - 22 && y >= jy + 10 && y < jy + 36) { hj = i; break; }
            jy += 58;
        }
    } else if (page == PAGE_SNAPSHOTS) {
        double sy = content_top() + 40 + 22;
        int idx = (int)((y - sy) / ROW_H);
        if (idx >= 0 && idx < snap_count && x >= content_x()) hs = idx;
    }

    if (hp != hover_page || hj != hover_job_remove || hs != hover_snap_row) {
        hover_page = hp;
        hover_job_remove = hj;
        hover_snap_row = hs;
        paint();
    }
}

/* ---- main ------------------------------------------------------------ */

int main(void) {
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);

    resolve_notify_tool();

    conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        fprintf(stderr, "nekos-shot: could not connect to X server\n");
        return 1;
    }
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    visual = find_visual(screen, screen->root_visual);
    keysyms = xcb_key_symbols_alloc(conn);

    win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW,
    };
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root,
                       0, 0, INIT_W, INIT_H, 0,
                       XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

    static const char title[] = "Backups";
    xcb_atom_t net_wm_name, utf8_string;
    {
        xcb_intern_atom_reply_t *r1 = xcb_intern_atom_reply(
            conn, xcb_intern_atom(conn, 0, 12, "_NET_WM_NAME"), NULL);
        xcb_intern_atom_reply_t *r2 = xcb_intern_atom_reply(
            conn, xcb_intern_atom(conn, 0, 11, "UTF8_STRING"), NULL);
        net_wm_name = r1 ? r1->atom : XCB_ATOM_NONE;
        utf8_string = r2 ? r2->atom : XCB_ATOM_NONE;
        free(r1);
        free(r2);
    }
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, net_wm_name,
                         utf8_string, 8, (uint32_t)(sizeof(title) - 1), title);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 8, (uint32_t)(sizeof(title) - 1), title);

    xcb_map_window(conn, win);
    xcb_flush(conn);

    paint();

    int xfd = xcb_get_file_descriptor(conn);

    for (;;) {
        int timeout_ms = -1;
        if (page == PAGE_STATUS) {
            int64_t elapsed = status_loaded_once ? now_ms() -
                ((int64_t)last_status_poll.tv_sec * 1000 + last_status_poll.tv_nsec / 1000000) : 100000;
            timeout_ms = elapsed >= 1000 ? 0 : (int)(1000 - elapsed);
        }

        struct pollfd pfds[2] = {
            { .fd = xfd, .events = POLLIN, .revents = 0 },
            { .fd = op_fd, .events = POLLIN, .revents = 0 },
        };
        poll(pfds, op_fd >= 0 ? 2 : 1, timeout_ms);

        if (page == PAGE_STATUS) {
            int64_t elapsed = status_loaded_once ? now_ms() -
                ((int64_t)last_status_poll.tv_sec * 1000 + last_status_poll.tv_nsec / 1000000) : 100000;
            if (elapsed >= 1000) { load_status(); paint(); }
        }

        if (op_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            ssize_t n;
            int eof = 0;
            while ((n = read(op_fd, buf, sizeof(buf))) != 0) {
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    eof = 1;
                    break;
                }
                log_append_chunk(buf, n);
            }
            if (n == 0) eof = 1;
            if (eof) finish_op();
            else paint();
        }

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(conn))) {
            switch (event->response_type & ~0x80) {
            case XCB_EXPOSE:
                paint();
                break;
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *ce = (xcb_configure_notify_event_t *)event;
                if (ce->width > 0 && ce->height > 0 && (ce->width != win_w || ce->height != win_h)) {
                    win_w = ce->width;
                    win_h = ce->height;
                    paint();
                }
                break;
            }
            case XCB_BUTTON_PRESS:
                handle_button_press((xcb_button_press_event_t *)event);
                break;
            case XCB_KEY_PRESS:
                handle_key((xcb_key_press_event_t *)event);
                break;
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)event;
                handle_motion(me->event_x, me->event_y);
                break;
            }
            case XCB_LEAVE_NOTIFY:
                if (hover_page != -1 || hover_job_remove != -1 || hover_snap_row != -1) {
                    hover_page = -1;
                    hover_job_remove = -1;
                    hover_snap_row = -1;
                    paint();
                }
                break;
            default:
                break;
            }
            free(event);
        }
        if (xcb_connection_has_error(conn)) break;
    }

    xcb_key_symbols_free(keysyms);
    xcb_disconnect(conn);
    return 0;
}
