/*
 * log.c - Thread-safe verbose logging with timestamps + rotation.
 *
 * File logging is ON BY DEFAULT (this is deliberate: on a silent startup
 * failure -- e.g. the Win98 loader refusing the EXE, or an early exit before
 * the console is even visible -- stderr is useless, so we need a persistent
 * on-disk trail). The log lives next to the executable (agent.log) and is
 * size-capped with a single rolled backup (agent.log.1), so it can be left
 * on permanently without growing without bound.
 *
 * Format: [HH:MM:SS][TAG] message
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "log.h"

static CRITICAL_SECTION g_log_cs;
static FILE *g_log_file = NULL;
static int  g_log_initialized = 0;
static char g_log_path[MAX_PATH] = "";   /* resolved active log path */
static long g_log_bytes = 0;             /* bytes in the current file */

/* Roll the log when it reaches this size, keeping one backup (.1). So the
 * on-disk footprint is bounded at ~2x this. 512 KB keeps plenty of history
 * while staying tiny even on a 383 MB-class box. */
#define LOG_MAX_BYTES  (512L * 1024L)

/* Local copy helper — keeps log.c self-contained (no util.h dependency), so
 * it's safe to call from early startup and the exception filter. */
static void log_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    for (; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Compute the default log path: <dir of the running exe>\agent.log. Falling
 * back to a fixed C:\ path if the module path can't be resolved. This keeps
 * the log next to the binary wherever it was installed (usually
 * C:\RETRO_AGENT), and works before any share is mapped. */
static void default_log_path(char *buf, DWORD cap)
{
    char mod[MAX_PATH];
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, mod, sizeof(mod));
    if (n == 0 || n >= sizeof(mod)) {
        _snprintf(buf, cap, "C:\\retro_agent.log");
        buf[cap - 1] = '\0';
        return;
    }
    slash = strrchr(mod, '\\');
    if (slash)
        *slash = '\0';
    else
        mod[0] = '\0';
    _snprintf(buf, cap, "%s%sagent.log", mod, mod[0] ? "\\" : "");
    buf[cap - 1] = '\0';
}

/* Rotate <path> -> <path>.1 (deleting any prior .1). Best-effort; ignores
 * failures (e.g. the backup being locked). Caller must NOT hold g_log_file
 * open on `path`. */
static void rotate_files(const char *path)
{
    char bak[MAX_PATH + 4];
    _snprintf(bak, sizeof(bak), "%s.1", path);
    bak[sizeof(bak) - 1] = '\0';
    DeleteFileA(bak);              /* ok if it doesn't exist */
    MoveFileA(path, bak);         /* ok if it fails; we reopen fresh below */
}

/* Open (or reopen) the log file for append, recording its current size so the
 * rotation counter is accurate even across restarts. Caller holds the CS
 * (or is in single-threaded init). */
static void open_log(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_log_file = fopen(g_log_path, "a");
    g_log_bytes = 0;
    if (g_log_file) {
        long pos;
        fseek(g_log_file, 0, SEEK_END);
        pos = ftell(g_log_file);
        if (pos > 0)
            g_log_bytes = pos;
    }
}

void log_init(const char *logfile)
{
    if (!g_log_initialized) {
        InitializeCriticalSection(&g_log_cs);
        g_log_initialized = 1;
    }

    /* logfile == NULL  -> default rotating file next to the exe (the common
     * case; file logging is on by default). A non-empty logfile overrides the
     * path (e.g. the -l flag). */
    if (logfile && logfile[0])
        log_strcpy(g_log_path, logfile, sizeof(g_log_path));
    else
        default_log_path(g_log_path, sizeof(g_log_path));

    /* Roll first if the existing file is already at/over the cap, so a fresh
     * run starts with headroom. */
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(g_log_path, GetFileExInfoStandard, &fad)
                && fad.nFileSizeHigh == 0
                && fad.nFileSizeLow >= (DWORD)LOG_MAX_BYTES) {
            rotate_files(g_log_path);
        }
    }

    open_log();

    /* Fallback: if the primary path isn't writable (e.g. the exe was launched
     * straight off a read-only share), retry in the temp dir so we still get
     * a log somewhere rather than silently losing it. */
    if (!g_log_file) {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n > 0 && n < sizeof(tmp)) {
            _snprintf(g_log_path, sizeof(g_log_path), "%sretro_agent.log", tmp);
            g_log_path[sizeof(g_log_path) - 1] = '\0';
            open_log();
        }
    }

    if (!g_log_file)
        fprintf(stderr, "[LOG] WARNING: cannot open log file: %s\n", g_log_path);
}

/* Expose the resolved log path so startup code can print it to the console
 * ("progress log: <path>") — helps an operator find it on the box. */
const char *log_path(void)
{
    return g_log_path;
}

void log_msg(const char *tag, const char *fmt, ...)
{
    SYSTEMTIME st;
    char prefix[64];
    char msg[2048];
    int plen, mlen;
    va_list ap;

    if (!g_log_initialized) return;

    GetLocalTime(&st);
    plen = _snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u][%-5s] ",
                     st.wHour, st.wMinute, st.wSecond, tag);
    if (plen < 0 || plen >= (int)sizeof(prefix)) plen = (int)sizeof(prefix) - 1;
    prefix[sizeof(prefix) - 1] = '\0';

    va_start(ap, fmt);
    mlen = _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (mlen < 0 || mlen >= (int)sizeof(msg)) mlen = (int)sizeof(msg) - 1;
    msg[sizeof(msg) - 1] = '\0';

    EnterCriticalSection(&g_log_cs);

    fprintf(stderr, "%s%s\n", prefix, msg);
    fflush(stderr);

    if (g_log_file) {
        fprintf(g_log_file, "%s%s\n", prefix, msg);
        fflush(g_log_file);
        g_log_bytes += plen + mlen + 1;

        /* Roll when the active file passes the cap, so a long-running (or
         * chatty) agent never grows the log unbounded. */
        if (g_log_bytes >= LOG_MAX_BYTES && g_log_path[0]) {
            fclose(g_log_file);
            g_log_file = NULL;
            rotate_files(g_log_path);
            open_log();
        }
    }

    LeaveCriticalSection(&g_log_cs);
}
