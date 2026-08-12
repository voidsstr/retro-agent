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
 *
 * BATCHING (added for the Win98 box's 1997 IDE drive)
 * ---------------------------------------------------
 * Writing + FlushFileBuffers on EVERY line means a physical seek and platter
 * write per log entry, forever, on a machine that runs 24/7. That is a lot of
 * head movement and wear for a log nobody reads most days. So lines are
 * accumulated in memory and written in batches.
 *
 * The startup-crash property above is NOT given up to get it. Instead:
 *
 *   - Startup is UNBUFFERED. Every line goes straight to disk until the agent
 *     declares itself up (log_set_buffered(1) once the helper threads are
 *     spawned). That is the window the original comment is about - the silent
 *     Deskpro startup crash - and it still behaves exactly as before.
 *   - After that, lines batch until the buffer fills or the flush interval
 *     passes, whichever comes first.
 *   - Every path that can end the process flushes first: the unhandled
 *     exception filter (log_crash), the console control handler (which is how
 *     this agent usually dies on Win9x - somebody closes its window), and the
 *     clean shutdown path (log_shutdown).
 *   - Anything logged at "important" severity flushes immediately, so the
 *     record of a reboot, an update or a fatal error is on disk before the
 *     thing it describes happens.
 *
 * What that leaves at risk is the last few seconds of routine chatter if the
 * MACHINE dies (power loss, hard hang) - which no amount of buffering
 * discipline can protect, short of the per-line flush we are deliberately
 * removing. LOG_FLUSH_MS bounds that window.
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

/* Pending-line buffer. 8K is ~80 typical lines: big enough that idle chatter
 * costs one write every flush interval instead of one per line, small enough
 * that it is never a meaningful chunk of a 31MB machine's RAM. */
#define LOG_BUF_BYTES  8192
/* Upper bound on how much routine logging a power cut can cost. */
#define LOG_FLUSH_MS   15000

static char  g_buf[LOG_BUF_BYTES];
static int   g_buf_len = 0;
static int   g_buffered = 0;          /* 0 until the agent is up: see header */
static HANDLE g_flush_thread = NULL;
static volatile int g_flush_stop = 0;

/* Local strcpy (no util.h dependency, safe from the crash logger). */
static void log_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    for (; i < cap - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * Where the log goes when nobody says otherwise.
 *
 * There is ONE known location - C:\RETRO_AGENT\agent.log - and the agent uses
 * it whether or not it was told to. Nobody starting this by hand should have
 * to remember a -l argument to get a log, and every fleet tool that goes
 * looking for one should find it in the same place on every box.
 *
 * Order: the fleet's fixed path, then the directory the exe actually runs
 * from (for a copy running somewhere else), then the temp dir. -l still
 * overrides everything.
 */
#define AGENT_LOG_DIR   "C:\\RETRO_AGENT"
#define AGENT_LOG_FILE  AGENT_LOG_DIR "\\agent.log"

/* Can we create/append a file here? Cheap and non-destructive. */
static int path_writable(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static void default_log_path(char *buf, DWORD cap)
{
    char mod[MAX_PATH];
    char *slash;
    DWORD n;

    /* 1. The known fleet location. Create the directory if this is a fresh
     *    box - CreateDirectory on an existing one is a harmless no-op. */
    CreateDirectoryA(AGENT_LOG_DIR, NULL);
    if (path_writable(AGENT_LOG_FILE)) {
        log_strcpy(buf, AGENT_LOG_FILE, cap);
        return;
    }

    /* 2. Wherever this exe actually lives. */
    n = GetModuleFileNameA(NULL, mod, sizeof(mod));
    if (n > 0 && n < sizeof(mod)) {
        slash = strrchr(mod, '\\');
        if (slash) *slash = '\0'; else mod[0] = '\0';
        _snprintf(buf, cap, "%s%sagent.log", mod, mod[0] ? "\\" : "");
        buf[cap - 1] = '\0';
        if (path_writable(buf)) return;
    }

    /* 3. Last resort. */
    log_strcpy(buf, "C:\\retro_agent.log", cap);
}

/* Append bytes to the log handle and (optionally) echo to a valid console
 * stderr. Commits to disk immediately for crash-durability. Does NOT take the
 * lock -- callers manage that (the crash logger deliberately runs lock-free). */
/* Write straight to the file and commit. No locking, no buffering -- this is
 * the bottom of the stack, used by the flusher and by the crash logger. */
static int disk_out(const char *s, DWORD len)
{
    DWORD wr;
    if (g_log_h == INVALID_HANDLE_VALUE || len == 0) return 0;
    SetFilePointer(g_log_h, 0, NULL, FILE_END);
    if (WriteFile(g_log_h, s, len, &wr, NULL)) {
        FlushFileBuffers(g_log_h);   /* durability: on disk before we return */
        g_log_bytes += (long)len;
        return 1;
    }
    return 0;
}

/* Push whatever is pending to disk. Caller holds the lock (or is the crash
 * path, which deliberately runs lock-free). */
static void flush_locked(void)
{
    if (g_buf_len > 0) {
        /* Only drop the pending lines once they are actually on disk. A full
         * disk or a yanked network path would otherwise discard up to 8KB of
         * log silently, every interval, with nothing to show it happened. */
        if (disk_out(g_buf, (DWORD)g_buf_len))
            g_buf_len = 0;
        else if (g_buf_len >= LOG_BUF_BYTES)
            g_buf_len = 0;   /* wedged and full: drop rather than stop logging */
    }
}

/* Append a formatted line: into the pending buffer when buffered, straight to
 * disk when not. Always echoes to the console immediately -- the console is
 * free, and somebody watching the agent's window should see it live. */
static void raw_out(const char *s, DWORD len)
{
    DWORD wr;
    HANDLE e;

    if (g_buffered && g_log_h != INVALID_HANDLE_VALUE) {
        if (g_buf_len + (int)len > LOG_BUF_BYTES)
            flush_locked();
        if ((int)len <= LOG_BUF_BYTES) {
            memcpy(g_buf + g_buf_len, s, len);
            g_buf_len += (int)len;
        } else {
            disk_out(s, len);        /* a line too big to buffer: write it */
        }
    } else {
        disk_out(s, len);
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
        /* Get pending lines into the OLD file before it is rolled aside;
         * otherwise they reappear at the top of the new one, out of order. */
        flush_locked();
        if (g_log_h != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log_h);
            g_log_h = INVALID_HANDLE_VALUE;
        }
        rotate_files(g_log_path);
        open_log();
    }
    LeaveCriticalSection(&g_log_cs);
}

/* Force everything pending to disk. Safe to call from anywhere. */
void log_flush(void)
{
    if (!g_log_initialized) return;
    EnterCriticalSection(&g_log_cs);
    flush_locked();
    LeaveCriticalSection(&g_log_cs);
}

/* Periodic flusher: bounds how much routine logging a power cut can cost. */
static DWORD WINAPI log_flush_thread(LPVOID unused)
{
    (void)unused;
    while (!g_flush_stop) {
        int slept = 0;
        /* Wake often enough to stop promptly on shutdown, flush rarely. */
        while (slept < LOG_FLUSH_MS && !g_flush_stop) {
            Sleep(250);
            slept += 250;
        }
        log_flush();
    }
    return 0;
}

/*
 * Switch to batched writes. Called once the agent is up and the startup
 * window - where a silent crash has to be reconstructed from the log - has
 * passed. Before this, every line goes straight to the platter as it always
 * did; after it, lines batch and a background thread flushes them.
 */
void log_set_buffered(int on)
{
    if (!g_log_initialized) return;

    EnterCriticalSection(&g_log_cs);
    if (!on) flush_locked();          /* never strand lines in the buffer */
    g_buffered = on ? 1 : 0;
    LeaveCriticalSection(&g_log_cs);

    if (on && !g_flush_thread) {
        DWORD tid;
        g_flush_stop = 0;
        g_flush_thread = CreateThread(NULL, 0, log_flush_thread, NULL, 0, &tid);
        if (!g_flush_thread) {
            /* No flusher means nothing drains the buffer and LOG_FLUSH_MS
             * bounds nothing - lines would sit in RAM indefinitely on an idle
             * agent. Thread creation genuinely fails on the 31MB Deskpro
             * (main.c logs exactly that for dosstage), so fall back rather
             * than assume. */
            EnterCriticalSection(&g_log_cs);
            g_buffered = 0;
            flush_locked();
            LeaveCriticalSection(&g_log_cs);
            log_msg(LOG_MAIN, "log: flusher thread failed to start (%lu) - "
                    "staying unbuffered", (unsigned long)GetLastError());
        }
    }
}

/* Clean shutdown: stop batching, flush, close. */
void log_shutdown(void)
{
    if (!g_log_initialized) return;

    g_flush_stop = 1;
    if (g_flush_thread) {
        WaitForSingleObject(g_flush_thread, 2000);
        CloseHandle(g_flush_thread);
        g_flush_thread = NULL;
    }

    EnterCriticalSection(&g_log_cs);
    g_buffered = 0;
    flush_locked();
    if (g_log_h != INVALID_HANDLE_VALUE) {
        /* Mark the close while the handle is still open, so a log that ends
         * here is visibly a CLEAN exit. Without it the file just stops, which
         * reads identically to the agent being killed. */
        disk_out("--- log closed (clean shutdown) ---\r\n", 38);
        CloseHandle(g_log_h);
        g_log_h = INVALID_HANDLE_VALUE;
    }
    g_log_bytes = 0;      /* no stale count to trigger a post-close rotate */
    LeaveCriticalSection(&g_log_cs);
}

/* Crash logger for the unhandled-exception filter: runs LOCK-FREE (the crash
 * may have happened while a thread held g_log_cs, so taking it could deadlock)
 * and writes straight to disk. Never call this on a hot path. */
void log_crash(const char *tag, const char *fmt, ...)
{
    static char snap[LOG_BUF_BYTES + 1024];
    char line[1024];
    int n, pend;
    va_list ap;

    /* Whatever was batched is part of the story of this crash - it is the
     * part that says what the agent was doing beforehand. But this runs
     * LOCK-FREE (the thread that died may hold the lock), so it must not
     * mutate g_buf/g_buf_len the way flush_locked() would: another thread
     * legitimately holding the lock could be appending to them right now,
     * and two writers each doing SetFilePointer(FILE_END)+WriteFile on the
     * same handle can interleave and lose the crash record itself.
     *
     * So: SNAPSHOT the pending bytes, append the crash line to the copy, and
     * emit the whole thing in ONE write. Nothing shared is written to, and
     * the crash line cannot be torn. Worst case the pending part is a
     * slightly stale or duplicated tail - a fine price during a crash. */
    pend = g_buf_len;
    if (pend < 0 || pend > LOG_BUF_BYTES) pend = 0;
    if (pend > 0) memcpy(snap, g_buf, (size_t)pend);

    va_start(ap, fmt);
    n = format_line(line, (int)sizeof(line), tag, fmt, ap);
    va_end(ap);

    if (n > 0 && n < (int)sizeof(line)) {
        memcpy(snap + pend, line, (size_t)n);
        disk_out(snap, (DWORD)(pend + n));
    } else {
        disk_out(snap, (DWORD)pend);
    }
}
