/*
 * log.c - Thread-safe logging with timestamps + rotation, built on RAW Win32
 * file I/O (CreateFile/WriteFile/FlushFileBuffers) -- deliberately NOT the C
 * runtime's stdio.
 *
 * Why raw Win32 and not fopen/fprintf/stderr:
 *   - On a silent startup crash (esp. on Win98) we MUST have whatever was
 *     logged already on disk. msvcrt's fprintf buffers in the CRT; worse, a
 *     write to a stdio stream (particularly stderr) can itself fault or block
 *     on Win9x depending on how the process was launched, which can take down
 *     log_msg() BEFORE it ever reaches the file -- leaving a created-but-empty
 *     log (exactly the symptom we hit on the Deskpro 2000).
 *   - WriteFile + FlushFileBuffers commits every line to disk immediately, so
 *     the LAST line in the log is always the last thing the agent did before
 *     it died. Combined with the startup breadcrumbs in main.c/agent_run,
 *     that pinpoints any crash location even when the unhandled-exception
 *     filter isn't reliably called (which is the case on Win9x).
 *
 * File logging is ON BY DEFAULT: <exe dir>\agent.log, size-capped (~512KB)
 * with one rolled backup (agent.log.1). -l overrides the path.
 *
 * Format: [HH:MM:SS][TAG] message
 */

#include <windows.h>
#include <stdio.h>     /* _vsnprintf / _snprintf: pure buffer formatting, no stdio streams */
#include <stdarg.h>
#include <string.h>

#include "log.h"

static CRITICAL_SECTION g_log_cs;
static int    g_log_initialized = 0;
static HANDLE g_log_h = INVALID_HANDLE_VALUE;   /* raw file handle */
static char   g_log_path[MAX_PATH] = "";
static long   g_log_bytes = 0;

/* Roll at this size, keeping one .1 backup (footprint bounded at ~2x). */
#define LOG_MAX_BYTES  (512L * 1024L)

/* Local strcpy (no util.h dependency, safe from the crash logger). */
static void log_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    for (; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Default log path: <dir of the running exe>\agent.log. */
static void default_log_path(char *buf, DWORD cap)
{
    char mod[MAX_PATH];
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, mod, sizeof(mod));
    if (n == 0 || n >= sizeof(mod)) {
        log_strcpy(buf, "C:\\retro_agent.log", cap);
        return;
    }
    slash = strrchr(mod, '\\');
    if (slash) *slash = '\0'; else mod[0] = '\0';
    _snprintf(buf, cap, "%s%sagent.log", mod, mod[0] ? "\\" : "");
    buf[cap - 1] = '\0';
}

/* Append bytes to the log handle and (optionally) echo to a valid console
 * stderr. Commits to disk immediately for crash-durability. Does NOT take the
 * lock -- callers manage that (the crash logger deliberately runs lock-free). */
static void raw_out(const char *s, DWORD len)
{
    DWORD wr;
    HANDLE e;
    if (g_log_h != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_log_h, 0, NULL, FILE_END);
        if (WriteFile(g_log_h, s, len, &wr, NULL)) {
            FlushFileBuffers(g_log_h);   /* durability: on disk before we return */
            g_log_bytes += (long)len;
        }
    }
    /* Best-effort console echo; guarded so an invalid handle (GUI launch)
     * can never fault us. */
    e = GetStdHandle(STD_ERROR_HANDLE);
    if (e != NULL && e != INVALID_HANDLE_VALUE)
        WriteFile(e, s, len, &wr, NULL);
}

static void rotate_files(const char *path)
{
    char bak[MAX_PATH + 4];
    _snprintf(bak, sizeof(bak), "%s.1", path);
    bak[sizeof(bak) - 1] = '\0';
    DeleteFileA(bak);
    MoveFileA(path, bak);
}

static void open_log(void)
{
    if (g_log_h != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_h);
        g_log_h = INVALID_HANDLE_VALUE;
    }
    /* FILE_SHARE_READ|WRITE so `type`/an editor can read it while we run. */
    g_log_h = CreateFileA(g_log_path, GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_log_bytes = 0;
    if (g_log_h != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(g_log_h, NULL);
        if (sz != INVALID_FILE_SIZE) g_log_bytes = (long)sz;
        SetFilePointer(g_log_h, 0, NULL, FILE_END);
    }
}

void log_init(const char *logfile)
{
    if (!g_log_initialized) {
        InitializeCriticalSection(&g_log_cs);
        g_log_initialized = 1;
    }

    if (logfile && logfile[0])
        log_strcpy(g_log_path, logfile, sizeof(g_log_path));
    else
        default_log_path(g_log_path, sizeof(g_log_path));

    /* Pre-rotate if the existing file is already at/over the cap. */
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_log_path, GetFileExInfoStandard, &fad)
                && fad.nFileSizeHigh == 0
                && fad.nFileSizeLow >= (DWORD)LOG_MAX_BYTES)
            rotate_files(g_log_path);
    }

    open_log();

    /* Fallback to the temp dir if the primary path isn't writable (e.g. the
     * exe was launched off a read-only share). */
    if (g_log_h == INVALID_HANDLE_VALUE) {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n > 0 && n < sizeof(tmp)) {
            _snprintf(g_log_path, sizeof(g_log_path), "%sretro_agent.log", tmp);
            g_log_path[sizeof(g_log_path) - 1] = '\0';
            open_log();
        }
    }

    /* Immediate proof-of-write marker: if THIS line is present but nothing
     * after it, the failure is very early; if the file is truly empty, the
     * handle never opened (check the path/permissions). */
    raw_out("--- log opened (raw win32) ---\r\n", 31);
}

const char *log_path(void)
{
    return g_log_path;
}

/* Format "[HH:MM:SS][TAG] msg\r\n" into `out`; returns length. */
static int format_line(char *out, int cap, const char *tag,
                       const char *fmt, va_list ap)
{
    SYSTEMTIME st;
    char msg[1900];
    int n;
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    msg[sizeof(msg) - 1] = '\0';
    GetLocalTime(&st);
    n = _snprintf(out, cap - 1, "[%02u:%02u:%02u][%-5s] %s\r\n",
                  st.wHour, st.wMinute, st.wSecond, tag, msg);
    if (n < 0 || n >= cap) n = cap - 1;
    out[n] = '\0';
    return n;
}

void log_msg(const char *tag, const char *fmt, ...)
{
    char line[2048];
    int n;
    va_list ap;

    if (!g_log_initialized) return;

    va_start(ap, fmt);
    n = format_line(line, (int)sizeof(line), tag, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_cs);
    raw_out(line, (DWORD)n);
    if (g_log_bytes >= LOG_MAX_BYTES && g_log_path[0]) {
        if (g_log_h != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log_h);
            g_log_h = INVALID_HANDLE_VALUE;
        }
        rotate_files(g_log_path);
        open_log();
    }
    LeaveCriticalSection(&g_log_cs);
}

/* Crash logger for the unhandled-exception filter: runs LOCK-FREE (the crash
 * may have happened while a thread held g_log_cs, so taking it could deadlock)
 * and writes straight to disk. Never call this on a hot path. */
void log_crash(const char *tag, const char *fmt, ...)
{
    char line[1024];
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = format_line(line, (int)sizeof(line), tag, fmt, ap);
    va_end(ap);

    raw_out(line, (DWORD)n);   /* no CS: durability over ordering during a crash */
}
