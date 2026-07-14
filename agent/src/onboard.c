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
 * provisioning/push_onboard.py */
#define ONBOARD_SUBPATH "Utility\\Retro Automation\\Onboard\\onboard.cmd"
#define LOCAL_DIR       "C:\\RETRO_AGENT"
#define LOCAL_ONBOARD   LOCAL_DIR "\\onboard.cmd"
#define LOCAL_ONBOARD_LOG LOCAL_DIR "\\onboard.log"

/* Run after retrowall (20s) so the shell/desktop is fully up. */
#define ONBOARD_DELAY_SEC  30

static int file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
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

    if (hklm_get_dword(FLAG_VALUE, 0) == 1) {
        log_msg(LOG_ONBOARD, "machine already onboarded, skipping");
        return;
    }

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
    _snprintf(share_cmd, sizeof(share_cmd), "%s\\%s", drive, ONBOARD_SUBPATH);
    if (!file_exists(share_cmd)) {
        _snprintf(share_cmd, sizeof(share_cmd), "%s\\%s", share, ONBOARD_SUBPATH);
    }
    if (!file_exists(share_cmd)) {
        printf("  onboarding payload not on the share yet - skipping.\n");
        printf("  (publish provisioning/onboard.cmd to the share; will retry.)\n");
        fflush(stdout);
        log_msg(LOG_ONBOARD, "no payload at %s, skipping (will retry next start)",
                share_cmd);
        return;  /* do NOT set the flag - retry once the payload is published */
    }

    CreateDirectoryA(LOCAL_DIR, NULL);
    if (!CopyFileA(share_cmd, LOCAL_ONBOARD, FALSE)) {
        log_msg(LOG_ONBOARD, "could not copy payload %s -> %s (%lu)",
                share_cmd, LOCAL_ONBOARD, (unsigned long)GetLastError());
        /* try running it straight off the share as a fallback */
        safe_strncpy(local_cmd, share_cmd, sizeof(local_cmd));
    } else {
        safe_strncpy(local_cmd, LOCAL_ONBOARD, sizeof(local_cmd));
    }

    printf("  running onboarding job (games + theme); this can take a while.\n");
    printf("  progress log: %s\n", LOCAL_ONBOARD_LOG);
    fflush(stdout);
    log_msg(LOG_ONBOARD, "launching onboarding batch: %s", local_cmd);

    /* Run the (idempotent) batch. It owns the Onboarded flag: it sets it when
     * it completes cleanly, so an interruption just resumes next start. We give
     * it a generous window but don't block the agent indefinitely. */
    {
        char full[768];
        _snprintf(full, sizeof(full),
                  "cmd /c \"\"%s\" \"%s\" > \"%s\" 2>&1\"",
                  local_cmd, drive, LOCAL_ONBOARD_LOG);
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
