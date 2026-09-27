/*
 * handlers.c - Command dispatch and routing
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include "hostpolicy.h"
#include <string.h>
#include <stdio.h>
#include <tlhelp32.h>

typedef struct {
    const char *name;
    int         has_args;
    void       (*handler_no_args)(SOCKET sock);
    void       (*handler_with_args)(SOCKET sock, const char *args);
    /*
     * 1 = this command RECONFIGURES THE HOST as a retro fleet box (skins it,
     * stages content onto it, changes how it boots or what drivers it runs).
     * Refused on modern Windows - see hostpolicy.h.
     *
     * This is deliberately NOT "anything that writes". The general-purpose
     * remote-control primitives - EXEC, UPLOAD, DELETE, REGWRITE, NETMAP,
     * SERVICE, REBOOT - are the OPERATOR's own hands, and an operator driving
     * their own PC through the agent is not the agent imposing a fleet policy
     * on it. Blocking those would make the agent useless on the very box it is
     * installed on to reach (the copier host), and would not stop anything the
     * operator could not do by opening a shell.
     *
     * Rows that omit this field get 0 from C's initializer rules, so a NEW
     * command defaults to ALLOWED. If you add one that reconfigures the host,
     * set this to 1 - tests/python/test_hostpolicy.py pins the current set.
     */
    int         reconfigures_host;
} cmd_entry_t;

static const cmd_entry_t commands[] = {
    { "PING",       0, handle_ping,       NULL, 0 },
    { "SYSINFO",    0, handle_sysinfo,    NULL, 0 },
    { "VIDEODIAG",  0, handle_videodiag,  NULL, 0 },
    { "DRIVERS",    1, NULL,              handle_drivers, 0 },
    { "SCREENSHOT", 1, NULL,              handle_screenshot, 0 },
    { "SCREENDIFF", 1, NULL,              handle_screendiff, 0 },
    { "CLICKSHOT",  1, NULL,              handle_clickshot, 0 },
    { "EXEC",       1, NULL,              handle_exec, 0 },
    { "EXECW",      1, NULL,              handle_execw, 0 },
    { "UPLOAD",     1, NULL,              handle_upload, 0 },
    { "DOWNLOAD",   1, NULL,              handle_download, 0 },
    { "DIRLIST",    1, NULL,              handle_dirlist, 0 },
    { "MKDIR",      1, NULL,              handle_mkdir, 0 },
    { "DELETE",     1, NULL,              handle_delete, 0 },
    { "REGREAD",    1, NULL,              handle_regread, 0 },
    { "REGWRITE",   1, NULL,              handle_regwrite, 0 },
    { "REGDELETE",  1, NULL,              handle_regdelete, 0 },
    { "PCISCAN",    0, handle_pciscan,    NULL, 0 },
    { "PROCLIST",   0, handle_proclist,   NULL, 0 },
    { "PROCKILL",   1, NULL,              handle_prockill, 0 },
    { "SHUTDOWN",   0, handle_shutdown,   NULL, 0 },
    { "REBOOT",     0, handle_reboot,     NULL, 0 },
    { "QUIT",       0, handle_quit,       NULL, 0 },
    { "RESTART",    0, handle_restart,    NULL, 0 },
    { "NETMAP",     1, NULL,              handle_netmap, 0 },
    { "NETUNMAP",   1, NULL,              handle_netunmap, 0 },
    { "FILECOPY",   1, NULL,              handle_filecopy, 0 },
    { "LAUNCH",     1, NULL,              handle_launch, 0 },
    { "WINLIST",    0, handle_winlist,    NULL, 0 },
    { "UICLICK",    1, NULL,              handle_uiclick, 0 },
    { "UIDRAG",     1, NULL,              handle_uidrag, 0 },
    { "UIKEY",      1, NULL,              handle_uikey, 0 },
    { "MONITOR",    1, NULL,              handle_monitor, 0 },
    { "DRVSNAPSHOT",1, NULL,              handle_drvsnapshot, 0 },
    { "AUTOLOGIN", 1, NULL,              handle_autologin, 1 },
    { "SERVICE",   1, NULL,              handle_service, 0 },
    { "SMARTINFO",  0, handle_smartinfo,  NULL, 0 },
    { "GAMEINDEX",  1, NULL,              handle_gameindex, 0 },
    { "GAMESYNC",   1, NULL,              handle_gamesync, 1 },
    { "ICONARRANGE",1, NULL,           handle_iconarrange, 1 },
    { "GAMERES",    1, NULL,               handle_gameres, 1 },
    { "DRVUPDATE",  1, NULL,             handle_drvupdate, 1 },
    { "HWPROFILE",  1, NULL,             handle_hwprofile, 0 },
    { "HWPUBLISH",  1, NULL,             handle_hwpublish, 0 },
    { "DISPLAYCFG", 1, NULL,             handle_displaycfg, 0 },
    { "AUDIOINFO",  0, handle_audioinfo,  NULL, 0 },
    { "SYSFIX",     1, NULL,             handle_sysfix, 0 },
    { "LICSTATUS",  1, NULL,            handle_licstatus, 0 },
    { "WPASAVE",    1, NULL,              handle_wpasave, 0 },
    { "WPALOAD",    1, NULL,              handle_wpaload, 1 },
    { "AUTOMAP",    1, NULL,             handle_automap, 0 },
    { "DOSSTAGE",   1, NULL,            handle_dosstage, 1 },
    { "PCIRESCAN",  1, NULL,           handle_pcirescan, 0 },
    { "PROMPT_PUSH",1, NULL,            handle_prompt_push, 0 },
    { "PROMPT_POP", 0, handle_prompt_pop, NULL, 0 },
    { "PROMPT_WAIT",1, NULL,            handle_prompt_wait, 0 },
    { "LOG_APPEND", 1, NULL,            handle_log_append, 0 },
    { "LOG_APPEND2",1, NULL,            handle_log_append2, 0 },
    { "LOG_READ",   1, NULL,            handle_log_read, 0 },
    { "LOG_WAIT",   1, NULL,            handle_log_wait, 0 },
    { "LOG_CLEAR",  0, handle_log_clear, NULL, 0 },
    { "PROXY_GET",  0, handle_proxy_get,  NULL, 0 },
    { "PROXY_SET",  1, NULL,            handle_proxy_set, 0 },
    { "STATUS_SET", 1, NULL,            handle_status_set, 0 },
    { "STATUS_GET", 0, handle_status_get, NULL, 0 },
    { "STATUS_WAIT",1, NULL,            handle_status_wait, 0 },
    { "AI_HELLO",   0, handle_ai_hello,   NULL, 0 },
    { "AI_RESTART", 0, handle_ai_restart, NULL, 0 },
    { "AI_ENABLE",  0, handle_ai_enable,  NULL, 0 },
    { "AI_DISABLE", 0, handle_ai_disable, NULL, 0 },
    { "MODEL_LOAD", 1, NULL,            handle_model_load, 0 },
    { "MODEL_UNLOAD",1, NULL,           handle_model_unload, 0 },
    { "MODEL_LIST", 0, handle_model_list, NULL, 0 },
    { "INFER_RUN",  1, NULL,            handle_infer_run, 0 },
    { "TENSOR",     1, NULL,            handle_tensor, 0 },
    { "AI_RAW",     1, NULL,            handle_ai_raw, 0 },
    { "AI_RAWP",    1, NULL,            handle_ai_rawp, 0 },
    { NULL,         0, NULL,              NULL, 0 }
};

/*
 * Chat text is passed VERBATIM: everything after exactly ONE separator space,
 * leading blanks included.
 *
 * Every other command's arguments are space-trimmed, and these four were too
 * - which destroyed text. The brain streams a reply as LOG_APPEND chunks split
 * wherever it flushes, so a chunk that began with the space between two words
 * lost it and the words were glued together on screen, and every indented
 * line of code or of a listing came out flush left. A prompt or a status line
 * is the user's or the brain's own text for the same reason.
 */
static int cmd_takes_raw_text(const char *name)
{
    static const char *const raw[] = {
        "LOG_APPEND", "LOG_APPEND2", "PROMPT_PUSH", "STATUS_SET"
    };
    int i;
    for (i = 0; i < (int)(sizeof(raw) / sizeof(raw[0])); i++)
        if (_stricmp(name, raw[i]) == 0)
            return 1;
    return 0;
}

/* The arguments of `cmd`, whose first word (the command name) is `name` and
 * ends at cmd[name_len]. NULL when there is no separator space at all. */
static const char *cmd_args_of(const char *cmd, int name_len, const char *name)
{
    if (cmd[name_len] != ' ')
        return NULL;
    if (cmd_takes_raw_text(name))
        return cmd + name_len + 1;          /* exactly one separator space */
    return str_skip_spaces(cmd + name_len + 1);
}

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

    /* Find args after first space (chat text verbatim - see above) */
    args = cmd_args_of(cmd, i, cmd_name);

    /* Look up command */
    for (entry = commands; entry->name; entry++) {
        if (_stricmp(cmd_name, entry->name) == 0) {
            /*
             * ONE choke point for the whole "do not reconfigure a modern PC"
             * rule. Put it here, not in each handler: a guard that has to be
             * remembered in fifteen places is one that will be missed in the
             * sixteenth, and the miss is silent - the box just gets skinned.
             *
             * The refusal is an ERROR with the reason in it, so the server
             * sees WHY rather than a command that appeared to succeed and
             * changed nothing. (SYSFIX and DISPLAYCFG are not flagged here:
             * each has a genuinely read-only mode worth keeping, so they carry
             * the same guard on their modifying branch instead.)
             */
            if (entry->reconfigures_host && !host_manages_this_box()) {
                char why[256];
                _snprintf(why, sizeof(why) - 1,
                          "%s refused: this is modern Windows and the retro agent "
                          "does not reconfigure it. Set HKLM\\%s\\%s (DWORD) to 1 "
                          "to manage this box anyway.",
                          entry->name, HOSTPOLICY_OVERRIDE_KEY,
                          HOSTPOLICY_OVERRIDE_VALUE);
                why[sizeof(why) - 1] = 0;
                log_msg(LOG_MAIN, "%s", why);
                send_error_response(sock, why);
                return;
            }
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
 * WIN9x POWER: the one route proven on Win98 hardware is the SHELL's.
 *
 * On .243 (Win98 SE, 2026-09-26) `rundll32.exe shell32.dll,SHExitWindowsEx 6`
 * rebooted the machine with the agent and retro_chat both running - Windows
 * asked each console app to close, both exited through their console
 * handlers, and the box went down. Every REBOOT through this agent's own path
 * failed the same way (2026-09-25 22:21, 09-26 10:02 and 22:21 box time):
 * "REBOOT: initiating", the agent gone, Windows still up.
 *
 * That path killed RETRO_CHAT.EXE / COMMAND.COM / CMD.EXE with
 * TerminateProcess first. On Win98 a Win32 console app's console lives in a
 * DOS VM (WINOA386.MOD, parented to the app - PROCLIST on .243), and killing
 * the app does not close that VM cleanly; a DOS VM that is still running is
 * exactly what Win98 refuses to shut down over, FORCE or not. It then kept the
 * agent deliberately alive (g_power_pending) on the theory that an exiting
 * requester cancels its own shutdown - a theory the working shell reboot
 * contradicts, since there the agent exited normally mid-shutdown.
 * (CONAGENT.EXE, removed from the kill list in 1.85.3, does not even appear
 * on this box: the console host is WINOA386.MOD.)
 *
 * So on 9x: start the shell's own ExitWindowsEx, kill nothing, and let
 * Windows close us like any other console app. SHExitWindowsEx takes the EWX
 * flags on its command line (1 shutdown, 2 reboot, 4 force, 8 power off).
 * REBOOT (6) is hardware-proven; SHUTDOWN (13) takes the same route and has
 * not been exercised on a box.
 */

/* How long a 9x agent waits after starting the shell's shutdown before
 * concluding it did not take. Windows closes us well inside this on a
 * shutdown that proceeds; the only cost of a generous bound is a late line. */
#define WIN9X_SHUTDOWN_WAIT_MS  90000

/* See handlers.h. NT only: keeps the console control handler from stopping
 * the agent while shutdown.exe is running. Never set on Win9x (above). */
volatile int g_power_pending = 0;

/* Win9x: one shell shutdown at a time; cleared again if it did not take. */
static volatile int g_9x_power_started = 0;

/* Is this a Win9x box? (Shutdown behaviour differs completely from NT.) */
static int is_win9x(void)
{
    OSVERSIONINFOA o;
    o.dwOSVersionInfoSize = sizeof(o);
    GetVersionExA(&o);
    return o.dwPlatformId != VER_PLATFORM_WIN32_NT;
}

/*
 * NT only: kill console/batch processes that can hold up the shutdown.
 * NEVER used on Win9x - see the block comment above.
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
                _stricmp(pe.szExeFile, "CMD.EXE") == 0) {
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

/* Win9x: if we are still here long after the shell was asked to shut
 * Windows down, it did not happen - say so, instead of looking like a
 * successful reboot. param = the command label (a string literal). */
static DWORD WINAPI win9x_power_watch(LPVOID param)
{
    Sleep(WIN9X_SHUTDOWN_WAIT_MS);
    log_msg(LOG_MAIN, "%s: STILL RUNNING %lu ms after asking the shell to shut "
            "Windows down - it did not take; look at the screen for a dialog "
            "(a program that would not close)",
            (const char *)param, (unsigned long)WIN9X_SHUTDOWN_WAIT_MS);
    log_flush();
    g_9x_power_started = 0;             /* allow another attempt */
    return 0;
}

static void do_system_power_9x(SOCKET sock, const char *label, UINT ewx_flags)
{
    char cmd[80];
    char msg[128];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE wt;
    DWORD tid;

    if (g_9x_power_started) {
        send_text_response(sock, "OK (a power operation is already in flight)");
        log_msg(LOG_MAIN, "%s: ignored - one is already in flight", label);
        return;
    }

    _snprintf(cmd, sizeof(cmd) - 1, "rundll32.exe shell32.dll,SHExitWindowsEx %u",
              ewx_flags);
    cmd[sizeof(cmd) - 1] = '\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    /* Suspended, so "OK" and the log line are out before anything starts
     * tearing the session down. */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) {
        _snprintf(msg, sizeof(msg) - 1, "ERR %s failed: could not start \"%s\" (error %lu)",
                  label, cmd, (unsigned long)GetLastError());
        msg[sizeof(msg) - 1] = '\0';
        log_msg(LOG_MAIN, "%s", msg);
        log_flush();
        send_text_response(sock, msg);
        return;
    }

    send_text_response(sock, "OK");
    log_msg(LOG_MAIN, "%s: initiating via \"%s\" (pid %lu); Windows will close "
            "this agent like any console app", label, cmd,
            (unsigned long)pi.dwProcessId);
    log_flush();

    g_9x_power_started = 1;
    wt = CreateThread(NULL, 0, win9x_power_watch, (LPVOID)label, 0, &tid);
    if (wt) CloseHandle(wt);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    /* Deliberately nothing else: no kills, no g_power_pending. */
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
 * NT system shutdown/reboot thread: shutdown.exe, with ExitWindowsEx as the
 * fallback. (Win9x never gets here - do_system_power_9x() above.)
 *
 * param = EWX_* flags cast to LPVOID.
 */
static DWORD WINAPI system_shutdown_thread(LPVOID param)
{
    UINT flags = (UINT)(UINT_PTR)param;
    OSVERSIONINFOA osvi;

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
    }

    return 0;
}

/*
 * Initiate system shutdown or reboot. Win9x: the shell's SHExitWindowsEx
 * (do_system_power_9x). NT: kill stray consoles, then shutdown.exe from a
 * helper thread, and stop the agent.
 */
static void do_system_power(SOCKET sock, const char *label, UINT ewx_flags)
{
    HANDLE th;

    if (is_win9x()) {
        do_system_power_9x(sock, label, ewx_flags);
        return;
    }

    if (g_power_pending) {
        /* Now that the agent survives a Win9x shutdown attempt, a repeated
         * REBOOT would stack another 60-second pumping thread and re-kill the
         * consoles each time. One at a time. */
        send_text_response(sock, "OK (a power operation is already in flight)");
        log_msg(LOG_MAIN, "%s: ignored - one is already in flight", label);
        return;
    }

    /* Create the worker FIRST, suspended, and only then say OK. Until
     * 2026-09-24 this sent "OK" up front and then called CreateThread with a
     * NULL lpThreadId - which Win95/98 reject with ERROR_INVALID_PARAMETER
     * (87). So REBOOT and SHUTDOWN never did anything on a Win9x box, while
     * answering OK every time; the only trace was one log line. Found on .243
     * when safe-reboot.py reported "rebooting: OK" and the uptime kept
     * counting. spawn_helper() had the same bug fixed long ago. */
    {
        DWORD tid;
        th = CreateThread(NULL, 0, system_shutdown_thread,
                          (LPVOID)(UINT_PTR)ewx_flags, CREATE_SUSPENDED, &tid);
    }
    if (!th) {
        DWORD err = GetLastError();
        char msg[96];
        _snprintf(msg, sizeof(msg) - 1,
                  "ERR %s failed: could not start the shutdown thread (error %lu)",
                  label, (unsigned long)err);
        msg[sizeof(msg) - 1] = '\0';
        log_msg(LOG_MAIN, "%s", msg);
        log_flush();
        send_text_response(sock, msg);
        return;
    }

    send_text_response(sock, "OK");
    log_msg(LOG_MAIN, "%s: initiating", label);
    /* Commit the log now: everything after this point races the machine
     * going down, and this line is what tells us a reboot was even asked
     * for if it does not come back. */
    log_flush();

    kill_console_processes();
    Sleep(200);

    /* Announce the pending power operation BEFORE anything can start tearing
     * the session down, so the console control handler never mistakes a
     * shutdown we asked for for a reason to stop the agent. */
    g_power_pending = 1;

    ResumeThread(th);
    CloseHandle(th);                    /* fire-and-forget: don't leak it */

    /* NT hands off to shutdown.exe, which survives us exiting. */
    Sleep(500);
    g_running = 0;
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
    if (GetVersion() & 0x80000000) {
        /* Win9x START.EXE takes NO window title. `start "" "x"` is cmd.exe
         * syntax: START.EXE reads the "" as the program, fails, and the agent
         * never comes back - which is what RESTART left on .243 (Win98 SE,
         * 2026-09-24): networking up, 9898 refused, nobody to relaunch it.
         * Use the 8.3 path, which needs no quotes, exactly as the auto-update
         * batch (hardware-proven on the Deskpro) does. */
        char shortp[MAX_PATH];
        if (!GetShortPathNameA(exe, shortp, sizeof(shortp)))
            safe_strncpy(shortp, exe, sizeof(shortp));
        fprintf(f, "start %s\r\n", shortp);
    } else {
        fprintf(f, "start \"\" \"%s\"\r\n", exe);
    }
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
