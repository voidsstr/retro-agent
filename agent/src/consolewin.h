/*
 * consolewin.h - the agent minimizes its own console window at startup.
 * The why and the decision table are in agent/shared/consolemin.h.
 */
#ifndef CONSOLEWIN_H
#define CONSOLEWIN_H

/*
 * Call once, from the main thread, in console mode, BEFORE any helper thread
 * or child process can exist (agent_run, right after SetConsoleTitle). It
 * gathers the facts synchronously - the "is this console shared with a shell"
 * count must not see a child the agent itself spawned - and hands the one call
 * that can block (ShowWindow sends messages to the console host's thread) to a
 * short-lived helper thread, so a cosmetic step can never stall startup.
 */
void consolewin_startup(int service_mode);

#endif /* CONSOLEWIN_H */
