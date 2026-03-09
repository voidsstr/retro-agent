/*
 * process.c - Process listing and management
 * Uses CreateToolhelp32Snapshot, Process32First/Next, TerminateProcess
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void handle_proclist(SOCKET sock)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    json_t j;
    char *result;

    json_init(&j);
    json_array_start(&j);

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                json_object_start(&j);
                json_kv_uint(&j, "pid", pe.th32ProcessID);
                json_kv_str(&j, "name", pe.szExeFile);
                json_kv_uint(&j, "threads", pe.cntThreads);
                json_kv_uint(&j, "parent_pid", pe.th32ParentProcessID);
                json_object_end(&j);
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }

    json_array_end(&j);

    result = json_finish(&j);
    send_text_response(sock, result);
    json_free(&j);
}

void handle_prockill(SOCKET sock, const char *args)
{
    DWORD pid;
    HANDLE hProc;

    if (!args || !args[0]) {
        send_error_response(sock, "PROCKILL requires a PID");
        return;
    }

    pid = (DWORD)strtoul(args, NULL, 10);
    if (pid == 0) {
        send_error_response(sock, "Invalid PID");
        return;
    }

    hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) {
        char err[128];
        _snprintf(err, sizeof(err), "Cannot open process %lu: error %lu",
                  (unsigned long)pid, GetLastError());
        send_error_response(sock, err);
        return;
    }

    if (TerminateProcess(hProc, 1)) {
        send_text_response(sock, "OK");
    } else {
        char err[128];
        _snprintf(err, sizeof(err), "TerminateProcess failed: %lu",
                  GetLastError());
        send_error_response(sock, err);
    }

    CloseHandle(hProc);
}
