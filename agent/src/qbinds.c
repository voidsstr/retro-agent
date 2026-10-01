/*
 * qbinds.c - QBINDS: keep the fleet's WASD layout in force in Quake 1 and
 * Quake II (agent 1.97.0). The decisions, the bodies and the story are in
 * agent/shared/qbinds.h; this is the file I/O, the passes and the command.
 *
 * WHERE IT RUNS
 *   - every agent start, from gamesync_thread() after the modern-host guard
 *     and BEFORE its "already provisioned - idle" return (that return is the
 *     NORMAL path on a fleet box - placing anything after it means it runs on
 *     almost no machine while looking installed);
 *   - after every title GAMESYNC copies (gs_run, after gameres_apply_title),
 *     so a freshly deployed Quake gets its layout in the same sync;
 *   - on demand: QBINDS [title] reports (read-only); QBINDS apply [title]
 *     enforces now (refused on a modern Windows host).
 *
 * WHAT IT TOUCHES: <games folder>\<title>\<gamedir>\FLEETKEY.CFG, for the
 * titles and gamedirs in qb_titles[], and NOTHING else. It reads autoexec.cfg
 * to say whether the chain reaches the file; it never writes it. A gamedir that
 * is not there is not created. A games folder that is configured but unusable
 * (GamesDir on a missing drive - .243's E: after a power loss) is refused
 * loudly; it NEVER falls back to C:.
 *
 * WIN9x SAFE: only kernel32 file calls the agent already imports and that run
 * on .243 (CreateFileA, ReadFile, WriteFile, FlushFileBuffers, GetFileAttributesA,
 * SetFileAttributesA, DeleteFileA, MoveFileA - no MoveFileExA, which 9x lacks),
 * no new DLL, no child process, no thread of its own. ~6 small files per pass.
 *
 * A settled box writes nothing and logs one line.
 */
#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "hostpolicy.h"
#include "../shared/qbinds.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

#define LOG_QB "QBINDS"

static CRITICAL_SECTION g_qb_lock;
static int g_qb_lock_ready;

void qbinds_init(void)
{
    if (!g_qb_lock_ready) {
        InitializeCriticalSection(&g_qb_lock);
        g_qb_lock_ready = 1;
    }
}

/* ---- registry ----------------------------------------------------------- */

static int qb_switch(int *present, DWORD *raw)
{
    HKEY h;
    DWORD v = 1, sz = sizeof(v), type = 0;
    *present = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, QB_REG_SWITCH, NULL, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS
                && type == REG_DWORD)
            *present = 1;
        RegCloseKey(h);
    }
    *raw = *present ? v : 1;
    return qb_mode(*present, v);
}

static void qb_store(const char *msg)
{
    HKEY h;
    SYSTEMTIME st;
    char line[400];
    GetLocalTime(&st);
    _snprintf(line, sizeof(line) - 1, "%04u-%02u-%02u %02u:%02u %s", st.wYear, st.wMonth,
              st.wDay, st.wHour, st.wMinute, msg);
    line[sizeof(line) - 1] = 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(h, QB_REG_RESULT, 0, REG_SZ, (const BYTE *)line, (DWORD)strlen(line) + 1);
    RegCloseKey(h);
}

static void qb_load(char *out, DWORD cch)
{
    HKEY h;
    DWORD type = 0, sz = cch - 1;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(h, QB_REG_RESULT, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS
            || type != REG_SZ)
        out[0] = 0;
    out[cch - 1] = 0;
    RegCloseKey(h);
}

/* ---- files -------------------------------------------------------------- */

static int qb_is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Read up to cap bytes. 1 = the file exists (buf/len filled; *big = it is
 * larger than cap), 0 = it does not (or cannot be opened). */
static int qb_read(const char *path, char *buf, DWORD cap, DWORD *len, int *big)
{
    HANDLE h;
    DWORD size, got = 0;
    *len = 0;
    if (big)
        *big = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(h, NULL);
    if (size != 0xFFFFFFFF && size > cap) {
        if (big)
            *big = 1;
        size = cap;
    }
    if (size == 0xFFFFFFFF)
        size = cap;
    if (size && !ReadFile(h, buf, size, &got, NULL))
        got = 0;
    CloseHandle(h);
    *len = got;
    return 1;
}

/* Does `path` hold exactly `want`? */
static int qb_holds(const char *path, const char *want, size_t wlen)
{
    char buf[QB_MAX_FILE];
    DWORD len = 0;
    int big = 0;
    if (!qb_read(path, buf, sizeof(buf), &len, &big) || big)
        return 0;
    return qb_file_state(1, buf, len, want, wlen) == QB_FILE_CURRENT;
}

/* Replace <dir>\FLEETKEY.CFG with `want`: write FLEETKEY.TMP, read it back,
 * then DeleteFile + MoveFile (MoveFileExA does not exist on 9x), then read the
 * result back. The only file this module ever opens for writing is the temp. */
static int qb_write(const char *dir, const char *want, size_t wlen, char *why, size_t whycch)
{
    char tmp[MAX_PATH], dst[MAX_PATH];
    HANDLE h;
    DWORD put = 0;
    BOOL ok;

    why[0] = 0;
    _snprintf(tmp, sizeof(tmp) - 1, "%s\\%s", dir, QB_TMP);
    tmp[sizeof(tmp) - 1] = 0;
    _snprintf(dst, sizeof(dst) - 1, "%s\\%s", dir, QB_FILE);
    dst[sizeof(dst) - 1] = 0;

    SetFileAttributesA(tmp, FILE_ATTRIBUTE_NORMAL);
    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf(why, whycch - 1, "cannot create %s (error %lu)", tmp,
                  (unsigned long)GetLastError());
        why[whycch - 1] = 0;
        return 0;
    }
    ok = WriteFile(h, want, (DWORD)wlen, &put, NULL) && put == (DWORD)wlen;
    if (ok)
        FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !qb_holds(tmp, want, wlen)) {
        _snprintf(why, whycch - 1, "%s did not read back as written (error %lu)", tmp,
                  (unsigned long)GetLastError());
        why[whycch - 1] = 0;
        DeleteFileA(tmp);
        return 0;
    }
    if (GetFileAttributesA(dst) != 0xFFFFFFFF) {
        SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileA(dst)) {
            _snprintf(why, whycch - 1, "cannot replace %s (error %lu)", dst,
                      (unsigned long)GetLastError());
            why[whycch - 1] = 0;
            DeleteFileA(tmp);
            return 0;
        }
    }
    if (!MoveFileA(tmp, dst)) {
        _snprintf(why, whycch - 1, "cannot rename %s to %s (error %lu)", QB_TMP, QB_FILE,
                  (unsigned long)GetLastError());
        why[whycch - 1] = 0;
        DeleteFileA(tmp);
        return 0;
    }
    if (!qb_holds(dst, want, wlen)) {
        _snprintf(why, whycch - 1, "%s does not read back as written", dst);
        why[whycch - 1] = 0;
        return 0;
    }
    return 1;
}

/* ---- one gamedir -------------------------------------------------------- */

typedef struct {
    const char *dir;
    int  present;
    int  state_before;      /* QB_FILE_* against what this mode wants */
    int  state_now;         /* QB_FILE_* against the WASD body, after the pass */
    int  written, failed;
    int  have_autoexec;
    int  effective;
    qb_scan_t scan;
    char why[200];
} qb_dir_t;

typedef struct {
    int  gamedirs, effective, written, failed, not_executed, unbindall;
    int  current, stale, missing;       /* against what the mode wants */
    int  titles_installed;
    char first_fail[300];
} qb_sum_t;

static void qb_do_dir(const char *title_dir, const qb_title_t *t, const char *gd, int mode,
                      int do_write, qb_dir_t *r)
{
    char dir[MAX_PATH], path[MAX_PATH];
    static char ae[QB_MAX_AUTOEXEC];        /* under g_qb_lock */
    char cur[QB_MAX_FILE];
    DWORD len = 0, aelen = 0;
    int have, big = 0, aebig = 0;
    size_t wlen = 0, wasdlen = 0;
    const char *want, *wasd;

    memset(r, 0, sizeof(*r));
    r->dir = gd;
    _snprintf(dir, sizeof(dir) - 1, "%s\\%s", title_dir, gd);
    dir[sizeof(dir) - 1] = 0;
    if (!qb_is_dir(dir))
        return;                             /* not created - not ours to make */
    r->present = 1;

    wasd = qb_body(t->profile, QB_MODE_WASD, &wasdlen);
    want = qb_body(t->profile, mode, &wlen);
    if (!want) {                            /* hands off: judged against WASD, never written */
        want = wasd;
        wlen = wasdlen;
    }

    _snprintf(path, sizeof(path) - 1, "%s\\%s", dir, QB_FILE);
    path[sizeof(path) - 1] = 0;
    have = qb_read(path, cur, sizeof(cur), &len, &big);
    r->state_before = big ? QB_FILE_STALE : qb_file_state(have, cur, len, want, wlen);

    if (do_write && mode != QB_MODE_HANDS_OFF && r->state_before != QB_FILE_CURRENT) {
        if (qb_write(dir, want, wlen, r->why, sizeof(r->why))) {
            r->written = 1;
            log_msg(LOG_QB, "%s written (was %s)", path,
                    r->state_before == QB_FILE_MISSING ? "missing" : "stale");
        } else {
            r->failed = 1;
            log_msg(LOG_QB, "*** QUAKE BINDS NOT APPLIED *** %s: %s", path, r->why);
        }
    }

    /* The post-condition: what the file holds NOW, against the WASD body. */
    have = qb_read(path, cur, sizeof(cur), &len, &big);
    r->state_now = big ? QB_FILE_STALE : qb_file_state(have, cur, len, wasd, wasdlen);

    _snprintf(path, sizeof(path) - 1, "%s\\autoexec.cfg", dir);
    path[sizeof(path) - 1] = 0;
    r->have_autoexec = qb_read(path, ae, sizeof(ae), &aelen, &aebig);
    qb_autoexec_scan(r->have_autoexec ? ae : NULL, aelen, t->profile, &r->scan);
    r->effective = qb_effective(r->state_now == QB_FILE_CURRENT, &r->scan);
}

static const char *qb_bindfile_name(const qb_dir_t *r)
{
    if (r->failed)
        return "failed";
    if (r->written)
        return "written";
    return r->state_before == QB_FILE_CURRENT ? "current"
         : r->state_before == QB_FILE_STALE ? "stale" : "missing";
}

static void qb_tally(qb_sum_t *s, const qb_dir_t *r, const char *title_dir)
{
    if (!r->present)
        return;
    s->gamedirs++;
    if (r->effective) s->effective++;
    if (r->written) s->written++;
    if (r->failed) {
        s->failed++;
        if (!s->first_fail[0]) {
            _snprintf(s->first_fail, sizeof(s->first_fail) - 1, "%s\\%s: %s", title_dir, r->dir, r->why);
            s->first_fail[sizeof(s->first_fail) - 1] = 0;
        }
    }
    if (!r->scan.exec_live) s->not_executed++;
    if (r->scan.unbindall != QB_UB_NONE) s->unbindall++;
    if (r->written || r->state_before == QB_FILE_CURRENT) s->current++;
    else if (!r->failed && r->state_before == QB_FILE_STALE) s->stale++;
    else if (!r->failed) s->missing++;
}

static void qb_dir_json(json_t *j, const qb_dir_t *r)
{
    int i;
    json_object_start(j);
    json_kv_str(j, "dir", r->dir);
    json_kv_bool(j, "present", r->present);
    if (r->present) {
        json_kv_str(j, "bindfile", qb_bindfile_name(r));
        json_kv_bool(j, "wasd_in_place", r->state_now == QB_FILE_CURRENT);
        json_kv_str(j, "exec", qb_exec_name(r->have_autoexec, &r->scan));
        json_kv_str(j, "unbindall", qb_unbindall_name(r->scan.unbindall));
        json_kv_bool(j, "quote_trap", r->scan.quote_trap);
        if (r->scan.quote_trap)
            json_kv_int(j, "quote_line", r->scan.quote_line);
        json_key(j, "rebinds_after");
        json_array_start(j);
        for (i = 0; i < r->scan.nrebind; i++)
            json_str(j, r->scan.rebinds[i]);
        json_array_end(j);
        json_kv_bool(j, "effective", r->effective);
        json_kv_bool(j, "written", r->written);
        if (r->failed)
            json_kv_str(j, "error", r->why);
    }
    json_object_end(j);
}

/* One title. Called with g_qb_lock held. */
static void qb_do_title(const char *games, const qb_title_t *t, int mode, int do_write,
                        qb_sum_t *s, json_t *j)
{
    char tdir[MAX_PATH];
    qb_dir_t r;
    int d, installed;

    _snprintf(tdir, sizeof(tdir) - 1, "%s\\%s", games, t->title);
    tdir[sizeof(tdir) - 1] = 0;
    installed = qb_is_dir(tdir);
    if (installed)
        s->titles_installed++;
    if (j) {
        json_object_start(j);
        json_kv_str(j, "title", t->title);
        json_kv_bool(j, "installed", installed);
        json_kv_str(j, "profile", qb_profiles[t->profile].name);
        json_key(j, "gamedirs");
        json_array_start(j);
    }
    for (d = 0; installed && d < QB_MAX_DIRS && t->dirs[d]; d++) {
        qb_do_dir(tdir, t, t->dirs[d], mode, do_write, &r);
        qb_tally(s, &r, tdir);
        if (j)
            qb_dir_json(j, &r);
    }
    if (j) {
        json_array_end(j);
        json_object_end(j);
    }
}

/* The state line - the same words for the log, the record and the JSON. */
static void qb_state(char *out, size_t cch, int mode, int do_write, const qb_sum_t *s)
{
    char tail[200];
    tail[0] = 0;
    if (s->not_executed || s->unbindall)
        _snprintf(tail, sizeof(tail) - 1, "; %d gamedir(s) do not exec fleetkey.cfg, "
                  "%d still run unbindall", s->not_executed, s->unbindall);
    tail[sizeof(tail) - 1] = 0;
    if (mode == QB_MODE_HANDS_OFF)
        _snprintf(out, cch - 1, "hands off: QuakeBinds=2 - FLEETKEY.CFG not touched; "
                  "%d of %d gamedir(s) have the fleet layout in force", s->effective, s->gamedirs);
    else if (s->failed)
        _snprintf(out, cch - 1, "FAILED: %d of %d bind file(s) could not be written - first %s",
                  s->failed, s->gamedirs, s->first_fail);
    else if (!s->titles_installed)
        _snprintf(out, cch - 1, "ok: no Quake title installed here - nothing to do");
    else if (do_write)
        _snprintf(out, cch - 1, "ok: %d %s bind file(s) current, %d written, %d with the "
                  "layout in force%s", s->current, qb_mode_name(mode), s->written,
                  s->effective, tail);
    else
        _snprintf(out, cch - 1, "read-only: %d %s bind file(s) current, %d stale, %d missing, "
                  "%d with the layout in force%s", s->current, qb_mode_name(mode), s->stale,
                  s->missing, s->effective, tail);
    out[cch - 1] = 0;
}

/* ---- the passes --------------------------------------------------------- */

/* The startup pass: every in-scope title in the games folder. Returns the
 * number of files written, -1 when it could not run. */
int qbinds_startup(void)
{
    char games[200], why[200], msg[400];
    qb_sum_t s;
    DWORD raw;
    int present, mode, i;

    if (host_policy_skip("qbinds (Quake key layout)"))
        return -1;
    qbinds_init();
    mode = qb_switch(&present, &raw);
    if (!gs_games_dir_why(games, sizeof(games), why, sizeof(why))) {
        _snprintf(msg, sizeof(msg) - 1, "NOT APPLIED: the games folder is unusable - %s "
                  "(never falls back to C:)", why);
        msg[sizeof(msg) - 1] = 0;
        log_msg(LOG_QB, "*** QUAKE BINDS NOT APPLIED *** %s", msg);
        qb_store(msg);
        return -1;
    }
    memset(&s, 0, sizeof(s));
    EnterCriticalSection(&g_qb_lock);
    for (i = 0; i < QB_NTITLES; i++)
        qb_do_title(games, &qb_titles[i], mode, 1, &s, NULL);
    LeaveCriticalSection(&g_qb_lock);
    qb_state(msg, sizeof(msg), mode, 1, &s);
    if (s.failed)
        log_msg(LOG_QB, "*** QUAKE BINDS NOT APPLIED *** %s", msg);
    else
        log_msg(LOG_QB, "qbinds: %s", msg);
    qb_store(msg);
    return s.written;
}

/* GAMESYNC's per-title hook (gs_run, after gameres_apply_title). Returns the
 * files written; *not_executed gets the gamedirs whose autoexec.cfg does not
 * reach FLEETKEY.CFG. Silent unless it wrote or failed. */
int qbinds_apply_title(const char *dst_dir, const char *title, int *not_executed)
{
    const qb_title_t *t = qb_title_find(title);
    qb_sum_t s;
    char games[MAX_PATH], *slash;
    DWORD raw;
    int present, mode;

    if (not_executed)
        *not_executed = 0;
    if (!t || !host_manages_this_box())
        return 0;
    qbinds_init();
    mode = qb_switch(&present, &raw);
    if (mode == QB_MODE_HANDS_OFF)
        return 0;
    /* dst_dir is <games>\<title>: the games folder is its parent. */
    lstrcpynA(games, dst_dir, sizeof(games));
    slash = strrchr(games, '\\');
    if (!slash)
        return 0;
    *slash = 0;
    memset(&s, 0, sizeof(s));
    EnterCriticalSection(&g_qb_lock);
    qb_do_title(games, t, mode, 1, &s, NULL);
    LeaveCriticalSection(&g_qb_lock);
    if (not_executed)
        *not_executed = s.not_executed;
    return s.written;
}

/* Copy the next blank-separated word of *p into out; 0 when there is none.
 * (No strtok_s: Win98's msvcrt.dll does not export it, and one unresolved
 * import stops the whole EXE loading there - see ntdyn.h.) */
static int qb_word(const char **p, char *out, size_t cch)
{
    size_t n = 0;
    const char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s) {
        *p = s;
        out[0] = 0;
        return 0;
    }
    while (*s && *s != ' ' && *s != '\t') {
        if (n + 1 < cch)
            out[n++] = *s;
        s++;
    }
    out[n] = 0;
    *p = s;
    return 1;
}

/* QBINDS [title] - read-only report.  QBINDS apply [title] - enforce now. */
void handle_qbinds(SOCKET sock, const char *args)
{
    char ws[3][64], games[200], why[200], state[400], last[400];
    const char *p = args ? args : "";
    const char *w1, *w2;
    const qb_title_t *only = NULL;
    int apply = 0, present, mode, i, nw = 0;
    DWORD raw;
    qb_sum_t s;
    json_t j;
    char *out;

    while (nw < 3 && qb_word(&p, ws[nw], sizeof(ws[nw])))
        nw++;
    w1 = nw > 0 ? ws[0] : NULL;
    w2 = nw > 1 ? ws[1] : NULL;
    if (w1 && _stricmp(w1, "apply") == 0) {
        apply = 1;
        w1 = w2;
        w2 = nw > 2 ? ws[2] : NULL;
    }
    if (w2) {
        send_error_response(sock, "QBINDS: usage QBINDS [title] | QBINDS apply [title]");
        return;
    }
    if (w1) {
        only = qb_title_find(w1);
        if (!only) {
            char e[200];
            _snprintf(e, sizeof(e) - 1, "QBINDS: '%s' is not a title QBINDS manages "
                      "(Quake1, Quake2Win9x, Quake2Complete)", w1);
            e[sizeof(e) - 1] = 0;
            send_error_response(sock, e);
            return;
        }
    }
    if (apply && !host_manages_this_box()) {
        send_error_response(sock, "QBINDS apply: this is a modern Windows host - the agent "
                                  "does not manage it");
        return;
    }

    qbinds_init();
    mode = qb_switch(&present, &raw);
    if (!gs_games_dir_why(games, sizeof(games), why, sizeof(why))) {
        char e[320];
        _snprintf(e, sizeof(e) - 1, "QBINDS: the games folder is unusable - %s "
                  "(never falls back to C:)", why);
        e[sizeof(e) - 1] = 0;
        if (apply)
            qb_store(e);
        send_error_response(sock, e);
        return;
    }

    memset(&s, 0, sizeof(s));
    json_init(&j);
    json_object_start(&j);
    json_kv_bool(&j, "enabled", mode == QB_MODE_WASD);
    json_kv_str(&j, "mode", qb_mode_name(mode));
    json_key(&j, "switch");
    if (present)
        json_uint(&j, raw);
    else
        json_null(&j);
    json_kv_str(&j, "games_dir", games);
    json_kv_str(&j, "file", QB_FILE);
    json_kv_int(&j, "version", QB_VERSION);
    json_kv_bool(&j, "applied", apply);
    json_key(&j, "titles");
    json_array_start(&j);
    EnterCriticalSection(&g_qb_lock);
    for (i = 0; i < QB_NTITLES; i++)
        if (!only || only == &qb_titles[i])
            qb_do_title(games, &qb_titles[i], mode, apply, &s, &j);
    LeaveCriticalSection(&g_qb_lock);
    json_array_end(&j);
    json_key(&j, "summary");
    json_object_start(&j);
    json_kv_int(&j, "gamedirs", s.gamedirs);
    json_kv_int(&j, "effective", s.effective);
    json_kv_int(&j, "written", s.written);
    json_kv_int(&j, "failed", s.failed);
    json_kv_int(&j, "not_executed", s.not_executed);
    json_kv_int(&j, "unbindall", s.unbindall);
    json_object_end(&j);
    qb_state(state, sizeof(state), mode, apply, &s);
    json_kv_str(&j, "state", state);
    if (apply) {
        if (s.failed)
            log_msg(LOG_QB, "*** QUAKE BINDS NOT APPLIED *** %s", state);
        else
            log_msg(LOG_QB, "QBINDS apply: %s", state);
        qb_store(state);
    }
    qb_load(last, sizeof(last));
    json_kv_str(&j, "last_pass", last);
    json_object_end(&j);
    out = json_finish(&j);
    if (!out) {
        send_error_response(sock, "QBINDS: out of memory");
        return;
    }
    send_text_response(sock, out);
    HeapFree(GetProcessHeap(), 0, out);
}
