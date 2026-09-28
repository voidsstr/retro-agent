/*
 * consolemin.h - should the agent minimize its OWN console window at startup?
 *
 * The decision half of agent/src/consolewin.c, Win32-free so
 * tests/native/test_consolemin.c compiles the exact code the agent runs.
 *
 * WHY (2026-09-28, agent 1.89.1): the agent is a console program, so every
 * start opens a ~670x340 console window in the middle of the desktop - and the
 * desktop is where the fleet keeps its game icons. WINLIST after a boot:
 *
 *   .243 (Win98, 1024x768)  the console covered the WHOLE icon bay (22,22-674,381)
 *   .124 / .195 / .110      it covered the top-left icons Auto Arrange packs there
 *
 * Worse than hiding icons, people click on it. Closing it is CTRL_CLOSE_EVENT
 * - "console control event 2" in .195's agent.log, twice on 2026-09-26, the
 * first of which left the box with no agent for 35 minutes - and a click into
 * the window can start a QuickEdit selection, which holds up any console
 * write the agent makes until the selection ends.
 *
 * So at startup the agent minimizes its own console: SW_SHOWMINNOACTIVE, which
 * keeps the taskbar button (the operator must still be able to FIND it - SW_HIDE
 * would make a running agent look like a dead one) and does not activate
 * anything (SW_MINIMIZE and SW_SHOWMINIMIZED both hand focus to another window,
 * which at boot can be a game or a setup dialog).
 *
 * WHERE EACH HALF LIVES
 *   NT (XP/7): GetConsoleWindow, resolved through ntdyn.c - it is Windows 2000+
 *              and a static import would make the EXE unloadable on Win98.
 *   Win9x:     there is no GetConsoleWindow, and the console window (class
 *              "tty", owned by WinOldAp) does not carry the SetConsoleTitle text
 *              - .243's reads "RETRO_~2" - so it cannot be found reliably from
 *              inside. The LAUNCHERS start it minimized instead: START.EXE /m in
 *              scripts/dosgames/AGENTRUN.BAT (the Run key) and in RESTART's
 *              batch (handlers.c).
 *
 * Opt-out: HKLM\Software\RetroAgent\ConsoleVisible (DWORD) nonzero = leave the
 * window exactly as it was launched. (NT only - COMMAND.COM cannot read the
 * registry, so on Win9x the /m lives in the batch.)
 */
#ifndef RETRO_CONSOLEMIN_H
#define RETRO_CONSOLEMIN_H

#include <string.h>

#define CM_OPTOUT_KEY    "Software\\RetroAgent"
#define CM_OPTOUT_VALUE  "ConsoleVisible"

/* ShowWindow's nCmdShow. SW_SHOWMINNOACTIVE = 7 in winuser.h; consolewin.c
 * pins the two together at compile time. NOT 0 (SW_HIDE: no taskbar button),
 * NOT 6 (SW_MINIMIZE) or 2 (SW_SHOWMINIMIZED): both activate another window. */
#define CM_SHOW_CMD      7

/* The class of the window GetConsoleWindow returns when the console is hosted
 * by Windows Terminal. Minimizing it minimizes the TERMINAL - with every other
 * tab the operator has open in it - so it is never touched. */
#define CM_PSEUDO_CLASS  "PseudoConsoleWindow"

enum cm_plan {
    CM_MINIMIZE = 0,      /* minimize it (SW_SHOWMINNOACTIVE) */
    CM_LEAVE_SERVICE,     /* NT service mode: there is no console */
    CM_LEAVE_OPTOUT,      /* ConsoleVisible is set */
    CM_LEAVE_UNMANAGED,   /* modern Windows, host policy hands-off */
    CM_LEAVE_NO_API,      /* no GetConsoleWindow: Win9x - the launcher does it */
    CM_LEAVE_NO_WINDOW,   /* GetConsoleWindow returned NULL */
    CM_LEAVE_PSEUDO,      /* Windows Terminal's stand-in window */
    CM_LEAVE_SHARED,      /* another process shares the console: a shell ran us */
    CM_LEAVE_ALREADY      /* already minimized (start /m, a shortcut, a person) */
};

typedef struct {
    int service_mode;          /* running under the SCM */
    int host_managed;          /* host_manages_this_box() */
    int optout_set;            /* ConsoleVisible present, REG_DWORD, nonzero */
    int have_api;              /* GetConsoleWindow resolved */
    int have_window;           /* ... and returned a window */
    const char *window_class;  /* GetClassName of it, or NULL/"" if unknown */
    unsigned long attached;    /* GetConsoleProcessList count; 0 = could not ask */
    int iconic;                /* IsIconic already */
} cm_facts_t;

/*
 * The order is the order of the log line's reason, most fundamental first.
 *
 * `attached` > 1 means the console is not the agent's own: someone started it
 * from a command prompt (retro_agent.exe -l ... while diagnosing), and that
 * prompt's window is what GetConsoleWindow returns. Minimizing it would take
 * the operator's shell away from under them. Every automatic launch - the Run
 * key, RESTART's `start`, the auto-update batch's `start` - gives the agent a
 * console of its own, so this only ever holds for a hand start. It is read
 * before any helper thread can have spawned a child onto our console. 0 means
 * the count could not be asked (Windows 2000 has no GetConsoleProcessList),
 * which is treated as "ours": every automatic launch is.
 */
static enum cm_plan cm_decide(const cm_facts_t *f)
{
    if (f->service_mode)     return CM_LEAVE_SERVICE;
    if (f->optout_set)       return CM_LEAVE_OPTOUT;
    if (!f->host_managed)    return CM_LEAVE_UNMANAGED;
    if (!f->have_api)        return CM_LEAVE_NO_API;
    if (!f->have_window)     return CM_LEAVE_NO_WINDOW;
    if (f->window_class && strcmp(f->window_class, CM_PSEUDO_CLASS) == 0)
        return CM_LEAVE_PSEUDO;
    if (f->attached > 1)     return CM_LEAVE_SHARED;
    if (f->iconic)           return CM_LEAVE_ALREADY;
    return CM_MINIMIZE;
}

static const char *cm_plan_text(enum cm_plan p)
{
    switch (p) {
    case CM_MINIMIZE:
        return "minimizing it to the taskbar so it cannot cover the desktop icons "
               "or be closed by a stray click";
    case CM_LEAVE_SERVICE:
        return "service mode - there is no console window";
    case CM_LEAVE_OPTOUT:
        return "HKLM\\" CM_OPTOUT_KEY "\\" CM_OPTOUT_VALUE " is set - leaving it as launched";
    case CM_LEAVE_UNMANAGED:
        return "modern Windows, host management is off - leaving it as launched";
    case CM_LEAVE_NO_API:
        return "this Windows has no GetConsoleWindow (9x) - the launcher starts "
               "the agent minimized (START /m in AGENTRUN.BAT and RESTART)";
    case CM_LEAVE_NO_WINDOW:
        return "GetConsoleWindow returned no window - nothing to minimize";
    case CM_LEAVE_PSEUDO:
        return "hosted by Windows Terminal - minimizing would minimize the "
               "whole terminal, leaving it";
    case CM_LEAVE_SHARED:
        return "the console is shared with the shell that started the agent - "
               "not minimizing someone's command prompt";
    case CM_LEAVE_ALREADY:
        return "already minimized";
    }
    return "?";
}

#endif /* RETRO_CONSOLEMIN_H */
