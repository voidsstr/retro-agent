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
#include "fxpanel.h"
#include "log.h"
#include "ntdyn.h"
#include "hostpolicy.h"
#include "bgwork.h"
#include "gameindex.h"
#include "../shared/drvprefs.h"
#include "../shared/drvmatch.h"
#include "../shared/drvsafe.h"
#include "../shared/drvplan.h"
#include "../shared/drvstore.h"
#include "../shared/gamegate.h"
#include "../shared/lnkcheck.h"
#include "../shared/gsresume.h"
#include "../shared/grledger.h"
#include "../shared/audiofix.h"
#include "../shared/regmerge.h"
#include "../shared/deskview.h"
#include "../shared/deskset.h"
#include "../shared/gsstall.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <shlobj.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <mmsystem.h>

#include "../shared/gamesdir.h"

#define LOG_GS "GAMESYNC"

/* The library the fleet publishes pre-installed game trees to. Overridable -
 * see gs_library_path() - because an address baked into a binary is a promise
 * we cannot keep across a NAS move. */
#define GS_DEFAULT_LIBRARY "\\\\192.168.1.122\\files\\Files\\Games-Library"
#define GS_DEST_DEFAULT    "C:\\Games"
#define GS_MARKER          "C:\\RETRO_AGENT\\gamesync.done"
/* Written into the image by stage-oem.sh. Its PRESENCE is what says
 * "this machine was just imaged" - see gs_new_image(). */
#define GS_NEWIMAGE_FLAG   "C:\\RETRO_AGENT\\newimage.flag"
#define GS_INI             "C:\\RETRO_AGENT\\gamesync.ini"

/* How many titles gs_run() can hold. The library was 46 on 2026-08-31 and it
 * grows; overflowing this used to be a silent truncation, so it is named, it is
 * logged when hit, and it has headroom. 1.93.0: 96 -> 256 (the Win9x DOS
 * titles for .243 alone add dozens). The per-title arrays live on gs_run's
 * stack (~85 KB at 256) - the worker threads have the default 2 MB. */
#define GS_MAX_TITLES      256

#define GS_CHUNK           (64u * 1024u)
/* Leave the OS room to breathe; filling C: to the last byte breaks XP in
 * confusing ways long before it reports "disk full". 300 MB was sized for
 * the XP boxes; on a period disk it is a quarter of the volume - .243 (Win98,
 * 1.2 GB C:) had 358 MB free and could add NO title bigger than 58 MB, which
 * refused the 199 MB Quake II base game while logging "needs 199 MB, only
 * 358 MB free". A volume under GS_SMALL_DISK keeps GS_FREE_MARGIN_SMALL
 * (150 MB - still more than Win9x's swap file grows to on a 128 MB box). See
 * gs_free_margin(). */
#define GS_FREE_MARGIN       ((__int64)300 * 1024 * 1024)
#define GS_FREE_MARGIN_SMALL ((__int64)150 * 1024 * 1024)
#define GS_SMALL_DISK        ((__int64)4096 * 1024 * 1024)
#define GS_LOG_EVERY_MS    2000
/* Staggered behind retrowall (20 s) and ahead of the game index (120 s):
 * until 1.85.0 all three woke at 20 s and hit the disk and the shell at
 * once, on boxes that were still finishing their own logon. */
#define GS_FIRST_DELAY_MS  40000

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
    /* Titles the capability gate refused. Counted SEPARATELY from
     * skipped_titles, which means "did not fit on the disk": a game left off
     * for want of space and a game left off because the machine cannot run it
     * are different facts and want different follow-up. */
    int     gated_titles;
    int     failed_files;
    /* The FULL PATH of the first file that failed to copy. `file` above is
     * merely whatever the walker was last on, which after a failure keeps
     * advancing - so reading `current_file` to find out what broke points at an
     * unrelated file in an unrelated title. That really happened: a failure in
     * CounterStrike16 was reported as UT2004's ONSNewTank-A.ukx, and the only
     * place the true name appeared was the agent log. Keep the FIRST failure,
     * not the last: the first is the one that started the trouble. */
    char    failed_file[260];
    DWORD   started;
    double  mbps;
    char    message[256];
    /* Is the run MOVING? (agent/shared/gsstall.h) `beat` is the tick of the
     * worker's last progress point; `stall` accumulates the gaps between them,
     * and whether the CPU was saturated across each (then it was starved - the
     * worker runs at THREAD_PRIORITY_IDLE). The cpu_* fields are the last
     * GetSystemTimes sample, so GAMESYNC STATUS can judge the gap still open. */
    DWORD   beat;
    gsst_t  stall;
    int     last_busy;          /* CPU busy % over the last closed stall, -1 */
    int     cpu_ok;
    DWORD   cpu_tick;
    unsigned __int64 cpu_idle, cpu_kernel, cpu_user;
    /* The resolution pass, as the last FINISHED run left it (0 while a run is
     * in progress): values it changed, and files it had adjusted that the
     * copy kept rather than taking the library's again (grledger.h). A
     * settled box reads gr_changed 0 - see the `gameres:` log line. */
    int     gr_changed;
    long    gr_kept;
    /* GAMERES VERIFY at the end of the last finished run: config targets
     * still WRONG (a settled box: 0) and ABSENT from this build.
     * gr_verified 0 = no run has verified yet - reported as -1. */
    int     gr_vwrong, gr_vabsent, gr_verified;
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

/* Escape a string for embedding in the hand-built status JSON below.
 *
 * That JSON is assembled with a raw _snprintf, and every other field it emits
 * is a bare FILENAME (fd.cFileName) or a fixed word, so nothing ever needed
 * escaping. `failed_file` is the first field carrying a FULL PATH - and a
 * Windows path is full of backslashes, which are the JSON escape character.
 * Emitted raw, "C:\Games\..." contains \G and \., neither a valid escape, so
 * the host's json.loads() raises and the whole status response is lost - a
 * strictly worse outcome than the missing field it was added to provide.
 *
 * Quotes are escaped too, and control characters dropped, since a filename may
 * legally contain neither but a corrupted directory entry might. */
static void gs_json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;

    if (!cap) return;
    for (; in && *in && o + 2 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '\\' || c == '"') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
        /* control characters are dropped rather than escaped: they cannot
         * appear in a real path and \uXXXX would need four more bytes */
    }
    out[o] = '\0';
}


/* ---------------------------------------------------------------------- */
/* progress heartbeat: is the run moving - and if not, is it starved?       */
/* ---------------------------------------------------------------------- */

/* WHY. The worker runs at THREAD_PRIORITY_IDLE (bgwork.h) so it never takes
 * the CPU from a game. On .110 (XP, one P4 core, 2026-09-28) a minimized
 * ioquake3 held 96% of the CPU and a run sat at "enumerating library" for
 * ~100 minutes - titles_total 0, elapsed_s climbing, the log creeping forward a
 * line every 5-20 s - and nothing anywhere said why. The decision is in
 * agent/shared/gsstall.h; this is the plumbing: every progress point calls
 * gs_beat(), a gap between two beats is a stall, and GetSystemTimes says
 * whether the CPU was saturated across it (starved) or not (the share or the
 * disk was slow). Reported in GAMESYNC STATUS and logged. */
#define GS_CPU_SAMPLE_MS  250UL       /* resample CPU times this often      */
#define GS_STALL_LOG_MS   600000UL    /* repeat a stall line every 10 min   */

static DWORD g_gs_stall_logged;       /* worker only                        */
static int   g_gs_stall_logged_v;

static unsigned __int64 gs_ft_u64(const FILETIME *ft)
{
    return ((unsigned __int64)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* A whole-machine CPU sample. 0 where Windows has no GetSystemTimes (9x,
 * 2000): then nothing is ever called "starved", only "stalled". */
static int gs_cpu_sample(unsigned __int64 *idle, unsigned __int64 *kern,
                         unsigned __int64 *user)
{
    FILETIME fi, fk, fu;
    if (!ntdyn_GetSystemTimes(&fi, &fk, &fu))
        return 0;
    *idle = gs_ft_u64(&fi);
    *kern = gs_ft_u64(&fk);
    *user = gs_ft_u64(&fu);
    return 1;
}

/* CPU busy % between an earlier sample and now; -1 = unknown. */
static int gs_cpu_busy_since(int have, unsigned __int64 i0, unsigned __int64 k0,
                             unsigned __int64 u0)
{
    unsigned __int64 i1, k1, u1;
    if (!have || !gs_cpu_sample(&i1, &k1, &u1))
        return -1;
    if (i1 < i0 || k1 < k0 || u1 < u0)
        return -1;
    return gsst_busy_pct(i1 - i0, k1 - k0, u1 - u0);
}

static void gs_stall_describe(char *out, size_t cap, int v, unsigned long lost,
                              unsigned long span, int busy)
{
    char cpu[24];

    out[0] = 0;
    if (busy >= 0)
        _snprintf(cpu, sizeof(cpu) - 1, "CPU %d%% busy", busy);
    else
        lstrcpynA(cpu, "CPU saturated", sizeof(cpu));
    cpu[sizeof(cpu) - 1] = 0;
    if (v == GSST_STARVED)
        _snprintf(out, cap - 1,
                  "STARVED OF CPU: no progress for %lu s of the last %lu s, "
                  "%s - GAMESYNC runs at idle priority by design, so a busy "
                  "CPU (a running game is the usual cause) stops it until the "
                  "CPU frees up",
                  lost / 1000, span / 1000, cpu);
    else if (v == GSST_SLOW && busy >= 0)
        _snprintf(out, cap - 1,
                  "STALLED: no progress for %lu s of the last %lu s with the "
                  "CPU only %d%% busy - not CPU starvation; the share or the "
                  "disk is slow",
                  lost / 1000, span / 1000, busy);
    else if (v == GSST_SLOW)
        _snprintf(out, cap - 1,
                  "STALLED: no progress for %lu s of the last %lu s (this "
                  "Windows cannot report CPU load, so a busy CPU and a slow "
                  "share look the same here)",
                  lost / 1000, span / 1000);
    out[cap - 1] = 0;
}

/* A progress point. Called by the WORKER from the walks, the copy loop and
 * gs_set_msg(); a no-op outside a run. The fast path is one GetTickCount(). */
static void gs_beat(void)
{
    DWORD now, gap;
    int   st = g_gs.state;

    if (st != GS_SIZING && st != GS_COPYING)
        return;
    now = GetTickCount();
    gap = now - g_gs.beat;
    if (gap >= GSST_GAP_MS) {
        unsigned long lost = 0, span = 0;
        int  busy, v;
        char line[320];

        EnterCriticalSection(&g_gs_lock);
        busy = gs_cpu_busy_since(g_gs.cpu_ok, g_gs.cpu_idle, g_gs.cpu_kernel,
                                 g_gs.cpu_user);
        gsst_note_gap(&g_gs.stall, now, gap, busy);
        g_gs.last_busy = busy;
        v = gsst_verdict(&g_gs.stall, now, &lost, &span);
        g_gs.cpu_ok = gs_cpu_sample(&g_gs.cpu_idle, &g_gs.cpu_kernel,
                                    &g_gs.cpu_user);
        g_gs.cpu_tick = now;
        g_gs.beat = now;
        LeaveCriticalSection(&g_gs_lock);

        if (v == GSST_OK)
            g_gs_stall_logged_v = GSST_OK;  /* recovered: say it again next time */
        else if (v != g_gs_stall_logged_v ||
                 now - g_gs_stall_logged >= GS_STALL_LOG_MS) {
            g_gs_stall_logged = now;
            g_gs_stall_logged_v = v;
            gs_stall_describe(line, sizeof(line), v, lost, span, busy);
            log_msg(LOG_GS, "%s", line);
        }
        return;
    }
    if (now - g_gs.cpu_tick >= GS_CPU_SAMPLE_MS) {
        EnterCriticalSection(&g_gs_lock);
        gsst_roll(&g_gs.stall, now);
        g_gs.cpu_ok = gs_cpu_sample(&g_gs.cpu_idle, &g_gs.cpu_kernel,
                                    &g_gs.cpu_user);
        g_gs.cpu_tick = now;
        LeaveCriticalSection(&g_gs_lock);
    }
    g_gs.beat = now;
}

static void gs_set_msg(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&g_gs_lock);
    va_start(ap, fmt);
    _vsnprintf(g_gs.message, sizeof(g_gs.message) - 1, fmt, ap);
    va_end(ap);
    g_gs.message[sizeof(g_gs.message) - 1] = 0;
    LeaveCriticalSection(&g_gs_lock);
    gs_beat();
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

/* Total size of the volume, or -1. */
static __int64 gs_total_bytes(const char *root)
{
    typedef BOOL (WINAPI *pGDFSE2)(LPCSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
    static pGDFSE2 fn;
    static int     looked;
    ULARGE_INTEGER avail, total, freeb;
    DWORD spc, bps, freec, totalc;

    if (!looked) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k)
            fn = (pGDFSE2)GetProcAddress(k, "GetDiskFreeSpaceExA");
        looked = 1;
    }
    if (fn && fn(root, &avail, &total, &freeb))
        return (__int64)total.QuadPart;
    if (GetDiskFreeSpaceA(root, &spc, &bps, &freec, &totalc))
        return (__int64)totalc * (__int64)spc * (__int64)bps;
    return -1;
}

/* ---- where titles are copied (1.93.0) --------------------------------------
 * HKLM\Software\RetroAgent\GamesDir (REG_SZ) overrides C:\Games for a box
 * whose C: is too small: .243's C: is 1.2 GB with 333 MB free while its second
 * disk has a 72 GB E:. Only a local "X:\folder" path on a FIXED drive is taken.
 * A value that is configured but unusable - malformed, or its drive missing,
 * which is exactly .243's E: after a power loss until the 1Bh reboot - makes
 * GAMESYNC REFUSE, never fall back: falling back would pour the library onto
 * the small C: the setting exists to protect. Absent = C:\Games, as always. */
static char g_gs_dest[160] = GS_DEST_DEFAULT;
static char g_gs_dest_root[4] = "C:\\";

/* 1 = usable (out = the folder, root = "X:\"), 0 = not configured (out =
 * C:\Games), -1 = configured but unusable (why says why). */
static int gs_dest_resolve(char *out, size_t cch, char *root, char *why, size_t why_cch)
{
    HKEY  h;
    char  v[200], norm[200];
    DWORD type = 0, sz = sizeof(v) - 1;
    int   have = 0;

    lstrcpynA(out, GS_DEST_DEFAULT, (int)cch);
    lstrcpynA(root, "C:\\", 4);
    if (why_cch) why[0] = 0;
    memset(v, 0, sizeof(v));
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        have = RegQueryValueExA(h, "GamesDir", NULL, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS && type == REG_SZ;
        RegCloseKey(h);
    }
    if (!have) return 0;
    v[sizeof(v) - 1] = 0;
    if (!gamesdir_normalise(v, norm, sizeof(norm), root)) {
        _snprintf(why, why_cch - 1, "GamesDir \"%.120s\" is not a local X:\\folder path", v);
        why[why_cch - 1] = 0;
        return -1;
    }
    if (GetDriveTypeA(root) != DRIVE_FIXED) {
        _snprintf(why, why_cch - 1, "GamesDir %s: drive %s is not a fixed disk right now (missing?)", norm, root);
        why[why_cch - 1] = 0;
        return -1;
    }
    lstrcpynA(out, norm, (int)cch);
    return 1;
}

/* For gameres.c and anyone else: the folder titles are installed in, or 0 when
 * a configured GamesDir is unusable (then nothing is "installed"). */
int gs_games_dir(char *out, size_t cch)
{
    char root[4], why[200];
    return gs_dest_resolve(out, cch, root, why, sizeof(why)) >= 0;
}

/* How much of C: GAMESYNC always leaves free: the smaller margin only on a
 * volume under 4 GB, the XP-era 300 MB everywhere else. An unmeasurable
 * volume keeps the larger margin - the safe direction. */
static __int64 gs_margin_for_disk(__int64 total)
{
    return (total > 0 && total < GS_SMALL_DISK) ? GS_FREE_MARGIN_SMALL : GS_FREE_MARGIN;
}

static __int64 gs_free_margin(void)
{
    return gs_margin_for_disk(gs_total_bytes("C:\\"));
}

/* The same margin, for the volume the titles go to. */
static __int64 gs_free_margin_for(const char *root)
{
    return gs_margin_for_disk(gs_total_bytes(root));
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
        gs_beat();
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

/* added        - bytes now accounted for, whether transferred or skipped
 * transferred  - of those, how many actually crossed the wire
 *
 * The two differ because resume counts a file that is already present at the
 * right size as complete without reading it. Feeding those bytes into the rate
 * window produced readings like 760 MB/s over SMB1 - obvious nonsense on its
 * own, but the real cost is that a burst of skips inflates the average and can
 * hide a genuinely stalled transfer for the next few samples. Percentage counts
 * both; throughput counts only what moved. */
static void gs_note_progress2(__int64 added, __int64 transferred)
{
    DWORD now = GetTickCount();
    DWORD dt;
    __int64 total, done;
    int pct;
    char title[128], file[260];
    double mbps;

    EnterCriticalSection(&g_gs_lock);
    g_gs.done_bytes += added;
    /* A re-listed directory (gs_copy_tree) walks files it already counted. */
    if (g_gs.total_bytes > 0 && g_gs.done_bytes > g_gs.total_bytes)
        g_gs.done_bytes = g_gs.total_bytes;
    total = g_gs.total_bytes;
    done  = g_gs.done_bytes;
    lstrcpynA(title, g_gs.title, sizeof(title));
    lstrcpynA(file,  g_gs.file,  sizeof(file));
    LeaveCriticalSection(&g_gs_lock);
    gs_beat();

    g_win_bytes += transferred;
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

static void gs_note_progress(__int64 added)
{
    gs_note_progress2(added, added);
}

/* ---------------------------------------------------------------------- */
/* did this run actually CHANGE the desktop?                                */
/* ---------------------------------------------------------------------- */

/* WHY THIS EXISTS. gs_run() used to re-lay-out the whole desktop at the end of
 * EVERY sync, unconditionally. GAMESYNC runs at startup, and the overwhelmingly
 * common case is a box that is already fully provisioned: every title skipped,
 * not one file copied, not one shortcut created. So every boot rebuilt the icon
 * layout for no reason, on every machine - which the user noticed as "the retro
 * agent is rebuilding icons all the time".
 *
 * The fix is not to arrange less eagerly in general - a freshly deployed game
 * whose icon never gets placed would be a worse bug, and it would show up
 * exactly during the staged-game fix loop that the whole fleet is doing. It is
 * to arrange when the desktop ACTUALLY CHANGED:
 *
 *   - a file was really written (not skipped by the size+mtime resume test), or
 *   - a .lnk was created that was not there before, or one was swept away.
 *
 * Note what is deliberately NOT counted: gs_make_game_shortcut() rewrites a
 * title's .lnk on every pass, so counting shortcut WRITES would be true on
 * every run and would measure nothing. Only a link that did not previously
 * exist changes the set of icons on the desktop. */
static long g_gs_desk_files;   /* files actually written this run          */
static long g_gs_desk_lnks;    /* net change in the set of desktop icons   */

/* THE SET OF ICONS, SAMPLED BEFORE ANY OF THIS RUN TOUCHES THE DESKTOP.
 *
 * Counting "a .lnk was created that did not exist a moment ago" is WRONG here,
 * and it silently defeated the whole gate in v1.73.0/1.74.x: gs_run() then began
 * by sweeping EVERY .lnk off the desktop, so by the time each title's shortcut
 * was written nothing was ever "already there" and every shortcut counted as
 * new. The honest question is "did the SET of desktop icons change?", so the set
 * is sampled before the run writes anything and compared at the end. A box that
 * rewrites the same 81 shortcuts has not changed.
 *
 * SINCE 1.90.0 THE SAME SAMPLE DECIDES WHAT THE SWEEP MAY REMOVE, AND THE SWEEP
 * RUNS LAST (agent/shared/deskset.h). Sweeping first emptied the desktop for as
 * long as the run took to reach each title again - ~100 minutes on .110 while a
 * game starved the idle-priority worker, and FOREVER on every run that failed,
 * was aborted or was killed before then (library unreachable: re-swept every two
 * minutes). Now shortcuts are rewritten in place and only what was on the
 * desktop at the start and was NOT put back by this run is moved away, once the
 * run has considered every title. The final desktop is unchanged; the empty
 * window is gone. */
static ds_set_t g_gs_dset;
static char     g_gs_desk_common[MAX_PATH];   /* where the sample was taken */
static char     g_gs_desk_user[MAX_PATH];

static void gs_desk_reset(void)
{
    g_gs_desk_files = 0;
    g_gs_desk_lnks = 0;
    ds_reset(&g_gs_dset);
    g_gs_desk_common[0] = 0;
    g_gs_desk_user[0] = 0;
}
static void gs_desk_note_file(void) { g_gs_desk_files++; }

/* Defined below, next to the desktop enumeration they need; declared here
 * because gs_place_tool_shortcuts() writes a .lnk earlier in the file. */
static const char *gs_desk_written_path(const char *lnk_path, char *buf, size_t cap);
static void gs_desk_note_lnk_written(const char *lnk_path);
static void gs_desk_note_lnk_kept(const char *lnk_path);
static void gs_desk_snapshot(void);
static void gs_desk_settle_lnks(void);
static int  gs_desk_changed(void)
{
    return (g_gs_desk_files > 0 || g_gs_desk_lnks > 0);
}
static long gs_desk_files(void) { return g_gs_desk_files; }
static long gs_desk_lnks(void)  { return g_gs_desk_lnks; }

/* WHY THESE ARE REPORTED IN THE `done:` LINE, NOT JUST USED INTERNALLY.
 *
 * gs_desk_changed() decides whether to rebuild the desktop icon layout. If it
 * is wrong in the "always true" direction the icon rebuild comes straight back
 * and NOTHING SAYS SO - the gate would be defeated permanently and silently,
 * which is this project's signature failure mode.
 *
 * The realistic way that happens is ONE file that re-copies on every single
 * pass: a destination whose mtime never stamps (SetFileTime failing on a
 * read-only or oddly-attributed file, a filesystem with coarser time
 * granularity than the source) fails the size+mtime resume test forever, so
 * every sync writes at least one file and the gate is always true.
 *
 * So the counts go in the log line an operator already reads. A steady-state
 * box must report `0 file(s) written, 0 new shortcut(s)`; a box that reports
 * the SAME small non-zero count on consecutive no-change syncs is announcing
 * exactly that defect instead of hiding it. That turns an untestable claim
 * into a one-line observation. */

/* gs_same_mtime - do these two files carry the same last-write time?
 *
 * WHY THIS EXISTS AT ALL: gs_copy_file used to treat "destination is the same
 * SIZE" as "destination is already correct". That premise was written down as
 * "these are immutable release trees" - and it stopped being true the moment we
 * started PATCHING staged games. A recompiled DLL very often lands on exactly
 * the same file-alignment boundary as the one it replaces, so a patch is the
 * normal case for same-size-different-content, not an edge case.
 *
 * It cost us a real outage: applying the official Deus Ex 1.112fm patch changed
 * 38 files, and SEVENTEEN of them kept their exact byte size - including
 * Core.dll (790,528) and DeusEx.exe (253,952). Every box that already had the
 * game therefore took the new Core.u (its size changed) and KEPT the retail
 * Core.dll (its size did not), producing a mixed-version Unreal install that
 * died at startup with
 *     Can't find 'intUObjectexecGetConfig' in 'Core.dll'
 * while GAMESYNC reported state=done, 0 failed - truthfully, because it had
 * decided those files were already current. Shogo lost 3 files the same way and
 * Descent 3 lost 1.
 *
 * TOLERANCE IS 2 SECONDS ON PURPOSE. FAT32 stores write times with 2-second
 * granularity, so a time set from an NTFS source is rounded on a FAT volume and
 * an exact comparison would never match - which would make every sync re-copy
 * the entire library on the Win9x boxes. This is the same "modify window" rsync
 * uses, and for the same reason.
 *
 * The alternative - hashing the file - was rejected when this code was written
 * and that judgement still holds: 6 GB over SMB1 on a Pentium III costs far
 * more than it could ever save.
 */
/* GS_MTIME_SLACK_100NS and the comparison itself live in
 * agent/shared/gsresume.h, with the decision gs_copy_file() makes from them,
 * so the regression tests compile the code the agent runs. */

static int gs_get_mtime(const char *path, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &ad))
        return 0;
    *ft = ad.ftLastWriteTime;
    return 1;
}

static long long gs_ft64(const FILETIME *f)
{
    return gsr_ft64(f->dwHighDateTime, f->dwLowDateTime);
}

/* How many times one file's copy may reopen its source after a failed read.
 * See the read loop in gs_copy_file(). */
#define GS_READ_RETRIES   6
/* Every time a copy has to reconnect to the share (a failed read or open of a
 * source file). A directory listing that was open across one of these cannot
 * be trusted - see gs_copy_tree. */
static volatile LONG g_gs_net_resets = 0;
/* Files GAMERES had adjusted that this run KEPT instead of copying the
 * library's copy back over them (agent/shared/grledger.h). Reset per run. */
static volatile LONG g_gs_gr_kept = 0;

/* src_list_ft: the source's last-write time as the directory listing
 * reported it, or NULL when the caller has none (then the source is asked). */
static int gs_copy_file(const char *src, const char *dst, __int64 src_size,
                        const FILETIME *src_list_ft)
{
    HANDLE hs, hd;
    char  *buf;
    DWORD  rd, wr;
    int    ok = 1;
    int    retries = 0;
    __int64 copied = 0, last_fail_at = 0;
    WIN32_FILE_ATTRIBUTE_DATA dad;
    int    dst_exists, verdict;
    long long dst_size = -1, dst_time = 0;
    /* the source's own time, asked over the network AT MOST once per file */
    int    asked_src = 0, have_src = 0;
    long long src_time = 0;
    FILETIME src_ft;
    /* nonzero if GAMERES recorded this destination: why it is not kept */
    int    gr_entry = 0;

    /* Resume: a destination that matches in BOTH size and last-write time is
     * treated as done. Size alone is NOT enough - see gs_same_mtime() above for
     * the Deus Ex 1.112fm outage that proved it. The mtime is preserved by the
     * copy below, so a file this agent wrote will match on the next pass and
     * resume still costs nothing.
     *
     * The first sync after this change re-copies anything whose mtime was never
     * set (i.e. everything an older agent copied). That is a one-off cost and it
     * is also the remedy: it repairs every box already carrying a half-applied
     * patch. */
    /* ONE read of the destination (size and time together), and the source's
     * time from the listing we are already walking. Only when that listing
     * time disagrees is the source asked over the network - see gsresume.h
     * for why this keeps the v1.62.0 test's exact answers. */
    dst_exists = GetFileAttributesExA(dst, GetFileExInfoStandard, &dad) != 0;
    if (dst_exists) {
        dst_size = ((long long)dad.nFileSizeHigh << 32) | dad.nFileSizeLow;
        dst_time = gs_ft64(&dad.ftLastWriteTime);
    }
    verdict = gsr_decide(dst_exists, dst_size, dst_time, (long long)src_size,
                         src_list_ft != NULL,
                         src_list_ft ? gs_ft64(src_list_ft) : 0);
    /*
     * NOT THE LIBRARY'S FILE - BUT PERHAPS DELIBERATELY SO. GAMERES rewrites a
     * title's config for this box's monitor after every copy, which leaves it
     * different from the library's; copying the library's back at the next
     * sync and letting GAMERES change it again was a fight neither side could
     * win: on .110 (1.90.0) every sync wrote 11-22 files, GAMERES changed 23
     * values and the icons were rebuilt, forever. So when GAMERES recorded
     * this destination (agent/shared/grledger.h), and the box's file is still
     * exactly what it left and the library's is still the one it adjusted,
     * the file is kept. Either side changing - the library shipped a new
     * version, or the file was edited here - and it is copied as before, and
     * GAMERES adjusts the new copy.
     *
     * The ledger can only turn a copy into a skip, never the reverse: it is
     * asked only after the resume test declined to skip, and "no record" or
     * "no opinion" leaves the resume test's verdict exactly as it was.
     */
    if (verdict != GSR_SKIP && dst_exists) {
        grl_entry_t le;
        memset(&le, 0, sizeof(le));
        if (gameres_ledger_lookup(dst, &le.base_size, &le.base_time,
                                  &le.out_size, &le.out_time)) {
            int lv = grl_decide(&le, dst_size, dst_time, (long long)src_size,
                                src_list_ft != NULL,
                                src_list_ft ? gs_ft64(src_list_ft) : 0);
            gr_entry = 1;
            if (lv == GRL_ASK_SOURCE) {
                asked_src = 1;
                have_src = gs_get_mtime(src, &src_ft);
                src_time = have_src ? gs_ft64(&src_ft) : 0;
                lv = grl_decide_source(&le, have_src, src_time);
            }
            if (lv == GRL_SKIP) {
                InterlockedIncrement((LONG *)&g_gs_gr_kept);
                gs_note_progress2(src_size, 0);
                return 1;
            }
            gr_entry = lv;              /* why it is NOT kept - said below */
        }
    }
    if (verdict == GSR_ASK_SOURCE) {
        if (!asked_src) {
            asked_src = 1;
            have_src = gs_get_mtime(src, &src_ft);
            src_time = have_src ? gs_ft64(&src_ft) : 0;
        }
        verdict = gsr_decide_source(have_src, src_time, dst_time);
    }
    if (verdict == GSR_SKIP) {
        gs_note_progress2(src_size, 0);   /* counted, but nothing crossed the wire */
        return 1;
    }
    /* A file GAMERES had adjusted is about to be replaced: say why, once -
     * this is the rare case (a library update, an edit on the box), and it is
     * the one that explains a non-zero `file(s) written` on a quiet box. */
    if (gr_entry)
        log_msg(LOG_GS, "%s: %s since GAMERES adjusted it - taking the "
                "library's copy again", dst,
                gr_entry == GRL_SRC_CHANGED ? "the library's copy changed"
                                            : "the file was changed on this box");

    hs = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "open failed (%lu): %s", GetLastError(), src);
        return 0;
    }
    /* Clear HIDDEN/READONLY/SYSTEM before opening the destination.
     *
     * CREATE_ALWAYS fails with ERROR_ACCESS_DENIED (5) when the file already
     * exists and carries FILE_ATTRIBUTE_HIDDEN or _READONLY and the call does
     * not pass the same attribute back. Several staged trees legitimately ship
     * hidden files (CounterStrike16 alone has BCShield.asi, BCShield.dll,
     * rev.ini, cstrike\liblist.gam and restart_debug.bat), so this was not an
     * edge case - it made `failed_files == 0` UNSATISFIABLE on every box in the
     * fleet, and because gs_write_marker() is skipped when failed_files != 0,
     * gamesync.done went stale everywhere too.
     *
     * It hid for so long because the early-out above returns success for any
     * file already at the right size: only a hidden file whose size DIFFERS
     * from the library's copy ever reaches this call. So it surfaced exactly
     * once a staged hidden file was edited - and then it was permanent, because
     * the box could never accept the new version.
     *
     * SetFileAttributesA is cheap, and failing is fine: if the file does not
     * exist there is nothing to clear, and CreateFileA reports the real error.
     * The destination is ours to own - the library's copy defines the tree. */
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);

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
            DWORD err = GetLastError();
            /* A failed read is not the end of the file. Win98's network client
             * drops long SMB reads from the NAS - error 55, "the specified
             * network resource is no longer available" - and on .243
             * (2026-09-24) it did so on every run, 7-40 s into Hexen II's 22 MB
             * and 77 MB paks. Giving up there abandoned the whole file, so the
             * box never got its marker and re-walked the entire library every
             * 120 s. Reopening the source reconnects the session; resume from
             * the byte we reached. Bounded, logged, and the abort flag still
             * wins. */
            /* The budget refills once 8 MB has crossed since the last failure,
             * so a long file on a flaky link can resume as often as it keeps
             * making progress, while a source that fails at the same byte
             * every time still gives up after GS_READ_RETRIES tries. */
            InterlockedIncrement((LONG *)&g_gs_net_resets);
            if (copied - last_fail_at >= 8 * 1024 * 1024)
                retries = 0;
            last_fail_at = copied;
            if (retries < GS_READ_RETRIES) {
                LONG  hi = (LONG)(copied >> 32);
                DWORD lo;
                retries++;
                log_msg(LOG_GS, "read failed (%lu) at %I64d of %I64d bytes - "
                        "reopening and resuming (retry %d/%d): %s",
                        err, copied, src_size, retries, GS_READ_RETRIES, src);
                CloseHandle(hs);
                Sleep(3000);
                hs = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hs != INVALID_HANDLE_VALUE) {
                    lo = SetFilePointer(hs, (LONG)(copied & 0xFFFFFFFF), &hi, FILE_BEGIN);
                    if (lo != 0xFFFFFFFFUL || GetLastError() == NO_ERROR)
                        continue;
                    log_msg(LOG_GS, "resume: could not seek to %I64d (%lu): %s",
                            copied, GetLastError(), src);
                } else {
                    log_msg(LOG_GS, "resume: could not reopen (%lu): %s",
                            GetLastError(), src);
                }
            } else {
                log_msg(LOG_GS, "read failed (%lu) after %d resumes - giving up: %s",
                        err, GS_READ_RETRIES, src);
            }
            if (hs == INVALID_HANDLE_VALUE)
                hs = NULL;              /* nothing left to close below */
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
        copied += rd;
        gs_note_progress((__int64)rd);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    /* Stamp the destination with the SOURCE's last-write time, while both
     * handles are still open. Without this the skip test above could never
     * match anything we wrote, and every sync would re-copy the whole library
     * forever. Failure is not fatal - it only costs one redundant copy next
     * time - so the result is deliberately not checked. */
    if (ok) {
        FILETIME ft;
        if (GetFileTime(hs, NULL, NULL, &ft))
            SetFileTime(hd, NULL, NULL, &ft);
    }
    if (hs)
        CloseHandle(hs);
    CloseHandle(hd);
    if (!ok)
        DeleteFileA(dst);      /* never leave a truncated file looking complete */
    else
        gs_desk_note_file();   /* a REAL write - the resume early-out returned
                                * long before here, so this run changed the box
                                * and the desktop is worth re-arranging */
    /* The box now holds the library's file (or none): GAMERES's record of the
     * old one describes nothing. If GAMERES adjusts the new copy it records
     * it afresh. */
    if (gr_entry)
        gameres_ledger_forget(dst);
    return ok;
}

/*
 * A DIRECTORY LISTING THAT ENDS EARLY IS A FAILURE, NOT THE END OF THE LIST.
 *
 * `while (FindNextFileA(...))` treats every FALSE as "no more files". On Win98
 * the network redirector drops the SMB session under load (error 55) - the
 * file copy survives that since 1.84.2 by reopening and seeking - but the
 * reconnect also invalidates every OPEN SEARCH HANDLE, so each directory's
 * listing simply stopped. Measured on .243 (2026-09-25): Quake2Win9x reported
 * "done: 4/52 title(s) copied, 0 file error(s)" with quake2.exe, every pak and
 * both launchers missing - a half-installed game certified as complete.
 *
 * So the listing must end in ERROR_NO_MORE_FILES. Anything else re-lists the
 * directory (up to GS_LIST_PASSES times): copying is resumable, so files that
 * already arrived are skipped by the size+mtime test and cost a local compare.
 * If it still cannot be listed to the end, that is recorded as a failure -
 * failed_files, the directory named - so gamesync.done is not written and the
 * next pass tries again.
 */
#define GS_LIST_PASSES 5

static int gs_copy_tree(const char *src, const char *dst)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    int    ok = 1, ok_file, pass;
    DWORD  err = ERROR_NO_MORE_FILES;
    __int64 sz;

    gs_mkdir_p(dst);
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", src);
    pat[sizeof(pat) - 1] = 0;
    for (pass = 1; pass <= GS_LIST_PASSES; pass++) {
        LONG resets_at_start = g_gs_net_resets;
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            if (err == ERROR_NO_MORE_FILES || err == ERROR_FILE_NOT_FOUND) {
                err = ERROR_NO_MORE_FILES;      /* an empty directory */
                break;
            }
        } else {
            do {
                if (fd.cFileName[0] == '.' &&
                    (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
                    continue;
                if (g_gs_abort)
                    break;
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
                    {
                        /* the listing already carries the source's time: pass it */
                        FILETIME list_ft = fd.ftLastWriteTime;
                        ok_file = gs_copy_file(s, d, sz, &list_ft);
                    }
                    if (!ok_file) {
                        EnterCriticalSection(&g_gs_lock);
                        if (g_gs.failed_files == 0)
                            lstrcpynA(g_gs.failed_file, d, sizeof(g_gs.failed_file));
                        g_gs.failed_files++;
                        LeaveCriticalSection(&g_gs_lock);
                        ok = 0;
                    }
                }
            } while (FindNextFileA(h, &fd));
            /* read BEFORE anything else can overwrite it - FindClose included */
            err = g_gs_abort ? ERROR_NO_MORE_FILES : GetLastError();
            FindClose(h);
        }
        if (g_gs_abort)
            return 0;
        /* WIN98 SAYS "NO MORE FILES" WHEN IT MEANS "I LOST THE SESSION". A
         * search handle that was open while a copy had to reconnect ends with
         * ERROR_NO_MORE_FILES, exactly like a finished listing - measured on
         * .243 after the error check above was already in place. So a listing
         * that spanned a reconnect is re-done regardless of how it ended. */
        if (err == ERROR_NO_MORE_FILES && g_gs_net_resets == resets_at_start)
            break;
        if (err == ERROR_NO_MORE_FILES) {
            err = ERROR_NETNAME_DELETED;        /* 64: what really happened */
            log_msg(LOG_GS, "the share connection was reset while %s was being listed "
                    "- listing it again (%d/%d)", src, pass, GS_LIST_PASSES);
            continue;
        }
        log_msg(LOG_GS, "listing of %s cut short (error %lu) - listing it again (%d/%d)",
                src, (unsigned long)err, pass, GS_LIST_PASSES);
        Sleep(2000);
    }
    if (err != ERROR_NO_MORE_FILES) {
        char what[MAX_PATH];
        _snprintf(what, sizeof(what) - 1, "%s (listing cut short, error %lu)",
                  src, (unsigned long)err);
        what[sizeof(what) - 1] = 0;
        log_msg(LOG_GS, "could not list %s to the end after %d tries - recorded as a failure",
                src, GS_LIST_PASSES);
        EnterCriticalSection(&g_gs_lock);
        if (g_gs.failed_files == 0)
            lstrcpynA(g_gs.failed_file, what, sizeof(g_gs.failed_file));
        g_gs.failed_files++;
        LeaveCriticalSection(&g_gs_lock);
        ok = 0;
    }
    return ok;
}

/* Restore a saved activation for THIS machine, if one exists.
 *
 * Calls the same code path as the WPALOAD command against the fleet's wpa
 * directory on the share. Everything about why this is safe - hardware binding,
 * no generation, first activation still manual - is in licstatus.c beside the
 * command itself. */
static void gs_restore_activation(void)
{
    char dir[MAX_PATH], msg[512];
    _snprintf(dir, sizeof(dir) - 1,
              "\\\\192.168.1.122\\files\\Files\\Utility\\Retro Automation\\wpa");
    dir[sizeof(dir) - 1] = 0;
    /* Both outcomes are logged, because "nothing was saved for this box" is
     * information the operator needs (it means a one-time activation is still
     * owed) rather than a silence. */
    if (wpa_restore_from(dir, msg, sizeof(msg)))
        log_msg(LOG_GS, "activation restored: %s", msg);
    else
        log_msg(LOG_GS, "activation not restored: %s", msg);
}

/* ---------------------------------------------------------------------- */
/* reclaim the driver payload                                              */
/* ---------------------------------------------------------------------- */

/* The image ships ~2.4 GB of PnP drivers to C:\D so GUI setup can find a driver
 * for whatever hardware it lands on. Once setup has finished, that directory is
 * dead weight - and on a period disk it is ruinous: the Gateway 550 has a SIX
 * gigabyte disk, of which C:\D was taking 2.43 GB across 17,886 files, leaving
 * 308 MB free and room for three games out of twenty-five.
 *
 * So a freshly imaged machine deletes it before provisioning, not after: the
 * space has to be free BEFORE the game copy decides what fits, or the reclaim
 * buys nothing on the machine that needs it most.
 *
 * Safe by construction: the agent runs at first logon, which is after GUI setup
 * has installed every device it is going to. Nothing references C:\D at run
 * time - DevicePath points there, but only PnP reads it, and only when new
 * hardware appears. The full set stays on the share, so adding hardware later
 * means re-staging from there rather than losing anything.
 *
 * Skipped entirely when the newimage flag is absent, so an established machine
 * someone has customised is never touched. */
#define GS_DRIVER_DIR      "C:\\D"

static __int64 gs_dir_bytes(const char *dir)
{
    int files = 0;
    return gs_dir_size(dir, &files);
}

static int gs_rmtree(const char *dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], sub[MAX_PATH];
    int    ok = 1;

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return RemoveDirectoryA(dir) ? 1 : 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;
        _snprintf(sub, sizeof(sub) - 1, "%s\\%s", dir, fd.cFileName);
        sub[sizeof(sub) - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!gs_rmtree(sub))
                ok = 0;
        } else {
            /* Setup marks some staged files read-only; clear it or the delete
             * silently leaves them and the directory never goes away. */
            SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileA(sub))
                ok = 0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!RemoveDirectoryA(dir))
        ok = 0;
    return ok;
}

/* ---------------------------------------------------------------------- */
/* the staged driver tree (C:\D): which INF serves an unconfigured device  */
/* ---------------------------------------------------------------------- */

/*
 * Two callers need the same answer. gs_install_missing_drivers() installs from
 * C:\D for every device Windows left with a problem code, and the reclaim guard
 * (gs_devices_unconfigured) keeps C:\D only while it could still help one.
 *
 * Until 1.85.1 both asked strstr() about the device's FIRST hardware id - the
 * "&REV_xx" one no INF names - so the installer never installed anything and the
 * guard deleted C:\D from under the very devices it served. Now:
 *   - agent/shared/drvmatch.h ranks candidate INFs (model lines only, most
 *     specific id first, family ids refused);
 *   - a candidate whose payload was never staged is refused before anything
 *     else looks at it (drvmatch_payload_ok - every ATI display INF in the
 *     image, for one);
 *   - WINDOWS ITSELF then confirms the INF has a driver node for this device
 *     (gs_inf_serves) before anything is forced;
 *   - installs run in SetupAPI's non-interactive mode, under a watchdog, so a
 *     "Files Needed" prompt fails the install instead of blocking the agent.
 *
 * One walk of the tree serves every problem device at once: it is ~3,700 INFs
 * and ~60 MB, which a Pentium III reads in seconds, not per device.
 */

typedef struct {
    SP_DEVINFO_DATA dev;
    DWORD           problem;
    char            desc[128];
    char            hw[1024];
    char            compat[1024];
    const char     *ids[DRVMATCH_MAX_IDS];
    int             nids;
    drvmatch_cands  cand;
    int             excl;           /* DRVSAFE_* reason: 3dfx, never automatic */
} gs_probdev;

#define GS_MAX_PROBDEV   32
#define GS_INF_READ_MAX  (2 * 1024 * 1024)
#define GS_INSTALL_WAIT  (10 * 60 * 1000)   /* one forced install, worst case */

/* What the install pass concluded about each device (DRVMATCH_V_*), so the
 * reclaim guard straight after it neither walks the tree again nor keeps C:\D
 * for a device the install pass already failed to fix. */
static struct { char id[200]; int verdict; } g_gs_dv[GS_MAX_PROBDEV];
static int g_gs_ndv;
/* A forced install that never returned: a UI we could not suppress is up on the
 * console. Nothing may be reclaimed or forced again this boot. */
static int g_gs_install_hung;

/* ONE driver install at a time. The startup passes, DRIVERS UPDATE and
 * DRVUPDATE all end in UpdateDriverForPlugAndPlayDevices, and the passes flip
 * the driver-signing policy and SetupAPI's non-interactive mode around it - two
 * at once would restore each other's saved state. Whoever finds it held says so
 * and does nothing. */
static volatile LONG g_gs_drv_busy;
static int  gs_drv_busy_enter(void) { return InterlockedExchange(&g_gs_drv_busy, 1) == 0; }
static void gs_drv_busy_leave(void) { InterlockedExchange(&g_gs_drv_busy, 0); }
/* The startup thread waits (up to 15 min: a forced install's watchdog is 10). */
static int gs_drv_busy_wait(void)
{
    int i;
    for (i = 0; i < 900; i++) {
        if (gs_drv_busy_enter())
            return 1;
        Sleep(1000);
    }
    return 0;
}

static int gs_dv_lookup(const char *id)
{
    int i;
    if (!id[0])
        return DRVMATCH_V_UNSEEN;
    for (i = 0; i < g_gs_ndv; i++)
        if (strcmp(g_gs_dv[i].id, id) == 0)
            return g_gs_dv[i].verdict;
    return DRVMATCH_V_UNSEEN;
}

static void gs_dv_record(const char *id, int verdict)
{
    if (g_gs_ndv >= GS_MAX_PROBDEV || !id[0])
        return;
    lstrcpynA(g_gs_dv[g_gs_ndv].id, id, sizeof(g_gs_dv[0].id));
    g_gs_dv[g_gs_ndv].verdict = verdict;
    g_gs_ndv++;
}

/* Defined with PREFER.TXT below: does a driver preference claim this device? */
static int gs_prefer_claims(const gs_probdev *pd);

/* How many boots has a forced install from C:\D been attempted for this device?
 * The installer runs on every startup while newimage.flag exists, so a driver
 * that installs and still leaves the device broken would otherwise be forced
 * again - and hold C:\D - forever. Two boots, then leave it. */
#define GS_DRVFIX_KEY "Software\\RetroAgent\\DriverFixes"

static DWORD gs_drvfix_attempts(const char *id, int bump)
{
    HKEY  k;
    DWORD n = 0, type = 0, sz = sizeof(n);

    if (!id[0])
        return 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, GS_DRVFIX_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(k, id, NULL, &type, (LPBYTE)&n, &sz) != ERROR_SUCCESS ||
        type != REG_DWORD)
        n = 0;
    if (bump) {
        n++;
        RegSetValueExA(k, id, 0, REG_DWORD, (const BYTE *)&n, sizeof(n));
    }
    RegCloseKey(k);
    return n;
}

/* The device's candidate ids, most specific first (see drvmatch.h). The strings
 * live in pd->hw / pd->compat. Both properties are REG_MULTI_SZ; the buffers are
 * zeroed and read two bytes short so each list is always double-NUL terminated,
 * and EVERY string is upper-cased - CharUpperA() on the buffer alone stops at
 * the first NUL, which is exactly how the old code came to see only one id. */
static void gs_device_ids(HDEVINFO set, gs_probdev *pd)
{
    char *p;

    memset(pd->hw, 0, sizeof(pd->hw));
    memset(pd->compat, 0, sizeof(pd->compat));
    if (!SetupDiGetDeviceRegistryPropertyA(set, &pd->dev, SPDRP_HARDWAREID, NULL,
                                           (PBYTE)pd->hw, sizeof(pd->hw) - 2, NULL))
        pd->hw[0] = pd->hw[1] = 0;
    if (!SetupDiGetDeviceRegistryPropertyA(set, &pd->dev, SPDRP_COMPATIBLEIDS, NULL,
                                           (PBYTE)pd->compat, sizeof(pd->compat) - 2,
                                           NULL))
        pd->compat[0] = pd->compat[1] = 0;
    for (p = pd->hw; *p; p += strlen(p) + 1)
        CharUpperA(p);
    for (p = pd->compat; *p; p += strlen(p) + 1)
        CharUpperA(p);
    pd->nids = drvmatch_collect(pd->hw, pd->compat, pd->ids, DRVMATCH_MAX_IDS);
}

/* ---- the 3dfx rule (1.87.0, agent/shared/drvsafe.h) --------------------- */

static int gs_read_inf(const char *path, char *buf);

/* Is any devnode below `dn` (to a few levels) a 3dfx device? */
static int gs_subtree_3dfx(DWORD dn, int depth)
{
    DWORD child, next;
    char  id[512];

    if (depth > 4 || ntdyn_CM_Get_Child(&child, dn, 0) != CR_SUCCESS)
        return 0;
    for (;;) {
        if (ntdyn_CM_Get_Device_IDA(child, id, sizeof(id), 0) == CR_SUCCESS && drvsafe_id_is_3dfx(id))
            return 1;
        if (gs_subtree_3dfx(child, depth + 1))
            return 1;
        if (ntdyn_CM_Get_Sibling(&next, child, 0) != CR_SUCCESS)
            return 0;
        child = next;
    }
}

/* Read one string from the device's class ("driver") key. */
static void gs_class_value(const char *drvkey, const char *name, char *out, DWORD cch)
{
    char  path[300];
    HKEY  k;
    DWORD type = 0, sz = cch;

    out[0] = 0;
    if (!drvkey[0])
        return;
    _snprintf(path, sizeof(path) - 1, "SYSTEM\\CurrentControlSet\\Control\\Class\\%s", drvkey);
    path[sizeof(path) - 1] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS || type != REG_SZ)
        out[0] = 0;
    out[cch - 1] = 0;
    RegCloseKey(k);
}

/* Why this device must never be touched automatically (a DRVSAFE_* reason), or
 * DRVSAFE_OK. hw/compat are the device's id lists (REG_MULTI_SZ). */
static int gs_device_3dfx(HDEVINFO set, SP_DEVINFO_DATA *dev, const char *hw, const char *compat)
{
    char  drvkey[200], service[128], mfg[128], prov[128], desc[160], infp[64], id[512];
    DWORD dn, parent;
    int   depth;

    if (drvsafe_ids_3dfx(hw, compat))
        return DRVSAFE_3DFX_ID;

    drvkey[0] = service[0] = mfg[0] = 0;
    SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_DRIVER, NULL, (PBYTE)drvkey, sizeof(drvkey) - 1, NULL);
    SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_SERVICE, NULL, (PBYTE)service, sizeof(service) - 1, NULL);
    SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_MFG, NULL, (PBYTE)mfg, sizeof(mfg) - 1, NULL);
    drvkey[sizeof(drvkey) - 1] = service[sizeof(service) - 1] = mfg[sizeof(mfg) - 1] = 0;
    gs_class_value(drvkey, "ProviderName", prov, sizeof(prov));
    gs_class_value(drvkey, "DriverDesc", desc, sizeof(desc));
    gs_class_value(drvkey, "InfPath", infp, sizeof(infp));
    if (drvsafe_driver_is_3dfx(prov, mfg, desc, infp, service))
        return DRVSAFE_3DFX_DRIVER;

    for (dn = dev->DevInst, depth = 0; depth < 12 && ntdyn_CM_Get_Parent(&parent, dn, 0) == CR_SUCCESS;
         depth++, dn = parent)
        if (ntdyn_CM_Get_Device_IDA(parent, id, sizeof(id), 0) == CR_SUCCESS && drvsafe_id_is_3dfx(id))
            return DRVSAFE_3DFX_ANCESTOR;

    if (drvsafe_is_pci_bridge(hw, compat) && gs_subtree_3dfx(dev->DevInst, 0))
        return DRVSAFE_3DFX_BRIDGE;
    return DRVSAFE_OK;
}

/* Is this INF 3dfx? Its WHOLE text, not the model lines: I001/I003/V001 also
 * register a global OpenGLdrivers\3dfx ICD. Unreadable = not refused here (the
 * caller's own read will fail the same way). */
static int gs_inf_is_3dfx(const char *path)
{
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_INF_READ_MAX + 2);
    int   yes = 0;
    if (!buf)
        return 0;
    if (gs_read_inf(path, buf))
        yes = drvsafe_inf_is_3dfx(buf);
    HeapFree(GetProcessHeap(), 0, buf);
    return yes;
}

/* Does `hwid` (a line-start prefix, as DRVUPDATE and PREFER.TXT use it) reach a
 * PRESENT device the 3dfx rule protects? Its own ids, its bound driver, its
 * parents or - for a bridge - its children: the HiNT bridge of a V5 6000 has no
 * 3dfx id of its own. Returns the DRVSAFE_* reason. */
static int gs_hwid_touches_3dfx(const char *hwid)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dev;
    DWORD           i;
    gs_probdev     *pd;
    size_t          n = strlen(hwid);
    int             why = DRVSAFE_OK;
    const char     *p;

    if (drvsafe_id_is_3dfx(hwid))
        return DRVSAFE_3DFX_ID;
    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return DRVSAFE_OK;
    pd = (gs_probdev *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(gs_probdev));
    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (i = 0; pd && why == DRVSAFE_OK && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        int hit = 0;
        pd->dev = dev;
        gs_device_ids(set, pd);
        for (p = pd->hw; *p && !hit; p += strlen(p) + 1)
            hit = _strnicmp(p, hwid, n) == 0;
        for (p = pd->compat; *p && !hit; p += strlen(p) + 1)
            hit = _strnicmp(p, hwid, n) == 0;
        if (hit)
            why = gs_device_3dfx(set, &dev, pd->hw, pd->compat);
    }
    if (pd)
        HeapFree(GetProcessHeap(), 0, pd);
    SetupDiDestroyDeviceInfoList(set);
    return why;
}

/* Every present device carrying a problem code, into out[] (zeroed by caller).
 * *truncated is set when there were more than `max` - a caller deciding whether
 * C:\D may be deleted must then treat the answer as unknown. */
static int gs_problem_devices(HDEVINFO set, gs_probdev *out, int max,
                              int *truncated)
{
    SP_DEVINFO_DATA dev;
    DWORD           i;
    int             n = 0;

    *truncated = 0;
    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        DWORD status = 0, problem = 0;
        if (ntdyn_CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) != CR_SUCCESS)
            continue;
        if (problem == 0 && !(status & DN_HAS_PROBLEM))
            continue;
        if (!drvmatch_problem_wants_driver(problem))
            continue;                   /* disabled: no driver will change that */
        if (n >= max) {
            *truncated = 1;
            break;
        }
        out[n].dev = dev;
        out[n].problem = problem;
        out[n].desc[0] = 0;
        SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL,
                                          (PBYTE)out[n].desc, sizeof(out[n].desc),
                                          NULL);
        gs_device_ids(set, &out[n]);
        out[n].excl = gs_device_3dfx(set, &dev, out[n].hw, out[n].compat);
        n++;
    }
    return n;
}

/* Read an INF, fold UTF-16, upper-case. buf must hold GS_INF_READ_MAX + 2. */
static int gs_read_inf(const char *path, char *buf)
{
    HANDLE h;
    DWORD  got = 0;
    size_t len;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(h, buf, GS_INF_READ_MAX, &got, NULL))
        got = 0;
    CloseHandle(h);
    if (!got)
        return 0;
    buf[got] = buf[got + 1] = 0;
    len = drvmatch_fold_utf16(buf, got);
    buf[len] = 0;
    CharUpperA(buf);                    /* INFs spell ids in mixed case */
    return 1;
}

/* One walk of C:\D, ranking candidate INFs for every device in d[0..n).
 * Returns 0, or -1 when the tree could not be searched at all - which callers
 * must NOT read as "nothing serves it": the guard keeps C:\D on -1. */
static int gs_scan_driver_tree(gs_probdev *d, int n)
{
    WIN32_FIND_DATAA fd, ff;
    HANDLE           hd, hf;
    char             pat[MAX_PATH], sub[MAX_PATH], infp[MAX_PATH];
    char            *buf;
    int              k, any = 0;

    for (k = 0; k < n; k++) {
        d[k].cand.n = 0;
        if (d[k].nids)
            any = 1;
    }
    if (!any)
        return 0;
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_INF_READ_MAX + 2);
    if (!buf)
        return -1;
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", GS_DRIVER_DIR);
    pat[sizeof(pat) - 1] = 0;
    hd = FindFirstFileA(pat, &fd);
    if (hd == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, buf);
        return -1;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
            continue;
        _snprintf(sub, sizeof(sub) - 1, "%s\\%s\\*.inf", GS_DRIVER_DIR, fd.cFileName);
        sub[sizeof(sub) - 1] = 0;
        hf = FindFirstFileA(sub, &ff);
        if (hf == INVALID_HANDLE_VALUE)
            continue;
        do {
            _snprintf(infp, sizeof(infp) - 1, "%s\\%s\\%s", GS_DRIVER_DIR,
                      fd.cFileName, ff.cFileName);
            infp[sizeof(infp) - 1] = 0;
            if (!gs_read_inf(infp, buf))
                continue;
            if (drvsafe_inf_is_3dfx(buf))
                continue;               /* a 3dfx INF is never an automatic candidate */
            drvmatch_prepare(buf);      /* model-line id fields only */
            for (k = 0; k < n; k++) {
                int idx;
                if (d[k].excl)
                    continue;
                idx = drvmatch_best(buf, d[k].ids, d[k].nids);
                if (idx >= 0)
                    drvmatch_cand_add(&d[k].cand, idx, infp);
            }
        } while (FindNextFileA(hf, &ff));
        FindClose(hf);
    } while (FindNextFileA(hd, &fd));
    FindClose(hd);
    HeapFree(GetProcessHeap(), 0, buf);
    return 0;
}

/* Is the INF's payload staged? See drvmatch_payload_ok() for why this is coarse.
 * A file counts as present as NAME or compressed NAME with a trailing '_'. */
typedef struct {
    char dir[MAX_PATH];
    int  files, missing, sys, sysmissing;
    char first_missing[64];
} gs_payload_ctx;

static void gs_payload_file(const char *rel, void *vctx)
{
    gs_payload_ctx *c = (gs_payload_ctx *)vctx;
    char            full[MAX_PATH + 400];
    size_t          n = strlen(rel);
    int             is_sys = n > 4 && strcmp(rel + n - 4, ".SYS") == 0;

    c->files++;
    if (is_sys)
        c->sys++;
    _snprintf(full, sizeof(full) - 1, "%s\\%s", c->dir, rel);
    full[sizeof(full) - 1] = 0;
    if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES)
        return;
    n = strlen(full);
    if (n) {
        full[n - 1] = '_';
        if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES)
            return;
    }
    if (!c->missing)
        lstrcpynA(c->first_missing, rel, sizeof(c->first_missing));
    c->missing++;
    if (is_sys)
        c->sysmissing++;
}

static int gs_inf_payload_ok(const char *inf, char *buf, char *why, size_t why_cch)
{
    gs_payload_ctx c;
    char          *slash;

    memset(&c, 0, sizeof(c));
    lstrcpynA(c.dir, inf, sizeof(c.dir));
    slash = strrchr(c.dir, '\\');
    if (slash)
        *slash = 0;
    if (!gs_read_inf(inf, buf))
        return 1;                       /* cannot read it: let Windows decide */
    drvmatch_payload(buf, gs_payload_file, &c);
    if (drvmatch_payload_ok(c.files, c.missing, c.sys, c.sysmissing))
        return 1;
    _snprintf(why, why_cch - 1, "%d of %d listed files missing, e.g. %s",
              c.missing, c.files, c.first_missing);
    why[why_cch - 1] = 0;
    return 0;
}

/* Ask Windows whether `inf` has a driver for this device, by its own rules -
 * [Manufacturer] decorations, every hardware AND compatible id, the lot - by
 * building a compatible-driver list from that single INF. Returns 1 yes, 0 no,
 * -1 cannot tell (API unavailable or the list could not be built).
 *
 * These are resolved at run time, not imported: they are not among the
 * SetupDi* entry points proven to load on Win98SE, and one unresolvable static
 * import kills the whole agent at EXE load there (see ntdyn.h). */
typedef BOOL (WINAPI *sdi_params_fn)(HDEVINFO, PSP_DEVINFO_DATA, PSP_DEVINSTALL_PARAMS_A);
typedef BOOL (WINAPI *sdi_build_fn)(HDEVINFO, PSP_DEVINFO_DATA, DWORD);
typedef BOOL (WINAPI *sdi_enum_fn)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, DWORD,
                                   PSP_DRVINFO_DATA_A);
typedef BOOL (WINAPI *sdi_nonint_fn)(BOOL);

static sdi_params_fn g_sdi_get_params, g_sdi_set_params;
static sdi_build_fn  g_sdi_build, g_sdi_destroy;
static sdi_enum_fn   g_sdi_enum;
static sdi_nonint_fn g_sdi_nonint;

static void gs_sdi_resolve(void)
{
    static int resolved;
    HMODULE    m;

    if (resolved)
        return;
    resolved = 1;
    m = GetModuleHandleA("setupapi.dll");
    if (!m)
        m = LoadLibraryA("setupapi.dll");
    if (!m)
        return;
    g_sdi_get_params = (sdi_params_fn)GetProcAddress(m, "SetupDiGetDeviceInstallParamsA");
    g_sdi_set_params = (sdi_params_fn)GetProcAddress(m, "SetupDiSetDeviceInstallParamsA");
    g_sdi_build      = (sdi_build_fn)GetProcAddress(m, "SetupDiBuildDriverInfoList");
    g_sdi_destroy    = (sdi_build_fn)GetProcAddress(m, "SetupDiDestroyDriverInfoList");
    g_sdi_enum       = (sdi_enum_fn)GetProcAddress(m, "SetupDiEnumDriverInfoA");
    g_sdi_nonint     = (sdi_nonint_fn)GetProcAddress(m, "SetupSetNonInteractiveMode");
}

static int gs_inf_serves(HDEVINFO set, SP_DEVINFO_DATA *dev, const char *inf)
{
    SP_DEVINSTALL_PARAMS_A p, orig;
    SP_DRVINFO_DATA_A      di;
    int                    ok;

    gs_sdi_resolve();
    if (!g_sdi_get_params || !g_sdi_set_params || !g_sdi_build || !g_sdi_destroy ||
        !g_sdi_enum)
        return -1;
    memset(&p, 0, sizeof(p));
    p.cbSize = sizeof(p);
    if (!g_sdi_get_params(set, dev, &p))
        return -1;
    orig = p;
    p.Flags |= DI_ENUMSINGLEINF;
    lstrcpynA(p.DriverPath, inf, sizeof(p.DriverPath));
    if (!g_sdi_set_params(set, dev, &p))
        return -1;
    if (g_sdi_build(set, dev, SPDIT_COMPATDRIVER)) {
        memset(&di, 0, sizeof(di));
        di.cbSize = sizeof(di);
        ok = g_sdi_enum(set, dev, SPDIT_COMPATDRIVER, 0, &di) ? 1 : 0;
        g_sdi_destroy(set, dev, SPDIT_COMPATDRIVER);
    } else {
        ok = -1;
    }
    g_sdi_set_params(set, dev, &orig);
    return ok;
}

/* May this candidate be forced onto the device? Payload first (cheap, and the
 * check Windows' own does not make), then Windows' confirmation. Returns 1 yes,
 * 0 no (with the reason logged), -1 yes-but-unconfirmed. */
static int gs_candidate_ok(HDEVINFO set, SP_DEVINFO_DATA *dev, const char *inf,
                           const char *id, const char *name, char *buf)
{
    char why[160];
    int  serves;

    if (!gs_inf_payload_ok(inf, buf, why, sizeof(why))) {
        log_msg(LOG_GS, "  %s names %s but its payload is not staged (%s) - skipped",
                inf, id, why);
        return 0;
    }
    serves = gs_inf_serves(set, dev, inf);
    if (serves == 0)
        log_msg(LOG_GS, "  %s names %s but Windows finds no driver in it for %s "
                        "- skipped", inf, id, name);
    return serves;
}

/*
 * Install drivers for devices Windows left unconfigured.
 *
 * GUI setup does not always get there. A Dell Dimension 3000 finished a clean
 * install and sat on the VGA fallback at 640x480 in 16 colours, with the Intel
 * 865G driver it needed present on its own disk in C:\D the whole time, waiting
 * for someone to walk over and answer a Found New Hardware wizard. Three
 * devices were in that state: the display, the audio, and a multimedia
 * controller. (Winnt.sif carries only the LAN + chipset path; DevicePath is
 * written at T-12, after GUI setup has already installed the devices.)
 *
 * So the agent finishes the job. For each device carrying a problem code:
 *   - a device PREFER.TXT names is left to gs_apply_driver_prefs(), which
 *     forces the exact build the fleet wants (the ranking here cannot know that
 *     .143's GeForce 6800 must get ForceWare 71.89 and not the 270.61 that
 *     sorts first);
 *   - otherwise the ranked candidates from C:\D are tried in order, each first
 *     checked (gs_candidate_ok), until one installs AND leaves the device
 *     working (or asks for the restart that will);
 *   - on at most two boots per device (gs_drvfix_attempts).
 *
 * The id handed to UpdateDriverForPlugAndPlayDevices is the one the INF names.
 * It matches that against every present device's hardware AND compatible ids,
 * which is why a family id ("*PNP0501", "USB\ROOT_HUB") never gets this far -
 * FORCE would reach every device that shares it - and why identical devices
 * are handled by one install, not one each.
 */
#define INSTALLFLAG_FORCE_          0x00000001
#define INSTALLFLAG_NONINTERACTIVE_ 0x00000004

typedef BOOL (WINAPI *updrv_fn)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

/* The job owns COPIES of its strings: a hung job is leaked on purpose, and the
 * device array its id and INF came from is freed while it is still running. */
typedef struct {
    updrv_fn update;
    char     id[256];
    char     inf[MAX_PATH];
    BOOL     reboot;
    BOOL     ok;
    DWORD    err;
} gs_install_job;

/* NONINTERACTIVE is honoured by XP SP3's newdev (flags up to 0x7 are accepted;
 * it maps 0x4 to the internal switch that fails with 1459 rather than showing
 * a finish-install page), and it is the only thing that stops newdev's own
 * wizard. SetupSetNonInteractiveMode covers setupapi's disk prompts - and newdev
 * CLEARS it at the end of every install, so it is set again before each call. */
static DWORD WINAPI gs_install_thread(LPVOID arg)
{
    gs_install_job *j = (gs_install_job *)arg;
    if (g_sdi_nonint)
        g_sdi_nonint(TRUE);
    j->ok = j->update(NULL, j->id, j->inf,
                      INSTALLFLAG_FORCE_ | INSTALLFLAG_NONINTERACTIVE_, &j->reboot);
    j->err = j->ok ? 0 : GetLastError();
    return 0;
}

/* One forced install, in SetupAPI's non-interactive mode (where it exists) and
 * under a watchdog. Returns 1 installed, 0 failed, -1 hung. On -1 the job and
 * its thread are deliberately leaked: they are still inside SetupAPI. */
static int gs_force_install(updrv_fn update, const char *id, const char *inf,
                            BOOL *reboot, DWORD *err)
{
    gs_install_job *j;
    HANDLE          th;
    DWORD           tid = 0;
    int             rc;

    j = (gs_install_job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*j));
    if (!j)
        return 0;
    j->update = update;
    lstrcpynA(j->id, id, sizeof(j->id));
    lstrcpynA(j->inf, inf, sizeof(j->inf));
    th = CreateThread(NULL, 0, gs_install_thread, j, 0, &tid);
    if (!th) {
        HeapFree(GetProcessHeap(), 0, j);
        return 0;
    }
    if (WaitForSingleObject(th, GS_INSTALL_WAIT) != WAIT_OBJECT_0) {
        CloseHandle(th);
        return -1;
    }
    CloseHandle(th);
    *reboot = j->reboot;
    *err = j->err;
    rc = j->ok ? 1 : 0;
    HeapFree(GetProcessHeap(), 0, j);
    return rc;
}

/* Driver-signing policy for the duration of the pass. DriverPacks edits the
 * INFs it ships, which breaks their catalogs - I015\ialmnt5.inf, the Dell
 * 865G's display driver, is not in igfxnt5.cat - and a NON-INTERACTIVE install
 * of an unsigned driver under policy Warn FAILS. winnt.sif's
 * DriverSigningPolicy=Ignore does not reliably survive setup (a freshly imaged
 * box has been found at Warn), so set Ignore (00) the way tools/README-drvupd.md
 * does by hand, and put back exactly what was there. */
#define GS_SIGNING_KEY "Software\\Microsoft\\Driver Signing"

typedef struct {
    int   had;
    DWORD type, size;
    BYTE  data[16];
} gs_signing_saved;

static void gs_signing_relax(gs_signing_saved *sv)
{
    HKEY k;
    BYTE ignore = 0;

    memset(sv, 0, sizeof(*sv));
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, GS_SIGNING_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    sv->size = sizeof(sv->data);
    sv->had = RegQueryValueExA(k, "Policy", NULL, &sv->type, sv->data,
                               &sv->size) == ERROR_SUCCESS;
    RegSetValueExA(k, "Policy", 0, REG_BINARY, &ignore, 1);
    RegCloseKey(k);
}

static void gs_signing_restore(const gs_signing_saved *sv)
{
    HKEY k;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, GS_SIGNING_KEY, 0, KEY_WRITE, &k) !=
        ERROR_SUCCESS)
        return;
    if (sv->had)
        RegSetValueExA(k, "Policy", 0, sv->type, sv->data, sv->size);
    else
        RegDeleteValueA(k, "Policy");
    RegCloseKey(k);
}

static void gs_install_missing_drivers(void)
{
    HDEVINFO    set;
    gs_probdev *pd;
    HMODULE     newdev;
    updrv_fn    update;
    char       *buf;
    BOOL        was_nonint = FALSE;
    gs_signing_saved signing;
    int         n, k, scan, truncated, fixed = 0, tried = 0;

    g_gs_ndv = 0;
    if (!gs_file_exists(GS_DRIVER_DIR))
        return;
    newdev = LoadLibraryA("newdev.dll");
    if (!newdev)
        return;
    update = (updrv_fn)GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesA");
    if (!update) {
        FreeLibrary(newdev);
        return;
    }
    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        FreeLibrary(newdev);
        return;
    }
    pd = (gs_probdev *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                 GS_MAX_PROBDEV * sizeof(gs_probdev));
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_INF_READ_MAX + 2);
    if (!pd || !buf) {
        if (pd)
            HeapFree(GetProcessHeap(), 0, pd);
        if (buf)
            HeapFree(GetProcessHeap(), 0, buf);
        SetupDiDestroyDeviceInfoList(set);
        FreeLibrary(newdev);
        return;
    }
    n = gs_problem_devices(set, pd, GS_MAX_PROBDEV, &truncated);
    if (truncated)
        log_msg(LOG_GS, "more than %d unconfigured devices - handling the first %d",
                GS_MAX_PROBDEV, GS_MAX_PROBDEV);
    scan = n ? gs_scan_driver_tree(pd, n) : 0;

    /* Fail instead of prompting. XP has no non-interactive flag for
     * UpdateDriverForPlugAndPlayDevices; this process-wide switch is what makes
     * a missing file an error rather than a dialog nobody will answer. */
    gs_sdi_resolve();
    if (g_sdi_nonint)
        was_nonint = g_sdi_nonint(TRUE);
    gs_signing_relax(&signing);

    for (k = 0; k < n && !g_gs_install_hung; k++) {
        const char *name = pd[k].desc[0] ? pd[k].desc : "(unnamed)";
        int         c, ran, verdict = DRVMATCH_V_FAILED;
        DWORD       attempts;

        if (gs_dv_lookup(pd[k].hw) != DRVMATCH_V_UNSEEN)
            continue;                   /* an identical device, already handled */
        if (pd[k].excl) {
            log_msg(LOG_GS, "unconfigured %s (problem %lu): %s - NOT touched (3dfx "
                            "drivers change only on an explicit ALLOW3DFX request)",
                    name, pd[k].problem, drvsafe_reason_name(pd[k].excl));
            gs_dv_record(pd[k].hw, DRVMATCH_V_EXCLUDED);
            continue;
        }
        if (gs_prefer_claims(&pd[k])) {
            log_msg(LOG_GS, "unconfigured %s: PREFER.TXT names it - leaving it to "
                            "the driver-preference pass", name);
            gs_dv_record(pd[k].hw, DRVMATCH_V_PREFER);
            continue;
        }
        if (scan < 0) {
            log_msg(LOG_GS, "unconfigured %s: could not search %s", name,
                    GS_DRIVER_DIR);
            gs_dv_record(pd[k].hw, DRVMATCH_V_UNSEARCHED);
            continue;
        }
        if (!pd[k].cand.n) {
            log_msg(LOG_GS, "no driver in %s for %s (%s) - leaving it",
                    GS_DRIVER_DIR, name, pd[k].hw[0] ? pd[k].hw : "no ids");
            gs_dv_record(pd[k].hw, DRVMATCH_V_NONE);
            continue;
        }
        if (gs_drvfix_attempts(pd[k].hw, 0) >= 2) {
            log_msg(LOG_GS, "unconfigured %s: C:\\D drivers already tried on two "
                            "boots - leaving it", name);
            gs_dv_record(pd[k].hw, DRVMATCH_V_FAILED);
            continue;
        }
        attempts = gs_drvfix_attempts(pd[k].hw, 1);
        tried++;
        ran = 0;
        for (c = 0; c < pd[k].cand.n; c++) {
            const char *inf = pd[k].cand.path[c];
            const char *id  = pd[k].ids[pd[k].cand.idx[c]];
            BOOL        reboot = FALSE;
            DWORD       err = 0, status = 0, problem = 0;
            int         ok, rc;

            ok = gs_candidate_ok(set, &pd[k].dev, inf, id, name, buf);
            if (ok == 0)
                continue;
            log_msg(LOG_GS, "installing %s for %s (matched %s%s)", inf, name, id,
                    ok < 0 ? ", unconfirmed" : "");
            rc = gs_force_install(update, id, inf, &reboot, &err);
            if (rc < 0) {
                g_gs_install_hung = 1;
                log_msg(LOG_GS, "  INSTALL DID NOT RETURN after %d min - probably a "
                                "prompt on the console. No more forced installs, "
                                "and %s is kept, this boot.",
                        GS_INSTALL_WAIT / 60000, GS_DRIVER_DIR);
                verdict = DRVMATCH_V_UNSEARCHED;
                break;
            }
            if (!rc) {
                ran = 1;
                log_msg(LOG_GS, "  FAILED (%lu)%s", err,
                        c + 1 < pd[k].cand.n ? " - trying the next candidate" : "");
                continue;
            }
            if (reboot) {
                fixed++;
                verdict = DRVMATCH_V_INSTALLED;
                log_msg(LOG_GS, "  installed (needs a reboot)");
                break;
            }
            /* Installed without a restart: is the device actually working? */
            Sleep(2000);
            if (ntdyn_CM_Get_DevNode_Status(&status, &problem, pd[k].dev.DevInst,
                                            0) == CR_SUCCESS &&
                (problem != 0 || (status & DN_HAS_PROBLEM)) &&
                drvmatch_problem_driver_fixable(problem)) {
                log_msg(LOG_GS, "  installed but the device still has problem %lu%s",
                        problem,
                        c + 1 < pd[k].cand.n ? " - trying the next candidate" : "");
                continue;
            }
            fixed++;
            verdict = DRVMATCH_V_INSTALLED;
            log_msg(LOG_GS, problem ? "  installed (device reports problem %lu, "
                                      "which no driver clears)"
                                    : "  installed - device working", problem);
            break;
        }
        /* Every install that RAN returned an error (a refusal, not a broken
         * device): keep C:\D for the next boot's attempt, if there is one. */
        if (verdict == DRVMATCH_V_FAILED && ran && attempts < 2)
            verdict = DRVMATCH_V_ERRORED;
        gs_dv_record(pd[k].hw, verdict);
    }
    gs_signing_restore(&signing);
    /* Not while a hung install is still inside setupapi: turning prompts back
     * on under it is how it would get one. */
    if (g_sdi_nonint && !g_gs_install_hung)
        g_sdi_nonint(was_nonint);
    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, pd);
    SetupDiDestroyDeviceInfoList(set);
    /* newdev.dll stays loaded if an install thread is still inside it. */
    if (!g_gs_install_hung)
        FreeLibrary(newdev);
    if (tried)
        log_msg(LOG_GS, "unconfigured devices: %d of %d now have drivers",
                fixed, tried);
}

/* Are there devices the staged tree could still fix?
 *
 * This gates the reclaim, and it matters: the staged drivers in C:\D are
 * precisely what the Found New Hardware wizard needs. Deleting them while a
 * device is still unconfigured would take away the only local copy at the exact
 * moment it is wanted - and on a machine whose NIC is the unconfigured device,
 * there is no network left to fetch a replacement over. So when the answer
 * cannot be determined, the answer is KEEP.
 *
 * But only devices the STAGED TREE could actually help are worth keeping it for.
 * The first version counted every unconfigured device, and two that C:\D can
 * never serve - a phantom PS/2 mouse on a machine with a USB one, and an in-box
 * WDM audio stub - held 2.4 GB of drivers on a 6 GB disk indefinitely, which in
 * turn left no room for the game library. The decision per device is
 * drvmatch_keeps_tree(), so the regression test pins it.
 */
static int gs_devices_unconfigured(void)
{
    HDEVINFO    set;
    gs_probdev *pd;
    char       *buf;
    int         n, k, bad = 0, scan = 0, need_scan = 0, truncated = 0;

    if (g_gs_install_hung) {
        log_msg(LOG_GS, "a forced install is still hung - keeping %s", GS_DRIVER_DIR);
        return 1;
    }
    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 1;                 /* cannot tell -> assume yes, keep drivers */
    pd = (gs_probdev *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                 GS_MAX_PROBDEV * sizeof(gs_probdev));
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_INF_READ_MAX + 2);
    if (!pd || !buf) {
        if (pd)
            HeapFree(GetProcessHeap(), 0, pd);
        if (buf)
            HeapFree(GetProcessHeap(), 0, buf);
        SetupDiDestroyDeviceInfoList(set);
        return 1;
    }
    n = gs_problem_devices(set, pd, GS_MAX_PROBDEV, &truncated);
    if (truncated) {
        log_msg(LOG_GS, "more than %d unconfigured devices - cannot judge them all, "
                        "keeping %s", GS_MAX_PROBDEV, GS_DRIVER_DIR);
        bad++;
    }
    for (k = 0; k < n; k++)
        if (!pd[k].excl && gs_dv_lookup(pd[k].hw) == DRVMATCH_V_UNSEEN)
            need_scan = 1;
    if (need_scan)
        scan = gs_scan_driver_tree(pd, n);

    for (k = 0; k < n; k++) {
        const char *name = pd[k].desc[0] ? pd[k].desc : "(unnamed)";
        int         v = gs_dv_lookup(pd[k].hw), confirmed = 0, c = 0;

        if (pd[k].excl) {
            log_msg(LOG_GS, "device not configured (problem %lu): %s - %s, never "
                            "installed automatically; not a reason to keep the tree",
                    pd[k].problem, name, drvsafe_reason_name(pd[k].excl));
            continue;
        }
        if (v == DRVMATCH_V_UNSEEN && scan == 0) {
            for (c = 0; c < pd[k].cand.n; c++)
                if (gs_candidate_ok(set, &pd[k].dev, pd[k].cand.path[c],
                                    pd[k].ids[pd[k].cand.idx[c]], name, buf) != 0) {
                    confirmed = 1;
                    break;
                }
        }
        if (drvmatch_keeps_tree(v, scan == 0, confirmed)) {
            if (confirmed)
                log_msg(LOG_GS, "device not configured (problem %lu): %s - a driver "
                                "for it IS staged (%s) - keeping %s", pd[k].problem,
                        name, pd[k].cand.path[c], GS_DRIVER_DIR);
            else
                log_msg(LOG_GS, "device not configured (problem %lu): %s - could "
                                "not search %s, keeping it", pd[k].problem, name,
                        GS_DRIVER_DIR);
            bad++;
            continue;
        }
        log_msg(LOG_GS, "device not configured (problem %lu): %s - %s; not a reason "
                        "to keep the tree", pd[k].problem, name,
                v == DRVMATCH_V_INSTALLED ? "its staged driver is installed" :
                v == DRVMATCH_V_FAILED    ? "the staged drivers did not fix it" :
                v == DRVMATCH_V_PREFER    ? "PREFER.TXT owns it" :
                                            "nothing in C:\\D serves it");
    }
    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, pd);
    SetupDiDestroyDeviceInfoList(set);
    return bad;
}

/* ---------------------------------------------------------------------- */
/* driver preferences - drivers we must FORCE over the one Windows chose    */
/* ---------------------------------------------------------------------- */

/*
 * A device WITHOUT a problem code is not necessarily a device with the right
 * driver, and gs_install_missing_drivers() above only ever looks at the ones
 * Windows failed to configure. That leaves the case that actually shipped: on
 * the freshly imaged .124 (2026-08-29) a GeForce2 GTS came up on Microsoft's
 * in-box nv4 6.14.10.5673 at 800x600 in 16-bit colour, status OK, problem code
 * 0 - with ForceWare 71.89 sitting unused in C:\D\G005 the whole time.
 *
 * TWO separate mechanisms put it there, both silent, and only an explicit
 * forced install beats either:
 *
 *  1. winnt.sif carries only the SHORT early driver path (LAN + chipset). The
 *     rest waits for DevicePath, which cmdlines.txt writes at T-12, AFTER GUI
 *     setup has installed the devices. That box's setupapi.log has exactly six
 *     "Found ... in C:\D\" lines and every one is an L or a C directory; not
 *     one G, H, I, M, N, S or T was ever consulted.
 *  2. Even when an INF in C:\D IS visible, XP penalises an untrusted driver
 *     node by +0x8000 in the rank ("#I087 Driver node not trusted, rank
 *     changed from 0x2000 to 0xa000"), so it loses to any trusted in-box
 *     match. DriverSigningPolicy=Ignore suppresses the DIALOG, not the RANK.
 *
 * So the image ships an explicit list - C:\D\PREFER.TXT, "<hardware id>\t<INF>"
 * per line, generated by stage-oem.sh from scripts/pxe/driver-prefs.txt - and
 * we force those with UpdateDriverForPlugAndPlayDevices, which does not consult
 * the ranking at all.
 *
 * WHY AN EXPLICIT LIST RATHER THAN A HEURISTIC. The C:\D ranking
 * (gs_scan_driver_tree) can tell which INFs serve a device, but not which BUILD
 * the fleet wants: for PCI\VEN_10DE&DEV_0150 three INFs qualify and the first
 * is G003\nv4_go.inf - ForceWare 270.61 MOBILE, a 2011 driver for a 2000 card.
 * Guessing here installs the wrong driver confidently, which is why
 * gs_install_missing_drivers() leaves every device PREFER.TXT names to this pass.
 *
 * ORDERING IS THE WHOLE POINT, and getting it wrong is worse than not trying:
 * this pass runs BEFORE gs_reclaim_drivers(), and the reclaim now refuses while
 * any applicable preference is unsatisfied. Reclaiming first would leave the
 * box with neither the right driver nor the payload to fix itself - which is
 * exactly where .124 ended up.
 *
 * Safe to reclaim AFTER a preference succeeds: setupapi copies the driver's
 * files out of C:\D into system32 and its INF into C:\WINDOWS\inf as part of
 * the install ("#-336 Copying file c:\d\g005\nv4_disp.dl_ to
 * C:\WINDOWS\system32\nv4_disp.dll"), so nothing needs the staged tree once the
 * install has returned - not even across the reboot it asks for.
 *
 * One-shot per box: the outcome is recorded under HKLM\Software\RetroAgent\
 * DriverPrefs keyed by hardware id, so a machine that keeps its newimage flag
 * does not re-install the same driver at every boot.
 */
#define GS_PREFER_FILE  "C:\\D\\PREFER.TXT"
#define GS_PREFS_KEY    "Software\\RetroAgent\\DriverPrefs"

static int gs_pref_record_get(const char *hwid, char *out, DWORD out_cch)
{
    HKEY  k;
    DWORD type = 0, cb;
    LONG  rc;

    if (out_cch == 0)
        return 0;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, GS_PREFS_KEY, 0, KEY_READ, &k)
            != ERROR_SUCCESS)
        return 0;
    cb = out_cch - 1;
    rc = RegQueryValueExA(k, hwid, NULL, &type, (LPBYTE)out, &cb);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS)
        return 0;
    if (cb >= out_cch)
        cb = out_cch - 1;
    out[cb] = 0;
    return 1;
}

static void gs_pref_record_set(const char *hwid, const char *val)
{
    HKEY  k;
    DWORD disp = 0;

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, GS_PREFS_KEY, 0, NULL, 0,
                        KEY_WRITE, NULL, &k, &disp) != ERROR_SUCCESS)
        return;
    RegSetValueExA(k, hwid, 0, REG_SZ, (const BYTE *)val,
                   (DWORD)(strlen(val) + 1));
    RegCloseKey(k);
}

/* Every hardware and compatible id of every PRESENT device, upper-cased, one
 * per line with a leading and trailing newline. A preference matches when its
 * id appears at the start of one of those lines, which is a plain substring
 * search for "\n" + id: PCI\VEN_10DE&DEV_0150 has to match the device's
 * PCI\VEN_10DE&DEV_0150&SUBSYS_002E10DE&REV_A4 without matching some other
 * device that merely contains those characters mid-string. */
#define GS_IDBUF_CAP  262144

static char *gs_present_device_ids(void)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dev;
    DWORD           i, len = 0;
    char           *buf;

    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_IDBUF_CAP);
    if (!buf)
        return NULL;
    buf[len++] = '\n';
    buf[len] = 0;

    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return buf;

    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        int prop;
        for (prop = 0; prop < 2; prop++) {
            char  ids[2048];
            char *p;
            DWORD want = prop ? SPDRP_COMPATIBLEIDS : SPDRP_HARDWAREID;

            memset(ids, 0, sizeof(ids));
            if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, want, NULL,
                                                   (PBYTE)ids,
                                                   sizeof(ids) - 2, NULL))
                continue;
            CharUpperA(ids);
            /* REG_MULTI_SZ: strings back to back, empty string terminates. */
            for (p = ids; *p; p += strlen(p) + 1) {
                DWORD n = (DWORD)strlen(p);
                if (len + n + 2 >= GS_IDBUF_CAP)
                    break;
                memcpy(buf + len, p, n);
                len += n;
                buf[len++] = '\n';
                buf[len] = 0;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return buf;
}

/* Does PREFER.TXT name this device? Matched exactly as gs_prefs_pass() matches
 * it - an id at the start of one of the device's own hardware/compatible ids -
 * so the installer and the preference pass can never both claim one device. */
static int gs_prefer_claims(const gs_probdev *pd)
{
    HANDLE      h;
    DWORD       got = 0, len = 0;
    char       *txt, *line, *next, devids[2200];
    const char *p;
    int         pass, hit = 0;

    if (!gs_file_exists(GS_PREFER_FILE))
        return 0;
    devids[len++] = '\n';
    for (pass = 0; pass < 2; pass++)
        for (p = pass ? pd->compat : pd->hw; *p; p += strlen(p) + 1) {
            DWORD m = (DWORD)strlen(p);
            if (len + m + 2 >= sizeof(devids))
                break;
            memcpy(devids + len, p, m);
            len += m;
            devids[len++] = '\n';
        }
    devids[len] = 0;

    h = CreateFileA(GS_PREFER_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    txt = (char *)HeapAlloc(GetProcessHeap(), 0, 262144);
    if (!txt) {
        CloseHandle(h);
        return 0;
    }
    if (!ReadFile(h, txt, 262143, &got, NULL))
        got = 0;
    txt[got] = 0;
    CloseHandle(h);
    for (line = txt; line && *line && !hit; line = next) {
        char *lhwid = NULL, *linf = NULL, hwid[256];
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!drvpref_split(line, &lhwid, &linf))
            continue;
        lstrcpynA(hwid, lhwid, sizeof(hwid));
        CharUpperA(hwid);
        hit = drvpref_present(devids, hwid);
    }
    HeapFree(GetProcessHeap(), 0, txt);
    return hit;
}

/* The INF PREFER.TXT assigns to this device, if any (first matching line). */
static int gs_prefer_inf_for(const gs_probdev *pd, char *out, DWORD out_cch)
{
    HANDLE      h;
    DWORD       got = 0, len = 0;
    char       *txt, *line, *next, devids[2200];
    const char *p;
    int         pass, hit = 0;

    devids[len++] = '\n';
    for (pass = 0; pass < 2; pass++)
        for (p = pass ? pd->compat : pd->hw; *p; p += strlen(p) + 1) {
            DWORD m = (DWORD)strlen(p);
            if (len + m + 2 >= sizeof(devids))
                break;
            memcpy(devids + len, p, m);
            len += m;
            devids[len++] = '\n';
        }
    devids[len] = 0;
    h = CreateFileA(GS_PREFER_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    txt = (char *)HeapAlloc(GetProcessHeap(), 0, 262144);
    if (!txt) {
        CloseHandle(h);
        return 0;
    }
    if (!ReadFile(h, txt, 262143, &got, NULL))
        got = 0;
    txt[got] = 0;
    CloseHandle(h);
    for (line = txt; line && *line && !hit; line = next) {
        char *lhwid = NULL, *linf = NULL, hwid[256];
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!drvpref_split(line, &lhwid, &linf))
            continue;
        lstrcpynA(hwid, lhwid, sizeof(hwid));
        CharUpperA(hwid);
        if (drvpref_present(devids, hwid)) {
            lstrcpynA(out, linf, out_cch);
            hit = 1;
        }
    }
    HeapFree(GetProcessHeap(), 0, txt);
    return hit;
}

/*
 * One pass over PREFER.TXT.
 *
 * apply != 0: install what is missing and record the outcome.
 * apply == 0: change nothing, just count.
 *
 * Returns the number of preferences that APPLY TO THIS MACHINE and are NOT
 * satisfied - which is what gates the reclaim. A preference for hardware this
 * box does not have is not unsatisfied, it is irrelevant, and must never hold
 * 2.4 GB of drivers on a 6 GB disk.
 */
static int gs_prefs_pass(int apply)
{
    HANDLE   h;
    DWORD    got = 0;
    char    *txt, *ids, *line, *next;
    int      blocking = 0, applied = 0, failed = 0, seen = 0;
    HMODULE  newdev = NULL;
    updrv_fn update = NULL;

    if (!gs_file_exists(GS_PREFER_FILE))
        return 0;
    h = CreateFileA(GS_PREFER_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    txt = (char *)HeapAlloc(GetProcessHeap(), 0, 262144);
    if (!txt) {
        CloseHandle(h);
        return 0;
    }
    if (!ReadFile(h, txt, 262143, &got, NULL))
        got = 0;
    txt[got] = 0;
    CloseHandle(h);

    ids = gs_present_device_ids();
    if (apply) {
        newdev = LoadLibraryA("newdev.dll");
        update = newdev ? (updrv_fn)GetProcAddress(newdev,
                              "UpdateDriverForPlugAndPlayDevicesA") : NULL;
    }

    for (line = txt; line && *line; line = next) {
        char  hwid[256], inf[MAX_PATH], rec[288];
        char *lhwid = NULL, *linf = NULL;

        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!drvpref_split(line, &lhwid, &linf))
            continue;
        lstrcpynA(hwid, lhwid, sizeof(hwid));
        lstrcpynA(inf, linf, sizeof(inf));
        CharUpperA(hwid);

        if (!drvpref_present(ids, hwid))
            continue;               /* not this machine's hardware */
        {
            int why = gs_hwid_touches_3dfx(hwid);
            if (why == DRVSAFE_OK && gs_file_exists(inf) && gs_inf_is_3dfx(inf))
                why = DRVSAFE_3DFX_INF;
            if (why != DRVSAFE_OK) {
                if (apply)
                    log_msg(LOG_GS, "driver preference %s -> %s: %s - NOT applied "
                                    "(3dfx drivers change only on an explicit request)",
                            hwid, inf, drvsafe_reason_name(why));
                continue;           /* not blocking either: it will never run */
            }
        }
        seen++;

        {
            int satisfied = gs_pref_record_get(hwid, rec, sizeof(rec)) &&
                            strncmp(rec, "ok", 2) == 0;
            int exists    = gs_file_exists(inf);

            if (!drvpref_blocks(1, satisfied, exists))
                continue;           /* already on our driver */
            if (!exists) {
                /* The staged tree is gone or the path is stale. Keep whatever
                 * is left rather than reclaiming on a broken preference. */
                blocking++;
                if (apply)
                    log_msg(LOG_GS, "driver preference %s: %s is not there",
                            hwid, inf);
                continue;
            }
            if (!apply || !update) {
                blocking++;
                continue;
            }
        }

        {
            BOOL reboot = FALSE;
            log_msg(LOG_GS, "driver preference: forcing %s onto %s", inf, hwid);
            if (update(NULL, hwid, inf, INSTALLFLAG_FORCE_, &reboot)) {
                char v[MAX_PATH + 8];
                _snprintf(v, sizeof(v) - 1, "ok %s", inf);
                v[sizeof(v) - 1] = 0;
                gs_pref_record_set(hwid, v);
                applied++;
                log_msg(LOG_GS, "  installed%s",
                        reboot ? " (needs a reboot to take effect)" : "");
            } else {
                char v[64];
                _snprintf(v, sizeof(v) - 1, "fail %lu", GetLastError());
                v[sizeof(v) - 1] = 0;
                gs_pref_record_set(hwid, v);
                failed++;
                blocking++;
                log_msg(LOG_GS, "  FAILED (%lu) - keeping %s so it can be "
                                "retried by hand", GetLastError(),
                        GS_DRIVER_DIR);
            }
        }
    }

    if (newdev)
        FreeLibrary(newdev);
    if (ids)
        HeapFree(GetProcessHeap(), 0, ids);
    HeapFree(GetProcessHeap(), 0, txt);
    if (apply && seen)
        log_msg(LOG_GS, "driver preferences: %d for this machine, %d installed, "
                        "%d failed", seen, applied, failed);
    return blocking;
}

static void gs_apply_driver_prefs(void)
{
    /* A forced install is still hung inside setupapi: forcing another one on
     * this thread, with no watchdog, is how the whole startup stops. The
     * reclaim is already refused, so the preference is simply retried next boot. */
    if (g_gs_install_hung) {
        log_msg(LOG_GS, "driver preferences skipped this boot - an install is hung");
        return;
    }
    gs_prefs_pass(1);
}

/* ---------------------------------------------------------------------- */
/* a sound card with a driver and no wave device                           */
/* ---------------------------------------------------------------------- */

/* shared/audiofix.h has the 2026-09-26 Dell this exists for: SoundMAX
 * installed and "working", no wave device, because the RunOnce entries that
 * register XP's kernel audio stack were consumed without ever running. The
 * repair re-applies the box's OWN wdmaudio.inf registration and runs exactly
 * those RunOnce entries - nothing we wrote, nothing another INF queued.
 *
 * Runs on every NT startup (not only on a fresh image): whatever consumed the
 * entries may do so at a later logon too, and on a healthy box the whole check
 * is one waveOutGetNumDevs() call. */
#define GS_AUDIOFIX_VALUE "AudioStackFix"
#define GS_MEDIA_CLASS \
    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e96c-e325-11ce-bfc1-08002be10318}"
#define GS_RUNONCE "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce"

/* A MEDIA-class instance whose MatchingDeviceId names hardware and which
 * carries a Drivers subkey (what a WDM sound INF's AddReg creates). Writes the
 * first one's id into out for the log. */
static int gs_audio_hw_bound(char *out, DWORD cch)
{
    HKEY cls, inst, drv;
    char name[16], id[256];
    DWORD i, n, sz, type;
    int found = 0;

    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, GS_MEDIA_CLASS, 0, KEY_READ, &cls) != ERROR_SUCCESS)
        return 0;
    for (i = 0; !found; i++) {
        n = sizeof(name);
        if (RegEnumKeyExA(cls, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExA(cls, name, 0, KEY_READ, &inst) != ERROR_SUCCESS)
            continue;
        sz = sizeof(id) - 1;
        if (RegQueryValueExA(inst, "MatchingDeviceId", NULL, &type, (LPBYTE)id, &sz) == ERROR_SUCCESS
                && type == REG_SZ) {
            id[sz < sizeof(id) ? sz : sizeof(id) - 1] = 0;
            if (audiofix_is_hw_id(id)
                    && RegOpenKeyExA(inst, "Drivers", 0, KEY_READ, &drv) == ERROR_SUCCESS) {
                RegCloseKey(drv);
                lstrcpynA(out, id, (int)cch);
                found = 1;
            }
        }
        RegCloseKey(inst);
    }
    RegCloseKey(cls);
    return found;
}

static int gs_key_exists(const char *path)
{
    HKEY h;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(h);
    return 1;
}

static DWORD gs_retro_dword(const char *name, DWORD def)
{
    HKEY h;
    DWORD v = def, sz = sizeof(v), type = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, name, NULL, &type, (LPBYTE)&v, &sz) != ERROR_SUCCESS
                || type != REG_DWORD)
            v = def;
        RegCloseKey(h);
    }
    return v;
}

static void gs_retro_set_dword(const char *name, DWORD v)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0,
                        KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(h);
    }
}

typedef HINF (WINAPI *gs_openinf_t)(PCSTR, PCSTR, DWORD, PUINT);
typedef BOOL (WINAPI *gs_infsect_t)(HWND, HINF, PCSTR, UINT, HKEY, PCSTR, UINT,
                                    PSP_FILE_CALLBACK_A, PVOID, HDEVINFO, PSP_DEVINFO_DATA);
typedef void (WINAPI *gs_closeinf_t)(HINF);

/* Write wdmaudio.inf's registration RunOnce entries, exactly as the sound INF's
 * Needs= would have. SPINST_REGISTRY only: its CopyFiles could prompt for
 * media, and the software devices' own installs copy what they need. */
static int gs_audio_rewrite_registration(char *why, size_t cch)
{
    static const char *sections[] = { "WDMAUDIO.Registration.NT", "WDMAUDIO.Registration" };
    HMODULE sa = LoadLibraryA("setupapi.dll");
    gs_openinf_t  openinf  = sa ? (gs_openinf_t)(void *)GetProcAddress(sa, "SetupOpenInfFileA") : NULL;
    gs_infsect_t  infsect  = sa ? (gs_infsect_t)(void *)GetProcAddress(sa, "SetupInstallFromInfSectionA") : NULL;
    gs_closeinf_t closeinf = sa ? (gs_closeinf_t)(void *)GetProcAddress(sa, "SetupCloseInfFile") : NULL;
    char inf[MAX_PATH];
    HINF h;
    int i, ok = 0;

    if (!openinf || !infsect || !closeinf) {
        lstrcpynA(why, "setupapi INF functions unavailable", (int)cch);
        return 0;
    }
    if (!GetWindowsDirectoryA(inf, sizeof(inf) - 20)) {
        lstrcpynA(why, "no Windows directory", (int)cch);
        return 0;
    }
    lstrcatA(inf, "\\inf\\wdmaudio.inf");
    h = openinf(inf, NULL, INF_STYLE_WIN4, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf(why, cch - 1, "cannot open %s (error %lu)", inf, GetLastError());
        why[cch - 1] = 0;
        return 0;
    }
    for (i = 0; i < 2 && !ok; i++)
        ok = infsect(NULL, h, sections[i], SPINST_REGISTRY, NULL, NULL, 0,
                     NULL, NULL, NULL, NULL) ? 1 : 0;
    if (!ok) {
        _snprintf(why, cch - 1, "%s: no registration section applied (error %lu)",
                  inf, GetLastError());
        why[cch - 1] = 0;
    }
    closeinf(h);
    return ok;
}

/* Run - and remove - every RunOnce value that is a streamci registration.
 * Anything else queued there is left for Windows. Returns how many ran. */
static int gs_audio_run_streamci(int *failed)
{
    HKEY h;
    char name[256], cmd[1024];
    DWORD i, nlen, clen, type;
    int ran = 0;

    *failed = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, GS_RUNONCE, 0, KEY_READ | KEY_WRITE, &h) != ERROR_SUCCESS)
        return 0;
    /* Re-enumerate from 0 after each delete: deleting shifts the indices. */
    for (i = 0; ; ) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        nlen = sizeof(name);
        clen = sizeof(cmd) - 1;
        if (RegEnumValueA(h, i, name, &nlen, NULL, &type, (LPBYTE)cmd, &clen) != ERROR_SUCCESS)
            break;
        cmd[clen < sizeof(cmd) ? clen : sizeof(cmd) - 1] = 0;
        if (type != REG_SZ || !audiofix_is_streamci_cmd(cmd)) {
            i++;
            continue;
        }
        RegDeleteValueA(h, name);          /* RunOnce semantics: gone before it runs */
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
                GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            ran++;
            if (code != 0) {
                (*failed)++;
                log_msg(LOG_GS, "audio stack: %s exited %lu", name, (unsigned long)code);
            }
        } else {
            (*failed)++;
            log_msg(LOG_GS, "audio stack: could not start %s (error %lu)", name, GetLastError());
        }
    }
    RegCloseKey(h);
    return ran;
}

static void gs_audio_stack_check(void)
{
    char hwid[256], why[256];
    unsigned wave;
    DWORD attempts;
    int verdict, ran, failed, waited;

    if (GetVersion() & 0x80000000UL)
        return;                                        /* 9x: a different audio stack */
    wave = waveOutGetNumDevs();
    if (wave > 0)
        return;                                        /* the normal case: one call */
    attempts = gs_retro_dword(GS_AUDIOFIX_VALUE, 0);
    verdict = audiofix_decide(1, wave, gs_audio_hw_bound(hwid, sizeof(hwid)),
                              gs_key_exists(AUDIOFIX_SYSAUDIO_KEY), attempts);
    if (verdict == AUDIOFIX_NONE)
        return;
    if (verdict == AUDIOFIX_OTHER_FAULT) {
        log_msg(LOG_GS, "audio: %s has a driver but there is NO wave device, and the "
                "kernel audio stack IS registered - not the missing-registration fault; left alone",
                hwid);
        return;
    }
    if (verdict == AUDIOFIX_GAVE_UP) {
        log_msg(LOG_GS, "audio: %s has a driver but NO wave device and the kernel audio "
                "stack is still unregistered after %lu attempt(s) - NO SOUND on this box "
                "(HKLM\\Software\\RetroAgent\\%s=0 to try again)",
                hwid, (unsigned long)attempts, GS_AUDIOFIX_VALUE);
        return;
    }
    gs_retro_set_dword(GS_AUDIOFIX_VALUE, attempts + 1);
    log_msg(LOG_GS, "audio: %s has a driver but NO wave device - the kernel audio stack "
            "(sysaudio/kmixer/wdmaud) was never registered; re-running wdmaudio.inf's "
            "registration (attempt %lu of %d)", hwid, (unsigned long)attempts + 1,
            AUDIOFIX_MAX_ATTEMPTS);
    why[0] = 0;
    if (!gs_audio_rewrite_registration(why, sizeof(why))) {
        log_msg(LOG_GS, "audio: repair FAILED - %s", why);
        return;
    }
    ran = gs_audio_run_streamci(&failed);
    /* The software devices install server-side after the registrations land;
     * on the Dell that took about 12 s for all eight. */
    for (waited = 0; waited < 90 && waveOutGetNumDevs() == 0; waited += 3)
        Sleep(3000);
    wave = waveOutGetNumDevs();
    log_msg(LOG_GS, "audio: %d registration(s) run, %d failed; wave-out devices now %u%s",
            ran, failed, wave, wave ? "" : " - STILL NO SOUND");
    if (wave)
        gs_retro_set_dword(GS_AUDIOFIX_VALUE, 0);
}

static void gs_reclaim_drivers(void)
{
    __int64 before, bytes;
    int     bad, pending;

    if (!gs_file_exists(GS_DRIVER_DIR))
        return;

    bad = gs_devices_unconfigured();
    if (bad) {
        log_msg(LOG_GS, "%d device(s) still need a driver - KEEPING %s so the "
                        "Found New Hardware wizard can find them", bad,
                GS_DRIVER_DIR);
        return;
    }
    /* A device with no problem code is NOT the same as a device with the right
     * driver - see gs_prefs_pass(). Deleting the tree while a preference for
     * hardware this box actually has is still unsatisfied leaves the machine
     * with neither the driver nor the payload to fix itself, which is precisely
     * how .124 ended up on Microsoft's in-box nv4 with 2.4 GB of NVIDIA drivers
     * already deleted. Preferences for hardware this box does not have do not
     * count; only what applies here can block. */
    pending = gs_prefs_pass(0);
    if (pending) {
        log_msg(LOG_GS, "%d driver preference(s) for this machine not yet "
                        "satisfied - KEEPING %s", pending, GS_DRIVER_DIR);
        return;
    }
    before = gs_free_bytes("C:\\");
    bytes  = gs_dir_bytes(GS_DRIVER_DIR);
    log_msg(LOG_GS, "reclaiming the staged driver payload: %s is %I64d MB",
            GS_DRIVER_DIR, bytes / 1048576);
    if (gs_rmtree(GS_DRIVER_DIR)) {
        __int64 after = gs_free_bytes("C:\\");
        log_msg(LOG_GS, "driver payload removed - free space %I64d -> %I64d MB",
                before < 0 ? -1 : before / 1048576,
                after < 0 ? -1 : after / 1048576);
    } else {
        /* Partial removal is not a failure worth stopping for: whatever went is
         * still space we did not have, and the games copy is what matters. */
        log_msg(LOG_GS, "driver payload only partly removed - continuing");
    }
}

/* ---------------------------------------------------------------------- */
/* desktop shortcuts                                                       */
/* ---------------------------------------------------------------------- */

/* A game copied to C:\Games is not much use if nobody can find it. Each title
 * ships launch.txt at its root - one line, the executable relative to the title
 * directory, optionally followed by a tab and a display name - and we turn that
 * into a desktop shortcut as soon as the title lands.
 *
 * Why a shipped file rather than guessing: the right executable is genuinely
 * not guessable. Red Alert 2's launcher is game.exe and NOT the ra2.exe sitting
 * beside it; Jedi Knight MotS is JKM.EXE, not the GOGLauncher.exe that looks
 * more like a launcher; Shogo's Shogo.exe is a front end for Client.exe. A
 * heuristic gets these wrong quietly, and the failure only shows up when
 * someone double-clicks and nothing happens.
 *
 * ole32 is bound at run time, like gameindex.c does, so a box where COM is
 * unavailable degrades to "no shortcut" rather than failing to start. */

static const GUID GS_CLSID_ShellLink =
    { 0x00021401, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
static const GUID GS_IID_IShellLinkA =
    { 0x000214EE, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
static const GUID GS_IID_IPersistFile =
    { 0x0000010B, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };

typedef HRESULT (WINAPI *gs_coinit_t)(LPVOID);
typedef void    (WINAPI *gs_councoinit_t)(void);
typedef HRESULT (WINAPI *gs_cocreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT (WINAPI *gs_shgetfolder_t)(HWND, int, HANDLE, DWORD, LPSTR);

static HMODULE          g_gs_ole32;
static HMODULE          g_gs_shell32;
static gs_coinit_t      g_gs_CoInitialize;
static gs_councoinit_t  g_gs_CoUninitialize;
static gs_cocreate_t    g_gs_CoCreateInstance;
static gs_shgetfolder_t g_gs_SHGetFolderPathA;

#define GS_CSIDL_DESKTOPDIRECTORY 0x0010
#define GS_CSIDL_COMMON_DESKTOPDIRECTORY 0x0019

static int gs_ole_load(void)
{
    if (g_gs_ole32)
        return g_gs_CoCreateInstance != NULL;
    g_gs_ole32 = LoadLibraryA("ole32.dll");
    if (!g_gs_ole32)
        return 0;
    g_gs_CoInitialize     = (gs_coinit_t)GetProcAddress(g_gs_ole32, "CoInitialize");
    g_gs_CoUninitialize   = (gs_councoinit_t)GetProcAddress(g_gs_ole32, "CoUninitialize");
    g_gs_CoCreateInstance = (gs_cocreate_t)GetProcAddress(g_gs_ole32, "CoCreateInstance");
    g_gs_shell32 = LoadLibraryA("shell32.dll");
    if (g_gs_shell32)
        g_gs_SHGetFolderPathA =
            (gs_shgetfolder_t)GetProcAddress(g_gs_shell32, "SHGetFolderPathA");
    return g_gs_CoCreateInstance != NULL;
}

/* Where shortcuts go. Prefer the ALL USERS desktop so the icons survive a
 * different account logging in - these boxes autologon as Administrator today,
 * but that is a setting, not a law. Falls back to the per-user desktop, then to
 * the 9x-era fixed path. */
/* The logged-on user's own Desktop, as opposed to the All Users one. Both hold
 * shortcuts and a sweep of only one leaves half the clutter behind. */
static int gs_user_desktop_dir(char *out, DWORD cch)
{
    out[0] = 0;
    if (g_gs_SHGetFolderPathA &&
        SUCCEEDED(g_gs_SHGetFolderPathA(NULL, GS_CSIDL_DESKTOPDIRECTORY,
                                        NULL, 0, out)) && out[0])
        return 1;
    {
        char prof[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("USERPROFILE", prof, sizeof(prof));
        if (n > 0 && n < sizeof(prof)) {
            _snprintf(out, cch - 1, "%s\\Desktop", prof);
            out[cch - 1] = 0;
            if (gs_file_exists(out))
                return 1;
        }
    }
    out[0] = 0;
    return 0;
}

static int gs_desktop_dir(char *out, DWORD cch)
{
    if (g_gs_SHGetFolderPathA) {
        if (SUCCEEDED(g_gs_SHGetFolderPathA(NULL, GS_CSIDL_COMMON_DESKTOPDIRECTORY,
                                            NULL, 0, out)) && out[0])
            return 1;
        if (SUCCEEDED(g_gs_SHGetFolderPathA(NULL, GS_CSIDL_DESKTOPDIRECTORY,
                                            NULL, 0, out)) && out[0])
            return 1;
    }
    {
        char win[MAX_PATH];
        if (GetWindowsDirectoryA(win, sizeof(win))) {
            _snprintf(out, cch - 1, "%s\\Desktop", win);
            out[cch - 1] = 0;
            if (gs_file_exists(out))
                return 1;
        }
    }
    return 0;
}

/* Find real icon artwork for a shortcut whose target may be a .bat.
 *
 * A .bat has no icon resources at all, so a shortcut pointing its icon at one
 * gets the generic batch icon. Most staged titles now launch through a
 * `Play <Game>.bat` (disc mounting, per-box serials, fullscreen), so without
 * this the desktop is a wall of identical icons.
 *
 * Order, cheapest and most explicit first:
 *   1. an explicit third TAB-separated field in launch.txt   (caller supplies)
 *   2. the first .exe the .bat actually names that exists on disk - the game's
 *      own executable, which is exactly the artwork we want. ABOVE the .ico
 *      sweep on purpose: a shipped .ico is often support/notes artwork.
 *   3. an .ico sitting in the title's own directory, skipping obvious
 *      non-game names
 *   4. any .exe in the title's directory, longest name first as a weak proxy
 *      for "the game" over "setup"/"uninstall"
 *
 * Returns 1 and fills `out` on success, 0 to leave the shortcut's icon alone.
 */
/* Look for `name` in each immediate subdirectory of `dir`. Bounded to one
 * level on purpose: it covers every staged layout (Unreal's System\, id's
 * game dirs) without walking a 6 GB tree on a Pentium III. */
static int gs_find_in_subdir(const char *dir, const char *name,
                             char *out, size_t cap)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], cand[MAX_PATH];

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        _snprintf(cand, sizeof(cand) - 1, "%s\\%s\\%s", dir, fd.cFileName, name);
        cand[sizeof(cand) - 1] = 0;
        if (gs_file_exists(cand)) {
            lstrcpynA(out, cand, (int)cap);
            FindClose(h);
            return 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

static int gs_bat_names_exe(const char *bat, const char *dst_dir,
                            char *out, size_t cap)
{
    HANDLE h;
    char   buf[8192];
    DWORD  got = 0;
    char  *p;

    h = CreateFileA(bat, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) || !got) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[got] = 0;

    /* Walk every ".exe" in the file and take the first whose path resolves.
     * A token may be quoted, may be bare, and may be relative to the title
     * directory or to the .bat's own directory - both are tried. Comment lines
     * are skipped so a `rem` mentioning an exe cannot win over the real one. */
    for (p = buf; *p; p++) {
        char cand[MAX_PATH], full[MAX_PATH];
        char *s2, *e2;
        size_t n;

        if ((p[0] != 'e' && p[0] != 'E') ||
            _strnicmp(p, "exe", 3) != 0 || p == buf || p[-1] != '.')
            continue;

        /* walk back to the start of the token */
        s2 = p - 1;
        while (s2 > buf && s2[-1] != '"' && s2[-1] != ' ' && s2[-1] != '\t' &&
               s2[-1] != '\r' && s2[-1] != '\n' && s2[-1] != '=')
            s2--;
        e2 = p + 3;
        n = (size_t)(e2 - s2);
        if (n == 0 || n >= sizeof(cand))
            continue;
        lstrcpynA(cand, s2, (int)n + 1);

        /* skip anything on a rem/:: comment line */
        {
            char *ls = s2;
            while (ls > buf && ls[-1] != '\n')
                ls--;
            while (*ls == ' ' || *ls == '\t')
                ls++;
            if (_strnicmp(ls, "rem", 3) == 0 || (ls[0] == ':' && ls[1] == ':'))
                continue;
        }
        /* The shell and the EMULATOR are not the game. DOSBox matters as much
         * as cmd.exe here: every DOS title's launcher names it first, so
         * without this System Shock 1's shortcut claimed to be DOSBox. */
        if (_stricmp(cand, "cmd.exe") == 0 || _stricmp(cand, "start.exe") == 0 ||
            _strnicmp(cand, "dosbox", 6) == 0 ||
            _stricmp(cand, "reg.exe") == 0 || _stricmp(cand, "taskkill.exe") == 0 ||
            _stricmp(cand, "attrib.exe") == 0 || _stricmp(cand, "xcopy.exe") == 0 ||
            _stricmp(cand, "daemon.exe") == 0 ||
            _strnicmp(cand, "batchmnt", 8) == 0)
            continue;

        if (cand[1] == ':' || cand[0] == '\\') {          /* already absolute */
            if (gs_file_exists(cand)) {
                lstrcpynA(out, cand, (int)cap);
                return 1;
            }
            continue;
        }
        _snprintf(full, sizeof(full) - 1, "%s\\%s", dst_dir, cand);
        full[sizeof(full) - 1] = 0;
        if (gs_file_exists(full)) {
            lstrcpynA(out, full, (int)cap);
            return 1;
        }
        /* Not in the title root - and that is the COMMON case for an Unreal
         * Engine title, whose launcher does `cd /d "%~dp0System"` before
         * naming the exe, so the name is relative to the directory it changed
         * to. Unreal Tournament and Unreal Gold both looked like resolver
         * failures for exactly this reason: `UnrealTournament.exe` is real,
         * but it lives in System\.
         *
         * Rather than parse `cd` (a .bat can change directory several times,
         * conditionally), look for the named file one level down. One level is
         * enough for every staged tree and keeps this bounded on a P3. */
        if (gs_find_in_subdir(dst_dir, cand, out, cap))
            return 1;
    }
    return 0;
}

static int gs_resolve_icon(const char *dst_dir, const char *target,
                           char *out, size_t cap)
{
    const char *ext;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], best[MAX_PATH];
    int    bestlen = -1;

    out[0] = 0;
    ext = target + lstrlenA(target);
    while (ext > target && *ext != '.' && *ext != '\\')
        ext--;

    /* An .exe already carries its own artwork - nothing to resolve. */
    if (_stricmp(ext, ".exe") == 0 || _stricmp(ext, ".com") == 0)
        return 0;

    /* 2. the exe the .bat itself launches.
     *
     * This is deliberately ABOVE the .ico sweep. "Any .ico in the directory" is
     * a much weaker signal than it looks: Thief 2's only icon is `support.ico`
     * (a HELP icon) and Tiberian Sun ships `NOTES.ICO` beside `SUN.ICO`, so an
     * unfiltered .ico rule confidently picks the wrong artwork - and a wrong
     * icon is worse than a dull one, because it actively misleads. */
    if (gs_bat_names_exe(target, dst_dir, out, cap))
        return 1;

    /* 3. an .ico shipped in the title's directory, skipping the obvious
     *    non-game ones. Last resort among the specific rules. */
    _snprintf(pat, sizeof(pat) - 1, "%s\\*.ico", dst_dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (_strnicmp(fd.cFileName, "support", 7) == 0 ||
                _strnicmp(fd.cFileName, "notes",   5) == 0 ||
                _strnicmp(fd.cFileName, "readme",  6) == 0 ||
                _strnicmp(fd.cFileName, "help",    4) == 0 ||
                _strnicmp(fd.cFileName, "manual",  6) == 0 ||
                _strnicmp(fd.cFileName, "unins",   5) == 0 ||
                _strnicmp(fd.cFileName, "setup",   5) == 0)
                continue;
            _snprintf(out, cap - 1, "%s\\%s", dst_dir, fd.cFileName);
            out[cap - 1] = 0;
            FindClose(h);
            return 1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    /* 4. weakest: any exe in the title dir, longest name wins. Deliberately
     *    last - it is a guess, and setup/uninstall exes live here too. */
    best[0] = 0;
    _snprintf(pat, sizeof(pat) - 1, "%s\\*.exe", dst_dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int l = lstrlenA(fd.cFileName);
            if (_strnicmp(fd.cFileName, "unins", 5) == 0 ||
                _strnicmp(fd.cFileName, "setup", 5) == 0)
                continue;
            if (l > bestlen) {
                bestlen = l;
                lstrcpynA(best, fd.cFileName, sizeof(best));
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (best[0]) {
        _snprintf(out, cap - 1, "%s\\%s", dst_dir, best);
        out[cap - 1] = 0;
        return 1;
    }
    return 0;
}

static int gs_make_shortcut(const char *target, const char *workdir,
                            const char *lnk_path, const char *desc,
                            const char *icon)
{
    IShellLinkA  *sl = NULL;
    IPersistFile *pf = NULL;
    WCHAR         wpath[MAX_PATH];
    HRESULT       hr;
    int           ok = 0;

    if (!g_gs_CoCreateInstance)
        return 0;
    hr = g_gs_CoCreateInstance(&GS_CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                               &GS_IID_IShellLinkA, (void **)&sl);
    if (FAILED(hr) || !sl)
        return 0;

    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetWorkingDirectory(sl, workdir);
    /* The icon comes from the game's own artwork, so the desktop shows the game
     * rather than a row of identical generic icons.
     *
     * Pointing at `target` is right for an .exe, which carries its icons in its
     * resources - and WRONG for a .bat, which carries none, so Windows falls
     * back to the generic batch-file icon. That became the common case as more
     * titles moved to a `Play <Game>.bat` launcher to mount a disc, generate a
     * per-box serial or force fullscreen: the desktop filled up with identical
     * gear icons and you could not tell the games apart. gs_resolve_icon()
     * finds the real artwork; `icon` is empty only when it found none. */
    sl->lpVtbl->SetIconLocation(sl, (icon && icon[0]) ? icon : target, 0);
    if (desc && desc[0])
        sl->lpVtbl->SetDescription(sl, desc);

    hr = sl->lpVtbl->QueryInterface(sl, &GS_IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr) && pf) {
        /* The shortcut is written OVER the existing one (the desktop is no
         * longer swept first - see gs_sweep_unclaimed), and a CREATE_ALWAYS
         * over a read-only, hidden or system file fails. The sweep used to
         * clear those bits before moving the file away; clear them here so
         * the rewrite lands exactly where the old sweep-then-create did. */
        char  real[MAX_PATH];
        const char *f = gs_desk_written_path(lnk_path, real, sizeof(real));
        DWORD a = GetFileAttributesA(f);    /* the .pif, for a DOS target on 9x */
        if (a != 0xFFFFFFFF && (a & (FILE_ATTRIBUTE_READONLY |
                                     FILE_ATTRIBUTE_HIDDEN |
                                     FILE_ATTRIBUTE_SYSTEM)))
            SetFileAttributesA(f, FILE_ATTRIBUTE_NORMAL);
        MultiByteToWideChar(CP_ACP, 0, lnk_path, -1, wpath, MAX_PATH);
        if (SUCCEEDED(pf->lpVtbl->Save(pf, wpath, TRUE)))
            ok = 1;
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* desktop sweep + wallpaper staging                                       */
/* ---------------------------------------------------------------------- */

#define GS_DESK_BACKUP  "C:\\retro-desktop-backup"
#define GS_WALL_DIR     "C:\\retro-wall"

/*
 * Desktop shortcuts for the agent itself and the chat client.
 *
 * The desktop sweep removes everything it does not recognise, and these two are
 * the things an operator most wants to reach from a fleet box - so they have to
 * be put back deliberately rather than left to survive by accident.
 *
 * Run on EVERY agent start, not only on a fresh image: a box that is swept
 * today should still have them tomorrow, and a machine that never went through
 * the imaging process should get them too. Both are cheap no-ops when the
 * shortcut already exists and points at the same place - which, until 1.85.0,
 * nothing actually checked: every start rebuilt both through COM and resaved
 * them, making Explorer refresh the desktop. gs_lnk_points_at() now reads the
 * existing file first (see agent/shared/lnkcheck.h).
 *
 * The chat client is only given an icon if it is actually on the box. A
 * shortcut to something that is not there is worse than no shortcut: it looks
 * like a working feature until someone clicks it.
 */
typedef DWORD (WINAPI *gs_getlongpath_t)(LPCSTR, LPSTR, DWORD);

/* Does the .lnk at `lnk` already point at `exe`? Reads the file; no COM. The
 * path is tried as given and in its short and long forms, because
 * GetModuleFileName reports whatever form the agent was started with. */
static int gs_lnk_points_at(const char *lnk, const char *exe)
{
    static gs_getlongpath_t getlong;
    static int looked;
    unsigned char buf[8192];
    char   alt[MAX_PATH];
    DWORD  got = 0, n;
    HANDLE h;

    h = CreateFileA(lnk, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(h, buf, sizeof(buf), &got, NULL))
        got = 0;
    CloseHandle(h);
    if (!got)
        return 0;
    if (lnk_bytes_name_path(buf, got, exe))
        return 1;
    n = GetShortPathNameA(exe, alt, sizeof(alt));
    if (n && n < sizeof(alt) && lnk_bytes_name_path(buf, got, alt))
        return 1;
    /* GetLongPathNameA is Win98/2000+; resolved so a 95/NT4 kernel32 still
     * loads the agent. */
    if (!looked) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k)
            getlong = (gs_getlongpath_t)GetProcAddress(k, "GetLongPathNameA");
        looked = 1;
    }
    if (getlong) {
        n = getlong(exe, alt, sizeof(alt));
        if (n && n < sizeof(alt) && lnk_bytes_name_path(buf, got, alt))
            return 1;
    }
    return 0;
}

/* Does the .lnk at `lnk` name `icon` as its icon location? (agent 1.94.0) */
static int gs_lnk_has_icon(const char *lnk, const char *icon)
{
    unsigned char buf[8192];
    DWORD  got = 0;
    HANDLE h = CreateFileA(lnk, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(h, buf, sizeof(buf), &got, NULL))
        got = 0;
    CloseHandle(h);
    return got && (lnk_bytes_counted_wstr(buf, got, icon) ||
                   lnk_bytes_name_path(buf, got, icon));
}

/* `icon`: NULL = the exe's own icon. Otherwise an icon file that must exist,
 * and an existing shortcut is "already correct" only if it names that icon -
 * so a shortcut made before the icon changed is rewritten once. */
static void gs_tool_shortcut(const char *exe, const char *name, const char *icon)
{
    char desktop[MAX_PATH], lnk[MAX_PATH], workdir[MAX_PATH];
    char *slash;

    if (!gs_file_exists(exe))
        return;                        /* not installed here - not an error */
    /* Every other failure gets a line. The first version returned silently on
     * all of them, so a shortcut that was never placed looked exactly like one
     * that was placed successfully - and two machines went a full cycle without
     * their agent and chat icons while the log said nothing at all. */
    if (!gs_ole_load()) {
        log_msg(LOG_GS, "%s: no shell link support - no shortcut", name);
        return;
    }
    if (!gs_desktop_dir(desktop, sizeof(desktop))) {
        log_msg(LOG_GS, "%s: cannot locate the desktop - no shortcut", name);
        return;
    }
    _snprintf(lnk, sizeof(lnk) - 1, "%s\\%s.lnk", desktop, name);
    lnk[sizeof(lnk) - 1] = 0;

    /* Already there and pointing at this exe: nothing to do, and no COM -
     * but CLAIM it, or the end-of-run sweep takes the operator's own icons
     * away because this run never wrote them. */
    if (icon && !gs_file_exists(icon))
        icon = NULL;                   /* fall back to the exe's own icon */
    if (gs_lnk_points_at(lnk, exe) && (!icon || gs_lnk_has_icon(lnk, icon))) {
        gs_desk_note_lnk_kept(lnk);
        return;
    }

    lstrcpynA(workdir, exe, sizeof(workdir));
    slash = workdir + lstrlenA(workdir);
    while (slash > workdir && *slash != '\\')
        slash--;
    *slash = 0;

    if (gs_make_shortcut(exe, workdir, lnk, name, icon)) {
        log_msg(LOG_GS, "desktop shortcut -> %s", name);
        gs_desk_note_lnk_written(lnk);
    } else {
        log_msg(LOG_GS, "%s: could not create the shortcut", name);
    }
}

void gs_place_tool_shortcuts(void)
{
    char exe[MAX_PATH];
    DWORD n;
    int   we_initialised = 0;

    /* COM has to be initialised ON THIS THREAD before CoCreateInstance will
     * hand back a ShellLink. gs_run() does that around its own shortcut work,
     * which is why calling this from inside gs_run worked and calling it at
     * thread start did not: same code, same machine, and the only difference
     * was whether COM happened to be initialised by someone else first. The
     * failure was CO_E_NOTINITIALIZED and looked exactly like "the shortcut
     * could not be created".
     *
     * Initialising here makes the function work wherever it is called from. A
     * second CoInitialize on an already-initialised thread returns S_FALSE and
     * is harmless - but then we must NOT uninitialise, or we would tear down
     * the caller's apartment. */
    if (!gs_ole_load())
        return;
    if (g_gs_CoInitialize) {
        HRESULT hr = g_gs_CoInitialize(NULL);
        we_initialised = (hr == S_OK);
    }

    /* The agent's own path, whatever it is - these boxes are not consistent
     * about where it lives (a dual-boot machine can run it from the OTHER
     * volume), so asking Windows beats assuming C:\RETRO_AGENT. */
    n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n > 0 && n < sizeof(exe))
        gs_tool_shortcut(exe, "Retro Agent", NULL);

    gs_tool_shortcut("C:\\RETRO_AGENT\\retro_chat.exe", "Retro Chat", NULL);

    /* The 3dfx Control Panel (scripts/3dfx/3dfxctl), wearing the 3dfx logo.
     * fxpanel_ensure() copies it onto a box whose card it serves; everywhere
     * else the exe is absent and this is a no-op. Without this line the
     * desktop shortcut lasted only until the next sync's sweep - .124 lost it
     * to two quiet GAMESYNCs on 2026-09-29. The icon is a separate file with
     * a new name because XP caches icons by path (3dfxctl.exe's old icon). */
    gs_tool_shortcut("C:\\RETRO_AGENT\\3dfxctl.exe", "3dfx Control Panel",
                     "C:\\RETRO_AGENT\\3dfxlogo.ico");

    if (we_initialised && g_gs_CoUninitialize)
        g_gs_CoUninitialize();
}

/* --- the desktop icon set: sample, claim, sweep (agent/shared/deskset.h) --- */

static const char *gs_basename(const char *p)
{
    const char *b = p + lstrlenA(p);
    while (b > p && *(b - 1) != '\\')
        b--;
    return b;
}

static void gs_desk_scan_dir(const char *desk, unsigned where)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH];

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", desk);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        /* ds_add() keeps only .lnk/.pif/.url - the kinds the sweep takes -
         * and compares the WHOLE name, case-insensitively. */
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            ds_add(&g_gs_dset, fd.cFileName, where);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Sample the desktop BEFORE this run writes a single shortcut. Must be called
 * first in gs_run(), after gs_desk_reset(). It records both what the rebuild
 * gate compares against and the ONLY things the end-of-run sweep may remove. */
static void gs_desk_snapshot(void)
{
    char desk[MAX_PATH], userdesk[MAX_PATH];

    if (gs_desktop_dir(desk, sizeof(desk))) {
        lstrcpynA(g_gs_desk_common, desk, sizeof(g_gs_desk_common));
        gs_desk_scan_dir(desk, DS_COMMON);
    } else {
        desk[0] = 0;
    }
    if (gs_user_desktop_dir(userdesk, sizeof(userdesk)) &&
        lstrcmpiA(userdesk, desk) != 0) {
        lstrcpynA(g_gs_desk_user, userdesk, sizeof(g_gs_desk_user));
        gs_desk_scan_dir(userdesk, DS_USER);
    }
}

/* Which desktop does this .lnk path sit on? The agent writes to All Users, so
 * anything that is not the user's own desktop counts as that. */
static unsigned gs_desk_where(const char *lnk_path)
{
    char dir[MAX_PATH];
    const char *name = gs_basename(lnk_path);
    int n = (int)(name - lnk_path);

    if (n <= 1 || n > (int)sizeof(dir) || !g_gs_desk_user[0])
        return DS_COMMON;
    lstrcpynA(dir, lnk_path, n);            /* up to, not including, the '\' */
    return lstrcmpiA(dir, g_gs_desk_user) == 0 ? DS_USER : DS_COMMON;
}

/* The desktop file this shortcut really became: on Windows 9x a shortcut to
 * an MS-DOS program - every `Play <Game>.bat`, and a DOS .exe - is saved as
 * <name>.pif, not the <name>.lnk that was asked for (see ds_written_name). */
static const char *gs_desk_written_path(const char *lnk_path, char *buf, size_t cap)
{
    char pif[MAX_PATH];
    size_t n = strlen(lnk_path);
    int lnk = GetFileAttributesA(lnk_path) != INVALID_FILE_ATTRIBUTES, has_pif = 0;
    if (!lnk && n > 4 && n < sizeof(pif)) {
        memcpy(pif, lnk_path, n + 1);
        memcpy(pif + n - 4, ".pif", 5);
        has_pif = GetFileAttributesA(pif) != INVALID_FILE_ATTRIBUTES;
    }
    return ds_written_name(lnk_path, lnk, has_pif, buf, cap);
}

/* A shortcut was written. It is only a CHANGE if that icon was not on the
 * desktop when this run started - otherwise we have merely rewritten it in
 * place. Either way it is CLAIMED: the end-of-run sweep leaves it alone. */
static void gs_desk_note_lnk_written(const char *lnk_path)
{
    char real[MAX_PATH];
    lnk_path = gs_desk_written_path(lnk_path, real, sizeof(real));
    if (ds_claim(&g_gs_dset, gs_basename(lnk_path), gs_desk_where(lnk_path), 1))
        /* Publish the running total so GAMESYNC STATUS is meaningful DURING a
         * run, not only after gs_desk_settle_lnks(). Reading 0 mid-run when the
         * final answer is 81 is worse than useless. */
        g_gs_desk_lnks = g_gs_dset.added;
}

/* A shortcut was found already correct and deliberately NOT rewritten
 * (gs_tool_shortcut). Claimed - so the sweep keeps it - and never a change. */
static void gs_desk_note_lnk_kept(const char *lnk_path)
{
    char real[MAX_PATH];
    lnk_path = gs_desk_written_path(lnk_path, real, sizeof(real));
    ds_claim(&g_gs_dset, gs_basename(lnk_path), gs_desk_where(lnk_path), 0);
}

/* Net change in the icon set: icons added, plus icons that were there at the
 * start and really went. Call once, after the sweep (or instead of it). */
static void gs_desk_settle_lnks(void)
{
    g_gs_desk_lnks = ds_changed(&g_gs_dset);
}

/*
 * Clear the desktop of everything that is not one of ours - at the END of the
 * run, and only what this run did not put back.
 *
 * A provisioned box should show the staged games and nothing else - not the
 * leftovers of whatever was installed on it before, not vendor advertising, not
 * a dozen stale shortcuts to games that are no longer there.
 *
 * These are MOVED, not deleted. A desktop is where people leave things they
 * care about, and a shortcut we did not recognise is not automatically
 * worthless; C:\retro-desktop-backup keeps them, so a wrong judgement here
 * costs somebody a look in a folder rather than their work. Only .lnk, .pif and
 * .url go - real files someone left on the desktop are left exactly where they
 * are.
 *
 * WHY LAST (1.90.0). This used to run FIRST and take every icon away, the
 * run's own included, and each came back only when the copy loop reached its
 * title. .110 showed what that costs: a run starved of CPU by a game sat at
 * "enumerating library" for ~100 minutes and the box had two icons the whole
 * time; a run that failed (share unreachable), was aborted or was killed left
 * the desktop empty until some later run finished. Now it moves only entries
 * of the pre-run sample that the run neither rewrote nor confirmed - so no
 * icon the run is going to put back is ever missing - and gs_run() calls it
 * only when ds_run_may_sweep() says the run considered every title.
 */
static void gs_sweep_unclaimed(void)
{
    char     src[MAX_PATH], dst[MAX_PATH];
    int      i, moved = 0, failed = 0, made_dir = 0;
    unsigned bit;

    for (i = 0; i < g_gs_dset.n; i++) {
        ds_entry_t *e = &g_gs_dset.e[i];
        unsigned    todo = ds_sweep_bits(e);

        for (bit = DS_COMMON; bit <= DS_USER; bit <<= 1) {
            const char *desk = (bit == DS_COMMON) ? g_gs_desk_common
                                                  : g_gs_desk_user;
            if (!(todo & bit) || !desk[0])
                continue;
            /* A path that does not fit is never guessed at: left in place. */
            if (lstrlenA(desk) + 1 + lstrlenA(e->name) >= MAX_PATH ||
                lstrlenA(GS_DESK_BACKUP) + 1 + lstrlenA(e->name) >= MAX_PATH)
                continue;
            _snprintf(src, sizeof(src) - 1, "%s\\%s", desk, e->name);
            _snprintf(dst, sizeof(dst) - 1, "%s\\%s", GS_DESK_BACKUP, e->name);
            src[sizeof(src) - 1] = dst[sizeof(dst) - 1] = 0;
            if (!gs_file_exists(src)) {
                ds_mark_gone(e, bit);     /* somebody removed it mid-run */
                continue;
            }
            if (!made_dir) {
                CreateDirectoryA(GS_DESK_BACKUP, NULL);
                made_dir = 1;
            }
            SetFileAttributesA(src, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(dst);                  /* MoveFile will not overwrite */
            if (MoveFileA(src, dst) || DeleteFileA(src)) {
                ds_mark_gone(e, bit);
                moved++;
            } else {
                failed++;
            }
        }
    }
    if (moved)
        log_msg(LOG_GS, "desktop swept: %d shortcut(s) this run did not put "
                        "back moved to %s", moved, GS_DESK_BACKUP);
    if (failed)
        log_msg(LOG_GS, "desktop sweep: %d shortcut(s) could NOT be moved to %s "
                        "and are still on the desktop", failed, GS_DESK_BACKUP);
    if (g_gs_dset.overflow)
        log_msg(LOG_GS, "desktop sweep: the desktop held more than %d "
                        "shortcuts (or a name too long to hold) - the ones not "
                        "sampled were left where they are", DS_MAX);
    /* A move counts as a change only through gs_desk_settle_lnks(): an entry
     * whose every original copy went, and whose name this run did not put
     * back, is an icon that is gone. */
}

/*
 * Put the fleet wallpapers on a box that never saw the install image.
 *
 * The imaged machines get C:\retro-wall from $OEM$. A machine built by hand
 * never had that, so retrowall's apply step found nothing and quietly did
 * nothing - which is why two hand-built boxes sat on the default XP desktop
 * while every imaged one looked like the fleet. The wallpapers now live beside
 * the game library on the same share the agent already reads, so any box can
 * fetch them whether it was imaged or not.
 */
static void gs_stage_wallpapers(const char *library)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    int              n = 0;

    _snprintf(pat, sizeof(pat) - 1, "%s\\_desktop\\*", library);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;                                /* share has no _desktop - fine */
    CreateDirectoryA(GS_WALL_DIR, NULL);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        _snprintf(src, sizeof(src) - 1, "%s\\_desktop\\%s", library, fd.cFileName);
        _snprintf(dst, sizeof(dst) - 1, "%s\\%s", GS_WALL_DIR, fd.cFileName);
        src[sizeof(src) - 1] = dst[sizeof(dst) - 1] = 0;
        /* Same-size means already there: this runs on every provision and the
         * wallpapers are 26 MB. The source's size is the listing's - no need to
         * ask the NAS again for each file. */
        if (gs_file_size(dst) ==
            (((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow))
            continue;
        if (CopyFileA(src, dst, FALSE))
            n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n)
        log_msg(LOG_GS, "staged %d wallpaper file(s) into %s", n, GS_WALL_DIR);
}

/* Make ONE desktop shortcut from a "<relative exe>[<TAB><display name>]" line. */
/* ---------------------------------------------------------------------- */
/* the hardware capability gate                                            */
/* ---------------------------------------------------------------------- */
/*
 * WHY GAMESYNC GATES AT ALL. The fleet spans a 1999 Pentium III with a Voodoo
 * and a 2011 Sandy Bridge quad, and this copied the whole library onto every
 * one of them. A machine that cannot run a title still spent an hour of SMB1
 * bandwidth on it and then wore a desktop icon that launches into a black
 * screen. Skipping the copy is strictly better on both counts.
 *
 * TWO SOURCES OF TRUTH, IN THIS ORDER:
 *
 *  1. A verdict file the host published for THIS machine's hardware profile,
 *     at <library>\_gamegate\<profile_hash>.txt. It carries the host's richer
 *     decisions, including the ones a language model was consulted about for
 *     genuinely borderline cases. Keyed on the hardware hash rather than on an
 *     IP, so it survives a re-image and is shared by identical boxes.
 *
 *  2. Failing that, the DETERMINISTIC rules right here, against the title's own
 *     requires.json. This is not a nicety: a freshly PXE-imaged box syncs its
 *     games before any host tool has ever seen it, and it must still not put
 *     Doom 3 on a Pentium III.
 *
 * FAIL-OPEN, DELIBERATELY. No verdict file, no requires.json, an unparsable
 * one, an unclassifiable GPU - all deploy. The gate blocks only on positive
 * evidence. Fail-closed here would produce a box that silently receives no
 * games and says nothing about why, which is exactly the failure shape
 * CLAUDE.md's "make failure VISIBLE" section exists to prevent. Every skip is
 * logged with its limiting factor and the two numbers behind it.
 *
 * KILL SWITCH: HKLM\Software\RetroAgent\GameGate = 0 disables it entirely and
 * restores the old copy-everything behaviour.
 */

static gg_profile_t g_gate_profile;
static int          g_gate_ready;         /* profile built for this run */
static int          g_gate_on = 1;
static char         g_gate_hash[17];
static char        *g_gate_verdicts;      /* published file, heap, or NULL */
static int          g_gate_verdict_n;     /* rows the published file carries */
static int          g_gate_verdict_decl;  /* rows its header CLAIMS it carries */

static int gs_gate_enabled(void)
{
    HKEY  h;
    DWORD type, val = 1, size = sizeof(val);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0,
                      KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, "GameGate", NULL, &type, (BYTE *)&val, &size)
                != ERROR_SUCCESS || type != REG_DWORD)
            val = 1;
        RegCloseKey(h);
    }
    return val != 0;
}

/* Slurp a small text file onto the heap. NULL when absent or too big; a
 * requires.json or verdict file bigger than this is a mistake, not a file. */
static char *gs_slurp(const char *path, DWORD cap)
{
    HANDLE h;
    DWORD  size, got = 0;
    char  *buf;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > cap) {
        CloseHandle(h);
        return NULL;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    if (!ReadFile(h, buf, size, &got, NULL)) {
        CloseHandle(h);
        HeapFree(GetProcessHeap(), 0, buf);
        return NULL;
    }
    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

static void gs_gate_init(const char *library)
{
    char path[MAX_PATH];

    g_gate_ready = 0;
    g_gate_on = gs_gate_enabled();
    if (g_gate_verdicts) {
        HeapFree(GetProcessHeap(), 0, g_gate_verdicts);
        g_gate_verdicts = NULL;
    }
    if (!g_gate_on) {
        log_msg(LOG_GS, "capability gate DISABLED "
                        "(HKLM\\Software\\RetroAgent\\GameGate=0) - every "
                        "title will be copied");
        return;
    }

    hwprofile_build(&g_gate_profile);
    /* The hash is taken BEFORE free_mb is filled in, which costs nothing (the
     * field is not one of the hashed ten) but makes the ordering explicit:
     * the hash names the machine, and free space is not part of its identity.
     * The disk_mb floor is measured against the volume titles land on
     * (GamesDir, 1.93.0; C: without it); a negative return (the call failed)
     * stays 0 and fails open. */
    gg_profile_hash(&g_gate_profile, g_gate_hash);
    {
        char dest[160], root[4], why[200];
        __int64 fb;
        (void)gs_dest_resolve(dest, sizeof(dest), root, why, sizeof(why));
        fb = gs_free_bytes(root);
        g_gate_profile.free_mb = fb > 0 ? (unsigned)(fb / 1048576) : 0u;
    }
    g_gate_ready = 1;

    log_msg(LOG_GS, "gate: profile %s - %s %u MHz x%u, %u MB RAM, "
                    "gpu %04X:%04X %u MB (%s), os %s, %u MB free on C:",
            g_gate_hash, g_gate_profile.cpu_vendor, g_gate_profile.cpu_mhz,
            g_gate_profile.cpu_count, g_gate_profile.ram_mb,
            g_gate_profile.gpu_ven, g_gate_profile.gpu_dev,
            g_gate_profile.vram_mb,
            gg_gpu_level_name(g_gate_profile.gpu_level),
            gg_os_level_name(g_gate_profile.os_level),
            g_gate_profile.free_mb);

    _snprintf(path, sizeof(path) - 1, "%s\\_gamegate\\%s.txt",
              library, g_gate_hash);
    path[sizeof(path) - 1] = 0;
    g_gate_verdicts = gs_slurp(path, 256u * 1024u);
    g_gate_verdict_n = gg_verdict_count(g_gate_verdicts);
    g_gate_verdict_decl = gg_verdict_declared(g_gate_verdicts);
    if (!g_gate_verdicts) {
        log_msg(LOG_GS, "gate: published verdicts not published for this "
                        "profile (%s)", path);
    } else {
        /* SAY HOW MANY. A one-title file overwrote the full one on seven boxes
         * and read as healthy because it was well formed; a count is what makes
         * that visible to the next person reading a log. */
        log_msg(LOG_GS, "gate: published verdicts loaded - %d verdict(s)%s (%s)",
                g_gate_verdict_n,
                (g_gate_verdict_decl && g_gate_verdict_decl != g_gate_verdict_n)
                    ? " - HEADER COUNT MISMATCH" : "",
                path);
        if (g_gate_verdict_decl && g_gate_verdict_decl != g_gate_verdict_n)
            log_msg(LOG_GS, "gate: WARNING published file claims titles=%d but "
                            "carries %d row(s) - it was truncated or partly "
                            "rewritten; local rules still apply",
                    g_gate_verdict_decl, g_gate_verdict_n);
    }
}

static void gs_gate_free(void)
{
    if (g_gate_verdicts) {
        HeapFree(GetProcessHeap(), 0, g_gate_verdicts);
        g_gate_verdicts = NULL;
    }
    g_gate_ready = 0;
}

/* Look one title up in the published file. Returns GG_V_* or -1 for "no line".
 * Works on a COPY of each line because gg_verdict_parse splits in place and
 * the buffer is reused for every title. */
static int gs_gate_published(const char *title, char *why, DWORD why_cch)
{
    const char *p = g_gate_verdicts;

    if (why && why_cch)
        why[0] = 0;
    if (!p)
        return -1;
    while (*p) {
        char  line[512], *t, *lim, *reason;
        DWORD n = 0;
        int   v;

        while (*p && *p != '\n' && n < sizeof(line) - 1)
            line[n++] = *p++;
        line[n] = 0;
        while (*p && *p != '\n')
            p++;                        /* skip an over-long remainder */
        if (*p == '\n')
            p++;

        v = gg_verdict_parse(line, &t, &lim, &reason);
        if (v < 0 || !t || lstrcmpiA(t, title) != 0)
            continue;
        if (why && why_cch) {
            _snprintf(why, why_cch - 1, "%s%s%s",
                      lim && lim[0] && lstrcmpA(lim, "-") ? lim : "",
                      (lim && lim[0] && lstrcmpA(lim, "-") && reason
                       && reason[0]) ? ": " : "",
                      reason && reason[0] ? reason : "");
            why[why_cch - 1] = 0;
        }
        return v;
    }
    return -1;
}

/* Read a title's requires.json. `root` is either the library (before the copy)
 * or the deployed tree (after it) - the file is copied with the title, so
 * shortcut gating works on a box with the share unreachable. */
static char *gs_gate_requires(const char *root, const char *title)
{
    char path[MAX_PATH];
    if (title && *title)
        _snprintf(path, sizeof(path) - 1, "%s\\%s\\requires.json", root, title);
    else
        _snprintf(path, sizeof(path) - 1, "%s\\requires.json", root);
    path[sizeof(path) - 1] = 0;
    return gs_slurp(path, 64u * 1024u);
}

/*
 * Was a refusal limited by DISK rather than by capability?  `why` opens with
 * the limiting factor (see gs_gate_published / gs_gate_allows_title).
 *
 * WHY THIS EXISTS. The gate's disk test compares the title's DECLARED
 * `disk_mb` against a `free_mb` sampled before the run, and it gives no credit
 * for a copy of the title already sitting on the volume. GAMESYNC's own room
 * check, further down gs_run(), asks the same question far better: the tree's
 * REAL measured size, the CURRENT free space, and the space an installed copy
 * is about to give back. So a `disk` verdict is deferred to that check rather
 * than being final, and whatever it decides is counted as skipped_titles.
 *
 * `skipped` ("did not fit") and `gated` ("this machine cannot run it") are
 * deliberately different counters with different follow-ups, and collapsing
 * them has now misreported two boxes: .243 was told a Pentium 1 cannot RUN
 * Warcraft II (13 of its 22 "gated" titles were merely too big for a 604 MB
 * volume), and .240 read `deploy=gated, runs=verified` for a FarCry that was
 * installed on that very disk and had been played - the gate `continue`d
 * twenty lines above the credit block that exists to prevent exactly that, so
 * a large installed title could never be patched once the disk filled. That is
 * the UnrealTournament-436 incident the credit block's own comment describes,
 * live again.
 */
static int gs_gate_limited_by_disk(const char *why)
{
    if (!why)
        return 0;
    if ((why[0] != 'd' && why[0] != 'D') || (why[1] != 'i' && why[1] != 'I')
        || (why[2] != 's' && why[2] != 'S') || (why[3] != 'k' && why[3] != 'K'))
        return 0;
    return why[4] == 0 || why[4] == ':';
}

/*
 * Should this TITLE be copied? Fills `why` with a human sentence either way.
 * Returns 1 to copy, 0 to skip.
 */
static int gs_gate_allows_title(const char *library, const char *title,
                                char *why, DWORD why_cch)
{
    char *json;
    gg_req_t r;
    gg_decision_t d;
    int v;

    if (why && why_cch)
        why[0] = 0;
    if (!g_gate_on || !g_gate_ready)
        return 1;

    /* The host's verdict wins: it saw the same rules plus, for the borderline
     * band, a model that was given the title's notes. */
    v = gs_gate_published(title, why, why_cch);
    if (v >= 0)
        return v != GG_V_NO;

    json = gs_gate_requires(library, title);
    if (!json)
        return 1;                       /* no declaration - not gated */
    gg_req_parse(json, &r);
    gg_decide(&g_gate_profile, &r, &d);
    HeapFree(GetProcessHeap(), 0, json);

    if (why && why_cch) {
        _snprintf(why, why_cch - 1, "%s: %s", d.limiting[0] ? d.limiting : "-",
                  d.reason);
        why[why_cch - 1] = 0;
    }
    return d.verdict != GG_V_NO;
}

/*
 * Should this ONE SHORTCUT be created? `dst_dir` is the deployed tree and
 * `target` the launch.txt first column.
 *
 * Two different reasons to say no, and they are NOT the same fact:
 *   - a hardware verdict of NO for that shortcut specifically;
 *   - a missing CAPABILITY, which is software state and remediable. The title
 *     still deployed; the shortcut is suppressed and the log names the fix.
 *     GAMESYNC does NOT re-run every boot - gamesync.done idles the startup
 *     thread on a provisioned box - so it does NOT come back by itself.
 *     Run GAMESYNC RESET then GAMESYNC START.
 */
static int gs_gate_allows_shortcut(const char *dst_dir, const char *title,
                                   const char *target, char *why,
                                   DWORD why_cch)
{
    char *json;
    gg_req_t r;
    gg_decision_t d;
    int ok = 1;

    if (why && why_cch)
        why[0] = 0;
    if (!g_gate_on || !g_gate_ready)
        return 1;

    json = gs_gate_requires(dst_dir, NULL);
    if (!json)
        return 1;
    gg_req_parse_shortcut(json, target, &r);
    gg_decide(&g_gate_profile, &r, &d);
    if (d.verdict == GG_V_NO) {
        /* The host's published TITLE verdict wins here too, as it does for
         * the copy - unless the "no" comes from this shortcut's own block
         * (gg_shortcut_no_stands, agent/shared/gamegate.h). Without this an
         * approved title copied and then lost its only icon (.124 Halo,
         * 2026-09-28). */
        gg_req_t       rt;
        gg_decision_t  td;
        int            pub = gs_gate_published(title, NULL, 0);

        gg_req_parse(json, &rt);
        gg_decide(&g_gate_profile, &rt, &td);
        if (!gg_shortcut_no_stands(pub, &td, &d)) {
            log_msg(LOG_GS, "%s: \"%s\" - local rules say no (%s: %s), the "
                            "host's published verdict says %s - shortcut kept",
                    title, target, d.limiting[0] ? d.limiting : "-", d.reason,
                    pub == GG_V_RUN ? "run" : "marginal");
            d.verdict = pub;
        }
    }
    HeapFree(GetProcessHeap(), 0, json);

    if (d.missing_caps) {
        unsigned bit;
        for (bit = 1; bit; bit <<= 1) {
            if (!(d.missing_caps & bit))
                continue;
            if (why && why_cch) {
                _snprintf(why, why_cch - 1, "needs %s - %s",
                          gg_capability_name(bit), gg_capability_remedy(bit));
                why[why_cch - 1] = 0;
            }
            break;
        }
        ok = 0;
    } else if (d.verdict == GG_V_NO && !gs_gate_limited_by_disk(d.limiting)) {
        if (why && why_cch) {
            _snprintf(why, why_cch - 1, "%s: %s",
                      d.limiting[0] ? d.limiting : "-", d.reason);
            why[why_cch - 1] = 0;
        }
        ok = 0;
    }
    /*
     * A `disk` verdict MUST NOT suppress a shortcut, and this is the same
     * fix gs_gate_allows_title() already carries, at the second call site it
     * was never applied to.
     *
     * disk_mb answers "is it worth an hour of SMB1 bandwidth copying this
     * tree?", not "can this machine run it" - and by the time we are here the
     * tree is ON THE DISK and gs_file_exists(target) has just confirmed the
     * launcher itself is present. Suppressing then takes the icon off a game
     * that is installed and works.
     *
     * Measured on .240 2026-08-31: Far Cry occupies 3609 MB of that box's
     * 76 GB volume, leaving 1492 MB free against its declared disk_mb of
     * 3700. The title gate correctly deferred and the tree copied, then this
     * function logged
     *     FarCry: SHORTCUT SUPPRESSED "Far Cry" (Play Far Cry.bat)
     *             - disk: not enough free disk (have 1492 MB, needs 3700)
     * and the box was left with an installed, previously VERIFIED Far Cry and
     * no way to start it. `titles_gated` read 0, so the summary line said
     * nothing was gated at all - the one trace was that single log line.
     */
    return ok;
}

static void gs_shortcut_from_line(const char *dst_dir, const char *title,
                                  char *line)
{
    char exe_rel[MAX_PATH], disp[128], target[MAX_PATH];
    char desktop[MAX_PATH], lnk[MAX_PATH], workdir[MAX_PATH];
    char icon_rel[MAX_PATH], icon[MAX_PATH];
    char *tab, *slash;

    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line || *line == '#')
        return;

    disp[0] = 0;
    icon_rel[0] = 0;
    tab = line;
    while (*tab && *tab != '\t')
        tab++;
    if (*tab == '\t') {
        char *tab2;
        *tab = 0;
        tab2 = tab + 1;
        while (*tab2 && *tab2 != '\t')
            tab2++;
        if (*tab2 == '\t') {          /* optional THIRD field: the icon */
            *tab2 = 0;
            lstrcpynA(icon_rel, tab2 + 1, sizeof(icon_rel));
        }
        lstrcpynA(disp, tab + 1, sizeof(disp));
    }
    lstrcpynA(exe_rel, line, sizeof(exe_rel));
    if (!exe_rel[0])
        return;
    if (!disp[0])
        lstrcpynA(disp, title, sizeof(disp));

    _snprintf(target, sizeof(target) - 1, "%s\\%s", dst_dir, exe_rel);
    target[sizeof(target) - 1] = 0;
    if (!gs_file_exists(target)) {
        log_msg(LOG_GS, "%s: launch.txt names %s but it is not there - "
                        "no shortcut", title, exe_rel);
        return;
    }
    /* Per-shortcut gate. A title's halves do not always need the same machine:
     * Battlefield 1942's single player wants a mounted disc while its LAN
     * launchers check neither disc nor CD key, so gating the whole title on
     * the harder half would take working multiplayer off most of the fleet. */
    {
        char why[192];
        if (!gs_gate_allows_shortcut(dst_dir, title, exe_rel, why,
                                     sizeof(why))) {
            log_msg(LOG_GS, "%s: SHORTCUT SUPPRESSED \"%s\" (%s) - %s",
                    title, disp, exe_rel, why);
            return;
        }
    }
    /* Working directory is the exe's own folder: many of these games look for
     * their data relative to the current directory and start in a broken state
     * if launched from elsewhere. */
    lstrcpynA(workdir, target, sizeof(workdir));
    slash = workdir + lstrlenA(workdir);
    while (slash > workdir && *slash != '\\')
        slash--;
    *slash = 0;

    if (!gs_ole_load() || !gs_desktop_dir(desktop, sizeof(desktop)))
        return;
    _snprintf(lnk, sizeof(lnk) - 1, "%s\\%s.lnk", desktop, disp);
    lnk[sizeof(lnk) - 1] = 0;

    /* Icon: an explicit third launch.txt field wins, because only the library
     * can know which artwork belongs to which of a title's several launchers -
     * Red Alert 2 ships both the game and Yuri's Revenge, and auto-detection
     * cannot tell them apart. Otherwise resolve it from the tree. */
    icon[0] = 0;
    if (icon_rel[0]) {
        _snprintf(icon, sizeof(icon) - 1, "%s\\%s", dst_dir, icon_rel);
        icon[sizeof(icon) - 1] = 0;
        if (!gs_file_exists(icon)) {
            log_msg(LOG_GS, "%s: launch.txt icon %s is not there - resolving",
                    title, icon_rel);
            icon[0] = 0;
        }
    }
    if (!icon[0])
        gs_resolve_icon(dst_dir, target, icon, sizeof(icon));

    /* Was this link already on the desktop? gs_make_shortcut() rewrites it
     * every pass, so counting writes would be true on every run and would
     * measure nothing. Only a link that was NOT there before changes the set
     * of icons, and only that is worth an arrange. */
    if (gs_make_shortcut(target, workdir, lnk, disp, icon)) {
        log_msg(LOG_GS, "%s: desktop shortcut -> %s (icon: %s)", title,
                exe_rel, icon[0] ? icon : "from target");
        gs_desk_note_lnk_written(lnk);
    } else {
        log_msg(LOG_GS, "%s: could not create desktop shortcut", title);
    }
}

/*
 * Make a desktop shortcut for EVERY line in launch.txt.
 *
 * It used to read only the first line, which quietly cost us the second half of
 * several titles: Red Alert 2's tree already contains Yuri's Revenge (RA2MD.exe
 * and the expandmd mixes), and Descent II ships a Glide build alongside the
 * plain Windows one. All present on disk, none reachable from the desktop.
 *
 * Blank lines and lines starting with # are skipped, so a launch.txt can
 * explain itself. A line naming a missing exe is logged and skipped rather than
 * aborting the rest - one broken entry must not cost a title its other
 * shortcuts.
 */
static void gs_make_game_shortcut(const char *dst_dir, const char *title)
{
    char   path[MAX_PATH];
    HANDLE h;
    char   buf[1024];
    DWORD  got = 0;
    char  *line, *end;

    _snprintf(path, sizeof(path) - 1, "%s\\launch.txt", dst_dir);
    path[sizeof(path) - 1] = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;                       /* no launch.txt - nothing to point at */
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) || !got) {
        CloseHandle(h);
        return;
    }
    CloseHandle(h);
    buf[got] = 0;

    line = buf;
    while (*line) {
        end = line;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        if (*end) {
            *end = 0;
            end++;
            /* step over the LF of a CRLF so the next line does not start on it */
            while (*end == '\r' || *end == '\n')
                end++;
        }
        gs_shortcut_from_line(dst_dir, title, line);
        line = end;
    }
}

/*
 * A title this run is NOT going to copy may still be INSTALLED on the box - and
 * the end-of-run sweep (gs_sweep_unclaimed) takes away every icon this run did
 * not claim. Without this, a game that is sitting on the disk and runs
 * perfectly loses its shortcuts on the first sync that gates or skips it, and
 * never gets them back, because the only other call to gs_make_game_shortcut()
 * is inside the copy branch. (Until 1.90.0 the sweep ran FIRST, so these icons
 * were missing from the start of the run until this call - see deskset.h.)
 *
 * THIS IS WHAT "I DON'T SEE ANY GAMES ON THE DESKTOP" TURNED OUT TO BE on .243
 * (2026-08-31). The engine index had found `c:\games\HexenII` installed on that
 * machine at 14:25; an hour later the desktop carried Quake and nothing else.
 * The games were on the box. Their icons were in C:\retro-desktop-backup.
 *
 * Safe for a GATED title too, and deliberately so: gs_shortcut_from_line() asks
 * the gate again PER SHORTCUT, and gg_req_parse_shortcut() overlays a
 * shortcut's own requirements on top of the title's - so a title-level hard NO
 * still suppresses every one of its icons, while a per-shortcut floor
 * suppresses only the icon that fails it. On a box with no 3D, Hexen II keeps
 * its software-renderer shortcut and loses the three OpenGL ones, which is
 * exactly the intended behaviour.
 */
static void gs_restore_shortcuts_if_installed(const char *title)
{
    char have[MAX_PATH];

    _snprintf(have, sizeof(have) - 1, "%s\\%s", g_gs_dest, title);
    have[sizeof(have) - 1] = 0;
    if (!gs_file_exists(have))
        return;
    log_msg(LOG_GS, "%s is installed but not copied this run - keeping its "
                    "desktop shortcut(s)", title);
    gs_make_game_shortcut(have, title);
}

/* ---------------------------------------------------------------------- */
/* desktop icon layout                                                     */
/* ---------------------------------------------------------------------- */

/* AUTO-ARRANGE IS THE FLEET DEFAULT (v1.72.0). READ THIS BEFORE CHANGING IT.
 *
 * The user's requirement is simple and absolute: "the icons are always auto
 * arranged". Windows' own Auto Arrange is the only mechanism that delivers
 * that, because the shell re-packs the desktop ITSELF on every event that
 * scatters icons - a resolution change, a fullscreen game exiting, a new
 * shortcut appearing, an Explorer restart. No agent, however often it runs,
 * can win that race; it can only tidy up afterwards, which is precisely the
 * "the icons keep moving" complaint this replaces.
 *
 * WHAT THIS COSTS: the icon bay. gs_icon_bay()/gs_arrange_cols() below place
 * each icon in a cell the wallpaper draws, to keep icons off the dossier art.
 * With Auto Arrange ON the shell packs icons into its own grid from the
 * top-left and IGNORES LVM_SETITEMPOSITION outright, so the bay cannot work at
 * the same time. The two are mutually exclusive, and pretending otherwise is
 * how this feature has broken before - two mechanisms silently fighting, each
 * correct on its own, with the last one to run winning.
 *
 * So exactly one of them runs, chosen by an explicit switch rather than by
 * accident:
 *
 *     HKLM\Software\RetroAgent\IconAutoArrange  (REG_DWORD)
 *         absent or 1  -> Auto Arrange ON, shell owns the layout  (DEFAULT)
 *         0            -> legacy icon bay, agent owns the layout
 *
 * The bay code is kept, not deleted, because that switch has to lead somewhere
 * - and because the wallpaper still draws the bay, so a box that wants the art
 * respected can have it back with one REGWRITE.
 *
 * IT IS A SET, NOT A TOGGLE. FCIDM_SHVIEW_AUTOARRANGE is a WM_COMMAND toggle.
 * Firing it blindly turns Auto Arrange OFF on a box that already had it on -
 * the exact inverse of the bug that used to leave icons in rows across the top
 * of the screen. So: read LVS_AUTOARRANGE first, post the toggle ONLY when the
 * bit is clear (i.e. only when the toggle moves it the way we want), then read
 * the bit back and log what actually happened. Never trust the call.
 *
 * WHY THE TOGGLE AND NOT JUST SetWindowLong. The WM_COMMAND goes through the
 * shell view's own handler, which updates Explorer's internal FOLDERSETTINGS
 * as well as the listview style - and it is that internal state Explorer
 * writes to the registry at logoff. SetWindowLongA(GWL_STYLE) changes only the
 * window, so the shell would write the old value back over us. It is therefore
 * the FALLBACK, for the case seen on .143 where the toggle did not take at all.
 *
 * PERSISTENCE. The desktop's view state lives in its shell bag
 * HKCU\Software\Microsoft\Windows\Shell\Bags\<slot>\Desktop, where <slot> is
 * the NodeSlot value on HKCU\...\Shell\BagMRU (the namespace root = the
 * desktop). It is 1 on every XP box measured and 4 on Win7's .195 - where this
 * code wrote to a hardcoded Bags\1 that nothing read (agent/shared/deskview.h,
 * gs_desktop_bag()). Value FFlags is the
 * FOLDERFLAGS word, in which bit 0 (FWF_AUTOARRANGE) is this setting and bit 2
 * (FWF_SNAPTOGRID) is "align to grid". Measured on .143 before this change:
 * FFlags = 0x220 (FWF_DESKTOP|FWF_NOCLIENTEDGE), both bits clear, exactly what
 * the agent had been forcing. We OR in bit 0 so a reboot comes up arranged
 * even before the agent's startup pass runs. That write is a BACKSTOP, not the
 * guarantee: Explorer may rewrite the bag from its own state, which is why the
 * live pass re-runs on EVERY agent startup (retrowall.c), the same way the
 * wallpaper and theme do.
 *
 * ALIGN-TO-GRID IS LEFT ALONE in auto-arrange mode. The agent used to clear
 * LVS_EX_SNAPTOGRID because its 103px row pitch walked icons out of the bay's
 * 80px cells (A/B-verified on .246). With the shell doing the packing that no
 * longer applies to anything, so clearing it would be churn against a setting
 * we no longer care about. It is still cleared in bay mode, where it still
 * matters.
 */

/* Park the desktop icons in the wallpaper's icon bay.
 *
 * The wallpaper (scripts/retro-wallpaper/gen_retro_wall.py) draws a visible
 * slot for every icon position. THE GEOMETRY BELOW MUST MATCH ITS icon_bay()
 * EXACTLY - if the two drift, icons land between slots and the whole point of
 * the design is lost. tests/native/test_icon_bay.c pins them together.
 *
 * That drift is not hypothetical: the previous wallpaper reserved a well in the
 * bottom-LEFT while arrange_icons.exe parked icons in the bottom-RIGHT, so the
 * art and the icons sat on top of each other.
 *
 * Doing this in the agent rather than a staged helper exe matters on a fresh
 * image, where C:\retro-wall does not exist yet - a machine gets a tidy desktop
 * on its first boot rather than after someone remembers to stage a tool. */

#define LVM_FIRST_           0x1000
#define LVM_GETITEMCOUNT_    (LVM_FIRST_ + 4)
#define LVM_ARRANGE_         (LVM_FIRST_ + 22)
#define LVM_SETITEMPOSITION_ (LVM_FIRST_ + 15)
#define LVM_SETEXSTYLE_      (LVM_FIRST_ + 54)
#define LVM_GETEXSTYLE_      (LVM_FIRST_ + 55)
#define LVA_DEFAULT_         0x0000
#define LVM_SETVIEW_         (LVM_FIRST_ + 142)   /* comctl32 6 */
#define LVM_GETVIEW_         (LVM_FIRST_ + 143)   /* comctl32 6 */
/* The shell's "Auto Arrange" menu command. NOT one constant, and NOT one per
 * platform family either: read from each OS's own SHELL32 menu resources -
 *     Win98 SE shell32 4.72:  0x7041 "&Auto Arrange"   (0x7051 is "&Help Topics")
 *     XP SP3 shell32 6.0:     0x7051 "&Auto Arrange"   (0x7071 is "Help and Support")
 *     Win7 6.1 shell32.mui:   0x7071 "&Auto arrange"   (0x7051 is "&List" !)
 * This used to be a single 0x7031, which is no menu command on EITHER, so the
 * toggle was a silent no-op everywhere - the "shell toggle silently failed on
 * .171 and .143" in CLAUDE.md. Its replacement (1.84.1) split only 9x from NT
 * and posted XP's 0x7051 on Windows 7, where it is the "List" view command: on
 * every boot that found auto-arrange clear it put .195's desktop into List
 * view (8 of 117 icons visible), logged "shell toggle did not take", set the
 * style bit and reported success. So the table lives in deskview.h keyed on
 * the exact version, and a version nobody has read the menus of gets 0 - post
 * NOTHING, use the style bit. Measured 2026-09-28 (Win7 .195, XP .110). */
static void gs_winver(int *is9x, unsigned *major, unsigned *minor)
{
    DWORD v = GetVersion();
    *is9x  = (v & 0x80000000UL) ? 1 : 0;
    *major = (unsigned)LOBYTE(LOWORD(v));
    *minor = (unsigned)HIBYTE(LOWORD(v));
}
static WPARAM gs_autoarrange_cmd(void)
{
    int is9x; unsigned major, minor;
    gs_winver(&is9x, &major, &minor);
    return (WPARAM)dv_autoarrange_cmd(is9x, major, minor);
}
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif
#ifndef LVS_EX_SNAPTOGRID
#define LVS_EX_SNAPTOGRID 0x00080000
#endif

/* The persisted desktop view state lives in the bag BagMRU's NodeSlot names -
 * see gs_desktop_bag(). There is deliberately no fixed path here any more.
 * The two FOLDERFLAGS bits we care about are the shell's own values - do not
 * renumber. */
#define GS_FWF_AUTOARRANGE DV_FWF_AUTOARRANGE
#define GS_FWF_SNAPTOGRID  DV_FWF_SNAPTOGRID
#define GS_ICON_KEY      "Software\\RetroAgent"
#define GS_ICON_VALUE    "IconAutoArrange"

typedef struct { int x, y, cell_w, cell_h, cols, rows; } gs_bay_t;

/* Mirror of icon_bay() in gen_retro_wall.py. Keep the arithmetic identical. */
static void gs_icon_bay(int w, int h, gs_bay_t *b)
{
    int margin_x = (int)(w * 0.018);
    int margin_y = (int)(h * 0.030);
    const int header_h = 34;

    b->cell_w = 76;
    b->cell_h = 80;
    if (margin_x < 18) margin_x = 18;
    if (margin_y < 18) margin_y = 18;
    b->cols = (int)((w * 0.34) / b->cell_w);
    if (b->cols < 2) b->cols = 2;
    b->rows = (h - margin_y - header_h - 24) / b->cell_h;
    if (b->rows < 3) b->rows = 3;
    b->x = margin_x;
    b->y = margin_y + header_h;
}

/* How many columns to actually use.
 *
 * The bay is a DRAWN panel: cols x rows cells, sized so the art has room. When
 * the library outgrows it, gs_arrange_bay used to keep packing DOWNWARD past
 * the last drawn row - which is fine on a big screen and silently loses icons
 * on a small one. At 1024x768 the bay is 4x8 = 32 slots; the staged library is
 * now 31 titles = 65 shortcuts, so rows 9 and beyond land below y=768 and those
 * icons cannot be clicked at all. Measured on .143, which is exactly that box.
 *
 * So on overflow, widen instead of lengthening: keep the bay's row count (the
 * screen decides that) and add whatever columns are needed, bounded by what
 * fits across the screen. The extra columns spill outside the drawn panel,
 * which is not pretty - but an icon beside the art beats an icon nobody can
 * reach, and the alternative is a desktop that silently hides half the games.
 *
 * No overflow means no change: at 1920x1080 the bay is 8x12 = 96 slots and 67
 * icons still land in exactly the cells the wallpaper drew.
 */
static int gs_arrange_cols(const gs_bay_t *bay, int screen_w, int count)
{
    int need, maxcols;

    if (count <= bay->cols * bay->rows)
        return bay->cols;

    need = (count + bay->rows - 1) / bay->rows;   /* cols to fit in bay.rows */
    maxcols = (screen_w - bay->x) / bay->cell_w;  /* what the screen allows */
    if (maxcols < 1)
        maxcols = 1;
    if (need > maxcols)
        need = maxcols;
    if (need < bay->cols)
        need = bay->cols;                          /* never narrow the bay */
    return need;
}

static HWND gs_desktop_listview(HWND *defview_out)
{
    HWND prog = FindWindowA("Progman", NULL);
    HWND defview = FindWindowExA(prog, NULL, "SHELLDLL_DefView", NULL);
    if (!defview) {
        HWND worker = NULL;
        while ((worker = FindWindowExA(NULL, worker, "WorkerW", NULL)) != NULL) {
            defview = FindWindowExA(worker, NULL, "SHELLDLL_DefView", NULL);
            if (defview)
                break;
        }
    }
    if (defview_out)
        *defview_out = defview;
    if (!defview)
        return NULL;
    return FindWindowExA(defview, NULL, "SysListView32", NULL);
}

/* Which layout does this box want? Default (value absent) is auto-arrange:
 * a box that has never heard of the switch gets the fleet behaviour. */
static int gs_want_autoarrange(void)
{
    HKEY  hk;
    DWORD val = 1, sz = sizeof(val), ty = REG_DWORD;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, GS_ICON_KEY, 0, KEY_QUERY_VALUE, &hk)
            == ERROR_SUCCESS) {
        if (RegQueryValueExA(hk, GS_ICON_VALUE, NULL, &ty, (LPBYTE)&val, &sz)
                != ERROR_SUCCESS || ty != REG_DWORD)
            val = 1;
        RegCloseKey(hk);
    }
    return val != 0;
}

/* ---- the desktop's shell bag ---------------------------------------------
 *
 * WHICH bag. The desktop is the shell namespace root, so its bag is the slot
 * the BagMRU ROOT key's NodeSlot value names. It is not always 1: measured
 * 2026-09-28, XP (.124, .110) Shell\BagMRU NodeSlot = 1, Win7 .195 = 4. This
 * used to be a fixed "Shell\Bags\1\Desktop", so on .195 every FFlags write
 * landed in a key nothing reads (the agent even created it: "Win7's .246 had
 * no Desktop subkey under Bags\1" - because its desktop was never in slot 1)
 * and ICONARRANGE reported that key's value as the desktop's. No NodeSlot at
 * all (Win98 has no BagMRU; a fresh profile before Explorer's first save)
 * falls back to 1, which is what every earlier agent used. */
typedef struct {
    char          path[96];
    unsigned long slot;
    int           from_mru;   /* 1 = BagMRU NodeSlot named it; 0 = fallback */
} gs_bag_t;

static int gs_reg_dword(HKEY hk, const char *name, DWORD *out)
{
    DWORD v = 0, sz = sizeof(v), ty = 0;
    if (RegQueryValueExA(hk, name, NULL, &ty, (LPBYTE)&v, &sz) != ERROR_SUCCESS
            || ty != REG_DWORD || sz != sizeof(v))
        return 0;
    *out = v;
    return 1;
}

static void gs_desktop_bag(gs_bag_t *b)
{
    HKEY  hk;
    DWORD v = 0;
    int   found = 0;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, DV_BAGMRU_KEY, 0, KEY_QUERY_VALUE,
                      &hk) == ERROR_SUCCESS) {
        found = gs_reg_dword(hk, DV_BAGMRU_VALUE, &v);
        RegCloseKey(hk);
    }
    b->slot = dv_bag_slot(found, (unsigned long)v);
    b->from_mru = found && b->slot == (unsigned long)v;
    if (!dv_bag_path(b->path, sizeof(b->path), b->slot))
        dv_bag_path(b->path, sizeof(b->path), DV_SLOT_DEFAULT);
}

/* Write one REG_DWORD and READ IT BACK. Returns 1 only when the value now
 * holds what we wrote - never on the strength of RegSetValueEx's return. */
static int gs_reg_set_dword_verified(HKEY hk, const char *bagpath,
                                     const char *name, DWORD val)
{
    DWORD got = 0;
    RegSetValueExA(hk, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
    if (gs_reg_dword(hk, name, &got) && got == val)
        return 1;
    log_msg(LOG_GS, "READ-BACK MISMATCH: wrote %s\\%s = %lu, it reads %lu - "
                    "the persisted desktop view is NOT what was asked for",
            bagpath, name, (unsigned long)val, (unsigned long)got);
    return 0;
}

/* Persist the setting in the desktop's shell bag so it survives a reboot even
 * if the agent's startup pass has not run yet. Read-modify-write: FFlags
 * carries several unrelated bits (FWF_DESKTOP, FWF_NOCLIENTEDGE...) and
 * stamping a whole word over it would change things nobody asked about.
 *
 * The key may not exist at all (a fresh profile before Explorer has saved the
 * desktop), so create it - in the slot BagMRU names, see gs_desktop_bag(). */
static void gs_bag_autoarrange(int on)
{
    gs_bag_t bag;
    HKEY  hk;
    DWORD flags = 0, disp = 0;
    unsigned long before, after;
    int   have;
    LONG  rc;

    gs_desktop_bag(&bag);
    rc = RegCreateKeyExA(HKEY_CURRENT_USER, bag.path, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE,
                         NULL, &hk, &disp);
    if (rc != ERROR_SUCCESS) {
        log_msg(LOG_GS, "shell bag %s not writable (error %ld) - auto-arrange "
                        "is still applied live, just not persisted here",
                bag.path, (long)rc);
        return;
    }
    have   = gs_reg_dword(hk, "FFlags", &flags);
    before = have ? (unsigned long)flags : DV_FFLAGS_DESKTOP_DEF;
    after  = dv_fflags_autoarrange(have, (unsigned long)flags, on);

    if (after != before) {
        if (gs_reg_set_dword_verified(hk, bag.path, "FFlags", (DWORD)after))
            log_msg(LOG_GS, "persisted auto-arrange %s: %s\\FFlags 0x%lx -> 0x%lx "
                            "(slot %lu from %s)",
                    on ? "ON" : "OFF", bag.path, before, after, bag.slot,
                    bag.from_mru ? "BagMRU NodeSlot" : "the default - no NodeSlot");
    } else {
        log_msg(LOG_GS, "auto-arrange already persisted (%s\\FFlags 0x%lx%s)",
                bag.path, before, have ? "" : ", absent = the shell default");
    }
    RegCloseKey(hk);
}

/* ---- the desktop's VIEW --------------------------------------------------
 *
 * The desktop must be in ICON view. .195 (Win7) was found in List view -
 * Mode=3, LogicalViewMode=4, IconSize=16 in its bag - with 8 of 117 icons
 * visible in one row, while ICONARRANGE said "autoarrange":true,"icons":119:
 * auto-arrange means nothing in List view, and neither report looked at the
 * view. The agent put it there itself (0x7051 is "List" on Win7, see
 * gs_autoarrange_cmd), but a person or a game can too, so this checks and
 * repairs rather than merely no longer causing it.
 *
 * It is a SET and it is conditional: a desktop already in icon view is left
 * completely alone (no post, no write), so a settled box changes nothing.
 * Live first, through the shell's OWN icon-view command where one has been
 * read from that Windows' menus - that updates Explorer's internal state, which
 * it writes back to the bag at logoff - then LVM_SETVIEW / the style bits if it
 * did not take. Then the persisted Mode/LogicalViewMode/IconSize, each read
 * back. Returns what it changed: bit 0 live, bit 1 persisted, bit 2 = the live
 * view is STILL wrong. */
#define GS_VIEW_LIVE_FIXED  1
#define GS_VIEW_BAG_FIXED   2
#define GS_VIEW_LIVE_FAILED 4

typedef struct {
    gs_bag_t      bag;
    unsigned long style;      /* live listview style, after the pass */
    long          lvview;     /* live LVM_GETVIEW after the pass, -1 = not asked */
    int           bag_open;   /* the bag key exists */
    dv_bagview_t  bv;         /* persisted Mode/LogicalViewMode/IconSize, after */
    int           result;     /* GS_VIEW_* */
} gs_view_t;

static long gs_lv_getview(HWND lv)
{
    int is9x; unsigned major, minor;
    DWORD_PTR r = 0;

    gs_winver(&is9x, &major, &minor);
    if (!dv_has_comctl6(is9x, major, minor))
        return -1;
    if (!SendMessageTimeoutA(lv, LVM_GETVIEW_, 0, 0,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r))
        return -1;
    return (long)r;
}

static void gs_bag_view_read(HKEY hk, dv_bagview_t *v)
{
    DWORD d = 0;
    memset(v, 0, sizeof(*v));
    if (gs_reg_dword(hk, "Mode", &d))            { v->has_mode = 1; v->mode = d; }
    if (gs_reg_dword(hk, "LogicalViewMode", &d)) { v->has_lvm = 1;  v->lvm = d; }
    if (gs_reg_dword(hk, "IconSize", &d))        { v->has_size = 1; v->icon_size = d; }
}

static int gs_desktop_view_fix(HWND defview, HWND lv, gs_view_t *out)
{
    int      is9x, vista, ok = 1;
    unsigned major, minor;
    HKEY     hk;
    gs_view_t r;

    memset(&r, 0, sizeof(r));
    gs_winver(&is9x, &major, &minor);
    vista = !is9x && major >= 6;
    gs_desktop_bag(&r.bag);

    r.style  = (unsigned long)GetWindowLongA(lv, GWL_STYLE);
    r.lvview = gs_lv_getview(lv);
    if (!dv_live_is_icon_view(r.style, r.lvview)) {
        WPARAM cmd = (WPARAM)dv_iconview_cmd(is9x, major, minor);
        log_msg(LOG_GS, "desktop is in %s view, not icons (style 0x%lx, "
                        "LVM_GETVIEW %ld) - restoring icon view",
                dv_view_name(r.style, r.lvview), r.style, r.lvview);
        if (defview && cmd) {
            /* The shell's own radio item - a SET, and it updates Explorer's
             * internal view state, not just the window. */
            PostMessageA(defview, WM_COMMAND, cmd, 0);
            Sleep(600);
            r.style  = (unsigned long)GetWindowLongA(lv, GWL_STYLE);
            r.lvview = gs_lv_getview(lv);
        }
        if (!dv_live_is_icon_view(r.style, r.lvview)) {
            DWORD_PTR dummy = 0;
            if (dv_has_comctl6(is9x, major, minor))
                SendMessageTimeoutA(lv, LVM_SETVIEW_, DV_LV_VIEW_ICON, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &dummy);
            r.style = (unsigned long)GetWindowLongA(lv, GWL_STYLE);
            if ((r.style & DV_LVS_TYPEMASK) != DV_LVS_ICON)
                SetWindowLongA(lv, GWL_STYLE,
                               (LONG)(r.style & ~DV_LVS_TYPEMASK) | DV_LVS_ICON);
            Sleep(200);
            r.style  = (unsigned long)GetWindowLongA(lv, GWL_STYLE);
            r.lvview = gs_lv_getview(lv);
            log_msg(LOG_GS, "%s - set the listview's view directly (now %s)",
                    cmd ? "the shell's icon-view command did not take"
                        : "no measured shell icon-view command on this Windows",
                    dv_view_name(r.style, r.lvview));
        } else {
            log_msg(LOG_GS, "icon view restored via the shell (command 0x%x)",
                    (unsigned)cmd);
        }
        if (dv_live_is_icon_view(r.style, r.lvview))
            r.result |= GS_VIEW_LIVE_FIXED;
        else {
            r.result |= GS_VIEW_LIVE_FAILED;
            log_msg(LOG_GS, "DESKTOP STILL NOT IN ICON VIEW (%s) - icons may be "
                            "hidden; restart Explorer or set View > Medium icons",
                    dv_view_name(r.style, r.lvview));
        }
    }

    /* Persisted: open, never create - a bag with no view values is already
     * the default icon view and there is nothing to repair. */
    if (RegOpenKeyExA(HKEY_CURRENT_USER, r.bag.path, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        dv_bagview_t fixed;
        r.bag_open = 1;
        gs_bag_view_read(hk, &r.bv);
        if (dv_bag_view_repair(&r.bv, vista, &fixed)) {
            log_msg(LOG_GS, "persisted desktop view in %s is not icons "
                            "(Mode %ld, LogicalViewMode %ld, IconSize %ld) - "
                            "restoring it",
                    r.bag.path,
                    r.bv.has_mode ? (long)r.bv.mode : -1L,
                    r.bv.has_lvm ? (long)r.bv.lvm : -1L,
                    r.bv.has_size ? (long)r.bv.icon_size : -1L);
            if (fixed.has_mode && (!r.bv.has_mode || fixed.mode != r.bv.mode))
                ok &= gs_reg_set_dword_verified(hk, r.bag.path, "Mode",
                                                (DWORD)fixed.mode);
            if (fixed.has_lvm && (!r.bv.has_lvm || fixed.lvm != r.bv.lvm))
                ok &= gs_reg_set_dword_verified(hk, r.bag.path, "LogicalViewMode",
                                                (DWORD)fixed.lvm);
            if (fixed.has_size &&
                    (!r.bv.has_size || fixed.icon_size != r.bv.icon_size))
                ok &= gs_reg_set_dword_verified(hk, r.bag.path, "IconSize",
                                                (DWORD)fixed.icon_size);
            gs_bag_view_read(hk, &r.bv);   /* report what is there now */
            if (ok && !dv_bag_view_is_bad(&r.bv)) {
                r.result |= GS_VIEW_BAG_FIXED;
                log_msg(LOG_GS, "persisted desktop view restored: Mode %ld, "
                                "LogicalViewMode %ld, IconSize %ld",
                        r.bv.has_mode ? (long)r.bv.mode : -1L,
                        r.bv.has_lvm ? (long)r.bv.lvm : -1L,
                        r.bv.has_size ? (long)r.bv.icon_size : -1L);
            }
        }
        RegCloseKey(hk);
    }
    if (out)
        *out = r;
    return r.result;
}

/* The same facts, read only - ICONARRANGE's post-condition. */
static void gs_desktop_view_read(HWND lv, gs_view_t *r)
{
    HKEY hk;
    int  result = r->result;

    memset(r, 0, sizeof(*r));
    r->result = result;
    gs_desktop_bag(&r->bag);
    r->style  = (unsigned long)GetWindowLongA(lv, GWL_STYLE);
    r->lvview = gs_lv_getview(lv);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, r->bag.path, 0, KEY_QUERY_VALUE,
                      &hk) == ERROR_SUCCESS) {
        r->bag_open = 1;
        gs_bag_view_read(hk, &r->bv);
        RegCloseKey(hk);
    }
}

/* Turn Auto Arrange ON, deterministically, and say whether it took.
 *
 * The ordering is the whole point - see the block comment at the top of this
 * section. Post the shell's toggle ONLY when the bit is clear, because it is a
 * toggle; fall back to the style bit when the toggle does not take; verify by
 * reading the bit back rather than trusting either call. */
static void gs_apply_autoarrange(HWND defview, HWND lv, int force)
{
    LONG style = GetWindowLongA(lv, GWL_STYLE);
    int  changed = 0;

    if (style & LVS_AUTOARRANGE) {
        log_msg(LOG_GS, "auto-arrange already on - left alone (posting the "
                        "shell toggle here would turn it OFF)");
    } else {
        if (defview && gs_autoarrange_cmd()) {
            /* PostMessage, not Send: a synchronous send into the shell can
             * block the agent indefinitely. Only with a MEASURED command id
             * for this Windows - 0x7051 is "List" on Win7. */
            PostMessageA(defview, WM_COMMAND, gs_autoarrange_cmd(), 0);
            Sleep(600);
            style = GetWindowLongA(lv, GWL_STYLE);
        }
        if (!(style & LVS_AUTOARRANGE)) {
            /* The toggle does not always take - it failed on .143 on every run
             * for weeks. Set the style bit directly; GWL_STYLE is settable
             * cross-process on a listview. This is a SET, so it cannot flip
             * the setting the wrong way, but it does NOT update Explorer's
             * internal view state, which is why gs_bag_autoarrange() below and
             * the every-startup re-apply both exist. */
            SetWindowLongA(lv, GWL_STYLE, style | LVS_AUTOARRANGE);
            Sleep(200);
            style = GetWindowLongA(lv, GWL_STYLE);
            log_msg(LOG_GS, "%s - set LVS_AUTOARRANGE directly (now %s)",
                    gs_autoarrange_cmd() ? "shell toggle did not take"
                        : "no measured Auto Arrange command on this Windows",
                    (style & LVS_AUTOARRANGE) ? "on" : "STILL OFF");
        } else {
            log_msg(LOG_GS, "auto-arrange turned on via the shell");
        }
        changed = (style & LVS_AUTOARRANGE) ? 1 : 0;
    }

    if (!(style & LVS_AUTOARRANGE)) {
        /* Still pack the icons once. LVM_ARRANGE carries no pointer, so it is
         * safe across processes on Win9x too, and without it a new game's icon
         * lands wherever Explorer dropped it - off-screen on .243. */
        SendMessageA(lv, LVM_ARRANGE_, LVA_DEFAULT_, 0);
        log_msg(LOG_GS, "auto-arrange COULD NOT BE SET on this desktop - "
                        "packed the icons once with LVM_ARRANGE instead");
    } else if (changed || force) {
        /* Setting the style does not re-pack what is already on screen; ask
         * for it explicitly so the desktop is tidy now rather than at the next
         * refresh. */
        SendMessageA(lv, LVM_ARRANGE_, LVA_DEFAULT_, 0);
    }
    /* ...and when auto-arrange was ALREADY on and nothing changed, do not send
     * LVM_ARRANGE at all. The shell is already keeping the desktop packed by
     * itself - that is the entire point of auto-arrange - so a re-pack here
     * would be visible churn that achieves nothing. This ran on every agent
     * startup, which is a large part of what the user saw as "rebuilding icons
     * all the time". */

    gs_bag_autoarrange(1);
}

/* Legacy layout: park every icon in a cell the wallpaper drew.
 *
 * Only reachable with HKLM\Software\RetroAgent\IconAutoArrange = 0. It must
 * turn Auto Arrange back OFF, or the shell ignores every position we set. */
static void gs_arrange_bay(HWND defview, HWND lv)
{
    gs_bay_t bay;
    int      count, i, col, row, cols;
    int      sw, sh;

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    gs_icon_bay(sw, sh, &bay);

    /* Auto Arrange must be OFF or the shell snaps every icon back to the
     * top-left grid and our positions never stick. Same toggle rule in
     * reverse: fire the WM_COMMAND only when the bit is SET. */
    {
        LONG style = GetWindowLongA(lv, GWL_STYLE);
        if (style & LVS_AUTOARRANGE) {
            log_msg(LOG_GS, "icon bay: auto-arrange is on - turning it off so "
                            "icon positions stick");
            if (defview && gs_autoarrange_cmd()) {
                PostMessageA(defview, WM_COMMAND, gs_autoarrange_cmd(), 0);
                Sleep(600);
                style = GetWindowLongA(lv, GWL_STYLE);
            }
            if (style & LVS_AUTOARRANGE) {
                SetWindowLongA(lv, GWL_STYLE, style & ~LVS_AUTOARRANGE);
                Sleep(200);
                style = GetWindowLongA(lv, GWL_STYLE);
                log_msg(LOG_GS, "icon bay: auto-arrange survived the toggle - "
                                "cleared the style directly (now %s)",
                        (style & LVS_AUTOARRANGE) ? "STILL ON" : "off");
            }
        }
    }
    gs_bag_autoarrange(0);

    /* "Align icons to grid" is a SECOND, independent setting. While
     * LVS_EX_SNAPTOGRID is set the shell ROUNDS every position we ask for to
     * its own grid, whose row pitch is the icon spacing PLUS the label -
     * measured at 103 px on a 1920x1080 box - so a bay drawn with 80 px cells
     * gets icons 103 px apart and they walk out of their slots down the
     * column. Verified by A/B on .246. Unlike auto-arrange this is not a
     * toggle: the message takes a (mask, value) pair, so passing value 0
     * clears it deterministically and cannot turn it on. */
    {
        DWORD exst = (DWORD)SendMessageA(lv, LVM_GETEXSTYLE_, 0, 0);
        if (exst & LVS_EX_SNAPTOGRID) {
            SendMessageA(lv, LVM_SETEXSTYLE_, LVS_EX_SNAPTOGRID, 0);
            exst = (DWORD)SendMessageA(lv, LVM_GETEXSTYLE_, 0, 0);
            log_msg(LOG_GS, "icon bay: align-to-grid was on - cleared it so "
                            "icons land in the bay's cells (now %s)",
                    (exst & LVS_EX_SNAPTOGRID) ? "STILL ON" : "off");
        }
    }

    count = (int)SendMessageA(lv, LVM_GETITEMCOUNT_, 0, 0);
    if (count <= 0)
        return;

    cols = gs_arrange_cols(&bay, sw, count);

    for (i = 0; i < count; i++) {
        col = i % cols;
        row = i / cols;
        /* Still more icons than the widened grid holds - a 3,000-title desktop
         * on an 800x600 screen. Keep packing downward as a last resort rather
         * than refusing; that is now genuinely the edge case it was meant to
         * be, instead of the normal state of a 1024x768 box. */
        if (row >= bay.rows)
            row = bay.rows - 1 + (i / cols - bay.rows + 1);
        /* +6 centres the icon in its drawn cell (cells are inset by 3 and the
         * icon's own bitmap is smaller than the cell). */
        SendMessageA(lv, LVM_SETITEMPOSITION_, (WPARAM)i,
                     MAKELPARAM(bay.x + col * bay.cell_w + 6,
                                bay.y + row * bay.cell_h + 6));
    }
    if (cols != bay.cols)
        log_msg(LOG_GS, "icon bay: arranged %d desktop icon(s) into %d columns "
                        "- the %dx%d bay holds only %d, so it was widened to "
                        "keep every icon on screen",
                count, cols, bay.cols, bay.rows, bay.cols * bay.rows);
    else
        log_msg(LOG_GS, "icon bay: arranged %d desktop icon(s) into the %dx%d "
                        "icon bay", count, bay.cols, bay.rows);
}

/* The one entry point. Called on every agent startup (retrowall.c), after a
 * GAMESYNC that actually changed the desktop, and on demand via ICONARRANGE.
 *
 * force = 0 is the routine pass: it makes sure the setting is right and does
 * as little as possible if it already is. force = 1 is a deliberate human
 * request and always does a full pass - "fixing issues" is a legitimate reason
 * to re-arrange a desktop that the agent thinks is already fine. */
void gs_desktop_icons_apply_ex(int force)
{
    HWND defview = NULL;
    HWND lv = gs_desktop_listview(&defview);
    int  view;

    if (!lv) {
        log_msg(LOG_GS, "desktop listview not found - icons left as they are");
        return;
    }
    /* The VIEW first: auto-arrange and the bay both mean nothing in List or
     * Details view (Win7 greys "Auto arrange" out there), and a view the shell
     * has just changed needs re-packing. */
    view = gs_desktop_view_fix(defview, lv, NULL);
    if (gs_want_autoarrange())
        gs_apply_autoarrange(defview, lv, force || (view & GS_VIEW_LIVE_FIXED));
    else
        gs_arrange_bay(defview, lv);
}

void gs_desktop_icons_apply(void)
{
    gs_desktop_icons_apply_ex(0);
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
 * worth having on disk, and refusing to continue would cost the other twenty.
 * Best-effort is not silent, though: the result is checked value by value
 * against the live registry and the log says exactly what landed.
 *
 * WINDOWS 9x (agent 1.86.1 on .243 logged "HexenII: cannot run regedit (0)"
 * - no Win9x box had EVER had an install.reg merged). The launch, the wait,
 * the verification and the fallback are explained in agent/shared/regmerge.h;
 * the logic that can be tested off-box lives there. */

/* install.reg files are a few KB; anything past this is not one of ours. */
#define GS_REG_MAX_BYTES (256 * 1024)

/* regedit /s is quick; never wait forever on it. */
#define GS_REGEDIT_WAIT_MS 60000

enum {
    GS_REGEDIT_OK = 0,      /* ran and exited 0                        */
    GS_REGEDIT_NOSTART,     /* could not be started                    */
    GS_REGEDIT_EXITCODE,    /* ran, exited non-zero                    */
    GS_REGEDIT_TIMEOUT      /* still running after GS_REGEDIT_WAIT_MS  */
};

static HKEY gs_rm_hkey(int root)
{
    switch (root) {
    case RM_HKLM: return HKEY_LOCAL_MACHINE;
    case RM_HKCU: return HKEY_CURRENT_USER;
    case RM_HKCR: return HKEY_CLASSES_ROOT;
    case RM_HKU:  return HKEY_USERS;
    case RM_HKCC: return HKEY_CURRENT_CONFIG;
    case RM_HKDD: return HKEY_DYN_DATA;
    default:      return NULL;
    }
}

/* Read the whole .reg into a heap buffer (NUL-terminated). */
static char *gs_reg_load(const char *path, DWORD *len)
{
    HANDLE h;
    DWORD size, got = 0;
    char *buf;

    *len = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(h, NULL);
    if (size == 0xFFFFFFFF || size > GS_REG_MAX_BYTES) {
        CloseHandle(h);
        return NULL;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (buf && !ReadFile(h, buf, size, &got, NULL))
        got = 0;
    CloseHandle(h);
    if (buf) {
        buf[got] = 0;
        *len = got;
    }
    return buf;
}

/* 1 = the registry now holds what the entry says, 0 = it does not,
 * -1 = cannot be judged here (an UNKNOWN line, a root we cannot open). */
static int gs_reg_entry_holds(const rm_entry_t *e)
{
    HKEY root = gs_rm_hkey(e->root), k;
    unsigned char buf[RM_DATA_MAX + 4];
    DWORD type = 0, len = sizeof(buf);
    LONG rc;

    if (e->op == RM_OP_UNKNOWN || !root)
        return -1;
    rc = RegOpenKeyExA(root, e->key, 0, KEY_QUERY_VALUE, &k);
    if (e->op == RM_OP_DELKEY) {
        if (rc == ERROR_SUCCESS)
            RegCloseKey(k);
        return rc != ERROR_SUCCESS;
    }
    if (rc != ERROR_SUCCESS)
        return e->op == RM_OP_DELVALUE;     /* no key: no value either */
    rc = RegQueryValueExA(k, e->name[0] ? e->name : NULL, NULL, &type, buf, &len);
    RegCloseKey(k);
    if (e->op == RM_OP_DELVALUE)
        return rc == ERROR_FILE_NOT_FOUND;
    if (rc == ERROR_MORE_DATA)
        return 0;                           /* bigger than anything we would write */
    return rm_value_matches(e, rc == ERROR_SUCCESS, (unsigned)type, buf,
                            rc == ERROR_SUCCESS ? (unsigned)len : 0);
}

/* Write one entry ourselves. 1 = written, 0 = failed (*err = the error),
 * -1 = not something we write (UNKNOWN, or a whole-key delete). */
static int gs_reg_entry_apply(const rm_entry_t *e, LONG *err)
{
    HKEY root = gs_rm_hkey(e->root), k;
    DWORD disp;
    LONG rc;

    *err = 0;
    if (!root || (e->op != RM_OP_SET && e->op != RM_OP_DELVALUE))
        return -1;
    if (e->op == RM_OP_DELVALUE) {
        rc = RegOpenKeyExA(root, e->key, 0, KEY_SET_VALUE, &k);
        if (rc != ERROR_SUCCESS)
            return 1;                       /* no key: the value is already gone */
        rc = RegDeleteValueA(k, e->name[0] ? e->name : NULL);
        RegCloseKey(k);
        if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND)
            return 1;
        *err = rc;
        return 0;
    }
    rc = RegCreateKeyExA(root, e->key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, &disp);
    if (rc != ERROR_SUCCESS) {
        *err = rc;
        return 0;
    }
    rc = RegSetValueExA(k, e->name[0] ? e->name : NULL, 0, e->type, e->data,
                        e->type == RM_REG_SZ ? e->len + 1 : e->len);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        *err = rc;
        return 0;
    }
    return 1;
}

/*
 * A VALUE THE RESOLUTION PASS OWNS IS THE BOX'S, NOT THE LIBRARY'S.
 *
 * install.reg is one byte-identical constant for eight monitors: the
 * CounterStrike16 one pins HKCU\Software\Valve\Half-Life\Settings
 * ScreenWidth at 800, HalfLife1's at 1024, MaxPayne's and
 * HiddenAndDangerous's their Display Width at 800. This merge re-applied that
 * constant on EVERY sync and GAMERES then put the box's own value back, so a
 * settled box reported "4 value(s) changed" for Counter-Strike and 2 each for
 * Max Payne and H&D forever (.110, agent 1.90.0, 2026-09-28) - and the
 * "a settled box must report 0 value(s) changed" signal was dead.
 *
 * So a value a GAMERES rule owns (agent/shared/gameres.h gr_reg_owner), for a
 * title that is installed here, is (a) read before regedit runs and put back
 * afterwards if regedit changed it, and (b) neither verified against nor
 * written from install.reg by the 9x fallback below. A value that does not
 * exist yet is left to install.reg, and GAMERES then sets the box's own and
 * says so. regedit itself still runs exactly as before - every key it
 * creates and everything else it sets is untouched.
 */
static int gs_reg_entry_owned(const rm_entry_t *e)
{
    const char *owner;
    char dir[MAX_PATH];

    if (e->op != RM_OP_SET || (e->root != RM_HKLM && e->root != RM_HKCU))
        return 0;
    owner = gameres_reg_owner(rm_root_name(e->root), e->key, e->name);
    if (!owner)
        return 0;
    /* Only while the owning title is here: HalfLife1's install.reg names the
     * key CounterStrike16's rule owns, and on a box without Counter-Strike
     * nothing else would ever set it. */
    _snprintf(dir, sizeof(dir) - 1, "%s\\%s", g_gs_dest, owner);
    dir[sizeof(dir) - 1] = 0;
    return gs_file_exists(dir);
}

#define GS_REG_KEEP_MAX   32
#define GS_REG_KEEP_DATA  256

typedef struct {
    int      root;
    char     key[RM_KEY_MAX];
    char     name[RM_NAME_MAX];
    DWORD    type, len;
    unsigned char data[GS_REG_KEEP_DATA];
} gs_reg_keep_t;

/* BEFORE regedit: the current value of every entry GAMERES owns. A value
 * that does not exist (or is bigger than anything GAMERES writes) is not
 * captured. Returns how many were. */
static int gs_reg_keep_snapshot(const char *text, DWORD len, gs_reg_keep_t *keep,
                                int max)
{
    rm_parser_t *ps = (rm_parser_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(rm_parser_t));
    rm_entry_t  *e  = (rm_entry_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(rm_entry_t));
    int n = 0;

    if (ps && e) {
        rm_init(ps, text, len);
        while (n < max && rm_next(ps, e)) {
            HKEY root, k;
            DWORD type = 0, dl = GS_REG_KEEP_DATA;
            if (!gs_reg_entry_owned(e))
                continue;
            root = gs_rm_hkey(e->root);
            if (!root || RegOpenKeyExA(root, e->key, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
                continue;
            if (RegQueryValueExA(k, e->name[0] ? e->name : NULL, NULL, &type,
                                 keep[n].data, &dl) == ERROR_SUCCESS) {
                keep[n].root = e->root;
                lstrcpynA(keep[n].key, e->key, sizeof(keep[n].key));
                lstrcpynA(keep[n].name, e->name, sizeof(keep[n].name));
                keep[n].type = type;
                keep[n].len  = dl;
                n++;
            }
            RegCloseKey(k);
        }
    }
    if (ps) HeapFree(GetProcessHeap(), 0, ps);
    if (e)  HeapFree(GetProcessHeap(), 0, e);
    return n;
}

/* AFTER regedit: put back each captured value regedit changed. Returns how
 * many it had to put back. */
static int gs_reg_keep_restore(const gs_reg_keep_t *keep, int n)
{
    int i, restored = 0;

    for (i = 0; i < n; i++) {
        HKEY root = gs_rm_hkey(keep[i].root), k;
        unsigned char cur[GS_REG_KEEP_DATA];
        DWORD type = 0, cl = sizeof(cur), disp;
        const char *name = keep[i].name[0] ? keep[i].name : NULL;
        LONG rc;

        if (!root || RegCreateKeyExA(root, keep[i].key, 0, NULL, 0,
                                     KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k,
                                     &disp) != ERROR_SUCCESS)
            continue;
        rc = RegQueryValueExA(k, name, NULL, &type, cur, &cl);
        if (rc != ERROR_SUCCESS || type != keep[i].type || cl != keep[i].len
            || memcmp(cur, keep[i].data, cl) != 0) {
            if (RegSetValueExA(k, name, 0, keep[i].type, keep[i].data,
                               keep[i].len) == ERROR_SUCCESS)
                restored++;
        }
        RegCloseKey(k);
    }
    return restored;
}

typedef struct {
    int total;          /* entries the file asks for                       */
    int holds;          /* ...the registry satisfies                        */
    int unknown;        /* ...this reader could not judge                   */
    int kept;           /* ...GAMERES owns: the box's, not the library's    */
    int dialect;        /* RM_DIALECT_* of the file                         */
    char first_miss[RM_KEY_MAX + RM_NAME_MAX + 16];
} gs_reg_tally_t;

/* Check every entry. With apply != 0, first write each one that does not
 * hold; *written counts the writes that succeeded. */
static void gs_reg_verify(const char *text, DWORD len, int apply,
                          gs_reg_tally_t *t, int *written, LONG *last_err)
{
    rm_parser_t *ps = (rm_parser_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(rm_parser_t));
    rm_entry_t  *e  = (rm_entry_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(rm_entry_t));

    memset(t, 0, sizeof(*t));
    if (written)
        *written = 0;
    if (!ps || !e) {
        if (ps) HeapFree(GetProcessHeap(), 0, ps);
        if (e)  HeapFree(GetProcessHeap(), 0, e);
        t->unknown = -1;
        return;
    }
    rm_init(ps, text, len);
    while (rm_next(ps, e)) {
        int ok;
        t->total++;
        if (gs_reg_entry_owned(e)) {
            t->kept++;          /* GAMERES sets it - never install.reg's constant */
            continue;
        }
        ok = gs_reg_entry_holds(e);
        if (ok < 0) {
            t->unknown++;
            continue;
        }
        if (!ok && apply) {
            LONG err = 0;
            if (gs_reg_entry_apply(e, &err) == 1) {
                if (written)
                    (*written)++;
                ok = gs_reg_entry_holds(e);
            } else if (err && last_err) {
                *last_err = err;
            }
        }
        if (ok > 0) {
            t->holds++;
        } else if (!t->first_miss[0]) {
            _snprintf(t->first_miss, sizeof(t->first_miss) - 1, "%s\\%s\\%s (line %d)",
                      rm_root_name(e->root), e->key,
                      e->op == RM_OP_DELKEY ? "[-key]" : (e->name[0] ? e->name : "@"),
                      e->lineno);
            t->first_miss[sizeof(t->first_miss) - 1] = 0;
        }
    }
    t->dialect = ps->dialect;
    HeapFree(GetProcessHeap(), 0, ps);
    HeapFree(GetProcessHeap(), 0, e);
}

/* Wait for a process while pumping this thread's messages. The sync thread
 * is a COM apartment (CoInitialize in gs_worker), so it owns a hidden window;
 * a child that broadcast a message and waited on it would otherwise hang for
 * the whole timeout. Returns 1 if the process ended, 0 on timeout. */
static int gs_wait_pumping(HANDLE h, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        DWORD spent = GetTickCount() - start;
        DWORD r;
        MSG msg;
        if (spent >= timeout_ms)
            return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        r = MsgWaitForMultipleObjects(1, &h, FALSE, timeout_ms - spent, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
            return 1;
        if (r != WAIT_OBJECT_0 + 1)
            return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

/* Run regedit /s on the file. The command line and flags come from
 * rm_build_cmd() (agent/shared/regmerge.h). why receives the log text for any
 * outcome but GS_REGEDIT_OK. */
static int gs_run_regedit(const char *reg_path, int is_9x, char *why, size_t why_cch)
{
    char regedit[MAX_PATH], arg[MAX_PATH], windir[MAX_PATH];
    char cmd[2 * MAX_PATH + 16];
    const char *cwd = NULL;
    unsigned flags = 0;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0, gle;
    HANDLE me = GetCurrentThread();
    int prio = THREAD_PRIORITY_ERROR_RETURN, rc;

    why[0] = 0;
    regedit[0] = 0;
    lstrcpynA(arg, reg_path, sizeof(arg));
    if (is_9x) {
        UINT n = GetWindowsDirectoryA(windir, sizeof(windir));
        char shortp[MAX_PATH];
        if (!n || n >= sizeof(windir)) {
            _snprintf(why, why_cch - 1, "GetWindowsDirectory failed, error %lu",
                      (unsigned long)GetLastError());
            why[why_cch - 1] = 0;
            return GS_REGEDIT_NOSTART;
        }
        _snprintf(regedit, sizeof(regedit) - 1, "%s%sREGEDIT.EXE", windir,
                  windir[n - 1] == '\\' ? "" : "\\");
        regedit[sizeof(regedit) - 1] = 0;
        if (!gs_file_exists(regedit)) {
            _snprintf(why, why_cch - 1, "no %s on this box", regedit);
            why[why_cch - 1] = 0;
            return GS_REGEDIT_NOSTART;
        }
        /* The 8.3 form, unquoted - the shape proven through EXEC on .243.
         * If there is none (8.3 names disabled), rm_build_cmd quotes it. */
        n = GetShortPathNameA(reg_path, shortp, sizeof(shortp));
        if (n && n < sizeof(shortp))
            lstrcpynA(arg, shortp, sizeof(arg));
        cwd = windir;
    }
    if (rm_build_cmd(is_9x, regedit, arg, cmd, sizeof(cmd), &flags) < 0) {
        _snprintf(why, why_cch - 1, "command line too long for %s", reg_path);
        why[why_cch - 1] = 0;
        return GS_REGEDIT_NOSTART;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    /* The sync thread runs at THREAD_PRIORITY_IDLE (thread_background()).
     * On 9x, run the few seconds of the merge at normal priority - one of the
     * four ways the failing 1.86.1 call differed from every start proven on
     * that box (regmerge.h). Restored below. */
    if (is_9x) {
        prio = GetThreadPriority(me);
        SetThreadPriority(me, THREAD_PRIORITY_NORMAL);
    }

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, (DWORD)flags, NULL, cwd, &si, &pi)) {
        gle = GetLastError();               /* before ANY other call */
        _snprintf(why, why_cch - 1, "CreateProcess(\"%s\") failed, error %lu%s", cmd,
                  (unsigned long)gle,
                  gle ? "" : " (Windows gave no reason)");
        why[why_cch - 1] = 0;
        rc = GS_REGEDIT_NOSTART;
        goto out;
    }
    if ((flags & RM_CREATE_SUSPENDED) && ResumeThread(pi.hThread) == (DWORD)-1) {
        gle = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        _snprintf(why, why_cch - 1, "ResumeThread on \"%s\" failed, error %lu", cmd,
                  (unsigned long)gle);
        why[why_cch - 1] = 0;
        rc = GS_REGEDIT_NOSTART;
    } else if (!gs_wait_pumping(pi.hProcess, GS_REGEDIT_WAIT_MS)) {
        /* A regedit still up after a minute is sitting on a dialog nobody
         * will answer - and on 9x an orphaned dialog blocks shutdown. */
        TerminateProcess(pi.hProcess, 1);
        _snprintf(why, why_cch - 1, "\"%s\" still running after %d s - killed it",
                  cmd, GS_REGEDIT_WAIT_MS / 1000);
        why[why_cch - 1] = 0;
        rc = GS_REGEDIT_TIMEOUT;
    } else if (GetExitCodeProcess(pi.hProcess, &code) && code != 0) {
        _snprintf(why, why_cch - 1, "\"%s\" exited %lu", cmd, (unsigned long)code);
        why[why_cch - 1] = 0;
        rc = GS_REGEDIT_EXITCODE;
    } else {
        rc = GS_REGEDIT_OK;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
out:
    if (is_9x && prio != THREAD_PRIORITY_ERROR_RETURN)
        SetThreadPriority(me, prio);
    return rc;
}

static void gs_merge_reg(const char *dst_dir, const char *title)
{
    char reg_path[MAX_PATH];
    char why[3 * MAX_PATH];
    char kept_note[96];
    char *text;
    DWORD len = 0;
    int is_9x = (GetVersion() & 0x80000000UL) != 0;
    int ran, written = 0, nkeep = 0, restored = 0;
    LONG werr = 0;
    gs_reg_tally_t t;
    gs_reg_keep_t *keep = NULL;

    _snprintf(reg_path, sizeof(reg_path) - 1, "%s\\install.reg", dst_dir);
    reg_path[sizeof(reg_path) - 1] = 0;
    if (!gs_file_exists(reg_path))
        return;

    /* Read the file FIRST: the values GAMERES owns are captured before
     * regedit writes the library's constants over them (gs_reg_entry_owned). */
    text = gs_reg_load(reg_path, &len);
    if (text)
        keep = (gs_reg_keep_t *)HeapAlloc(GetProcessHeap(), 0,
                                          GS_REG_KEEP_MAX * sizeof(gs_reg_keep_t));
    if (keep)
        nkeep = gs_reg_keep_snapshot(text, len, keep, GS_REG_KEEP_MAX);

    ran = gs_run_regedit(reg_path, is_9x, why, sizeof(why));
    if (ran == GS_REGEDIT_NOSTART)
        log_msg(LOG_GS, "%s: cannot run regedit: %s", title, why);
    else if (ran != GS_REGEDIT_OK)
        log_msg(LOG_GS, "%s: regedit did not finish cleanly: %s", title, why);

    /* ...and put back what regedit changed of them. */
    if (nkeep)
        restored = gs_reg_keep_restore(keep, nkeep);
    if (keep)
        HeapFree(GetProcessHeap(), 0, keep);

    /* The post-condition: every value the file names, read back. */
    if (!text) {
        log_msg(LOG_GS, "%s: install.reg could not be read back - merge %s",
                title, ran == GS_REGEDIT_OK ? "NOT VERIFIED" : "FAILED");
        return;
    }
    gs_reg_verify(text, len, 0, &t, NULL, NULL);
    kept_note[0] = 0;
    if (t.kept) {
        _snprintf(kept_note, sizeof(kept_note) - 1,
                  "; %d per-box value(s) left to GAMERES (%d put back)",
                  t.kept, restored);
        kept_note[sizeof(kept_note) - 1] = 0;
    }

    if (t.total > 0 && t.holds + t.unknown + t.kept == t.total) {
        if (t.kept == t.total)
            log_msg(LOG_GS, "%s: install.reg sets only per-box values - all %d left "
                    "to GAMERES (%d put back after regedit)", title, t.kept, restored);
        else if (ran == GS_REGEDIT_OK)
            log_msg(LOG_GS, "%s: merged install.reg via regedit - %d/%d value(s) verified%s%s",
                    title, t.holds, t.total - t.kept,
                    t.unknown ? " (some not checkable)" : "", kept_note);
        else
            log_msg(LOG_GS, "%s: install.reg values already present (%d/%d verified%s) "
                    "although regedit failed", title, t.holds, t.total - t.kept,
                    kept_note);
    } else if (t.total == 0 && t.dialect != RM_DIALECT_NONE) {
        /* BF1942 and Turok2 ship an install.reg that is all comments, on
         * purpose ("THIS FILE DELIBERATELY WRITES NOTHING"). */
        log_msg(LOG_GS, "%s: install.reg sets no values - nothing to verify", title);
    } else if (t.total <= 0) {
        log_msg(LOG_GS, "%s: install.reg has no entries this agent can read "
                "(no REGEDIT4 header?) - merge %s",
                title, ran == GS_REGEDIT_OK ? "NOT VERIFIED" : "FAILED");
    } else if (ran != GS_REGEDIT_OK || is_9x) {
        /* regedit did not land them. Write them ourselves - loudly: on Win9x
         * this is the path every title took until 1.86.1 was diagnosed, and
         * it must never again read as a success. NT, where regedit ran and
         * still left something out, only reports - see below. */
        int missing = t.total - t.holds - t.unknown - t.kept;
        gs_reg_verify(text, len, 1, &t, &written, &werr);
        log_msg(LOG_GS, "%s: REGEDIT DID NOT MERGE install.reg (%d value(s) missing) - "
                "agent wrote %d itself; now %d/%d verified%s%s%s", title, missing,
                written, t.holds, t.total - t.kept, kept_note,
                t.holds + t.unknown + t.kept == t.total ? "" : " - GAME MAY NOT LAUNCH, first: ",
                t.holds + t.unknown + t.kept == t.total ? "" : t.first_miss);
        if (werr)
            log_msg(LOG_GS, "%s: a registry write failed, error %ld", title, (long)werr);
    } else {
        log_msg(LOG_GS, "%s: INSTALL.REG NOT FULLY MERGED - regedit exited 0 but %d of %d "
                "value(s) are missing or different (first: %s) - game may not launch",
                title, t.total - t.holds - t.unknown - t.kept, t.total - t.kept,
                t.first_miss);
    }
    HeapFree(GetProcessHeap(), 0, text);
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
    char   titles[GS_MAX_TITLES][128];
    __int64 sizes[GS_MAX_TITLES];
    /* The gate's verdict per title, decided BEFORE the sizing walk (below). */
    char   gated[GS_MAX_TITLES];
    char   gated_why[GS_MAX_TITLES][192];
    int    n = 0, i, files = 0, ok_titles = 0, capped = 0, n_gated = 0;
    int    gr_titles = 0, gr_changed = 0, gr_absent_t = 0;
    int    gr_vok = -1, gr_vwrong = 0, gr_vabsent = 0;
    int    listing_complete = 0;
    DWORD  enum_err = 0;
    __int64 grand = 0, freeb, margin;

    g_gs_abort = 0;
    InterlockedExchange((LONG *)&g_gs_gr_kept, 0);
    g_win_tick = GetTickCount();
    g_win_bytes = 0;
    g_last_log = 0;

    /* RESET FIRST, THEN SNAPSHOT. Ordering matters and getting it wrong is
     * silent: gs_desk_reset() clears the snapshot as well as the counters, so
     * calling it after gs_desk_snapshot() throws the sampled icon set away and
     * every rewritten shortcut counts as ADDED - which is the very bug this
     * snapshot exists to fix, reintroduced one line later. Measured on .171:
     * shortcuts_changed came back as 81 on a box whose icons had not changed
     * at all. tests/python/test_icon_autoarrange_source.py pins the order. */
    gs_desk_reset();

    /* Sample the icon set BEFORE this run writes a single shortcut.
     *
     * AND SWEEP NOTHING HERE. Until 1.90.0 the next line moved every icon on
     * the desktop into C:\retro-desktop-backup, and each game's icon came back
     * only when the copy loop reached that title. On .110 (2026-09-28) a run
     * starved of CPU by a running game sat in "enumerating library" for ~100
     * minutes with two icons on the desktop where 97 had been; a run that fails
     * below (library unreachable, retried every 2 minutes) left it that way for
     * good. The sweep is now gs_sweep_unclaimed(), after the copy loop: it
     * removes only what this run did not put back, and only when the run
     * considered every title. tests/python/test_gamesync_sweep_order.py. */
    gs_desk_snapshot();
    /* The operator's own tools: rewritten in place if they moved, otherwise
     * claimed as they are - either way the end-of-run sweep keeps them. */
    gs_place_tool_shortcuts();
    gs_stage_wallpapers(library);

    EnterCriticalSection(&g_gs_lock);
    memset(&g_gs, 0, sizeof(g_gs));
    g_gs.state   = GS_SIZING;
    g_gs.started = GetTickCount();
    /* The progress heartbeat starts now (gs_beat, agent/shared/gsstall.h). */
    g_gs.beat      = g_gs.started;
    g_gs.cpu_tick  = g_gs.started;
    g_gs.last_busy = -1;
    gsst_reset(&g_gs.stall, g_gs.started);
    g_gs.cpu_ok = gs_cpu_sample(&g_gs.cpu_idle, &g_gs.cpu_kernel, &g_gs.cpu_user);
    /* NB: gs_desk_reset() is deliberately NOT here - it must run before
     * gs_desk_snapshot() above, or it wipes the icon set we just sampled. */
    LeaveCriticalSection(&g_gs_lock);
    g_gs_stall_logged = 0;
    g_gs_stall_logged_v = GSST_OK;

    log_msg(LOG_GS, "library: %s", library);
    /* Say which step the run is on. "enumerating library" used to cover
     * everything up to the end of the sizing walk, so a run stuck anywhere in
     * it looked the same. */
    gs_set_msg("profiling this machine for the capability gate");

    /* Build this machine's hardware profile and load any verdict file the host
     * published for it, BEFORE the sizing pass - the per-title decision below
     * needs both, and doing it once per run keeps a CPUID+registry sweep off
     * the inner loop. */
    gs_gate_init(library);

    /* Read the monitor ONCE per run rather than per title: it is an EDID
     * fetch plus a full mode enumeration, and the answer cannot change
     * mid-sync. The log line it emits is also the only place an operator can
     * see WHY a box was given the resolution it was given. */
    gs_set_msg("reading the monitor");
    gameres_probe();
    /* Raise the desktop to the panel's best refresh BEFORE any title is
     * written: the id Tech 3 configs carry the persisted rate so they agree
     * with what each title's own launcher writes, so the desktop has to be on
     * its best rate first for that number to be the best one. It also reaches
     * every engine that has no refresh setting of its own, which on this
     * library is most of them. */
    gameres_apply_display();

    {
        char why[200];
        if (gs_dest_resolve(g_gs_dest, sizeof(g_gs_dest), g_gs_dest_root, why, sizeof(why)) < 0) {
            /* Refuse, loudly: see gs_dest_resolve() - never fall back to C:. */
            log_msg(LOG_GS, "NOT SYNCING: %s", why);
            EnterCriticalSection(&g_gs_lock);
            g_gs.state = GS_FAILED;
            LeaveCriticalSection(&g_gs_lock);
            gs_set_msg("NOT SYNCING: %s", why);
            return;
        }
        log_msg(LOG_GS, "titles go to %s", g_gs_dest);
    }

    gs_set_msg("enumerating library");
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", library);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        /* Nothing has been swept: the desktop is exactly as this run found it,
         * which is the point - this path is retried every two minutes while
         * the NAS is down, and each retry used to empty the desktop again. */
        log_msg(LOG_GS, "cannot reach library (%lu): %s", GetLastError(), library);
        EnterCriticalSection(&g_gs_lock);
        g_gs.state = GS_FAILED;
        LeaveCriticalSection(&g_gs_lock);
        gs_set_msg("library unreachable - will retry");
        return;
    }
    /*
     * NAME THE TITLES FIRST, SIZE THEM AFTERWARDS - and say so if the walk
     * stopped early.
     *
     * gs_dir_size() used to be called from inside this loop, which meant the
     * outer FindFirstFile handle stayed open across a full recursive walk of
     * every title's tree - minutes of further SMB traffic per title, over the
     * SAME redirector connection. On Win9x that search context does not
     * survive it: FindNextFileA simply returns FALSE partway down the library
     * and the loop ends, with no error anywhere and a `state=done` at the end
     * of the run. Measured on .243 (Win98SE, Pentium 1) on 2026-08-31: a
     * 46-title library enumerated as 25 titles, and the 21 past the cut - one
     * of which the gate approves for that box - were never even considered.
     * The status line reported `titles_total: 25` and looked entirely healthy.
     *
     * So: collect the names, CLOSE the handle, then size. The handle now lives
     * for one directory listing instead of for the whole sizing pass.
     */
    SetLastError(0);
    do {
        if (fd.cFileName[0] == '.')
            continue;
        /* Directories beginning with _ are the library's own support folders,
         * not games: _desktop holds the fleet wallpapers, _patches the record
         * of what has been patched. Counting them as titles copied 26 MB of
         * wallpaper onto every box as if it were a game, and reported 30
         * titles where there were 29. */
        if (fd.cFileName[0] == '_')
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (n >= GS_MAX_TITLES) {
            capped = 1;
            break;
        }
        lstrcpynA(titles[n], fd.cFileName, sizeof(titles[0]));
        n++;
        SetLastError(0);
    } while (FindNextFileA(h, &fd));
    enum_err = GetLastError();
    FindClose(h);

    /* A truncated library must SAY it was truncated. Both of these end the
     * loop and both used to be silent. */
    if (capped)
        log_msg(LOG_GS, "library enumeration CAPPED at %d title(s) - the rest "
                        "were NOT considered; raise GS_MAX_TITLES",
                GS_MAX_TITLES);
    else if (enum_err != 0 && enum_err != ERROR_NO_MORE_FILES)
        log_msg(LOG_GS, "library enumeration STOPPED EARLY after %d title(s) "
                        "(error %lu) - the library is bigger than this and "
                        "everything past that point was NEVER considered",
                n, enum_err);
    /* A run that did not see the whole library must not sweep the desktop at
     * the end: the icons of the titles it never looked at would go with it. */
    listing_complete = !capped &&
                       (enum_err == 0 || enum_err == ERROR_NO_MORE_FILES);

    /* Sized below, after the gate - see there. The ordering pass moves these
     * along with the names, so they must start defined. */
    for (i = 0; i < n; i++) {
        sizes[i] = 0;
        gated[i] = 0;
        gated_why[i][0] = 0;
    }

    /* Order the titles before copying any of them.
     *
     * WHY THIS MATTERS MORE THAN IT LOOKS. A period disk is small - the Gateway
     * 550 that prompted this has SIX gigabytes - so on most fleet machines the
     * library does not fit and the disk-fit check skips most of it. Without an
     * order, "which games does this machine get" is decided by whatever order
     * the directory happened to enumerate in. That box ended up with Quake III,
     * Soldier of Fortune and System Shock: three fine games chosen by accident.
     *
     * _priority.txt in the library root fixes that: one title per line, best
     * first. Anything not listed keeps its existing relative order and follows.
     * Missing file means unchanged behaviour. */
    {
        char  pri_path[MAX_PATH];
        char *buf;
        DWORD got = 0;
        HANDLE ph;
        int ordered = 0;

        _snprintf(pri_path, sizeof(pri_path) - 1, "%s\\_priority.txt", library);
        pri_path[sizeof(pri_path) - 1] = 0;
        ph = CreateFileA(pri_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, 0, NULL);
        if (ph != INVALID_HANDLE_VALUE) {
            buf = (char *)HeapAlloc(GetProcessHeap(), 0, 8192);
            if (buf && ReadFile(ph, buf, 8191, &got, NULL) && got) {
                char *line = buf;
                buf[got] = 0;
                while (*line && ordered < n) {
                    char *end = line;
                    int   j;
                    while (*end && *end != '\r' && *end != '\n')
                        end++;
                    if (*end) {
                        *end = 0;
                        end++;
                    }
                    /* Skip leading whitespace AND any stray CR/LF. The line
                     * splitter above NUL-terminates at the first CR of a CRLF
                     * and steps over it, which leaves the LF at the head of the
                     * next line - so without this every second line would fail
                     * to match and the ordering would silently half-work. */
                    while (*line == ' ' || *line == '\t' || *line == '\r'
                           || *line == '\n')
                        line++;
                    if (*line && *line != '#' && *line != ';') {
                        /* Move a named title up to the next ordered slot. */
                        for (j = ordered; j < n; j++) {
                            if (lstrcmpiA(titles[j], line) != 0)
                                continue;
                            if (j != ordered) {
                                char    tn[128];
                                __int64 ts;
                                lstrcpynA(tn, titles[j], sizeof(tn));
                                ts = sizes[j];
                                for (; j > ordered; j--) {
                                    lstrcpynA(titles[j], titles[j - 1], sizeof(titles[0]));
                                    sizes[j] = sizes[j - 1];
                                }
                                lstrcpynA(titles[ordered], tn, sizeof(titles[0]));
                                sizes[ordered] = ts;
                            }
                            ordered++;
                            break;
                        }
                    }
                    line = end;
                }
            }
            if (buf)
                HeapFree(GetProcessHeap(), 0, buf);
            CloseHandle(ph);
            if (ordered)
                log_msg(LOG_GS, "_priority.txt ordered the first %d of %d "
                                "title(s)", ordered, n);
        }
    }

    if (n == 0) {
        log_msg(LOG_GS, "library is empty - nothing to do");
        EnterCriticalSection(&g_gs_lock);
        g_gs.state = GS_DONE;
        LeaveCriticalSection(&g_gs_lock);
        gs_write_marker(0);
        return;
    }

    /* ASK THE GATE FIRST, THEN SIZE ONLY WHAT MAY BE COPIED.
     *
     * The sizing pass is a full recursive walk of each title's tree ON THE
     * SHARE - thousands of SMB round trips for a big title. It used to walk
     * every title, including the ones the capability gate was about to refuse
     * (22 of 46 on the Pentium-1 .243), whose size nothing ever uses. The gate
     * reads only the profile, the published verdicts and the title's small
     * requires.json, so asking it first costs nothing and the refused trees are
     * never walked. A disk-limited "no" is not a refusal (see
     * gs_gate_limited_by_disk): that title is sized, because the disk check
     * below needs its real size. */
    for (i = 0; i < n; i++) {
        char why[192];
        if (!gs_gate_allows_title(library, titles[i], why, sizeof(why))
            && !gs_gate_limited_by_disk(why)) {
            gated[i] = 1;
            lstrcpynA(gated_why[i], why, sizeof(gated_why[0]));
            n_gated++;
            continue;
        }
        gs_set_msg("sizing %s (%d of %d)", titles[i], i + 1, n);
        _snprintf(src, sizeof(src) - 1, "%s\\%s", library, titles[i]);
        src[sizeof(src) - 1] = 0;
        sizes[i] = gs_dir_size(src, &files);
        grand += sizes[i];
    }

    freeb = gs_free_bytes(g_gs_dest_root);
    log_msg(LOG_GS, "%d title(s) (%d gated, not walked), %d file(s), "
            "%I64d MB to copy; %c: has %I64d MB free",
            n, n_gated, files, grand / 1048576, g_gs_dest_root[0],
            freeb < 0 ? (__int64)-1 : freeb / 1048576);

    /*
     * Does the published file actually cover this library? A host publish is
     * per-PROFILE and whole-library by definition, so a file carrying fewer
     * rows than there are titles means something overwrote it with a partial
     * pass - which is precisely how nine ollama adjudications were lost without
     * a single error anywhere. The gate is unharmed (local rules still decide
     * every title), so this is a WARNING and never a refusal to sync; but it
     * must be SAID, because the verdicts a published file uniquely carries are
     * the marginal-band ones this box cannot recompute for itself.
     */
    /* The MIRROR of the check below, and the one that catches a truncated
     * enumeration from the other side: the published file is whole-library by
     * definition, so if it carries MORE rows than we enumerated, we did not see
     * the whole library. On .243 this would have read "covers 46 of 25". */
    if (g_gate_on && g_gate_verdicts && g_gate_verdict_n > n)
        log_msg(LOG_GS, "gate: WARNING the published verdict file lists %d "
                        "title(s) but only %d were enumerated - the library "
                        "listing was TRUNCATED and %d title(s) were never "
                        "considered",
                g_gate_verdict_n, n, g_gate_verdict_n - n);

    if (g_gate_on && g_gate_verdicts && g_gate_verdict_n < n)
        log_msg(LOG_GS, "gate: WARNING published file covers %d of %d title(s) "
                        "- it was overwritten by a partial publish; any "
                        "host-adjudicated verdict for the missing %d has been "
                        "lost and those titles fall back to local rules",
                g_gate_verdict_n, n, n - g_gate_verdict_n);

    EnterCriticalSection(&g_gs_lock);
    g_gs.state        = GS_COPYING;
    g_gs.total_titles = n;
    g_gs.total_bytes  = grand;
    LeaveCriticalSection(&g_gs_lock);

    gs_mkdir_p(g_gs_dest);

    for (i = 0; i < n; i++) {
        if (g_gs_abort) {
            log_msg(LOG_GS, "aborted by request");
            break;
        }
        /* Can this machine actually RUN it? Decided before the sizing walk
         * (above), and before the disk maths here, because a title the box
         * cannot run should not be charged against the space a title it CAN
         * run needs. */
        if (gated[i]) {
            log_msg(LOG_GS, "GATED %s - %s", titles[i], gated_why[i]);
            EnterCriticalSection(&g_gs_lock);
            g_gs.gated_titles++;
            /* Its bytes were never added to the target (it was not sized),
             * so the percentage still reaches 100 without subtracting. */
            LeaveCriticalSection(&g_gs_lock);
            gs_restore_shortcuts_if_installed(titles[i]);
            continue;
        }

        /* Re-measure per title: earlier titles have just consumed space, and
         * on a period disk the difference decides whether this one fits. */
        freeb = gs_free_bytes(g_gs_dest_root);
        margin = gs_free_margin_for(g_gs_dest_root);
        /* The credit below walks the INSTALLED tree, so take it only when it
         * can change the answer: if the title fits without it, it fits with it
         * (the credit is never negative). Same verdict, and a box with room to
         * spare no longer walks every installed title on every sync. */
        if (freeb >= 0 && sizes[i] + margin > freeb) {
            /* A title ALREADY INSTALLED is being updated, not added, so what it
             * needs is the difference - the space its current copy occupies is
             * about to be reused. Charging it the full size meant an installed
             * game could never be patched on a full disk: a 6 GB box kept its
             * OLD Unreal Tournament 436 while the patched 469e sat on the share,
             * skipped for "needing" a gigabyte it was already using. The server
             * runs 469e and a 436 client cannot join it, so that skip was the
             * difference between a working game and an unusable one. */
            char  have[MAX_PATH];
            int   nfiles = 0;
            __int64 existing;
            _snprintf(have, sizeof(have) - 1, "%s\\%s", g_gs_dest, titles[i]);
            have[sizeof(have) - 1] = 0;
            if (gs_file_exists(have)) {
                existing = gs_dir_size(have, &nfiles);
                if (existing > 0 && freeb >= 0)
                    freeb += existing;
            }
        }
        if (freeb >= 0 && sizes[i] + margin > freeb) {
            log_msg(LOG_GS, "SKIP %s - needs %I64d MB + %I64d MB kept free, only %I64d MB free",
                    titles[i], sizes[i] / 1048576, margin / 1048576, freeb / 1048576);
            EnterCriticalSection(&g_gs_lock);
            g_gs.skipped_titles++;
            /* Its bytes are never going to arrive; drop them from the target
             * so the overall percentage still reaches 100. */
            g_gs.total_bytes -= sizes[i];
            LeaveCriticalSection(&g_gs_lock);
            gs_restore_shortcuts_if_installed(titles[i]);
            continue;
        }
        EnterCriticalSection(&g_gs_lock);
        lstrcpynA(g_gs.title, titles[i], sizeof(g_gs.title));
        g_gs.file[0] = 0;
        LeaveCriticalSection(&g_gs_lock);
        gs_set_msg("copying %s", titles[i]);
        log_msg(LOG_GS, "==> %s (%I64d MB)", titles[i], sizes[i] / 1048576);

        _snprintf(src, sizeof(src) - 1, "%s\\%s", library, titles[i]);
        _snprintf(dst, sizeof(dst) - 1, "%s\\%s", g_gs_dest, titles[i]);
        src[sizeof(src) - 1] = 0;
        dst[sizeof(dst) - 1] = 0;

        if (gs_copy_tree(src, dst)) {
            int gr_absent = 0;
            ok_titles++;
            gs_merge_reg(dst, titles[i]);
            /*
             * THE RESOLUTION PASS RUNS AFTER gs_merge_reg(), AND THE ORDER IS
             * THE POINT. A staged install.reg is a byte-identical constant
             * shipped to eight different monitors, and Half-Life's pins
             * HKCU\Software\Valve\Half-Life\Settings ScreenWidth to 1024 -
             * the one key every GoldSrc title on the box shares, there being
             * no Software\Valve\CounterStrike key at all. Merging it and then
             * leaving it is how a 1080p machine gets re-pinned to 1024x768 on
             * every single sync, with the launcher unable to undo something
             * written after it ran. Anything that later re-orders these two
             * calls silently restores that bug.
             */
            gr_changed += gameres_apply_title(dst, titles[i], &gr_absent);
            gr_absent_t += gr_absent;
            if (gameres_has_rules(titles[i]))
                gr_titles++;
            gs_make_game_shortcut(dst, titles[i]);
        } else {
            log_msg(LOG_GS, "%s finished with errors", titles[i]);
        }

        EnterCriticalSection(&g_gs_lock);
        g_gs.done_titles++;
        LeaveCriticalSection(&g_gs_lock);
    }

    /* What GAMERES rewrote this run has to be on disk before the next sync
     * looks, or that sync copies the library's files straight back and the
     * two fight again (agent/shared/grledger.h). Also on an aborted run: the
     * records it holds are true whether or not every title was reached. */
    gameres_ledger_save();

    /* ...and then the POST-CONDITION: is every installed title actually set
     * to its resolution now? Read-only, the same checks the writers made,
     * and never a reason to fail the sync - it reports, it does not judge.
     * Before `state` turns done, so a STATUS that reads done carries it.
     * Skipped on an aborted run, which never reached every title. */
    gr_vok = -1;
    if (!g_gs_abort)
        gr_vok = gameres_verify_sync(&gr_vwrong, &gr_vabsent);

    EnterCriticalSection(&g_gs_lock);
    g_gs.state = g_gs_abort ? GS_FAILED : GS_DONE;
    i = g_gs.failed_files;
    g_gs.gr_changed = gr_changed;
    g_gs.gr_kept    = g_gs_gr_kept;
    if (gr_vok >= 0) {
        g_gs.gr_vwrong   = gr_vwrong;
        g_gs.gr_vabsent  = gr_vabsent;
        g_gs.gr_verified = 1;
    }
    LeaveCriticalSection(&g_gs_lock);

    /* NOW - and only now - take off the desktop what was there when the run
     * started and was not put back by it: stale shortcuts to games no longer
     * staged, a title the gate now refuses, vendor clutter. Only a run that
     * considered EVERY title may do it (ds_run_may_sweep): an aborted run, or
     * one whose library listing was cut short or capped, never looked at some
     * titles, and sweeping would take their icons with it. */
    if (ds_run_may_sweep(g_gs_abort != 0, listing_complete))
        gs_sweep_unclaimed();
    else
        log_msg(LOG_GS, "desktop NOT swept: this run %s, so some titles were "
                        "never considered - every icon stays where it is",
                g_gs_abort ? "was aborted" : "saw an incomplete library listing");

    /* Arrange AFTER every shortcut exists: the shell creates a listview item
     * per .lnk asynchronously, so arranging per title would keep re-sorting a
     * list that is still growing.
     *
     * ...and ONLY when this run actually changed the desktop. GAMESYNC runs at
     * startup and the usual case is a fully provisioned box where every title
     * was skipped, nothing was copied and no shortcut was created. Arranging
     * there rebuilt the layout on every boot of every machine for no reason.
     * A deploy that DID change something still arranges - that case is wanted,
     * it is what the staged-game fix loop depends on, and suppressing it would
     * leave a freshly deployed game's icon unplaced. */
    /* Resolve the icon set against the pre-run snapshot before anything reads
     * the counter: icons added, plus icons that were there and really went. */
    gs_desk_settle_lnks();

    if (gs_desk_changed()) {
        log_msg(LOG_GS, "desktop changed (%ld file(s) written, %ld new/removed "
                        "shortcut(s)) - arranging icons",
                gs_desk_files(), gs_desk_lnks());
        Sleep(2000);
        gs_desktop_icons_apply();
    } else {
        log_msg(LOG_GS, "nothing changed - icons left alone (0 file(s) written, "
                        "0 new/removed shortcut(s)); use ICONARRANGE to force "
                        "a pass");
    }

    /* The written/shortcut counts belong on the line an operator already reads.
     * See the comment on gs_desk_files() - a steady-state box MUST report
     * `0 file(s) written`, and one that reports the same small non-zero count
     * every pass is telling you a file re-copies forever and the icon-rebuild
     * gate is permanently defeated. Without this, that is invisible. */
    log_msg(LOG_GS, "done: %d/%d title(s) copied, %d skipped (no room), "
            "%d gated (machine cannot run), %d file error(s), "
            "%ld file(s) written, %ld new/removed shortcut(s)",
            ok_titles, n, g_gs.skipped_titles, g_gs.gated_titles, i,
            gs_desk_files(), gs_desk_lnks());
    /* The resolution pass reports on its own line, and it reports CHANGES -
     * a steady-state box must read 0, exactly like `file(s) written` above.
     * A box that reports the same non-zero count on consecutive no-change
     * syncs is announcing that something rewrites a config forever, which is
     * this project's signature invisible fault one directory along. */
    /* ...and it says how many of the files it had adjusted were KEPT rather
     * than copied back from the library. On a settled box that is every such
     * file and `value(s) changed` is 0; until 1.90.x the two undid each other
     * on every sync and neither number could ever settle (grledger.h). */
    log_msg(LOG_GS, "gameres: %d title(s) have resolution rules, "
                    "%d value(s) changed, %d target(s) absent from this build, "
                    "%ld adjusted file(s) kept (library copy unchanged)",
            gr_titles, gr_changed, gr_absent_t, (long)g_gs_gr_kept);
    /* How much of the run went waiting, and why - on the line after `done:`,
     * so a slow sync explains itself (see gs_beat). Silent when it never
     * stalled. */
    {
        unsigned long stall_ms, starved_ms;
        EnterCriticalSection(&g_gs_lock);
        stall_ms = g_gs.stall.total_stall_ms;
        starved_ms = g_gs.stall.total_starved_ms;
        LeaveCriticalSection(&g_gs_lock);
        if (stall_ms >= GSST_GAP_MS)
            log_msg(LOG_GS, "progress: %lu s of this run went waiting between "
                            "progress points, %lu s of it with the CPU "
                            "saturated (starved - GAMESYNC runs at idle "
                            "priority)", stall_ms / 1000, starved_ms / 1000);
    }
    gs_gate_free();
    gs_set_msg("complete - %d title(s)", ok_titles);

    /* The game index checks for changes only every 15 minutes now. A sync
     * that really wrote files or changed the icon set is the one moment the
     * host's favourites pipeline most wants the index to move, so wake it. */
    if (gs_desk_changed())
        gameindex_poke();

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

    /* Started by the startup thread OR by a GAMESYNC command - either way
     * a library copy is background work, and CreateThread does not inherit
     * the creator's priority. */
    thread_background();
    lstrcpynA(lib, a->library, sizeof(lib));
    HeapFree(GetProcessHeap(), 0, a);

    /* COM must be initialised on the thread that uses it. Do it around the
     * whole run rather than per shortcut - CoInitialize is cheap but the
     * apartment has to outlive every interface pointer we hold. */
    if (gs_ole_load() && g_gs_CoInitialize)
        g_gs_CoInitialize(NULL);
    gs_run(lib);
    if (g_gs_CoUninitialize)
        g_gs_CoUninitialize();
    InterlockedExchange((LONG *)&g_gs_running, 0);
    return 0;
}

static int gs_start(const char *library)
{
    gs_arg_t *a;
    HANDLE    th;
    DWORD     tid;

    /* Never copy the retro library onto a modern Windows host (see
     * gamesync_thread). The GAMESYNC command is already refused there by the
     * command table; this covers every other caller. */
    if (!host_manages_this_box()) {
        log_msg(LOG_GS, "not starting: modern Windows host is not managed "
                "(HKLM\\Software\\RetroAgent\\ManageModernWindows=1 overrides)");
        return 0;
    }

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
    /* GAMERES's ledger lock: gs_copy_file() reads the ledger and the GAMERES
     * command writes it, from different threads. Made here, at startup on the
     * main thread, before either can run. */
    gameres_init();
}

/* Report what the image left behind, so the log says which build a box came
 * from rather than just that it is new. Best-effort: the flag is small and
 * plain text, and a missing or unreadable one is not an error. */
static void gs_log_image_flag(void)
{
    HANDLE h;
    char   buf[256];
    DWORD  got = 0;
    char  *p;

    h = CreateFileA(GS_NEWIMAGE_FLAG, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        for (p = buf; *p; p++)
            if (*p == '\r' || *p == '\n')
                *p = ' ';
        log_msg(LOG_GS, "image flag: %s", buf);
    }
    CloseHandle(h);
}

static void gs_drivers_autopass(void);   /* DRIVERS UPDATE, below */

DWORD WINAPI gamesync_thread(LPVOID param)
{
    int fresh;

    (void)param;
    thread_background();

    /* A MODERN WINDOWS HOST GETS NOTHING FROM THIS THREAD - no games, no
     * desktop shortcuts, no first-boot driver work. Until 1.83.1 this thread
     * had no host-policy check at all: 1.82.0 refused the GAMESYNC *command*
     * on Windows 10/11 through the command table, but this startup thread
     * provisions on its own whenever gamesync.done is absent, which on a box
     * that was never meant to be provisioned is always. WHITEBEAST (Win11,
     * the fleet's server host) was 2.6 GB into copying all 51 staged titles
     * when it was stopped on 2026-09-24. Checked FIRST, before the delay, so
     * nothing below can run on such a box. */
    if (!host_manages_this_box()) {
        log_msg(LOG_GS, "modern Windows host - not provisioning games, not placing "
                "shortcuts (HKLM\\Software\\RetroAgent\\ManageModernWindows=1 overrides)");
        return 0;
    }

    /* Let the desktop settle and the redirector come up before touching a
     * UNC path; on a fresh XP logon the network is not ready immediately. */
    Sleep(GS_FIRST_DELAY_MS);

    /* Put the operator's own tools on the desktop on EVERY start, whether or
     * not provisioning has anything left to do - after a desktop sweep these
     * are the only way back to the agent and the chat client without a file
     * browser.
     *
     * AFTER the delay, not before: the first attempt ran the moment the thread
     * started, when the shell has not finished coming up, so SHGetFolderPath
     * had no desktop to give and the whole thing returned without placing
     * anything or saying so.
     *
     * The 3dfx Control Panel is copied onto a box with a card it serves FIRST
     * (agent 1.94.0, src/fxpanel.c), so the shortcut pass below finds it. */
    fxpanel_ensure();
    gs_place_tool_shortcuts();

    /* Two independent signals, and they answer different questions.
     *
     *   newimage.flag  is placed BY THE IMAGE, so its presence is positive
     *                  evidence that this box was just installed.
     *   gamesync.done  is written by US once a run completes cleanly.
     *
     * Absence of the done-marker alone is a weak signal - it is also absent if
     * someone deleted it, or on a box that predates this feature entirely. The
     * flag is what lets the log say 'freshly imaged' and mean it. */
    fresh = gs_file_exists(GS_NEWIMAGE_FLAG);
    if (fresh) {
        gs_log_image_flag();
        /* Restore this machine's own activation, if it has one saved. A
         * reinstall of an already-activated box should not need the operator to
         * activate it again - wpa.dbl is hardware-bound, so this only ever
         * restores what this same machine earned. Silent no-op when nothing is
         * saved, because a box that has never been activated is the normal case
         * on first image and not an error. */
        gs_restore_activation();
        /* Finish the driver work GUI setup left undone, THEN decide whether the
         * staged tree is still needed. Order matters: reclaiming first would
         * delete the drivers this is about to install. */
        if (gs_drv_busy_wait()) {
            gs_install_missing_drivers();
            /* Then the drivers Windows DID configure, badly. This must come
             * before the reclaim: a preference cannot be applied from a tree
             * we have already deleted. */
            gs_apply_driver_prefs();
            gs_drv_busy_leave();
        } else {
            log_msg(LOG_GS, "DRIVER INSTALL SKIPPED: another driver install held the lock for 15 min");
        }
        /* Before the marker check, and before any copying: on a small disk this
         * is the difference between three games and a dozen. */
        gs_reclaim_drivers();
    }
    /* Every NT start, fresh image or not: a sound card whose driver installed
     * while XP's kernel audio stack never registered has no wave device, and
     * nothing else says so. One waveOutGetNumDevs() call when it is fine. */
    gs_audio_stack_check();
    /* 1.89.0: every NT start, a device whose driver is missing gets one from
     * the share's driver store - never display, never 3dfx, at most two boots
     * per device (DriverUpdate=0 switches it off). NOT gated on !fresh:
     * nothing ever deletes newimage.flag, so every boot of a PXE-imaged box is
     * "fresh" (.110, imaged weeks ago) and a !fresh gate would never fire on
     * the boxes this exists for. A device the image pass above already judged
     * this boot is left to it (skip_seen), so nothing is forced twice. */
    gs_drivers_autopass();                      /* XP only - it checks */

    if (gs_file_exists(GS_MARKER)) {
        log_msg(LOG_GS, "already provisioned (%s present) - idle", GS_MARKER);
        return 0;
    }
    log_msg(LOG_GS, fresh
            ? "FRESHLY IMAGED machine - provisioning game library"
            : "no provisioning marker - provisioning game library");
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

/*
 * DRVUPDATE <hardware-id> [inf-path]
 *
 * Force a device onto a specific driver, even when it already has a working
 * one. The automatic repair only touches devices carrying a PROBLEM code, and
 * that is the right default - but it leaves the case that actually bit us: a
 * GeForce2 GTS running Microsoft's in-box nv4_disp 5.6.7.3, reporting status OK
 * and rendering Quake III at a crawl because the in-box driver has barely any
 * OpenGL. Nothing is broken by Windows' reckoning; it is just the wrong driver.
 *
 * With no INF given it searches the staged tree, so "DRVUPDATE PCI\VEN_10DE&DEV_0150"
 * is enough once the right driver is in the image.
 */
/* ---- DRIVERS UPDATE (1.89.0) ---------------------------------------------- *
 * The user's "keep every driver on the box correct, except 3dfx", for XP.
 * Targets: devices whose problem a driver can fix, plus (tier generic) display
 * adapters on the VGA stub. Never 3dfx (drvsafe - the device, and every other
 * device the forced id reaches), never the operator's disabled devices, never
 * a display adapter or a Display-class INF from the automatic pass, at most two
 * boots per device (DriverFixes) unless a manual run says `retry`.
 * Candidates: PREFER.TXT first (it names the build the fleet wants - the
 * ranking alone gives a GeForce2 the 270.61 MOBILE driver), then the local
 * C:\D when a fresh image still has it, else the share's store through its
 * index (scripts/fleet/driverstore.py: DRVINDEX\<bucket>.TXT, built with the
 * agent's own matcher). A real install copies only the candidate's DIRECTORY
 * (size + mtime resume, free space checked, deleted again afterwards); a dry
 * run judges the INF where it sits on the share and copies nothing. Every
 * candidate is payload-checked and confirmed by Windows (gs_candidate_ok)
 * before the same forced, non-interactive, watchdogged install the
 * fresh-image pass uses. Every outcome says what happened - "the index was
 * unreachable" and "an earlier install hung" are never "nothing to do". */
#define GS_STORE_DEF   "\\\\192.168.1.122\\files\\Files\\OS\\XPSP3-FLEET\\$OEM$\\$1\\D"
#define GS_INDEX_DEF   "\\\\192.168.1.122\\files\\Files\\OS\\XPSP3-FLEET\\DRVINDEX"
#define GS_LSTORE      "C:\\RETRO_AGENT\\DRVSTORE"
#define GS_BUCKET_MAX  64
#define GS_UPD_MAX     GS_MAX_PROBDEV

typedef struct { char name[16]; char *text; } gs_bucket;

typedef struct {
    char        desc[128], id[200], inf[MAX_PATH], detail[128];
    const char *outcome;
    int         reboot;
} gs_updres;

/* The store and its index are XP x86 drivers: XP (5.1) only. On Windows 7 an
 * undecorated XP models section is still accepted, so an XP driver would be
 * forced onto it; and the signing-policy key the pass relaxes is XP's. */
static int gs_drvupd_os_ok(void)
{
    DWORD v = GetVersion();
    return !(v & 0x80000000UL) && LOBYTE(LOWORD(v)) == 5 && HIBYTE(LOWORD(v)) == 1;
}

static void gs_agent_str(const char *name, const char *def, char *out, DWORD cch)
{
    HKEY  k;
    DWORD type = 0, sz = cch;
    lstrcpynA(out, def, cch);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS || type != REG_SZ || !out[0])
        lstrcpynA(out, def, cch);
    out[cch - 1] = 0;
    RegCloseKey(k);
}

/* One bucket's text, cached. A bucket that does not exist is normal (the store
 * has nothing for that vendor); a FULL cache is not, and is reported. */
static const char *gs_bucket_text(gs_bucket *c, int *nc, int *full, const char *index_root,
                                  const char *bucket)
{
    char path[MAX_PATH];
    int  i;
    for (i = 0; i < *nc; i++)
        if (strcmp(c[i].name, bucket) == 0)
            return c[i].text;
    if (*nc >= GS_BUCKET_MAX) {
        *full = 1;
        return NULL;
    }
    _snprintf(path, sizeof(path) - 1, "%s\\%s.TXT", index_root, bucket);
    path[sizeof(path) - 1] = 0;
    lstrcpynA(c[*nc].name, bucket, sizeof(c[*nc].name));
    c[*nc].text = gs_slurp(path, 4 * 1024 * 1024);
    return c[(*nc)++].text;
}

static void gs_cand_add_unique(drvmatch_cands *c, int idx, const char *path)
{
    int i;
    for (i = 0; i < c->n; i++)
        if (_stricmp(c->path[i], path) == 0)
            return;
    drvmatch_cand_add(c, idx, path);
}

/* Candidates for one device from the store index: every line whose id equals
 * one of the device's ids (whole id, drvstore_match), ranked by id specificity
 * then index order. Paths are store-relative "DIR\INF", validated. */
static void gs_store_candidates(gs_probdev *d, gs_bucket *c, int *nc, int *full,
                                const char *index_root)
{
    int i;
    d->cand.n = 0;
    for (i = 0; i < d->nids; i++) {
        char        b[16], rel[MAX_PATH];
        const char *t;
        drvstore_bucket(d->ids[i], b);
        t = gs_bucket_text(c, nc, full, index_root, b);
        while (t && drvstore_match(&t, d->ids[i], rel, sizeof(rel)))
            if (drvstore_rel_ok(rel))
                gs_cand_add_unique(&d->cand, i, rel);
    }
}

/* PREFER.TXT (from the local C:\D or the store) names this device? Then its
 * INF goes first, and the id forced is the PREFERENCE's id - the one the INF
 * names (gs_apply_driver_prefs does the same). Its lines say C:\D\DIR\INF: kept
 * as-is for the local tree, made store-relative ("DIR\INF") for the store. */
static void gs_prefer_first(gs_probdev *d, const char *prefer_txt, int local,
                            char *pref_id, size_t pref_cch)
{
    const char *line, *next, *p;
    char        devids[2200];
    size_t      len = 0;
    int         pass;

    pref_id[0] = 0;
    if (!prefer_txt)
        return;
    devids[len++] = '\n';
    for (pass = 0; pass < 2; pass++)
        for (p = pass ? d->compat : d->hw; *p; p += strlen(p) + 1) {
            size_t m = strlen(p);
            if (len + m + 2 >= sizeof(devids))
                break;
            memcpy(devids + len, p, m);
            len += m;
            devids[len++] = '\n';
        }
    devids[len] = 0;
    for (line = prefer_txt; *line; line = next) {
        char   buf[600], *hw = NULL, *inf = NULL, hwid[256];
        size_t n;
        next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        n = (size_t)(next - line) < sizeof(buf) - 1 ? (size_t)(next - line) : sizeof(buf) - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        if (!drvpref_split(buf, &hw, &inf))
            continue;
        lstrcpynA(hwid, hw, sizeof(hwid));
        CharUpperA(hwid);
        if (drvpref_present(devids, hwid)) {
            const char    *rel = inf;
            drvmatch_cands keep = d->cand;
            int            k;
            if (!local) {
                if (_strnicmp(rel, "C:\\D\\", 5) == 0)
                    rel += 5;
                if (!drvstore_rel_ok(rel))
                    return;                     /* not a store path we will join */
            }
            d->cand.n = 0;
            drvmatch_cand_add(&d->cand, -1, rel);         /* ranks above every id */
            for (k = 0; k < keep.n; k++)
                if (_stricmp(keep.path[k], rel) != 0)
                    drvmatch_cand_add(&d->cand, keep.idx[k], keep.path[k]);
            lstrcpynA(pref_id, hwid, (int)pref_cch);
            return;
        }
    }
}

/* Is this device a display adapter? Its class says so once it has a driver;
 * WITHOUT one XP files it under "Other devices" as a "Video Controller (VGA
 * Compatible)" whose class is not Display - only its PCI class-code id says
 * what it is. */
static int gs_drv_is_display(const char *cls, const gs_probdev *d)
{
    const char *p;
    int         pass;
    if (_stricmp(cls, "Display") == 0)
        return 1;
    for (pass = 0; pass < 2; pass++)
        for (p = pass ? d->compat : d->hw; *p; p += strlen(p) + 1)
            if (_strnicmp(p, "PCI\\CC_03", 9) == 0)
                return 1;
    return 0;
}

/* Does this INF install a Display-class driver? ([Version] Class=Display.) */
static int gs_inf_is_display_class(const char *inf, char *buf)
{
    const char *line, *next;
    int         in_ver = 0;
    if (!gs_read_inf(inf, buf))                 /* upper-cased */
        return 0;
    for (line = buf; *line; line = next) {
        const char *p = line;
        next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        while (p < next && (*p == ' ' || *p == '\t'))
            p++;
        if (*p == '[') {
            in_ver = strncmp(p, "[VERSION]", 9) == 0;
            continue;
        }
        if (!in_ver || strncmp(p, "CLASS", 5) != 0)
            continue;
        p += 5;
        while (p < next && (*p == ' ' || *p == '\t'))
            p++;
        if (*p != '=')
            continue;                           /* ClassGUID, ClassVer ... */
        p++;
        while (p < next && (*p == ' ' || *p == '\t' || *p == '"'))
            p++;
        return strncmp(p, "DISPLAY", 7) == 0 && !drvmatch_idch(p[7]);
    }
    return 0;
}

/* Copy one store directory (files only - the store is flat) to the local
 * store for a REAL install; the local INF path goes to out. A file already
 * there with the same size AND write time is kept (CopyFile carries the source
 * time over - the v1.62.0 GAMESYNC lesson: size alone hides an edit). Checks
 * the free space first and every copy's result. 0 + why on any failure. */
static int gs_store_fetch(const char *store_root, const char *rel, char *out, size_t cap,
                          char *why, size_t why_cch)
{
    char             dir[64], src[MAX_PATH], dst[MAX_PATH], pat[MAX_PATH];
    const char      *bs = strchr(rel, '\\');
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    size_t           dl;
    __int64          need = 0, freeb;

    why[0] = 0;
    if (!drvstore_rel_ok(rel) || !bs || (dl = (size_t)(bs - rel)) == 0 || dl >= sizeof(dir)) {
        lstrcpynA(why, "bad store path", (int)why_cch);
        return 0;
    }
    memcpy(dir, rel, dl);
    dir[dl] = 0;
    _snprintf(pat, sizeof(pat) - 1, "%s\\%s\\*", store_root, dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        _snprintf(why, why_cch - 1, "store directory unreachable (%lu)", GetLastError());
        why[why_cch - 1] = 0;
        return 0;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            need += ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    freeb = gs_free_bytes("C:\\");
    if (freeb >= 0 && freeb < need + gs_free_margin()) {
        _snprintf(why, why_cch - 1, "no room: %lu MB needed, %lu MB free",
                  (unsigned long)(need >> 20), (unsigned long)(freeb >> 20));
        why[why_cch - 1] = 0;
        return 0;
    }
    CreateDirectoryA(GS_LSTORE, NULL);
    _snprintf(dst, sizeof(dst) - 1, "%s\\%s", GS_LSTORE, dir);
    dst[sizeof(dst) - 1] = 0;
    CreateDirectoryA(dst, NULL);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        lstrcpynA(why, "store directory vanished", (int)why_cch);
        return 0;
    }
    do {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        _snprintf(src, sizeof(src) - 1, "%s\\%s\\%s", store_root, dir, fd.cFileName);
        src[sizeof(src) - 1] = 0;
        _snprintf(dst, sizeof(dst) - 1, "%s\\%s\\%s", GS_LSTORE, dir, fd.cFileName);
        dst[sizeof(dst) - 1] = 0;
        if (GetFileAttributesExA(dst, GetFileExInfoStandard, &a) && a.nFileSizeLow == fd.nFileSizeLow
                && a.nFileSizeHigh == fd.nFileSizeHigh
                && CompareFileTime(&a.ftLastWriteTime, &fd.ftLastWriteTime) == 0)
            continue;
        SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileA(src, dst, FALSE)) {
            _snprintf(why, why_cch - 1, "copy of %s failed (%lu)", fd.cFileName, GetLastError());
            why[why_cch - 1] = 0;
            FindClose(h);
            return 0;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    _snprintf(out, cap - 1, "%s\\%s", GS_LSTORE, rel);
    out[cap - 1] = 0;
    if (!gs_file_exists(out)) {
        lstrcpynA(why, "INF missing after the copy", (int)why_cch);
        return 0;
    }
    return 1;
}

/* After a real install: Windows has copied what it needs (oemN.inf, the
 * driver files), so the local copy of the store directory goes. */
static void gs_store_drop(const char *rel)
{
    char             dir[64], pat[MAX_PATH], f[MAX_PATH];
    const char      *bs = strchr(rel, '\\');
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    size_t           dl;

    if (!drvstore_rel_ok(rel) || !bs || (dl = (size_t)(bs - rel)) == 0 || dl >= sizeof(dir))
        return;
    memcpy(dir, rel, dl);
    dir[dl] = 0;
    _snprintf(pat, sizeof(pat) - 1, "%s\\%s\\*", GS_LSTORE, dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            _snprintf(f, sizeof(f) - 1, "%s\\%s\\%s", GS_LSTORE, dir, fd.cFileName);
            f[sizeof(f) - 1] = 0;
            SetFileAttributesA(f, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(f);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    _snprintf(f, sizeof(f) - 1, "%s\\%s", GS_LSTORE, dir);
    f[sizeof(f) - 1] = 0;
    RemoveDirectoryA(f);
    RemoveDirectoryA(GS_LSTORE);                /* only succeeds once empty */
}

/* What the caller asked for. */
typedef struct {
    int want_generic;   /* also display adapters on the VGA stub */
    int allow_display;  /* display devices / Display-class INFs at all (never automatic) */
    int dry;            /* decide and report; install nothing, copy nothing */
    int skip_seen;      /* leave devices the image pass judged this boot to it */
    int retry;          /* ignore the two-boot cap (manual only) */
} gs_updopts;

static gs_updres *gs_updres_add(gs_updres *res, int maxres, int *nres, const gs_probdev *d,
                                const char *outcome)
{
    gs_updres *r;
    if (*nres >= maxres)
        return NULL;
    r = &res[(*nres)++];
    memset(r, 0, sizeof(*r));
    lstrcpynA(r->desc, d->desc[0] ? d->desc : d->hw, sizeof(r->desc));
    lstrcpynA(r->id, d->hw, sizeof(r->id));
    r->outcome = outcome;
    return r;
}

/* The core. Returns installs made; one result row per device looked at. */
static int gs_drivers_update_locked(const gs_updopts *o, gs_updres *res, int maxres, int *nres,
                                    char *note, int cch)
{
    HDEVINFO         set;
    SP_DEVINFO_DATA  dev;
    gs_probdev      *pd;
    gs_bucket        cache[GS_BUCKET_MAX];
    HMODULE          newdev = NULL;
    updrv_fn         update = NULL;
    char            *buf, *prefer = NULL, *prefid;
    char             store[MAX_PATH], index_root[MAX_PATH], manifest[MAX_PATH], done_hw[GS_UPD_MAX][200];
    BOOL             was_nonint = FALSE;
    gs_signing_saved signing;
    int              n = 0, k, i, nc = 0, full = 0, truncated = 0, index_ok = 1, ndone_hw = 0;
    int              local = gs_file_exists(GS_DRIVER_DIR), done = 0, hung_before = g_gs_install_hung;
    DWORD            j;

    *nres = 0;
    note[0] = 0;
    memset(cache, 0, sizeof(cache));
    if (!o->dry && !hung_before) {
        newdev = LoadLibraryA("newdev.dll");
        update = newdev ? (updrv_fn)GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesA") : NULL;
        if (!update) {
            if (newdev) FreeLibrary(newdev);
            lstrcpynA(note, "newdev.dll unavailable - no driver can be installed", cch);
            return 0;
        }
    }
    gs_agent_str("DriverStore", GS_STORE_DEF, store, sizeof(store));
    gs_agent_str("DriverIndex", GS_INDEX_DEF, index_root, sizeof(index_root));
    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    pd = (gs_probdev *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, GS_UPD_MAX * sizeof(gs_probdev));
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_INF_READ_MAX + 2);
    prefid = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, GS_UPD_MAX * 256);
    if (set == INVALID_HANDLE_VALUE || !pd || !buf || !prefid) {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        if (pd) HeapFree(GetProcessHeap(), 0, pd);
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        if (prefid) HeapFree(GetProcessHeap(), 0, prefid);
        if (newdev) FreeLibrary(newdev);
        lstrcpynA(note, "cannot enumerate devices", cch);
        return 0;
    }

    /* targets: problem devices a driver can fix ... */
    n = gs_problem_devices(set, pd, GS_UPD_MAX, &truncated);
    for (k = 0; k < n; ) {
        char        cls[32] = "";
        const char *why = NULL;
        SetupDiGetDeviceRegistryPropertyA(set, &pd[k].dev, SPDRP_CLASS, NULL, (PBYTE)cls, sizeof(cls) - 1, NULL);
        if (pd[k].excl)
            why = "excluded_3dfx";
        else if (!drvmatch_problem_driver_fixable(pd[k].problem))
            why = "problem_not_driver_fixable";
        else if (!o->allow_display && gs_drv_is_display(cls, &pd[k]))
            why = "display_explicit_only";
        else if (o->skip_seen && gs_dv_lookup(pd[k].hw) != DRVMATCH_V_UNSEEN)
            why = "left_to_image_pass";
        if (!why) {
            k++;
            continue;
        }
        gs_updres_add(res, maxres, nres, &pd[k], why);      /* reported, not touched */
        memmove(&pd[k], &pd[k + 1], (size_t)(n - k - 1) * sizeof(gs_probdev));
        n--;
        for (i = k; i < n; i++)                  /* ids point into hw/compat: re-point */
            pd[i].nids = drvmatch_collect(pd[i].hw, pd[i].compat, pd[i].ids, DRVMATCH_MAX_IDS);
    }
    /* ... and, for tier generic, display adapters on the stub: not disabled,
     * not already a target above, and never 3dfx (reported). */
    if (o->want_generic && o->allow_display) {
        memset(&dev, 0, sizeof(dev));
        dev.cbSize = sizeof(dev);
        for (j = 0; n < GS_UPD_MAX && SetupDiEnumDeviceInfo(set, j, &dev); j++) {
            char  cls[32] = "", drvkey[200] = "", match[160], desc[128] = "";
            DWORD status = 0, problem = 0;
            int   dup = 0;
            SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_CLASS, NULL, (PBYTE)cls, sizeof(cls) - 1, NULL);
            if (_stricmp(cls, "Display") != 0)
                continue;
            SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DRIVER, NULL, (PBYTE)drvkey, sizeof(drvkey) - 1, NULL);
            SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL, (PBYTE)desc, sizeof(desc) - 1, NULL);
            gs_class_value(drvkey, "MatchingDeviceId", match, sizeof(match));
            if (!drvplan_display_stub(cls, match, desc))
                continue;
            if (ntdyn_CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) == CR_SUCCESS
                    && (problem == 22 || problem == 29))
                continue;                       /* the operator's (or the BIOS's) choice */
            for (i = 0; i < n && !dup; i++)
                dup = pd[i].dev.DevInst == dev.DevInst;
            if (dup)
                continue;
            memset(&pd[n], 0, sizeof(gs_probdev));
            pd[n].dev = dev;
            pd[n].problem = problem;
            lstrcpynA(pd[n].desc, desc, sizeof(pd[n].desc));
            gs_device_ids(set, &pd[n]);
            pd[n].excl = gs_device_3dfx(set, &dev, pd[n].hw, pd[n].compat);
            if (pd[n].excl) {
                gs_updres_add(res, maxres, nres, &pd[n], "excluded_3dfx");
                continue;
            }
            n++;
        }
    }

    /* candidates */
    if (local) {
        prefer = gs_slurp(GS_PREFER_FILE, 262144);
        if (n && gs_scan_driver_tree(pd, n) < 0) {
            lstrcpynA(note, "could not search C:\\D", cch);
            index_ok = 0;
        }
    } else {
        char pp[MAX_PATH];
        _snprintf(manifest, sizeof(manifest) - 1, "%s\\MANIFEST.TXT", index_root);
        manifest[sizeof(manifest) - 1] = 0;
        index_ok = gs_file_exists(manifest);
        if (!index_ok) {
            _snprintf(note, cch - 1, "DRIVER-STORE INDEX UNREACHABLE (%s) - nothing could be looked up",
                      manifest);
            note[cch - 1] = 0;
        }
        _snprintf(pp, sizeof(pp) - 1, "%s\\PREFER.TXT", store);
        pp[sizeof(pp) - 1] = 0;
        prefer = gs_slurp(pp, 262144);
        for (k = 0; k < n && index_ok; k++)
            gs_store_candidates(&pd[k], cache, &nc, &full, index_root);
    }
    for (k = 0; k < n; k++)
        gs_prefer_first(&pd[k], prefer, local, prefid + k * 256, 256);

    if (!o->dry && !hung_before) {
        gs_sdi_resolve();
        if (g_sdi_nonint)
            was_nonint = g_sdi_nonint(TRUE);
        gs_signing_relax(&signing);
    }
    for (k = 0; k < n; k++) {
        gs_updres *r = gs_updres_add(res, maxres, nres, &pd[k],
                                     pd[k].cand.n ? "no_candidate_confirmed" : "no_candidate");
        DWORD      tries, status = 0, problem = 0;
        int        c, bumped = 0, twin = 0;
        if (!r)
            break;
        if (!index_ok) {
            r->outcome = local ? "tree_unsearchable" : "index_unreachable";
            continue;
        }
        if (g_gs_install_hung && !o->dry) {
            r->outcome = "skipped_install_hung";
            lstrcpynA(r->detail, "an earlier forced install never returned - restart the agent", sizeof(r->detail));
            continue;
        }
        for (i = 0; i < ndone_hw && !twin; i++)
            twin = strcmp(done_hw[i], pd[k].hw) == 0;
        if (twin) {
            r->outcome = "same_as_earlier_device";   /* one forced install covers identical devices */
            continue;
        }
        if (!o->dry && pd[k].problem && ntdyn_CM_Get_DevNode_Status(&status, &problem, pd[k].dev.DevInst, 0)
                == CR_SUCCESS && problem == 0 && !(status & DN_HAS_PROBLEM)) {
            r->outcome = "fixed_by_earlier_install";
            continue;
        }
        tries = gs_drvfix_attempts(pd[k].hw, 0);
        if (tries >= 2 && !o->retry) {
            r->outcome = "tried_out";
            _snprintf(r->detail, sizeof(r->detail) - 1, "%lu boots already tried (DRIVERS UPDATE ... retry)",
                      (unsigned long)tries);
            continue;
        }
        for (c = 0; c < pd[k].cand.n; c++) {
            const char *id = pd[k].cand.idx[c] >= 0 ? pd[k].ids[pd[k].cand.idx[c]] : prefid + k * 256;
            char        inf[MAX_PATH], why[128];
            BOOL        reboot = FALSE;
            DWORD       err = 0;
            int         ok, rc, fetched = 0;

            if (!id[0])
                continue;
            if (local) {
                lstrcpynA(inf, pd[k].cand.path[c], sizeof(inf));
            } else if (o->dry) {
                /* judge it where it sits: a dry run copies nothing */
                _snprintf(inf, sizeof(inf) - 1, "%s\\%s", store, pd[k].cand.path[c]);
                inf[sizeof(inf) - 1] = 0;
            } else if (gs_store_fetch(store, pd[k].cand.path[c], inf, sizeof(inf), why, sizeof(why))) {
                fetched = 1;
            } else {
                r->outcome = "fetch_failed";
                _snprintf(r->detail, sizeof(r->detail) - 1, "%s: %s", pd[k].cand.path[c], why);
                r->detail[sizeof(r->detail) - 1] = 0;
                gs_store_drop(pd[k].cand.path[c]);
                continue;
            }
            if (gs_inf_is_3dfx(inf) || gs_hwid_touches_3dfx(id)) {
                /* the INF, or ANY present device the forced id reaches */
                r->outcome = "excluded_3dfx";
                if (fetched) gs_store_drop(pd[k].cand.path[c]);
                continue;
            }
            if (!o->allow_display && gs_inf_is_display_class(inf, buf)) {
                r->outcome = "display_explicit_only";
                if (fetched) gs_store_drop(pd[k].cand.path[c]);
                continue;
            }
            ok = gs_candidate_ok(set, &pd[k].dev, inf, id, r->desc, buf);
            if (ok == 0) {
                if (fetched) gs_store_drop(pd[k].cand.path[c]);
                continue;
            }
            lstrcpynA(r->inf, inf, sizeof(r->inf));
            if (ok < 0)
                lstrcpynA(r->detail, "Windows could not confirm the INF serves it", sizeof(r->detail));
            if (o->dry) {
                r->outcome = "would_install";
                break;
            }
            if (!bumped++)
                gs_drvfix_attempts(pd[k].hw, 1);     /* one per device per run */
            log_msg(LOG_GS, "DRIVERS UPDATE: installing %s for %s (matched %s%s)", inf, r->desc, id,
                    ok < 0 ? ", unconfirmed" : "");
            rc = gs_force_install(update, id, inf, &reboot, &err);
            if (rc < 0) {
                g_gs_install_hung = 1;
                r->outcome = "hung";
                lstrcpynA(r->detail, "the install never returned - probably a prompt on the console",
                          sizeof(r->detail));
                log_msg(LOG_GS, "DRIVERS UPDATE: INSTALL DID NOT RETURN after %d min - no more forced "
                                "installs until the agent restarts", GS_INSTALL_WAIT / 60000);
                break;                          /* its files stay: the install is still reading them */
            }
            if (fetched)
                gs_store_drop(pd[k].cand.path[c]);
            if (!rc) {
                r->outcome = "failed";
                _snprintf(r->detail, sizeof(r->detail) - 1, "error %lu", err);
                continue;
            }
            if (reboot) {
                r->outcome = "installed_reboot";
                r->reboot = 1;
                done++;
                break;
            }
            Sleep(2000);
            if (ntdyn_CM_Get_DevNode_Status(&status, &problem, pd[k].dev.DevInst, 0) == CR_SUCCESS
                    && (problem != 0 || (status & DN_HAS_PROBLEM)) && drvmatch_problem_driver_fixable(problem)) {
                r->outcome = "installed_still_problem";
                _snprintf(r->detail, sizeof(r->detail) - 1, "problem %lu after the install", problem);
                continue;
            }
            r->outcome = "installed";
            done++;
            break;
        }
        if (done && ndone_hw < GS_UPD_MAX && (!strcmp(r->outcome, "installed")
                                              || !strcmp(r->outcome, "installed_reboot")))
            lstrcpynA(done_hw[ndone_hw++], pd[k].hw, sizeof(done_hw[0]));
        log_msg(LOG_GS, "DRIVERS UPDATE: %s -> %s %s %s", r->desc, r->outcome, r->inf, r->detail);
    }
    if (!o->dry && !hung_before) {
        gs_signing_restore(&signing);
        /* Not while a hung install is still inside setupapi: turning prompts
         * back on under it is how it would get one (gs_install_missing_drivers
         * does the same). */
        if (g_sdi_nonint && !g_gs_install_hung)
            g_sdi_nonint(was_nonint);
    }
    for (i = 0; i < nc; i++)
        if (cache[i].text)
            HeapFree(GetProcessHeap(), 0, cache[i].text);
    if (prefer)
        HeapFree(GetProcessHeap(), 0, prefer);
    if (!note[0])
        _snprintf(note, cch - 1, "store %s%s%s%s", local ? GS_DRIVER_DIR : store,
                  truncated ? "; DEVICE LIST TRUNCATED" : "",
                  full ? "; INDEX CACHE FULL - some ids not looked up" : "",
                  g_gs_install_hung ? "; AN INSTALL HUNG - no more forced installs until the agent restarts" : "");
    note[cch - 1] = 0;
    HeapFree(GetProcessHeap(), 0, prefid);
    HeapFree(GetProcessHeap(), 0, buf);
    HeapFree(GetProcessHeap(), 0, pd);
    SetupDiDestroyDeviceInfoList(set);
    /* newdev.dll stays loaded while an install thread is still inside it -
     * unloading it under that thread kills the agent when the dialog closes. */
    if (newdev && !g_gs_install_hung)
        FreeLibrary(newdev);
    return done;
}

static int gs_drivers_update_core(const gs_updopts *o, gs_updres *res, int maxres, int *nres,
                                  char *note, int cch)
{
    int done;
    if (!gs_drv_busy_enter()) {
        *nres = 0;
        lstrcpynA(note, "another driver install is running - nothing done", cch);
        return 0;
    }
    done = gs_drivers_update_locked(o, res, maxres, nres, note, cch);
    /* A hung install still owns SetupAPI: the lock stays held, so nothing
     * else forces an install into it before the agent restarts. */
    if (!g_gs_install_hung)
        gs_drv_busy_leave();
    return done;
}

/* DRIVERS UPDATE [missing|generic|all] [dry] [retry] - XP. The host policy
 * and the OS gate are enforced by handle_drivers (video.c) too. */
void gs_drivers_update(SOCKET sock, const char *args)
{
    gs_updres *res;
    gs_updopts o;
    int        nres = 0, k, done;
    char       note[300], bad[32];
    json_t     j;
    char      *out;

    memset(&o, 0, sizeof(o));
    o.allow_display = 1;                        /* a manual run may; the automatic pass may not */
    if (!drvplan_parse_update(args, &o.want_generic, &o.dry, &o.retry, bad, sizeof(bad))) {
        char msg[160];
        _snprintf(msg, sizeof(msg) - 1, "DRIVERS UPDATE: unknown word '%s' - use "
                  "[missing|generic|all] [dry] [retry]; nothing done", bad);
        msg[sizeof(msg) - 1] = 0;
        send_error_response(sock, msg);
        return;
    }
    if (!gs_drvupd_os_ok()) {
        send_error_response(sock, "DRIVERS UPDATE: Windows XP only - the store holds XP x86 drivers");
        return;
    }
    res = (gs_updres *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 2 * GS_UPD_MAX * sizeof(gs_updres));
    if (!res) {
        send_error_response(sock, "DRIVERS UPDATE: out of memory");
        return;
    }
    done = gs_drivers_update_core(&o, res, 2 * GS_UPD_MAX, &nres, note, sizeof(note));
    json_init(&j);
    json_object_start(&j);
    json_kv_bool(&j, "dry", o.dry);
    json_kv_str(&j, "tier", o.want_generic ? "missing+generic" : "missing");
    json_kv_bool(&j, "retry", o.retry);
    json_kv_str(&j, "note", note);
    json_kv_int(&j, "installed", done);
    json_key(&j, "devices");
    json_array_start(&j);
    for (k = 0; k < nres; k++) {
        json_object_start(&j);
        json_kv_str(&j, "desc", res[k].desc);
        json_kv_str(&j, "id", res[k].id);
        json_kv_str(&j, "outcome", res[k].outcome ? res[k].outcome : "");
        json_kv_str(&j, "inf", res[k].inf);
        json_kv_str(&j, "detail", res[k].detail);
        json_kv_bool(&j, "reboot", res[k].reboot);
        json_object_end(&j);
    }
    json_array_end(&j);
    json_object_end(&j);
    out = json_finish(&j);
    if (out) {
        send_text_response(sock, out);
        HeapFree(GetProcessHeap(), 0, out);
    } else {
        send_error_response(sock, "DRIVERS UPDATE: out of memory");
    }
    HeapFree(GetProcessHeap(), 0, res);
}

static void gs_drvupd_record(const char *text)
{
    HKEY       key;
    SYSTEMTIME t;
    char       v[480];
    GetLocalTime(&t);
    _snprintf(v, sizeof(v) - 1, "%04u-%02u-%02u %02u:%02u %s", t.wYear, t.wMonth, t.wDay, t.wHour,
              t.wMinute, text);
    v[sizeof(v) - 1] = 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, NULL, 0, KEY_WRITE, NULL, &key,
                        NULL) == ERROR_SUCCESS) {
        RegSetValueExA(key, "DriverUpdateBoot", 0, REG_SZ, (const BYTE *)v, (DWORD)strlen(v) + 1);
        RegCloseKey(key);
    }
}

/* The automatic pass at every XP start, after the image pass: missing drivers
 * only, never display, never 3dfx. Switch: HKLM\Software\RetroAgent
 * DriverUpdate = 0. DriverUpdateBoot is dated, and reads PENDING until this
 * start's pass has finished - the last boot's answer is never today's. */
static void gs_drivers_autopass(void)
{
    gs_updres *res;
    gs_updopts o;
    int        nres = 0, done, k;
    DWORD      on = 1, sz = sizeof(on), type = 0;
    HKEY       key;
    char       note[300], summary[400];

    if (!gs_drvupd_os_ok())
        return;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\RetroAgent", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExA(key, "DriverUpdate", NULL, &type, (LPBYTE)&on, &sz) != ERROR_SUCCESS || type != REG_DWORD)
            on = 1;
        RegCloseKey(key);
    }
    if (!on) {
        log_msg(LOG_GS, "driver update pass: disabled by DriverUpdate=0");
        gs_drvupd_record("DISABLED (DriverUpdate=0)");
        return;
    }
    gs_drvupd_record("PENDING: this start's pass has not finished");
    res = (gs_updres *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 2 * GS_UPD_MAX * sizeof(gs_updres));
    if (!res) {
        gs_drvupd_record("FAILED: out of memory");
        return;
    }
    memset(&o, 0, sizeof(o));
    o.skip_seen = 1;                            /* want_generic, allow_display, dry, retry: 0 */
    done = gs_drivers_update_core(&o, res, 2 * GS_UPD_MAX, &nres, note, sizeof(note));
    _snprintf(summary, sizeof(summary) - 1, "%d installed, %d device(s) looked at; %s", done, nres, note);
    summary[sizeof(summary) - 1] = 0;
    for (k = 0; k < nres; k++) {
        size_t l = strlen(summary);
        if (l + 80 >= sizeof(summary)) {
            _snprintf(summary + l, sizeof(summary) - l - 1, "; +%d more", nres - k);
            break;
        }
        _snprintf(summary + l, sizeof(summary) - l - 1, "; %.40s: %s", res[k].desc, res[k].outcome ? res[k].outcome : "");
    }
    summary[sizeof(summary) - 1] = 0;
    log_msg(LOG_GS, "driver update pass: %s", summary);
    gs_drvupd_record(summary);
    HeapFree(GetProcessHeap(), 0, res);
}

/* ---- DRIVERS STATUS / PLAN on NT (1.88.0) -------------------------------- *
 * Report-only: every present device, its driver, and ONE state from
 * agent/shared/drvplan.h. PLAN also ranks C:\D candidates for the missing and
 * generic ones, the same way the install pass would (3dfx INFs never count). */
typedef struct {
    char          id[200], desc[128], cls[32], prov[64], date[24], ver[40], inf[32], match[160];
    unsigned long problem;
    int           state, excl, pd;
} gs_drvrec;

#define GS_DRVREC_MAX 400

void gs_drivers_status(SOCKET sock, int plan)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dev;
    DWORD           i;
    gs_drvrec      *rec;
    gs_probdev     *pd = NULL, *one;
    int             n = 0, npd = 0, k, counts[DRVST_COUNT], truncated = 0, scan = 1;
    int             store = gs_file_exists(GS_DRIVER_DIR);
    json_t          j;
    char           *out;

    memset(counts, 0, sizeof(counts));
    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        send_error_response(sock, "DRIVERS STATUS: cannot enumerate devices");
        return;
    }
    rec = (gs_drvrec *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, GS_DRVREC_MAX * sizeof(gs_drvrec));
    one = (gs_probdev *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(gs_probdev));
    if (plan && store)
        pd = (gs_probdev *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, GS_MAX_PROBDEV * sizeof(gs_probdev));
    if (!rec || !one || (plan && store && !pd)) {
        if (rec) HeapFree(GetProcessHeap(), 0, rec);
        if (one) HeapFree(GetProcessHeap(), 0, one);
        if (pd) HeapFree(GetProcessHeap(), 0, pd);
        SetupDiDestroyDeviceInfoList(set);
        send_error_response(sock, "DRIVERS STATUS: out of memory");
        return;
    }
    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        gs_drvrec *r;
        char       drvkey[200];
        DWORD      status = 0, problem = 0;

        if (n >= GS_DRVREC_MAX) { truncated = 1; break; }
        r = &rec[n];
        r->pd = -1;
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME, NULL, (PBYTE)r->desc, sizeof(r->desc) - 1, NULL))
            SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL, (PBYTE)r->desc, sizeof(r->desc) - 1, NULL);
        SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_CLASS, NULL, (PBYTE)r->cls, sizeof(r->cls) - 1, NULL);
        drvkey[0] = 0;
        SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DRIVER, NULL, (PBYTE)drvkey, sizeof(drvkey) - 1, NULL);
        drvkey[sizeof(drvkey) - 1] = 0;
        if (ntdyn_CM_Get_Device_IDA(dev.DevInst, r->id, sizeof(r->id), 0) != CR_SUCCESS)
            r->id[0] = 0;
        ntdyn_CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0);
        r->problem = problem;
        gs_class_value(drvkey, "ProviderName", r->prov, sizeof(r->prov));
        gs_class_value(drvkey, "DriverDate", r->date, sizeof(r->date));
        gs_class_value(drvkey, "DriverVersion", r->ver, sizeof(r->ver));
        gs_class_value(drvkey, "InfPath", r->inf, sizeof(r->inf));
        gs_class_value(drvkey, "MatchingDeviceId", r->match, sizeof(r->match));
        one->dev = dev;
        gs_device_ids(set, one);
        r->excl = gs_device_3dfx(set, &dev, one->hw, one->compat);
        r->state = drvplan_state(problem, drvkey[0] != 0, 0, r->excl, r->cls, r->match, r->desc);
        counts[r->state]++;
        if (pd && (r->state == DRVST_MISSING || r->state == DRVST_GENERIC) && npd < GS_MAX_PROBDEV) {
            memcpy(&pd[npd], one, sizeof(gs_probdev));
            pd[npd].nids = drvmatch_collect(pd[npd].hw, pd[npd].compat, pd[npd].ids, DRVMATCH_MAX_IDS);
            r->pd = npd++;
        }
        n++;
    }
    if (pd && npd)
        scan = gs_scan_driver_tree(pd, npd) == 0;

    json_init(&j);
    json_object_start(&j);
    json_kv_str(&j, "os", "nt");
    json_kv_bool(&j, "plan", plan);
    json_kv_str(&j, "store", store ? GS_DRIVER_DIR : "none");
    json_kv_bool(&j, "truncated", truncated);
    json_key(&j, "counts");
    json_object_start(&j);
    for (k = 0; k < DRVST_COUNT; k++)
        json_kv_int(&j, drvst_name(k), counts[k]);
    json_object_end(&j);
    json_key(&j, "devices");
    json_array_start(&j);
    for (k = 0; k < n; k++) {
        gs_drvrec *r = &rec[k];
        json_object_start(&j);
        json_kv_str(&j, "id", r->id);
        json_kv_str(&j, "desc", r->desc);
        json_kv_str(&j, "class", r->cls);
        json_kv_str(&j, "state", drvst_name(r->state));
        json_kv_uint(&j, "problem", r->problem);
        json_kv_str(&j, "provider", r->prov);
        json_kv_str(&j, "date", r->date);
        json_kv_str(&j, "version", r->ver);
        json_kv_str(&j, "inf", r->inf);
        json_kv_str(&j, "matching", r->match);
        if (r->excl)
            json_kv_str(&j, "excluded", drvsafe_reason_name(r->excl));
        if (r->pd >= 0) {
            json_kv_str(&j, "candidate", !scan ? "(could not search the store)"
                                         : pd[r->pd].cand.n ? pd[r->pd].cand.path[0] : "");
        }
        json_object_end(&j);
    }
    json_array_end(&j);
    json_object_end(&j);
    out = json_finish(&j);
    if (out) {
        send_text_response(sock, out);
        HeapFree(GetProcessHeap(), 0, out);
    } else {
        send_error_response(sock, "DRIVERS STATUS: out of memory");
    }
    HeapFree(GetProcessHeap(), 0, rec);
    HeapFree(GetProcessHeap(), 0, one);
    if (pd) HeapFree(GetProcessHeap(), 0, pd);
    SetupDiDestroyDeviceInfoList(set);
}

/* Copy `args` without its standalone ALLOW3DFX token(s). */
static void gs_strip_allow(const char *args, char *out, size_t cap)
{
    size_t o = 0, n = strlen(DRVSAFE_ALLOW_TOKEN);
    const char *p = args;
    while (*p && o + 1 < cap) {
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - s) == n && _strnicmp(s, DRVSAFE_ALLOW_TOKEN, n) == 0) {
            while (*p == ' ' || *p == '\t') p++;
            continue;                   /* drop the token and its trailing blanks */
        }
        while (s < p && o + 1 < cap) out[o++] = *s++;
        while ((*p == ' ' || *p == '\t') && o + 1 < cap) out[o++] = *p++;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = 0;
}

void handle_drvupdate(SOCKET sock, const char *args_in)
{
    char     hwid[256], inf[MAX_PATH], argsbuf[600];
    HMODULE  newdev;
    updrv_fn update;
    BOOL     reboot = FALSE, ok;
    const char *sp, *args = argsbuf;
    int      allow3dfx, why3dfx;

    /* ALLOW3DFX is the chat's explicit "yes, this 3dfx device" - one device,
     * never ALL, never persisted (agent/shared/drvsafe.h). */
    allow3dfx = drvsafe_args_allow(args_in);
    gs_strip_allow(args_in, argsbuf, sizeof(argsbuf));
    while (*args == ' ')
        args++;
    if (!*args) {
        send_error_response(sock, "usage: DRVUPDATE <hardware-id> [inf-path] [ALLOW3DFX]");
        return;
    }
    sp = strchr(args, ' ');
    if (sp) {
        int n = (int)(sp - args);
        if (n >= (int)sizeof(hwid))
            n = sizeof(hwid) - 1;
        memcpy(hwid, args, n);
        hwid[n] = 0;
        while (*sp == ' ')
            sp++;
        lstrcpynA(inf, sp, sizeof(inf));
    } else {
        lstrcpynA(hwid, args, sizeof(hwid));
        inf[0] = 0;
    }
    CharUpperA(hwid);

    why3dfx = gs_hwid_touches_3dfx(hwid);
    if (why3dfx != DRVSAFE_OK && !allow3dfx) {
        char msg[300];
        _snprintf(msg, sizeof(msg) - 1, "refused: %s is a %s - 3dfx drivers are changed "
                  "only on an explicit request (DRVUPDATE <id> <inf> ALLOW3DFX)",
                  hwid, drvsafe_reason_name(why3dfx));
        msg[sizeof(msg) - 1] = 0;
        log_msg(LOG_GS, "DRVUPDATE %s", msg);
        send_error_response(sock, msg);
        return;
    }
    if (allow3dfx && !inf[0]) {
        send_error_response(sock, "ALLOW3DFX needs an explicit INF path: a 3dfx INF is "
                                  "never chosen automatically");
        return;
    }

    if (!inf[0]) {
        /* No device handle here, so no gs_inf_serves(): the text ranking alone
         * picks it. Give an INF path when that is not good enough. */
        gs_probdev *one = (gs_probdev *)HeapAlloc(GetProcessHeap(),
                                                  HEAP_ZERO_MEMORY,
                                                  sizeof(gs_probdev));
        int got = 0;
        if (!one) {
            send_error_response(sock, "out of memory");
            return;
        }
        lstrcpynA(one->hw, hwid, sizeof(one->hw) - 2);
        one->nids = drvmatch_collect(one->hw, NULL, one->ids, DRVMATCH_MAX_IDS);
        if (!one->nids) {
            HeapFree(GetProcessHeap(), 0, one);
            send_error_response(sock, "that id names a family of devices - "
                                      "give the INF path explicitly");
            return;
        }
        /* PREFER.TXT first: it names the build the fleet wants, which the
         * ranking cannot know (PCI\VEN_10DE&DEV_0150 ranks G003's 270.61 MOBILE
         * driver first; PREFER.TXT says G005's 71.89). */
        if (gs_prefer_claims(one) && gs_prefer_inf_for(one, inf, sizeof(inf))) {
            got = 1;
        } else if (gs_scan_driver_tree(one, 1) == 0 && one->cand.n) {
            char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_INF_READ_MAX + 2);
            int   c;
            for (c = 0; buf && c < one->cand.n && !got; c++) {
                char why[160];
                if (gs_inf_payload_ok(one->cand.path[c], buf, why, sizeof(why))) {
                    lstrcpynA(inf, one->cand.path[c], sizeof(inf));
                    got = 1;
                }
            }
            if (buf)
                HeapFree(GetProcessHeap(), 0, buf);
        }
        HeapFree(GetProcessHeap(), 0, one);
        if (!got) {
            send_error_response(sock, "no INF in " GS_DRIVER_DIR " names that hardware id");
            return;
        }
    }
    if (!gs_file_exists(inf)) {
        send_error_response(sock, "INF not found");
        return;
    }
    if (!allow3dfx && gs_inf_is_3dfx(inf)) {
        log_msg(LOG_GS, "DRVUPDATE refused: %s is a 3dfx INF", inf);
        send_error_response(sock, "refused: that is a 3dfx INF - 3dfx drivers are changed "
                                  "only on an explicit request (add ALLOW3DFX, one device)");
        return;
    }
    if (allow3dfx)
        log_msg(LOG_GS, "DRVUPDATE: *** EXPLICIT 3dfx REQUEST (ALLOW3DFX) *** %s -> %s", hwid, inf);

    newdev = LoadLibraryA("newdev.dll");
    update = newdev ? (updrv_fn)GetProcAddress(newdev,
                          "UpdateDriverForPlugAndPlayDevicesA") : NULL;
    if (!update) {
        if (newdev)
            FreeLibrary(newdev);
        send_error_response(sock, "newdev.dll unavailable");
        return;
    }
    if (g_gs_install_hung) {
        FreeLibrary(newdev);
        send_error_response(sock, "DRVUPDATE: an earlier forced install never returned (a prompt on "
                                  "the console?) - restart the agent first");
        return;
    }
    if (!gs_drv_busy_enter()) {
        FreeLibrary(newdev);
        send_error_response(sock, "DRVUPDATE: another driver install is running - try again later");
        return;
    }
    log_msg(LOG_GS, "DRVUPDATE %s -> %s", hwid, inf);
    ok = update(NULL, hwid, inf, INSTALLFLAG_FORCE_, &reboot);
    gs_drv_busy_leave();
    if (ok) {
        char msg[512];
        _snprintf(msg, sizeof(msg) - 1, "OK installed %s for %s%s", inf, hwid,
                  reboot ? " (reboot required)" : "");
        msg[sizeof(msg) - 1] = 0;
        log_msg(LOG_GS, "%s", msg);
        send_text_response(sock, msg);
    } else {
        char msg[256];
        _snprintf(msg, sizeof(msg) - 1, "install failed, error %lu",
                  GetLastError());
        msg[sizeof(msg) - 1] = 0;
        log_msg(LOG_GS, "DRVUPDATE %s", msg);
        send_error_response(sock, msg);
    }
    FreeLibrary(newdev);
}

/* ICONARRANGE [bay|auto] - apply the desktop icon layout now, and REPORT THE
 * POST-CONDITION rather than "OK".
 *
 * A log line saying we set auto-arrange is not evidence that auto-arrange is
 * set; the whole reason this code reads its own writes back is that the shell
 * toggle has silently refused before. So the reply carries the live
 * LVS_AUTOARRANGE bit, the persisted FFlags word and the icon count, which is
 * what a fleet-wide verification sweep actually needs.
 *
 * And the VIEW, and WHERE it read the persisted state from. On .195 this
 * answered "autoarrange":true,"icons":119,"fflags":545 while 8 of the 117
 * icons were visible: the desktop was in List view, and 545 came from a
 * Bags\1\Desktop nothing reads (the real bag was Bags\4). So the reply also
 * names the bag (path + slot + whether BagMRU's NodeSlot named it), the live
 * view (style type bits and LVM_GETVIEW, and a name), the persisted
 * Mode/LogicalViewMode/IconSize (null when absent), and whether THIS call had
 * to repair the view live or persisted. A settled box answers
 * "view":"icon","view_repaired":false,"bag_view_repaired":false.
 *
 * The optional argument overrides the box's stored preference for this call
 * only: "bay" runs the legacy wallpaper-bay layout once, "auto" forces
 * auto-arrange. With no argument it follows
 * HKLM\Software\RetroAgent\IconAutoArrange (default: auto). */
static void gs_json_opt_dword(char *out, size_t cap, int have, unsigned long v)
{
    if (have)
        _snprintf(out, cap - 1, "%lu", v);
    else
        _snprintf(out, cap - 1, "null");
    out[cap - 1] = '\0';
}

void handle_iconarrange(SOCKET sock, const char *args)
{
    HWND  defview = NULL;
    HWND  lv;
    char  msg[1024];
    char  bagesc[sizeof(((gs_bag_t *)0)->path) * 2 + 1];
    char  jmode[16], jlvm[16], jsize[16];
    LONG  style;
    DWORD flags = 0;
    HKEY  hk;
    int   count, have_flags = 0, view_result;
    gs_view_t view;
    const char *mode;

    lv = gs_desktop_listview(&defview);
    if (!lv) {
        send_text_response(sock, "ERR desktop listview not found - is explorer "
                                 "running in this session?");
        return;
    }

    /* The view first, for every mode - see gs_desktop_icons_apply_ex(). */
    view_result = gs_desktop_view_fix(defview, lv, NULL);

    if (args && (str_starts_with(args, "bay") || str_starts_with(args, "BAY"))) {
        mode = "bay";
        gs_arrange_bay(defview, lv);
    } else if (args && (str_starts_with(args, "auto") ||
                        str_starts_with(args, "AUTO"))) {
        mode = "auto";
        gs_apply_autoarrange(defview, lv, 1);
    } else {
        mode = gs_want_autoarrange() ? "auto" : "bay";
        gs_desktop_icons_apply_ex(1);
    }

    /* Post-condition: read everything again, after the pass. */
    view.result = view_result;
    gs_desktop_view_read(lv, &view);
    style = GetWindowLongA(lv, GWL_STYLE);
    count = (int)SendMessageA(lv, LVM_GETITEMCOUNT_, 0, 0);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, view.bag.path, 0, KEY_QUERY_VALUE,
                      &hk) == ERROR_SUCCESS) {
        have_flags = gs_reg_dword(hk, "FFlags", &flags);
        RegCloseKey(hk);
    }
    gs_json_escape(view.bag.path, bagesc, sizeof(bagesc));
    gs_json_opt_dword(jmode, sizeof(jmode), view.bv.has_mode, view.bv.mode);
    gs_json_opt_dword(jlvm, sizeof(jlvm), view.bv.has_lvm, view.bv.lvm);
    gs_json_opt_dword(jsize, sizeof(jsize), view.bv.has_size, view.bv.icon_size);
    _snprintf(msg, sizeof(msg) - 1,
              "{\"mode\":\"%s\",\"autoarrange\":%s,\"fflags\":%lu,"
              "\"fflags_autoarrange\":%s,\"icons\":%d,\"screen\":\"%dx%d\","
              "\"bag\":\"HKCU\\\\%s\",\"bag_slot\":%lu,\"bag_slot_source\":\"%s\","
              "\"bag_exists\":%s,\"view\":\"%s\",\"lv_style_type\":%lu,"
              "\"lv_view\":%ld,\"bag_mode\":%s,\"bag_logical_view_mode\":%s,"
              "\"bag_icon_size\":%s,\"view_repaired\":%s,"
              "\"bag_view_repaired\":%s,\"view_still_wrong\":%s}",
              mode,
              (style & LVS_AUTOARRANGE) ? "true" : "false",
              (unsigned long)flags,
              (have_flags && (flags & GS_FWF_AUTOARRANGE)) ? "true" : "false",
              count,
              GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
              bagesc, view.bag.slot,
              view.bag.from_mru ? "BagMRU NodeSlot" : "default",
              view.bag_open ? "true" : "false",
              dv_view_name(view.style, view.lvview),
              view.style & DV_LVS_TYPEMASK, view.lvview,
              jmode, jlvm, jsize,
              (view_result & GS_VIEW_LIVE_FIXED) ? "true" : "false",
              (view_result & GS_VIEW_BAG_FIXED) ? "true" : "false",
              dv_live_is_icon_view(view.style, view.lvview) ? "false" : "true");
    msg[sizeof(msg) - 1] = '\0';
    send_text_response(sock, msg);
}

void handle_gamesync(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    /* 3 KB: failed_file alone escapes to 520 bytes, the message can carry a
     * stall explanation, and with every string field full the reply is ~2.2
     * KB - a truncated reply is not valid JSON. */
    char   json[3072];
    char   msg[640];
    char   why[320];
    gsst_t st;
    unsigned long since_ms = 0, lost = 0, span = 0;
    int    running, verdict = GSST_OK, busy;
    DWORD  now;
    /* twice the source plus the terminator: every byte can double */
    char    esc_failed[sizeof(((gs_state_t *)0)->failed_file) * 2 + 1];
    char    dest_raw[160], dest_esc[330];
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

    /* IS IT MOVING? The worker's own accounting covers every gap that has
     * CLOSED; a worker starved right now cannot close one, so the gap still
     * open is judged here, on this (normal-priority) thread, from the same CPU
     * sample. See gs_beat() and agent/shared/gsstall.h. */
    now = GetTickCount();
    running = (s.state == GS_SIZING || s.state == GS_COPYING);
    st = s.stall;
    busy = s.started ? s.last_busy : -1;    /* no run yet: nothing measured */
    if (running) {
        since_ms = now - s.beat;
        if (since_ms >= GSST_GAP_MS) {
            int open_busy = gs_cpu_busy_since(s.cpu_ok, s.cpu_idle,
                                              s.cpu_kernel, s.cpu_user);
            gsst_note_gap(&st, now, since_ms, open_busy);
            if (open_busy >= 0)
                busy = open_busy;
        }
        verdict = gsst_verdict(&st, now, &lost, &span);
    }
    lstrcpynA(msg, s.message, sizeof(msg));
    if (verdict != GSST_OK) {
        gs_stall_describe(why, sizeof(why), verdict, lost, span, busy);
        _snprintf(msg, sizeof(msg) - 1, "%s - %s", s.message, why);
        msg[sizeof(msg) - 1] = 0;
    }

    gs_json_escape(s.failed_file, esc_failed, sizeof(esc_failed));
    if (!gs_games_dir(dest_raw, sizeof(dest_raw)))
        lstrcpynA(dest_raw, "(GamesDir set but unusable - see the log)", sizeof(dest_raw));
    gs_json_escape(dest_raw, dest_esc, sizeof(dest_esc));

    _snprintf(json, sizeof(json) - 1,
        "{\"state\":\"%s\",\"percent\":%d,"
        "\"titles_done\":%d,\"titles_total\":%d,\"titles_skipped\":%d,"
        "\"titles_gated\":%d,"
        "\"mb_done\":%I64d,\"mb_total\":%I64d,\"mbps\":%.2f,"
        "\"current_title\":\"%s\",\"current_file\":\"%s\","
        "\"failed_files\":%d,\"failed_file\":\"%s\","
        "\"elapsed_s\":%d,\"provisioned\":%s,"
        /* files_written/shortcuts_changed are what the icon-rebuild gate
         * decides on. Reported so a caller can SEE whether a steady-state sync
         * really had nothing to do - a box that reports the same non-zero
         * files_written every pass has a file that re-copies forever, which
         * defeats the gate silently. See gs_desk_files(). */
        "\"files_written\":%ld,\"shortcuts_changed\":%ld,"
        /* The resolution pass of the last finished run: values it changed
         * (a settled box: 0) and files it had adjusted that the copy kept. */
        "\"gameres_changed\":%d,\"gameres_kept\":%ld,"
        /* GAMERES VERIFY after the last finished run: targets still wrong
         * (a settled box: 0) and absent from this build; -1 = not yet. */
        "\"gameres_verify_wrong\":%d,\"gameres_verify_absent\":%d,"
        /* Is the run MOVING? since_progress_s is how long ago the worker last
         * made progress; stalled_s is time lost in gaps of >= 3 s, starved_s
         * the part of it with the CPU saturated (idle priority; a running game
         * stops the sync). cpu_busy_pct is -1 where Windows cannot say. */
        "\"since_progress_s\":%lu,\"stalled_s\":%lu,\"starved_s\":%lu,"
        "\"cpu_busy_pct\":%d,"
        "\"new_image\":%s,\"dest\":\"%s\",\"message\":\"%s\"}",
        names[(s.state >= 0 && s.state <= GS_SKIPPED) ? s.state : 0],
        pct, s.done_titles, s.total_titles, s.skipped_titles, s.gated_titles,
        s.done_bytes / 1048576, s.total_bytes / 1048576, s.mbps,
        s.title, s.file, s.failed_files, esc_failed, elapsed,
        gs_file_exists(GS_MARKER) ? "true" : "false",
        gs_desk_files(), gs_desk_lnks(),
        s.gr_changed, s.gr_kept,
        s.gr_verified ? s.gr_vwrong : -1, s.gr_verified ? s.gr_vabsent : -1,
        since_ms / 1000, st.total_stall_ms / 1000, st.total_starved_ms / 1000,
        busy,
        gs_file_exists(GS_NEWIMAGE_FLAG) ? "true" : "false",
        dest_esc, msg);
    /* new_image is deliberately reported alongside provisioned: together they
     * distinguish "fresh box, not yet done" from "old box someone reset". */
    json[sizeof(json) - 1] = 0;
    send_text_response(sock, json);
}
