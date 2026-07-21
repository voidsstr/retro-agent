/*
 * monitor.c - MONITOR: real-time streaming status for benchmark/diagnostic
 * supervision.
 *
 * MONITOR <interval_ms> <max_ticks> [procname]
 *
 * Streams one text frame per tick (thread-per-client mode makes this safe):
 *   t=<ms> fg="<foreground title>" nwin=<visible windows> proc=<0|1|-> \
 *   mode=<WxH> new="<newest visible title>"
 * Ends with a final frame "END ticks=<n>" (or when the client disconnects —
 * any send failure stops the stream).
 *
 * Purpose: replace 45s reconnect-polling during fullscreen D3D/Glide runs
 * with a 250ms-1s push stream over one persistent connection, so the
 * controller sees the exact moment a test starts, a process dies, or a
 * modal appears. Cheap: no GDI capture, just window/process enumeration.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"

#define LOG_MON "MONITOR"

static int g_mon_count;
static char g_mon_last_title[256];

static BOOL CALLBACK mon_enum_cb(HWND hwnd, LPARAM lParam)
{
    char title[256];
    (void)lParam;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    title[0] = '\0';
    GetWindowTextA(hwnd, title, sizeof(title));
    if (title[0] == '\0')
        return TRUE;
    /* remember the first (z-topmost) titled window as "newest" */
    if (g_mon_last_title[0] == '\0')
        strncpy(g_mon_last_title, title, sizeof(g_mon_last_title) - 1);
    g_mon_count++;
    return TRUE;
}

static int mon_proc_alive(const char *name)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    int found = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                found = 1;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

void handle_monitor(SOCKET sock, const char *args)
{
    int interval_ms = 1000;
    int max_ticks = 60;
    char procname[64];
    int tick;
    char line[512];
    char fg_title[256];

    procname[0] = '\0';
    if (args && args[0]) {
        char argbuf[160];
        char *p, *tok;
        strncpy(argbuf, args, sizeof(argbuf) - 1);
        argbuf[sizeof(argbuf) - 1] = '\0';
        p = argbuf;
        tok = strtok(p, " ");
        if (tok) interval_ms = atoi(tok);
        tok = strtok(NULL, " ");
        if (tok) max_ticks = atoi(tok);
        tok = strtok(NULL, " ");
        if (tok) {
            strncpy(procname, tok, sizeof(procname) - 1);
            procname[sizeof(procname) - 1] = '\0';
        }
    }
    if (interval_ms < 100) interval_ms = 100;
    if (interval_ms > 60000) interval_ms = 60000;
    if (max_ticks < 1) max_ticks = 1;
    if (max_ticks > 7200) max_ticks = 7200;

    log_msg(LOG_MON, "MONITOR start: every %dms x %d, proc=%s",
            interval_ms, max_ticks, procname[0] ? procname : "(none)");

    for (tick = 0; tick < max_ticks; tick++) {
        HWND fg;
        HDC dc;
        int sw = 0, sh = 0;
        int alive = -2;

        g_mon_count = 0;
        g_mon_last_title[0] = '\0';
        EnumWindows(mon_enum_cb, 0);

        fg = GetForegroundWindow();
        fg_title[0] = '\0';
        if (fg)
            GetWindowTextA(fg, fg_title, sizeof(fg_title));

        dc = GetDC(NULL);
        if (dc) {
            sw = GetDeviceCaps(dc, HORZRES);
            sh = GetDeviceCaps(dc, VERTRES);
            ReleaseDC(NULL, dc);
        }

        if (procname[0])
            alive = mon_proc_alive(procname);

        _snprintf(line, sizeof(line),
                  "t=%lu fg=\"%s\" nwin=%d proc=%d mode=%dx%d top=\"%s\"",
                  (unsigned long)GetTickCount(), fg_title, g_mon_count,
                  alive, sw, sh, g_mon_last_title);
        line[sizeof(line) - 1] = '\0';

        if (send_text_response(sock, line) != 0) {
            log_msg(LOG_MON, "MONITOR: client gone at tick %d", tick);
            return;
        }

        if (tick + 1 < max_ticks)
            Sleep((DWORD)interval_ms);
    }

    _snprintf(line, sizeof(line), "END ticks=%d", max_ticks);
    send_text_response(sock, line);
    log_msg(LOG_MON, "MONITOR done: %d ticks", max_ticks);
}
