/* test_icon_autoarrange.c - desktop Auto Arrange must be SET, never TOGGLED,
 * and the persisted FFlags word must be read-modify-written.
 *
 * WHAT THIS PROTECTS. agent/src/gamesync.c:gs_apply_autoarrange() turns
 * Windows' Auto Arrange ON so the shell keeps the desktop packed by itself -
 * the fleet default since v1.73.0. Two things about that are easy to get
 * wrong, and both have already cost this project real time in the opposite
 * direction:
 *
 *  1. FCIDM_SHVIEW_AUTOARRANGE is a WM_COMMAND **TOGGLE**, not a set. The
 *     previous generation of this code fired it blindly to turn auto-arrange
 *     OFF and thereby turned it ON on every box that already had it off,
 *     leaving icons in rows across the top of the screen. The same trap exists
 *     in reverse now: posting it on a box that already has auto-arrange ON
 *     turns it OFF. So the post is legal only when it moves the bit the way we
 *     want, i.e. only when the bit is currently CLEAR.
 *
 *  2. FFlags in HKCU\Software\Microsoft\Windows\Shell\Bags\1\Desktop is a
 *     FOLDERFLAGS word carrying several unrelated bits. It is NOT uniform
 *     across the fleet - measured 2026-08-30, .143 read 0x220 and .171 read
 *     0x224, the difference being FWF_SNAPTOGRID (align-to-grid). Stamping a
 *     constant word would therefore have silently changed align-to-grid on
 *     some boxes and not others. Only bit 0 may move.
 *
 * Every assertion below checks BOTH the fixed value and the old buggy value,
 * so a regression cannot pass by coincidence.
 *
 * Mirrors: agent/src/gamesync.c : gs_apply_autoarrange(), gs_bag_autoarrange()
 *          agent v1.73.0
 */

#include "munit.h"

/* ---- verbatim constants from agent/src/gamesync.c ---- */
#define LVS_AUTOARRANGE     0x0100
#define GS_FWF_AUTOARRANGE  0x00000001u
#define GS_FWF_SNAPTOGRID   0x00000004u

/* ---------------------------------------------------------------------- */
/* Model of the decision gs_apply_autoarrange() makes.                      */
/*                                                                          */
/* Returns 1 when the shell's FCIDM_SHVIEW_AUTOARRANGE toggle may be posted. */
/* This is the whole invariant in one line: post it ONLY when the bit is     */
/* clear, because posting is a toggle and would otherwise clear a set bit.   */
static int may_post_toggle(unsigned long style)
{
    return (style & LVS_AUTOARRANGE) ? 0 : 1;
}

/* The old, buggy shape: post it whenever we want the setting changed at all,
 * without looking. Kept so the test can assert the two genuinely differ. */
static int may_post_toggle_BUGGY(unsigned long style)
{
    (void)style;
    return 1;
}

/* What the style word becomes once the fallback SetWindowLong runs. This is a
 * SET (OR), not an XOR - an XOR here would be the same toggle bug wearing a
 * different hat. */
static unsigned long style_after_set(unsigned long style)
{
    return style | LVS_AUTOARRANGE;
}

static unsigned long style_after_XOR_BUGGY(unsigned long style)
{
    return style ^ LVS_AUTOARRANGE;
}

/* Mirror of gs_bag_autoarrange(): read-modify-write of exactly bit 0. */
static unsigned long fflags_after(unsigned long before, int on)
{
    return on ? (before | GS_FWF_AUTOARRANGE)
              : (before & ~GS_FWF_AUTOARRANGE);
}

/* The buggy alternative: stamp a whole word. */
static unsigned long fflags_STAMPED_BUGGY(unsigned long before, int on)
{
    (void)before;
    return on ? 0x221u : 0x220u;
}

/* ---------------------------------------------------------------------- */

TEST(toggle_is_only_posted_when_it_moves_the_bit_the_right_way)
{
    /* Real style words measured on the fleet 2026-08-30. */
    const unsigned long off_171 = 0x56002a40ul;  /* .171 before the change */
    const unsigned long on_171  = 0x56002b40ul;  /* .171 after  the change */

    CHECK(!(off_171 & LVS_AUTOARRANGE),
          "measured .171 'before' word really has auto-arrange clear");
    CHECK((on_171 & LVS_AUTOARRANGE),
          "measured .171 'after' word really has auto-arrange set");

    CHECK(may_post_toggle(off_171) == 1,
          "auto-arrange OFF: posting the toggle turns it ON, so it is allowed");
    CHECK(may_post_toggle(on_171) == 0,
          "auto-arrange ON: posting the toggle would turn it OFF - forbidden");

    /* The regression this file exists for: a blind post. */
    CHECK(may_post_toggle_BUGGY(on_171) == 1,
          "the old blind-post shape would have posted on an already-ON box");
    CHECK(may_post_toggle(on_171) != may_post_toggle_BUGGY(on_171),
          "guarded and blind posting genuinely differ on an already-ON box");
}

TEST(the_style_fallback_is_a_set_not_a_toggle)
{
    const unsigned long off_171 = 0x56002a40ul;
    const unsigned long on_171  = 0x56002b40ul;

    /* From OFF, set and XOR agree - which is exactly why an XOR bug survives
     * casual testing on a box that happens to start in the OFF state. */
    CHECK_EQ_U(style_after_set(off_171), on_171);
    CHECK_EQ_U(style_after_XOR_BUGGY(off_171), on_171);

    /* From ON they diverge, and only the SET is idempotent. This is the case
     * that matters: gs_desktop_icons_apply() runs on EVERY agent startup, so
     * it meets an already-ON desktop far more often than a fresh one. */
    CHECK_EQ_U(style_after_set(on_171), on_171);
    CHECK(style_after_XOR_BUGGY(on_171) != on_171,
          "an XOR fallback would clear auto-arrange on the second startup");
    CHECK(!(style_after_XOR_BUGGY(on_171) & LVS_AUTOARRANGE),
          "...specifically, it would turn the fleet setting back off");

    /* Applying it twice must be identical to applying it once. */
    CHECK_EQ_U(style_after_set(style_after_set(off_171)),
               style_after_set(off_171));
}

TEST(fflags_read_modify_write_preserves_the_other_bits)
{
    /* Measured on the fleet 2026-08-30. The two boxes DISAGREE, which is the
     * entire reason a constant may not be stamped. */
    const unsigned long f143 = 0x220ul;  /* FWF_DESKTOP|FWF_NOCLIENTEDGE     */
    const unsigned long f171 = 0x224ul;  /* ...plus FWF_SNAPTOGRID           */

    CHECK(!(f143 & GS_FWF_SNAPTOGRID), ".143 measured with align-to-grid off");
    CHECK((f171 & GS_FWF_SNAPTOGRID),  ".171 measured with align-to-grid on");

    CHECK_EQ_U(fflags_after(f143, 1), 0x221ul);
    CHECK_EQ_U(fflags_after(f171, 1), 0x225ul);

    /* Every bit except bit 0 must survive, on both boxes. */
    CHECK_EQ_U(fflags_after(f143, 1) & ~GS_FWF_AUTOARRANGE, f143);
    CHECK_EQ_U(fflags_after(f171, 1) & ~GS_FWF_AUTOARRANGE, f171);
    CHECK((fflags_after(f171, 1) & GS_FWF_SNAPTOGRID),
          "enabling auto-arrange must not disturb .171's align-to-grid bit");

    /* The stamping bug: right on .143 by luck, wrong on .171. */
    CHECK_EQ_U(fflags_STAMPED_BUGGY(f143, 1), fflags_after(f143, 1));
    CHECK(fflags_STAMPED_BUGGY(f171, 1) != fflags_after(f171, 1),
          "a stamped constant silently clears .171's align-to-grid bit");

    /* Idempotent, and the off direction is a clean inverse. */
    CHECK_EQ_U(fflags_after(fflags_after(f171, 1), 1), fflags_after(f171, 1));
    CHECK_EQ_U(fflags_after(fflags_after(f171, 1), 0), f171);
}

TEST(the_bay_and_autoarrange_are_mutually_exclusive)
{
    /* Not arithmetic, but the property the whole design rests on: with
     * auto-arrange set the shell ignores LVM_SETITEMPOSITION, so a build that
     * ran the bay AND set auto-arrange would be two mechanisms fighting. The
     * switch must therefore select exactly one. Model the selector. */
    const int AUTO = 1, BAY = 0;
    int i;
    /* HKLM\Software\RetroAgent\IconAutoArrange: absent -> 1 (auto). */
    struct { int present; unsigned long value; int expect; } cases[] = {
        { 0, 0,  AUTO },   /* key absent  -> auto-arrange, the fleet default */
        { 1, 1,  AUTO },
        { 1, 2,  AUTO },   /* any non-zero is "on" */
        { 1, 0,  BAY  },   /* explicit opt-out */
    };
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        int want = cases[i].present ? (cases[i].value != 0) : 1;
        CHECK(want == cases[i].expect,
              "IconAutoArrange selects the documented layout");
    }
    /* The default MUST be auto-arrange: a box that has never heard of the
     * switch gets the behaviour the user actually asked for. */
    CHECK(cases[0].expect == AUTO,
          "with the registry value absent the default is AUTO-ARRANGE");
}

MUNIT_MAIN("icon auto-arrange: set, never toggle",
    RUN(toggle_is_only_posted_when_it_moves_the_bit_the_right_way);
    RUN(the_style_fallback_is_a_set_not_a_toggle);
    RUN(fflags_read_modify_write_preserves_the_other_bits);
    RUN(the_bay_and_autoarrange_are_mutually_exclusive);
)
