/*
 * onboard.c - First-run onboarding for a freshly-provisioned retro PC.
 *
 * When a machine runs the agent for the first time we want it to bootstrap
 * itself into the fleet: map the file share, pull down a core set of games,
 * and make sure the retro desktop (wallpaper rotation, parked icons, the dark
 * "hacker" XP theme) is in place. This module is the TRIGGER for that; the
 * actual per-game install steps live in a data-driven batch staged on the
 * share (provisioning/onboard.cmd), so the game list can grow without
 * recompiling the fleet binary.
 *
 * Flow (onboard_thread, background, after the shell has settled):
 *
 *   1. If HKLM\Software\RetroAgent\Onboarded == 1, do nothing (already done).
 *   2. Print an ONBOARDING banner to the console + log.
 *   3. Map the file share (net use <drive> <unc>), reading path/creds/drive
 *      from HKLM\Software\RetroAgent (sensible defaults if unset).
 *   4. If the onboarding payload (onboard.cmd) is present on the share, copy it
 *      local and run it. The batch is idempotent (it skips games already
 *      installed) and sets the Onboarded flag itself when it finishes cleanly.
 *   5. If no payload is staged yet, log and skip WITHOUT setting the flag, so a
 *      later start retries once the payload has been published to the share.
 *
 * Safe-by-default: with no payload on the share this module is a complete
 * no-op, exactly like retrowall when nothing is staged. The new binary is
 * therefore inert on the existing fleet until the payload is deliberately
 * published. The Onboarded marker is owned by the batch (not set here) so an
 * interrupted onboarding simply resumes on the next start.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "handlers.h"
#include "util.h"
#include "log.h"

#define LOG_ONBOARD  "ONBOARD"

#define ONBOARD_KEY     "Software\\RetroAgent"
#define FLAG_VALUE      "Onboarded"
#define SHARE_VALUE     "SharePath"
#define SHARE_DRIVE_V   "ShareDrive"
#define SHARE_USER_V    "ShareUser"
#define SHARE_PASS_V    "SharePass"

#define DEFAULT_SHARE   "\\\\192.168.1.122\\files"
#define DEFAULT_DRIVE   "Z:"

/* onboarding payload, relative to the share root; staged by
 * provisioning/push_onboard.py. TWO dialects: onboard.cmd runs under NT/XP
 * cmd.exe; onboard_9x.bat runs under Win98 COMMAND.COM (which has no cmd.exe
 * and rejects cmd.exe-only batch syntax). onboard_apply_startup picks the
 * right one for this OS. */
#define ONBOARD_SUBDIR   "Utility\\Retro Automation\\Onboard"
#define ONBOARD_NAME_NT  "onboard.cmd"
#define ONBOARD_NAME_9X  "onboard_9x.bat"
#define LOCAL_DIR        "C:\\RETRO_AGENT"
#define LOCAL_ONBOARD_LOG LOCAL_DIR "\\onboard.log"

/* Run after retrowall (20s) so the shell/desktop is fully up. */
#define ONBOARD_DELAY_SEC  30

static int file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int onboard_is_nt(void)
{
    OSVERSIONINFOA osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);
    return osvi.dwPlatformId == VER_PLATFORM_WIN32_NT;
}

/* Detect this box's game-relevant hardware and export it as ONB_<FLAG>
 * environment variables the onboarding batch reads to gate which games get
 * copied. The batch is launched via CreateProcessA with a NULL environment,
 * so it inherits this process's env — setting the vars here is enough. Flags
 * mirror onboard.json "capabilities":
 *   ONB_GPU3D   - a 3D display adapter (3dfx/NVIDIA/ATI/Intel)
 *   ONB_CPUFAST - CPU family >= 6 (Pentium Pro/II/III/4+, i.e. not a plain P1)
 *   ONB_RAM64   - >= 64 MB, ONB_RAM128 - >= 128 MB
 * A Pentium-1 + 2D box (e.g. a Compaq Deskpro 2000) meets neither gpu3d nor
 * cpufast, so every current game is [HWSKIP]'d and it onboards with no games. */
static void set_capability_env(void)
{
    SYSTEM_INFO si;
    MEMORYSTATUS ms;
    DISPLAY_DEVICEA dd;
    DWORD ram_mb;
    int cpufast, gpu3d = 0;

    GetSystemInfo(&si);
    cpufast = (si.wProcessorLevel >= 6);

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    ram_mb = (DWORD)(ms.dwTotalPhys / (1024 * 1024));

    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        const char *s = dd.DeviceString, *id = dd.DeviceID;
        if (strstr(s, "3dfx") || strstr(s, "Voodoo") ||
            strstr(s, "NVIDIA") || strstr(s, "GeForce") || strstr(s, "RIVA") ||
            strstr(s, "ATI") || strstr(s, "Radeon") ||
            strstr(s, "RAGE") || strstr(s, "Rage") || strstr(s, "Intel") ||
            strstr(id, "VEN_121A") || strstr(id, "VEN_10DE") ||
            strstr(id, "VEN_1002") || strstr(id, "VEN_8086"))
            gpu3d = 1;
    }

    SetEnvironmentVariableA("ONB_GPU3D",   gpu3d ? "1" : "0");
    SetEnvironmentVariableA("ONB_CPUFAST", cpufast ? "1" : "0");
    SetEnvironmentVariableA("ONB_RAM64",   ram_mb >= 64 ? "1" : "0");
    SetEnvironmentVariableA("ONB_RAM128",  ram_mb >= 128 ? "1" : "0");

    log_msg(LOG_ONBOARD, "capability: cpu_family=%u cpufast=%d gpu3d=%d "
            "ram=%luMB adapter=\"%s\"",
            si.wProcessorLevel, cpufast, gpu3d,
            (unsigned long)ram_mb, dd.DeviceString);
}

/* Read a REG_DWORD from HKLM\Software\RetroAgent. Returns the value, or `def`. */
static DWORD hklm_get_dword(const char *name, DWORD def)
{
    HKEY h;
    DWORD type, val = def, size = sizeof(val);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ONBOARD_KEY, 0, KEY_QUERY_VALUE, &h)
            == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, name, NULL, &type, (BYTE *)&val, &size)
                != ERROR_SUCCESS || type != REG_DWORD)
            val = def;
        RegCloseKey(h);
    }
    return val;
}

/* Read a REG_SZ from HKLM\Software\RetroAgent, falling back to `fallback`. */
static void hklm_get_sz(const char *name, const char *fallback,
                        char *buf, DWORD bufsize)
{
    HKEY h;
    DWORD type, size = bufsize;
    int got = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ONBOARD_KEY, 0, KEY_QUERY_VALUE, &h)
            == ERROR_SUCCESS) {
        if (RegQueryValueExA(h, name, NULL, &type, (BYTE *)buf, &size)
                == ERROR_SUCCESS && type == REG_SZ && buf[0])
            got = 1;
        RegCloseKey(h);
    }
    if (!got)
        safe_strncpy(buf, fallback, bufsize);
    else
        buf[bufsize - 1] = '\0';
}

/* Launch a process, optionally waiting up to wait_ms for it to exit. */
static BOOL run_process(const char *cmdline, DWORD wait_ms)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[1024];

    safe_strncpy(cmd, cmdline, sizeof(cmd));
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_msg(LOG_ONBOARD, "failed to launch \"%s\" (%lu)",
                cmdline, (unsigned long)GetLastError());
        return FALSE;
    }
    if (wait_ms)
        WaitForSingleObject(pi.hProcess, wait_ms);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

/* Map the file share to its drive letter. Tries session auth first, then
 * explicit stored creds if present. Best-effort; the batch also re-maps. */
static void map_share(const char *unc, const char *drive,
                      const char *user, const char *pass)
{
    char cmd[768];

    /* Drop any stale mapping on that letter first (ignore failure). */
    _snprintf(cmd, sizeof(cmd), "net use %s /delete /y", drive);
    run_process(cmd, 8000);

    if (user[0] && pass[0]) {
        _snprintf(cmd, sizeof(cmd),
                  "net use %s \"%s\" %s /user:%s /persistent:yes",
                  drive, unc, pass, user);
    } else {
        _snprintf(cmd, sizeof(cmd),
                  "net use %s \"%s\" /persistent:yes", drive, unc);
    }
    log_msg(LOG_ONBOARD, "mapping share %s -> %s", unc, drive);
    run_process(cmd, 20000);
}

void onboard_apply_startup(void)
{
    char share[MAX_PATH], drive[16], user[128], pass[128];
    char share_cmd[MAX_PATH + 128];
    char local_cmd[512];
    char local_onboard[MAX_PATH];
    const char *payload_name;
    int is_nt;

    if (hklm_get_dword(FLAG_VALUE, 0) == 1) {
        log_msg(LOG_ONBOARD, "machine already onboarded, skipping");
        return;
    }

    /* Pick the batch dialect for this OS: Win98 has no cmd.exe and rejects
     * cmd.exe-only batch syntax, so it runs onboard_9x.bat under COMMAND.COM;
     * NT/XP runs onboard.cmd under cmd.exe. */
    is_nt = onboard_is_nt();
    payload_name = is_nt ? ONBOARD_NAME_NT : ONBOARD_NAME_9X;
    _snprintf(local_onboard, sizeof(local_onboard), "%s\\%s",
              LOCAL_DIR, payload_name);
    local_onboard[sizeof(local_onboard) - 1] = '\0';

    printf("\n");
    printf("========================================================\n");
    printf("  RETRO AGENT - ONBOARDING THIS MACHINE\n");
    printf("  first run: mapping share, staging core games + desktop\n");
    printf("========================================================\n");
    fflush(stdout);
    log_msg(LOG_ONBOARD, "ONBOARDING: first run detected, bootstrapping machine");

    hklm_get_sz(SHARE_VALUE, DEFAULT_SHARE, share, sizeof(share));
    hklm_get_sz(SHARE_DRIVE_V, DEFAULT_DRIVE, drive, sizeof(drive));
    hklm_get_sz(SHARE_USER_V, "", user, sizeof(user));
    hklm_get_sz(SHARE_PASS_V, "", pass, sizeof(pass));

    map_share(share, drive, user, pass);

    /* Locate the onboarding payload on the share (prefer the mapped drive,
     * fall back to the raw UNC in case the mapping is slow to settle). */
    _snprintf(share_cmd, sizeof(share_cmd), "%s\\%s\\%s",
              drive, ONBOARD_SUBDIR, payload_name);
    if (!file_exists(share_cmd)) {
        _snprintf(share_cmd, sizeof(share_cmd), "%s\\%s\\%s",
                  share, ONBOARD_SUBDIR, payload_name);
    }
    if (!file_exists(share_cmd)) {
        printf("  onboarding payload (%s) not on the share yet - skipping.\n",
               payload_name);
        fflush(stdout);
        log_msg(LOG_ONBOARD, "no payload at %s, skipping (will retry next start)",
                share_cmd);
        return;  /* do NOT set the flag - retry once the payload is published */
    }

    /* Export detected hardware capability so the batch can gate games. */
    set_capability_env();

    CreateDirectoryA(LOCAL_DIR, NULL);
    if (!CopyFileA(share_cmd, local_onboard, FALSE)) {
        log_msg(LOG_ONBOARD, "could not copy payload %s -> %s (%lu)",
                share_cmd, local_onboard, (unsigned long)GetLastError());
        /* try running it straight off the share as a fallback */
        safe_strncpy(local_cmd, share_cmd, sizeof(local_cmd));
    } else {
        safe_strncpy(local_cmd, local_onboard, sizeof(local_cmd));
    }

    printf("  running onboarding job (games + theme); this can take a while.\n");
    printf("  progress log: %s\n", LOCAL_ONBOARD_LOG);
    fflush(stdout);
    log_msg(LOG_ONBOARD, "launching onboarding batch (%s): %s",
            is_nt ? "NT" : "Win9x", local_cmd);

    /* Run the (idempotent) batch. It owns the Onboarded flag: it sets it when
     * it completes cleanly, so an interruption just resumes next start. We give
     * it a generous window but don't block the agent indefinitely.
     *
     * Shell + redirection differ by OS: NT has cmd.exe and supports 2>&1;
     * Win98 has only COMMAND.COM and no 2>&1. */
    {
        char full[768];
        if (is_nt) {
            _snprintf(full, sizeof(full),
                      "cmd /c \"\"%s\" \"%s\" > \"%s\" 2>&1\"",
                      local_cmd, drive, LOCAL_ONBOARD_LOG);
        } else {
            _snprintf(full, sizeof(full),
                      "command.com /c %s %s > %s",
                      local_cmd, drive, LOCAL_ONBOARD_LOG);
        }
        run_process(full, 0);  /* detached; batch marks completion itself */
    }
    log_msg(LOG_ONBOARD, "onboarding batch launched");
}

DWORD WINAPI onboard_thread(LPVOID param)
{
    (void)param;
    Sleep(ONBOARD_DELAY_SEC * 1000);
    if (!g_running)
        return 0;
    onboard_apply_startup();
    return 0;
}
