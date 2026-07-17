/*
 * retrowall.c - Ensure the retro "dossier" wallpaper rotation is applied and the
 * desktop icons are parked in the blank icon well, every time the agent starts.
 *
 * The per-machine wallpaper BMPs and the two helper exes are staged onto the
 * machine ahead of time by the server-side retro-wallpaper skill
 * (deploy_rotation.py) into C:\retro-wall\ :
 *
 *     C:\retro-wall\wall00.bmp .. wallNN.bmp   (the iterations)
 *     C:\retro-wall\rotate_wall.exe            (GUI, single-instance, cycles them)
 *     C:\retro-wall\arrange_icons.exe          (parks icons in the bottom-right well)
 *
 * The agent cannot render those BMPs itself (they encode per-machine specs), so
 * this module only *applies* what has been staged. If nothing is staged yet it
 * quietly does nothing. When the rotation is present it, on every startup:
 *
 *   1. makes sure the wallpaper style is set (stretch, no tile),
 *   2. applies a retro wallpaper immediately if the current one isn't ours
 *      ("set them if they are not already set"),
 *   3. (re)installs the HKCU Run key so the rotator survives logon,
 *   4. launches the rotator (its named mutex dedupes, so this is a no-op if it's
 *      already running), and
 *   5. runs arrange_icons.exe to move the desktop icons into the blank well.
 *
 * Runs in a background thread after a short delay so the shell/desktop is up
 * (the icon listview lives in explorer, and SPI_SETDESKWALLPAPER needs the
 * interactive session). The agent normally runs from an HKLM Run key under
 * autologon, i.e. in the user's session, so these calls reach the visible
 * desktop. In NT service mode (session 0) they are harmless no-ops.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "handlers.h"
#include "util.h"
#include "log.h"

#define WALLDIR        "C:\\retro-wall"
#define ROTATE_EXE     WALLDIR "\\rotate_wall.exe"
#define ARRANGE_EXE    WALLDIR "\\arrange_icons.exe"
#define THEME_REG      WALLDIR "\\retro_theme.reg"
#define SETCOLORS_EXE  WALLDIR "\\setsyscolors.exe"
#define WALL0_BMP      WALLDIR "\\wall00.bmp"
#define RUN_KEY        "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE      "RetroWallRotate"
#define DESKTOP_KEY    "Control Panel\\Desktop"

/* Let the shell/desktop finish coming up before we touch it. */
#define RETROWALL_DELAY_SEC  20

#ifndef SPI_SETDESKWALLPAPER
#define SPI_SETDESKWALLPAPER 0x0014
#endif

static int file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* Set a REG_SZ under HKCU\<subkey>. */
static void hkcu_set_sz(const char *subkey, const char *name, const char *value)
{
    HKEY h;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &h, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(h, name, 0, REG_SZ,
                       (const BYTE *)value, (DWORD)(strlen(value) + 1));
        RegCloseKey(h);
    }
}

/* Read a REG_SZ from HKCU\<subkey>. Returns 0 on success. */
static int hkcu_get_sz(const char *subkey, const char *name, char *buf, DWORD bufsize)
{
    HKEY h;
    DWORD type, size = bufsize;
    LONG rc;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &h)
            != ERROR_SUCCESS)
        return -1;
    rc = RegQueryValueExA(h, name, NULL, &type, (BYTE *)buf, &size);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return -1;
    buf[bufsize - 1] = '\0';
    return 0;
}

/* Launch a process. If wait_ms > 0, wait up to that long for it to exit. */
static void run_process(const char *cmdline, DWORD wait_ms)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[512];

    safe_strncpy(cmd, cmdline, sizeof(cmd));  /* CreateProcess may modify the buffer */

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (wait_ms)
            WaitForSingleObject(pi.hProcess, wait_ms);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        log_msg(LOG_MAIN, "retrowall: failed to launch \"%s\" (%lu)",
                cmdline, (unsigned long)GetLastError());
    }
}

/*
 * Apply the staged wallpaper rotation + icon layout. Safe to call every startup;
 * a no-op when nothing is staged.
 */
void retrowall_apply_startup(void)
{
    char runcmd[512];
    char curwall[MAX_PATH];

    if (!file_exists(ROTATE_EXE)) {
        log_msg(LOG_MAIN, "retrowall: no rotation staged (%s missing), skipping",
                ROTATE_EXE);
        return;
    }

    log_msg(LOG_MAIN, "retrowall: applying staged wallpaper rotation + icon layout");

    /* 1. Wallpaper style: stretch to fill (2), no tiling. */
    hkcu_set_sz(DESKTOP_KEY, "WallpaperStyle", "2");
    hkcu_set_sz(DESKTOP_KEY, "TileWallpaper", "0");

    /* 2. If the current wallpaper isn't one of ours, apply wall00 immediately so
     *    it's set even before the rotator's first tick. The rotator then advances
     *    it on its interval. */
    if (file_exists(WALL0_BMP)) {
        int need = 1;
        if (hkcu_get_sz(DESKTOP_KEY, "Wallpaper", curwall, sizeof(curwall)) == 0) {
            if (_strnicmp(curwall, WALLDIR "\\", (int)strlen(WALLDIR) + 1) == 0)
                need = 0;  /* already a retro-wall wallpaper */
        }
        if (need) {
            SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, (PVOID)WALL0_BMP,
                                  SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
            log_msg(LOG_MAIN, "retrowall: applied %s (was \"%s\")",
                    WALL0_BMP, curwall[0] ? curwall : "(none)");
        }
    }

    /* 3. Persist the rotator across logon. Preserve an existing command (it
     *    carries the chosen interval); otherwise install a default. */
    if (hkcu_get_sz(RUN_KEY, RUN_VALUE, runcmd, sizeof(runcmd)) != 0 || !runcmd[0]) {
        _snprintf(runcmd, sizeof(runcmd), "%s 60", ROTATE_EXE);
        hkcu_set_sz(RUN_KEY, RUN_VALUE, runcmd);
        log_msg(LOG_MAIN, "retrowall: installed Run key (%s)", runcmd);
    }

    /* 4. Ensure the rotator is running (its named mutex makes this idempotent). */
    run_process(runcmd, 0);

    /* 5. Park the desktop icons in the blank well. */
    if (file_exists(ARRANGE_EXE)) {
        run_process(ARRANGE_EXE, 15000);
        log_msg(LOG_MAIN, "retrowall: arranged desktop icons");
    } else {
        log_msg(LOG_MAIN, "retrowall: %s missing, icons not arranged", ARRANGE_EXE);
    }

    /* 6. Apply the dark "hacker" system-color theme (the fleet-wide look) on
     *    every startup, the same way the wallpaper + icons are. Staged as
     *    retro_theme.reg (writes HKCU\Control Panel\Colors) + setsyscolors.exe
     *    (pushes those colors live via SetSysColors so it takes effect without a
     *    re-logon). Both are staged by the retro-wallpaper skill's
     *    deploy_rotation.py; a no-op here if not present. */
    if (file_exists(THEME_REG)) {
        char themecmd[512];
        _snprintf(themecmd, sizeof(themecmd), "regedit /s \"%s\"", THEME_REG);
        run_process(themecmd, 8000);          /* HKCU\Control Panel\Colors */
        if (file_exists(SETCOLORS_EXE)) {
            run_process(SETCOLORS_EXE, 8000); /* apply live via SetSysColors */
            log_msg(LOG_MAIN, "retrowall: applied dark theme (colors + live SetSysColors)");
        } else {
            log_msg(LOG_MAIN, "retrowall: theme colors set; %s missing "
                    "(live on next logon)", SETCOLORS_EXE);
        }
    } else {
        log_msg(LOG_MAIN, "retrowall: %s missing, dark theme not applied", THEME_REG);
    }
}

DWORD WINAPI retrowall_thread(LPVOID param)
{
    (void)param;
    Sleep(RETROWALL_DELAY_SEC * 1000);
    if (!g_running)
        return 0;
    retrowall_apply_startup();
    return 0;
}
