/* test_desk_sweep_order.c - TRUE-SOURCE: compiles the REAL
 * agent/shared/deskset.h (what GAMESYNC's desktop sweep may remove, and did the
 * icon set change?) and agent/shared/gsstall.h (is the run moving, and if not,
 * is it starved of CPU?). agent 1.90.0.
 *
 * THE DEFECT (.110, XP P4, 2026-09-28). gs_run() began with gs_sweep_desktop(),
 * which moved EVERY .lnk/.pif/.url off both desktops, and each game icon came
 * back only when the copy loop reached its title. A run started at 11:17 then
 * sat in state=sizing, message "enumerating library", titles_total 0, for ~100
 * minutes - a minimized ioquake3 held 96% of the single CPU and the worker runs
 * at THREAD_PRIORITY_IDLE - and the desktop showed two icons (Retro Agent,
 * Retro Chat) where 97 had been. The agent log: "desktop swept: 97 shortcut(s)
 * moved to C:\retro-desktop-backup", then nothing about why. Evidence:
 * .claude/evidence-icons/192.168.1.110/ (desktop-dirlist.json lists exactly
 * those two .lnk files; gamesync-status-busy.json reads elapsed_s 3583,
 * message "enumerating library").
 *
 * THE FIX, pinned here from both sides - the old-buggy value and the fixed one:
 *   - nothing is removed until the END of a run that considered every title;
 *     then only what was on the desktop at the start and was not put back;
 *   - the final desktop is the one the old order produced (parity);
 *   - the icon-rebuild gate still reads 0 on a box whose icons did not change;
 *   - a stall is measured, and called STARVATION only when the CPU was busy.
 */
#include "munit.h"
#include <string.h>
#include <stdio.h>

#include "../../agent/shared/deskset.h"
#include "../../agent/shared/gsstall.h"

/* ---------------------------------------------------------------------- */
/* a tiny model of the two desktops                                       */
/* ---------------------------------------------------------------------- */
#define MAXI 128
typedef struct { char name[DS_NAME]; unsigned where; } icon_t;
typedef struct { icon_t i[MAXI]; int n; } desk_t;

static void put(desk_t *d, const char *name, unsigned where)
{
    snprintf(d->i[d->n].name, DS_NAME, "%s", name);
    d->i[d->n].where = where;
    d->n++;
}
static int has(const desk_t *d, const char *name, unsigned where)
{
    int k;
    for (k = 0; k < d->n; k++)
        if (ds_ieq(d->i[k].name, name) && d->i[k].where == where)
            return 1;
    return 0;
}
static void del(desk_t *d, const char *name, unsigned where)
{
    int k;
    for (k = 0; k < d->n; k++)
        if (ds_ieq(d->i[k].name, name) && d->i[k].where == where) {
            d->i[k] = d->i[--d->n];
            return;
        }
}
/* A rewrite in place: the file is replaced, the icon never leaves. */
static void write_lnk(desk_t *d, const char *name)
{
    if (!has(d, name, DS_COMMON))
        put(d, name, DS_COMMON);
}
static void snapshot(ds_set_t *s, const desk_t *d)
{
    int k;
    ds_reset(s);
    for (k = 0; k < d->n; k++)
        ds_add(s, d->i[k].name, d->i[k].where);
}
/* gs_sweep_unclaimed(): move away exactly what ds_sweep_bits() names. */
static int sweep_unclaimed(ds_set_t *s, desk_t *d)
{
    int k, moved = 0;
    unsigned bit;
    for (k = 0; k < s->n; k++)
        for (bit = DS_COMMON; bit <= DS_USER; bit <<= 1)
            if (ds_sweep_bits(&s->e[k]) & bit) {
                del(d, s->e[k].name, bit);
                ds_mark_gone(&s->e[k], bit);
                moved++;
            }
    return moved;
}
/* THE OLD ORDER (<= 1.89.1): every sweepable file off both desktops, first. */
static int sweep_everything_first_OLD(desk_t *d)
{
    int k, moved = 0;
    for (k = d->n - 1; k >= 0; k--)
        if (ds_is_sweepable(d->i[k].name)) {
            d->i[k] = d->i[--d->n];
            moved++;
        }
    return moved;
}

/* .110's desktop before the run: 95 game shortcuts plus the two tool icons. */
static void desk_110(desk_t *d)
{
    int k;
    char nm[64];
    d->n = 0;
    for (k = 0; k < 95; k++) {
        snprintf(nm, sizeof(nm), "Game %02d.lnk", k);
        put(d, nm, DS_COMMON);
    }
    put(d, "Retro Agent.lnk", DS_COMMON);
    put(d, "Retro Chat.lnk", DS_COMMON);
}

/* ---------------------------------------------------------------------- */

TEST(during_the_sizing_phase_the_desktop_keeps_every_icon)
{
    static desk_t old_d, new_d;
    static ds_set_t s;

    /* OLD: gs_run() swept, then put back the two tool shortcuts, then started
     * enumerating the library - and a starved run stayed right there. */
    desk_110(&old_d);
    CHECK_EQ_I(sweep_everything_first_OLD(&old_d), 97);   /* "97 shortcut(s)" */
    write_lnk(&old_d, "Retro Agent.lnk");
    write_lnk(&old_d, "Retro Chat.lnk");
    CHECK_EQ_I(old_d.n, 2);     /* desktop-dirlist.json: exactly these two */

    /* NEW: snapshot, claim the tools, and NOTHING is removed while the run is
     * sizing, enumerating or copying - the sweep is the last step. */
    desk_110(&new_d);
    snapshot(&s, &new_d);
    ds_claim(&s, "Retro Agent.lnk", DS_COMMON, 0);        /* already correct */
    ds_claim(&s, "Retro Chat.lnk", DS_COMMON, 0);
    CHECK_EQ_I(new_d.n, 97);
    CHECK(new_d.n != old_d.n, "the stall window no longer empties the desktop");
}

TEST(the_end_state_is_the_one_the_old_order_produced)
{
    static desk_t old_d, new_d;
    static ds_set_t s;
    static const char *start[] = {
        "Quake III Arena.lnk", "Descent.lnk", "Retro Agent.lnk",
        "Retro Chat.lnk", "Removed From Library.lnk", "Vendor Offer.url",
    };
    static const char *written[] = {           /* what this run puts back */
        "Quake III Arena.lnk", "Descent.lnk", "Far Cry.lnk",
    };
    int k;

    old_d.n = new_d.n = 0;
    for (k = 0; k < 6; k++) {
        put(&old_d, start[k], DS_COMMON);
        put(&new_d, start[k], DS_COMMON);
    }
    put(&old_d, "Descent.lnk", DS_USER);     /* a per-user duplicate */
    put(&new_d, "Descent.lnk", DS_USER);
    put(&old_d, "notes.txt", DS_USER);       /* a real file - never touched */
    put(&new_d, "notes.txt", DS_USER);

    /* OLD order: sweep all, write the run's set, re-place the tools. */
    sweep_everything_first_OLD(&old_d);
    write_lnk(&old_d, "Retro Agent.lnk");
    write_lnk(&old_d, "Retro Chat.lnk");
    for (k = 0; k < 3; k++)
        write_lnk(&old_d, written[k]);

    /* NEW order: snapshot, claim, rewrite in place, sweep at the end. */
    snapshot(&s, &new_d);
    ds_claim(&s, "Retro Agent.lnk", DS_COMMON, 0);
    ds_claim(&s, "Retro Chat.lnk", DS_COMMON, 0);
    for (k = 0; k < 3; k++) {
        ds_claim(&s, written[k], DS_COMMON, 1);
        write_lnk(&new_d, written[k]);
    }
    CHECK(ds_run_may_sweep(0, 1), "a complete run sweeps");
    CHECK_EQ_I(sweep_unclaimed(&s, &new_d), 3);  /* stale, .url, user dup */

    CHECK_EQ_I(new_d.n, old_d.n);
    for (k = 0; k < old_d.n; k++)
        CHECK(has(&new_d, old_d.i[k].name, old_d.i[k].where),
              "every icon the old order ended with is there");
    CHECK(!has(&new_d, "Removed From Library.lnk", DS_COMMON),
          "a title no longer staged still loses its icon");
    CHECK(!has(&new_d, "Vendor Offer.url", DS_COMMON), ".url clutter still goes");
    CHECK(!has(&new_d, "Descent.lnk", DS_USER),
          "a per-user duplicate still goes, or one icon becomes two");
    CHECK(has(&new_d, "notes.txt", DS_USER), "a real file is never swept");
}

TEST(a_run_that_does_not_finish_removes_nothing)
{
    static desk_t d;
    static ds_set_t s;

    CHECK(!ds_run_may_sweep(1, 1), "an aborted run must not sweep");
    CHECK(!ds_run_may_sweep(0, 0), "a truncated or capped listing must not sweep");
    CHECK(!ds_run_may_sweep(1, 0), "neither");
    CHECK(ds_run_may_sweep(0, 1), "only a complete run sweeps");

    /* Aborted after the first title: the titles it never reached keep their
     * icons, and the gate sees no removal. OLD: all of them were already gone. */
    desk_110(&d);
    snapshot(&s, &d);
    ds_claim(&s, "Game 00.lnk", DS_COMMON, 1);
    if (ds_run_may_sweep(1, 1))
        sweep_unclaimed(&s, &d);
    CHECK_EQ_I(d.n, 97);
    CHECK_EQ_I(ds_changed(&s), 0);
}

TEST(the_rebuild_gate_still_reads_zero_on_an_unchanged_desktop)
{
    static desk_t d;
    static ds_set_t s;
    int k;
    char nm[64];

    desk_110(&d);
    snapshot(&s, &d);
    ds_claim(&s, "Retro Agent.lnk", DS_COMMON, 0);
    ds_claim(&s, "Retro Chat.lnk", DS_COMMON, 0);
    for (k = 0; k < 95; k++) {                /* every icon rewritten in place */
        snprintf(nm, sizeof(nm), "GAME %02d.LNK", k);   /* case differs */
        CHECK_EQ_I(ds_claim(&s, nm, DS_COMMON, 1), 0);
    }
    CHECK_EQ_I(sweep_unclaimed(&s, &d), 0);
    CHECK_EQ_I(ds_changed(&s), 0);
    /* The shipped-and-broken v1.73.0 counter: every write counted as new. */
    CHECK(ds_changed(&s) != 95, "counting writes would say 95 here");
}

TEST(a_genuine_addition_or_removal_still_counts)
{
    static desk_t d;
    static ds_set_t s;

    d.n = 0;
    put(&d, "Quake.lnk", DS_COMMON);
    put(&d, "Descent.lnk", DS_COMMON);
    snapshot(&s, &d);
    CHECK_EQ_I(ds_claim(&s, "Quake.lnk", DS_COMMON, 1), 0);
    CHECK_EQ_I(ds_claim(&s, "Far Cry.lnk", DS_COMMON, 1), 1);   /* new */
    CHECK_EQ_I(ds_changed(&s), 1);      /* Descent not gone until it is moved */
    sweep_unclaimed(&s, &d);
    CHECK_EQ_I(ds_changed(&s), 2);      /* one added, one really removed */

    /* A move that FAILS leaves the icon on the desktop: not a removal. */
    ds_reset(&s);
    ds_add(&s, "Stuck.lnk", DS_COMMON);
    CHECK_EQ_I(ds_sweep_bits(&s.e[0]), DS_COMMON);
    CHECK_EQ_I(ds_changed(&s), 0);      /* not marked gone: still there */
}

TEST(a_duplicate_on_the_user_desktop_goes_without_counting_a_change)
{
    static ds_set_t s;
    ds_reset(&s);
    ds_add(&s, "Quake.lnk", DS_COMMON);
    ds_add(&s, "quake.lnk", DS_USER);   /* same name, other desktop, other case */
    CHECK_EQ_I(s.n, 1);
    ds_claim(&s, "Quake.lnk", DS_COMMON, 1);
    CHECK_EQ_I(ds_sweep_bits(&s.e[0]), DS_USER);
    ds_mark_gone(&s.e[0], DS_USER);
    CHECK_EQ_I(ds_sweep_bits(&s.e[0]), 0);
    CHECK_EQ_I(ds_changed(&s), 0);      /* the icon is still visible */
}

TEST(a_long_name_is_matched_whole)
{
    /* The previous snapshot kept 95 characters. Harmless for a counter; fatal
     * once the snapshot decides what the sweep removes: the run's own rewrite
     * of a long-named shortcut would not match and the sweep would take it. */
    static ds_set_t s;
    char longname[200];
    char trunc95[96];
    memset(longname, 'x', 150);
    memcpy(longname + 150, " - Extended Edition.lnk", 24);
    memcpy(trunc95, longname, 95);             /* what GS_LNK_NAME 96 kept */
    trunc95[95] = 0;

    ds_reset(&s);
    ds_add(&s, longname, DS_COMMON);
    CHECK_EQ_I(s.overflow, 0);
    CHECK_EQ_I(ds_claim(&s, longname, DS_COMMON, 1), 0);
    CHECK_EQ_I(ds_sweep_bits(&s.e[0]), 0);   /* FIXED: kept */

    /* OLD: a 95-char record never matches the full name it is compared with */
    CHECK(!ds_ieq(trunc95, longname), "a truncated record cannot match");
}

TEST(what_was_not_sampled_is_never_swept_or_counted)
{
    static ds_set_t s;
    char nm[32];
    int k;

    ds_reset(&s);
    for (k = 0; k < DS_MAX + 5; k++) {
        snprintf(nm, sizeof(nm), "icon%03d.lnk", k);
        ds_add(&s, nm, DS_COMMON);
    }
    CHECK_EQ_I(s.n, DS_MAX);
    CHECK_EQ_I(s.overflow, 1);
    /* one of the unsampled five, rewritten: not invented as "new" */
    snprintf(nm, sizeof(nm), "icon%03d.lnk", DS_MAX + 2);
    CHECK_EQ_I(ds_claim(&s, nm, DS_COMMON, 1), 0);
    CHECK_EQ_I(s.added, 0);
    /* a confirmed-but-not-written icon never counts either */
    ds_reset(&s);
    CHECK_EQ_I(ds_claim(&s, "Retro Agent.lnk", DS_COMMON, 0), 0);
    CHECK_EQ_I(s.added, 0);
}

TEST(only_shortcut_files_are_candidates)
{
    CHECK(ds_is_sweepable("Quake.lnk"), ".lnk");
    CHECK(ds_is_sweepable("DOOM.PIF"), ".pif, any case");
    CHECK(ds_is_sweepable("Offer.Url"), ".url, any case");
    CHECK(!ds_is_sweepable("notes.txt"), "a real file");
    CHECK(!ds_is_sweepable("game.exe"), "an exe someone left there");
    CHECK(!ds_is_sweepable("readme.lnk.txt"), "only the LAST extension counts");
    CHECK(!ds_is_sweepable("lnk"), "no extension at all");
}

/* ---------------------------------------------------------------------- */
/* gsstall.h: is it moving, and if not, is it starved?                      */
/* ---------------------------------------------------------------------- */

/* The old code had no such measurement: the status said "enumerating library"
 * for 100 minutes whatever was happening. Modelled as "always OK". */
static int verdict_OLD_SILENT(void) { return GSST_OK; }

TEST(busy_percent_arithmetic)
{
    CHECK_EQ_I(gsst_busy_pct(0, 100, 0), 100);    /* no idle at all */
    CHECK_EQ_I(gsst_busy_pct(100, 100, 0), 0);    /* kernel time is all idle */
    CHECK_EQ_I(gsst_busy_pct(4, 50, 50), 96);     /* .110: ioquake3 at 96% */
    CHECK_EQ_I(gsst_busy_pct(0, 0, 0), -1);       /* no interval: unknown */
    CHECK_EQ_I(gsst_busy_pct(500, 100, 0), 0);    /* never negative */
}

TEST(a_healthy_walk_is_never_reported)
{
    gsst_t s;
    unsigned long now = 1000, lost, span;
    int k;
    gsst_reset(&s, now);
    for (k = 0; k < 20000; k++) {           /* 20 ms between files, 400 s */
        now += 20;
        gsst_note_gap(&s, now, 20, 99);
    }
    CHECK_EQ_I(gsst_verdict(&s, now, &lost, &span), GSST_OK);
    CHECK_EQ_U(s.total_stall_ms, 0);
}

TEST(the_110_run_is_reported_as_starved)
{
    /* NT's balance-set manager gives a starved thread a quantum every ~4 s, so
     * the worker crept forward in 4-6 s steps with the CPU saturated. */
    gsst_t s;
    unsigned long now = 5000, lost = 0, span = 0;
    int k;
    gsst_reset(&s, now);
    for (k = 0; k < 1200; k++) {            /* ~100 minutes */
        unsigned long gap = 4000 + (unsigned long)(k % 3) * 1000;
        now += gap;
        gsst_note_gap(&s, now, gap, 97);
    }
    CHECK_EQ_I(gsst_verdict(&s, now, &lost, &span), GSST_STARVED);
    CHECK(lost >= GSST_REPORT_MS, "at least a minute lost");
    CHECK(lost * 2 >= span, "most of the recent window lost");
    CHECK(s.total_starved_ms > 90UL * 60 * 1000, "the whole run was starved");
    CHECK(verdict_OLD_SILENT() != GSST_STARVED, "the old status said nothing");
}

TEST(a_slow_share_is_a_stall_not_starvation)
{
    gsst_t s;
    unsigned long now = 0;
    int k;
    gsst_reset(&s, now);
    for (k = 0; k < 20; k++) {              /* 10 s per listing, CPU idle */
        now += 10000;
        gsst_note_gap(&s, now, 10000, 6);
    }
    CHECK_EQ_I(gsst_verdict(&s, now, NULL, NULL), GSST_SLOW);
    CHECK_EQ_U(s.total_starved_ms, 0);

    /* With no CPU measurement (9x, 2000) a stall is never called starvation */
    gsst_reset(&s, 0);
    now = 0;
    for (k = 0; k < 20; k++) {
        now += 10000;
        gsst_note_gap(&s, now, 10000, -1);
    }
    CHECK_EQ_I(gsst_verdict(&s, now, NULL, NULL), GSST_SLOW);
}

TEST(it_speaks_about_the_recent_window_not_the_whole_run)
{
    gsst_t s;
    unsigned long now = 0;
    int k;

    /* Four hours of healthy copying with one minute of hiccups spread through
     * it: nothing to report. */
    gsst_reset(&s, now);
    for (k = 0; k < 20; k++) {
        now += 12UL * 60 * 1000;
        gsst_note_gap(&s, now, 3000, 99);
    }
    CHECK_EQ_I(gsst_verdict(&s, now, NULL, NULL), GSST_OK);

    /* ...and then a game starts: six minutes starved is reported NOW, even
     * though the run as a whole is mostly healthy. */
    for (k = 0; k < 72; k++) {
        now += 5000;
        gsst_note_gap(&s, now, 5000, 98);
    }
    CHECK_EQ_I(gsst_verdict(&s, now, NULL, NULL), GSST_STARVED);
}

TEST(one_gap_longer_than_the_window_is_still_reported)
{
    gsst_t s;
    unsigned long lost = 0, span = 0;
    gsst_reset(&s, 0);
    gsst_note_gap(&s, 20UL * 60 * 1000, 20UL * 60 * 1000, 99);
    CHECK_EQ_I(gsst_verdict(&s, 20UL * 60 * 1000, &lost, &span), GSST_STARVED);
    CHECK(span >= lost, "the span is never shorter than what was lost");
}

MUNIT_MAIN("gamesync: sweep the desktop LAST; say when the run is starved",
    RUN(during_the_sizing_phase_the_desktop_keeps_every_icon);
    RUN(the_end_state_is_the_one_the_old_order_produced);
    RUN(a_run_that_does_not_finish_removes_nothing);
    RUN(the_rebuild_gate_still_reads_zero_on_an_unchanged_desktop);
    RUN(a_genuine_addition_or_removal_still_counts);
    RUN(a_duplicate_on_the_user_desktop_goes_without_counting_a_change);
    RUN(a_long_name_is_matched_whole);
    RUN(what_was_not_sampled_is_never_swept_or_counted);
    RUN(only_shortcut_files_are_candidates);
    RUN(busy_percent_arithmetic);
    RUN(a_healthy_walk_is_never_reported);
    RUN(the_110_run_is_reported_as_starved);
    RUN(a_slow_share_is_a_stall_not_starvation);
    RUN(it_speaks_about_the_recent_window_not_the_whole_run);
    RUN(one_gap_longer_than_the_window_is_still_reported);
)
