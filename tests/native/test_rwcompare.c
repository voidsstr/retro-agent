/* test_rwcompare.c - TRUE-SOURCE: compiles the REAL agent/shared/rwcompare.h,
 * the "is it already like that?" checks behind retrowall's startup pass
 * (agent 1.85.0).
 *
 * THE OLD-BUGGY BEHAVIOUR (retrowall.c, <= 1.84.x): on every agent start,
 * SetSysColors (a WM_SYSCOLORCHANGE broadcast - every window repaints), two
 * screensaver SPI_SET*(SPIF_SENDWININICHANGE) broadcasts, a wallpaper reload,
 * and on XP a cmd.exe+taskkill spawn - whatever the desktop already was.
 */
#include "munit.h"
#include <string.h>

#include "../../agent/shared/rwcompare.h"

#define N 4

TEST(colours_already_live_need_no_setsyscolors)
{
    unsigned long want[N] = { 0x000000, 0x00E600, 0x001C00, 0x007000 };
    unsigned long cur[N]  = { 0x000000, 0x00E600, 0x001C00, 0x007000 };
    int sup[N] = { 1, 1, 1, 1 };
    CHECK_EQ_I(rw_colors_need_apply(cur, want, sup, N), 0);
    cur[2] = 0xD4D0C8;                  /* the grey Windows Standard face */
    CHECK_EQ_I(rw_colors_need_apply(cur, want, sup, N), 1);
}

TEST(an_index_this_windows_lacks_never_forces_a_reapply)
{
    /* COLOR_MENUHILIGHT (29) does not exist on Win98: GetSysColor returns 0
     * for it and SetSysColors cannot change that. Counted, it would make every
     * boot re-apply the whole scheme - the old behaviour by another route. */
    unsigned long want[N] = { 0x000000, 0x00E600, 0x001C00, 0x007000 };
    unsigned long cur[N]  = { 0x000000, 0x00E600, 0x001C00, 0x000000 };
    int sup[N] = { 1, 1, 1, 0 };
    CHECK_EQ_I(rw_colors_need_apply(cur, want, sup, N), 0);
    sup[3] = 1;
    CHECK_EQ_I(rw_colors_need_apply(cur, want, sup, N), 1);
}

TEST(the_screensaver_is_only_set_where_it_differs)
{
    CHECK_EQ_I(rw_screensaver_sets(1, 1, 1, 600, 600), 0);
    CHECK_EQ_I(rw_screensaver_sets(1, 0, 1, 600, 600), RW_SS_SET_ACTIVE);
    CHECK_EQ_I(rw_screensaver_sets(1, 1, 1, 900, 600), RW_SS_SET_TIMEOUT);
    /* a GET that failed is treated as different - one redundant SET beats a
     * setting that is never made */
    CHECK_EQ_I(rw_screensaver_sets(0, 0, 0, 0, 600),
               RW_SS_SET_ACTIVE | RW_SS_SET_TIMEOUT);
}

TEST(toolhelp_names_are_matched_on_nt_and_9x)
{
    /* NT: a bare name. 9x: the full path. Comparing the whole string never
     * matches on 9x - the bug autoupdate.c's kill_retro_chat had until
     * 98a1b95 (.243, 2026-09-25). */
    CHECK_EQ_I(rw_image_is("rotate_wall.exe", "rotate_wall.exe"), 1);
    CHECK_EQ_I(rw_image_is("C:\\RETRO-WALL\\ROTATE_WALL.EXE", "rotate_wall.exe"), 1);
    CHECK_EQ_I(rw_image_is("C:\\retro-wall\\rotate_wall.exe.superseded",
                           "rotate_wall.exe"), 0);
    CHECK_EQ_I(rw_image_is("C:\\x\\not_rotate_wall.exe", "rotate_wall.exe"), 0);
    CHECK_EQ_I(rw_image_is("", "rotate_wall.exe"), 0);
}

TEST(registry_strings_compare_case_insensitively)
{
    CHECK_EQ_I(rw_ieq("C:\\retro-wall\\retrowall_1920x1080.bmp",
                      "c:\\RETRO-WALL\\RETROWALL_1920X1080.BMP"), 1);
    CHECK_EQ_I(rw_ieq("0 0 0", "0 0 0"), 1);
    CHECK_EQ_I(rw_ieq("0 0 0", "0 0 1"), 0);
    CHECK_EQ_I(rw_ieq("600", "6000"), 0);
}

MUNIT_MAIN("retrowall compare-first (agent/shared/rwcompare.h, agent 1.85.0)",
    RUN(colours_already_live_need_no_setsyscolors);
    RUN(an_index_this_windows_lacks_never_forces_a_reapply);
    RUN(the_screensaver_is_only_set_where_it_differs);
    RUN(toolhelp_names_are_matched_on_nt_and_9x);
    RUN(registry_strings_compare_case_insensitively);
)
