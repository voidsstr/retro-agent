/*
 * autoupdate.c - Self-update from network share at startup
 *
 * Runs in a background thread after the agent starts.  Compares the
 * local binary size with the copy on the network share.  If they
 * differ, copies the share binary to a temp location, writes a small
 * batch script that waits for the old process to exit, overwrites the
 * local binary, and starts the new one.  Then signals the agent to
 * shut down so the batch can take over.
 *
 * The share path is read from the registry:
 *   HKLM\Software\RetroAgent\UpdatePath  (REG_SZ)
 * Default: \\192.168.1.122\files\Utility\Retro Automation\retro_agent.exe
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

#define LOG_UPDATE  "UPDATE"

#define UPDATE_KEY      "Software\\RetroAgent"
#define UPDATE_VALUE    "UpdatePath"
#define DEFAULT_UPDATE_PATH \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\retro_agent.exe"

/* Delay before checking (seconds).  Gives network time to initialize. */
#define UPDATE_DELAY_SEC  15

static void get_local_exe_path(char *buf, int bufsize)
{
    GetModuleFileNameA(NULL, buf, bufsize);
    buf[bufsize - 1] = '\0';
}

static int get_update_path(char *buf, int bufsize)
{
    HKEY hKey;
    DWORD size, type;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, UPDATE_KEY, 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        size = bufsize;
        if (RegQueryValueExA(hKey, UPDATE_VALUE, NULL, &type,
                             (BYTE *)buf, &size) == ERROR_SUCCESS
            && type == REG_SZ && buf[0]) {
            RegCloseKey(hKey);
            return 1;
        }
        RegCloseKey(hKey);
    }

    /* Fallback to default */
    safe_strncpy(buf, DEFAULT_UPDATE_PATH, bufsize);
    return 1;
}

static DWORD get_file_size(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(h);
    return fd.nFileSizeLow;
}

static int build_restart_bat(const char *bat_path, const char *install_dir,
                             const char *temp_exe)
{
    FILE *f = fopen(bat_path, "wb");
    if (!f) return 0;

    /* Win98 COMMAND.COM: no 2>&1, don't redirect copy output
     * (suppresses errorlevel).  Paths have no spaces so no quotes needed. */
    fprintf(f, "@echo off\r\n");
    fprintf(f, "echo Auto-update: waiting for old agent to exit...\r\n");
    fprintf(f, ":wait\r\n");
    fprintf(f, "ping -n 3 127.0.0.1 > nul\r\n");
    fprintf(f, "copy /Y %s %s\\retro_agent.exe\r\n", temp_exe, install_dir);
    fprintf(f, "if errorlevel 1 goto wait\r\n");
    fprintf(f, "del %s\r\n", temp_exe);
    fprintf(f, "echo Auto-update: starting new agent...\r\n");
    fprintf(f, "start %s\\retro_agent.exe\r\n", install_dir);
    fprintf(f, "del %s\\autoupdate.bat\r\n", install_dir);

    fclose(f);
    return 1;
}

DWORD WINAPI autoupdate_thread(LPVOID param)
{
    char local_path[MAX_PATH];
    char update_path[512];
    char install_dir[MAX_PATH];
    char temp_exe[MAX_PATH];
    char bat_path[MAX_PATH];
    DWORD local_size, remote_size;
    char *last_slash;

    (void)param;

    /* Wait for network to stabilize */
    Sleep(UPDATE_DELAY_SEC * 1000);

    if (!g_running) return 0;

    get_local_exe_path(local_path, sizeof(local_path));
    if (!get_update_path(update_path, sizeof(update_path))) {
        log_msg(LOG_UPDATE, "No update path configured, skipping");
        return 0;
    }

    log_msg(LOG_UPDATE, "Checking for update: %s", update_path);

    /* Get local and remote file sizes */
    local_size = get_file_size(local_path);
    remote_size = get_file_size(update_path);

    if (remote_size == 0) {
        log_msg(LOG_UPDATE, "Cannot access share binary (network not ready or path invalid)");
        return 0;
    }

    if (local_size == remote_size) {
        log_msg(LOG_UPDATE, "Binary is current (%lu bytes), no update needed",
                (unsigned long)local_size);
        return 0;
    }

    log_msg(LOG_UPDATE, "Update available: local=%lu remote=%lu bytes",
            (unsigned long)local_size, (unsigned long)remote_size);

    /* Derive install directory from local exe path */
    safe_strncpy(install_dir, local_path, sizeof(install_dir));
    last_slash = strrchr(install_dir, '\\');
    if (last_slash) *last_slash = '\0';

    /* Copy new binary from share to temp location */
    _snprintf(temp_exe, sizeof(temp_exe), "%s\\retro_update.tmp", install_dir);
    temp_exe[sizeof(temp_exe) - 1] = '\0';

    log_msg(LOG_UPDATE, "Copying update to %s", temp_exe);
    if (!CopyFileA(update_path, temp_exe, FALSE)) {
        log_msg(LOG_UPDATE, "CopyFile failed: %lu", (unsigned long)GetLastError());
        return 0;
    }

    /* Verify copy succeeded */
    if (get_file_size(temp_exe) != remote_size) {
        log_msg(LOG_UPDATE, "Copy size mismatch, aborting");
        DeleteFileA(temp_exe);
        return 0;
    }

    /* Write restart batch script */
    _snprintf(bat_path, sizeof(bat_path), "%s\\autoupdate.bat", install_dir);
    bat_path[sizeof(bat_path) - 1] = '\0';

    if (!build_restart_bat(bat_path, install_dir, temp_exe)) {
        log_msg(LOG_UPDATE, "Cannot write restart batch, aborting");
        DeleteFileA(temp_exe);
        return 0;
    }

    /* Launch the batch and shut down */
    log_msg(LOG_UPDATE, "Launching restart batch and shutting down");
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        memset(&pi, 0, sizeof(pi));

        if (CreateProcessA(NULL, bat_path, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    g_running = 0;
    return 0;
}
