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
#include "../shared/drvprefs.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <shlobj.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#define LOG_GS "GAMESYNC"

/* The library the fleet publishes pre-installed game trees to. Overridable -
 * see gs_library_path() - because an address baked into a binary is a promise
 * we cannot keep across a NAS move. */
#define GS_DEFAULT_LIBRARY "\\\\192.168.1.122\\files\\Files\\Games-Library"
#define GS_DEST            "C:\\Games"
#define GS_MARKER          "C:\\RETRO_AGENT\\gamesync.done"
/* Written into the image by stage-oem.sh. Its PRESENCE is what says
 * "this machine was just imaged" - see gs_new_image(). */
#define GS_NEWIMAGE_FLAG   "C:\\RETRO_AGENT\\newimage.flag"
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
    total = g_gs.total_bytes;
    done  = g_gs.done_bytes;
    lstrcpynA(title, g_gs.title, sizeof(title));
    lstrcpynA(file,  g_gs.file,  sizeof(file));
    LeaveCriticalSection(&g_gs_lock);

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
#define GS_MTIME_SLACK_100NS  (2 * 10000000LL)   /* 2 s, in 100ns FILETIME units */

static int gs_get_mtime(const char *path, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &ad))
        return 0;
    *ft = ad.ftLastWriteTime;
    return 1;
}

static int gs_same_mtime(const FILETIME *a, const FILETIME *b)
{
    __int64 ta = ((__int64)a->dwHighDateTime << 32) | a->dwLowDateTime;
    __int64 tb = ((__int64)b->dwHighDateTime << 32) | b->dwLowDateTime;
    __int64 d  = ta - tb;
    if (d < 0) d = -d;
    return d <= GS_MTIME_SLACK_100NS;
}

static int gs_copy_file(const char *src, const char *dst, __int64 src_size)
{
    HANDLE hs, hd;
    char  *buf;
    DWORD  rd, wr;
    int    ok = 1;
    __int64 already;
    FILETIME src_ft, dst_ft;

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
    already = gs_file_size(dst);
    if (already >= 0 && already == src_size &&
        gs_get_mtime(src, &src_ft) && gs_get_mtime(dst, &dst_ft) &&
        gs_same_mtime(&src_ft, &dst_ft)) {
        gs_note_progress2(src_size, 0);   /* counted, but nothing crossed the wire */
        return 1;
    }

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
                if (g_gs.failed_files == 0)
                    lstrcpynA(g_gs.failed_file, d, sizeof(g_gs.failed_file));
                g_gs.failed_files++;
                LeaveCriticalSection(&g_gs_lock);
                ok = 0;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
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

/* Defined below, with the driver installer - the reclaim guard needs it to tell
 * a device we could fix from one we could not. */
static int gs_find_inf_for(const char *hwid, char *out, DWORD out_cch);

/* Are there devices Windows has not managed to configure?
 *
 * This gates the reclaim, and it matters: the staged drivers in C:\D are
 * precisely what the Found New Hardware wizard needs. Deleting them while a
 * device is still unconfigured would take away the only local copy at the exact
 * moment it is wanted - and on a machine whose NIC is the unconfigured device,
 * there is no network left to fetch a replacement over. That is the worst
 * failure this agent could cause, so the check is deliberately conservative:
 * ANY device with a problem code means keep the drivers.
 */
static int gs_devices_unconfigured(void)
{
    HDEVINFO         set;
    SP_DEVINFO_DATA  dev;
    DWORD            i;
    int              bad = 0;

    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 1;                 /* cannot tell -> assume yes, keep drivers */

    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        DWORD status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) != CR_SUCCESS)
            continue;
        if (problem != 0 || (status & DN_HAS_PROBLEM)) {
            char name[256], ids[1024], inf[MAX_PATH];
            name[0] = ids[0] = 0;
            SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL,
                                              (PBYTE)name, sizeof(name), NULL);
            /* Only devices the STAGED TREE could actually help are worth
             * keeping it for. The first version counted every unconfigured
             * device, and two that C:\D can never serve - a phantom PS/2 mouse
             * on a machine with a USB one, and an in-box WDM audio stub - held
             * 2.4 GB of drivers on a 6 GB disk indefinitely, which in turn left
             * no room for the game library. A device we have no driver for is
             * not a reason to keep drivers. */
            if (SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID,
                                                  NULL, (PBYTE)ids,
                                                  sizeof(ids), NULL) && ids[0]) {
                CharUpperA(ids);
                if (gs_find_inf_for(ids, inf, sizeof(inf))) {
                    log_msg(LOG_GS, "device not configured (problem %lu): %s "
                                    "- a driver for it IS staged",
                            problem, name[0] ? name : "(unnamed)");
                    bad++;
                    continue;
                }
            }
            log_msg(LOG_GS, "device not configured (problem %lu): %s - nothing "
                            "in %s serves it, not a reason to keep the tree",
                    problem, name[0] ? name : "(unnamed)", GS_DRIVER_DIR);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return bad;
}

/*
 * Install drivers for devices Windows left unconfigured.
 *
 * GUI setup does not always get there. A Dell Dimension 3000 finished a clean
 * install and sat on the VGA fallback at 640x480 in 16 colours, with the Intel
 * 865G driver it needed present on its own disk in C:\D the whole time, waiting
 * for someone to walk over and answer a Found New Hardware wizard. Three
 * devices were in that state: the display, the audio, and a multimedia
 * controller.
 *
 * So the agent finishes the job. For each device carrying a problem code it
 * takes the hardware ID, finds an INF in the staged driver tree that mentions
 * it, and hands both to UpdateDriverForPlugAndPlayDevices - the documented way
 * to drive a PnP install without a dialog.
 *
 * Deliberately quiet about failure. Not every unconfigured device has a driver
 * in the tree, and one that cannot be matched is not an error worth alarming
 * anyone about; it just stays as it was, exactly as if we had not tried.
 */
#define INSTALLFLAG_FORCE_ 0x00000001

typedef BOOL (WINAPI *updrv_fn)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

/* Find an INF under C:\D naming this hardware id. The tree is ~500 directories
 * and 18,000 files, so this walks INFs only and stops at the first match. */
static int gs_find_inf_for(const char *hwid, char *out, DWORD out_cch)
{
    WIN32_FIND_DATAA fd, ff;
    HANDLE           hd, hf;
    char             pat[MAX_PATH], sub[MAX_PATH], infp[MAX_PATH];
    int              found = 0;

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", GS_DRIVER_DIR);
    pat[sizeof(pat) - 1] = 0;
    hd = FindFirstFileA(pat, &fd);
    if (hd == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        _snprintf(sub, sizeof(sub) - 1, "%s\\%s\\*.inf", GS_DRIVER_DIR,
                  fd.cFileName);
        sub[sizeof(sub) - 1] = 0;
        hf = FindFirstFileA(sub, &ff);
        if (hf == INVALID_HANDLE_VALUE)
            continue;
        do {
            HANDLE  h;
            DWORD   got = 0;
            char   *buf;
            _snprintf(infp, sizeof(infp) - 1, "%s\\%s\\%s", GS_DRIVER_DIR,
                      fd.cFileName, ff.cFileName);
            infp[sizeof(infp) - 1] = 0;
            h = CreateFileA(infp, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            buf = (char *)HeapAlloc(GetProcessHeap(), 0, 262144);
            if (buf) {
                if (ReadFile(h, buf, 262143, &got, NULL) && got) {
                    buf[got] = 0;
                    /* INFs spell hardware ids in mixed case; compare upper. */
                    CharUpperA(buf);
                    if (strstr(buf, hwid)) {
                        lstrcpynA(out, infp, out_cch);
                        found = 1;
                    }
                }
                HeapFree(GetProcessHeap(), 0, buf);
            }
            CloseHandle(h);
        } while (!found && FindNextFileA(hf, &ff));
        FindClose(hf);
    } while (!found && FindNextFileA(hd, &fd));
    FindClose(hd);
    return found;
}

static void gs_install_missing_drivers(void)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dev;
    DWORD           i;
    HMODULE         newdev;
    updrv_fn        update;
    int             fixed = 0, tried = 0;

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

    memset(&dev, 0, sizeof(dev));
    dev.cbSize = sizeof(dev);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        DWORD status = 0, problem = 0;
        char  ids[1024], desc[256], inf[MAX_PATH];
        BOOL  reboot = FALSE;
        char *p;

        if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) != CR_SUCCESS)
            continue;
        if (problem == 0 && !(status & DN_HAS_PROBLEM))
            continue;

        ids[0] = desc[0] = 0;
        SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL,
                                          (PBYTE)desc, sizeof(desc), NULL);
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)ids, sizeof(ids), NULL))
            continue;
        /* REG_MULTI_SZ - the first id is the most specific. */
        for (p = ids; *p; p++)
            ;
        CharUpperA(ids);
        if (!ids[0])
            continue;

        if (!gs_find_inf_for(ids, inf, sizeof(inf))) {
            log_msg(LOG_GS, "no driver in %s for %s (%s) - leaving it",
                    GS_DRIVER_DIR, desc[0] ? desc : "(unnamed)", ids);
            continue;
        }
        tried++;
        log_msg(LOG_GS, "installing %s for %s", inf, desc[0] ? desc : ids);
        if (update(NULL, ids, inf, INSTALLFLAG_FORCE_, &reboot)) {
            fixed++;
            log_msg(LOG_GS, "  installed%s", reboot ? " (needs a reboot)" : "");
        } else {
            log_msg(LOG_GS, "  failed (%lu) - the Found New Hardware wizard can "
                            "still do it from %s", GetLastError(), GS_DRIVER_DIR);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    FreeLibrary(newdev);
    if (tried)
        log_msg(LOG_GS, "unconfigured devices: %d of %d now have drivers",
                fixed, tried);
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
 * WHY AN EXPLICIT LIST RATHER THAN A HEURISTIC. gs_find_inf_for() returns the
 * FIRST INF naming a hardware id, and for PCI\VEN_10DE&DEV_0150 that is
 * G003\nv4_go.inf - ForceWare 270.61 MOBILE, a 2011 driver for a 2000 card.
 * Guessing here installs the wrong driver confidently.
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
    gs_prefs_pass(1);
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
 * shortcut already exists and points at the same place.
 *
 * The chat client is only given an icon if it is actually on the box. A
 * shortcut to something that is not there is worse than no shortcut: it looks
 * like a working feature until someone clicks it.
 */
static void gs_tool_shortcut(const char *exe, const char *name)
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

    lstrcpynA(workdir, exe, sizeof(workdir));
    slash = workdir + lstrlenA(workdir);
    while (slash > workdir && *slash != '\\')
        slash--;
    *slash = 0;

    if (gs_make_shortcut(exe, workdir, lnk, name, NULL))
        log_msg(LOG_GS, "desktop shortcut -> %s", name);
    else
        log_msg(LOG_GS, "%s: could not create the shortcut", name);
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
        gs_tool_shortcut(exe, "Retro Agent");

    gs_tool_shortcut("C:\\RETRO_AGENT\\retro_chat.exe", "Retro Chat");

    if (we_initialised && g_gs_CoUninitialize)
        g_gs_CoUninitialize();
}

/*
 * Clear the desktop of everything that is not one of ours.
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
 */
static int gs_sweep_desktop_dir(const char *desk)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    int              moved = 0;

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", desk);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        const char *ext;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        ext = fd.cFileName + lstrlenA(fd.cFileName);
        while (ext > fd.cFileName && *ext != '.')
            ext--;
        if (lstrcmpiA(ext, ".lnk") != 0 && lstrcmpiA(ext, ".pif") != 0 &&
            lstrcmpiA(ext, ".url") != 0)
            continue;

        _snprintf(src, sizeof(src) - 1, "%s\\%s", desk, fd.cFileName);
        _snprintf(dst, sizeof(dst) - 1, "%s\\%s", GS_DESK_BACKUP, fd.cFileName);
        src[sizeof(src) - 1] = dst[sizeof(dst) - 1] = 0;
        SetFileAttributesA(src, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(dst);                      /* MoveFile will not overwrite */
        if (MoveFileA(src, dst) || DeleteFileA(src))
            moved++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return moved;
}

static void gs_sweep_desktop(void)
{
    char desk[MAX_PATH], userdesk[MAX_PATH];
    int  moved = 0;

    CreateDirectoryA(GS_DESK_BACKUP, NULL);
    /* Both desktops: shortcuts land in All Users or in the logged-on user's
     * profile depending on who installed what, and a sweep that only does one
     * leaves half the clutter behind. */
    if (gs_desktop_dir(desk, sizeof(desk)))
        moved += gs_sweep_desktop_dir(desk);
    if (gs_user_desktop_dir(userdesk, sizeof(userdesk)) &&
        lstrcmpiA(userdesk, desk) != 0)
        moved += gs_sweep_desktop_dir(userdesk);
    if (moved)
        log_msg(LOG_GS, "desktop swept: %d shortcut(s) moved to %s",
                moved, GS_DESK_BACKUP);
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
         * wallpapers are 26 MB. */
        if (gs_file_size(dst) == gs_file_size(src))
            continue;
        if (CopyFileA(src, dst, FALSE))
            n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n)
        log_msg(LOG_GS, "staged %d wallpaper file(s) into %s", n, GS_WALL_DIR);
}

/* Make ONE desktop shortcut from a "<relative exe>[<TAB><display name>]" line. */
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

    if (gs_make_shortcut(target, workdir, lnk, disp, icon))
        log_msg(LOG_GS, "%s: desktop shortcut -> %s (icon: %s)", title, exe_rel,
                icon[0] ? icon : "from target");
    else
        log_msg(LOG_GS, "%s: could not create desktop shortcut", title);
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

/* ---------------------------------------------------------------------- */
/* desktop icon arrangement                                                */
/* ---------------------------------------------------------------------- */

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
#define LVM_SETITEMPOSITION_ (LVM_FIRST_ + 15)
#define LVM_SETEXSTYLE_      (LVM_FIRST_ + 54)
#define LVM_GETEXSTYLE_      (LVM_FIRST_ + 55)
#define FCIDM_SHVIEW_AUTOARRANGE_ 0x7031
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif
#ifndef LVS_EX_SNAPTOGRID
#define LVS_EX_SNAPTOGRID 0x00080000
#endif

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
 * the library outgrows it, gs_arrange_icons used to keep packing DOWNWARD past
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

static void gs_arrange_icons(void)
{
    HWND     defview = NULL;
    HWND     lv = gs_desktop_listview(&defview);
    gs_bay_t bay;
    int      count, i, col, row, cols;
    int      sw, sh;

    if (!lv) {
        log_msg(LOG_GS, "desktop listview not found - icons left as they are");
        return;
    }
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    gs_icon_bay(sw, sh, &bay);

    /* Auto Arrange must be OFF or the shell snaps every icon back to the
     * top-left grid and our positions never stick.
     *
     * FCIDM_SHVIEW_AUTOARRANGE is a TOGGLE, not a set - so firing it blindly
     * turns the setting ON when it was already off, which is worse than doing
     * nothing and is precisely what happened: the icons ended up in neat rows
     * across the top of the screen instead of in the bay. Read the listview's
     * LVS_AUTOARRANGE style first (GetWindowLong works cross-process) and only
     * toggle when it is actually set.
     *
     * PostMessage rather than SendMessage because a synchronous send into the
     * shell can block us indefinitely. */
    if (defview) {
        LONG style = GetWindowLongA(lv, GWL_STYLE);
        if (style & LVS_AUTOARRANGE) {
            log_msg(LOG_GS, "auto-arrange is on - turning it off so icon "
                            "positions stick");
            PostMessageA(defview, WM_COMMAND, FCIDM_SHVIEW_AUTOARRANGE_, 0);
            Sleep(600);
            style = GetWindowLongA(lv, GWL_STYLE);
            if (style & LVS_AUTOARRANGE) {
                /* The toggle does not always take. On .143 it failed on EVERY
                 * run for weeks - the agent logged "still on" each time and the
                 * shell then laid the icons out in its own grid, sprawled over
                 * the wallpaper art instead of in the bay, which is what the
                 * whole feature exists to prevent.
                 *
                 * Clear the style bit directly. GWL_STYLE is settable
                 * cross-process on a listview, and this is precisely what
                 * scripts/retro-wallpaper/arrange_icons.c has always done -
                 * the agent was the only arranger missing the call. Unlike the
                 * WM_COMMAND it is a SET, not a toggle, so it cannot turn
                 * auto-arrange ON where a box had it off. */
                SetWindowLongA(lv, GWL_STYLE, style & ~LVS_AUTOARRANGE);
                Sleep(200);
                style = GetWindowLongA(lv, GWL_STYLE);
                log_msg(LOG_GS, "auto-arrange survived the toggle - cleared "
                                "the style directly (now %s)",
                        (style & LVS_AUTOARRANGE) ? "STILL ON" : "off");
            }
        }
    }

    /* "Align icons to grid" is a SECOND, independent setting, and clearing
     * auto-arrange does nothing about it. While LVS_EX_SNAPTOGRID is set the
     * shell ROUNDS every position we ask for to its own grid, whose row pitch
     * is the icon spacing PLUS the label - measured at 103 px on a 1920x1080
     * box - so a bay drawn with 80 px cells gets icons 103 px apart and they
     * walk out of their slots down the column. Verified by A/B on .246: the
     * identical arrange gave 103 px row pitch with the flag set and exactly
     * 80 px with it cleared.
     *
     * Windows enables align-to-grid by default, so this affected every box,
     * not one. Unlike auto-arrange this is not a toggle: the message takes a
     * (mask, value) pair, so passing value 0 clears it deterministically and
     * cannot turn it on. */
    {
        DWORD exst = (DWORD)SendMessageA(lv, LVM_GETEXSTYLE_, 0, 0);
        if (exst & LVS_EX_SNAPTOGRID) {
            SendMessageA(lv, LVM_SETEXSTYLE_, LVS_EX_SNAPTOGRID, 0);
            exst = (DWORD)SendMessageA(lv, LVM_GETEXSTYLE_, 0, 0);
            log_msg(LOG_GS, "align-to-grid was on - cleared it so icons land "
                            "in the bay's cells (now %s)",
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
        log_msg(LOG_GS, "arranged %d desktop icon(s) into %d columns - the "
                        "%dx%d bay holds only %d, so it was widened to keep "
                        "every icon on screen",
                count, cols, bay.cols, bay.rows, bay.cols * bay.rows);
    else
        log_msg(LOG_GS, "arranged %d desktop icon(s) into the %dx%d icon bay",
                count, bay.cols, bay.rows);
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

    /* Clear the desktop and make sure the wallpapers are on disk BEFORE any
     * shortcut is written, so what the sweep removes is only what was already
     * there. Doing it afterwards would take our own game icons straight back
     * off again.
     *
     * Both run on every provision, imaged box or not: a hand-built machine is
     * exactly the one that has a cluttered desktop and no C:\retro-wall. */
    gs_sweep_desktop();
    /* Immediately after the sweep, so the tools the operator needs are never
     * missing between the sweep and the next agent start. */
    gs_place_tool_shortcuts();
    gs_stage_wallpapers(library);

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
        /* Directories beginning with _ are the library's own support folders,
         * not games: _desktop holds the fleet wallpapers, _patches the record
         * of what has been patched. Counting them as titles copied 26 MB of
         * wallpaper onto every box as if it were a game, and reported 30
         * titles where there were 29. */
        if (fd.cFileName[0] == '_')
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
        {
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
            _snprintf(have, sizeof(have) - 1, "%s\\%s", GS_DEST, titles[i]);
            have[sizeof(have) - 1] = 0;
            if (gs_file_exists(have)) {
                existing = gs_dir_size(have, &nfiles);
                if (existing > 0 && freeb >= 0)
                    freeb += existing;
            }
        }
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
            gs_make_game_shortcut(dst, titles[i]);
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

    /* Arrange AFTER every shortcut exists: the shell creates a listview item
     * per .lnk asynchronously, so arranging per title would keep re-sorting a
     * list that is still growing. */
    Sleep(2000);
    gs_arrange_icons();

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

DWORD WINAPI gamesync_thread(LPVOID param)
{
    int fresh;

    (void)param;
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
     * anything or saying so. */
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
        gs_install_missing_drivers();
        /* Then the drivers Windows DID configure, badly. This must come before
         * the reclaim: a preference cannot be applied from a tree we have
         * already deleted. */
        gs_apply_driver_prefs();
        /* Before the marker check, and before any copying: on a small disk this
         * is the difference between three games and a dozen. */
        gs_reclaim_drivers();
    }

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
void handle_drvupdate(SOCKET sock, const char *args)
{
    char     hwid[256], inf[MAX_PATH];
    HMODULE  newdev;
    updrv_fn update;
    BOOL     reboot = FALSE;
    const char *sp;

    while (*args == ' ')
        args++;
    if (!*args) {
        send_error_response(sock, "usage: DRVUPDATE <hardware-id> [inf-path]");
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

    if (!inf[0]) {
        if (!gs_find_inf_for(hwid, inf, sizeof(inf))) {
            send_error_response(sock, "no INF in " GS_DRIVER_DIR " names that hardware id");
            return;
        }
    }
    if (!gs_file_exists(inf)) {
        send_error_response(sock, "INF not found");
        return;
    }

    newdev = LoadLibraryA("newdev.dll");
    update = newdev ? (updrv_fn)GetProcAddress(newdev,
                          "UpdateDriverForPlugAndPlayDevicesA") : NULL;
    if (!update) {
        if (newdev)
            FreeLibrary(newdev);
        send_error_response(sock, "newdev.dll unavailable");
        return;
    }
    log_msg(LOG_GS, "DRVUPDATE %s -> %s", hwid, inf);
    if (update(NULL, hwid, inf, INSTALLFLAG_FORCE_, &reboot)) {
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

void handle_gamesync(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    char   json[1024];
    /* twice the source plus the terminator: every byte can double */
    char    esc_failed[sizeof(((gs_state_t *)0)->failed_file) * 2 + 1];
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

    gs_json_escape(s.failed_file, esc_failed, sizeof(esc_failed));

    _snprintf(json, sizeof(json) - 1,
        "{\"state\":\"%s\",\"percent\":%d,"
        "\"titles_done\":%d,\"titles_total\":%d,\"titles_skipped\":%d,"
        "\"mb_done\":%I64d,\"mb_total\":%I64d,\"mbps\":%.2f,"
        "\"current_title\":\"%s\",\"current_file\":\"%s\","
        "\"failed_files\":%d,\"failed_file\":\"%s\","
        "\"elapsed_s\":%d,\"provisioned\":%s,"
        "\"new_image\":%s,\"message\":\"%s\"}",
        names[(s.state >= 0 && s.state <= GS_SKIPPED) ? s.state : 0],
        pct, s.done_titles, s.total_titles, s.skipped_titles,
        s.done_bytes / 1048576, s.total_bytes / 1048576, s.mbps,
        s.title, s.file, s.failed_files, esc_failed, elapsed,
        gs_file_exists(GS_MARKER) ? "true" : "false",
        gs_file_exists(GS_NEWIMAGE_FLAG) ? "true" : "false",
        s.message);
    /* new_image is deliberately reported alongside provisioned: together they
     * distinguish "fresh box, not yet done" from "old box someone reset". */
    json[sizeof(json) - 1] = 0;
    send_text_response(sock, json);
}
