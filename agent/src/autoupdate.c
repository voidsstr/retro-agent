/*
 * autoupdate.c - Self-update from network share at startup
 *
 * Runs in a background thread after the agent starts. Two responsibilities:
 *
 *   1. Update retro_chat.exe in-place: if the share copy differs from
 *      the local copy, kill any running retro_chat process, copy the
 *      new binary into the install dir, and relaunch it. This is safe
 *      to do without restarting the agent because retro_chat is a
 *      child process, not a dependency of the agent itself.
 *
 *   2. Update retro_agent.exe via restart: if the share copy differs,
 *      stage the new binary as retro_update.tmp, write a batch script
 *      that waits for the old agent to exit and then swaps the binary,
 *      then signal the agent to shut down so the batch can take over.
 *
 * The share paths are read from the registry under HKLM\Software\RetroAgent:
 *   UpdatePath     - retro_agent.exe (default: \\192.168.1.122\files\Utility\Retro Automation\retro_agent.exe)
 *   ChatUpdatePath - retro_chat.exe  (default: \\192.168.1.122\files\Utility\Retro Automation\retro_chat.exe)
 *
 * Both checks happen on every start; the chat check runs first so its
 * binary is current even when the agent itself doesn't need an update.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <tlhelp32.h>

#define LOG_UPDATE  "UPDATE"

#define UPDATE_KEY        "Software\\RetroAgent"
#define UPDATE_VALUE      "UpdatePath"
#define CHAT_UPDATE_VALUE "ChatUpdatePath"
#define DEFAULT_UPDATE_PATH \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\retro_agent.exe"
#define DEFAULT_CHAT_UPDATE_PATH \
    "\\\\192.168.1.122\\files\\Utility\\Retro Automation\\retro_chat.exe"

/* Delay before checking (seconds).  Gives network time to initialize. */
#define UPDATE_DELAY_SEC  15

static void get_local_exe_path(char *buf, int bufsize)
{
    GetModuleFileNameA(NULL, buf, bufsize);
    buf[bufsize - 1] = '\0';
}

static int read_reg_path(const char *value_name, const char *fallback,
                         char *buf, int bufsize)
{
    HKEY hKey;
    DWORD size, type;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, UPDATE_KEY, 0,
                      KEY_READ, &hKey) == ERROR_SUCCESS) {
        size = bufsize;
        if (RegQueryValueExA(hKey, value_name, NULL, &type,
                             (BYTE *)buf, &size) == ERROR_SUCCESS
            && type == REG_SZ && buf[0]) {
            RegCloseKey(hKey);
            return 1;
        }
        RegCloseKey(hKey);
    }
    safe_strncpy(buf, fallback, bufsize);
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

/*
 * Kill any running retro_chat.exe process so we can overwrite its
 * binary. Safe to call even if no chat process is running.
 */
static int kill_retro_chat(void)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    int killed = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "retro_chat.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE,
                                       pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 0);
                    CloseHandle(h);
                    killed++;
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

/*
 * Update retro_chat.exe in place. Returns 1 on success/no-op, 0 on
 * failure. Does NOT trigger an agent restart — chat is independent.
 *
 * Steps:
 *   1. Read share path from registry (or use default).
 *   2. Compare local and remote sizes; skip if equal.
 *   3. Kill running retro_chat.exe (so we can overwrite the binary).
 *   4. Wait briefly, then CopyFile from share to install dir.
 *   5. Relaunch the new retro_chat.exe via CreateProcess.
 */
static void update_retro_chat(const char *install_dir)
{
    char chat_path[MAX_PATH];
    char share_path[512];
    DWORD local_size, remote_size;
    int kill_count;

    _snprintf(chat_path, sizeof(chat_path), "%s\\retro_chat.exe", install_dir);
    chat_path[sizeof(chat_path) - 1] = '\0';

    read_reg_path(CHAT_UPDATE_VALUE, DEFAULT_CHAT_UPDATE_PATH,
                  share_path, sizeof(share_path));

    log_msg(LOG_UPDATE, "Checking chat update: %s", share_path);

    remote_size = get_file_size(share_path);
    if (remote_size == 0) {
        log_msg(LOG_UPDATE, "Chat: cannot access share binary");
        return;
    }

    local_size = get_file_size(chat_path);

    if (local_size == remote_size) {
        log_msg(LOG_UPDATE, "Chat: current (%lu bytes), no update",
                (unsigned long)local_size);
        return;
    }

    log_msg(LOG_UPDATE, "Chat: update available local=%lu remote=%lu",
            (unsigned long)local_size, (unsigned long)remote_size);

    /* Kill any running retro_chat so we can overwrite the file. */
    kill_count = kill_retro_chat();
    if (kill_count > 0) {
        log_msg(LOG_UPDATE, "Chat: terminated %d running instance(s)",
                kill_count);
        Sleep(500);  /* let file handle release */
    }

    /* Replace the binary. */
    if (!CopyFileA(share_path, chat_path, FALSE)) {
        log_msg(LOG_UPDATE, "Chat: CopyFile failed: %lu",
                (unsigned long)GetLastError());
        return;
    }

    log_msg(LOG_UPDATE, "Chat: updated to %lu bytes",
            (unsigned long)remote_size);

    /* Relaunch the new binary. CreateProcess (not start) so it inherits
     * a clean environment without a console window. */
    {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[MAX_PATH + 4];

        _snprintf(cmd, sizeof(cmd), "\"%s\"", chat_path);
        cmd[sizeof(cmd) - 1] = '\0';

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));

        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                           CREATE_NEW_CONSOLE, NULL, install_dir, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            log_msg(LOG_UPDATE, "Chat: relaunched");
        } else {
            log_msg(LOG_UPDATE, "Chat: relaunch failed: %lu",
                    (unsigned long)GetLastError());
        }
    }
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

    /* Derive install directory from local exe path (need it for chat check) */
    safe_strncpy(install_dir, local_path, sizeof(install_dir));
    last_slash = strrchr(install_dir, '\\');
    if (last_slash) *last_slash = '\0';

    /* Update retro_chat first — it doesn't restart the agent. */
    update_retro_chat(install_dir);

    if (!g_running) return 0;

    /* Now check for an agent self-update. */
    read_reg_path(UPDATE_VALUE, DEFAULT_UPDATE_PATH,
                  update_path, sizeof(update_path));

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
