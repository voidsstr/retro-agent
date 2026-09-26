/*
 * vcr_pace.h - the one gate every vcr-kmd tool passes through to change the
 * display mode (user-mode, Win32; header-only, include once per program).
 *
 * WHY. Every mode switch makes the monitor drop and re-acquire sync; on a CRT
 * a change of horizontal-frequency band clicks its mode relays and steps the
 * high voltage. On 2026-09-26 the first silicon battery on .124 (a 1998 Sony
 * CPD-G200) switched through 123 modes with a return to the desktop after
 * each: ~250 re-syncs at two a second, and the user heard every one. The fix
 * is not a politer caller but a floor enforced where the switch happens:
 *
 *   - at least VCR_PACE_MIN_MS between any two switches, ACROSS PROCESSES:
 *     the time of the last switch lives in a file on the box, so a script
 *     that runs one tool after another cannot go faster than the floor;
 *   - a mode a tool switched INTO is HELD at least VCR_PACE_MIN_MS before the
 *     tool switches out - including the switch XP makes for us when a process
 *     that set a temporary (CDS_FULLSCREEN / exclusive) mode exits.
 *
 * USE, around every switch - ChangeDisplaySettings, IDirectDraw
 * SetDisplayMode / RestoreDisplayMode, a fullscreen D3D CreateDevice /
 * Release, grSstWinOpen / grSstWinClose:
 *
 *     if (!vcr_pace_before_switch()) {    // the switch lock, the floor, a pre-stamp
 *         ...do NOT switch: say g_vcr_pace_why, end the run...
 *     }
 *     ...the switch...
 *     vcr_pace_after_switch();            // records it, gives the lock back
 *
 * (vcr_pace_after_restore() after a give-back.) A caller that waited and then
 * does NOT switch calls vcr_pace_cancel() - see there. A caller refused on the
 * way OUT of a temporary mode must not give it back by another route either
 * (a Release, leaving exclusive mode, grGlideShutdown): it ends the run, and
 * the exit hold below paces the revert XP makes as the process goes.
 *
 * FAIL CLOSED. The first two versions of this gate failed OPEN - each hole
 * below let a switch follow another at once. The rules now: a time the gate
 * cannot read counts as "switched just now", never as "long ago"; and a switch
 * the gate cannot pace is not made.
 *   - The stamp is written in place (OPEN_ALWAYS, 8 bytes at offset 0). It
 *     was CREATE_ALWAYS, which truncates first: a reader in that gap saw a
 *     0-byte file and took it for "never switched".
 *   - A stamp file that EXISTS but cannot be read in full - a sharing
 *     violation from an agent DOWNLOAD or a virus scanner, C:\vcr being a
 *     file, a short or zero stamp - waits the whole floor. Only a stamp that
 *     is not there at all means "never".
 *   - Every switch is also remembered IN-PROCESS and the wait is measured from
 *     the later of the two, so a stamp write that fails still paces this
 *     process. The failure is reported on stderr and the write retried at
 *     exit, because the next tool reads only the file.
 *   - A named mutex (VCR_PACE_MUTEX) is held from the wait through the switch
 *     to the stamp. Without it two tools read the same stamp, slept once,
 *     woke together and both switched. It is NOT optional: a tool that cannot
 *     have it within VCR_PACE_LOCK_MS (a holder hung in a driver call, a
 *     CreateMutex that failed) does not switch - vcr_pace_before_switch()
 *     answers 0 and g_vcr_pace_why says "pace lock busy". Round 2 went on
 *     without the lock after a floor, and a stuck holder may switch at any
 *     moment.
 *   - The wait sleeps at most VCR_PACE_CHUNK_MS at a time and reads the stamp
 *     again after every sleep: it ends when the floor has passed since the
 *     LATEST switch anyone recorded. A floor that never passes (a stamp that
 *     keeps moving) is given up after VCR_PACE_WAIT_CAP_MS - and then there is
 *     no switch. Round 2 settled for the last stamp it read and switched.
 *   - The stamp is written BEFORE the switch as well as after it: the switch
 *     is imminent once the wait is over, and a switch call can take seconds.
 *     A reader that measured from the previous stamp while it was in flight
 *     could overtake it.
 *
 * EXITS. The first vcr_pace_before_switch() installs a top-level exception
 * filter, the first vcr_pace_after_switch() an atexit() handler. A program
 * that returns from main - from ANY path, error paths included - or CRASHES
 * while it holds a temporary mode (or in the middle of a switch) holds that
 * mode for the floor and records the revert XP is about to make; a crash then
 * ends the process at once (the tools set SEM_NOGPFAULTERRORBOX, so no dialog
 * keeps the mode up). That revert comes AFTER the stamp: atexit runs before
 * the DLLs detach - glide3x's DLL_PROCESS_DETACH runs grGlideShutdown, with up
 * to ~4 s of idle waits, before its RestoreDisplayMode - and XP drops a dead
 * process's DirectDraw or CDS_FULLSCREEN mode later still. So the exit hold
 * stamps the revert AHEAD, at now + VCR_PACE_EXIT_LAG_MS, and a wait that
 * reads a stamp in the future waits until that time plus the floor.
 *
 * KILLS. Nothing inside a process can pace its own kill - TerminateProcess,
 * an agent PROCKILL, EXECW's tree-kill on timeout, taskkill /f: XP reverts the
 * mode the instant the process dies, and nothing records it. The host's
 * remedies are `vcrctl pace-kill <pid>` (a kill made through this gate:
 * vcr_pace_kill() below) and, after any other kill, `vcrctl pace-mark`, which
 * stamps the box so the next switch is measured from the revert. The gate
 * does what it can alone: a tool killed while it held the switch lock
 * (mid-wait, which is most of a paced sweep's life) leaves the lock ABANDONED,
 * and the next tool reads that as a switch just now.
 *
 * The floor is a floor: vcr_pace_set_min() can raise it (a caller's --pace),
 * never lower it, and never past VCR_PACE_MAX_MS - a pace argument outside
 * 0..VCR_PACE_MAX_MS is refused by vcr_pace_parse_ms(), not wrapped.
 */
#ifndef VCR_PACE_H
#define VCR_PACE_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VCR_PACE_MIN_MS       3000u
/* the most a caller's --pace / PACE_MS may raise the floor to. It was
 * atoi() into a DWORD: "-1" became 0xFFFFFFFF, a floor of seven weeks */
#define VCR_PACE_MAX_MS       30000u
#define VCR_PACE_DIR          "C:\\vcr"
#define VCR_PACE_FILE         "C:\\vcr\\lastswitch.dat"
/* one switch at a time on the box, from the wait to the stamp */
#define VCR_PACE_MUTEX        "vcr-pace"
/* how long to queue behind another tool's switch before refusing to switch.
 * A holder waits out one floor and switches - seconds; one that keeps the lock
 * this long is stuck (hung in a driver call, suspended) and may switch at any
 * moment, so the queued tool must not switch at all. Short enough that the
 * host's command budgets can include it. */
#define VCR_PACE_LOCK_MS      20000u
/* the longest single sleep of a wait: the stamp is read again after each */
#define VCR_PACE_CHUNK_MS     1000u
/* a wait this long has outlived any floor a caller can ask for: it says so
 * once and goes on waiting ... */
#define VCR_PACE_WAIT_MAX_MS  60000u
/* ... to this hard cap (2 x VCR_PACE_WAIT_MAX_MS), and then refuses the switch.
 * Nothing that follows the rules moves the stamp that often; a host stamping
 * in a loop must not stall a tool forever, nor let it switch. */
#define VCR_PACE_WAIT_CAP_MS  120000u
/* how far after its stamp a revert can land: the exit hold's and a paced
 * kill's stamps are this far AHEAD (see EXITS above) */
#define VCR_PACE_EXIT_LAG_MS  5000u
/* the furthest ahead a stamp written by this gate can be (with room for a
 * clock nudged back a few seconds). A stamp further ahead than this is not an
 * exit hold's: the clock went back, and the elapsed time is unknown */
#define VCR_PACE_AHEAD_MAX_MS 10000u

/* what vcr_pace_last_ms() answers when there is no time to give */
#define VCR_PACE_NEVER        0ull       /* no stamp: nothing ever switched */
#define VCR_PACE_UNKNOWN      (~0ull)    /* a stamp is there and cannot be read */

/* g_vcr_pace_why after a refusal: the host keys on these words */
#define VCR_PACE_WHY_LOCK     "pace lock busy"
#define VCR_PACE_WHY_WAIT     "pace floor never passed"

#ifndef VCR_PACE_REPORT
/* stderr: the tools answer on stdout, one JSON line at a time, and the host
 * skips lines that are not JSON - so this is seen without becoming an answer */
#define VCR_PACE_REPORT(what, err) \
    fprintf(stderr, "vcr_pace: %s (error %lu)\n", (what), (unsigned long)(err))
#endif

static DWORD     g_vcr_pace_ms = VCR_PACE_MIN_MS;
static int       g_vcr_pace_exit_armed;
static int       g_vcr_pace_filter_armed;
static int       g_vcr_pace_temp_mode;      /* a temporary mode is (probably) set */
static ULONGLONG g_vcr_pace_mine;           /* this process's last switch, ms; 0 = none */
static int       g_vcr_pace_unsaved;        /* ... which the box's stamp does not have */
static HANDLE    g_vcr_pace_lock;           /* VCR_PACE_MUTEX, opened on first use */
static unsigned  g_vcr_pace_depth;          /* how often this process holds it */
static int       g_vcr_pace_armed;          /* a before-call waited and pre-stamped; its switch is to come */
/* why the last vcr_pace_before_switch() refused (VCR_PACE_WHY_*); NULL after
 * one that let the switch go ahead */
static const char *g_vcr_pace_why;
static LPTOP_LEVEL_EXCEPTION_FILTER g_vcr_pace_prev_filter;

static ULONGLONG vcr_pace_now_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((ULONGLONG)ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000u;
}

/* The box's last switch, from the stamp file. VCR_PACE_NEVER only when there
 * is no stamp; VCR_PACE_UNKNOWN when there is one that cannot be read in
 * full, which the wait treats as a switch just now. */
static ULONGLONG vcr_pace_last_ms(void)
{
    ULONGLONG t = 0;
    DWORD got = 0, err;
    HANDLE h = CreateFileA(VCR_PACE_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND)
            return VCR_PACE_NEVER;
        if (err != ERROR_PATH_NOT_FOUND)
            return VCR_PACE_UNKNOWN;        /* sharing violation, access denied ... */
        /* no C:\vcr: nothing was ever stamped. But a FILE called C:\vcr
         * answers the same, and there no stamp can ever be written - so every
         * switch must wait the whole floor, not none */
        if (GetFileAttributesA(VCR_PACE_DIR) != INVALID_FILE_ATTRIBUTES)
            return VCR_PACE_UNKNOWN;
        err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? VCR_PACE_NEVER
                                                                          : VCR_PACE_UNKNOWN;
    }
    if (!ReadFile(h, &t, sizeof t, &got, NULL) || got != sizeof t || t == VCR_PACE_NEVER ||
        t == VCR_PACE_UNKNOWN)
        t = VCR_PACE_UNKNOWN;               /* short, torn or zeroed: it is there */
    CloseHandle(h);
    return t;
}

/* Write t to the box's stamp IN PLACE - OPEN_ALWAYS, 8 bytes at offset 0 -
 * never truncating first (a reader in that gap would see "never"; a brand-new
 * file is 0 bytes for a moment too, and the reader takes that as "just now").
 * No FlushFileBuffers: the other tools read through the same cache, and a
 * stamp lost to a power cut describes a mode the power cut ended anyway.
 * A failure is reported and remembered for the retry at exit. */
static int vcr_pace_store(ULONGLONG t)
{
    DWORD put = 0, err = 0;
    int ok = 0;
    HANDLE h;
    CreateDirectoryA(VCR_PACE_DIR, NULL);
    h = CreateFileA(VCR_PACE_FILE, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
    } else {
        ok = WriteFile(h, &t, sizeof t, &put, NULL) && put == sizeof t;
        if (!ok)
            err = GetLastError();
        CloseHandle(h);
    }
    g_vcr_pace_unsaved = !ok;
    if (!ok)
        VCR_PACE_REPORT("cannot write the switch stamp " VCR_PACE_FILE
                        " - the next tool on the box cannot see this switch", err);
    return ok;
}

/* A switch happened now. Remembered here first - that copy paces this
 * process whatever becomes of the file - then on the box. */
static int vcr_pace_mark(void)
{
    g_vcr_pace_mine = vcr_pace_now_ms();
    return vcr_pace_store(g_vcr_pace_mine);
}

/* A switch that will land up to `ms` from now and that nothing will be left
 * to record when it does - the revert XP makes after the exit hold, or after
 * a paced kill. Stamped at its latest, so the next switch waits a floor past
 * that. */
static int vcr_pace_mark_ahead(DWORD ms)
{
    g_vcr_pace_mine = vcr_pace_now_ms() + ms;
    return vcr_pace_store(g_vcr_pace_mine);
}

/* What is left of a floor `full` that began at t. A t after now means the
 * clock went back since: the elapsed time is unknown, so the whole floor. */
static DWORD vcr_pace_rest(ULONGLONG t, ULONGLONG now, DWORD full)
{
    if (t > now)
        return full;
    return now - t < full ? full - (DWORD)(now - t) : 0;
}

/* Fold one recorded switch time into the wait (*need, the sleep still owed).
 * A stamp AHEAD of now by no more than any gate writes is a revert still to
 * come (an exit hold's, a paced kill's): waited out to its time and a floor
 * past it. Further ahead, the clock went back: a blind key, which the wait
 * turns into a whole floor. *key comes in as the blind key already in force,
 * and a stamp that IS that key stays blind: judged once, when first seen - as
 * the clock catches up with it, it would otherwise slide inside the horizon
 * and be waited out to its time after all. */
static void vcr_pace_fold(ULONGLONG t, ULONGLONG now, DWORD full, DWORD *need, ULONGLONG *key)
{
    DWORD n;
    if (t == VCR_PACE_NEVER || t == *key)
        return;
    if (t > now + VCR_PACE_AHEAD_MAX_MS) {
        if (*key != VCR_PACE_UNKNOWN && t > *key)
            *key = t;
        return;
    }
    n = t > now ? (DWORD)(t - now) + full : vcr_pace_rest(t, now, full);
    if (n > *need)
        *need = n;
}

/* Sleep until the floor has passed since the LATEST switch recorded - by the
 * box's stamp or by this process - at most VCR_PACE_CHUNK_MS at a time, and
 * read the stamp again after every sleep: the host's pace-mark after a kill,
 * an exit hold, a crash filter can move it while this one sleeps. An elapsed
 * time that cannot be measured (the stamp unreadable, or far in the future
 * because the clock went back) waits a whole floor from when it was first seen
 * rather than none: an unknown elapsed time is not a known-long one. Once per
 * such condition, not once per read, or a stamp that stays unreadable would
 * never let a tool switch. 1 = the floor has passed; 0 = it did not pass
 * within VCR_PACE_WAIT_CAP_MS, and the caller must not switch. */
static int vcr_pace_wait(void)
{
    ULONGLONG blind = 0, blind_at = 0;      /* the condition, and when it was first seen */
    DWORD slept = 0, n;
    int said = 0;
    for (;;) {
        ULONGLONG last = vcr_pace_last_ms(), now = vcr_pace_now_ms(), key = blind;
        DWORD full = g_vcr_pace_ms, need = 0;
        if (last == VCR_PACE_UNKNOWN) {
            key = VCR_PACE_UNKNOWN;
            last = VCR_PACE_NEVER;
        }
        vcr_pace_fold(last, now, full, &need, &key);
        vcr_pace_fold(g_vcr_pace_mine, now, full, &need, &key);
        /* the whole floor is owed from when the condition was first seen - a
         * deadline, not a sleep, so it survives being slept in chunks */
        if (key != blind) {
            blind = key;
            blind_at = now;
        }
        if (blind && (n = vcr_pace_rest(blind_at, now, full)) > need)
            need = n;
        if (!need)
            return 1;
        if (slept >= VCR_PACE_WAIT_CAP_MS) {
            VCR_PACE_REPORT("the switch stamp " VCR_PACE_FILE " kept moving and the floor never"
                            " passed - not switching", ERROR_TIMEOUT);
            return 0;
        }
        if (slept >= VCR_PACE_WAIT_MAX_MS && !said) {
            said = 1;
            VCR_PACE_REPORT("the switch stamp " VCR_PACE_FILE " keeps moving - still waiting",
                            ERROR_TIMEOUT);
        }
        if (need > VCR_PACE_CHUNK_MS)
            need = VCR_PACE_CHUNK_MS;
        Sleep(need);
        slept += need;
    }
}

/* raise the floor to ms (a caller's --pace), never past VCR_PACE_MAX_MS */
static void vcr_pace_set_min(DWORD ms)
{
    if (ms > VCR_PACE_MAX_MS)
        ms = VCR_PACE_MAX_MS;
    if (ms > g_vcr_pace_ms)
        g_vcr_pace_ms = ms;
}

/* A --pace / PACE_MS argument: plain decimal milliseconds, 0..VCR_PACE_MAX_MS.
 * Anything else - a sign, a hex prefix, trailing junk, more than the maximum -
 * is refused (0), for the caller to report and stop on: a pace nobody meant is
 * not quietly turned into one (atoi("-1") in a DWORD was a seven-week floor). */
static int vcr_pace_parse_ms(const char *s, DWORD *ms)
{
    DWORD v = 0;
    if (!s || *s < '0' || *s > '9')
        return 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (DWORD)(*s - '0');
        if (v > VCR_PACE_MAX_MS)
            return 0;
    }
    if (*s)
        return 0;
    *ms = v;
    return 1;
}

/* Take the box's switch lock: 1 = held, 0 = not. Recursive (a hold, then a
 * give-back inside it) and counted, so the after-call gives back exactly what
 * was taken. Anything but a clean acquire counts as a switch just now - a
 * whole floor for anything this process does next, and longer if the stamp
 * says so:
 *   - WAIT_ABANDONED: acquired, but its owner died holding it - killed or
 *     crashed while it waited to switch or was switching - and XP has just
 *     reverted whatever mode it held, unrecorded;
 *   - a timeout (a stuck holder, which may yet switch at any moment) or a
 *     failure: NOT held, and the caller must not switch (the exit hold, which
 *     cannot stop XP's revert, goes on without it). */
static int vcr_pace_lock(void)
{
    DWORD got = WAIT_FAILED;
    if (!g_vcr_pace_lock)
        g_vcr_pace_lock = CreateMutexA(NULL, FALSE, VCR_PACE_MUTEX);
    if (g_vcr_pace_lock)
        got = WaitForSingleObject(g_vcr_pace_lock, VCR_PACE_LOCK_MS);
    if (got == WAIT_OBJECT_0 || got == WAIT_ABANDONED)
        g_vcr_pace_depth++;
    if (got == WAIT_OBJECT_0)
        return 1;
    if (got == WAIT_ABANDONED)
        VCR_PACE_REPORT("switch lock " VCR_PACE_MUTEX " abandoned - its owner died holding"
                        " it; waiting a whole floor", got);
    else
        VCR_PACE_REPORT("no switch lock " VCR_PACE_MUTEX " - " VCR_PACE_WHY_LOCK,
                        got == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
    g_vcr_pace_mine = vcr_pace_now_ms();
    return got == WAIT_ABANDONED;
}

/* Give back what this process took beyond `depth`. Given back entirely, no
 * waited-for switch of this process is pending any more. */
static void vcr_pace_unlock_to(unsigned depth)
{
    while (g_vcr_pace_depth > depth) {
        g_vcr_pace_depth--;
        ReleaseMutex(g_vcr_pace_lock);
    }
    if (!g_vcr_pace_depth)
        g_vcr_pace_armed = 0;
}

/* The switch the caller waited for is not going to happen (vcrctl modeseq
 * stopped from the host before its next mode): give the lock back without
 * recording anything more. A lock kept to the end of the process would be
 * found ABANDONED, and the next tool would wait a floor for a switch never
 * made. (A macro: a static function a lab never calls is a warning there.) */
#define vcr_pace_cancel() vcr_pace_unlock_to(0)

/* The process is about to lose a mode that XP reverts as it goes: hold the
 * mode for the floor, then record the revert - AHEAD, because it lands after
 * this runs (see EXITS). Neither a lock that cannot be had nor a floor that
 * never passes stops here: XP makes that revert whatever this does, so the
 * best left is to delay it by the floor and stamp it. The callers give the
 * lock back after the stamp rather than keep it to the end: a tool that takes
 * it then waits out the stamp, while a lock kept to the end would be found
 * abandoned, which means a death nobody paced. */
static void vcr_pace_hold_to_exit(void)
{
    vcr_pace_lock();
    vcr_pace_wait();
    vcr_pace_mark_ahead(VCR_PACE_EXIT_LAG_MS);
}

static void vcr_pace_at_exit(void)
{
    if (g_vcr_pace_temp_mode) {
        g_vcr_pace_temp_mode = 0;           /* once: the crash filter must not hold again */
        vcr_pace_hold_to_exit();
    } else if (g_vcr_pace_unsaved) {
        /* the box never got this process's last switch and the next tool
         * reads only the box: one more try, now the DOWNLOAD or scanner that
         * held the file may be done with it */
        vcr_pace_store(g_vcr_pace_mine);
    }
    vcr_pace_unlock_to(0);                  /* never left for the next tool to find abandoned */
}

/* A crash skips atexit(). This runs instead, on the faulting thread, before
 * XP ends the process and reverts its mode. Held = a temporary mode is set,
 * or the crash came between vcr_pace_before_switch and _after_switch - inside
 * a driver's SetDisplayMode, say - where a switch may just have been made.
 * Then hold and stamp as at exit, and end the process
 * (EXCEPTION_EXECUTE_HANDLER - no dialog, the tools set
 * SEM_NOGPFAULTERRORBOX). The filter installed before this one (mingw's,
 * which runs a signal() handler) still gets its say: if it repaired the fault,
 * execution continues, and so does the pacing state it had. */
static LONG WINAPI vcr_pace_crash_filter(struct _EXCEPTION_POINTERS *ep)
{
    int temp = g_vcr_pace_temp_mode, held = temp || g_vcr_pace_depth;
    unsigned depth = g_vcr_pace_depth;
    LONG r = EXCEPTION_CONTINUE_SEARCH;
    if (held) {
        g_vcr_pace_temp_mode = 0;           /* an atexit run by the exit must not hold again */
        /* mid-switch: the switch it was making may have landed a moment ago,
         * unrecorded - hold it as one, the whole floor */
        if (depth)
            g_vcr_pace_mine = vcr_pace_now_ms();
        vcr_pace_hold_to_exit();
    }
    if (g_vcr_pace_prev_filter)
        r = g_vcr_pace_prev_filter(ep);
    if (r == EXCEPTION_CONTINUE_EXECUTION) {
        g_vcr_pace_temp_mode = temp;
        vcr_pace_unlock_to(depth);
        return r;
    }
    vcr_pace_unlock_to(0);
    return held ? EXCEPTION_EXECUTE_HANDLER : r;
}

/* Once per process, chained to the filter already there (mingw's CRT
 * installs one before main). */
static void vcr_pace_arm_filter(void)
{
    if (g_vcr_pace_filter_armed)
        return;
    g_vcr_pace_filter_armed = 1;
    g_vcr_pace_prev_filter = SetUnhandledExceptionFilter(vcr_pace_crash_filter);
}

/* Waits out the floor since the last switch, holding the box's switch lock,
 * and stamps the switch the caller is about to make. 1 = go ahead and switch
 * (the lock is held until the after-call); 0 = do NOT switch - the lock could
 * not be had or the floor never passed (g_vcr_pace_why says which), and
 * nothing is held. The crash filter goes in HERE, before the first switch
 * rather than after it: a driver faulting inside that very switch is the
 * likeliest crash.
 * A before-call NESTED in one that already waited (vcrctl modeseq waits,
 * checks its stop file, then set_mode paces the same switch again) does not
 * wait a second floor from its own pre-stamp: it is the same switch. */
static int vcr_pace_before_switch(void)
{
    unsigned depth = g_vcr_pace_depth;
    vcr_pace_arm_filter();
    if (!vcr_pace_lock()) {
        g_vcr_pace_why = VCR_PACE_WHY_LOCK;
        return 0;
    }
    if (depth && g_vcr_pace_armed) {
        g_vcr_pace_why = NULL;
        return 1;
    }
    if (!vcr_pace_wait()) {
        vcr_pace_unlock_to(depth);
        g_vcr_pace_why = VCR_PACE_WHY_WAIT;
        return 0;
    }
    vcr_pace_mark();                        /* the pre-stamp: the switch is imminent */
    g_vcr_pace_armed = 1;
    g_vcr_pace_why = NULL;
    return 1;
}

/* temp: 1 = this program now holds a temporary mode XP will revert at exit
 * (CDS_FULLSCREEN, DirectDraw exclusive, fullscreen D3D, Glide open);
 * 0 = it just gave one back (restore / release / close). */
static void vcr_pace_after_switch_ex(int temp)
{
    vcr_pace_mark();
    g_vcr_pace_temp_mode = temp;
    if (!g_vcr_pace_exit_armed) {
        g_vcr_pace_exit_armed = 1;
        atexit(vcr_pace_at_exit);
    }
    vcr_pace_arm_filter();                  /* a no-op after any before-call */
    vcr_pace_unlock_to(0);                  /* after the stamp: the next holder reads it */
}

#define vcr_pace_after_switch()   vcr_pace_after_switch_ex(1)
#define vcr_pace_after_restore()  vcr_pace_after_switch_ex(0)

#ifdef VCR_PACE_WANT_KILL
/* ---- vcrctl pace-kill: a kill made through the gate --------------------------
 * Compiled only where it is used (vcrctl.c defines VCR_PACE_WANT_KILL; the
 * labs do not, and a static function a program never calls is a warning). */

/* how long a paced kill waits for its victim to be gone */
#define VCR_PACE_KILL_WAIT_MS 10000u

/* The images pace-kill will end: the tools that switch modes through this
 * gate, plus gdilab (it switches nothing, but lab_run's cleanup kills only
 * through pace-kill, so a hung gdilab would otherwise stay on the box) - and
 * nothing else: never the agent that ran it, never the system. */
static const char *const vcr_pace_kill_images[] = {
    "vcrctl.exe", "ddlab.exe", "d3dprobe.exe", "glidelab.exe", "gdilab.exe"
};

static int vcr_pace_same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* The refusals that need no process: the System Idle Process (0), System
 * (4) and pace-kill itself. NULL = none of those. */
static const char *vcr_pace_kill_pid_refusal(DWORD pid, DWORD self)
{
    if (pid == 0 || pid == 4)
        return "pid 0 and 4 are the system's";
    if (pid == self)
        return "pace-kill does not kill itself";
    return NULL;
}

/* NULL = pid may be pace-killed; else why not. `image` is the process's image
 * name as Toolhelp gives it (NULL: no such process), compared without its path
 * and case - XP answers "DDLAB.EXE" as readily as "ddlab.exe". */
static const char *vcr_pace_kill_refusal(DWORD pid, DWORD self, const char *image)
{
    const char *base, *why = vcr_pace_kill_pid_refusal(pid, self);
    unsigned i;
    if (why)
        return why;
    if (!image)
        return "no such process";
    base = strrchr(image, '\\');
    base = base ? base + 1 : image;
    for (i = 0; i < sizeof vcr_pace_kill_images / sizeof vcr_pace_kill_images[0]; i++)
        if (vcr_pace_same_name(base, vcr_pace_kill_images[i]))
            return NULL;
    return "not a vcr-kmd tool - only vcrctl, ddlab, d3dprobe and glidelab are killed";
}

/* Kill `proc` as a switch: the victim may hold a temporary mode, and XP
 * reverts it the instant the victim dies. Under the switch lock, a floor after
 * the latest stamp: TerminateProcess, a bounded wait for the victim to be
 * gone, the revert stamped AHEAD (a dying process's mode can be dropped after
 * TerminateProcess returns, and later still while it is not yet gone), the
 * lock given back. 1 = killed (*exited: gone within VCR_PACE_KILL_WAIT_MS);
 * 0 = refused by the gate, nothing killed (g_vcr_pace_why: a victim holding
 * the lock itself - hung inside a switch - is refused too, and the host does
 * NOT fall back to a plain kill: that would revert its mode unpaced, so the
 * survivor is reported for a person); -1 = TerminateProcess failed (*err). */
static int vcr_pace_kill(HANDLE proc, int *exited, DWORD *err)
{
    *exited = 0;
    *err = 0;
    if (!vcr_pace_before_switch())
        return 0;
    if (!TerminateProcess(proc, 1)) {
        *err = GetLastError();
        vcr_pace_cancel();                  /* nothing died: the pre-stamp is all it cost */
        return -1;
    }
    *exited = WaitForSingleObject(proc, VCR_PACE_KILL_WAIT_MS) == WAIT_OBJECT_0;
    vcr_pace_mark_ahead(VCR_PACE_EXIT_LAG_MS);
    vcr_pace_unlock_to(0);
    return 1;
}
#endif /* VCR_PACE_WANT_KILL */

#endif /* VCR_PACE_H */
