/* agent/shared/uiguard.h - the synthetic keystrokes UIKEY refuses
 * (agent 1.91.0, input.c handle_uikey).
 *
 * 2026-09-28, .124: a sweep closing Aliens vs Predator sent ALT+F4, then
 * TILDE, "quit", RETURN. The game was alive but not in the foreground, so
 * ALT+F4 reached the DESKTOP and opened "Shut Down Windows", the RETURN
 * confirmed it, and the box restarted (agent log: console control event 5,
 * then EventLog 6006). Before 1.91.0 UIKEY sent every key blind (the "old"
 * verdict below: always OK). */
#include "munit.h"
#include "../../agent/shared/uiguard.h"

static int old_verdict(void) { return UIG_OK; }

TEST(the_124_restart_sequence_is_refused)
{
    /* ALT+F4 with the desktop focused: the key that opened the dialog */
    CHECK_EQ_I(uig_check("ALT+F4", "Progman", "Program Manager"),
               UIG_REFUSE_ALTF4_SHELL);
    CHECK_EQ_I(old_verdict(), UIG_OK);
    /* ...and every key of the "console quit" that followed, into the dialog */
    CHECK_EQ_I(uig_check("TILDE", "#32770", "Shut Down Windows"), UIG_REFUSE_SHUTDOWN_DLG);
    CHECK_EQ_I(uig_check("TEXT:quit", "#32770", "Shut Down Windows"), UIG_REFUSE_SHUTDOWN_DLG);
    CHECK_EQ_I(uig_check("RETURN", "#32770", "Shut Down Windows"), UIG_REFUSE_SHUTDOWN_DLG);
}

TEST(alt_f4_to_the_taskbar_or_a_web_desktop_is_refused_too)
{
    CHECK_EQ_I(uig_check("ALT+F4", "Shell_TrayWnd", ""), UIG_REFUSE_ALTF4_SHELL);
    CHECK_EQ_I(uig_check("ALT+F4", "WorkerW", ""), UIG_REFUSE_ALTF4_SHELL);
    /* spelling does not matter, nor do extra modifiers */
    CHECK_EQ_I(uig_check("alt+f4", "progman", ""), UIG_REFUSE_ALTF4_SHELL);
    CHECK_EQ_I(uig_check("SHIFT+ALT+F4", "Progman", ""), UIG_REFUSE_ALTF4_SHELL);
}

TEST(alt_f4_still_closes_a_game_or_a_dialog)
{
    /* what ALT+F4 is FOR - the guard must not take it away */
    CHECK_EQ_I(uig_check("ALT+F4", "Halo", "Halo"), UIG_OK);
    CHECK_EQ_I(uig_check("ALT+F4", "#32770", "Error"), UIG_OK);
    CHECK_EQ_I(uig_check("ALT+F4", "ConsoleWindowClass", "C:\\WINDOWS\\system32\\cmd.exe"), UIG_OK);
    /* F4 without ALT, and ALT with another key, are not ALT+F4 */
    CHECK_EQ_I(uig_check("F4", "Progman", ""), UIG_OK);
    CHECK_EQ_I(uig_check("ALT+F", "Progman", ""), UIG_OK);
    CHECK_EQ_I(uig_check("ALT+F44", "Progman", ""), UIG_OK);
    CHECK_EQ_I(uig_check("CTRL+F4", "Progman", ""), UIG_OK);
}

TEST(escape_is_the_one_key_that_reaches_the_dialog)
{
    /* a tool that finds the dialog up must be able to cancel it */
    CHECK_EQ_I(uig_check("ESCAPE", "#32770", "Shut Down Windows"), UIG_OK);
    CHECK_EQ_I(uig_check("esc", "#32770", "Turn off computer"), UIG_OK);
    CHECK_EQ_I(uig_check("RETURN", "#32770", "Turn off computer"), UIG_REFUSE_SHUTDOWN_DLG);
    CHECK_EQ_I(uig_check("SPACE", "#32770", "Log Off Windows"), UIG_REFUSE_SHUTDOWN_DLG);
}

TEST(other_dialogs_and_the_desktop_keep_their_keys)
{
    /* a game's own dialog titled like nothing above takes any key */
    CHECK_EQ_I(uig_check("RETURN", "#32770", "Unreal Tournament"), UIG_OK);
    /* the title only counts on a real dialog class */
    CHECK_EQ_I(uig_check("RETURN", "Halo", "Shut Down Windows"), UIG_OK);
    /* keys that are not ALT+F4 may still go to the desktop (icon navigation) */
    CHECK_EQ_I(uig_check("F5", "Progman", "Program Manager"), UIG_OK);
    CHECK_EQ_I(uig_check("TEXT:abc", "Progman", ""), UIG_OK);
    /* no foreground at all: an ALT+F4 has no known target and is refused;
     * any other key is harmless and goes */
    CHECK_EQ_I(uig_check("ALT+F4", "", ""), UIG_REFUSE_ALTF4_SHELL);
    CHECK_EQ_I(uig_check("ALT+F4", 0, 0), UIG_REFUSE_ALTF4_SHELL);
    CHECK_EQ_I(uig_check("RETURN", "", ""), UIG_OK);
    CHECK(uig_reason(UIG_REFUSE_ALTF4_SHELL)[0] == 'r', "a refusal says it is one");
}

MUNIT_MAIN("uiguard (UIKEY refuses keys that shut Windows down)",
    RUN(the_124_restart_sequence_is_refused);
    RUN(alt_f4_to_the_taskbar_or_a_web_desktop_is_refused_too);
    RUN(alt_f4_still_closes_a_game_or_a_dialog);
    RUN(escape_is_the_one_key_that_reaches_the_dialog);
    RUN(other_dialogs_and_the_desktop_keep_their_keys);
)
