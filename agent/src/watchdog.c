/*
 * watchdog.c - recover the agent when a command is stuck behind a hung
 * fullscreen game.
 *
 * A fullscreen Glide game (Quake 2 / Quake 3 / CS 1.6) that crashes or hangs
 * during GL init locks the Voodoo display. The agent's display-touching
 * commands (SCREENSHOT / GDI / UI automation) then block, and because a
 * controller talks to the agent one connection at a time, the whole agent
 * looks unresponsive ("crashed game blocked agent communication").
 *
 * This watchdog runs on its own thread, independent of the command threads. If
 * a command has been in a handler longer than WATCHDOG_STUCK_MS *and* a known
 * fullscreen game process is running, it force-kills the game and restores the
 * desktop display mode. That frees the Glide fullscreen lock, so the stuck GDI
 * call returns and the agent becomes responsive again on its own.
 *
 * The game-running guard means a legitimately long command (a big DOWNLOAD, a
 * long EXECW install) never triggers a false recovery.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

#include "handlers.h"
#include "util.h"
#include "log.h"

/* Set by the command loop (main.c) around each handle_command call. */
volatile LONG  g_cmd_inflight = 0;   /* # commands currently inside a handler */
volatile DWORD g_cmd_start    = 0;   /* GetTickCount() when the last one began */

#define WATCHDOG_POLL_MS    8000     /* check cadence */
#define WATCHDOG_STUCK_MS   75000    /* a handler stuck this long == wedged */

/* Fullscreen games whose crash/hang can lock the display. */
static const char *const GAME_EXES[] = {
    "quake3.exe", "quake2.exe", "hl.exe", "glquake.exe", "quake.exe",
    "3dmark2001se.exe", "3dmark2000.exe", NULL
};

static int game_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    int found = 0, i;

    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            for (i = 0; GAME_EXES[i]; i++) {
                if (_stricmp(pe.szExeFile, GAME_EXES[i]) == 0) {
                    found = 1;
                    break;
                }
            }
        } while (!found && Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void run_hidden(const char *cmdline, DWORD wait_ms)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[256];

    safe_strncpy(buf, cmdline, sizeof(buf));   /* CreateProcess may modify it */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (CreateProcessA(NULL, buf, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (wait_ms)
            WaitForSingleObject(pi.hProcess, wait_ms);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static void recover_from_hung_game(void)
{
    run_hidden("cmd /c taskkill /f /im quake3.exe /im quake2.exe /im hl.exe "
               "/im glquake.exe /im quake.exe 2>nul", 8000);
    /* Return the primary display to its registry (desktop) mode. This releases
     * a stale Glide fullscreen lock so a blocked GDI/BitBlt can complete. */
    ChangeDisplaySettingsA(NULL, 0);
    log_msg(LOG_MAIN,
            "watchdog: killed hung fullscreen game + restored desktop display mode");
}

DWORD WINAPI watchdog_thread(LPVOID param)
{
    (void)param;
    for (;;) {
        Sleep(WATCHDOG_POLL_MS);
        if (!g_running)
            return 0;
        if (g_cmd_inflight > 0) {
            DWORD elapsed = GetTickCount() - g_cmd_start;
            if (elapsed > WATCHDOG_STUCK_MS && game_running()) {
                log_msg(LOG_MAIN,
                        "watchdog: command wedged %lu ms with a game running - recovering",
                        (unsigned long)elapsed);
                recover_from_hung_game();
                Sleep(6000);   /* let the freed op unwind; avoid immediate re-fire */
            }
        }
    }
}
