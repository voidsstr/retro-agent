/*
 * bgwork.c - background helper threads: priority and idle waiting.
 * The model and the reasons are in bgwork.h.
 */
#include <windows.h>

#include "bgwork.h"
#include "handlers.h"   /* g_running */
#include "log.h"

void thread_background(void)
{
    HANDLE me = GetCurrentThread();

    if (GetThreadPriority(me) == THREAD_PRIORITY_IDLE)
        return;
    /* A failure here is worth a line: the helper would run above the game,
     * which is the very thing this exists to stop, and nothing else would
     * say so. */
    if (!SetThreadPriority(me, THREAD_PRIORITY_IDLE))
        log_msg(LOG_MAIN, "could not lower a background thread (%lu) - it "
                "runs at the agent's own priority",
                (unsigned long)GetLastError());
}

int agent_nap(DWORD ms)
{
    while (ms > 0 && g_running) {
        DWORD slice = ms > BG_NAP_SLICE_MS ? BG_NAP_SLICE_MS : ms;
        Sleep(slice);
        ms -= slice;
    }
    return g_running;
}
