/* test_shortcutdir.c - TRUE-SOURCE: compiles the REAL
 * agent/shared/shortcutdir.h (HKLM\Software\RetroAgent\ShortcutFolder - one
 * folder on the desktop instead of 150 loose game icons). agent 1.99.0.
 *
 * WHY EACH REFUSAL IS A TEST AND NOT A STYLE RULE. The sweep (deskset.h)
 * DELETES - it moves every .lnk/.pif/.url a run did not put back into
 * C:\retro-desktop-backup - and with a folder configured the sweep follows
 * wherever this module points. So:
 *
 *   - a value holding a PATH must be refused, not joined: "..\..\Windows" or
 *     "C:\Windows\System32" in one registry value would otherwise aim the
 *     sweep at a system directory, reachable by typo;
 *   - a TRAILING SPACE OR DOT must be refused, because Windows silently strips
 *     those when it creates the directory - the folder we then look in and
 *     sweep is not the folder we made, so every run re-adds every shortcut to
 *     a directory it cannot see and the icon-change gate never settles at 0;
 *   - a path that does not FIT must fail, never truncate: a truncated
 *     directory path is the one failure mode that has the sweep delete files
 *     from somewhere else entirely.
 *
 * Also asserts the fallback contract: a refused name returns 0 and writes no
 * path, so the caller falls back to the desktop LOUDLY. A refusal that quietly
 * meant "the desktop" is how 150 icons reappear on a machine that asked for a
 * folder - the exact complaint this feature answers.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>

#include "../../agent/shared/shortcutdir.h"

#define DESK "C:\\Documents and Settings\\All Users\\Desktop"

TEST(t_an_ordinary_folder_name_is_taken)
{
    char out[SCD_PATH_MAX];

    CHECK_EQ_I(scd_name_ok("Retro Games"), 1);
    CHECK_EQ_I(scd_name_ok("Games"), 1);
    CHECK_EQ_I(scd_name_ok("Games (retro)"), 1);   /* parens are fine in a DIR */
    CHECK_EQ_I(scd_join(DESK, "Retro Games", out, sizeof(out)), 1);
    CHECK(strcmp(out, DESK "\\Retro Games") == 0, "joined path");
}

TEST(t_a_path_is_not_a_folder_name)
{
    char out[SCD_PATH_MAX];

    /* Each of these would redirect the whole shortcut set - and the sweep. */
    CHECK_EQ_I(scd_name_ok("..\\..\\Windows"), 0);
    CHECK_EQ_I(scd_name_ok("C:\\Windows\\System32"), 0);
    CHECK_EQ_I(scd_name_ok("Games\\Retro"), 0);
    CHECK_EQ_I(scd_name_ok("Games/Retro"), 0);
    CHECK_EQ_I(scd_name_ok(".."), 0);
    CHECK_EQ_I(scd_name_ok("."), 0);
    CHECK_EQ_I(scd_name_ok("C:"), 0);

    /* ...and a refused name produces NO path at all. */
    CHECK_EQ_I(scd_join(DESK, "..\\..\\Windows", out, sizeof(out)), 0);
    CHECK_EQ_I((int)out[0], 0);
    CHECK_EQ_I(scd_join(DESK, "C:\\Windows\\System32", out, sizeof(out)), 0);
    CHECK_EQ_I((int)out[0], 0);
}

TEST(t_names_windows_would_silently_rewrite_are_refused)
{
    /* CreateDirectory("Games ") makes "Games"; we would then sweep "Games " -
     * which does not exist - and re-add every shortcut on every run. */
    CHECK_EQ_I(scd_name_ok("Games "), 0);
    CHECK_EQ_I(scd_name_ok("Games."), 0);
    CHECK_EQ_I(scd_name_ok(" Games"), 0);
    /* An inner space or dot is kept by Windows and is allowed. */
    CHECK_EQ_I(scd_name_ok("Retro Games"), 1);
    CHECK_EQ_I(scd_name_ok("Games.v2"), 1);
}

TEST(t_illegal_and_unmatchable_characters_are_refused)
{
    char bad[8];

    CHECK_EQ_I(scd_name_ok("Game*s"), 0);
    CHECK_EQ_I(scd_name_ok("Game?s"), 0);
    CHECK_EQ_I(scd_name_ok("Game\"s"), 0);
    CHECK_EQ_I(scd_name_ok("Game<s"), 0);
    CHECK_EQ_I(scd_name_ok("Game>s"), 0);
    CHECK_EQ_I(scd_name_ok("Game|s"), 0);
    /* A control character cannot be typed in the shell and the sweep's
     * case-insensitive name compare would never match it back. */
    snprintf(bad, sizeof(bad), "Ga%ces", 7);
    CHECK_EQ_I(scd_name_ok(bad), 0);
}

TEST(t_empty_means_the_desktop_as_before)
{
    char out[SCD_PATH_MAX];

    CHECK_EQ_I(scd_name_ok(""), 0);
    CHECK_EQ_I(scd_name_ok(NULL), 0);
    CHECK_EQ_I(scd_join(DESK, "", out, sizeof(out)), 0);
}

TEST(t_a_name_too_long_is_refused_not_truncated)
{
    char long_name[SCD_NAME_MAX + 8];
    char ok_name[SCD_NAME_MAX + 1];
    char out[SCD_PATH_MAX];

    memset(long_name, 'G', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    CHECK_EQ_I(scd_name_ok(long_name), 0);

    memset(ok_name, 'G', SCD_NAME_MAX);
    ok_name[SCD_NAME_MAX] = 0;
    CHECK_EQ_I(scd_name_ok(ok_name), 1);          /* exactly at the limit */
    CHECK_EQ_I(scd_join(DESK, ok_name, out, sizeof(out)), 1);
}

TEST(t_a_path_that_does_not_fit_fails_rather_than_truncating)
{
    char out[24];
    char deep[SCD_PATH_MAX];
    char tiny[4];

    /* The joined path is longer than the buffer: 0, and NOTHING written. */
    CHECK_EQ_I(scd_join(DESK, "Retro Games", out, sizeof(out)), 0);
    CHECK_EQ_I((int)out[0], 0);

    /* A near-MAX_PATH desktop plus a folder name: still refused, not cut. */
    memset(deep, 'd', sizeof(deep) - 1);
    deep[0] = 'C'; deep[1] = ':'; deep[2] = '\\';
    deep[sizeof(deep) - 1] = 0;
    CHECK_EQ_I(scd_join(deep, "Retro Games", out, sizeof(out)), 0);

    /* Degenerate buffers must not be written through. */
    CHECK_EQ_I(scd_join(DESK, "Games", tiny, sizeof(tiny)), 0);
    CHECK_EQ_I(scd_join(DESK, "Games", out, 0), 0);
    CHECK_EQ_I(scd_join(DESK, "Games", NULL, sizeof(out)), 0);
}

TEST(t_a_parent_ending_in_a_backslash_does_not_double_it)
{
    char out[SCD_PATH_MAX];

    /* SHGetFolderPath does not return a trailing slash, but a root does:
     * "C:\" + "Games" must be "C:\Games", not "C:\\Games", or the sweep's
     * name compare and the shell disagree about the directory. */
    CHECK_EQ_I(scd_join("C:\\", "Games", out, sizeof(out)), 1);
    CHECK(strcmp(out, "C:\\Games") == 0, "no doubled separator");
    CHECK_EQ_I(scd_join("", "Games", out, sizeof(out)), 0);
    CHECK_EQ_I(scd_join(NULL, "Games", out, sizeof(out)), 0);
}

MUNIT_MAIN("shortcutdir (ShortcutFolder: one folder, not 150 icons)", {
    RUN(t_an_ordinary_folder_name_is_taken);
    RUN(t_a_path_is_not_a_folder_name);
    RUN(t_names_windows_would_silently_rewrite_are_refused);
    RUN(t_illegal_and_unmatchable_characters_are_refused);
    RUN(t_empty_means_the_desktop_as_before);
    RUN(t_a_name_too_long_is_refused_not_truncated);
    RUN(t_a_path_that_does_not_fit_fails_rather_than_truncating);
    RUN(t_a_parent_ending_in_a_backslash_does_not_double_it);
})
