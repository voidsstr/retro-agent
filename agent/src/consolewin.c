/*
 * consolewin.c - minimize the agent's own console window at startup.
 *
 * Why, and every case it deliberately leaves alone: agent/shared/consolemin.h.
 * In short: the console opened in the middle of the desktop at every boot,
 * covered the icon bay on .243 and the top-left icons on .124/.195/.110, and
 * people closed it by accident (CTRL_CLOSE, .195, 2026-09-26: 35 minutes with
 * no agent).
 *
 * NT only. GetConsoleWindow does not exist on Win9x (resolved through
 * ntdyn.c, never imported), and there the launchers start the agent minimized
 * instead: START.EXE /m in AGENTRUN.BAT and in RESTART's batch.
 */
#include <windows.h>

#include "consolewin.h"
#include "../shared/consolemin.h"
#include "hostpolicy.h"
#include "ntdyn.h"
#include "log.h"

/* consolemin.h is Win32-free (the native test compiles it on Linux), so it
 * carries SW_SHOWMINNOACTIVE as a bare number. Pin the two together. */
typedef char cm_show_cmd_is_showminnoactive[(CM_SHOW_CMD == SW_SHOWMINNOACTIVE) ? 1 : -1];

#define LOG_CONSOLE "CONSOLE"

/* HKLM\Software\RetroAgent\ConsoleVisible: REG_DWORD, nonzero = opt out. */
static int optout_set(void)
{
    HKEY  k;
    DWORD val = 0, sz = sizeof(val), type = 0;
    int   on = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, CM_OPTOUT_KEY, 0,
                      KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(k, CM_OPTOUT_VALUE, NULL, &type,
                         (LPBYTE)&val, &sz) == ERROR_SUCCESS &&
        type == REG_DWORD && val != 0)
        on = 1;
    RegCloseKey(k);
    return on;
}

/* The one call here that can block: ShowWindow on a window another process
 * owns (csrss on XP, conhost on 7) SENDS it messages. So it runs on its own
 * short-lived thread - a cosmetic step must never be able to stall the agent's
 * startup - and it READS THE RESULT BACK rather than trusting the call: a log
 * line saying "minimized" is not evidence that it is. */
static DWORD WINAPI minimize_thread(LPVOID param)
{
    HWND hwnd = (HWND)param;

    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    if (IsIconic(hwnd))
        log_msg(LOG_CONSOLE, "console window minimized to the taskbar "
                "(IsIconic=1) - it no longer covers the desktop icons; click its "
                "taskbar button to see it, or set HKLM\\%s\\%s=1 to keep it open",
                CM_OPTOUT_KEY, CM_OPTOUT_VALUE);
    else
        log_msg(LOG_CONSOLE, "console window minimize DID NOT TAKE (IsIconic=0) - "
                "it is still on the desktop, over the icons");
    return 0;
}

void consolewin_startup(int service_mode)
{
    cm_facts_t f;
    enum cm_plan plan;
    HWND hwnd = NULL;
    char cls[64];
    DWORD pids[4];
    DWORD tid;
    HANDLE h;

    memset(&f, 0, sizeof(f));
    cls[0] = '\0';

    f.service_mode = service_mode;
    if (!service_mode) {
        f.optout_set   = optout_set();
        f.host_managed = host_manages_this_box();
        f.have_api     = ntdyn_console_window_available();
        if (f.have_api)
            hwnd = ntdyn_GetConsoleWindow();
        f.have_window = hwnd != NULL;
        if (hwnd) {
            /* Neither of these sends a message: both read the window's own
             * state, so they are safe on the calling thread. */
            if (!GetClassNameA(hwnd, cls, sizeof(cls)))
                cls[0] = '\0';
            f.iconic = IsIconic(hwnd) ? 1 : 0;
        }
        f.window_class = cls;
        /* Read NOW, from the main thread, before any helper can have started
         * a child onto this console - a child of ours would look like the
         * shell that started us. */
        f.attached = ntdyn_GetConsoleProcessList(pids, 4);
    }

    plan = cm_decide(&f);
    if (plan == CM_LEAVE_SERVICE)
        return;                       /* nothing to say: there is no console */

    log_msg(LOG_CONSOLE, "console window%s%s: %s",
            cls[0] ? " class=" : "", cls, cm_plan_text(plan));
    if (plan != CM_MINIMIZE)
        return;

    /* &tid, never NULL: Win9x rejects a NULL lpThreadId (main.c spawn_helper).
     * This path is NT-only, but the rule is kept everywhere so nobody copies
     * the one exception. */
    h = CreateThread(NULL, 0, minimize_thread, (LPVOID)hwnd, 0, &tid);
    if (!h) {
        log_msg(LOG_CONSOLE, "console minimize thread FAILED to start: %lu - "
                "the console stays on the desktop", (unsigned long)GetLastError());
        return;
    }
    CloseHandle(h);
}
