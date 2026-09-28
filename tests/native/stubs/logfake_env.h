/* logfake_env.h - a fake Win32 on POSIX threads, just big enough to compile
 * and RUN the real agent/src/log.c natively, with a console that can FREEZE.
 *
 * What log.c decides is under test - which thread touches the console, what
 * is held while it does, whether a logger can be made to wait on it - so the
 * threads, critical sections and events here are REAL (pthreads), and so is
 * the log file (a POSIX fd). The console is a capture buffer behind a gate:
 *
 *   fake_console_freeze(1)  every console call (WriteFile on the console
 *                           handle, Get/SetConsoleMode, SetConsoleTitleA)
 *                           blocks until fake_console_freeze(0) - exactly
 *                           what a QuickEdit/Mark selection or a hung
 *                           conhost/display does to a real console.
 *   fake_console_text()     everything the console has been sent.
 *   fake_console_threads()  how many DISTINCT threads ever called into it.
 *
 * Only what log.c touches is modelled. Keep it that way.
 */
#ifndef LOGFAKE_ENV_H
#define LOGFAKE_ENV_H

/* log.c uses a subset of these; unused fakes are not a defect. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef unsigned long DWORD;
typedef long          LONG;
typedef int           BOOL;
typedef unsigned short WORD;
typedef void         *HANDLE;
typedef void         *LPVOID;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#define WINAPI
#define TRUE  1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE             0xFFFFFFFFul
#define WAIT_OBJECT_0        0ul
#define WAIT_TIMEOUT         258ul
#define WAIT_FAILED          0xFFFFFFFFul
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_SIZE    0xFFFFFFFFul
#define GENERIC_WRITE        0x40000000ul
#define FILE_SHARE_READ      1
#define FILE_SHARE_WRITE     2
#define OPEN_ALWAYS          4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_END             2
#define STD_INPUT_HANDLE     ((DWORD)-10)
#define STD_OUTPUT_HANDLE    ((DWORD)-11)
#define STD_ERROR_HANDLE     ((DWORD)-12)
#define THREAD_PRIORITY_IDLE   (-15)
#define THREAD_PRIORITY_NORMAL 0
#define _snprintf  snprintf
#define _vsnprintf vsnprintf

typedef struct { pthread_mutex_t m; } CRITICAL_SECTION;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute,
                 wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD dwFileAttributes; DWORD nFileSizeHigh, nFileSizeLow; }
        WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;

/* ---------------------------------------------------------------- handles */
enum { FH_FILE = 1, FH_CONOUT, FH_CONIN, FH_EVENT, FH_THREAD };

typedef struct fake_h {
    int kind;
    int fd;                           /* FH_FILE */
    pthread_mutex_t m;                /* FH_EVENT / FH_THREAD */
    pthread_cond_t  c;
    int signaled, manual, done;
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
    pthread_t tid;
} fake_h;

static fake_h fake_conout = { FH_CONOUT, -1, PTHREAD_MUTEX_INITIALIZER,
                              PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0 };
static fake_h fake_conin  = { FH_CONIN, -1, PTHREAD_MUTEX_INITIALIZER,
                              PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0 };

/* ---------------------------------------------------------------- console */
static pthread_mutex_t fake_con_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  fake_con_c = PTHREAD_COND_INITIALIZER;
static int    fake_con_frozen = 0;
static int    fake_con_present = 1;          /* 0 = no console (a service) */
static char   fake_con_buf[1 << 20];
static size_t fake_con_len = 0;
static DWORD  fake_con_mode = 0x01F7;        /* QuickEdit (0x40) on */
static int    fake_con_setmode_calls = 0;
static char   fake_con_title[256] = "";
static pthread_t fake_con_tids[8];
static int    fake_con_ntids = 0;
static DWORD  fake_version = 0x1DB10106ul;   /* NT 6.1 (Windows 7) */
static int    fake_fail_create_thread = 0;

static void fake_console_freeze(int on)
{
    pthread_mutex_lock(&fake_con_m);
    fake_con_frozen = on;
    pthread_cond_broadcast(&fake_con_c);
    pthread_mutex_unlock(&fake_con_m);
}

/* Enter a console call: wait out a freeze, record the calling thread. */
static void fake_con_enter(void)
{
    int i, seen = 0;
    pthread_mutex_lock(&fake_con_m);
    while (fake_con_frozen)
        pthread_cond_wait(&fake_con_c, &fake_con_m);
    for (i = 0; i < fake_con_ntids; i++)
        if (pthread_equal(fake_con_tids[i], pthread_self())) seen = 1;
    if (!seen && fake_con_ntids < 8)
        fake_con_tids[fake_con_ntids++] = pthread_self();
}

static void fake_con_leave(void) { pthread_mutex_unlock(&fake_con_m); }

static int fake_console_threads(void)
{
    int n;
    pthread_mutex_lock(&fake_con_m);
    n = fake_con_ntids;
    pthread_mutex_unlock(&fake_con_m);
    return n;
}

/* A NUL-terminated snapshot of the console so far (caller frees). */
static char *fake_console_text(void)
{
    char *s;
    pthread_mutex_lock(&fake_con_m);
    s = (char *)malloc(fake_con_len + 1);
    memcpy(s, fake_con_buf, fake_con_len);
    s[fake_con_len] = '\0';
    pthread_mutex_unlock(&fake_con_m);
    return s;
}

/* ------------------------------------------------------------------ misc */
static DWORD GetLastError(void) { return 0; }
static DWORD GetVersion(void) { return fake_version; }
static void  Sleep(DWORD ms) { usleep((useconds_t)ms * 1000u); }

static DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000ul + ts.tv_nsec / 1000000ul);
}

static void GetLocalTime(SYSTEMTIME *st)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    memset(st, 0, sizeof(*st));
    st->wHour = (WORD)tmv.tm_hour;
    st->wMinute = (WORD)tmv.tm_min;
    st->wSecond = (WORD)tmv.tm_sec;
}

static LONG InterlockedIncrement(LONG *p) { return __sync_add_and_fetch(p, 1); }

/* ------------------------------------------------------- critical section */
static void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutexattr_t a;                 /* Win32 CSs are recursive */
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
}
static void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(&cs->m); }
static void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(&cs->m); }

/* --------------------------------------------------------------- priority */
static _Thread_local int fake_prio = 0;
static HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static int  GetThreadPriority(HANDLE h) { (void)h; return fake_prio; }
static BOOL SetThreadPriority(HANDLE h, int p) { (void)h; fake_prio = p; return TRUE; }

/* ------------------------------------------------------ events & threads */
static fake_h *fake_new(int kind)
{
    fake_h *h = (fake_h *)calloc(1, sizeof(*h));
    h->kind = kind;
    h->fd = -1;
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, NULL);
    return h;
}

static HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name)
{
    fake_h *h = fake_new(FH_EVENT);
    (void)sa; (void)name;
    h->manual = manual;
    h->signaled = initial;
    return h;
}

static BOOL SetEvent(HANDLE hh)
{
    fake_h *h = (fake_h *)hh;
    pthread_mutex_lock(&h->m);
    h->signaled = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return TRUE;
}

static void *fake_thread_main(void *p)
{
    fake_h *h = (fake_h *)p;
    h->fn(h->arg);
    pthread_mutex_lock(&h->m);
    h->done = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return NULL;
}

static HANDLE CreateThread(void *sa, DWORD stack, LPTHREAD_START_ROUTINE fn,
                           LPVOID arg, DWORD flags, DWORD *tid)
{
    fake_h *h;
    (void)sa; (void)stack; (void)flags;
    if (!tid || fake_fail_create_thread)   /* Win9x: NULL lpThreadId = error 87 */
        return NULL;
    h = fake_new(FH_THREAD);
    h->fn = fn;
    h->arg = arg;
    if (pthread_create(&h->tid, NULL, fake_thread_main, h) != 0)
        return NULL;
    pthread_detach(h->tid);
    *tid = 1;
    return h;
}

static DWORD WaitForSingleObject(HANDLE hh, DWORD ms)
{
    fake_h *h = (fake_h *)hh;
    struct timespec dl;
    int rc = 0, *flag;
    if (!h || h == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    flag = h->kind == FH_THREAD ? &h->done : &h->signaled;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (ms != INFINITE) {
        dl.tv_sec += (time_t)(ms / 1000);
        dl.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&h->m);
    while (!*flag && rc == 0)
        rc = ms == INFINITE ? pthread_cond_wait(&h->c, &h->m)
                            : pthread_cond_timedwait(&h->c, &h->m, &dl);
    if (!*flag) { pthread_mutex_unlock(&h->m); return WAIT_TIMEOUT; }
    if (h->kind == FH_EVENT && !h->manual) h->signaled = 0;
    pthread_mutex_unlock(&h->m);
    return WAIT_OBJECT_0;
}

/* ------------------------------------------------------------------ files */
static HANDLE GetStdHandle(DWORD which)
{
    if (!fake_con_present) return NULL;
    if (which == STD_INPUT_HANDLE) return &fake_conin;
    return &fake_conout;
}

static HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *sa,
                          DWORD disp, DWORD attrs, HANDLE tmpl)
{
    fake_h *h;
    int fd;
    (void)access; (void)share; (void)sa; (void)disp; (void)attrs; (void)tmpl;
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return INVALID_HANDLE_VALUE;
    h = fake_new(FH_FILE);
    h->fd = fd;
    return h;
}

static BOOL WriteFile(HANDLE hh, const void *buf, DWORD len, DWORD *wr, void *ov)
{
    fake_h *h = (fake_h *)hh;
    (void)ov;
    if (h->kind == FH_CONOUT) {
        fake_con_enter();
        if (fake_con_len + len <= sizeof(fake_con_buf)) {
            memcpy(fake_con_buf + fake_con_len, buf, len);
            fake_con_len += len;
        }
        fake_con_leave();
        if (wr) *wr = len;
        return TRUE;
    }
    if (h->kind != FH_FILE) return FALSE;
    if (write(h->fd, buf, len) != (ssize_t)len) return FALSE;
    if (wr) *wr = len;
    return TRUE;
}

static BOOL  FlushFileBuffers(HANDLE h) { (void)h; return TRUE; }
static DWORD SetFilePointer(HANDLE hh, LONG d, LONG *hi, DWORD how)
{
    fake_h *h = (fake_h *)hh;
    (void)hi;
    return (DWORD)lseek(h->fd, d, how == FILE_END ? SEEK_END : SEEK_SET);
}
static DWORD GetFileSize(HANDLE hh, DWORD *hi)
{
    struct stat st;
    (void)hi;
    if (fstat(((fake_h *)hh)->fd, &st) != 0) return INVALID_FILE_SIZE;
    return (DWORD)st.st_size;
}
static BOOL CloseHandle(HANDLE hh)
{
    fake_h *h = (fake_h *)hh;
    if (h && h != INVALID_HANDLE_VALUE && h->kind == FH_FILE && h->fd >= 0) {
        close(h->fd);
        h->fd = -1;
    }
    return TRUE;            /* thread/event objects are deliberately leaked */
}
static BOOL GetFileAttributesExA(const char *p, GET_FILEEX_INFO_LEVELS l,
                                 WIN32_FILE_ATTRIBUTE_DATA *d)
{
    struct stat st;
    (void)l;
    if (stat(p, &st) != 0) return FALSE;
    memset(d, 0, sizeof(*d));
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
static BOOL  DeleteFileA(const char *p) { return unlink(p) == 0; }
static BOOL  MoveFileA(const char *a, const char *b) { return rename(a, b) == 0; }
static BOOL  CreateDirectoryA(const char *p, void *sa) { (void)sa; return mkdir(p, 0755) == 0; }
static DWORD GetModuleFileNameA(void *m, char *b, DWORD n) { (void)m; (void)b; (void)n; return 0; }
static DWORD GetTempPathA(DWORD n, char *b) { snprintf(b, n, "/tmp/"); return 5; }

/* ---------------------------------------------------------- console API */
static BOOL GetConsoleMode(HANDLE h, DWORD *mode)
{
    if (h != &fake_conin) return FALSE;
    fake_con_enter();
    *mode = fake_con_mode;
    fake_con_leave();
    return TRUE;
}
static BOOL SetConsoleMode(HANDLE h, DWORD mode)
{
    if (h != &fake_conin) return FALSE;
    fake_con_enter();
    fake_con_setmode_calls++;
    /* a real console applies the extended flags only with 0x80 present */
    fake_con_mode = (mode & 0x80) ? mode : (mode | (fake_con_mode & 0x60));
    fake_con_leave();
    return TRUE;
}
static BOOL SetConsoleTitleA(const char *t)
{
    fake_con_enter();
    snprintf(fake_con_title, sizeof(fake_con_title), "%s", t);
    fake_con_leave();
    return TRUE;
}

#endif /* LOGFAKE_ENV_H */
