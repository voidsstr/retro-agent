/* test_vcr_kmd_pace.c - TRUE-SOURCE test of voodoo-cleanroom/vcr-kmd/tools/vcr_pace.h,
 * the one gate every vcr-kmd tool passes through to change the display mode.
 *
 * Every mode switch makes the monitor drop and re-acquire sync. On 2026-09-26
 * the first silicon battery on .124 (a 1998 Sony CPD-G200 CRT) made ~250 of
 * them at two a second, and the user heard every relay click. The fix is a
 * floor enforced where the switch happens, so this compiles the REAL header
 * against a fake Win32 - a virtual clock with Sleep advancing it, the box's
 * C:\vcr and stamp file held in memory with failures to inject, one named
 * mutex shared by the "processes" the test runs in turn, and the process's
 * top-level exception filter slot - and asserts what it promises:
 *   - at least VCR_PACE_MIN_MS (>= 3 s) between two switches - ACROSS
 *     PROCESSES, because the stamp is a file on the box: a script running one
 *     tool after another cannot beat it;
 *   - a clock that went backwards waits the whole floor, not none;
 *   - vcr_pace_set_min (a caller's --pace) raises the floor, never lowers it;
 *   - a temporary mode still set when the program exits is HELD for the floor
 *     and the revert XP makes is stamped; a mode already given back is not.
 * And, since an adversarial review found the first version FAILED OPEN:
 *   - a stamp that exists and cannot be read in full (sharing violation,
 *     C:\vcr a file, 0 or 4 bytes, zeroed) waits the whole floor - once, not
 *     forever; only a stamp that is not there means "never switched";
 *   - the stamp is written in place, never truncated first;
 *   - a failed stamp write is reported, still paces its own process from the
 *     in-process copy, and is retried at exit for the next tool;
 *   - the wait reads the stamp again after every sleep, and a stamp that
 *     never stops moving still cannot stall a tool forever;
 *   - the switch lock (a named mutex) is held from the wait to the stamp,
 *     balanced however the calls nest; a stuck holder costs a queued tool a
 *     timeout and a whole floor, not a deadlock; a lock abandoned by a killed
 *     tool counts as acquired and as a switch just now;
 *   - `vcrctl pace-mark` after a kill measures the next switch from the kill;
 *   - a crash with a temporary mode held (or mid-switch) holds it, stamps,
 *     and ends the process; with nothing held it is left to the filter
 *     installed before; a filter that repaired the fault keeps the state.
 * And, since a third review found it still failed open in places:
 *   - a lock that cannot be had (a stuck holder, CreateMutex refused) means
 *     NO switch: vcr_pace_before_switch() answers 0, "pace lock busy", and
 *     neither waits nor stamps; a temporary mode held by the refused tool is
 *     still given back through the exit hold, lock or no lock;
 *   - the stamp is written before the switch (the pre-stamp) as well as
 *     after, and a before-call nested in one that waited does not wait again;
 *   - the exit hold stamps the revert AHEAD (it lands after atexit), and a
 *     wait reads a stamp ahead as "until then, plus the floor" - within the
 *     horizon a gate writes; further ahead is a clock that went back;
 *   - --pace / PACE_MS: decimal 0..VCR_PACE_MAX_MS or refused, never wrapped;
 *     vcr_pace_set_min clamps at VCR_PACE_MAX_MS;
 *   - the wait sleeps in chunks, reads the stamp after every one, and a floor
 *     that never passes is refused at the hard cap - no switch;
 *   - vcrctl pace-kill: refuses pid 0/4/itself and anything but our four
 *     tools; a kill is made under the lock a floor after the latest stamp,
 *     waited for (bounded) and stamped ahead; a busy lock kills nothing.
 * The switch SITES in the tools are checked in tests/python/
 * test_vcr_kmd_monitor_safety.py; this file is the gate's own behaviour.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "munit.h"

/* ---- the fake Win32 vcr_pace.h needs (its <windows.h> is stubs/windows.h,
 * which is empty) ------------------------------------------------------------ */
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef long               LONG;
typedef unsigned long long ULONGLONG;
typedef void              *HANDLE;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
struct _EXCEPTION_POINTERS { void *ExceptionRecord, *ContextRecord; };
#define WINAPI
typedef LONG (WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(struct _EXCEPTION_POINTERS *);

#define FALSE                    0
#define INVALID_HANDLE_VALUE     ((HANDLE)(ptrdiff_t)-1)
#define GENERIC_READ             0x80000000ul
#define GENERIC_WRITE            0x40000000ul
#define FILE_SHARE_READ          1ul
#define FILE_SHARE_WRITE         2ul
#define CREATE_ALWAYS            2ul
#define OPEN_EXISTING            3ul
#define OPEN_ALWAYS              4ul
#define TRUNCATE_EXISTING        5ul
#define INVALID_FILE_ATTRIBUTES  ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10ul
#define FILE_ATTRIBUTE_ARCHIVE   0x20ul
#define ERROR_FILE_NOT_FOUND     2ul
#define ERROR_PATH_NOT_FOUND     3ul
#define ERROR_ACCESS_DENIED      5ul
#define ERROR_SHARING_VIOLATION  32ul
#define ERROR_INVALID_HANDLE     6ul
#define ERROR_DISK_FULL          112ul
#define ERROR_TIMEOUT            1460ul
#define WAIT_OBJECT_0            0ul
#define WAIT_ABANDONED           0x80ul
#define WAIT_TIMEOUT             258ul
#define WAIT_FAILED              0xfffffffful
#define EXCEPTION_EXECUTE_HANDLER     1
#define EXCEPTION_CONTINUE_SEARCH     0
#define EXCEPTION_CONTINUE_EXECUTION  (-1)

/* 2026-09-26 00:00 UTC in FILETIME units (100 ns since 1601) - a real date,
 * so the header's ms arithmetic runs on numbers of the size it meets on XP */
#define T0_100NS 134348544000000000ull

static ULONGLONG g_now;              /* virtual clock, 100 ns units */
static DWORD     g_slept_ms;         /* every Sleep() since the last reset */
static unsigned  g_sleeps;
static DWORD     g_max_sleep;        /* the longest single Sleep() since the last reset */
static DWORD     g_last_error;

/* another process acting while this one sleeps: runs halfway through the
 * next Sleep (and every one after it, if g_on_sleep_keep) */
static void (*g_on_sleep)(void);
static int g_on_sleep_keep;

/* ---- the box: C:\vcr and the stamp file ---------------------------------- */
enum { DIR_ABSENT, DIR_IS_DIR, DIR_IS_FILE };
static int g_dir;                    /* what C:\vcr is */
static int g_dir_made;

static struct {
    int exists;
    unsigned char bytes[8];
    DWORD len;
    char name[64];                   /* the path the header last opened */
    unsigned writes;
} g_file;

static DWORD    g_read_fail;         /* opening the stamp to READ fails with this */
static DWORD    g_write_fail;        /* opening it to WRITE fails with this */
static int      g_writefile_fails;   /* WriteFile itself fails (disk full) */
static unsigned g_truncations;       /* opens that emptied the stamp first */
static unsigned g_reads;
static unsigned g_unlocked_reads;    /* stamp reads while this process lacked the lock */
static unsigned g_unlocked_writes;   /* ... and stamp writes */

/* ---- the named mutex, one for the whole box -------------------------------- */
static int g_proc;                   /* the "process" running now */
static struct { int owner; unsigned count; int abandoned; } g_mx;
static char     g_mx_name[32];
static unsigned g_mx_creates, g_mx_takes, g_mx_releases, g_mx_bad_releases, g_mx_timeouts;
static unsigned g_mx_dropped;        /* takes the kernel let go of when their process died */
static int      g_mx_create_fails;
static int      g_mx_tag;

/* ---- this process's top-level exception filter slot ------------------------ */
static LPTOP_LEVEL_EXCEPTION_FILTER g_filter;
static unsigned g_filter_sets;
static unsigned g_crt_calls;         /* the filter mingw's CRT installs before main */
static LONG     g_crt_answer;

/* ---- what the header reported (VCR_PACE_REPORT, below) --------------------- */
static unsigned g_reports;
static DWORD    g_report_err;
static char     g_report_what[160];

static void GetSystemTimeAsFileTime(FILETIME *ft)
{
    ft->dwLowDateTime = (DWORD)(g_now & 0xffffffffu);
    ft->dwHighDateTime = (DWORD)(g_now >> 32);
}

static void Sleep(DWORD ms)
{
    ULONGLONG half = (ULONGLONG)ms * 5000u;
    g_slept_ms += ms;
    g_sleeps++;
    if (ms > g_max_sleep)
        g_max_sleep = ms;
    if (g_on_sleep) {
        void (*f)(void) = g_on_sleep;
        if (!g_on_sleep_keep)
            g_on_sleep = NULL;
        g_now += half;
        f();
        g_now += (ULONGLONG)ms * 10000u - half;
    } else {
        g_now += (ULONGLONG)ms * 10000u;
    }
}

static DWORD GetLastError(void) { return g_last_error; }

static BOOL CreateDirectoryA(const char *path, void *sec)
{
    (void)sec;
    g_dir_made += !strcmp(path, "C:\\vcr");
    if (g_dir == DIR_ABSENT)
        g_dir = DIR_IS_DIR;
    return g_dir == DIR_IS_DIR;
}

static DWORD GetFileAttributesA(const char *path)
{
    if (!strcmp(path, "C:\\vcr") && g_dir == DIR_IS_DIR)
        return FILE_ATTRIBUTE_DIRECTORY;
    if (!strcmp(path, "C:\\vcr") && g_dir == DIR_IS_FILE)
        return FILE_ATTRIBUTE_ARCHIVE;
    g_last_error = ERROR_FILE_NOT_FOUND;
    return INVALID_FILE_ATTRIBUTES;
}

static int g_handle_tag;             /* any non-NULL, non-INVALID address */

static HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sec, DWORD disp,
                          DWORD flags, HANDLE tmpl)
{
    int write = (access & GENERIC_WRITE) != 0;
    (void)share; (void)sec; (void)flags; (void)tmpl;
    strncpy(g_file.name, name, sizeof g_file.name - 1);
    if (!write) {
        g_reads++;
        g_unlocked_reads += g_mx.owner != g_proc;
    }
    if (g_dir != DIR_IS_DIR) {           /* no C:\vcr, or C:\vcr is a file */
        g_last_error = ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    if (!write && g_read_fail) {
        g_last_error = g_read_fail;
        return INVALID_HANDLE_VALUE;
    }
    if (write && g_write_fail) {
        g_last_error = g_write_fail;
        return INVALID_HANDLE_VALUE;
    }
    if (disp == OPEN_EXISTING && !g_file.exists) {
        g_last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    if (disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING) {
        g_truncations++;
        g_file.len = 0;
    }
    if (!g_file.exists) {
        g_file.exists = 1;
        g_file.len = 0;
    }
    return &g_handle_tag;
}

static BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
    DWORD k = n < g_file.len ? n : g_file.len;
    (void)h; (void)ov;
    memcpy(buf, g_file.bytes, k);
    *got = k;
    return 1;
}

/* every handle starts at offset 0: a write lands on the stamp's 8 bytes */
static BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *ov)
{
    (void)h; (void)ov;
    if (g_writefile_fails) {
        *put = 0;
        g_last_error = ERROR_DISK_FULL;
        return 0;
    }
    if (n > sizeof g_file.bytes)
        n = sizeof g_file.bytes;
    g_unlocked_writes += g_mx.owner != g_proc;
    memcpy(g_file.bytes, buf, n);
    if (g_file.len < n)
        g_file.len = n;
    g_file.writes++;
    *put = n;
    return 1;
}

static BOOL CloseHandle(HANDLE h) { (void)h; return 1; }

static HANDLE CreateMutexA(void *sec, BOOL initial, const char *name)
{
    (void)sec; (void)initial;
    g_mx_creates++;
    strncpy(g_mx_name, name, sizeof g_mx_name - 1);
    if (g_mx_create_fails) {
        g_last_error = ERROR_ACCESS_DENIED;
        return NULL;
    }
    return &g_mx_tag;
}

/* ---- the process `vcrctl pace-kill` is pointed at ----------------------------- */
static int g_victim_tag;
static struct {
    int alive;
    int dies;                        /* TerminateProcess ends it at once */
    DWORD terminate_err;             /* TerminateProcess fails with this */
    unsigned terminates;
    ULONGLONG killed_at;             /* ms */
    int owner_at_kill;               /* who held the switch lock then */
    DWORD waited;                    /* what the killer waited for it to go */
} g_victim;

static BOOL TerminateProcess(HANDLE h, unsigned code)
{
    (void)code;
    if (h != &g_victim_tag) {
        g_last_error = ERROR_INVALID_HANDLE;
        return 0;
    }
    if (g_victim.terminate_err) {
        g_last_error = g_victim.terminate_err;
        return 0;
    }
    g_victim.terminates++;
    g_victim.killed_at = g_now / 10000u;
    g_victim.owner_at_kill = g_mx.owner;
    if (g_victim.dies)
        g_victim.alive = 0;
    return 1;
}

/* the victim: signalled once dead, else the whole timeout passes.
 * The mutex - free: taken (abandoned: taken, and says so); ours: recursion;
 * another process's: that holder is stuck, so the whole timeout passes */
static DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (h == &g_victim_tag) {
        if (!g_victim.alive)
            return WAIT_OBJECT_0;
        g_victim.waited += ms;
        g_now += (ULONGLONG)ms * 10000u;
        return WAIT_TIMEOUT;
    }
    if (!g_mx.owner) {
        g_mx.owner = g_proc;
        g_mx.count = 1;
        g_mx_takes++;
        if (g_mx.abandoned) {
            g_mx.abandoned = 0;
            return WAIT_ABANDONED;
        }
        return WAIT_OBJECT_0;
    }
    if (g_mx.owner == g_proc) {
        g_mx.count++;
        g_mx_takes++;
        return WAIT_OBJECT_0;
    }
    g_mx_timeouts++;
    g_now += (ULONGLONG)ms * 10000u;
    return WAIT_TIMEOUT;
}

static BOOL ReleaseMutex(HANDLE h)
{
    if (h != &g_mx_tag || g_mx.owner != g_proc || !g_mx.count) {
        g_mx_bad_releases++;
        return 0;
    }
    g_mx_releases++;
    if (!--g_mx.count)
        g_mx.owner = 0;
    return 1;
}

static LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_filter;
    g_filter = f;
    g_filter_sets++;
    return prev;
}

/* mingw's _gnu_exception_handler, as far as this test cares */
static LONG WINAPI crt_filter(struct _EXCEPTION_POINTERS *ep)
{
    (void)ep;
    g_crt_calls++;
    return g_crt_answer;
}

static void fake_report(const char *what, DWORD err)
{
    g_reports++;
    g_report_err = err;
    strncpy(g_report_what, what, sizeof g_report_what - 1);
}
#define VCR_PACE_REPORT(what, err) fake_report((what), (err))

/* the process's exit: the header registers its hold with atexit(), and this
 * test runs several "processes" in one, so it keeps the list itself */
static void (*g_atexit[8])(void);
static unsigned g_natexit;

static int fake_atexit(void (*f)(void))
{
    if (g_natexit < 8)
        g_atexit[g_natexit++] = f;
    return 0;
}
#define atexit fake_atexit                  /* <stdlib.h> is already in, above */

/* the header under test - with vcrctl's paced kill, which the labs leave out */
#define VCR_PACE_WANT_KILL
#include "../../voodoo-cleanroom/vcr-kmd/tools/vcr_pace.h"

/* ---- helpers --------------------------------------------------------------- */
static ULONGLONG now_ms(void) { return g_now / 10000u; }
static void advance_ms(ULONGLONG ms) { g_now += ms * 10000u; }

static ULONGLONG stamp_ms(void)
{
    ULONGLONG t = 0;
    if (g_file.exists && g_file.len == sizeof t)
        memcpy(&t, g_file.bytes, sizeof t);
    return t;
}

static void put_stamp(ULONGLONG t, DWORD len)
{
    g_dir = DIR_IS_DIR;
    g_file.exists = 1;
    memcpy(g_file.bytes, &t, sizeof t);
    g_file.len = len;
}

/* another process on the box records a switch now (its own mark, or the
 * host's `vcrctl pace-mark` after a kill) */
static ULONGLONG g_stamped_at;
static void someone_stamps_now(void)
{
    g_stamped_at = now_ms();
    put_stamp(g_stamped_at, 8);
}

/* ... or a revert still to come: an exit hold's, a paced kill's */
static void someone_stamps_ahead(void)
{
    g_stamped_at = now_ms() + VCR_PACE_EXIT_LAG_MS;
    put_stamp(g_stamped_at, 8);
}

/* a host stamping in a loop - for ten times the cap, so a gate that lost its
 * cap fails the test instead of hanging the suite */
static void a_storm_of_stamps(void)
{
    if (g_slept_ms < 10 * VCR_PACE_WAIT_MAX_MS)
        someone_stamps_now();
}

static void reset_sleeps(void) { g_slept_ms = 0; g_sleeps = 0; g_max_sleep = 0; }

/* a new process on the same box: the header's statics start over, the stamp
 * file and the named mutex (the box's state) do not */
static void new_process(void)
{
    g_proc++;
    g_vcr_pace_ms = VCR_PACE_MIN_MS;
    g_vcr_pace_exit_armed = 0;
    g_vcr_pace_filter_armed = 0;
    g_vcr_pace_temp_mode = 0;
    g_vcr_pace_mine = 0;
    g_vcr_pace_unsaved = 0;
    g_vcr_pace_lock = NULL;
    g_vcr_pace_depth = 0;
    g_vcr_pace_armed = 0;
    g_vcr_pace_why = NULL;
    g_vcr_pace_prev_filter = NULL;
    g_natexit = 0;
    g_filter = crt_filter;               /* the CRT installs its own before main */
    g_filter_sets = 0;
    g_crt_calls = 0;
    g_crt_answer = EXCEPTION_CONTINUE_SEARCH;
    reset_sleeps();
}

/* the process is gone: the kernel abandons a mutex it still owns */
static void process_dies(void)
{
    if (g_mx.owner == g_proc) {
        g_mx_dropped += g_mx.count;
        g_mx.owner = 0;
        g_mx.count = 0;
        g_mx.abandoned = 1;
    }
}

/* return from main: the C runtime runs atexit handlers, last first */
static void process_exit(void)
{
    while (g_natexit)
        g_atexit[--g_natexit]();
    process_dies();
}

/* TerminateProcess / PROCKILL / EXECW's tree-kill: nothing of ours runs */
static void process_killed(void) { process_dies(); }

/* an unhandled exception: XP calls the top-level filter; unless it continues
 * execution, the process ends */
static LONG process_crashes(void)
{
    struct _EXCEPTION_POINTERS ep = { NULL, NULL };
    LONG r = g_filter ? g_filter(&ep) : EXCEPTION_CONTINUE_SEARCH;
    if (r != EXCEPTION_CONTINUE_EXECUTION)
        process_dies();
    return r;
}

/* a brand-new box: no switch recorded anywhere */
static void fresh_box(void)
{
    memset(&g_file, 0, sizeof g_file);
    memset(&g_mx, 0, sizeof g_mx);
    g_dir = DIR_ABSENT;
    g_dir_made = 0;
    g_read_fail = g_write_fail = 0;
    g_writefile_fails = 0;
    g_truncations = g_reads = g_unlocked_reads = g_unlocked_writes = 0;
    g_mx_creates = g_mx_takes = g_mx_releases = g_mx_bad_releases = g_mx_timeouts = 0;
    g_mx_dropped = 0;
    g_mx_create_fails = 0;
    g_reports = 0;
    g_on_sleep = NULL;
    g_on_sleep_keep = 0;
    memset(&g_victim, 0, sizeof g_victim);
    g_now = T0_100NS;
    new_process();
}

/* one switch, the way every tool makes it: wait, switch, record */
static void a_switch(void)
{
    CHECK(vcr_pace_before_switch(), "the gate lets the switch go ahead");
    vcr_pace_after_switch();
}

static int lock_is_free_and_balanced(void)
{
    return !g_mx.owner && g_mx_takes == g_mx_releases + g_mx_dropped && !g_mx_bad_releases;
}

/* ---- the floor ------------------------------------------------------------- */
TEST(the_floor_is_at_least_three_seconds_on_a_file_on_the_box) {
    CHECK(VCR_PACE_MIN_MS >= 3000u, "3 s between switches at least");
    CHECK(!strcmp(VCR_PACE_FILE, "C:\\vcr\\lastswitch.dat"), "the stamp lives on the box");
    fresh_box();
    a_switch();
    CHECK(!strcmp(g_file.name, VCR_PACE_FILE), "the stamp is written where it is read");
    CHECK(g_dir_made >= 1, "C:\\vcr is created if absent - no directory must mean no floor");
    CHECK_EQ_U(stamp_ms(), now_ms());
}

TEST(a_fresh_box_switches_without_waiting) {
    fresh_box();
    CHECK(vcr_pace_before_switch(), "nothing to wait for");
    CHECK_EQ_U(g_sleeps, 0);
    CHECK(g_vcr_pace_why == NULL, "and no refusal to report");
}

TEST(back_to_back_switches_wait_out_the_floor) {
    fresh_box();
    a_switch();
    advance_ms(1000);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 1000);   /* the rest of the floor, no more */
    CHECK(g_max_sleep <= VCR_PACE_CHUNK_MS, "in chunks");
    vcr_pace_after_switch();
    reset_sleeps();
    advance_ms(VCR_PACE_MIN_MS + 1);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_sleeps, 0);                          /* already past it: no wait */
}

TEST(the_2026_09_26_rhythm_is_slowed_to_the_floor) {
    /* the battery on .124 asked for a switch every ~0.5 s; through the gate
     * no two of them may be closer than the floor */
    ULONGLONG at[40];
    unsigned i;
    fresh_box();
    for (i = 0; i < 40; i++) {
        a_switch();
        at[i] = stamp_ms();
        advance_ms(500);
    }
    for (i = 1; i < 40; i++)
        CHECK(at[i] - at[i - 1] >= VCR_PACE_MIN_MS, "every gap >= the floor");
    CHECK(at[39] - at[0] >= 39ull * VCR_PACE_MIN_MS, "40 switches take >= 39 floors");
}

TEST(the_floor_holds_across_processes) {
    /* a script that runs one tool after another (setmode, then golden, ...)
     * must not beat it: each tool is a new process */
    fresh_box();
    a_switch();                                        /* tool A: into a mode */
    advance_ms(VCR_PACE_MIN_MS);
    vcr_pace_before_switch();
    vcr_pace_after_restore();                          /* ... and back out, then exits */
    process_exit();
    new_process();                                     /* tool B, 1 s later */
    advance_ms(1000);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 1000);    /* only the file carried A's stamp */
}

TEST(a_clock_that_went_backwards_waits_the_whole_floor) {
    ULONGLONG stamp;
    fresh_box();
    a_switch();
    stamp = g_now;
    g_now = stamp - 600ull * 1000u * 10000u;           /* 10 minutes back */
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);           /* unknown elapsed != long elapsed */
}

TEST(set_min_raises_the_floor_and_never_lowers_it) {
    fresh_box();
    vcr_pace_set_min(5000);
    a_switch();
    advance_ms(1000);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, 4000);                      /* a caller's --pace 5000 */
    vcr_pace_after_switch();
    vcr_pace_set_min(1000);                            /* cannot undercut 5000 ... */
    advance_ms(1000);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, 4000);
    vcr_pace_after_switch();
    process_exit();                                    /* its exit hold stamps the revert ahead */
    new_process();
    vcr_pace_set_min(100);                             /* ... nor the 3 s floor */
    advance_ms(1000);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_EXIT_LAG_MS - 1000 + VCR_PACE_MIN_MS);
}

/* ---- the exit hold ---------------------------------------------------------- */
TEST(a_temporary_mode_is_held_at_exit_and_the_revert_stamped) {
    /* a tool that switched INTO a CDS_FULLSCREEN / exclusive / Glide mode and
     * returns from main 200 ms later: XP reverts the mode as the process ends,
     * a second re-sync - held for the floor, from ANY return path */
    ULONGLONG exit_at;
    fresh_box();
    a_switch();
    CHECK_EQ_U(g_natexit, 1);
    advance_ms(200);
    reset_sleeps();
    process_exit();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 200);
    exit_at = now_ms();
    /* the revert is a switch too - and it lands AFTER this hold (the DLLs
     * detach after atexit, glide3x's with seconds of idle waits): stamped at
     * its latest */
    CHECK_EQ_U(stamp_ms(), exit_at + VCR_PACE_EXIT_LAG_MS);
    CHECK(lock_is_free_and_balanced(), "the exit hold gives the lock back after the stamp");
    new_process();                                     /* the next tool, at once */
    vcr_pace_before_switch();
    /* measured from the revert's latest: the lag, then the floor */
    CHECK_EQ_U(g_slept_ms, VCR_PACE_EXIT_LAG_MS + VCR_PACE_MIN_MS);
    CHECK_EQ_U(g_reports, 0);                          /* a normal exit leaves nothing abandoned */
}

TEST(a_mode_given_back_is_not_held_again_at_exit) {
    unsigned writes;
    fresh_box();
    a_switch();
    vcr_pace_before_switch();                          /* the hold */
    vcr_pace_after_restore();                          /* RestoreDisplayMode / CDS(NULL) */
    writes = g_file.writes;
    reset_sleeps();
    process_exit();
    CHECK_EQ_U(g_sleeps, 0);
    CHECK_EQ_U(g_file.writes, writes);                 /* no phantom switch stamped */
}

TEST(a_failed_give_back_keeps_the_exit_hold) {
    /* vcrctl's pace_gave_back(0) is vcr_pace_after_switch_ex(1): the mode may
     * still be up, and XP will revert it at exit */
    fresh_box();
    a_switch();
    vcr_pace_before_switch();
    vcr_pace_after_switch_ex(1);
    advance_ms(100);
    reset_sleeps();
    process_exit();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 100);
}

TEST(the_exit_hold_is_registered_once) {
    fresh_box();
    a_switch();
    a_switch();
    vcr_pace_after_restore();
    a_switch();
    CHECK_EQ_U(g_natexit, 1);
    CHECK_EQ_U(g_filter_sets, 1);                      /* and the crash filter once */
    CHECK(g_filter == vcr_pace_crash_filter, "the crash filter is the process's top filter");
}

TEST(a_program_that_never_switches_is_never_held) {
    /* ddlab caps, windowed d3dprobe: they must not cost the box 3 s */
    unsigned takes;
    fresh_box();
    a_switch();
    process_exit();
    new_process();
    takes = g_mx_takes;
    reset_sleeps();
    process_exit();
    CHECK_EQ_U(g_natexit, 0);
    CHECK_EQ_U(g_sleeps, 0);
    CHECK_EQ_U(g_mx_takes, takes);                     /* nor touch the switch lock */
    CHECK_EQ_U(g_filter_sets, 0);
}

/* ---- failing closed: a stamp that is there but cannot be read ---------------- */
TEST(a_stamp_that_exists_but_cannot_be_read_waits_the_whole_floor) {
    /* an agent DOWNLOAD or a virus scanner holding the file: round 1 read that
     * as "never switched" and did not wait at all */
    static const DWORD errs[] = { ERROR_SHARING_VIOLATION, ERROR_ACCESS_DENIED };
    unsigned i;
    for (i = 0; i < 2; i++) {
        fresh_box();
        a_switch();
        process_exit();                                /* A held its mode, stamped the revert */
        new_process();
        advance_ms(60000);                             /* B, long after: the file says 60 s */
        g_read_fail = errs[i];
        vcr_pace_before_switch();
        /* unknown != long ago; and a floor once, from when it was first seen
         * - not a floor per chunk, though it stays unreadable throughout */
        CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
        CHECK_EQ_U(g_sleeps, VCR_PACE_MIN_MS / VCR_PACE_CHUNK_MS);
        vcr_pace_after_switch();
    }
    /* and it beats this process's own, shorter memory of its last switch */
    fresh_box();
    a_switch();
    advance_ms(1000);
    g_read_fail = ERROR_SHARING_VIOLATION;
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);           /* not the 2000 the copy says */
}

TEST(a_short_torn_or_zeroed_stamp_waits_the_whole_floor) {
    /* 0 bytes is what a reader saw in round 1's CREATE_ALWAYS gap; a brand-new
     * file is 0 bytes for a moment even with OPEN_ALWAYS */
    static const DWORD lens[] = { 0, 4, 8 };
    unsigned i;
    for (i = 0; i < 3; i++) {
        fresh_box();
        put_stamp(i == 2 ? 0 : now_ms() - 60000, lens[i]);   /* the 8-byte one is zero */
        vcr_pace_before_switch();
        CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
        CHECK_EQ_U(g_sleeps, VCR_PACE_MIN_MS / VCR_PACE_CHUNK_MS);
    }
}

TEST(c_vcr_as_a_file_waits_the_whole_floor_and_says_the_stamp_is_lost) {
    /* no stamp can ever be kept there: every switch waits the whole floor
     * (the same process's copy included), and each lost stamp is reported */
    fresh_box();
    g_dir = DIR_IS_FILE;
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
    vcr_pace_after_switch();
    CHECK_EQ_U(g_reports, 2);                          /* the pre-stamp's and the stamp's */
    CHECK_EQ_U(g_report_err, ERROR_PATH_NOT_FOUND);
    CHECK(strstr(g_report_what, VCR_PACE_FILE) != NULL, "the report names the stamp");
    advance_ms(500);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
}

TEST(only_a_missing_stamp_means_never_switched) {
    fresh_box();
    g_dir = DIR_IS_DIR;                                /* C:\vcr there, no stamp in it */
    vcr_pace_before_switch();
    CHECK_EQ_U(g_sleeps, 0);
    vcr_pace_after_switch();
    /* the stamp deleted mid-run: this process still remembers its switch */
    memset(&g_file, 0, sizeof g_file);
    advance_ms(1000);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 1000);
}

TEST(the_stamp_is_written_in_place_never_truncated_first) {
    unsigned i;
    fresh_box();
    for (i = 0; i < 5; i++) {
        a_switch();
        CHECK_EQ_U(g_file.len, 8);
    }
    CHECK_EQ_U(g_truncations, 0);                      /* no CREATE_ALWAYS gap to read "never" in */
    CHECK_EQ_U(g_reports, 0);
}

/* ---- a failed write: this process's copy, a report, a retry ---------------- */
TEST(a_failed_stamp_write_paces_this_process_says_so_and_retries_at_exit) {
    ULONGLONG old, restored;
    fresh_box();
    put_stamp(now_ms() - 60000, 8);                    /* an old, readable stamp */
    old = stamp_ms();
    g_write_fail = ERROR_SHARING_VIOLATION;            /* a DOWNLOAD holds it */
    a_switch();
    CHECK_EQ_U(g_reports, 2);                          /* never silently: pre-stamp and stamp */
    CHECK_EQ_U(g_report_err, ERROR_SHARING_VIOLATION);
    CHECK_EQ_U(stamp_ms(), old);                       /* the file did not get it ... */
    advance_ms(1000);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 1000);    /* ... the process's copy paced it */
    vcr_pace_after_restore();
    restored = now_ms();
    CHECK_EQ_U(g_reports, 4);
    g_write_fail = 0;                                  /* the DOWNLOAD is done */
    advance_ms(200);
    reset_sleeps();
    process_exit();
    CHECK_EQ_U(g_sleeps, 0);                           /* nothing to hold ... */
    CHECK_EQ_U(stamp_ms(), restored);                  /* ... but the box gets the last switch */
    new_process();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 200);
    /* WriteFile itself failing is the same story ... */
    fresh_box();
    put_stamp(now_ms() - 60000, 8);
    g_writefile_fails = 1;
    a_switch();
    CHECK_EQ_U(g_reports, 2);
    CHECK_EQ_U(g_report_err, ERROR_DISK_FULL);
    advance_ms(500);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 500);
    /* ... and where the failed write CREATED the file, the empty stamp it
     * left reads as "just now" for everyone, this process included */
    fresh_box();
    g_writefile_fails = 1;
    a_switch();
    CHECK_EQ_U(g_reports, 2);
    CHECK(g_file.exists && !g_file.len, "an empty stamp was left");
    advance_ms(500);
    reset_sleeps();
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
}

/* ---- the wait reads again after every sleep ---------------------------------- */
TEST(the_wait_reads_the_stamp_again_after_every_sleep) {
    /* B wakes from the first chunk of the rest of A's floor - and someone
     * stamped meanwhile (the host's pace-mark after a kill, an exit hold): B
     * waits the floor from THAT, not from what it read before it slept */
    ULONGLONG t;
    unsigned reads;
    fresh_box();
    a_switch();                                        /* A, at t */
    t = now_ms();
    process_killed();
    new_process();
    advance_ms(1000);
    g_on_sleep = someone_stamps_now;                   /* at t + 1500, mid-chunk */
    reads = g_reads;
    vcr_pace_before_switch();
    CHECK_EQ_U(g_stamped_at, t + 1500);
    CHECK_EQ_U(g_slept_ms, 3500);                      /* 1000, then the new stamp's rest */
    CHECK_EQ_U(g_sleeps, 4);                           /* 1000 + 1000 + 1000 + 500 */
    CHECK(g_max_sleep <= VCR_PACE_CHUNK_MS, "never more than a chunk between reads");
    CHECK(g_reads - reads >= g_sleeps + 1, "read before and after every sleep");
    CHECK_EQ_U(now_ms() - g_stamped_at, VCR_PACE_MIN_MS);   /* the floor since the LATEST stamp */
}

TEST(a_stamp_that_never_stops_moving_cannot_stall_a_tool_forever) {
    /* nothing that follows the rules stamps this often - but a host stamping
     * in a loop must not wedge a tool. Round 2 settled for the last stamp it
     * read and SWITCHED; now it waits on to the hard cap and then refuses:
     * no switch, nothing stamped, the lock given back */
    unsigned writes;
    int go;
    fresh_box();
    a_switch();
    process_exit();
    new_process();
    g_on_sleep = a_storm_of_stamps;
    g_on_sleep_keep = 1;
    g_reports = 0;
    go = vcr_pace_before_switch();
    g_on_sleep = NULL;
    g_on_sleep_keep = 0;
    CHECK(!go, "the floor never passed: no switch");
    CHECK(g_vcr_pace_why && !strcmp(g_vcr_pace_why, VCR_PACE_WHY_WAIT), "and it says why");
    CHECK(VCR_PACE_WAIT_CAP_MS >= 2 * VCR_PACE_WAIT_MAX_MS, "the cap is twice the max");
    CHECK_EQ_U(g_slept_ms, VCR_PACE_WAIT_CAP_MS);      /* past the max, on to the cap, no further */
    CHECK_EQ_U(g_reports, 2);                          /* "keeps moving" at the max, then the refusal */
    CHECK_EQ_U(g_report_err, ERROR_TIMEOUT);
    CHECK(lock_is_free_and_balanced(), "the refused wait gives its lock back");
    CHECK_EQ_U(g_vcr_pace_mine, 0);                    /* no switch of its own recorded */
    writes = g_file.writes;
    process_exit();
    CHECK_EQ_U(g_file.writes, writes);                 /* and none at exit: it held no mode */
}

/* ---- the switch lock ----------------------------------------------------------- */
TEST(the_switch_lock_is_held_from_the_wait_to_the_stamp) {
    fresh_box();
    vcr_pace_before_switch();
    CHECK(!strcmp(g_mx_name, VCR_PACE_MUTEX), "one named lock for the box");
    CHECK_EQ_I(g_mx.owner, g_proc);                    /* held across the wait ... */
    CHECK_EQ_U(g_mx.count, 1);
    CHECK_EQ_U(g_unlocked_reads, 0);                   /* ... which read the stamp under it */
    vcr_pace_after_switch();
    CHECK(lock_is_free_and_balanced(), "given back ...");
    CHECK_EQ_U(g_unlocked_writes, 0);                  /* ... AFTER the stamp: the next holder reads it */
    CHECK_EQ_U(stamp_ms(), now_ms());
}

TEST(the_switch_lock_is_balanced_however_the_calls_nest) {
    unsigned i;
    fresh_box();
    for (i = 0; i < 5; i++)
        a_switch();
    CHECK(lock_is_free_and_balanced(), "plain switches");
    CHECK_EQ_U(g_mx_creates, 1);                       /* opened once per process */
    /* a hold, then the give-back paced inside it; and vcrctl modeseq, whose
     * set_mode paces again inside the loop's wait - which must not wait a
     * second floor from the first one's pre-stamp */
    vcr_pace_before_switch();
    reset_sleeps();
    CHECK(vcr_pace_before_switch(), "nested: go ahead");
    CHECK_EQ_U(g_sleeps, 0);
    CHECK_EQ_U(g_mx.count, 2);
    vcr_pace_after_restore();
    CHECK(lock_is_free_and_balanced(), "one after-call gives back both");
    /* vcrctl ddraw after a FAILED RestoreDisplayMode */
    vcr_pace_before_switch();
    vcr_pace_after_switch_ex(1);                       /* pace_gave_back(0) */
    vcr_pace_before_switch();                          /* leaving exclusive mode */
    vcr_pace_after_switch();
    CHECK(lock_is_free_and_balanced(), "vcrctl ddraw's failure path");
    /* vcrctl modeseq stopped from the host after its wait: no switch */
    vcr_pace_before_switch();
    vcr_pace_cancel();
    CHECK(lock_is_free_and_balanced(), "a cancelled wait");
    CHECK_EQ_U(g_unlocked_reads, 0);
    /* an after-call with no before-call gives back nothing that is not ours */
    vcr_pace_after_restore();
    CHECK(lock_is_free_and_balanced(), "nothing to give back");
    process_exit();
    CHECK(!g_mx.abandoned, "a normal exit leaves the lock free, not abandoned");
}

TEST(a_stuck_holder_cannot_deadlock_a_queued_tool_or_let_it_beat_the_floor) {
    /* Round 2 went on WITHOUT the lock after the timeout and a floor - and a
     * stuck holder may switch at any moment. Now the queued tool does not
     * switch at all: it says "pace lock busy" and neither waits nor stamps */
    int stuck;
    unsigned writes;
    ULONGLONG asked;
    fresh_box();
    a_switch();
    vcr_pace_before_switch();                          /* A, hung inside its switch */
    stuck = g_proc;
    writes = g_file.writes;
    new_process();                                     /* B queues behind it */
    reset_sleeps();
    asked = now_ms();
    CHECK(!vcr_pace_before_switch(), "B does not switch");
    CHECK(g_vcr_pace_why && !strcmp(g_vcr_pace_why, VCR_PACE_WHY_LOCK), "\"pace lock busy\"");
    CHECK(!strcmp(VCR_PACE_WHY_LOCK, "pace lock busy"), "the words the host keys on");
    CHECK_EQ_U(g_mx_timeouts, 1);                      /* a bounded wait for the lock ... */
    CHECK_EQ_U(now_ms() - asked, VCR_PACE_LOCK_MS);
    CHECK(VCR_PACE_LOCK_MS <= 20000u, "short enough for a host's command budget");
    CHECK_EQ_U(g_reports, 1);                          /* ... said so ... */
    CHECK_EQ_U(g_report_err, WAIT_TIMEOUT);
    CHECK_EQ_U(g_sleeps, 0);                           /* ... and no floor: nothing to pace */
    CHECK_EQ_U(g_file.writes, writes);                 /* no stamp for a switch not made */
    CHECK_EQ_I(g_mx.owner, stuck);                     /* B gave back nothing it lacked */
    CHECK_EQ_U(g_mx_bad_releases, 0);
    /* no lock at all (CreateMutex refused): refused the same way */
    fresh_box();
    g_mx_create_fails = 1;
    CHECK(!vcr_pace_before_switch(), "no lock, no switch");
    CHECK(g_vcr_pace_why && !strcmp(g_vcr_pace_why, VCR_PACE_WHY_LOCK), "\"pace lock busy\"");
    CHECK_EQ_U(g_reports, 1);
    CHECK_EQ_U(g_sleeps, 0);
    CHECK_EQ_U(g_file.writes, 0);
    CHECK_EQ_U(g_mx_bad_releases, 0);
}

TEST(a_lock_abandoned_by_a_killed_tool_is_acquired_and_waits_a_floor) {
    /* modeseq spends most of its life waiting under the lock - holding the
     * previous test mode. Killed there, XP reverts that mode at once and
     * nothing records it; the abandoned lock is the one trace, and it counts
     * as acquired AND as a switch just now */
    fresh_box();
    a_switch();                                        /* A: into a test mode */
    advance_ms(1000);
    vcr_pace_before_switch();                          /* A waits to switch on ... */
    process_killed();                                  /* ... and is tree-killed */
    CHECK(g_mx.abandoned, "the kernel abandons the lock");
    new_process();
    advance_ms(500);                                   /* B: A's pre-stamp is 0.5 s old */
    vcr_pace_before_switch();
    CHECK_EQ_I(g_mx.owner, g_proc);                    /* WAIT_ABANDONED is acquired */
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);           /* from the kill, not from A's stamp */
    CHECK_EQ_U(g_reports, 1);
    CHECK_EQ_U(g_report_err, WAIT_ABANDONED);
    vcr_pace_after_switch();
    CHECK(lock_is_free_and_balanced(), "and given back like any other");
}

TEST(pace_mark_after_a_kill_measures_the_next_switch_from_the_revert) {
    /* A killed OUTSIDE the lock (mid GDI test, say): nothing of ours runs.
     * The host runs `vcrctl pace-mark` next, which switches nothing */
    ULONGLONG killed_at;
    unsigned takes;
    fresh_box();
    a_switch();                                        /* A: into a test mode at t */
    advance_ms(1000);
    process_killed();                                  /* XP reverts it at t + 1000 */
    killed_at = now_ms();
    new_process();                                     /* vcrctl pace-mark */
    takes = g_mx_takes;
    CHECK(vcr_pace_mark(), "pace-mark writes the stamp");
    CHECK_EQ_U(stamp_ms(), killed_at);
    CHECK_EQ_U(g_mx_takes, takes);                     /* no lock: it stamps the kill, not later */
    process_exit();
    CHECK_EQ_U(g_sleeps, 0);                           /* and holds nothing */
    new_process();                                     /* B, 500 ms after the kill */
    advance_ms(500);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 500);     /* not the 1500 A's stamp would give */
    /* a pace-mark that cannot write says so */
    new_process();
    g_write_fail = ERROR_ACCESS_DENIED;
    CHECK(!vcr_pace_mark(), "a failed pace-mark is not ok");
    CHECK_EQ_U(g_reports, 1);
}

/* ---- a crash ------------------------------------------------------------------- */
TEST(a_crash_holding_a_temporary_mode_holds_it_stamps_and_ends_the_process) {
    fresh_box();
    a_switch();                                        /* into a test mode */
    advance_ms(500);
    reset_sleeps();
    CHECK_EQ_I(process_crashes(), EXCEPTION_EXECUTE_HANDLER);
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS - 500);     /* the mode held for the floor */
    /* the revert recorded - ahead: XP drops the dead process's mode after this */
    CHECK_EQ_U(stamp_ms(), now_ms() + VCR_PACE_EXIT_LAG_MS);
    CHECK_EQ_U(g_crt_calls, 1);                        /* chained to the filter before */
    CHECK(lock_is_free_and_balanced(), "the lock is given back, not abandoned");
    reset_sleeps();
    process_exit();                                    /* msvcrt may run atexit on the way out */
    CHECK_EQ_U(g_sleeps, 0);                           /* ... which must not hold it again */
    new_process();
    vcr_pace_before_switch();
    /* the next tool: from the revert's latest */
    CHECK_EQ_U(g_slept_ms, VCR_PACE_EXIT_LAG_MS + VCR_PACE_MIN_MS);
}

TEST(a_crash_in_the_middle_of_a_switch_is_held_as_one) {
    /* a driver DLL faulting inside SetDisplayMode: the switch may have landed
     * a moment ago, unrecorded - hold it a whole floor. The process's FIRST
     * switch included: the filter is in before it starts */
    fresh_box();
    vcr_pace_before_switch();                          /* nothing recorded yet */
    CHECK(g_filter == vcr_pace_crash_filter, "installed by the first before-call");
    advance_ms(100);
    CHECK_EQ_I(process_crashes(), EXCEPTION_EXECUTE_HANDLER);
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
    CHECK_EQ_U(stamp_ms(), now_ms() + VCR_PACE_EXIT_LAG_MS);
    CHECK(lock_is_free_and_balanced(), "both takes given back");
}

TEST(a_crash_with_no_mode_held_is_left_to_the_filter_before) {
    unsigned writes;
    fresh_box();
    a_switch();
    vcr_pace_before_switch();
    vcr_pace_after_restore();                          /* nothing held any more */
    writes = g_file.writes;
    reset_sleeps();
    CHECK_EQ_I(process_crashes(), EXCEPTION_CONTINUE_SEARCH);   /* the CRT's answer */
    CHECK_EQ_U(g_crt_calls, 1);
    CHECK_EQ_U(g_sleeps, 0);
    CHECK_EQ_U(g_file.writes, writes);
    /* and with no filter installed before ours, the default handling */
    fresh_box();
    g_filter = NULL;
    a_switch();
    vcr_pace_before_switch();
    vcr_pace_after_restore();
    CHECK_EQ_I(process_crashes(), EXCEPTION_CONTINUE_SEARCH);
}

TEST(a_filter_that_repaired_the_fault_keeps_the_pacing_state) {
    fresh_box();
    a_switch();
    g_crt_answer = EXCEPTION_CONTINUE_EXECUTION;       /* a signal() handler fixed it */
    CHECK_EQ_I(process_crashes(), EXCEPTION_CONTINUE_EXECUTION);
    CHECK(lock_is_free_and_balanced(), "the filter's own take given back");
    CHECK(g_vcr_pace_temp_mode, "the mode is still held, so the exit still holds it");
    advance_ms(100);
    reset_sleeps();
    process_exit();
    /* the filter's hold stamped a revert ahead, and the exit waits it out */
    CHECK_EQ_U(g_slept_ms, VCR_PACE_EXIT_LAG_MS + VCR_PACE_MIN_MS - 100);
    /* repaired mid-switch: the caller's take is still the caller's */
    fresh_box();
    g_crt_answer = EXCEPTION_CONTINUE_EXECUTION;
    vcr_pace_before_switch();
    process_crashes();
    CHECK_EQ_I(g_mx.owner, g_proc);
    CHECK_EQ_U(g_mx.count, 1);
    vcr_pace_after_switch();
    CHECK(lock_is_free_and_balanced(), "the caller's after-call gives it back");
}

/* ---- round 3: the lock fails closed ------------------------------------------ */
TEST(a_tool_refused_on_the_way_out_still_gives_its_mode_back_through_the_exit_hold) {
    /* The switch OUT refused: the tool must not give the mode back by another
     * route (a Release, grGlideShutdown - unpaced) but end its run, and XP
     * reverts the mode as it exits. The exit hold paces that revert even with
     * the lock still stuck: XP makes it whatever the hold does */
    fresh_box();
    a_switch();                                        /* into a temporary mode */
    g_mx.owner = 99;                                   /* another tool hangs holding the lock */
    g_mx.count = 1;
    advance_ms(500);
    CHECK(!vcr_pace_before_switch(), "the switch out is refused");
    CHECK(g_vcr_pace_temp_mode, "the mode is still held");
    reset_sleeps();
    process_exit();                                    /* return from main */
    CHECK_EQ_U(g_mx_timeouts, 2);                      /* the refusal's, and the exit hold's */
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);           /* a whole floor, lock or no lock */
    CHECK_EQ_U(stamp_ms(), now_ms() + VCR_PACE_EXIT_LAG_MS);   /* and the revert stamped */
    CHECK_EQ_I(g_mx.owner, 99);                        /* nothing given back it never had */
    CHECK_EQ_U(g_mx_bad_releases, 0);
}

/* ---- round 3: the pre-stamp ---------------------------------------------------- */
TEST(the_stamp_is_written_before_the_switch_as_well_as_after_it) {
    /* a switch call can take seconds (a mode set on the 4-chip board): once
     * the wait is over the switch is imminent, so the box is told then, and
     * again when it has landed. A reader in between sees "just now", not the
     * switch a minute ago; a tool killed inside the call leaves the pre-stamp
     * behind as well as the abandoned lock */
    ULONGLONG pre;
    unsigned writes;
    fresh_box();
    a_switch();
    advance_ms(60000);
    writes = g_file.writes;
    CHECK(vcr_pace_before_switch(), "go");
    pre = now_ms();
    CHECK_EQ_U(stamp_ms(), pre);
    CHECK_EQ_U(g_file.writes, writes + 1);
    CHECK_EQ_U(g_unlocked_writes, 0);                  /* under the lock */
    advance_ms(2500);                                  /* the switch call */
    CHECK_EQ_U(vcr_pace_last_ms(), pre);               /* what anyone reading now sees */
    vcr_pace_after_switch();
    CHECK_EQ_U(stamp_ms(), now_ms());                  /* and again once it has landed */
    advance_ms(60000);
    CHECK(vcr_pace_before_switch(), "go");
    pre = now_ms();
    advance_ms(1000);
    process_killed();                                  /* tree-killed inside the switch */
    CHECK_EQ_U(stamp_ms(), pre);                       /* the pre-stamp is its trace on the box */
}

TEST(a_before_call_nested_in_one_that_waited_does_not_wait_again) {
    /* vcrctl modeseq waits (then looks for its stop file), and set_mode paces
     * the same switch again inside that wait. Measured from the first one's
     * pre-stamp, the nested wait would sit a SECOND floor: 6 s a mode, twice
     * what the host budgets. Nested, it is the same switch */
    fresh_box();
    a_switch();
    reset_sleeps();
    CHECK(vcr_pace_before_switch(), "the loop's wait");
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
    reset_sleeps();
    CHECK(vcr_pace_before_switch(), "set_mode's, nested");
    CHECK_EQ_U(g_sleeps, 0);
    vcr_pace_after_switch();
    CHECK(lock_is_free_and_balanced(), "one after-call gives back both");
    reset_sleeps();
    CHECK(vcr_pace_before_switch(), "the next mode");
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);           /* a floor from that switch, not less */
    /* a stop seen after the wait: cancelled - the lock back, the pre-stamp
     * left (it costs the next switch a floor, never lets one come sooner) */
    vcr_pace_cancel();
    CHECK(lock_is_free_and_balanced(), "cancelled");
    CHECK_EQ_U(stamp_ms(), now_ms());
    reset_sleeps();
    CHECK(vcr_pace_before_switch(), "a before-call after a cancel is not nested");
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
}

/* ---- round 3: exit reverts land after the stamp ---------------------------------- */
TEST(a_stamp_ahead_is_waited_out_to_its_time_and_a_floor_past_it) {
    /* the exit hold and a paced kill stamp their revert AHEAD, by
     * VCR_PACE_EXIT_LAG_MS: glide3x's DLL_PROCESS_DETACH runs grGlideShutdown,
     * with ~4 s of idle waits, before its RestoreDisplayMode - after atexit */
    CHECK(VCR_PACE_EXIT_LAG_MS >= 5000u, "past glide3x's ~4 s shutdown");
    CHECK(VCR_PACE_AHEAD_MAX_MS >= VCR_PACE_EXIT_LAG_MS, "a gate's own stamp is in the horizon");
    fresh_box();
    put_stamp(now_ms() + VCR_PACE_EXIT_LAG_MS, 8);
    CHECK(vcr_pace_before_switch(), "go");
    /* until the stamp, then the floor - not a floor from now */
    CHECK_EQ_U(g_slept_ms, VCR_PACE_EXIT_LAG_MS + VCR_PACE_MIN_MS);
    CHECK(g_max_sleep <= VCR_PACE_CHUNK_MS, "in chunks");
    /* the horizon's edge: waited out the same way */
    fresh_box();
    put_stamp(now_ms() + VCR_PACE_AHEAD_MAX_MS, 8);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_AHEAD_MAX_MS + VCR_PACE_MIN_MS);
    /* past it no gate wrote the stamp: the clock went back - a whole floor,
     * not the minutes (or a year) the stamp claims */
    fresh_box();
    put_stamp(now_ms() + VCR_PACE_AHEAD_MAX_MS + 1, 8);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
    fresh_box();
    put_stamp(now_ms() + 365ull * 86400000u, 8);
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MIN_MS);
    /* a stamp put ahead WHILE a wait sleeps (another tool's exit hold, a
     * paced kill) is read at the next chunk and waited out */
    fresh_box();
    a_switch();
    process_killed();
    new_process();
    advance_ms(1000);
    g_on_sleep = someone_stamps_ahead;
    vcr_pace_before_switch();
    CHECK_EQ_U(now_ms() - g_stamped_at, VCR_PACE_MIN_MS);
}

/* ---- round 3: the pace argument ---------------------------------------------------- */
TEST(pace_arguments_are_refused_or_clamped_never_wrapped) {
    /* (DWORD)atoi("-1") was 0xFFFFFFFF: a floor of 49 days on every switch */
    static const struct { const char *s; int ok; DWORD v; } c[] = {
        { "0", 1, 0 }, { "3000", 1, 3000 }, { "30000", 1, 30000 }, { "0050", 1, 50 },
        { "30001", 0, 0 }, { "-1", 0, 0 }, { "+5", 0, 0 }, { "0x10", 0, 0 }, { "", 0, 0 },
        { "12a", 0, 0 }, { " 5", 0, 0 }, { "5 ", 0, 0 }, { "4294967295", 0, 0 },
        { "99999999999999999999", 0, 0 },
    };
    unsigned i;
    DWORD ms = 12345;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        ms = 12345;
        CHECK_EQ_I(vcr_pace_parse_ms(c[i].s, &ms), c[i].ok);
        CHECK_EQ_U(ms, c[i].ok ? c[i].v : 12345);      /* untouched when refused */
    }
    CHECK(!vcr_pace_parse_ms(NULL, &ms), "no argument");
    CHECK(VCR_PACE_MAX_MS == 30000u, "30 s at most");
    /* and whatever reaches set_min is clamped there */
    fresh_box();
    vcr_pace_set_min(0xFFFFFFFFul);
    CHECK_EQ_U(g_vcr_pace_ms, VCR_PACE_MAX_MS);
    vcr_pace_set_min(VCR_PACE_MAX_MS + 1);
    CHECK_EQ_U(g_vcr_pace_ms, VCR_PACE_MAX_MS);
    a_switch();
    reset_sleeps();
    CHECK(vcr_pace_before_switch(), "the largest floor is waited out, not refused");
    CHECK_EQ_U(g_slept_ms, VCR_PACE_MAX_MS);
    CHECK(VCR_PACE_MAX_MS + VCR_PACE_AHEAD_MAX_MS < VCR_PACE_WAIT_MAX_MS,
          "no legitimate wait reaches the max, let alone the cap");
}

/* ---- round 3: chunks, re-reads and the hard cap ------------------------------------ */
TEST(the_wait_sleeps_in_chunks_and_reads_the_stamp_after_each) {
    unsigned reads;
    fresh_box();
    vcr_pace_set_min(10000);
    a_switch();
    reset_sleeps();
    reads = g_reads;
    vcr_pace_before_switch();
    CHECK_EQ_U(g_slept_ms, 10000);
    CHECK(VCR_PACE_CHUNK_MS <= 1000u, "at most a second between reads");
    CHECK_EQ_U(g_max_sleep, VCR_PACE_CHUNK_MS);
    CHECK_EQ_U(g_sleeps, 10000 / VCR_PACE_CHUNK_MS);
    CHECK_EQ_U(g_reads - reads, g_sleeps + 1);         /* a read after every sleep */
}

/* ---- round 3: vcrctl pace-kill ----------------------------------------------------- */
TEST(pace_kill_refuses_anything_but_our_own_tools) {
    static const char *const ours[] = {
        "vcrctl.exe", "ddlab.exe", "d3dprobe.exe", "glidelab.exe", "DDLAB.EXE", "GlideLab.Exe",
        "C:\\vcr\\d3dprobe.exe",
    };
    static const char *const not_ours[] = {
        "retro_agent.exe", "RETRO_AGENT.EXE", "cmd.exe", "csrss.exe", "winlogon.exe",
        "explorer.exe", "System", "", "vcrctl", "vcrctl.exe.old", "xvcrctl.exe", "vcrctl.ex",
        "C:\\vcr\\retro_agent.exe", "C:\\vcrctl.exe\\cmd.exe",
    };
    const DWORD self = 1234;
    unsigned i;
    for (i = 0; i < sizeof ours / sizeof ours[0]; i++)
        CHECK(vcr_pace_kill_refusal(2000, self, ours[i]) == NULL, ours[i]);
    for (i = 0; i < sizeof not_ours / sizeof not_ours[0]; i++)
        CHECK(vcr_pace_kill_refusal(2000, self, not_ours[i]) != NULL, not_ours[i]);
    /* whatever the name: the Idle process, System, pace-kill itself */
    CHECK(vcr_pace_kill_refusal(0, self, "vcrctl.exe") != NULL, "pid 0");
    CHECK(vcr_pace_kill_refusal(4, self, "vcrctl.exe") != NULL, "pid 4");
    CHECK(vcr_pace_kill_refusal(self, self, "vcrctl.exe") != NULL, "itself");
    CHECK(vcr_pace_kill_refusal(2000, self, NULL) != NULL, "no such process");
    /* those need no process, so they are refused before anything is opened */
    CHECK(vcr_pace_kill_pid_refusal(0, self) != NULL, "pid 0, unopened");
    CHECK(vcr_pace_kill_pid_refusal(4, self) != NULL, "pid 4, unopened");
    CHECK(vcr_pace_kill_pid_refusal(self, self) != NULL, "itself, unopened");
    CHECK(vcr_pace_kill_pid_refusal(2000, self) == NULL, "any other pid goes on to the name");
}

TEST(pace_kill_is_made_as_a_paced_switch) {
    /* the victim may hold a test mode, which XP drops the instant it dies:
     * so the kill is a switch - under the lock, a floor after the latest
     * stamp - and its revert is stamped ahead */
    ULONGLONG t;
    int exited = -1;
    DWORD err = 99;
    fresh_box();
    someone_stamps_now();                              /* the victim switched into its mode */
    t = now_ms();
    g_victim.alive = 1;
    g_victim.dies = 1;
    new_process();                                     /* vcrctl pace-kill, a second later */
    advance_ms(1000);
    CHECK_EQ_I(vcr_pace_kill(&g_victim_tag, &exited, &err), 1);
    CHECK_EQ_I(exited, 1);
    CHECK_EQ_U(err, 0);
    CHECK_EQ_U(g_victim.terminates, 1);
    CHECK_EQ_U(g_victim.killed_at, t + VCR_PACE_MIN_MS);     /* a floor after the victim's switch */
    CHECK_EQ_I(g_victim.owner_at_kill, g_proc);              /* under the switch lock */
    CHECK_EQ_U(stamp_ms(), now_ms() + VCR_PACE_EXIT_LAG_MS); /* its revert, stamped ahead */
    CHECK(lock_is_free_and_balanced(), "the lock given back");
    CHECK_EQ_I(g_vcr_pace_temp_mode, 0);                     /* pace-kill holds no mode itself */
    process_exit();
    CHECK_EQ_U(g_natexit, 0);
    new_process();                                     /* the next switch on the box */
    CHECK(vcr_pace_before_switch(), "go");
    CHECK_EQ_U(g_slept_ms, VCR_PACE_EXIT_LAG_MS + VCR_PACE_MIN_MS);
}

TEST(pace_kill_kills_nothing_when_the_lock_is_busy) {
    /* the victim hung inside its own switch holds the lock: the paced kill is
     * refused like any switch, and the host falls back to a plain kill and a
     * pace-mark (said, in "pace lock busy") */
    int exited = -1;
    DWORD err = 99;
    fresh_box();
    g_victim.alive = 1;
    g_victim.dies = 1;
    g_mx.owner = 77;
    g_mx.count = 1;
    CHECK_EQ_I(vcr_pace_kill(&g_victim_tag, &exited, &err), 0);
    CHECK_EQ_U(g_victim.terminates, 0);
    CHECK(g_victim.alive, "the victim lives");
    CHECK_EQ_I(exited, 0);
    CHECK(g_vcr_pace_why && !strcmp(g_vcr_pace_why, VCR_PACE_WHY_LOCK), "pace lock busy");
    CHECK_EQ_U(g_file.writes, 0);                      /* no stamp for a kill not made */
    CHECK_EQ_I(g_mx.owner, 77);
    CHECK_EQ_U(g_mx_bad_releases, 0);
}

TEST(pace_kill_says_when_the_victim_is_not_gone_or_cannot_be_killed) {
    int exited = -1;
    DWORD err = 99;
    /* terminated, but a thread stuck in the kernel keeps it: a bounded wait,
     * exited = 0, and the revert stamped all the same */
    fresh_box();
    g_victim.alive = 1;
    g_victim.dies = 0;
    CHECK_EQ_I(vcr_pace_kill(&g_victim_tag, &exited, &err), 1);
    CHECK_EQ_I(exited, 0);
    CHECK_EQ_U(g_victim.waited, VCR_PACE_KILL_WAIT_MS);
    CHECK(VCR_PACE_KILL_WAIT_MS <= 10000u, "bounded");
    CHECK_EQ_U(stamp_ms(), now_ms() + VCR_PACE_EXIT_LAG_MS);
    CHECK(lock_is_free_and_balanced(), "given back");
    /* TerminateProcess refused: nothing died - said, with its error, the lock back */
    fresh_box();
    g_victim.alive = 1;
    g_victim.terminate_err = ERROR_ACCESS_DENIED;
    CHECK_EQ_I(vcr_pace_kill(&g_victim_tag, &exited, &err), -1);
    CHECK_EQ_U(err, ERROR_ACCESS_DENIED);
    CHECK_EQ_I(exited, 0);
    CHECK(g_victim.alive, "still alive");
    CHECK(lock_is_free_and_balanced(), "given back");
}

MUNIT_MAIN("vcr-kmd monitor pace gate (tools/vcr_pace.h)", {
    RUN(the_floor_is_at_least_three_seconds_on_a_file_on_the_box);
    RUN(a_fresh_box_switches_without_waiting);
    RUN(back_to_back_switches_wait_out_the_floor);
    RUN(the_2026_09_26_rhythm_is_slowed_to_the_floor);
    RUN(the_floor_holds_across_processes);
    RUN(a_clock_that_went_backwards_waits_the_whole_floor);
    RUN(set_min_raises_the_floor_and_never_lowers_it);
    RUN(a_temporary_mode_is_held_at_exit_and_the_revert_stamped);
    RUN(a_mode_given_back_is_not_held_again_at_exit);
    RUN(a_failed_give_back_keeps_the_exit_hold);
    RUN(the_exit_hold_is_registered_once);
    RUN(a_program_that_never_switches_is_never_held);
    RUN(a_stamp_that_exists_but_cannot_be_read_waits_the_whole_floor);
    RUN(a_short_torn_or_zeroed_stamp_waits_the_whole_floor);
    RUN(c_vcr_as_a_file_waits_the_whole_floor_and_says_the_stamp_is_lost);
    RUN(only_a_missing_stamp_means_never_switched);
    RUN(the_stamp_is_written_in_place_never_truncated_first);
    RUN(a_failed_stamp_write_paces_this_process_says_so_and_retries_at_exit);
    RUN(the_wait_reads_the_stamp_again_after_every_sleep);
    RUN(a_stamp_that_never_stops_moving_cannot_stall_a_tool_forever);
    RUN(the_switch_lock_is_held_from_the_wait_to_the_stamp);
    RUN(the_switch_lock_is_balanced_however_the_calls_nest);
    RUN(a_stuck_holder_cannot_deadlock_a_queued_tool_or_let_it_beat_the_floor);
    RUN(a_lock_abandoned_by_a_killed_tool_is_acquired_and_waits_a_floor);
    RUN(pace_mark_after_a_kill_measures_the_next_switch_from_the_revert);
    RUN(a_crash_holding_a_temporary_mode_holds_it_stamps_and_ends_the_process);
    RUN(a_crash_in_the_middle_of_a_switch_is_held_as_one);
    RUN(a_crash_with_no_mode_held_is_left_to_the_filter_before);
    RUN(a_filter_that_repaired_the_fault_keeps_the_pacing_state);
    RUN(a_tool_refused_on_the_way_out_still_gives_its_mode_back_through_the_exit_hold);
    RUN(the_stamp_is_written_before_the_switch_as_well_as_after_it);
    RUN(a_before_call_nested_in_one_that_waited_does_not_wait_again);
    RUN(a_stamp_ahead_is_waited_out_to_its_time_and_a_floor_past_it);
    RUN(pace_arguments_are_refused_or_clamped_never_wrapped);
    RUN(the_wait_sleeps_in_chunks_and_reads_the_stamp_after_each);
    RUN(pace_kill_refuses_anything_but_our_own_tools);
    RUN(pace_kill_is_made_as_a_paced_switch);
    RUN(pace_kill_kills_nothing_when_the_lock_is_busy);
    RUN(pace_kill_says_when_the_victim_is_not_gone_or_cannot_be_killed);
})
