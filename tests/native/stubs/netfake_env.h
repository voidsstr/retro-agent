/* netfake_env.h — a fake Win32 + Winsock, just big enough to compile and RUN
 * the real agent/src/chatproxy.c and agent/src/protocol.c natively.
 *
 * What those two files decide is what is under test - when a long-poll
 * answers, whether it can spin, whether a prompt survives a dead connection,
 * whether a recv can wait forever - not Microsoft's APIs. So the calls they
 * make are redirected here to a deterministic, single-threaded model:
 *
 *   TIME     GetTickCount() is a counter. A wait that nothing satisfies
 *            ADVANCES it by the full timeout, so "blocked for 30 s" costs the
 *            test nothing and is observable (fake_now).
 *   EVENTS   reference-counted, signal state only. WaitForSingleObject on a
 *            signalled event returns at once and COUNTS the call: a spin shows
 *            up as thousands of waits, and FAKE_SPIN_LIMIT aborts the test
 *            rather than hanging it. A test can schedule one action to run
 *            "while" a wait blocks (fake_during_wait) - that is how another
 *            thread's STATUS_SET/PROMPT_PUSH arriving mid-wait is modelled.
 *   SOCKETS  small ints with an inbound byte queue, a peer-closed flag and a
 *            writable flag. select() reports what that state implies and, if
 *            nothing is ready, advances time by its timeout. A recv() with
 *            nothing queued on a live peer would block FOREVER on real
 *            Winsock: here it is counted in fake_blocked_forever and fails,
 *            so a test can assert it never happens. send() appends to the
 *            socket's outbound capture.
 *
 * Only what the two modules touch is modelled. Keep it that way.
 */
#ifndef NETFAKE_ENV_H
#define NETFAKE_ENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- basic types ----
 * DWORD/LONG are `long` here, as wide as the host's: the agent formats them
 * with %lu everywhere, and no test depends on 32-bit tick wrap-around. */
typedef unsigned long   DWORD;
typedef long            LONG;
typedef int             BOOL;
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef void           *HANDLE;
typedef void           *LPVOID;
typedef void           *HKEY;
typedef unsigned long   UINT_PTR;
typedef int             SOCKET;
typedef struct { int held; } CRITICAL_SECTION;

#define WINAPI
#define TRUE  1
#define FALSE 0
#define INFINITE              0xFFFFFFFFul
#define WAIT_OBJECT_0         0u
#define WAIT_TIMEOUT          258u
#define DUPLICATE_SAME_ACCESS 2
#define MSG_PEEK              2
#define INVALID_SOCKET        (-1)
#define MAX_PATH              260

#define ERROR_SUCCESS       0L
#define KEY_READ            1
#define KEY_WRITE           2
#define REG_SZ              1
#define HKEY_LOCAL_MACHINE  ((HKEY)0x80000002)

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion;
    DWORD dwBuildNumber, dwPlatformId;
    char  szCSDVersion[128];
} OSVERSIONINFOA;

#define _snprintf snprintf

/* ---- fake clock ---- */
static DWORD fake_now = 1000;
static inline DWORD GetTickCount(void) { return fake_now; }
static inline void Sleep(DWORD ms) { fake_now += ms; }

/* ---- critical sections (single-threaded: just check pairing) ---- */
static int fake_lock_errors = 0;
static inline void InitializeCriticalSection(CRITICAL_SECTION *cs) { cs->held = 0; }
static inline void EnterCriticalSection(CRITICAL_SECTION *cs)
{ if (cs->held) fake_lock_errors++; cs->held = 1; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs)
{ if (!cs->held) fake_lock_errors++; cs->held = 0; }

/* ---- events ---- */
typedef struct { int signaled, manual, refs; } fake_event_t;
static int fake_events_live = 0;
static int fake_waits = 0;               /* total WaitForSingleObject calls */
static void (*fake_during_wait)(void) = NULL;
#define FAKE_SPIN_LIMIT 5000

static inline HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name)
{
    fake_event_t *e = (fake_event_t *)calloc(1, sizeof(*e));
    (void)sa; (void)name;
    e->manual = manual;
    e->signaled = initial;
    e->refs = 1;
    fake_events_live++;
    return e;
}
#define CreateEvent CreateEventA

static inline BOOL SetEvent(HANDLE h) { ((fake_event_t *)h)->signaled = 1; return TRUE; }
static inline BOOL ResetEvent(HANDLE h) { ((fake_event_t *)h)->signaled = 0; return TRUE; }

static inline BOOL CloseHandle(HANDLE h)
{
    fake_event_t *e = (fake_event_t *)h;
    if (--e->refs == 0) { free(e); fake_events_live--; }
    return TRUE;
}

static inline HANDLE GetCurrentProcess(void) { return (HANDLE)-1; }
static inline BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst,
                            DWORD acc, BOOL inh, DWORD opt)
{
    (void)sp; (void)tp; (void)acc; (void)inh; (void)opt;
    ((fake_event_t *)src)->refs++;
    *dst = src;
    return TRUE;
}

static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    fake_event_t *e = (fake_event_t *)h;
    if (++fake_waits > FAKE_SPIN_LIMIT) {
        fprintf(stderr, "    FAIL: WaitForSingleObject called %d times - "
                "the waiter is SPINNING\n", fake_waits);
        exit(2);
    }
    if (!e->signaled && fake_during_wait) {
        void (*fn)(void) = fake_during_wait;
        fake_during_wait = NULL;
        fn();                          /* "another thread" acts meanwhile */
    }
    if (e->signaled) {
        if (!e->manual) e->signaled = 0;
        return WAIT_OBJECT_0;
    }
    fake_now += ms;
    return WAIT_TIMEOUT;
}

/* ---- sockets ---- */
#define FAKE_MAX_SOCKS 16
typedef struct {
    unsigned char in[300000];
    int in_len;             /* bytes queued for recv() */
    int in_trickle;         /* >0: recv returns at most this many per call */
    int in_gap_ms;          /* ms that pass before each trickled piece */
    int peer_closed;
    int unwritable;         /* select(writefds) never reports it */
    unsigned char *out;     /* everything send() was given */
    int out_len, out_cap;
    int send_calls, max_send;
} fake_sock_t;

static fake_sock_t fake_socks[FAKE_MAX_SOCKS];
static int fake_blocked_forever = 0;   /* recv/send that would never return */

static inline void fake_sock_reset(void)
{
    int i;
    for (i = 0; i < FAKE_MAX_SOCKS; i++) {
        free(fake_socks[i].out);
        memset(&fake_socks[i], 0, sizeof(fake_socks[i]));
    }
    fake_blocked_forever = 0;
}

static inline void fake_sock_feed(SOCKET s, const void *data, int len)
{
    memcpy(fake_socks[s].in + fake_socks[s].in_len, data, len);
    fake_socks[s].in_len += len;
}

typedef struct { unsigned int fd_count; SOCKET fd_array[64]; } fake_fd_set;
struct fake_timeval { long tv_sec; long tv_usec; };
#define fd_set  fake_fd_set
#define timeval fake_timeval
#define FD_ZERO(set)    ((set)->fd_count = 0)
#define FD_SET(s, set)  ((set)->fd_array[(set)->fd_count++] = (s))

static inline int fake_sock_readable(SOCKET s)
{
    return fake_socks[s].in_len > 0 || fake_socks[s].peer_closed;
}

static inline int select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                  const struct timeval *tv)
{
    int ready = 0;
    unsigned int i;
    DWORD ms = tv ? (DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : INFINITE;
    (void)nfds; (void)e;
    if (r) for (i = 0; i < r->fd_count; i++) {
        fake_sock_t *fs = &fake_socks[r->fd_array[i]];
        if (fs->in_len > 0 && fs->in_trickle && fs->in_gap_ms) {
            if ((DWORD)fs->in_gap_ms > ms) { fake_now += ms; return 0; }
            fake_now += fs->in_gap_ms;
        }
        if (fake_sock_readable(r->fd_array[i])) ready++;
    }
    if (w) for (i = 0; i < w->fd_count; i++)
        if (!fake_socks[w->fd_array[i]].unwritable) ready++;
    if (!ready && ms != INFINITE) fake_now += ms;
    return ready;
}

static inline int recv(SOCKET s, char *buf, int len, int flags)
{
    fake_sock_t *fs = &fake_socks[s];
    int n;
    if (fs->in_len == 0) {
        if (fs->peer_closed) return 0;
        fake_blocked_forever++;        /* real Winsock would never return */
        return -1;
    }
    n = len < fs->in_len ? len : fs->in_len;
    if (fs->in_trickle && n > fs->in_trickle) n = fs->in_trickle;
    memcpy(buf, fs->in, n);
    if (!(flags & MSG_PEEK)) {
        memmove(fs->in, fs->in + n, fs->in_len - n);
        fs->in_len -= n;
    }
    return n;
}

static inline int send(SOCKET s, const char *buf, int len, int flags)
{
    fake_sock_t *fs = &fake_socks[s];
    (void)flags;
    if (fs->unwritable) { fake_blocked_forever++; return -1; }
    if (fs->out_len + len > fs->out_cap) {
        fs->out_cap = (fs->out_len + len) * 2 + 64;
        fs->out = (unsigned char *)realloc(fs->out, fs->out_cap);
    }
    memcpy(fs->out + fs->out_len, buf, len);
    fs->out_len += len;
    fs->send_calls++;
    if (len > fs->max_send) fs->max_send = len;
    return len;
}

/* ---- heap / misc used by protocol.c ---- */
static inline HANDLE GetProcessHeap(void) { return (HANDLE)1; }
static inline void *HeapAlloc(HANDLE h, DWORD f, size_t n) { (void)h; (void)f; return malloc(n ? n : 1); }
static inline BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; free(p); return TRUE; }
static inline BOOL GetComputerNameA(char *b, DWORD *n) { snprintf(b, *n, "FAKEBOX"); return TRUE; }
static inline BOOL GetVersionExA(OSVERSIONINFOA *o) { o->dwMajorVersion = 5; o->dwMinorVersion = 1; return TRUE; }
static inline DWORD GetModuleFileNameA(HANDLE m, char *b, DWORD n) { (void)m; snprintf(b, n, "C:\\RA\\retro_agent.exe"); return 1; }
static inline DWORD GetFileAttributesA(const char *p) { (void)p; return 0xFFFFFFFFu; }

/* ---- registry (PROXY_GET/SET: present, never exercised) ---- */
static inline LONG RegOpenKeyExA(HKEY k, const char *p, DWORD o, DWORD a, HKEY *r)
{ (void)k; (void)p; (void)o; (void)a; (void)r; return 2; }
static inline LONG RegQueryValueExA(HKEY k, const char *n, DWORD *r, DWORD *t, BYTE *d, DWORD *s)
{ (void)k; (void)n; (void)r; (void)t; (void)d; (void)s; return 2; }
static inline LONG RegCreateKeyExA(HKEY k, const char *p, DWORD r, char *c, DWORD o, DWORD a,
                            void *sa, HKEY *res, DWORD *disp)
{ (void)k; (void)p; (void)r; (void)c; (void)o; (void)a; (void)sa; (void)res; (void)disp; return 2; }
static inline LONG RegSetValueExA(HKEY k, const char *n, DWORD r, DWORD t, const BYTE *d, DWORD s)
{ (void)k; (void)n; (void)r; (void)t; (void)d; (void)s; return 2; }
static inline LONG RegCloseKey(HKEY k) { (void)k; return 0; }

#endif /* NETFAKE_ENV_H */
