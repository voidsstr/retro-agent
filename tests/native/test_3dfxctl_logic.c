/* test_3dfxctl_logic.c
 *
 * The 3dfx Control Panel's decisions, TRUE SOURCE: this file includes
 * scripts/3dfx/3dfxctl/ctl_logic.h - the settings table and every function the
 * panel (3dfxctl.c) calls to decide what to offer and what to write. Clean-room
 * lane (vcr-kmd + our h5 Glide + our MesaFX ICD), 2026-09-28.
 *
 * What is pinned, each with the value the panel must NOT produce as well:
 *  1. ANTI-ALIASING NEVER ARMS ITSELF. No preset selects an AA configuration,
 *     none touches the kernel AA switch; an AA mode is not even listed until
 *     the person allowed experimental modes; an AA write without the
 *     confirmation is refused before anything is written; choosing a non-AA
 *     mode disarms Diag\SliAA (a present switch is deleted, never left armed
 *     behind SLI).
 *  2. On the V5 6000 (4 chips) cfg 1 is never offered (our guarded Glide
 *     refuses it; an unguarded one froze .124), nor 3/4 (the ICD would render
 *     without AA while Glide forces it) - AA is 6/7/8 there, the values Glide
 *     and the ICD agree on. 0 and 5 are the validated modes.
 *  3. The kernel switches the panel may write are exactly the four read
 *     without a reboot; every diagnostic / boot-time switch is refused.
 *  4. Refresh rates: only rates the driver enumerates, 0/1 Hz (the driver's
 *     "default" sentinels) never; a mode not in the list is not "listed".
 *  5. The Glide registry key is chosen exactly as minihwc.c getRegPath does.
 *  6. Float values: the stack default is written as ABSENT (deleted), not as
 *     a number; values are clamped, never wrapped.
 *  7. THE GRAPHICS CLOCK (2026-09-29, the Clock tab, vcr-kmd VCR_ESC_CLOCK):
 *     an overclock asks to be kept, stock and below never do; the panel's
 *     range is 133-200 MHz; and a saved clock is re-applied at logon ONLY
 *     when Windows' ShutdownTime stamp moved since it was set - a crash, a
 *     missing stamp or an out-of-range value never re-applies it.
 */
#include <stdio.h>
#include <string.h>
#include "munit.h"
#include "../../scripts/3dfx/3dfxctl/ctl_logic.h"

static int in(const long *v, int n, long x)
{
    int i;
    for (i = 0; i < n; i++)
        if (v[i] == x)
            return 1;
    return 0;
}

TEST(t_no_preset_ever_selects_aa)
{
    int p, boards[] = { 0, 1, 2, 4 }, b;
    const ctl_preset_row *r;
    char buf[8];
    const char *v;
    for (p = 0; p < CTL_NPRESETS; p++)
        for (b = 0; b < 4; b++) {
            int got = ctl_preset_value(p, CTL_ID_AA, (unsigned)boards[b], &v, buf, sizeof buf);
            if (got == CTL_PRESET_SET && v)
                CHECK(!ctl_aa_is_aa(atol(v)), "a preset selected an anti-aliasing mode");
        }
    /* the presets name no kernel AA switch and no AA value anywhere */
    for (r = ctl_presets; r->id; r++)
        for (p = 0; p < CTL_NPRESETS; p++) {
            const char *x = r->v[p];
            if (x == CTL_KEEP || x == CTL_SLI || x == CTL_ABSENT)
                continue;
            if (r->id == CTL_ID_AA)
                CHECK(!ctl_aa_is_aa(atol(x)), "a preset's literal AA value turns AA on");
        }
    /* "all chips in SLI" is 5 on a V5 6000, 2 on a 5500, 0 on one chip */
    CHECK_EQ_I(ctl_preset_value(CTL_PRESET_QUALITY, CTL_ID_AA, 4, &v, buf, sizeof buf),
               CTL_PRESET_SET);
    CHECK(v && strcmp(v, "5") == 0, "quality preset on 4 chips is not cfg 5");
    CHECK_EQ_I(ctl_preset_value(CTL_PRESET_SPEED, CTL_ID_AA, 2, &v, buf, sizeof buf),
               CTL_PRESET_SET);
    CHECK(v && strcmp(v, "2") == 0, "speed preset on 2 chips is not cfg 2");
    /* unknown board: the preset leaves the chip mode alone rather than guess */
    CHECK_EQ_I(ctl_preset_value(CTL_PRESET_SPEED, CTL_ID_AA, 0, &v, buf, sizeof buf),
               CTL_PRESET_KEEP);
    /* defaults delete the value (Glide's own default, 2 = SLI) */
    CHECK_EQ_I(ctl_preset_value(CTL_PRESET_DEFAULTS, CTL_ID_AA, 4, &v, buf, sizeof buf),
               CTL_PRESET_SET);
    CHECK(v == CTL_ABSENT, "defaults preset does not delete the AA value");
    /* the desktop refresh is never touched by a preset */
    for (p = 0; p < CTL_NPRESETS; p++)
        CHECK_EQ_I(ctl_preset_value(p, CTL_ID_DESK_REFRESH, 4, &v, buf, sizeof buf),
                   CTL_PRESET_KEEP);
}

TEST(t_aa_modes_are_listed_only_when_allowed)
{
    long out[16];
    int n, i;
    ctl_aa_info a;
    n = ctl_aa_choices(4, 0, out, 16);
    CHECK_EQ_I(n, 2);
    CHECK(in(out, n, 5) && in(out, n, 0), "4 chips, AA not allowed: not exactly {5, 0}");
    for (i = 0; i < n; i++)
        CHECK(!ctl_aa_is_aa(out[i]), "an AA mode is listed without the permission");
    n = ctl_aa_choices(4, 1, out, 16);
    CHECK_EQ_I(n, 5);
    CHECK(in(out, n, 6) && in(out, n, 7) && in(out, n, 8), "4 chips: AA must be 6/7/8");
    CHECK(!in(out, n, 1), "cfg 1 offered on a 4-chip board (froze .124)");
    CHECK(!in(out, n, 3) && !in(out, n, 4), "cfg 3/4 offered on 4 chips (ICD/Glide disagree)");
    CHECK(!in(out, n, 2), "cfg 2 listed beside 5 on 4 chips (the same SLI twice)");
    for (i = 0; i < n; i++) {
        ctl_aa_describe(out[i], 4, &a);
        if (ctl_aa_is_aa(out[i]))
            CHECK_EQ_I(a.status, CTL_AA_EXPERIMENTAL);
    }
    ctl_aa_describe(0, 4, &a);
    CHECK_EQ_I(a.status, CTL_AA_VALIDATED);
    ctl_aa_describe(5, 4, &a);
    CHECK_EQ_I(a.status, CTL_AA_VALIDATED);
    ctl_aa_describe(1, 4, &a);
    CHECK_EQ_I(a.status, CTL_AA_NOT_OFFERED);
    ctl_aa_describe(6, 4, &a);
    CHECK(strstr(a.evidence, "ghost") != NULL, "cfg 6's evidence does not say ghost image");
    ctl_aa_describe(7, 4, &a);
    CHECK(strstr(a.evidence, "FROZE") != NULL, "cfg 7's evidence does not say it froze");
    /* two chips: 3/4 are the AA values, 6/7/8 are not */
    n = ctl_aa_choices(2, 1, out, 16);
    CHECK(in(out, n, 3) && in(out, n, 4) && !in(out, n, 6) && !in(out, n, 7) && !in(out, n, 8),
          "2 chips: AA must be 3/4");
    /* one chip: only 1 */
    n = ctl_aa_choices(1, 1, out, 16);
    CHECK(n == 2 && in(out, n, 0) && in(out, n, 1), "1 chip: {0, 1}");
    /* board size unknown: nothing at all is offered */
    CHECK_EQ_I(ctl_aa_choices(0, 1, out, 16), 0);
    /* the AA set is Glide's own table (h5SliAaConfigEnv: aaSample > 0) */
    for (i = 0; i <= 9; i++)
        CHECK_EQ_I(ctl_aa_is_aa(i), i == 1 || i == 3 || i == 4 || i == 6 || i == 7 || i == 8);
}

TEST(t_aa_writes_need_the_confirmation_and_disarm_after)
{
    ctl_aa_plan p;
    /* AA without the confirmation: refused, NOTHING to write */
    ctl_aa_make_plan(4, 6, 0, 0, &p);
    CHECK(p.refused != NULL, "AA without confirmation was not refused");
    CHECK_EQ_I(p.write_cfg, 0);
    CHECK_EQ_I(p.sliaa, 0);
    /* with it: the configuration AND the kernel switch */
    ctl_aa_make_plan(4, 7, 1, 0, &p);
    CHECK(p.refused == NULL, "confirmed AA refused");
    CHECK(p.write_cfg && strcmp(p.cfg_value, "7") == 0, "cfg 7 not written as \"7\"");
    CHECK_EQ_I(p.sliaa, 1);
    /* back to SLI: the switch is deleted */
    ctl_aa_make_plan(4, 5, 0, 1, &p);
    CHECK(p.refused == NULL && strcmp(p.cfg_value, "5") == 0, "SLI not written");
    CHECK_EQ_I(p.sliaa, -1);
    /* ... and left alone when it was not there */
    ctl_aa_make_plan(4, 0, 0, 0, &p);
    CHECK_EQ_I(p.sliaa, 0);
    /* the driver default deletes the value, and disarms */
    ctl_aa_make_plan(4, -1, 0, 1, &p);
    CHECK(p.write_cfg && p.cfg_absent && p.sliaa == -1, "default does not delete + disarm");
    /* a value not offered for the board is refused even with the confirmation */
    ctl_aa_make_plan(4, 1, 1, 0, &p);
    CHECK(p.refused != NULL && !p.write_cfg, "cfg 1 on 4 chips accepted");
    ctl_aa_make_plan(4, 3, 1, 0, &p);
    CHECK(p.refused != NULL, "cfg 3 on 4 chips accepted");
    ctl_aa_make_plan(0, 5, 1, 0, &p);
    CHECK(p.refused != NULL, "a chip mode accepted on a board of unknown size");
}

TEST(t_only_the_live_kernel_switches_are_writable)
{
    static const char *const ok[] = { "SliAA", "Accel2DText", "Accel2DPattern", "Accel2DLine" };
    /* every Diag value vcr-kmd reads that is NOT for the panel (README "Diag
     * switches" and miniport VcrDiagGet): boot-time, diagnostic or dangerous */
    static const char *const never[] = {
        "AllowPoke", "SliPersistAll", "SliAAVendorRecipe", "SliAAReadback", "D3D32", "Reset3D",
        "FlipDeadline", "Disable", "Accel2D", "D3D", "TexPortFlush", "DebugPort", "DbgPrint",
        "LogLevel", "LogEntries", "EdidFilter", "Ddc", "MonTrustEnvelope", "MonReset", "MonId",
        "MonHminKhz", "MonHmaxKhz", "MonVminHz", "MonVmaxHz", "MonMaxPixclkKhz",
        "MaxBootAttempts", "BootAttempts", "BootCount", "HwCursor", "Sli", "Sli6kClock",
        "FixPciDecode", "PciLowThresh", "DesktopOffset", "MaxPixclkKhz", "TwoXAboveKhz",
        "NapalmVpcExtra", "LfbMemoryConfig", "OpenGLName", "OpenGLVersion", "PhaseLog",
        "LastPhase", "sliaa", "SLIAA", "", NULL };
    unsigned i;
    for (i = 0; i < 4; i++)
        CHECK(ctl_diag_name_ok(ok[i]), "a live switch is not writable");
    for (i = 0; never[i]; i++)
        CHECK(!ctl_diag_name_ok(never[i]), "a boot-time / diagnostic Diag name is writable");
    CHECK(!ctl_diag_name_ok(NULL), "NULL name accepted");
    /* and every Diag row of the table is one of the four */
    {
        const ctl_row *r;
        for (r = ctl_rows; r->name; r++)
            if (r->store == CTL_ST_DIAG)
                CHECK(ctl_diag_name_ok(r->name), "a Diag row names a switch the writer refuses");
    }
}

TEST(t_refresh_rates_come_only_from_the_driver)
{
    static const ctl_mode m[] = {
        { 1024, 768, 32, 85 }, { 1024, 768, 32, 60 }, { 1024, 768, 32, 75 },
        { 1024, 768, 32, 85 }, { 1024, 768, 32, 1 }, { 1024, 768, 32, 0 },
        { 1024, 768, 16, 100 }, { 1600, 1200, 32, 60 }, { 640, 480, 32, 120 },
        { 320, 240, 16, 160 }, { 1280, 1024, 8, 72 } };
    unsigned hz[16];
    ctl_rate_span sp[16];
    int n = ctl_desk_rates(m, 11, 1024, 768, 32, hz, 16), i;
    CHECK_EQ_I(n, 3);                           /* 85, 75, 60 - no dup, no 0/1 */
    CHECK(hz[0] == 85 && hz[1] == 75 && hz[2] == 60, "desk rates not highest first");
    CHECK(ctl_mode_listed(m, 11, 1024, 768, 32, 75), "a listed mode not found");
    CHECK(!ctl_mode_listed(m, 11, 1024, 768, 32, 100), "100 Hz is listed only at 16 bpp");
    CHECK(!ctl_mode_listed(m, 11, 1024, 768, 32, 1), "the 1 Hz sentinel counted as a mode");
    CHECK(!ctl_mode_listed(m, 11, 1024, 768, 32, 0), "the 0 Hz sentinel counted as a mode");
    n = ctl_glide_rates(m, 11, sp, 16);
    for (i = 0; i < n; i++) {
        CHECK(sp[i].hz > 1, "a sentinel offered as a Glide rate");
        CHECK(sp[i].hz != 160, "a 320x240 rate offered (below Glide's 512x384)");
        CHECK(sp[i].hz != 72, "an 8-bit mode's rate offered");
    }
    CHECK_EQ_I(n, 5);                           /* 120, 100, 85, 75, 60 */
    CHECK(sp[0].hz == 120 && sp[n - 1].hz == 60, "Glide rates not highest first");
    for (i = 0; i < n; i++)
        if (sp[i].hz == 60)
            CHECK(sp[i].minw == 1024 && sp[i].maxw == 1600, "60 Hz span wrong");
    CHECK_EQ_I(ctl_desk_rates(m, 11, 800, 600, 32, hz, 16), 0);
}

/* The 2D switches are read at a NEW display surface; XP's same-mode
 * ChangeDisplaySettingsEx(CDS_RESET) is only DrvAssertMode(FALSE/TRUE) on the
 * same PDEV (vcr-kmd recorder, QEMU test bed, 2026-09-28), so the panel passes
 * through another LISTED mode of the same resolution and back. */
TEST(t_the_pass_through_mode_is_listed_and_keeps_the_resolution)
{
    static const ctl_mode m[] = {
        { 1024, 768, 32, 85 }, { 1024, 768, 32, 75 }, { 1024, 768, 32, 60 },
        { 1024, 768, 32, 1 }, { 1024, 768, 16, 85 }, { 800, 600, 32, 85 },
        { 1280, 1024, 32, 60 }, { 640, 480, 32, 60 }, { 640, 480, 16, 72 },
        { 1600, 1200, 16, 60 }, { 1600, 1200, 8, 60 } };
    ctl_mode cur, out;
    int n = (int)(sizeof m / sizeof m[0]);
    /* nearest LOWER rate at the same resolution and depth */
    cur.w = 1024; cur.h = 768; cur.bpp = 32; cur.hz = 85;
    CHECK(ctl_bounce_mode(m, n, &cur, &out), "no pass-through for 1024x768x32@85");
    CHECK(out.w == 1024 && out.h == 768 && out.bpp == 32 && out.hz == 75, "not 75 Hz");
    /* the lowest rate: the nearest HIGHER one - never the 1 Hz sentinel */
    cur.hz = 60;
    CHECK(ctl_bounce_mode(m, n, &cur, &out) && out.hz == 75 && out.bpp == 32,
          "60 Hz did not pass through 75 Hz");
    /* one rate only: the other depth at the same rate */
    cur.w = 640; cur.h = 480; cur.bpp = 32; cur.hz = 60;
    CHECK(ctl_bounce_mode(m, n, &cur, &out), "no pass-through for 640x480x32@60");
    CHECK(out.w == 640 && out.h == 480 && out.bpp == 16 && out.hz == 72,
          "640x480: not the 16-bit mode (72 Hz, the only one listed)");
    cur.w = 1600; cur.h = 1200; cur.bpp = 16; cur.hz = 60;   /* 8 bpp is not a way through */
    CHECK(!ctl_bounce_mode(m, n, &cur, &out), "1600x1200x16: passed through 8 bpp");
    /* nothing else at this resolution: no switch at all */
    cur.w = 1280; cur.h = 1024; cur.bpp = 32; cur.hz = 60;
    CHECK(!ctl_bounce_mode(m, n, &cur, &out), "1280x1024: invented a mode");
    cur.w = 800; cur.h = 600; cur.bpp = 32; cur.hz = 85;
    CHECK(!ctl_bounce_mode(m, n, &cur, &out), "800x600: invented a mode");
    /* whatever it picks is in the list */
    cur.w = 1024; cur.h = 768; cur.bpp = 32; cur.hz = 75;
    CHECK(ctl_bounce_mode(m, n, &cur, &out) &&
          ctl_mode_listed(m, n, out.w, out.h, out.bpp, out.hz), "picked an unlisted mode");
}

TEST(t_a_preset_leaves_an_equivalent_chip_mode_alone)
{
    /* four chips: absent (Glide's 2), 2 and 5 are all "every chip in SLI" */
    CHECK(ctl_aa_same_mode(0, 0, 1, 5, 4), "absent vs 5 on 4 chips");
    CHECK(ctl_aa_same_mode(1, 2, 1, 5, 4), "2 vs 5 on 4 chips");
    CHECK(!ctl_aa_same_mode(1, 0, 1, 5, 4), "single chip vs SLI on 4 chips");
    CHECK(!ctl_aa_same_mode(1, 6, 1, 5, 4), "AA vs SLI");
    CHECK(ctl_aa_same_mode(1, 7, 1, 7, 4), "a value is itself");
    CHECK(!ctl_aa_same_mode(1, 6, 1, 3, 4), "two AA values are not interchangeable");
    /* one chip: absent, 0, 2, 5 all mean the one chip */
    CHECK(ctl_aa_same_mode(0, 0, 1, 0, 1), "absent vs 0 on 1 chip");
    CHECK(ctl_aa_same_mode(1, 5, 1, 0, 1), "5 vs 0 on 1 chip");
    CHECK(!ctl_aa_same_mode(1, 1, 1, 0, 1), "AA vs none on 1 chip");
}

TEST(t_the_glide_key_follows_getregpath)
{
    CHECK(strcmp(ctl_glide_regpath(1),
                 "SYSTEM\\CurrentControlSet\\Services\\3dfxvs\\Device0\\glide") == 0,
          "3dfxvs\\Device0 present: wrong key");
    CHECK(strcmp(ctl_glide_regpath(0),
                 "SYSTEM\\CurrentControlSet\\Services\\banshee\\Device0\\glide") == 0,
          "3dfxvs\\Device0 absent: wrong key");
    CHECK(strcmp(CTL_KEY_DIAG, "SYSTEM\\CurrentControlSet\\Services\\vcrmp\\Diag") == 0,
          "wrong Diag key");
}

TEST(t_floats_default_to_absent_and_clamp)
{
    char b[16];
    float f = 0;
    CHECK(ctl_float_value(1.30f, 1.30f, b) == CTL_ABSENT, "the default was written as a number");
    CHECK(ctl_float_value(1.304f, 1.30f, b) == CTL_ABSENT, "within half a step is the default");
    CHECK(strcmp(ctl_float_value(1.00f, 1.30f, b), "1.00") == 0, "1.00 not written");
    CHECK(strcmp(ctl_float_value(2.5f, 1.0f, b), "2.50") == 0, "2.50 not written");
    CHECK(strcmp(ctl_float_value(12.0f, 1.0f, b), "9.99") == 0, "not clamped at 9.99");
    CHECK(strcmp(ctl_float_value(-3.0f, 1.0f, b), "0.00") == 0, "not clamped at 0");
    CHECK(ctl_parse_float("1.30", &f) && f > 1.29f && f < 1.31f, "1.30 not parsed");
    CHECK(ctl_parse_float("-0.5", &f) && f < -0.49f, "-0.5 not parsed");
    CHECK(!ctl_parse_float("1.3x", &f), "junk accepted");
    CHECK(!ctl_parse_float("", &f), "empty accepted");
    CHECK(!ctl_parse_float(".", &f), "a bare dot accepted");
}

TEST(t_table_is_well_formed)
{
    const ctl_row *r;
    int ids[256], n = 0, i;
    for (r = ctl_rows; r->name; r++) {
        for (i = 0; i < n; i++)
            CHECK(ids[i] != r->id, "duplicate row id");
        if (n < 256)
            ids[n++] = r->id;
        CHECK(r->lanes == CTL_LANE_VCR || r->lanes == CTL_LANE_VINTAGE, "row lane");
        CHECK(r->label && r->help && r->group, "row text missing");
        if (r->lanes == CTL_LANE_VCR) {
            /* our lane writes only its own stores */
            CHECK(r->store == CTL_ST_GLIDE || r->store == CTL_ST_ENV || r->store == CTL_ST_DIAG ||
                  r->store == CTL_ST_DISPLAY, "an our-stack row writes a vintage key");
            CHECK(r->when != CTL_WHEN_REBOOT, "an our-stack row needs a reboot");
            if (r->store == CTL_ST_ENV)
                CHECK_EQ_I(r->when, CTL_WHEN_EXPLORER);
            if (r->store == CTL_ST_DIAG || r->store == CTL_ST_DISPLAY)
                CHECK_EQ_I(r->when, CTL_WHEN_NOW);
            /* a kernel switch is experimental unless it is one of the two the
             * driver turned ON by default after silicon verification
             * (2026-09-28: pattern fills and lines, whose off switch is 0) */
            if (r->store == CTL_ST_DIAG && r->choices != ctl_c_2d_on)
                CHECK(r->flags & CTL_F_EXPERIMENTAL, "a kernel switch row not marked experimental");
            if (r->choices == ctl_c_2d_on) {
                CHECK(r->id == CTL_ID_2D_PAT || r->id == CTL_ID_2D_LINE, "default-on row");
                CHECK(r->choices[0].value == NULL && strcmp(r->choices[1].value, "0") == 0,
                      "default-on: absent = on, the off switch is 0");
            }
            CHECK(r->flags & (CTL_F_R_GLIDE | CTL_F_R_ICD | CTL_F_R_KERNEL) ||
                  r->store == CTL_ST_DISPLAY, "an our-stack row names no reader");
        }
        if (r->kind == CTL_K_CHECK) {
            CHECK(r->choices && r->choices[0].label && r->choices[1].label &&
                  !r->choices[2].label, "a checkbox needs exactly off/on");
            if (r->lanes == CTL_LANE_VCR)
                CHECK(r->choices[0].value == NULL, "a checkbox's off is not the default");
        }
        if (r->kind == CTL_K_CHOICE && r->lanes == CTL_LANE_VCR)
            CHECK(r->choices && r->choices[0].value == NULL,
                  "an our-stack choice's first item is not the stack default");
        if (r->kind == CTL_K_FLOAT)
            CHECK(r->fmin < r->fdef && r->fdef < r->fmax, "a float default outside its range");
        /* the AA row is the only way to write the SLI/AA value */
        if (strcmp(r->name, "SSTH3_SLI_AA_CONFIGURATION") == 0 && r->lanes == CTL_LANE_VCR)
            CHECK_EQ_I(r->kind, CTL_K_AA);
    }
    CHECK(n >= 17, "the table lost rows");
}

/* 2026-09-28, .124: the Display & 2D tab showed "Pattern fills" and "Straight
 * lines" UNTICKED while the driver reported both ON - the box was ticked only
 * for choices[1], which on a default-ON row is the OFF value "0". */
TEST(t_a_default_on_checkbox_is_ticked_when_absent)
{
    int ci = ctl_check_index(ctl_c_2d_on);
    CHECK(ci == 0, "the tick of a default-on row is choices[0]");
    CHECK(ctl_value_matches(ctl_c_2d_on[ci].value, 0, ""), "absent (on) shows ticked");
    CHECK(!ctl_value_matches(ctl_c_2d_on[ci].value, 1, "0"), "an explicit 0 (off) shows unticked");
    CHECK(ctl_c_2d_on[1 - ci].value && strcmp(ctl_c_2d_on[1 - ci].value, "0") == 0,
          "unticking writes 0");
    CHECK(ctl_c_2d_on[ci].value == NULL, "ticking clears the value (absent = on)");
    /* the old rule, on the same state, was wrong both ways */
    CHECK(!ctl_value_matches(ctl_c_2d_on[1].value, 0, ""), "old rule: absent showed unticked");
    /* an ordinary off-by-default row is unchanged */
    CHECK(ctl_check_index(ctl_c_2d) == 1, "text on the engine: the tick is choices[1]");
    CHECK(!ctl_value_matches(ctl_c_2d[1].value, 0, ""), "absent (off) shows unticked");
    CHECK(ctl_value_matches(ctl_c_2d[1].value, 1, "1"), "1 shows ticked");
}

/* ---- 7. the graphics clock ------------------------------------------------------------ */

/* .124's VBIOS clock: pllCtrl1 0xE721 = 166.806 MHz (measured 2026-09-29) */
#define BOOT 166806u

TEST(t_an_overclock_asks_to_be_kept_stock_and_below_never)
{
    CHECK_EQ_I(ctl_clock_tier(BOOT, BOOT), CTL_CLK_STOCK);
    CHECK_EQ_I(ctl_clock_tier(167000u, BOOT), CTL_CLK_STOCK);  /* the slider's 167 is stock */
    CHECK_EQ_I(ctl_clock_tier(149744u, BOOT), CTL_CLK_UNDER);  /* 150's real PLL clock */
    CHECK_EQ_I(ctl_clock_tier(175000u, BOOT), CTL_CLK_MILD);
    CHECK_EQ_I(ctl_clock_tier(183000u, BOOT), CTL_CLK_MILD);
    CHECK_EQ_I(ctl_clock_tier(190000u, BOOT), CTL_CLK_STRONG);
    CHECK(!ctl_clock_confirm(BOOT, BOOT), "stock does not ask");
    CHECK(!ctl_clock_confirm(150000u, BOOT), "an underclock does not ask");
    CHECK(ctl_clock_confirm(175000u, BOOT), "an overclock asks to be kept");
    CHECK(ctl_clock_confirm(200000u, BOOT), "a strong overclock asks to be kept");
    /* a driver that could not read the VBIOS word: the nominal 166 MHz is the reference */
    CHECK(ctl_clock_confirm(180000u, 0), "unknown stock: still asks above 166");
    CHECK(!ctl_clock_confirm(150000u, 0), "unknown stock: below 166 does not ask");
}

TEST(t_the_panel_offers_133_to_200_mhz)
{
    CHECK(!ctl_clock_mhz_ok(132) && ctl_clock_mhz_ok(133), "the floor");
    CHECK(ctl_clock_mhz_ok(200) && !ctl_clock_mhz_ok(201), "the ceiling");
    CHECK(!ctl_clock_mhz_ok(219), "the driver's 219 is not offered");
    CHECK(!ctl_clock_mhz_ok(120), "the driver's 120 is not offered");
}

TEST(t_a_saved_clock_is_reapplied_only_after_a_clean_shutdown)
{
    static const unsigned char set_at[8] = { 0x84, 0xFE, 0x50, 0x1D, 0xCE, 0x4F, 0xDD, 0x01 };
    static const unsigned char later[8]  = { 0xC4, 0x45, 0x2B, 0x90, 0xCF, 0x4F, 0xDD, 0x01 };
    static const unsigned char earlier[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x01 };
    /* the two stamps .124 wrote at two agent reboots, 04:50:56 and 05:01:18 UTC */
    CHECK_EQ_I(ctl_clock_startup(175000u, 0, later, 8, set_at, 8), CTL_CLK_START_APPLY);
    /* the stamp did not move: the session that ran the clock never shut down cleanly */
    CHECK_EQ_I(ctl_clock_startup(175000u, 0, set_at, 8, set_at, 8), CTL_CLK_START_UNCLEAN);
    /* equality only - an OLDER stamp (the RTC reset after a power loss) still moved */
    CHECK_EQ_I(ctl_clock_startup(175000u, 0, earlier, 8, set_at, 8), CTL_CLK_START_APPLY);
    /* no stamp on either side is no proof */
    CHECK_EQ_I(ctl_clock_startup(175000u, 0, NULL, 0, set_at, 8), CTL_CLK_START_UNKNOWN);
    CHECK_EQ_I(ctl_clock_startup(175000u, 0, later, 8, NULL, 0), CTL_CLK_START_UNKNOWN);
    CHECK_EQ_I(ctl_clock_startup(175000u, 0, later, 4, set_at, 8), CTL_CLK_START_UNKNOWN);
    /* nothing saved, and a value nobody could have set */
    CHECK_EQ_I(ctl_clock_startup(0u, 0, later, 8, set_at, 8), CTL_CLK_START_NONE);
    CHECK_EQ_I(ctl_clock_startup(219000u, 0, later, 8, set_at, 8), CTL_CLK_START_RANGE);
    CHECK_EQ_I(ctl_clock_startup(120000u, 0, later, 8, set_at, 8), CTL_CLK_START_RANGE);
    CHECK_EQ_I(ctl_clock_startup(BOOT, 0, later, 8, set_at, 8), CTL_CLK_START_APPLY);
    /* A LOGON IS NOT A BOOT: the card still runs the saved clock (a logoff and
     * logon, the stamp unmoved) - nothing to do, and NOT an unclean shutdown;
     * the first build forgot the saved clock there and said the card was at stock */
    CHECK_EQ_I(ctl_clock_startup(175000u, 1, set_at, 8, set_at, 8), CTL_CLK_START_RUNNING);
    CHECK_EQ_I(ctl_clock_startup(175000u, 1, NULL, 0, NULL, 0), CTL_CLK_START_RUNNING);
    /* ... but a bad saved value is still refused first, and nothing saved is nothing */
    CHECK_EQ_I(ctl_clock_startup(219000u, 1, later, 8, set_at, 8), CTL_CLK_START_RANGE);
    CHECK_EQ_I(ctl_clock_startup(0u, 1, later, 8, set_at, 8), CTL_CLK_START_NONE);
}

MUNIT_MAIN("3dfxctl logic (the 3dfx Control Panel's decisions, true source)",
    RUN(t_no_preset_ever_selects_aa);
    RUN(t_aa_modes_are_listed_only_when_allowed);
    RUN(t_aa_writes_need_the_confirmation_and_disarm_after);
    RUN(t_only_the_live_kernel_switches_are_writable);
    RUN(t_refresh_rates_come_only_from_the_driver);
    RUN(t_the_pass_through_mode_is_listed_and_keeps_the_resolution);
    RUN(t_a_preset_leaves_an_equivalent_chip_mode_alone);
    RUN(t_the_glide_key_follows_getregpath);
    RUN(t_floats_default_to_absent_and_clamp);
    RUN(t_table_is_well_formed);
    RUN(t_a_default_on_checkbox_is_ticked_when_absent);
    RUN(t_an_overclock_asks_to_be_kept_stock_and_below_never);
    RUN(t_the_panel_offers_133_to_200_mhz);
    RUN(t_a_saved_clock_is_reapplied_only_after_a_clean_shutdown);
)
