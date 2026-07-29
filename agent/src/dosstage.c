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
 *   - files already present at the same size are skipped (idempotent, so a
 *     reboot costs one directory scan, not a re-copy),
 *   - the bulk payload (preview tiles) is copied last, with a breather
 *     between files so the box stays usable while it streams.
 *
 * Everything uses CopyFileA / FindFirstFileA rather than shelling out:
 * Win98 has no cmd.exe and xcopy hangs over a NETMAP'd share on XP.
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
#define DOSSTAGE_PATH     "DosStagePath"
#define DOSSTAGE_MARKER   "DosStaged"

#define DEFAULT_DOSSTAGE_PATH \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation"

/* Start after the update/wallpaper threads have had their turn. */
#define DOSSTAGE_DELAY_SEC   45
/* Breather between bulk (tile) copies so a Pentium 1 stays responsive. */
#define DOSSTAGE_TILE_PAUSE_MS 120

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

/* ---- copy helpers ---- */

static DWORD file_size_of(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    DWORD size;
    if (h == INVALID_HANDLE_VALUE) return 0xFFFFFFFF;   /* missing */
    size = fd.nFileSizeLow;
    FindClose(h);
    return size;
}

/* Copy src -> dst unless dst already exists at the same size.
 * Returns 1 if copied, 0 if skipped/failed. */
static int copy_if_different(const char *src, const char *dst)
{
    DWORD s = file_size_of(src);
    DWORD d = file_size_of(dst);

    if (s == 0xFFFFFFFF) return 0;          /* nothing on the share */
    if (d == s) return 0;                   /* already current */

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
    if (h == INVALID_HANDLE_VALUE) return 0;

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

    read_share_root(share, sizeof(share));
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

    if (!g_running) return;

    /* --- DOSGAME network tools --- */
    _snprintf(src_dir, sizeof(src_dir), "%s\\dosgame\\NET", share);
    copied += copy_dir(src_dir, "C:\\DOSGAME\\NET", 0);

    if (!g_running) return;

    /* --- preview tiles (the bulk payload, paced) --- */
    _snprintf(src_dir, sizeof(src_dir), "%s\\dosgame\\TILES", share);
    copied += copy_dir(src_dir, "C:\\DOSGAME\\TILES",
                       DOSSTAGE_TILE_PAUSE_MS);

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
