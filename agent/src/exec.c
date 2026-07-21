/*
 * exec.c - Remote command execution via cmd.exe /c
 * Uses CreateProcessA with redirected stdout/stderr pipes.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define EXEC_TIMEOUT_MS  60000  /* default EXEC timeout (60s) */
#define EXEC_MAX_MS     900000  /* EXECW clamp (15 min) - fast debug turnaround */
#define EXEC_POLL_MS       200  /* poll interval for pipe/process check */
#define EXEC_BUF_SIZE    65536

/* Shared exec-with-capture core. timeout_ms bounds the run; on timeout the child
 * tree is killed. If mark_timeout, a marker is appended so the caller can tell a
 * timeout from a clean exit. Used by EXEC (60s) and EXECW (<seconds>). */
static void do_exec(SOCKET sock, const char *args, DWORD timeout_ms, int mark_timeout)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[MAX_COMMAND_LEN];
    char *output = NULL;
    DWORD output_len = 0;
    DWORD output_cap = EXEC_BUF_SIZE;
    DWORD bytes_read, exit_code;
    BOOL success;
    int did_timeout = 0;

    if (!args || !args[0]) {
        send_error_response(sock, "EXEC requires a command");
        return;
    }

    /* Build shell command - use command.com on Win9x, cmd.exe on NT */
    {
        OSVERSIONINFOA osvi;
        const char *shell;
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        GetVersionExA(&osvi);
        shell = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT)
                ? "cmd.exe /c " : "command.com /c ";
        _snprintf(cmdline, sizeof(cmdline), "%s%s", shell, args);
    }
    log_msg(LOG_EXEC, "EXEC: cmdline=\"%s\"", cmdline);

    /* Create pipe for stdout+stderr capture */
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        send_error_response(sock, "Failed to create pipe");
        return;
    }

    /* Don't let child inherit the read end. Resolved dynamically —
     * SetHandleInformation is Win2000+ only, see util.c. */
    set_handle_noinherit(hReadPipe);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    si.wShowWindow = SW_HIDE;

    memset(&pi, 0, sizeof(pi));

    /* CREATE_NO_WINDOW is invalid for 16-bit command.com on Win9x.
     * Use 0 and rely on SW_HIDE via STARTUPINFO instead. */
    {
        OSVERSIONINFOA osvi;
        DWORD flags;
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        GetVersionExA(&osvi);
        flags = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT)
                ? CREATE_NO_WINDOW : 0;
        success = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                                 flags, NULL, "C:\\", &si, &pi);
    }

    /* Close write end in parent so ReadFile will return when child exits */
    CloseHandle(hWritePipe);
    hWritePipe = NULL;

    if (!success) {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_EXEC, "CreateProcessA failed: %lu", (unsigned long)gle);
        _snprintf(err, sizeof(err), "CreateProcess failed: %lu", gle);
        CloseHandle(hReadPipe);
        send_error_response(sock, err);
        return;
    }
    log_msg(LOG_EXEC, "CreateProcessA OK, PID=%lu", (unsigned long)pi.dwProcessId);

    /* Read all output using non-blocking PeekNamedPipe + short waits.
     * This prevents a hanging child process from blocking the entire
     * agent's multiplex loop indefinitely. Each iteration blocks for
     * at most EXEC_POLL_MS (200ms). */
    output = (char *)HeapAlloc(GetProcessHeap(), 0, output_cap);
    if (!output) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);
        send_error_response(sock, "Out of memory");
        return;
    }

    {
        DWORD start_tick = GetTickCount();
        DWORD avail;
        int timed_out = 0;

        while (1) {
            /* Check timeout */
            if (GetTickCount() - start_tick >= timeout_ms) {
                log_msg(LOG_EXEC, "EXEC timeout (%lums), killing PID %lu",
                        (unsigned long)timeout_ms,
                        (unsigned long)pi.dwProcessId);
                /* kill the whole tree (child may have spawned the real app) */
                {
                    char kc[64];
                    _snprintf(kc, sizeof(kc), "taskkill /f /t /pid %lu",
                              (unsigned long)pi.dwProcessId);
                    /* best-effort tree kill via a detached helper (NT only) */
                    { STARTUPINFOA ksi; PROCESS_INFORMATION kpi;
                      memset(&ksi,0,sizeof(ksi)); ksi.cb=sizeof(ksi);
                      ksi.dwFlags=STARTF_USESHOWWINDOW; ksi.wShowWindow=SW_HIDE;
                      memset(&kpi,0,sizeof(kpi));
                      if (CreateProcessA(NULL, kc, NULL, NULL, FALSE,
                                         CREATE_NO_WINDOW, NULL, NULL, &ksi, &kpi)) {
                          CloseHandle(kpi.hProcess); CloseHandle(kpi.hThread);
                      }
                    }
                }
                TerminateProcess(pi.hProcess, 1);
                timed_out = 1;
                did_timeout = 1;
                break;
            }

            /* Check if data is available without blocking */
            avail = 0;
            if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL)
                && avail > 0) {
                /* Data available - ReadFile won't block */
                if (output_len + 4096 > output_cap) {
                    output_cap *= 2;
                    output = (char *)HeapReAlloc(GetProcessHeap(), 0,
                                                 output, output_cap);
                    if (!output) break;
                }

                if (!ReadFile(hReadPipe, output + output_len, 4096,
                              &bytes_read, NULL) || bytes_read == 0)
                    break;

                output_len += bytes_read;
                continue;  /* Check for more data immediately */
            }

            /* No data available - check if process exited */
            if (WaitForSingleObject(pi.hProcess, EXEC_POLL_MS)
                == WAIT_OBJECT_0) {
                /* Process exited - drain any remaining pipe data */
                while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL)
                       && avail > 0) {
                    if (output_len + 4096 > output_cap) {
                        output_cap *= 2;
                        output = (char *)HeapReAlloc(GetProcessHeap(), 0,
                                                     output, output_cap);
                        if (!output) break;
                    }
                    if (!ReadFile(hReadPipe, output + output_len, 4096,
                                  &bytes_read, NULL) || bytes_read == 0)
                        break;
                    output_len += bytes_read;
                }
                break;
            }
            /* Process still running, no data yet - loop back and re-check */
        }

        if (timed_out) {
            WaitForSingleObject(pi.hProcess, 2000);  /* Let it die */
        }
    }

    GetExitCodeProcess(pi.hProcess, &exit_code);

    log_msg(LOG_EXEC, "Process %lu exited, code=%lu, output=%lu bytes",
            (unsigned long)pi.dwProcessId, (unsigned long)exit_code,
            (unsigned long)output_len);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    if (output) {
        /* Optional timeout marker (EXECW) so a killed run is distinguishable. */
        if (mark_timeout && did_timeout) {
            const char *mk = "\n[EXECW: timed out, process tree killed]\n";
            DWORD mklen = (DWORD)strlen(mk);
            if (output_len + mklen + 1 > output_cap) {
                output = (char *)HeapReAlloc(GetProcessHeap(), 0,
                                             output, output_len + mklen + 1);
            }
            if (output) { memcpy(output + output_len, mk, mklen); output_len += mklen; }
        }
        /* Null-terminate for safety */
        if (output && output_len + 1 > output_cap) {
            output = (char *)HeapReAlloc(GetProcessHeap(), 0,
                                         output, output_len + 1);
        }
        if (output) {
            output[output_len] = '\0';
            send_text_response(sock, output);
        }
        if (output) HeapFree(GetProcessHeap(), 0, output);
    } else {
        send_text_response(sock, "(no output)");
    }
}

/* EXEC - default 60s timeout (unchanged behavior). */
void handle_exec(SOCKET sock, const char *args)
{
    do_exec(sock, args, EXEC_TIMEOUT_MS, 0);
}

/* EXECW <seconds> <command> - configurable long timeout + capture, for fast
 * debug turnaround (run a benchmark/game/timedemo and get its output in ONE
 * round-trip instead of LAUNCH + sleep + reconnect + read). */
void handle_execw(SOCKET sock, const char *args)
{
    long secs;
    char *end;
    const char *cmd;

    if (!args || !args[0]) {
        send_error_response(sock, "EXECW requires: <seconds> <command>");
        return;
    }
    secs = strtol(args, &end, 10);
    if (end == args || secs < 1) {
        send_error_response(sock, "EXECW: first arg must be timeout seconds");
        return;
    }
    if (secs * 1000L > EXEC_MAX_MS) secs = EXEC_MAX_MS / 1000;
    cmd = end;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) {
        send_error_response(sock, "EXECW: missing command after seconds");
        return;
    }
    do_exec(sock, cmd, (DWORD)secs * 1000, 1);
}

/*
 * LAUNCH - Start a process visible and non-blocking.
 * Unlike EXEC (hidden, blocking, pipe capture), LAUNCH is designed for
 * GUI installers that need to be visible for screenshot-based UI automation.
 * Returns immediately with the PID.
 */
void handle_launch(SOCKET sock, const char *args)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdline[MAX_COMMAND_LEN];
    BOOL success;
    json_t j;

    if (!args || !args[0]) {
        send_error_response(sock, "LAUNCH requires a command");
        return;
    }

    /* Build shell command - use command.com on Win9x, cmd.exe on NT */
    {
        OSVERSIONINFOA osvi;
        const char *shell;
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        GetVersionExA(&osvi);
        shell = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT)
                ? "cmd.exe /c " : "command.com /c ";
        _snprintf(cmdline, sizeof(cmdline), "%s%s", shell, args);
    }
    log_msg(LOG_EXEC, "LAUNCH: cmdline=\"%s\"", cmdline);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    memset(&pi, 0, sizeof(pi));

    success = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                             0, NULL, "C:\\", &si, &pi);

    if (!success) {
        char err[256];
        DWORD gle = GetLastError();
        log_msg(LOG_EXEC, "LAUNCH CreateProcessA failed: %lu", (unsigned long)gle);
        _snprintf(err, sizeof(err), "CreateProcess failed: %lu", gle);
        send_error_response(sock, err);
        return;
    }

    log_msg(LOG_EXEC, "LAUNCH OK, PID=%lu", (unsigned long)pi.dwProcessId);

    /* Close handles - we don't wait for the process */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* Return JSON with PID and command */
    json_init(&j);
    json_object_start(&j);
    json_kv_uint(&j, "pid", pi.dwProcessId);
    json_kv_str(&j, "command", args);
    json_object_end(&j);

    {
        char *result = json_finish(&j);
        if (result) {
            send_text_response(sock, result);
            HeapFree(GetProcessHeap(), 0, result);
        } else {
            send_error_response(sock, "Out of memory");
        }
    }
}
