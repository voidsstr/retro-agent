/*
 * dosstage.c - Stage the DOS programs on DOS-capable machines
 *
 * The fleet's DOS lane ships two real-mode programs (see agent/doschat/ and
 * scripts/dosgames/):
 *
 *   DOSCHAT.EXE  the retro agent + chat in one exe, for the DOS side
 *   DOSGAME.EXE  the DOS game manager (menu, LAN install, preview tiles)
 *
 * A Windows 9x box boots into DOS 7.1 (and dual-boot boxes drop to real DOS),
 * so those machines should already have the DOS programs on disk when the user
 * gets there — no manual copy, no "go fetch it from the share" step. This
 * module pulls them from the share into C:\DOSCHAT and C:\DOSGAME whenever the
 * agent starts on a DOS-capable OS.
 *
 * Deliberately NOT run on NT/2000/XP: those boxes have no DOS to boot into,
 * and staging ~11 MB of preview tiles there is pure waste.
 *
 * Design constraints learned from onboarding (see onboard.c): the fleet's
 * slowest DOS-capable box is a Pentium-1 Compaq Deskpro 2000, where a
 * flat-out SMB copy saturates the machine and makes the agent look hung. So:
 *   - the thread runs at below-normal priority,
 *   - it starts after a delay (network + shell settle first),
 *   - files already present at the same size AND timestamp are skipped
 *     (idempotent, so a reboot costs one directory scan, not a re-copy;
 *     size alone is not enough — see copy_if_different),
 *   - the bulk payload (preview tiles) is copied last, with a breather
 *     between files so the box stays usable while it streams.
 *
 * Everything uses CopyFileA / FindFirstFileA rather than shelling out:
 * Win98 has no cmd.exe and xcopy hangs over a NETMAP'd share on XP.
 *
 * Reaching the share is the part that bites: on Windows 9x a bare UNC path
 * is NOT readable without an authenticated session, so CopyFileA/FindFirstFile
 * against \\server\share\... silently finds nothing (hardware-confirmed on
 * the Deskpro, which staged 0 files while reporting success). The fleet
 * already maps the share to a drive letter, so resolve_share_root() falls
 * back to that mapping before giving up.
 *
 * Registry (HKLM\Software\RetroAgent):
 *   DosStage      REG_DWORD  0 = disabled; absent/1 = enabled (default)
 *   DosStagePath  REG_SZ     share dir holding doschat\ and dosgame\
 *   DosStaged     REG_SZ     timestamp of the last successful stage
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

#define LOG_DOSSTAGE "DOSSTAGE"

#define DOSSTAGE_KEY      "Software\\RetroAgent"
#define DOSSTAGE_ENABLE   "DosStage"
#define DOSSTAGE_TILES    "DosStageTiles"   /* opt-in: the bulk tile payload */
#define DOSSTAGE_PATH     "DosStagePath"
#define DOSSTAGE_MARKER   "DosStaged"

#define DEFAULT_DOSSTAGE_PATH \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation"

/* Start after the update/wallpaper threads have had their turn. */
#define DOSSTAGE_DELAY_SEC   45
/* Breather between bulk (tile) copies so a Pentium 1 stays responsive. */
#define DOSSTAGE_TILE_PAUSE_MS 120

/* Below this much free RAM we do not stage at all.
 *
 * The preview tiles are ~11MB across 163 files. On the Deskpro (31MB total,
 * reporting 0MB available) staging that stream killed the agent outright and
 * took the box off the network — repeatedly, ~45s after every start, which is
 * what made it look like a startup crash (2026-07-29). A cosmetic feature
 * must never cost a box its agent, so: tiles are opt-in, and on a machine
 * this tight we skip staging entirely and say why. */
#define DOSSTAGE_MIN_FREE_MB 6

static int g_stage_running = 0;

/* ---- OS capability ---- */

/*
 * "DOS-capable" = the box can actually boot/shell to real-mode DOS.
 *
 * VER_PLATFORM_WIN32_WINDOWS covers Windows 95/98/98SE/ME, all of which sit
 * on DOS 7.x. NT-family (VER_PLATFORM_WIN32_NT) has no DOS to boot into —
 * its NTVDM is not the same thing and can't run the packet-driver stack the
 * DOS programs need.
 */
int dosstage_os_is_dos_capable(void)
{
    OSVERSIONINFOA osvi;
    memset(&osvi, 0, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (!GetVersionExA(&osvi)) return 0;
    return osvi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS;
}

/* ---- registry helpers ---- */

/* Free physical memory, in MB. */
static DWORD free_mb(void)
{
    MEMORYSTATUS ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (DWORD)(ms.dwAvailPhys / (1024UL * 1024UL));
}

/* The bulk preview-tile payload is opt-in (DosStageTiles=1). DOSCHAT and
 * DOSGAME themselves are only ~600KB together; the tiles are 11MB and are
 * pure decoration for the game menu. */
static int dosstage_tiles_enabled(void)
{
    HKEY hKey;
    DWORD value = 0, size = sizeof(value), type = 0;
    int enabled = 0;                 /* default OFF */

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, DOSSTAGE_KEY, 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, DOSSTAGE_TILES, NULL, &type,
                             (BYTE *)&value, &size) == ERROR_SUCCESS
            && type == REG_DWORD) {
            enabled = (value != 0);
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

static int dosstage_enabled(void)
{
    HKEY hKey;
    DWORD value = 1, size = sizeof(value), type = 0;
    int enabled = 1;                 /* default ON for DOS-capable boxes */

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, DOSSTAGE_KEY, 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, DOSSTAGE_ENABLE, NULL, &type,
                             (BYTE *)&value, &size) == ERROR_SUCCESS
            && type == REG_DWORD) {
            enabled = (value != 0);
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

static void read_share_root(char *buf, int bufsize)
{
    HKEY hKey;
    DWORD size, type;

    safe_strncpy(buf, DEFAULT_DOSSTAGE_PATH, bufsize);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, DOSSTAGE_KEY, 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        size = (DWORD)bufsize;
        if (RegQueryValueExA(hKey, DOSSTAGE_PATH, NULL, &type,
                             (BYTE *)buf, &size) != ERROR_SUCCESS
            || type != REG_SZ || !buf[0]) {
            safe_strncpy(buf, DEFAULT_DOSSTAGE_PATH, bufsize);
        }
        RegCloseKey(hKey);
    }
    buf[bufsize - 1] = '\0';
}

static void write_marker(const char *text)
{
    HKEY hKey;
    DWORD disp;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, DOSSTAGE_KEY, 0, NULL, 0,
                        KEY_WRITE, NULL, &hKey, &disp) != ERROR_SUCCESS)
        return;
    RegSetValueExA(hKey, DOSSTAGE_MARKER, 0, REG_SZ,
                   (const BYTE *)text, (DWORD)strlen(text) + 1);
    RegCloseKey(hKey);
}

/* ---- share reachability ---- */

/* Does this root hold our payload? Either package is enough — a share may
 * legitimately carry only one (e.g. a DosStagePath pointing at a chat-only
 * staging area), and refusing that would be a surprise. */
static int dir_is_readable(const char *dir);

static int root_has_payload(const char *root)
{
    char probe[MAX_PATH];

    _snprintf(probe, sizeof(probe), "%s\\dosgame", root);
    probe[sizeof(probe) - 1] = '\0';
    if (dir_is_readable(probe)) return 1;

    _snprintf(probe, sizeof(probe), "%s\\doschat", root);
    probe[sizeof(probe) - 1] = '\0';
    return dir_is_readable(probe);
}

/* Can we enumerate this directory? (The cheap "is the share readable" test.) */
static int dir_is_readable(const char *dir)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    _snprintf(pattern, sizeof(pattern), "%s\\*.*", dir);
    pattern[sizeof(pattern) - 1] = '\0';
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

/*
 * Turn the configured share root into something this box can actually read.
 *
 * Order:
 *   1. the configured path as-is (works on NT, and on 9x once a session
 *      exists for that server),
 *   2. the same share reached through a mapped network drive — the fleet maps
 *      \\192.168.1.122\files with credentials, and on Win98 that mapping is
 *      the ONLY authenticated way in.
 *
 * For (2) we take the part of the UNC after \\server\share (e.g.
 * "Utility\Retro Automation") and look for a remote drive that has our
 * payload under it. Matching on the payload rather than on the remote name
 * keeps this free of mpr.dll, which 9x loads inconsistently.
 *
 * Returns 1 and fills `out` on success, 0 if the payload is unreachable.
 */
static int resolve_share_root(const char *configured, char *out, int outsize)
{
    char probe[MAX_PATH];
    const char *sub;
    char letter;

    /* (1) as configured */
    if (root_has_payload(configured)) {
        safe_strncpy(out, configured, outsize);
        return 1;
    }

    /* Not a UNC path? Then there's no drive-letter fallback to try. */
    if (!(configured[0] == '\\' && configured[1] == '\\')) return 0;

    /* Skip past \\server\share to the subpath ("Utility\Retro Automation"). */
    sub = configured + 2;
    while (*sub && *sub != '\\') sub++;          /* end of server */
    if (*sub) sub++;
    while (*sub && *sub != '\\') sub++;          /* end of share name */
    if (*sub) sub++;
    if (!*sub) return 0;

    /* (2) any mapped network drive that holds our payload */
    for (letter = 'C'; letter <= 'Z'; letter++) {
        char root[8];
        _snprintf(root, sizeof(root), "%c:\\", letter);
        if (GetDriveTypeA(root) != DRIVE_REMOTE) continue;

        _snprintf(probe, sizeof(probe), "%c:\\%s", letter, sub);
        probe[sizeof(probe) - 1] = '\0';
        if (root_has_payload(probe)) {
            _snprintf(out, outsize, "%c:\\%s", letter, sub);
            out[outsize - 1] = '\0';
            log_msg(LOG_DOSSTAGE,
                    "share reached via mapped drive %c: (UNC not readable)",
                    letter);
            return 1;
        }
    }
    return 0;
}

/* ---- copy helpers ---- */

/* Set for the duration of a DOSSTAGE force run. Without this, "force" only
 * bypassed the DosStage=0 enable flag, so an operator who could SEE a stale
 * file on a box had no way to replace it short of deleting it by hand. */
static int g_stage_force = 0;

/* Returns 0 on success and fills size + last-write time; 1 if the file is
 * missing. Both facts come from the one FindFirstFileA the old size-only
 * helper was already paying for. */
static int file_stat_of(const char *path, DWORD *size, FILETIME *mtime)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    *size = fd.nFileSizeLow;
    *mtime = fd.ftLastWriteTime;
    FindClose(h);
    return 0;
}

/* Copy src -> dst unless dst is already identical.
 *
 * "Same size" is NOT identical, and this payload is mostly batch scripts where
 * the ordinary edit preserves length exactly: changing NETUP.BAT's probe from
 * io 0x300 to 0x320, swapping "goto try4" for "goto try5", or correcting
 * PACKETINT 0x62 to 0x63. Publishing such a fix to the share left every
 * already-staged box on the broken file forever, while dosstage_run logged
 * "staged 0 file(s)" and wrote the DosStaged marker as if it had worked. The
 * agent's own updater learned this exact lesson once already - that is why it
 * carries a .ver sidecar instead of comparing sizes - and the DOS staging path
 * never got the same treatment.
 *
 * So compare the last-write time as well, and copy whenever the share's copy
 * is NEWER. CopyFileA preserves the timestamp, so a staged file matches its
 * source exactly and a re-run is still a no-op.
 *
 * Returns 1 if copied, 0 if skipped/failed. */
static int copy_if_different(const char *src, const char *dst)
{
    DWORD s = 0, d = 0;
    FILETIME smt, dmt;

    if (file_stat_of(src, &s, &smt)) return 0;      /* nothing on the share */
    if (!g_stage_force && !file_stat_of(dst, &d, &dmt)
        && d == s && CompareFileTime(&smt, &dmt) <= 0)
        return 0;                                   /* already current */

    if (!CopyFileA(src, dst, FALSE)) {
        log_msg(LOG_DOSSTAGE, "copy failed (%lu): %s",
                (unsigned long)GetLastError(), dst);
        return 0;
    }
    return 1;
}

/*
 * Copy every file from a share subdirectory into a local directory.
 * pause_ms > 0 sleeps between files (used for the bulk tile payload so the
 * copy doesn't monopolize a Pentium-1 box). Returns files copied.
 */
static int copy_dir(const char *src_dir, const char *dst_dir, int pause_ms)
{
    char pattern[MAX_PATH];
    char src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int copied = 0;

    CreateDirectoryA(dst_dir, NULL);

    _snprintf(pattern, sizeof(pattern), "%s\\*.*", src_dir);
    pattern[sizeof(pattern) - 1] = '\0';

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log_msg(LOG_DOSSTAGE, "source not readable: %s", src_dir);
        return 0;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        _snprintf(src, sizeof(src), "%s\\%s", src_dir, fd.cFileName);
        _snprintf(dst, sizeof(dst), "%s\\%s", dst_dir, fd.cFileName);
        src[sizeof(src) - 1] = dst[sizeof(dst) - 1] = '\0';

        if (copy_if_different(src, dst)) {
            copied++;
            if (pause_ms > 0) Sleep(pause_ms);
        }
        if (!g_running) break;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return copied;
}

/* ---- the staging job ---- */

/*
 * Stage both DOS packages. Order matters on a slow box: the programs and
 * their network stack come first so the box is usable the moment the copy
 * starts paying off, and the ~11 MB of preview tiles trickles in last.
 */
void dosstage_run(int force)
{
    g_stage_force = force;
    char configured[512];
    char share[512];
    char src_dir[MAX_PATH], dst_dir[MAX_PATH];
    char src[MAX_PATH];
    char marker[128];
    SYSTEMTIME st;
    int copied = 0;

    if (!dosstage_os_is_dos_capable()) {
        log_msg(LOG_DOSSTAGE, "skipped: not a DOS-capable OS (NT family)");
        return;
    }
    if (!force && !dosstage_enabled()) {
        log_msg(LOG_DOSSTAGE, "skipped: disabled via %s\\%s",
                DOSSTAGE_KEY, DOSSTAGE_ENABLE);
        return;
    }

    /* Memory guard: never trade the agent for a file copy. */
    {
        DWORD avail = free_mb();
        if (avail < DOSSTAGE_MIN_FREE_MB) {
            log_msg(LOG_DOSSTAGE,
                    "skipped: only %luMB free (need %dMB) - staging would "
                    "put the agent at risk on this box",
                    (unsigned long)avail, DOSSTAGE_MIN_FREE_MB);
            return;
        }
    }

    read_share_root(configured, sizeof(configured));
    if (!resolve_share_root(configured, share, sizeof(share))) {
        /* Nothing was copied and nothing will be — say so loudly rather than
         * reporting a successful stage of zero files. */
        log_msg(LOG_DOSSTAGE,
                "share not reachable (%s) - is it mapped? no files staged",
                configured);
        return;
    }
    log_msg(LOG_DOSSTAGE, "staging DOS programs from %s", share);

    /* --- DOSCHAT: agent + chat in one exe --- */
    _snprintf(src_dir, sizeof(src_dir), "%s\\doschat", share);
    safe_strncpy(dst_dir, "C:\\DOSCHAT", sizeof(dst_dir));
    copied += copy_dir(src_dir, dst_dir, 0);

    if (!g_running) return;

    /* --- DOSGAME: game manager --- */
    _snprintf(src_dir, sizeof(src_dir), "%s\\dosgame", share);
    safe_strncpy(dst_dir, "C:\\DOSGAME", sizeof(dst_dir));
    copied += copy_dir(src_dir, dst_dir, 0);

    /* DOSGAME.BAT is the launcher wrapper and lives at the root of C: —
     * the menu exits with code 42 and the batch relaunches it, which is how
     * a game gets all of conventional memory. */
    _snprintf(src, sizeof(src), "%s\\dosgame\\DOSGAME.BAT", share);
    copied += copy_if_different(src, "C:\\DOSGAME.BAT");

    /* PLAY.BAT / CHAT.BAT are the user-facing entry points ("type PLAY") and
     * belong at the root next to DOSGAME.BAT; both call C:\DOSGAME\NETUP.BAT
     * so networking comes up without any manual setup. */
    _snprintf(src, sizeof(src), "%s\\dosgame\\PLAY.BAT", share);
    copied += copy_if_different(src, "C:\\PLAY.BAT");
    _snprintf(src, sizeof(src), "%s\\dosgame\\CHAT.BAT", share);
    copied += copy_if_different(src, "C:\\CHAT.BAT");

    /* DOSSTART.BAT is what Windows runs when the operator picks "Restart in
     * MS-DOS mode" - NOT AUTOEXEC.BAT, which is the real-mode BOOT file. A
     * box can therefore be perfectly set up at the boot prompt and completely
     * bare in MS-DOS mode, which is exactly how .243 behaved: no PATH to the
     * DOS tools, nothing saying PLAY exists, and - because nothing on the box
     * logged anything between the Shut Down dialog and the operator typing
     * PLAY - no evidence at all when it stuck at a bare cursor.
     *
     * It was hand-placed on .243 and staged nowhere, so every other DOS box
     * had none. Staging it puts the log marker on all of them: if the marker
     * reaches DOSGAME.LOG the transition completed and DOS is up; if it does
     * not, the machine never got out of the Windows shutdown. */
    _snprintf(src, sizeof(src), "%s\\dosgame\\DOSSTART.BAT", share);
    copied += copy_if_different(src, "C:\\WINDOWS\\DOSSTART.BAT");

    if (!g_running) return;

    /* --- DOSGAME network tools --- */
    _snprintf(src_dir, sizeof(src_dir), "%s\\dosgame\\NET", share);
    copied += copy_dir(src_dir, "C:\\DOSGAME\\NET", 0);

    if (!g_running) return;

    /* --- preview tiles: opt-in only (11MB across 163 files) --- */
    if (dosstage_tiles_enabled()) {
        _snprintf(src_dir, sizeof(src_dir), "%s\\dosgame\\TILES", share);
        copied += copy_dir(src_dir, "C:\\DOSGAME\\TILES",
                           DOSSTAGE_TILE_PAUSE_MS);
    } else {
        log_msg(LOG_DOSSTAGE,
                "preview tiles skipped (set %s=1 to stage them)",
                DOSSTAGE_TILES);
    }

    GetLocalTime(&st);
    _snprintf(marker, sizeof(marker), "%04d-%02d-%02d %02d:%02d (%d files)",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, copied);
    marker[sizeof(marker) - 1] = '\0';
    write_marker(marker);

    log_msg(LOG_DOSSTAGE, "staged %d file(s) to C:\\DOSCHAT and C:\\DOSGAME",
            copied);
}

static DWORD WINAPI dosstage_run_thread(LPVOID param)
{
    int force = (int)(UINT_PTR)param;
    g_stage_running = 1;
    dosstage_run(force);
    g_stage_running = 0;
    return 0;
}

/*
 * Startup thread: on a DOS-capable box, stage the DOS programs in the
 * background. On NT this exits immediately after the platform check, so it
 * costs a fleet XP box nothing.
 */
DWORD WINAPI dosstage_thread(LPVOID param)
{
    (void)param;

    if (!dosstage_os_is_dos_capable()) return 0;

    /* Below normal: on the Pentium-1 box this competes with the shell
     * coming up, and a laggy first minute reads as a hung agent. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    Sleep(DOSSTAGE_DELAY_SEC * 1000);
    if (!g_running) return 0;

    dosstage_run(0);
    return 0;
}

/*
 * DOSSTAGE [force] - stage the DOS programs on demand.
 *
 * Reports why it won't run rather than silently doing nothing: on an NT box
 * the answer is always "no DOS to boot into", which is a configuration fact
 * the caller wants to see.
 */
void handle_dosstage(SOCKET sock, const char *args)
{
    int force = (args && (str_starts_with(args, "force") ||
                          str_starts_with(args, "FORCE")));
    HANDLE h;

    if (!dosstage_os_is_dos_capable()) {
        send_text_response(sock,
            "skipped: this OS is not DOS-capable (NT family) - "
            "the DOS programs are only staged on Windows 9x/ME");
        return;
    }
    if (!force && !dosstage_enabled()) {
        send_text_response(sock,
            "skipped: staging disabled (HKLM\\Software\\RetroAgent\\DosStage=0) "
            "- use DOSSTAGE force to override");
        return;
    }
    if (g_stage_running) {
        send_text_response(sock, "already staging");
        return;
    }

    h = CreateThread(NULL, 0, dosstage_run_thread,
                     (LPVOID)(UINT_PTR)force, 0, NULL);
    if (h) CloseHandle(h);
    log_msg(LOG_DOSSTAGE, "DOSSTAGE command received (force=%d)", force);
    send_text_response(sock, force ? "DOS staging started (forced)"
                                   : "DOS staging started");
}
