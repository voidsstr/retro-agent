/*
 * gamesync.c - provision the game library onto a freshly installed box.
 *
 * WHY THIS EXISTS. The games used to ride along inside the XP image as an
 * $OEM$ payload, which text-mode setup copies to C: before the machine has
 * ever booted. That does not scale: the library is 6.4 GB today and growing,
 * setup copies it over SMB1 at the ~2.3 MB/s these Pentium III boxes actually
 * manage, and it all has to fit on a period disk. Worse, a single bad file in
 * the payload fails the whole OS install, because to setup it is not "a game
 * that did not copy", it is "a source file is missing".
 *
 * So the OS image is lean and this runs afterwards instead. By the time it
 * does, the machine has a real network stack, the agent is supervising the
 * copy, a failure costs one game rather than the install, and we can look at
 * the disk we actually landed on and skip what will not fit.
 *
 * FIRST-BOOT DETECTION is a marker file, not a registry flag: the agent has to
 * behave identically on 9x, where the Run key fires only at logon and nothing
 * supervises us. Marker absent means "new install, provision now".
 *
 * PROGRESS is logged continuously - overall percent, the title and file in
 * hand, and the measured transfer rate - because a 6 GB copy over SMB1 on a
 * 500 MHz machine takes a long while, and an operator watching a silent agent
 * cannot tell a slow copy from a wedged one. Speed comes from a sliding
 * window, not a cumulative average, so a stall shows up as the rate falling
 * rather than being hidden by a good first minute.
 *
 * The copy is resumable. Any file already present at the right size is
 * skipped, so a machine that lost power halfway does not start over.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

#define LOG_GS "GAMESYNC"

/* The library the fleet publishes pre-installed game trees to. Overridable -
 * see gs_library_path() - because an address baked into a binary is a promise
 * we cannot keep across a NAS move. */
#define GS_DEFAULT_LIBRARY "\\\\192.168.1.122\\files\\Files\\Games-Library"
#define GS_DEST            "C:\\Games"
#define GS_MARKER          "C:\\RETRO_AGENT\\gamesync.done"
#define GS_INI             "C:\\RETRO_AGENT\\gamesync.ini"

#define GS_CHUNK           (64u * 1024u)
/* Leave the OS room to breathe; filling C: to the last byte breaks XP in
 * confusing ways long before it reports "disk full". */
#define GS_FREE_MARGIN     ((__int64)300 * 1024 * 1024)
#define GS_LOG_EVERY_MS    2000
#define GS_FIRST_DELAY_MS  20000

enum { GS_IDLE = 0, GS_SIZING, GS_COPYING, GS_DONE, GS_FAILED, GS_SKIPPED };

typedef struct {
    int     state;
    char    title[128];
    char    file[260];
    __int64 total_bytes;
    __int64 done_bytes;
    int     total_titles;
    int     done_titles;
    int     skipped_titles;
    int     failed_files;
    DWORD   started;
    double  mbps;
    char    message[256];
} gs_state_t;

static CRITICAL_SECTION g_gs_lock;
static int       g_gs_lock_ready;
static gs_state_t g_gs;
static volatile LONG g_gs_running;
static volatile LONG g_gs_abort;

/* Sliding-window rate measurement, touched only by the worker thread. */
static DWORD   g_win_tick;
static __int64 g_win_bytes;
static DWORD   g_last_log;

/* ---------------------------------------------------------------------- */
/* small helpers                                                           */
/* ---------------------------------------------------------------------- */

static void gs_set_msg(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&g_gs_lock);
    va_start(ap, fmt);
    _vsnprintf(g_gs.message, sizeof(g_gs.message) - 1, fmt, ap);
    va_end(ap);
    g_gs.message[sizeof(g_gs.message) - 1] = 0;
    LeaveCriticalSection(&g_gs_lock);
}

static int gs_file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF;
}

static __int64 gs_file_size(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    __int64 sz;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    sz = ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    FindClose(h);
    return sz;
}

/* GetDiskFreeSpaceEx is absent on the earliest 95 builds, so bind it at run
 * time and fall back to the cluster-arithmetic call. Reporting "no idea" as
 * a huge number would be worse than useless here - it would let us fill the
 * disk - so a total failure to measure returns -1 and the caller copies
 * anyway rather than refusing everything. */
typedef BOOL (WINAPI *pGDFSE)(LPCSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);

static __int64 gs_free_bytes(const char *root)
{
    static pGDFSE fn;
    static int    looked;
    ULARGE_INTEGER avail, total, freeb;
    DWORD spc, bps, freec, totalc;

    if (!looked) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k)
            fn = (pGDFSE)GetProcAddress(k, "GetDiskFreeSpaceExA");
        looked = 1;
    }
    if (fn) {
        if (fn(root, &avail, &total, &freeb))
            return (__int64)avail.QuadPart;
    }
    if (GetDiskFreeSpaceA(root, &spc, &bps, &freec, &totalc))
        return (__int64)freec * (__int64)spc * (__int64)bps;
    return -1;
}

static void gs_mkdir_p(const char *path)
{
    char tmp[MAX_PATH];
    int  i, n;

    n = lstrlenA(path);
    if (n <= 0 || n >= (int)sizeof(tmp))
        return;
    lstrcpynA(tmp, path, sizeof(tmp));
    /* Skip the UNC or drive prefix so we never try to create "\\server". */
    i = (tmp[0] == '\\' && tmp[1] == '\\') ? 2 : 0;
    for (; tmp[i]; i++) {
        if (tmp[i] == '\\' && i > 2) {
            tmp[i] = 0;
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static void gs_library_path(char *out, DWORD cch)
{
    HANDLE h;
    char   buf[512];
    DWORD  got = 0;
    char  *p;

    lstrcpynA(out, GS_DEFAULT_LIBRARY, cch);

    h = CreateFileA(GS_INI, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        p = buf;
        while (*p) {
            if (str_starts_with(p, "library=")) {
                char *q = p + 8, *e;
                e = q;
                while (*e && *e != '\r' && *e != '\n')
                    e++;
                *e = 0;
                if (*q)
                    lstrcpynA(out, q, cch);
                break;
            }
            while (*p && *p != '\n')
                p++;
            if (*p)
                p++;
        }
    }
    CloseHandle(h);
}

/* ---------------------------------------------------------------------- */
/* sizing and copying                                                      */
/* ---------------------------------------------------------------------- */

static __int64 gs_dir_size(const char *dir, int *files)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], sub[MAX_PATH];
    __int64 total = 0;

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;
        _snprintf(sub, sizeof(sub) - 1, "%s\\%s", dir, fd.cFileName);
        sub[sizeof(sub) - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += gs_dir_size(sub, files);
        } else {
            total += ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (files)
                (*files)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}

static void gs_note_progress(__int64 added)
{
    DWORD now = GetTickCount();
    DWORD dt;
    __int64 total, done;
    int pct;
    char title[128], file[260];
    double mbps;

    EnterCriticalSection(&g_gs_lock);
    g_gs.done_bytes += added;
    total = g_gs.total_bytes;
    done  = g_gs.done_bytes;
    lstrcpynA(title, g_gs.title, sizeof(title));
    lstrcpynA(file,  g_gs.file,  sizeof(file));
    LeaveCriticalSection(&g_gs_lock);

    g_win_bytes += added;
    dt = now - g_win_tick;
    /* Recompute the rate about once a second. A cumulative average would let
     * a fast first minute mask a stall for a long time. */
    if (dt >= 1000) {
        mbps = ((double)g_win_bytes / 1048576.0) / ((double)dt / 1000.0);
        EnterCriticalSection(&g_gs_lock);
        g_gs.mbps = mbps;
        LeaveCriticalSection(&g_gs_lock);
        g_win_tick  = now;
        g_win_bytes = 0;
    }

    if (now - g_last_log >= GS_LOG_EVERY_MS) {
        g_last_log = now;
        EnterCriticalSection(&g_gs_lock);
        mbps = g_gs.mbps;
        LeaveCriticalSection(&g_gs_lock);
        pct = total > 0 ? (int)((done * 100) / total) : 0;
        log_msg(LOG_GS, "%3d%% | %I64d/%I64d MB | %.2f MB/s | %s | %s",
                pct, done / 1048576, total / 1048576, mbps, title, file);
    }
}

static int gs_copy_file(const char *src, const char *dst, __int64 src_size)
{
    HANDLE hs, hd;
    char  *buf;
    DWORD  rd, wr;
    int    ok = 1;
    __int64 already;

    /* Resume: an identical-sized file is treated as done. Size alone is a
     * weak check, but these are immutable release trees - and the alternative,
     * hashing 6 GB over SMB1 on a P3, costs more than it could ever save. */
    already = gs_file_size(dst);
    if (already >= 0 && already == src_size) {
        gs_note_progress(src_size);
        return 1;
    }

    hs = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "open failed (%lu): %s", GetLastError(), src);
        return 0;
    }
    hd = CreateFileA(dst, GENERIC_WRITE, 0, NULL,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hd == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "create failed (%lu): %s", GetLastError(), dst);
        CloseHandle(hs);
        return 0;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_CHUNK);
    if (!buf) {
        CloseHandle(hs);
        CloseHandle(hd);
        return 0;
    }
    for (;;) {
        if (InterlockedExchange((LONG *)&g_gs_abort, g_gs_abort) != 0) {
            ok = 0;
            break;
        }
        if (!ReadFile(hs, buf, GS_CHUNK, &rd, NULL)) {
            log_msg(LOG_GS, "read failed (%lu): %s", GetLastError(), src);
            ok = 0;
            break;
        }
        if (rd == 0)
            break;
        if (!WriteFile(hd, buf, rd, &wr, NULL) || wr != rd) {
            log_msg(LOG_GS, "write failed (%lu): %s", GetLastError(), dst);
            ok = 0;
            break;
        }
        gs_note_progress((__int64)rd);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(hs);
    CloseHandle(hd);
    if (!ok)
        DeleteFileA(dst);      /* never leave a truncated file looking complete */
    return ok;
}

static int gs_copy_tree(const char *src, const char *dst)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    int    ok = 1;
    __int64 sz;

    gs_mkdir_p(dst);
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", src);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;
        if (g_gs_abort)
            { ok = 0; break; }
        _snprintf(s, sizeof(s) - 1, "%s\\%s", src, fd.cFileName);
        _snprintf(d, sizeof(d) - 1, "%s\\%s", dst, fd.cFileName);
        s[sizeof(s) - 1] = 0;
        d[sizeof(d) - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!gs_copy_tree(s, d))
                ok = 0;
        } else {
            EnterCriticalSection(&g_gs_lock);
            lstrcpynA(g_gs.file, fd.cFileName, sizeof(g_gs.file));
            LeaveCriticalSection(&g_gs_lock);
            sz = ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (!gs_copy_file(s, d, sz)) {
                EnterCriticalSection(&g_gs_lock);
                g_gs.failed_files++;
                LeaveCriticalSection(&g_gs_lock);
                ok = 0;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* per-title registry merge                                                */
/* ---------------------------------------------------------------------- */

/* Some games will not launch from a copied directory alone. Jedi Knight and
 * Mysteries of the Sith each open "<CD Path>\jk_.cd" as a disc-presence check
 * and refuse to start without the registry value pointing at the marker file
 * that ships in their tree; Red Alert 2 and others want an install path. A
 * title that needs this carries install.reg at its root, written against
 * C:\Games\<Title>, and we merge it here - immediately after that title's
 * files land, so a failure is attributable to the title rather than showing up
 * as a mysteriously broken game weeks later.
 *
 * Merging is best-effort by design: a game that fails to register is still
 * worth having on disk, and refusing to continue would cost the other twenty. */
static void gs_merge_reg(const char *dst_dir, const char *title)
{
    char reg_path[MAX_PATH];
    char cmd[MAX_PATH + 64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    _snprintf(reg_path, sizeof(reg_path) - 1, "%s\\install.reg", dst_dir);
    reg_path[sizeof(reg_path) - 1] = 0;
    if (!gs_file_exists(reg_path))
        return;

    _snprintf(cmd, sizeof(cmd) - 1, "regedit /s \"%s\"", reg_path);
    cmd[sizeof(cmd) - 1] = 0;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_msg(LOG_GS, "%s: cannot run regedit (%lu) - game may not launch",
                title, GetLastError());
        return;
    }
    /* regedit /s is quick, but never wait forever on it. */
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT)
        log_msg(LOG_GS, "%s: regedit still running after 60s, leaving it", title);
    else if (GetExitCodeProcess(pi.hProcess, &code) && code != 0)
        log_msg(LOG_GS, "%s: regedit exited %lu", title, code);
    else
        log_msg(LOG_GS, "%s: merged install.reg", title);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* ---------------------------------------------------------------------- */
/* the run                                                                 */
/* ---------------------------------------------------------------------- */

static void gs_write_marker(int titles)
{
    HANDLE h;
    char   line[256];
    DWORD  wr;
    SYSTEMTIME st;

    gs_mkdir_p("C:\\RETRO_AGENT");
    h = CreateFileA(GS_MARKER, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    GetLocalTime(&st);
    _snprintf(line, sizeof(line) - 1,
              "provisioned %04d-%02d-%02d %02d:%02d:%02d titles=%d\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              titles);
    line[sizeof(line) - 1] = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &wr, NULL);
    CloseHandle(h);
}

static void gs_run(const char *library)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    char   titles[64][128];
    __int64 sizes[64];
    int    n = 0, i, files = 0, ok_titles = 0;
    __int64 grand = 0, freeb;

    g_gs_abort = 0;
    g_win_tick = GetTickCount();
    g_win_bytes = 0;
    g_last_log = 0;

    EnterCriticalSection(&g_gs_lock);
    memset(&g_gs, 0, sizeof(g_gs));
    g_gs.state   = GS_SIZING;
    g_gs.started = GetTickCount();
    LeaveCriticalSection(&g_gs_lock);

    log_msg(LOG_GS, "library: %s", library);
    gs_set_msg("enumerating library");

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", library);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "cannot reach library (%lu): %s", GetLastError(), library);
        EnterCriticalSection(&g_gs_lock);
        g_gs.state = GS_FAILED;
        LeaveCriticalSection(&g_gs_lock);
        gs_set_msg("library unreachable - will retry");
        return;
    }
    do {
        if (fd.cFileName[0] == '.')
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (n >= 64)
            break;
        lstrcpynA(titles[n], fd.cFileName, sizeof(titles[0]));
        _snprintf(src, sizeof(src) - 1, "%s\\%s", library, fd.cFileName);
        src[sizeof(src) - 1] = 0;
        sizes[n] = gs_dir_size(src, &files);
        grand += sizes[n];
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (n == 0) {
        log_msg(LOG_GS, "library is empty - nothing to do");
        EnterCriticalSection(&g_gs_lock);
        g_gs.state = GS_DONE;
        LeaveCriticalSection(&g_gs_lock);
        gs_write_marker(0);
        return;
    }

    freeb = gs_free_bytes("C:\\");
    log_msg(LOG_GS, "%d title(s), %d file(s), %I64d MB to copy; C: has %I64d MB free",
            n, files, grand / 1048576,
            freeb < 0 ? (__int64)-1 : freeb / 1048576);

    EnterCriticalSection(&g_gs_lock);
    g_gs.state        = GS_COPYING;
    g_gs.total_titles = n;
    g_gs.total_bytes  = grand;
    LeaveCriticalSection(&g_gs_lock);

    gs_mkdir_p(GS_DEST);

    for (i = 0; i < n; i++) {
        if (g_gs_abort) {
            log_msg(LOG_GS, "aborted by request");
            break;
        }
        /* Re-measure per title: earlier titles have just consumed space, and
         * on a period disk the difference decides whether this one fits. */
        freeb = gs_free_bytes("C:\\");
        if (freeb >= 0 && sizes[i] + GS_FREE_MARGIN > freeb) {
            log_msg(LOG_GS, "SKIP %s - needs %I64d MB, only %I64d MB free",
                    titles[i], sizes[i] / 1048576, freeb / 1048576);
            EnterCriticalSection(&g_gs_lock);
            g_gs.skipped_titles++;
            /* Its bytes are never going to arrive; drop them from the target
             * so the overall percentage still reaches 100. */
            g_gs.total_bytes -= sizes[i];
            LeaveCriticalSection(&g_gs_lock);
            continue;
        }
        EnterCriticalSection(&g_gs_lock);
        lstrcpynA(g_gs.title, titles[i], sizeof(g_gs.title));
        g_gs.file[0] = 0;
        LeaveCriticalSection(&g_gs_lock);
        gs_set_msg("copying %s", titles[i]);
        log_msg(LOG_GS, "==> %s (%I64d MB)", titles[i], sizes[i] / 1048576);

        _snprintf(src, sizeof(src) - 1, "%s\\%s", library, titles[i]);
        _snprintf(dst, sizeof(dst) - 1, "%s\\%s", GS_DEST, titles[i]);
        src[sizeof(src) - 1] = 0;
        dst[sizeof(dst) - 1] = 0;

        if (gs_copy_tree(src, dst)) {
            ok_titles++;
            gs_merge_reg(dst, titles[i]);
        } else {
            log_msg(LOG_GS, "%s finished with errors", titles[i]);
        }

        EnterCriticalSection(&g_gs_lock);
        g_gs.done_titles++;
        LeaveCriticalSection(&g_gs_lock);
    }

    EnterCriticalSection(&g_gs_lock);
    g_gs.state = g_gs_abort ? GS_FAILED : GS_DONE;
    i = g_gs.failed_files;
    LeaveCriticalSection(&g_gs_lock);

    log_msg(LOG_GS, "done: %d/%d title(s) copied, %d skipped, %d file error(s)",
            ok_titles, n, g_gs.skipped_titles, i);
    gs_set_msg("complete - %d title(s)", ok_titles);

    /* Only claim the box is provisioned if nothing failed. A marker written
     * over a partial run would make the next boot skip the retry. */
    if (!g_gs_abort && i == 0)
        gs_write_marker(ok_titles);
}

/* ---------------------------------------------------------------------- */
/* thread and handler                                                      */
/* ---------------------------------------------------------------------- */

typedef struct { char library[MAX_PATH]; } gs_arg_t;

static DWORD WINAPI gs_worker(LPVOID param)
{
    gs_arg_t *a = (gs_arg_t *)param;
    char lib[MAX_PATH];

    lstrcpynA(lib, a->library, sizeof(lib));
    HeapFree(GetProcessHeap(), 0, a);

    gs_run(lib);
    InterlockedExchange((LONG *)&g_gs_running, 0);
    return 0;
}

static int gs_start(const char *library)
{
    gs_arg_t *a;
    HANDLE    th;
    DWORD     tid;

    if (InterlockedCompareExchange((LONG *)&g_gs_running, 1, 0) != 0)
        return 0;                          /* already running */

    a = (gs_arg_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(gs_arg_t));
    if (!a) {
        InterlockedExchange((LONG *)&g_gs_running, 0);
        return 0;
    }
    if (library && library[0])
        lstrcpynA(a->library, library, sizeof(a->library));
    else
        gs_library_path(a->library, sizeof(a->library));

    th = CreateThread(NULL, 0, gs_worker, a, 0, &tid);
    if (!th) {
        HeapFree(GetProcessHeap(), 0, a);
        InterlockedExchange((LONG *)&g_gs_running, 0);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

void gamesync_init(void)
{
    if (!g_gs_lock_ready) {
        InitializeCriticalSection(&g_gs_lock);
        g_gs_lock_ready = 1;
    }
}

DWORD WINAPI gamesync_thread(LPVOID param)
{
    (void)param;
    /* Let the desktop settle and the redirector come up before touching a
     * UNC path; on a fresh XP logon the network is not ready immediately. */
    Sleep(GS_FIRST_DELAY_MS);

    if (gs_file_exists(GS_MARKER)) {
        log_msg(LOG_GS, "already provisioned (%s present) - idle", GS_MARKER);
        return 0;
    }
    log_msg(LOG_GS, "new install detected - provisioning game library");
    for (;;) {
        if (gs_file_exists(GS_MARKER))
            return 0;
        if (!g_gs_running)
            gs_start(NULL);
        /* If the library was unreachable the run failed fast; wait before
         * trying again rather than hammering a NAS that may still be waking. */
        Sleep(120000);
    }
}

void handle_gamesync(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    char   json[1024];
    gs_state_t s;
    const char *names[] = { "idle", "sizing", "copying", "done", "failed", "skipped" };
    int    pct, elapsed;
    char   lib[MAX_PATH];

    gamesync_init();

    if (str_starts_with(a, "START")) {
        const char *unc = str_skip_spaces(a + 5);
        if (gs_start(unc[0] ? unc : NULL))
            send_text_response(sock, "OK started");
        else
            send_text_response(sock, "OK already running");
        return;
    }
    if (str_starts_with(a, "ABORT")) {
        InterlockedExchange((LONG *)&g_gs_abort, 1);
        send_text_response(sock, "OK aborting");
        return;
    }
    if (str_starts_with(a, "RESET")) {
        /* Forget that this box was provisioned, so the next start re-runs. */
        DeleteFileA(GS_MARKER);
        send_text_response(sock, "OK marker cleared");
        return;
    }
    if (str_starts_with(a, "LIBRARY")) {
        gs_library_path(lib, sizeof(lib));
        send_text_response(sock, lib);
        return;
    }

    EnterCriticalSection(&g_gs_lock);
    s = g_gs;
    LeaveCriticalSection(&g_gs_lock);

    pct = s.total_bytes > 0 ? (int)((s.done_bytes * 100) / s.total_bytes) : 0;
    elapsed = s.started ? (int)((GetTickCount() - s.started) / 1000) : 0;

    _snprintf(json, sizeof(json) - 1,
        "{\"state\":\"%s\",\"percent\":%d,"
        "\"titles_done\":%d,\"titles_total\":%d,\"titles_skipped\":%d,"
        "\"mb_done\":%I64d,\"mb_total\":%I64d,\"mbps\":%.2f,"
        "\"current_title\":\"%s\",\"current_file\":\"%s\","
        "\"failed_files\":%d,\"elapsed_s\":%d,\"provisioned\":%s,"
        "\"message\":\"%s\"}",
        names[(s.state >= 0 && s.state <= GS_SKIPPED) ? s.state : 0],
        pct, s.done_titles, s.total_titles, s.skipped_titles,
        s.done_bytes / 1048576, s.total_bytes / 1048576, s.mbps,
        s.title, s.file, s.failed_files, elapsed,
        gs_file_exists(GS_MARKER) ? "true" : "false",
        s.message);
    json[sizeof(json) - 1] = 0;
    send_text_response(sock, json);
}
