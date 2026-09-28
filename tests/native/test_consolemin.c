/* agent/shared/consolemin.h - does the agent minimize its own console at
 * startup? (agent/src/consolewin.c:consolewin_startup, the 2026-09-28
 * console-covers-icons fix.)
 *
 * The defect: every agent start opened its console in the middle of the
 * desktop and left it there. WINLIST after a boot: on .243 (Win98) it covered
 * the whole icon bay (22,22-674,381); on .124/.195/.110 it covered the
 * top-left icons Auto Arrange packs there. People closed it by accident
 * (CTRL_CLOSE_EVENT, .195, twice on 09-26 - 35 minutes with no agent).
 *
 * Pinned here: the ordinary boot (Run key, XP/7, a fleet box) is MINIMIZE,
 * where the old agent did nothing at all; and every case that must be LEFT
 * ALONE stays left alone - above all a console shared with an operator's
 * command prompt and a Windows Terminal host, where minimizing would take
 * someone's own window away. */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/consolemin.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

/* An XP fleet box after logon: Run key -> retro_agent.exe in a console of
 * its own, restored, over the icons. */
static cm_facts_t boot(void)
{
    cm_facts_t f;
    memset(&f, 0, sizeof(f));
    f.service_mode = 0;
    f.host_managed = 1;
    f.optout_set   = 0;
    f.have_api     = 1;
    f.have_window  = 1;
    f.window_class = "ConsoleWindowClass";   /* what WINLIST shows on .195/.110 */
    f.attached     = 1;
    f.iconic       = 0;
    return f;
}

/* What the agent did before this fix, for every input: nothing. */
static enum cm_plan old_agent(const cm_facts_t *f) { (void)f; return CM_LEAVE_NO_WINDOW; }

int main(void)
{
    cm_facts_t f;
    printf("test_consolemin (agent/shared/consolemin.h)\n");

    /* ---- the fix ---------------------------------------------------- */
    f = boot();
    CHECK(cm_decide(&f) == CM_MINIMIZE,
          "ordinary boot on a fleet box: MINIMIZE (the fix)");
    CHECK(old_agent(&f) != CM_MINIMIZE,
          "the old agent left that same console over the icons (the bug)");

    /* Win 7 conhost reports the same class; a missing class name (the read
     * failed) must not stop the minimize either. */
    f = boot(); f.window_class = "";
    CHECK(cm_decide(&f) == CM_MINIMIZE, "unknown window class still minimizes");
    f = boot(); f.window_class = NULL;
    CHECK(cm_decide(&f) == CM_MINIMIZE, "NULL window class still minimizes");

    /* Windows 2000 has GetConsoleWindow but no GetConsoleProcessList:
     * count 0 means "could not ask", and every automatic launch is ours. */
    f = boot(); f.attached = 0;
    CHECK(cm_decide(&f) == CM_MINIMIZE, "process count unknown (Win2000): minimize");

    /* ---- the show command ------------------------------------------ */
    CHECK(CM_SHOW_CMD == 7, "SW_SHOWMINNOACTIVE (7): minimized, taskbar button kept, nothing activated");
    CHECK(CM_SHOW_CMD != 0, "not SW_HIDE (0): the operator must still find it on the taskbar");
    CHECK(CM_SHOW_CMD != 6 && CM_SHOW_CMD != 2,
          "not SW_MINIMIZE (6) / SW_SHOWMINIMIZED (2): both hand focus to another window");

    /* ---- what must be LEFT ALONE ------------------------------------ */
    f = boot(); f.service_mode = 1;
    CHECK(cm_decide(&f) == CM_LEAVE_SERVICE, "NT service mode: no console, leave");

    f = boot(); f.optout_set = 1;
    CHECK(cm_decide(&f) == CM_LEAVE_OPTOUT, "ConsoleVisible=1 opts out");

    f = boot(); f.host_managed = 0;
    CHECK(cm_decide(&f) == CM_LEAVE_UNMANAGED, "modern Windows, unmanaged: leave");

    f = boot(); f.have_api = 0; f.have_window = 0; f.window_class = ""; f.attached = 0;
    CHECK(cm_decide(&f) == CM_LEAVE_NO_API,
          "Win9x (no GetConsoleWindow): leave - START /m in the launcher does it");

    f = boot(); f.have_window = 0; f.window_class = "";
    CHECK(cm_decide(&f) == CM_LEAVE_NO_WINDOW, "GetConsoleWindow NULL: leave");

    f = boot(); f.window_class = CM_PSEUDO_CLASS;
    CHECK(cm_decide(&f) == CM_LEAVE_PSEUDO,
          "Windows Terminal host: leave (it would minimize the whole terminal)");

    f = boot(); f.attached = 2;
    CHECK(cm_decide(&f) == CM_LEAVE_SHARED,
          "console shared with the cmd.exe that started it by hand: leave");
    f = boot(); f.attached = 5;
    CHECK(cm_decide(&f) == CM_LEAVE_SHARED, "any count above one is shared");

    f = boot(); f.iconic = 1;
    CHECK(cm_decide(&f) == CM_LEAVE_ALREADY,
          "already minimized (start /min, a shortcut): nothing to do");

    /* The opt-out outranks everything a live box could report, and service
     * mode outranks the opt-out (there is nothing to keep visible). */
    f = boot(); f.optout_set = 1; f.window_class = CM_PSEUDO_CLASS; f.attached = 3;
    CHECK(cm_decide(&f) == CM_LEAVE_OPTOUT, "opt-out is the reason logged when set");
    f = boot(); f.service_mode = 1; f.optout_set = 1;
    CHECK(cm_decide(&f) == CM_LEAVE_SERVICE, "service mode is decided first");

    /* A Windows Terminal window is left alone even when nothing else shares
     * the console - the pseudo window IS the terminal's. */
    f = boot(); f.window_class = CM_PSEUDO_CLASS; f.attached = 1;
    CHECK(cm_decide(&f) == CM_LEAVE_PSEUDO, "pseudo-console with one process: still leave");

    /* ---- every plan says something ---------------------------------- */
    {
        int p, ok = 1;
        for (p = CM_MINIMIZE; p <= CM_LEAVE_ALREADY; p++) {
            const char *t = cm_plan_text((enum cm_plan)p);
            if (!t || !t[0] || strcmp(t, "?") == 0) ok = 0;
        }
        CHECK(ok, "every plan has a log reason");
        CHECK(strstr(cm_plan_text(CM_LEAVE_OPTOUT), "ConsoleVisible") != NULL,
              "the opt-out reason names the registry value");
        CHECK(strstr(cm_plan_text(CM_LEAVE_NO_API), "START /m") != NULL,
              "the Win9x reason says where the minimize happens instead");
    }

    printf("  %d/%d passed\n", runs - fails, runs);
    return fails ? 1 : 0;
}
