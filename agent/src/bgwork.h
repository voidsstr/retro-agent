/*
 * bgwork.h - how the agent's BACKGROUND helper threads behave.
 *
 * THE PRIORITY MODEL (agent 1.85.0)
 * ---------------------------------
 * The process runs in HIGH_PRIORITY_CLASS (main.c), so every thread that
 * serves a command - the accept/multiplex loop, the per-connection threads on
 * NT, the discovery responder, the watchdog, the log flusher - sits at base
 * priority 13, above a normal-class game (8). That is deliberate and it is
 * unchanged: a fullscreen benchmark at normal priority starved an agent that
 * ran at normal priority for 30-60 s at a time, which is exactly when the box
 * has to stay reachable.
 *
 * What was wrong is that EVERY OTHER thread inherited base 13 as well: the
 * game-index disk walk, the share log mirror, the theme/wallpaper pass, the
 * library sync, the self-update, the hardware publish and the DOS staging all
 * ran above Explorer and above the game the box exists to play. On a Pentium
 * 166 that is a visible stall at every boot.
 *
 * So each background helper calls thread_background() first. Inside a HIGH
 * class process the only thread level BELOW a normal-class game is
 * THREAD_PRIORITY_IDLE (base 1): LOWEST is 13-2 = 11 and BELOW_NORMAL is 12,
 * both still above the game. IDLE means "run when nothing in the foreground
 * wants the CPU" - on an idle box that is full speed, and during a game it is
 * "wait until the game yields".
 *
 * Starving a thread that holds a lock the command path needs is the one real
 * risk of IDLE, so the locks are handled explicitly:
 *   - the log lock is held across disk I/O, so log.c lifts an IDLE caller to
 *     normal for the milliseconds it holds it (log_lift());
 *   - GAMEINDEX SCAN waits on the scanner, so the handler lifts the scanner
 *     thread before it waits (gameindex.c);
 *   - the remaining shared locks (g_gs_lock, g_gi_lock) guard a few copied
 *     bytes with no I/O inside, and both Windows families already relieve a
 *     starved lock holder (the NT balance-set manager boosts a thread that has
 *     been ready ~4 s; 9x boosts a critical-section owner) - MSDN "Priority
 *     Inversion".
 *
 * NEVER call thread_background() on a thread that serves commands.
 * tests/python/test_agent_priority_model.py pins both directions.
 */
#ifndef BGWORK_H
#define BGWORK_H

#include <windows.h>

/* Drop the calling thread to THREAD_PRIORITY_IDLE. First statement of every
 * background helper thread (and safe to repeat - GAMEINDEX SCAN may have
 * lifted the scanner for one pass). */
void thread_background(void);

/* Sleep for up to `ms`, returning early once the agent is stopping.
 *
 * Replaces the 1-second sleep-poll loops the helpers had: those woke every
 * helper once a second for the life of the agent just to re-read g_running.
 * The slice can be long because nothing waits for these threads - agent_run()
 * ends in ExitProcess(), which ends every helper wherever it is - so the slice
 * only bounds how long a helper could run on after a stop request, and the
 * answer to that is already "it cannot: the process is gone".
 *
 * Returns g_running (non-zero = keep going). */
#define BG_NAP_SLICE_MS  60000
int agent_nap(DWORD ms);

#endif /* BGWORK_H */
