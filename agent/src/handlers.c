/*
 * handlers.c - Command dispatch and routing
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <tlhelp32.h>

typedef struct {
    const char *name;
    int         has_args;
    void       (*handler_no_args)(SOCKET sock);
    void       (*handler_with_args)(SOCKET sock, const char *args);
} cmd_entry_t;

static const cmd_entry_t commands[] = {
    { "PING",       0, handle_ping,       NULL },
    { "SYSINFO",    0, handle_sysinfo,    NULL },
    { "VIDEODIAG",  0, handle_videodiag,  NULL },
    { "DRIVERS",    1, NULL,              handle_drivers },
    { "SCREENSHOT", 1, NULL,              handle_screenshot },
    { "SCREENDIFF", 1, NULL,              handle_screendiff },
    { "EXEC",       1, NULL,              handle_exec },
    { "UPLOAD",     1, NULL,              handle_upload },
    { "DOWNLOAD",   1, NULL,              handle_download },
    { "DIRLIST",    1, NULL,              handle_dirlist },
    { "MKDIR",      1, NULL,              handle_mkdir },
    { "DELETE",     1, NULL,              handle_delete },
    { "REGREAD",    1, NULL,              handle_regread },
    { "REGWRITE",   1, NULL,              handle_regwrite },
    { "REGDELETE",  1, NULL,              handle_regdelete },
    { "PCISCAN",    0, handle_pciscan,    NULL },
    { "PROCLIST",   0, handle_proclist,   NULL },
    { "PROCKILL",   1, NULL,              handle_prockill },
    { "SHUTDOWN",   0, handle_shutdown,   NULL },
    { "REBOOT",     0, handle_reboot,     NULL },
    { "QUIT",       0, handle_quit,       NULL },
    { "NETMAP",     1, NULL,              handle_netmap },
    { "NETUNMAP",   1, NULL,              handle_netunmap },
    { "FILECOPY",   1, NULL,              handle_filecopy },
    { "LAUNCH",     1, NULL,              handle_launch },
    { "WINLIST",    0, handle_winlist,    NULL },
    { "UICLICK",    1, NULL,              handle_uiclick },
    { "UIDRAG",     1, NULL,              handle_uidrag },
    { "UIKEY",      1, NULL,              handle_uikey },
    { "DRVSNAPSHOT",1, NULL,              handle_drvsnapshot },
    { "AUTOLOGIN", 1, NULL,              handle_autologin },
    { "SERVICE",   1, NULL,              handle_service },
    { "SMARTINFO",  0, handle_smartinfo,  NULL },
    { "DISPLAYCFG", 1, NULL,             handle_displaycfg },
    { "AUDIOINFO",  0, handle_audioinfo,  NULL },
    { "SYSFIX",     1, NULL,             handle_sysfix },
    { "AUTOMAP",    1, NULL,             handle_automap },
    { NULL,         0, NULL,              NULL }
};

void handle_command(SOCKET sock, const char *cmd, DWORD cmd_len)
{
    const cmd_entry_t *entry;
    char cmd_name[32];
    const char *args = NULL;
    int i;

    (void)cmd_len;

    /* Extract command name (first word) */
    for (i = 0; cmd[i] && cmd[i] != ' ' && i < (int)sizeof(cmd_name) - 1; i++)
        cmd_name[i] = cmd[i];
    cmd_name[i] = '\0';

    /* Find args after first space */
    if (cmd[i] == ' ')
        args = str_skip_spaces(cmd + i + 1);

    /* Look up command */
    for (entry = commands; entry->name; entry++) {
        if (_stricmp(cmd_name, entry->name) == 0) {
            if (entry->has_args) {
                entry->handler_with_args(sock, args ? args : "");
            } else {
                entry->handler_no_args(sock);
            }
            return;
        }
    }

    send_error_response(sock, "Unknown command");
}

/* Simple handlers implemented directly here */

void handle_ping(SOCKET sock)
{
    send_text_response(sock, "PONG");
}

/*
 * Kill console/batch processes that can block ExitWindowsEx on Win9x.
 * Win98's EWX_FORCE doesn't reliably terminate console host windows
 * (WINOA386.MOD) or COMMAND.COM/CMD.EXE instances running batch files.
 */
static void kill_console_processes(void)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    DWORD my_pid = GetCurrentProcessId();

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            HANDLE h;
            if (pe.th32ProcessID == my_pid) continue;

            if (_stricmp(pe.szExeFile, "COMMAND.COM") == 0 ||
                _stricmp(pe.szExeFile, "CMD.EXE") == 0 ||
                _stricmp(pe.szExeFile, "CONAGENT.EXE") == 0) {
                h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    log_msg(LOG_MAIN, "Shutdown: killing %s (PID %lu)",
                            pe.szExeFile, (unsigned long)pe.th32ProcessID);
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

/*
 * Enable SeShutdownPrivilege on NT (required for ExitWindowsEx/shutdown.exe).
 */
static void acquire_shutdown_privilege(void)
{
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        LookupPrivilegeValueA(NULL, "SeShutdownPrivilege",
                              &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(hToken);
    }
}

/*
 * System shutdown/reboot thread.
 *
 * ExitWindowsEx on Win9x requires the calling thread to have a message
 * queue, otherwise shutdown messages are never dispatched and the call
 * silently fails.  We create a hidden window (gives us a msg queue),
 * call ExitWindowsEx, then pump messages.
 *
 * On NT, ExitWindowsEx works but shutdown.exe is more reliable as a
 * fallback (handles services, forced app termination, etc.).
 *
 * param = EWX_* flags cast to LPVOID.
 */
static DWORD WINAPI system_shutdown_thread(LPVOID param)
{
    UINT flags = (UINT)(UINT_PTR)param;
    OSVERSIONINFOA osvi;
    BOOL result;
    MSG msg;
    HWND hwnd;

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);

    if (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        acquire_shutdown_privilege();

        /* NT/XP: shutdown.exe is the most reliable method.
         * Handles services, logged-in users, and forced termination. */
        {
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            char cmd[64];

            if (flags & EWX_REBOOT)
                safe_strncpy(cmd, "shutdown.exe /r /t 0 /f", sizeof(cmd));
            else
                safe_strncpy(cmd, "shutdown.exe /s /t 0 /f", sizeof(cmd));

            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            memset(&pi, 0, sizeof(pi));

            log_msg(LOG_MAIN, "Shutdown: running \"%s\"", cmd);
            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                               CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            } else {
                log_msg(LOG_MAIN, "Shutdown: CreateProcess failed (%lu), "
                        "trying ExitWindowsEx",
                        (unsigned long)GetLastError());
                ExitWindowsEx(flags, 0);
            }
        }
    } else {
        /* Win9x: ExitWindowsEx needs a message queue on this thread.
         * Create a hidden zero-size popup to get one. */
        hwnd = CreateWindowA("STATIC", "", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);

        log_msg(LOG_MAIN, "Shutdown: ExitWindowsEx(%u) on Win9x", flags);
        result = ExitWindowsEx(flags, 0);
        log_msg(LOG_MAIN, "Shutdown: ExitWindowsEx = %d, err=%lu",
                result, (unsigned long)GetLastError());

        /* Pump messages for up to 5s to let shutdown proceed */
        {
            DWORD deadline = GetTickCount() + 5000;
            while (GetTickCount() < deadline) {
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                Sleep(100);
            }
        }

        if (hwnd) DestroyWindow(hwnd);
    }

    return 0;
}

/*
 * Initiate system shutdown or reboot.
 * Kills blocking console processes, then launches a helper thread
 * with a message pump to perform the actual ExitWindowsEx/shutdown.exe.
 */
static void do_system_power(SOCKET sock, const char *label, UINT ewx_flags)
{
    send_text_response(sock, "OK");
    log_msg(LOG_MAIN, "%s: initiating", label);

    kill_console_processes();
    Sleep(200);

    CreateThread(NULL, 0, system_shutdown_thread,
                 (LPVOID)(UINT_PTR)ewx_flags, 0, NULL);

    /* Let the shutdown thread start before we exit the agent loop */
    Sleep(500);
    g_running = 0;
}

void handle_quit(SOCKET sock)
{
    send_text_response(sock, "OK");
    g_running = 0;
}

void handle_shutdown(SOCKET sock)
{
    do_system_power(sock, "SHUTDOWN",
                    EWX_SHUTDOWN | EWX_FORCE | EWX_POWEROFF);
}

void handle_reboot(SOCKET sock)
{
    do_system_power(sock, "REBOOT", EWX_REBOOT | EWX_FORCE);
}
