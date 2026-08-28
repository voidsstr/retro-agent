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
    { "CLICKSHOT",  1, NULL,              handle_clickshot },
    { "EXEC",       1, NULL,              handle_exec },
    { "EXECW",      1, NULL,              handle_execw },
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
    { "RESTART",    0, handle_restart,    NULL },
    { "NETMAP",     1, NULL,              handle_netmap },
    { "NETUNMAP",   1, NULL,              handle_netunmap },
    { "FILECOPY",   1, NULL,              handle_filecopy },
    { "LAUNCH",     1, NULL,              handle_launch },
    { "WINLIST",    0, handle_winlist,    NULL },
    { "UICLICK",    1, NULL,              handle_uiclick },
    { "UIDRAG",     1, NULL,              handle_uidrag },
    { "UIKEY",      1, NULL,              handle_uikey },
    { "MONITOR",    1, NULL,              handle_monitor },
    { "DRVSNAPSHOT",1, NULL,              handle_drvsnapshot },
    { "AUTOLOGIN", 1, NULL,              handle_autologin },
    { "SERVICE",   1, NULL,              handle_service },
    { "SMARTINFO",  0, handle_smartinfo,  NULL },
    { "GAMEINDEX",  1, NULL,              handle_gameindex },
    { "GAMESYNC",   1, NULL,              handle_gamesync },
    { "DRVUPDATE",  1, NULL,             handle_drvupdate },
    { "DISPLAYCFG", 1, NULL,             handle_displaycfg },
    { "AUDIOINFO",  0, handle_audioinfo,  NULL },
    { "SYSFIX",     1, NULL,             handle_sysfix },
    { "LICSTATUS",  1, NULL,            handle_licstatus },
    { "AUTOMAP",    1, NULL,             handle_automap },
    { "ONBOARD",    1, NULL,             handle_onboard },
    { "DOSSTAGE",   1, NULL,            handle_dosstage },
    { "PROMPT_PUSH",1, NULL,            handle_prompt_push },
    { "PROMPT_POP", 0, handle_prompt_pop, NULL },
    { "PROMPT_WAIT",1, NULL,            handle_prompt_wait },
    { "LOG_APPEND", 1, NULL,            handle_log_append },
    { "LOG_READ",   1, NULL,            handle_log_read },
    { "LOG_WAIT",   1, NULL,            handle_log_wait },
    { "LOG_CLEAR",  0, handle_log_clear, NULL },
    { "PROXY_GET",  0, handle_proxy_get,  NULL },
    { "PROXY_SET",  1, NULL,            handle_proxy_set },
    { "STATUS_SET", 1, NULL,            handle_status_set },
    { "STATUS_GET", 0, handle_status_get, NULL },
    { "STATUS_WAIT",1, NULL,            handle_status_wait },
    { "AI_HELLO",   0, handle_ai_hello,   NULL },
    { "AI_RESTART", 0, handle_ai_restart, NULL },
    { "AI_ENABLE",  0, handle_ai_enable,  NULL },
    { "AI_DISABLE", 0, handle_ai_disable, NULL },
    { "MODEL_LOAD", 1, NULL,            handle_model_load },
    { "MODEL_UNLOAD",1, NULL,           handle_model_unload },
    { "MODEL_LIST", 0, handle_model_list, NULL },
    { "INFER_RUN",  1, NULL,            handle_infer_run },
    { "TENSOR",     1, NULL,            handle_tensor },
    { "AI_RAW",     1, NULL,            handle_ai_raw },
    { "AI_RAWP",    1, NULL,            handle_ai_rawp },
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
/* How long the Win9x shutdown thread keeps pumping before concluding the
 * reboot did not take. Generous: the Deskpro takes its time closing DOS
 * boxes, and the only cost of waiting is a later log line. */
#define WIN9X_SHUTDOWN_WAIT_MS  60000

/* See handlers.h: keeps the console control handler from stopping the agent
 * while Win9x is tearing the session down at our own request. */
volatile int g_power_pending = 0;

/* Is this a Win9x box? (Shutdown behaviour differs completely from NT.) */
static int is_win9x(void)
{
    OSVERSIONINFOA o;
    o.dwOSVersionInfoSize = sizeof(o);
    GetVersionExA(&o);
    return o.dwPlatformId != VER_PLATFORM_WIN32_NT;
}

/* Relaunch the chat client we killed to clear its shutdown veto. Only used
 * when the reboot did NOT take - otherwise the machine is going down anyway
 * and the Run key starts it again at the next logon. */
static void restart_retro_chat(void)
{
    char path[MAX_PATH];
    char *slash;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (!GetModuleFileNameA(NULL, path, sizeof(path))) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    safe_strncpy(slash + 1, "retro_chat.exe",
                 (int)(sizeof(path) - (slash + 1 - path)));

    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        log_msg(LOG_MAIN, "Shutdown: reboot did not take - restarted %s", path);
    }
}

static void kill_console_processes(int kill_chat)
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

            /* retro_chat is one of ours and it is a console app too, so on
             * Win9x it gets a WM_QUERYENDSESSION vote like any other window
             * and can hold the shutdown up. It restarts from the Run key. */
            if (_stricmp(pe.szExeFile, "COMMAND.COM") == 0 ||
                _stricmp(pe.szExeFile, "CMD.EXE") == 0 ||
                _stricmp(pe.szExeFile, "CONAGENT.EXE") == 0 ||
                (kill_chat &&
                 _stricmp(pe.szExeFile, "RETRO_CHAT.EXE") == 0)) {
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
        /*
         * Win9x. Three things have to be true or the machine simply stays up,
         * which is what it did: REBOOT returned OK, the agent closed, and
         * Windows never restarted.
         *
         *  1. The calling thread needs a message queue, or the shutdown
         *     messages are never dispatched. The hidden popup provides one.
         *  2. We must keep pumping until Windows kills us. Win9x sends
         *     WM_QUERYENDSESSION round every top-level window and waits for
         *     the answers; a process that stops pumping IS the veto.
         *  3. THE AGENT MUST NOT EXIT. do_system_power() used to set
         *     g_running = 0 half a second after starting this thread, so the
         *     process died while the shutdown was still being negotiated and
         *     took this thread with it - aborting the very shutdown it had
         *     just asked for. On Win9x the caller now leaves g_running alone
         *     and lets the OS terminate us.
         */
        DWORD deadline;

        hwnd = CreateWindowA("STATIC", "", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);

        log_msg(LOG_MAIN, "Shutdown: ExitWindowsEx(%u) on Win9x", flags);
        result = ExitWindowsEx(flags, 0);
        log_msg(LOG_MAIN, "Shutdown: ExitWindowsEx = %d, err=%lu",
                result, (unsigned long)GetLastError());
        /* From here the machine may go at any moment; this verdict is the
         * whole record of whether the call was accepted. */
        log_flush();

        if (!result) {
            /* EWX_FORCE can be refused while a DOS box is still closing.
             * Give the kills a moment, then try again without FORCE, which
             * some Win9x builds accept when the forced form did not. */
            Sleep(1000);
            kill_console_processes(1);
            Sleep(500);
            result = ExitWindowsEx(flags & ~EWX_FORCE, 0);
            log_msg(LOG_MAIN, "Shutdown: retry without FORCE = %d, err=%lu",
                    result, (unsigned long)GetLastError());
            log_flush();
        }

        /* Pump until the OS tears us down. The bound is a backstop only: if
         * we are still alive after it, the reboot genuinely did not take and
         * saying so in the log beats looking like a successful reboot. */
        deadline = GetTickCount() + WIN9X_SHUTDOWN_WAIT_MS;
        while ((long)(GetTickCount() - deadline) < 0) {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            Sleep(50);
        }

        log_msg(LOG_MAIN, "Shutdown: STILL RUNNING after %lu ms - the reboot "
                "did not take; the machine needs a manual restart",
                (unsigned long)WIN9X_SHUTDOWN_WAIT_MS);
        /* We killed the chat client to clear its veto and the machine is
         * still here, so put it back rather than leaving the box without it
         * until somebody logs on again. */
        restart_retro_chat();
        g_power_pending = 0;      /* allow another attempt */
        log_flush();

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
    OSVERSIONINFOA osvi;

    if (g_power_pending) {
        /* Now that the agent survives a Win9x shutdown attempt, a repeated
         * REBOOT would stack another 60-second pumping thread and re-kill the
         * consoles each time. One at a time. */
        send_text_response(sock, "OK (a power operation is already in flight)");
        log_msg(LOG_MAIN, "%s: ignored - one is already in flight", label);
        return;
    }

    send_text_response(sock, "OK");
    log_msg(LOG_MAIN, "%s: initiating", label);
    /* Commit the log now: everything after this point races the machine
     * going down, and this line is what tells us a reboot was even asked
     * for if it does not come back. */
    log_flush();

    /* Only clear the chat client's shutdown veto on Win9x: on NT it has no
     * vote, and killing it there is pure collateral damage. */
    kill_console_processes(is_win9x());
    Sleep(200);

    /* Announce the pending power operation BEFORE anything can start tearing
     * the session down, so the console control handler never mistakes a
     * shutdown we asked for for a reason to stop the agent. */
    g_power_pending = 1;

    {
        HANDLE th = CreateThread(NULL, 0, system_shutdown_thread,
                                 (LPVOID)(UINT_PTR)ewx_flags, 0, NULL);
        if (th) CloseHandle(th);        /* fire-and-forget: don't leak it */
        else {
            log_msg(LOG_MAIN, "%s: FAILED to start the shutdown thread", label);
            log_flush();
            g_power_pending = 0;
        }
    }

    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionExA(&osvi);

    if (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        /* NT hands off to shutdown.exe, which survives us exiting. */
        Sleep(500);
        g_running = 0;
    }
    /* Win9x: deliberately DO NOT stop the agent. Killing ourselves here is
     * what broke REBOOT on Win98 - the shutdown is still being negotiated
     * with every top-level window, and the process that requested it dying
     * mid-negotiation cancels it. The result looked exactly like a working
     * command: "OK" on the wire, retro_agent.exe closes... and Windows
     * stays up. Let the OS terminate us instead. */
}

void handle_quit(SOCKET sock)
{
    send_text_response(sock, "OK");
    g_running = 0;
}

/*
 * RESTART - stop the agent and bring it back up.
 *
 * QUIT alone is a footgun on Win9x: nothing supervises the agent there (the
 * RetroAgent Run key only fires at logon), so a remote QUIT takes the box off
 * the network until someone walks over to it. Learned the hard way on the
 * Deskpro, 2026-07-29.
 *
 * So: write a detached batch that waits for this process to exit and then
 * relaunches the exe, start it, and only then stop. The orphaned batch
 * survives the agent's death and brings it back — the same trick used to
 * restart games and the agent on .124.
 */
void handle_restart(SOCKET sock)
{
    char exe[MAX_PATH];
    char dir[MAX_PATH];
    char bat[MAX_PATH];
    char cmd[MAX_PATH * 2];
    char *slash;
    FILE *f;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameA(NULL, exe, sizeof(exe));
    exe[sizeof(exe) - 1] = '\0';

    safe_strncpy(dir, exe, sizeof(dir));
    slash = strrchr(dir, '\\');
    if (slash) *slash = '\0';

    _snprintf(bat, sizeof(bat), "%s\\restart.bat", dir);
    bat[sizeof(bat) - 1] = '\0';

    f = fopen(bat, "wb");
    if (!f) {
        send_error_response(sock, "cannot write restart batch");
        return;
    }
    /* Win9x COMMAND.COM dialect: no 2>&1, ping as the sleep. */
    fprintf(f, "@echo off\r\n");
    fprintf(f, "ping -n 4 127.0.0.1 > nul\r\n");
    fprintf(f, "start \"\" \"%s\"\r\n", exe);
    fclose(f);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    _snprintf(cmd, sizeof(cmd), "%s", bat);
    cmd[sizeof(cmd) - 1] = '\0';

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
        send_error_response(sock, "cannot launch restart batch");
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    send_text_response(sock, "OK restarting");
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
