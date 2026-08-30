/* test_icon_rebuild_gate.c - the desktop icon layout is rebuilt only when the
 * desktop actually CHANGED.
 *
 * WHAT THIS PROTECTS. gs_run() used to end with an unconditional
 * gs_arrange_icons(). GAMESYNC runs at startup, and the overwhelmingly common
 * case is a box that is already fully provisioned: every title skipped, not one
 * file copied, not one shortcut created. So every boot of every machine rebuilt
 * the whole icon layout for nothing, which the user reported as "the retro
 * agent is rebuilding icons all the time".
 *
 * THE OVER-CORRECTION IS WORSE THAN THE BUG, so this test pins both sides. A
 * title that really was deployed MUST still arrange - that is the staged-game
 * fix loop the whole fleet runs, and a freshly deployed game whose icon never
 * gets placed is a visible regression. The rule is therefore:
 *
 *     arrange  <=>  a file was really written, OR a .lnk appeared/disappeared
 *
 * TWO SUBTLETIES, both of which make a naive counter wrong:
 *
 *  1. A file that the resume test SKIPS (same size AND same mtime) is not a
 *     change. gs_copy_file() returns success for it, so counting "copy
 *     succeeded" would be true on every run and would measure nothing.
 *
 *  2. gs_make_game_shortcut() REWRITES a title's .lnk on every pass whether or
 *     not anything about it changed. So counting shortcut WRITES is likewise
 *     always true. Only a link that was NOT on the desktop before changes the
 *     set of icons.
 *
 * Mirrors: agent/src/gamesync.c : gs_desk_changed(), gs_copy_file(),
 *          gs_shortcut_from_line(), gs_sweep_desktop()   -- agent v1.73.0
 */

#include "munit.h"
#include <string.h>

/* ---- mirror of the accounting in agent/src/gamesync.c ---- */
static long files_written;   /* g_gs_desk_files */
static long lnks_changed;    /* g_gs_desk_lnks  */

static void desk_reset(void) { files_written = 0; lnks_changed = 0; }
static int  desk_changed(void) { return files_written > 0 || lnks_changed > 0; }

/* gs_copy_file(): the resume early-out returns BEFORE the counter. */
static void copy_file(int skipped_by_resume, int ok)
{
    if (skipped_by_resume)
        return;            /* returned 1, but nothing crossed the wire */
    if (ok)
        files_written++;
}

/* gs_shortcut_from_line(): only a link that was not already there counts. */
static void write_shortcut(int was_already_there, int ok)
{
    if (ok && !was_already_there)
        lnks_changed++;
}

/* gs_sweep_desktop(): icons REMOVED is a change too. */
static void sweep_desktop(int moved) { lnks_changed += moved; }

/* The buggy predecessor: arrange at the end of every run, full stop. */
static int desk_changed_ALWAYS_BUGGY(void) { return 1; }

/* ---------------------------------------------------------------------- */

TEST(a_no_op_sync_on_a_provisioned_box_does_not_arrange)
{
    int i;
    desk_reset();
    /* The every-boot case: 37 titles, all already present and up to date, and
     * every .lnk already on the desktop from a previous run. */
    for (i = 0; i < 4000; i++)
        copy_file(/*skipped_by_resume=*/1, /*ok=*/1);
    for (i = 0; i < 82; i++)
        write_shortcut(/*was_already_there=*/1, /*ok=*/1);

    CHECK(files_written == 0,
          "a file skipped by the size+mtime resume test is not a change");
    CHECK(lnks_changed == 0,
          "rewriting a .lnk that was already there is not a change");
    CHECK(desk_changed() == 0,
          "a fully provisioned box must NOT rebuild its icon layout on boot");

    /* The regression this file exists for. */
    CHECK(desk_changed_ALWAYS_BUGGY() == 1,
          "the old unconditional call would have arranged here");
    CHECK(desk_changed() != desk_changed_ALWAYS_BUGGY(),
          "gated and unconditional genuinely differ on the every-boot case");
}

TEST(a_real_deploy_still_arranges)
{
    /* This is the case the user explicitly WANTS kept: a title purged and
     * redeployed by the staged-game fix loop. Suppressing it would leave the
     * new game's icon unplaced, which is worse than the original bug. */
    desk_reset();
    copy_file(0, 1);                    /* one file really written */
    CHECK(files_written == 1, "a real write is counted");
    CHECK(desk_changed(), "a title whose files were copied must arrange");

    /* ...and a brand-new shortcut alone is enough, even with no file copied:
     * a launch.txt can gain a line for a title already fully on disk. */
    desk_reset();
    write_shortcut(/*was_already_there=*/0, /*ok=*/1);
    CHECK(lnks_changed == 1, "a NEW .lnk is counted");
    CHECK(desk_changed(), "a newly created shortcut must arrange");

    /* ...and so is a removal: the sweep takes icons off the desktop. */
    desk_reset();
    sweep_desktop(3);
    CHECK(desk_changed(), "shortcuts swept away must arrange");

    /* A failed copy changes nothing and must not trigger a rebuild. */
    desk_reset();
    copy_file(0, 0);
    CHECK(!desk_changed(), "a FAILED copy is not a change");
}

TEST(the_counters_are_per_run_not_cumulative)
{
    /* gs_run() calls gs_desk_reset() at the top. Without that, one real deploy
     * would make every subsequent boot look like a change for the life of the
     * process - i.e. the bug would come straight back on any box that syncs
     * more than once without restarting. */
    desk_reset();
    copy_file(0, 1);
    CHECK(desk_changed(), "run 1 deployed something");

    desk_reset();                        /* start of run 2 */
    copy_file(1, 1);                     /* everything skipped this time */
    CHECK(!desk_changed(),
          "run 2 changed nothing, so run 1's work must not still count");
}

TEST(a_forced_pass_ignores_the_gate_entirely)
{
    /* ICONARRANGE is a deliberate human act - "fixing issues" is a legitimate
     * reason to re-arrange a desktop the agent believes is already fine. The
     * gate must not be able to refuse it. */
    int force;
    desk_reset();                        /* nothing changed at all */
    for (force = 0; force <= 1; force++) {
        int will_arrange = force || desk_changed();
        if (force)
            CHECK(will_arrange, "a forced pass always arranges");
        else
            CHECK(!will_arrange, "an unforced pass on an unchanged desktop does not");
    }
}

TEST(auto_arrange_already_on_does_not_re_pack)
{
    /* The second churn source, and it was in the new code: with auto-arrange
     * already ON the shell is keeping the desktop packed by itself, so sending
     * LVM_ARRANGE achieves nothing and is visible churn. It ran on every agent
     * startup. Send it only when we just turned the setting on, or when forced. */
    struct { int was_on, force, expect_arrange; } cases[] = {
        { 1, 0, 0 },   /* the every-startup case: already on, nothing to do   */
        { 0, 0, 1 },   /* we just turned it on: re-pack now, not at next idle */
        { 1, 1, 1 },   /* explicit ICONARRANGE on an already-on box           */
        { 0, 1, 1 },
    };
    int i;
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        int changed = !cases[i].was_on;          /* we set it => it changed */
        int arrange = (changed || cases[i].force);
        CHECK(arrange == cases[i].expect_arrange,
              "LVM_ARRANGE is sent only on a real change or an explicit force");
    }
}

/* --- the icon SET model, which is what the gate actually has to ask --- */
/* gs_run() sweeps EVERY .lnk off the desktop before writing any, so "was this
 * file there a moment ago?" is always false and cannot be the question. The
 * question is whether the SET differs from the one sampled before the sweep. */
#define SETMAX 8
typedef struct { const char *pre[SETMAX]; int pre_n; char seen[SETMAX]; long added; } iconset_t;

static void set_snapshot(iconset_t *s, const char **names, int n)
{
    int i;
    s->pre_n = n; s->added = 0;
    for (i = 0; i < n; i++) { s->pre[i] = names[i]; s->seen[i] = 0; }
}
static void set_written(iconset_t *s, const char *name)
{
    int i;
    for (i = 0; i < s->pre_n; i++)
        if (strcmp(s->pre[i], name) == 0) { s->seen[i] = 1; return; }
    s->added++;
}
static long set_changed(const iconset_t *s)
{
    int i; long gone = 0;
    for (i = 0; i < s->pre_n; i++) if (!s->seen[i]) gone++;
    return s->added + gone;
}
/* The shipped-and-broken v1.73.0 shape: count every write as new, because the
 * sweep guaranteed nothing was ever already there. */
static long set_changed_SWEPT_BUGGY(int shortcuts_written) { return shortcuts_written; }

TEST(sweeping_and_recreating_the_same_icons_is_not_a_change)
{
    /* The real defect, found on .171 minutes after the counters shipped: a
     * fully provisioned box swept 81 shortcuts and wrote back the same 81, and
     * the gate called that a change on every single sync, forever. */
    static const char *before[] = { "Quake.lnk", "Descent.lnk", "Half-Life.lnk" };
    iconset_t s;
    set_snapshot(&s, before, 3);
    set_written(&s, "Quake.lnk");
    set_written(&s, "Descent.lnk");
    set_written(&s, "Half-Life.lnk");
    CHECK(set_changed(&s) == 0,
          "sweeping and rewriting the SAME icons is not a change");
    CHECK(set_changed_SWEPT_BUGGY(3) == 3,
          "the shipped v1.73.0 shape counted all three as new");
    CHECK(set_changed(&s) != set_changed_SWEPT_BUGGY(3),
          "the two genuinely differ on the every-boot case - this is the bug");
}

TEST(a_genuinely_new_or_removed_icon_still_counts)
{
    static const char *before[] = { "Quake.lnk", "Descent.lnk" };
    iconset_t s;

    /* A title added to the library: its shortcut was not there before. */
    set_snapshot(&s, before, 2);
    set_written(&s, "Quake.lnk");
    set_written(&s, "Descent.lnk");
    set_written(&s, "Far Cry.lnk");
    CHECK(set_changed(&s) == 1, "one genuinely new icon counts as one change");

    /* A title removed from the library: its shortcut is never rewritten. */
    set_snapshot(&s, before, 2);
    set_written(&s, "Quake.lnk");
    CHECK(set_changed(&s) == 1, "an icon that is gone counts as one change");

    /* Both at once. */
    set_snapshot(&s, before, 2);
    set_written(&s, "Quake.lnk");
    set_written(&s, "Far Cry.lnk");
    CHECK(set_changed(&s) == 2, "one added and one removed is two changes");

    /* A desktop that was empty and gains icons - a fresh image. */
    set_snapshot(&s, before, 0);
    set_written(&s, "Quake.lnk");
    CHECK(set_changed(&s) == 1, "a fresh box's first shortcut is a change");
}

MUNIT_MAIN("icon rebuild gate: only when the desktop changed",
    RUN(a_no_op_sync_on_a_provisioned_box_does_not_arrange);
    RUN(a_real_deploy_still_arranges);
    RUN(the_counters_are_per_run_not_cumulative);
    RUN(a_forced_pass_ignores_the_gate_entirely);
    RUN(auto_arrange_already_on_does_not_re_pack);
    RUN(sweeping_and_recreating_the_same_icons_is_not_a_change);
    RUN(a_genuinely_new_or_removed_icon_still_counts);
)
