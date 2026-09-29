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
 *
 * THE CONSOLE IS NEVER WRITTEN WHILE THE LOG LOCK IS HELD (agent 1.90.0)
 * ---------------------------------------------------------------------
 * raw_out() used to echo each line with WriteFile(stderr) under g_log_cs. A
 * console write blocks for as long as the console is not being serviced - a
 * QuickEdit/Mark selection in the agent's window, a hung conhost/csrss, a hung
 * display - and the thread stuck in it kept the lock, so every thread that
 * logs (the accept loop, every command handler, the flusher) queued behind it
 * and the agent went deaf while the OS was fine. ADMIN-PC (Win7) sat like that
 * for 35 hours after its Radeon hung, 2026-09-26.
 *
 * Now the loggers only COPY the line into a bounded ring (agent/shared/
 * conring.h) and ONE low-priority thread, log_echo_thread(), writes the ring
 * to the console with no lock held. A console that stops accepting output
 * stalls that thread alone: the ring fills, further lines are dropped and
 * counted (never waited for), and a notice marks the gap when the console
 * recovers. The file path is untouched - agent.log still gets every line.
 * Every other console touch the agent makes (the startup/progress printf's via
 * con_printf(), the window title) goes the same way, so no thread that serves
 * or logs ever calls into the console. On NT the echo thread also clears
 * QuickEdit on the agent's console, so a stray click cannot start a selection.
 * Tests: tests/native/test_conring.c (the ring), tests/native/
 * test_log_echo_thread.c (this file on real threads with a console that
 * freezes) and tests/python/test_log_console_echo.py (source invariants).
 */

#include <windows.h>
#include <stdio.h>     /* _vsnprintf / _snprintf: pure buffer formatting, no stdio streams */
#include <stdarg.h>
#include <string.h>

#include "log.h"
#include "../shared/conring.h"
#include "../shared/busytick.h"

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
static DWORD g_last_flush = 0;        /* GetTickCount of the last write-out */
static HANDLE g_flush_thread = NULL;
/* Monotonic counters for readers that need to know whether the FILE changed
 * (the share mirror in main.c): successful writes to the current file, and
 * rotations. Bumped with InterlockedIncrement - the crash logger writes
 * without the lock. */
static volatile LONG g_write_seq = 0;
static volatile LONG g_rotate_seq = 0;
static HANDLE g_flush_evt = NULL;     /* set by log_shutdown to stop the flusher */
static volatile int g_flush_stop = 0;

/* ---- console echo (see the header: never written under g_log_cs) ------ */
/* 8 KB is a screenful and a half of lines for a person watching the window:
 * enough to ride out a burst, small next to the Deskpro's 31 MB. */
#define ECHO_RING_BYTES 8192
/* How much the echo thread moves per pass - one full-size log line. */
#define ECHO_CHUNK      2048
/* A console write that has not returned after this long is reported. The
 * echo thread runs at IDLE, so this must comfortably exceed the time NT's
 * balance-set manager (~4 s) takes to boost a starved thread. */
#define ECHO_STALL_MS   30000

static char      g_echo_store[ECHO_RING_BYTES];
static conring_t g_echo;                  /* guarded by g_log_cs */
static HANDLE    g_echo_h = INVALID_HANDLE_VALUE;  /* console output */
static HANDLE    g_echo_evt = NULL;       /* auto-reset: "the ring has work" */
static HANDLE    g_echo_thread = NULL;
static volatile int g_echo_on = 0;        /* 1 while lines are queued for it */
static volatile int g_echo_stop = 0;
/* GetTickCount()|1 while the echo thread is inside a console call, else 0.
 * Read lock-free by the flusher to report a console that stopped answering. */
static volatile DWORD g_echo_busy_since = 0;
static char g_echo_title[128];            /* guarded by g_log_cs */
static int  g_echo_title_pending = 0;     /* guarded by g_log_cs */

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
        InterlockedIncrement((LONG *)&g_write_seq);
        return 1;
    }
    return 0;
}

/* Push whatever is pending to disk. Caller holds the lock (or is the crash
 * path, which deliberately runs lock-free). */
static void flush_locked(void)
{
    g_last_flush = GetTickCount();
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

/* Queue a line for the console echo thread. Caller holds g_log_cs. This is a
 * memcpy into the ring and, when the ring was empty, one SetEvent: it cannot
 * wait for the console, whatever state the console is in. A full ring drops
 * the line (the file still gets it) - see conring.h. */
static void echo_push_locked(const char *s, DWORD len)
{
    if (!g_echo_on)
        return;
    if ((conring_push(&g_echo, s, (unsigned int)len) & CONRING_WAKE)
            && g_echo_evt)
        SetEvent(g_echo_evt);
}

/* Append a formatted line: into the pending buffer when buffered, straight to
 * disk when not, and queue it for the console. The console is NOT written
 * here: this runs under g_log_cs, and a console write can block for as long
 * as the console is frozen (agent <= 1.89.1 did exactly that - see the
 * header). log_echo_thread() shows it a moment later. */
static void raw_out(const char *s, DWORD len)
{
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

    echo_push_locked(s, len);
}

static void rotate_files(const char *path)
{
    char bak[MAX_PATH + 4];
    _snprintf(bak, sizeof(bak), "%s.1", path);
    bak[sizeof(bak) - 1] = '\0';
    DeleteFileA(bak);
    MoveFileA(path, bak);
    InterlockedIncrement((LONG *)&g_rotate_seq);
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

static void echo_prepare(void);
static void echo_launch(void);

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

    /* The console echo's ring comes up before the first line so that line
     * reaches the window too; its thread starts right after it. */
    echo_prepare();

    /* Immediate proof-of-write marker: if THIS line is present but nothing
     * after it, the failure is very early; if the file is truly empty, the
     * handle never opened (check the path/permissions). sizeof-1, not a
     * hand count: the string is 32 bytes and a hand-written 31 dropped its
     * '\n' for years ("---\r[04:45:45]..." in every agent.log - the same
     * slip the close marker below once had). */
    raw_out("--- log opened (raw win32) ---\r\n",
            sizeof("--- log opened (raw win32) ---\r\n") - 1);

    echo_launch();
}

const char *log_path(void)
{
    return g_log_path;
}

unsigned long log_write_seq(void)
{
    return (unsigned long)g_write_seq;
}

unsigned long log_rotation_seq(void)
{
    return (unsigned long)g_rotate_seq;
}

/*
 * PRIORITY-INVERSION GUARD for g_log_cs.
 *
 * Background helpers run at THREAD_PRIORITY_IDLE (bgwork.h) and every one of
 * them logs. This lock is held across DISK I/O - every line in the unbuffered
 * startup window, and every flush after it - so an IDLE thread preempted
 * inside it by a busy foreground game would keep it for as long as the game
 * keeps the CPU, and the command thread's next log line would queue behind it.
 * On a Win9x box that thread is the ONLY one serving commands.
 *
 * So whoever holds the log lock runs at no less than normal priority: an IDLE
 * caller is lifted for the few milliseconds it holds it, then put back. Every
 * other caller pays one GetThreadPriority. The crash logger stays lock-free
 * and untouched.
 */
static int log_lift(void)
{
    HANDLE me = GetCurrentThread();
    if (GetThreadPriority(me) != THREAD_PRIORITY_IDLE)
        return 0;
    SetThreadPriority(me, THREAD_PRIORITY_NORMAL);
    return 1;
}

static void log_unlift(int lifted)
{
    if (lifted)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
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
    int n, lifted;
    va_list ap;

    if (!g_log_initialized) return;

    va_start(ap, fmt);
    n = format_line(line, (int)sizeof(line), tag, fmt, ap);
    va_end(ap);

    lifted = log_lift();
    EnterCriticalSection(&g_log_cs);
    raw_out(line, (DWORD)n);

    /* Age out the buffer HERE rather than relying on the flusher thread.
     * CreateThread genuinely fails on the 31MB Deskpro (main.c logs exactly
     * that for dosstage), and an earlier version responded by falling back to
     * unbuffered - which put the per-line platter write straight back, the
     * very thing batching exists to stop. Checking the clock on the path that
     * is already holding the lock costs nothing and needs no thread. */
    if (g_buffered && g_buf_len > 0 &&
        (DWORD)(GetTickCount() - g_last_flush) >= (DWORD)LOG_FLUSH_MS)
        flush_locked();

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
    log_unlift(lifted);
}

/* Force everything pending to disk. Safe to call from anywhere. */
void log_flush(void)
{
    int lifted;
    if (!g_log_initialized) return;
    lifted = log_lift();
    EnterCriticalSection(&g_log_cs);
    flush_locked();
    LeaveCriticalSection(&g_log_cs);
    log_unlift(lifted);
}

/* ======================================================================
 * CONSOLE ECHO - the only code in the agent that writes to the console.
 *
 * Everything here that calls into the console (WriteFile on the console
 * handle, SetConsoleTitleA, Get/SetConsoleMode) runs on log_echo_thread() and
 * with g_log_cs NOT held. Everything else only touches the ring, under the
 * lock, which is a memcpy. tests/python/test_log_console_echo.py pins both.
 *
 * WIN9x / MULTIPLEX: this thread is exactly as safe there as on NT, and it is
 * the better answer there too. A 9x console is a DOS VM that a DOS child or a
 * selection can hold for as long as it likes (.243 went 74 minutes deaf on
 * 2026-09-28 when the accept loop's printf sat behind one); now only this
 * thread waits, while the single thread that serves every client keeps
 * serving. There is no non-blocking console write to fall back on - console
 * handles take no overlapped I/O on either family - so if the thread cannot
 * be created (CreateThread genuinely fails on the 31 MB Deskpro) the echo is
 * switched OFF, never done inline: agent.log is the record, the console is a
 * courtesy, and a cosmetic feature must never cost a box its agent.
 * ====================================================================== */

#ifndef ENABLE_QUICK_EDIT_MODE
#define ENABLE_QUICK_EDIT_MODE 0x0040
#endif
#ifndef ENABLE_EXTENDED_FLAGS
#define ENABLE_EXTENDED_FLAGS  0x0080
#endif

/* Bracket every console call so the flusher can see one that never returns. */
static void echo_busy_begin(void)
{
    g_echo_busy_since = busytick_mark(GetTickCount());
}

static void echo_busy_end(const char *what)
{
    DWORD since = g_echo_busy_since;
    DWORD took = busytick_elapsed(GetTickCount(), since);   /* busytick.h */
    g_echo_busy_since = 0;
    if (since && took >= (DWORD)ECHO_STALL_MS)
        log_msg(LOG_MAIN, "console: %s returned after %lu s - the console is "
                "accepting output again", what, (unsigned long)(took / 1000));
}

/* Echo thread only, no lock held. */
static void echo_write(const char *s, DWORD len)
{
    DWORD wr = 0;
    echo_busy_begin();
    WriteFile(g_echo_h, s, len, &wr, NULL);
    echo_busy_end("a console write");
}

/* NT: a click in a console window with QuickEdit on starts a selection, and
 * a console with a selection accepts no output until it ends - which used to
 * stop the whole agent (see the header). Clear it on our console so a stray
 * click cannot do that. Win9x consoles have no QuickEdit. Echo thread only:
 * Get/SetConsoleMode are calls into the console like any other. The window's
 * Edit > Mark menu can still start a selection by hand; that now stalls this
 * thread alone. The setting is not restored at exit: that would be one more
 * console call on the exit path, which must never wait on the console. */
static void console_quickedit_off(void)
{
    HANDLE in;
    DWORD before = 0, after = 0;
    BOOL got, set;

    if (GetVersion() & 0x80000000) {
        log_msg(LOG_MAIN, "console: Win9x - no QuickEdit to clear");
        return;
    }
    in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == NULL || in == INVALID_HANDLE_VALUE) {
        log_msg(LOG_MAIN, "console: no console input - QuickEdit not applicable");
        return;
    }
    echo_busy_begin();
    got = GetConsoleMode(in, &before);
    set = got && SetConsoleMode(in, (before | ENABLE_EXTENDED_FLAGS)
                                    & ~(DWORD)ENABLE_QUICK_EDIT_MODE);
    if (got)
        GetConsoleMode(in, &after);     /* the post-condition, not the call */
    echo_busy_end("a console mode change");

    if (!got)
        log_msg(LOG_MAIN, "console: standard input is not a console (%lu) - "
                "QuickEdit not applicable", (unsigned long)GetLastError());
    else if (!set || (after & ENABLE_QUICK_EDIT_MODE))
        log_msg(LOG_MAIN, "console: could not clear QuickEdit (input mode "
                "0x%04lx -> 0x%04lx) - a click in the window can still pause "
                "console output; the agent is unaffected either way",
                (unsigned long)before, (unsigned long)after);
    else
        log_msg(LOG_MAIN, "console: QuickEdit off (input mode 0x%04lx -> "
                "0x%04lx) - a click in the agent's window cannot start a "
                "selection", (unsigned long)before, (unsigned long)after);
}

/* The one thread that talks to the console. IDLE, like every background
 * helper (bgwork.h): echo is for a person watching the window, and must never
 * compete with a game. It lifts itself (log_lift) for the memcpy it does under
 * g_log_cs, so being starved can never leave the lock held. */
static DWORD WINAPI log_echo_thread(LPVOID unused)
{
    char chunk[ECHO_CHUNK];
    char title[sizeof(g_echo_title)];
    (void)unused;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    console_quickedit_off();

    for (;;) {
        unsigned int n, gap = 0;
        unsigned long total = 0;
        int have_title = 0, lifted;

        lifted = log_lift();
        EnterCriticalSection(&g_log_cs);
        n = conring_take(&g_echo, chunk, sizeof(chunk), &gap);
        total = g_echo.total_dropped;
        if (g_echo_title_pending) {
            log_strcpy(title, g_echo_title, sizeof(title));
            g_echo_title_pending = 0;
            have_title = 1;
        }
        LeaveCriticalSection(&g_log_cs);
        log_unlift(lifted);

        /* ---- from here on no lock is held: the console may block us ---- */
        if (have_title) {
            echo_busy_begin();
            SetConsoleTitleA(title);
            echo_busy_end("a console title change");
        }
        if (n)
            echo_write(chunk, n);
        if (gap) {
            char note[200];
            int k = _snprintf(note, sizeof(note) - 1,
                              "[console: %u line(s) not shown here - the "
                              "console stopped accepting output; agent.log "
                              "has every line]\r\n", gap);
            if (k < 0 || k >= (int)sizeof(note)) k = (int)sizeof(note) - 1;
            note[k] = '\0';
            echo_write(note, (DWORD)k);
            log_msg(LOG_MAIN, "console: %u line(s) were not shown on the "
                    "console while it was not accepting output (all are in "
                    "this log; %lu since start)", gap, total);
        }
        if (!n && !gap && !have_title) {
            if (g_echo_stop)
                break;
            if (WaitForSingleObject(g_echo_evt, INFINITE) == WAIT_FAILED)
                break;
        }
    }
    return 0;
}

/* Called from log_init before the first line: pick the console handle and
 * arm the ring. No console (an NT service, a detached start) = no echo. */
static void echo_prepare(void)
{
    HANDLE h;
    if (g_echo_on || g_echo_thread)
        return;
    h = GetStdHandle(STD_ERROR_HANDLE);           /* where the echo always went */
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return;
    g_echo_evt = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    if (!g_echo_evt)
        return;
    g_echo_h = h;
    conring_init(&g_echo, g_echo_store, sizeof(g_echo_store));
    g_echo_on = 1;
}

/* Called from log_init after the first line: start the echo thread, and say
 * which way it went - this line is how a box proves it runs the fix. */
static void echo_launch(void)
{
    DWORD tid;
    if (!g_echo_on || g_echo_thread)
        return;
    g_echo_stop = 0;
    /* &tid, not NULL: Win9x rejects a NULL lpThreadId (error 87). */
    g_echo_thread = CreateThread(NULL, 0, log_echo_thread, NULL, 0, &tid);
    if (!g_echo_thread) {
        DWORD err = GetLastError();
        EnterCriticalSection(&g_log_cs);
        g_echo_on = 0;          /* never fall back to writing inline */
        LeaveCriticalSection(&g_log_cs);
        log_msg(LOG_MAIN, "log: no console echo thread (%lu) - console echo "
                "is OFF; agent.log has every line", (unsigned long)err);
        return;
    }
    log_msg(LOG_MAIN, "log: console echo on its own thread (%u-byte ring, "
            "idle priority) - a console that stops accepting output drops "
            "echo lines and cannot stall the agent", g_echo.cap);
}

/* Flusher, every LOG_FLUSH_MS: report - once - a console call that has not
 * returned for ECHO_STALL_MS. Lock-free read; the echo thread may be stuck.
 * (The flusher starts with batching, ~2 min after startup; a stall before
 * that is reported by echo_busy_end() when the call finally returns.) */
static void echo_stall_check(void)
{
    static DWORD reported = 0;     /* the busy stamp last reported */
    DWORD since = g_echo_busy_since;
    DWORD held;
    if (!since || since == reported)
        return;
    held = busytick_elapsed(GetTickCount(), since);         /* busytick.h */
    if (held < (DWORD)ECHO_STALL_MS)
        return;
    reported = since;
    log_msg(LOG_MAIN, "console: a console call has not returned for %lu s - "
            "the console is not being serviced (a selection/Mark in its "
            "window, or a hung console or display). The agent is unaffected: "
            "it keeps serving, and logging here; the window catches up when "
            "the console recovers", (unsigned long)(held / 1000));
    log_flush();
}

/* Stop the echo thread, letting it drain first - but bounded: a frozen
 * console keeps it inside WriteFile, and exiting must no more wait on the
 * console than logging may. Must be called WITHOUT g_log_cs held (the thread
 * needs it to drain). */
static void echo_stop(void)
{
    if (!g_echo_thread)
        return;
    g_echo_stop = 1;
    if (g_echo_evt)
        SetEvent(g_echo_evt);
    if (WaitForSingleObject(g_echo_thread, 1000) == WAIT_OBJECT_0) {
        CloseHandle(g_echo_thread);
        g_echo_thread = NULL;
    }
    /* else: left running inside the console; the process ends it. The event
     * is never closed, so if that thread does come back it waits on a live
     * handle rather than a dead (or recycled) one. */
}

/* printf for the console, from any thread: formats and queues, never waits.
 * `\n` becomes `\r\n` as msvcrt's text-mode stdout did. */
void con_printf(const char *fmt, ...)
{
    char msg[1024];
    char out[2048];
    int n, i, k = 0, lifted;
    va_list ap;

    if (!g_log_initialized || !g_echo_on)
        return;
    va_start(ap, fmt);
    n = _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(msg) - 1)
        n = (int)sizeof(msg) - 1;
    msg[n] = '\0';
    for (i = 0; i < n && k < (int)sizeof(out) - 2; i++) {
        if (msg[i] == '\n' && (i == 0 || msg[i - 1] != '\r'))
            out[k++] = '\r';
        out[k++] = msg[i];
    }

    lifted = log_lift();
    EnterCriticalSection(&g_log_cs);
    echo_push_locked(out, (DWORD)k);
    LeaveCriticalSection(&g_log_cs);
    log_unlift(lifted);
}

/* SetConsoleTitleA, done by the echo thread. */
void log_console_title(const char *title)
{
    int lifted;
    if (!g_log_initialized || !g_echo_on)
        return;
    lifted = log_lift();
    EnterCriticalSection(&g_log_cs);
    log_strcpy(g_echo_title, title ? title : "", sizeof(g_echo_title));
    g_echo_title_pending = 1;
    LeaveCriticalSection(&g_log_cs);
    log_unlift(lifted);
    if (g_echo_evt)
        SetEvent(g_echo_evt);
}

/* Periodic flusher: bounds how much routine logging a power cut can cost.
 *
 * It WAITS on an event rather than polling. It used to wake every 250 ms -
 * 240 times a minute, for the life of the agent, on every box - purely so a
 * shutdown would not wait out the 15 s interval. log_shutdown() now signals
 * the event, which ends the wait at once, so the thread wakes four times a
 * minute and only touches the disk when something was logged. */
static DWORD WINAPI log_flush_thread(LPVOID unused)
{
    (void)unused;
    while (!g_flush_stop) {
        if (g_flush_evt)
            WaitForSingleObject(g_flush_evt, LOG_FLUSH_MS);
        else
            Sleep(LOG_FLUSH_MS);   /* no event: log_shutdown's 2 s wait times
                                    * out and it flushes itself - still safe */
        log_flush();
        echo_stall_check();
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
    g_last_flush = GetTickCount();
    LeaveCriticalSection(&g_log_cs);

    if (on && !g_flush_thread) {
        DWORD tid;
        g_flush_stop = 0;
        if (!g_flush_evt)
            g_flush_evt = CreateEventA(NULL, TRUE, FALSE, NULL);  /* manual reset */
        g_flush_thread = CreateThread(NULL, 0, log_flush_thread, NULL, 0, &tid);
        if (!g_flush_thread) {
            /* Stay batched. The thread is only an optimisation for an IDLE
             * agent - the age check in log_msg() already bounds staleness
             * whenever anything is actually being logged, and an agent with
             * nothing to log has nothing to lose. Falling back to unbuffered
             * here (as an earlier version did) restored the per-line platter
             * write on the one machine batching was written for. */
            log_msg(LOG_MAIN, "log: no flusher thread (%lu) - batching on the "
                    "logging path only", (unsigned long)GetLastError());
        }
    }
}

/* Clean shutdown: stop batching, flush, close. */
void log_shutdown(void)
{
    if (!g_log_initialized) return;

    /* The console first, and never under g_log_cs: the echo thread needs the
     * lock to drain what is left (e.g. "Shutting down..."), and a frozen
     * console costs this at most a second. */
    echo_stop();

    g_flush_stop = 1;
    if (g_flush_evt)
        SetEvent(g_flush_evt);          /* end the flusher's wait now */
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
        /* sizeof-1, not a hand-counted literal: the string is 37 bytes and the
         * hand-written 38 wrote one byte past it — the literal's own NUL. In
         * bounds, so no UB, but it embeds a 0x00 in agent.log on EVERY clean
         * shutdown, after which grep reports "Binary file agent.log matches"
         * and the fleet log tooling this change set exists to serve goes
         * blind. Caught reviewing this merge, before it reached any box. */
        disk_out("--- log closed (clean shutdown) ---\r\n",
                 sizeof("--- log closed (clean shutdown) ---\r\n") - 1);
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
