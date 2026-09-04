/* test_gameres.c - TRUE-SOURCE: compiles agent/shared/gameres.h, the logic the
 * agent runs to decide what resolution each staged game gets on this box.
 *
 * WHAT THIS PINS, and why each one is here rather than trusted:
 *
 *  1. THE DEFECT THAT PROMPTED THE PASS. HalfLife1/install.reg pins the shared
 *     GoldSrc mode key to 1024x768 and gs_merge_reg() re-applies it on every
 *     sync, on every box. The rule table must expand to the PANEL's mode on a
 *     1080p machine, or the pass exists and changes nothing - which is this
 *     project's signature failure, a fix that reports success.
 *
 *  2. THE TWO MODE TABLES DIVERGE AT INDEX 8. id Tech 2's entry 8 is 1280x960
 *     (4:3); id Tech 3's is 1280x1024 (5:4). Handing a Quake III-family engine
 *     the id Tech 2 index asks a 16:9 panel for a squashed picture, measured on
 *     SoF2 on .123. Both selectors are asserted against the same target.
 *
 *  3. NO EDID MEANS A 4:3 TUBE, NOT "BELIEVE THE DESKTOP". .133 lost its EDID
 *     across a reboot and the old fallback instantly handed 1280x1024 back to a
 *     tube measured at 37x28 cm that morning. The test drives exactly that.
 *
 *  4. A MODE MUST BE ONE THE DRIVER OFFERS. On .246 an unofferable 1152x864
 *     made RTCW set the desktop to 1280x960 and then draw into a window with
 *     r_fullscreen still 1 - it neither errored nor did what was asked.
 *
 *  5. AN EMPTY MODE LIST MEANS "COULD NOT ASK", NOT "NOTHING IS OFFERED".
 *     .143's GeForce 6800 answers ENUM_CURRENT_SETTINGS and then returns FALSE
 *     at index 0. Treating that as a refusal would drive every title to the
 *     640x480 floor on exactly the boxes whose driver is least cooperative.
 *
 *  6. THE LIVE DESKTOP MODE IS NEVER AN INPUT. gr_decide() is not even given
 *     one - the signature is the assertion. A game that exits without restoring
 *     leaves the desktop at 640x480 (.123 and .240 were both found there), and
 *     a pass that WRITES that conclusion pins the box at 640x480 for good.
 */

#include "munit.h"
#include <string.h>
#include <stdlib.h>

#include "../../agent/shared/gameres.h"

/* Case-insensitive ".bat" suffix test. Windows filenames are case-insensitive
 * and this repo has paid for case-sensitive comparisons against them more than
 * once, so the check that keeps this table out of the launchers is written the
 * way the filesystem actually behaves. */
static int ends_with_bat(const char *s)
{
    size_t n = strlen(s);
    if (n < 4) return 0;
    s += n - 4;
    return s[0] == '.'
        && (s[1] == 'b' || s[1] == 'B')
        && (s[2] == 'a' || s[2] == 'A')
        && (s[3] == 't' || s[3] == 'T');
}

/* ------------------------------------------------------------------ */
/* Real fleet panels and the modes their drivers really enumerate.      */
/* ------------------------------------------------------------------ */

/* A 1080p LCD: 60 Hz everywhere, which is what a flat panel offers. */
static void modes_lcd1080(gr_modes_t *l)
{
    static const int t[][3] = {
        {640,480,60},{720,480,60},{800,600,60},{1024,768,60},{1152,864,60},
        {1280,720,60},{1280,960,60},{1280,1024,60},{1360,768,60},
        {1440,900,60},{1600,900,60},{1680,1050,60},{1920,1080,60}
    };
    size_t i;
    gr_modes_reset(l);
    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        gr_modes_add(l, t[i][0], t[i][1], t[i][2]);
}

/*
 * A CRT, where refresh is the whole point: the rate on offer FALLS as the
 * resolution rises, which is why "the monitor's highest refresh" is not one
 * number for the box. Each row is added several times, the way a driver
 * enumerates one entry per (resolution, depth, rate) - including the 0 Hz
 * "driver default" sentinel, which must never be chosen.
 */
static void modes_crt(gr_modes_t *l)
{
    static const int t[][3] = {
        {640,480,0},{640,480,60},{640,480,85},{640,480,120},
        {800,600,60},{800,600,85},{800,600,120},
        {1024,768,60},{1024,768,85},{1024,768,100},
        {1152,864,60},{1152,864,85},
        {1280,960,60},{1280,960,75},
        {1280,1024,60},{1280,1024,75},
        {1600,1200,60}
    };
    size_t i;
    gr_modes_reset(l);
    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        gr_modes_add(l, t[i][0], t[i][1], t[i][2]);
}

/* .123 / .145 / .240 / .246 - a 1920x1080 panel. The DELL P2312H does NOT set
 * the digital-input bit; it is caught by the refresh-range half of the test,
 * which is exactly why that half exists. */
static gr_panel_t panel_1080p(void)
{
    gr_panel_t p;
    memset(&p, 0, sizeof(p));
    p.ok = 1; p.native_w = 1920; p.native_h = 1080; p.native_hz = 60;
    p.digital = 0; p.vmax = 76; p.hcm = 51; p.vcm = 29;
    return p;
}

/* .171 - a Gateway VX1120, a 4:3 tube that quotes 75 Hz preferred and 160 Hz
 * maximum. Its desktop was persisted at 1280x1024, i.e. 5:4 on a 4:3 tube. */
static gr_panel_t panel_crt43(void)
{
    gr_panel_t p;
    memset(&p, 0, sizeof(p));
    p.ok = 1; p.native_w = 1920; p.native_h = 1440; p.native_hz = 75;
    p.digital = 0; p.vmax = 160; p.hcm = 36; p.vcm = 27;
    return p;
}

static gr_panel_t panel_none(void)
{
    gr_panel_t p;
    memset(&p, 0, sizeof(p));
    return p;
}

/* ------------------------------------------------------------------ */

TEST(t_lcd_gets_native)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    modes_lcd1080(&l);
    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);

    CHECK_EQ_I(t.lcd, 1);
    CHECK_EQ_I(t.w, 1920);
    CHECK_EQ_I(t.h, 1080);
    CHECK(strcmp(t.aspect, "16:9") == 0,
          "the target aspect should read 16:9");
    CHECK_EQ_I(t.wide, 1);
    CHECK_EQ_I(t.edid, 1);

    /* The 4:3-only engines get a correctly PROPORTIONED mode, never 1280x1024
     * and never a stretched 16:9 one. 1280x960 is the largest classic 4:3 mode
     * inside 1920x1080 that this driver offers. */
    CHECK_EQ_I(t.w43, 1280);
    CHECK_EQ_I(t.h43, 960);

    /* vert- correction: 106 at 16:9, so the 4:3 vertical field of view is
     * preserved rather than cropped. */
    CHECK_EQ_I(t.fov, 106);
    CHECK_EQ_I(t.d3ar, 1);            /* id Tech 4: 1 = 16:9 */
}

/* THE TABLES DIVERGE AT INDEX 8 AND THAT IS THE WHOLE POINT OF HAVING TWO. */
TEST(t_q2_q3_tables_differ)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    modes_lcd1080(&l);
    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);

    /* id Tech 2 index 8 IS 1280x960 - the same mode as the 4:3 target. */
    CHECK_EQ_I(t.q2mode, 8);
    CHECK_EQ_I(gr_q2tab[t.q2mode].w, 1280);
    CHECK_EQ_I(gr_q2tab[t.q2mode].h, 960);

    /* id Tech 3 index 8 is 1280x1024 - 5:4 - so the selector must NOT choose
     * it and lands on 7 = 1152x864 instead. Asserting the resolution and not
     * just the index, because the index alone would pass with either table. */
    CHECK_EQ_I(t.q3mode, 7);
    CHECK_EQ_I(gr_q3tab[t.q3mode].w, 1152);
    CHECK_EQ_I(gr_q3tab[t.q3mode].h, 864);
    CHECK_EQ_I(gr_q3tab[8].h, 1024);      /* the trap, still there */

    /* And the two indices are NOT interchangeable: handing FR_Q2MODE to an
     * id Tech 3 engine here would ask for 1280x1024 on a 16:9 panel. */
    CHECK_EQ_I(gr_q3tab[t.q2mode].h, 1024);
}

TEST(t_crt_never_5_4)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_crt43();
    modes_crt(&l);
    /* The desktop is PERSISTED at 1280x1024 - the wrong shape it was found in. */
    gr_decide(&pan, &l, 1280, 1024, 85, 32, 0, 0, &t);

    CHECK_EQ_I(t.lcd, 0);
    CHECK_EQ_I(t.w, 1280);
    CHECK_EQ_I(t.h, 960);             /* 4:3, not the 5:4 it was in */
    CHECK(strcmp(t.aspect, "4:3") == 0,
          "the target aspect should read 4:3");
    CHECK_EQ_I(t.fov, 90);
    CHECK_EQ_I(t.fr_hz, 85);          /* FLEETRES's FR_HZ, not a hardcoded 60 */
}

/* .133 lost its EDID across a reboot and was instantly handed 1280x1024 back. */
TEST(t_no_edid_assumes_4_3)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_none();
    modes_crt(&l);
    gr_decide(&pan, &l, 1280, 1024, 85, 32, 0, 0, &t);

    CHECK_EQ_I(t.edid, 0);            /* inferred, and it SAYS so */
    CHECK_EQ_I(t.lcd, 0);
    CHECK_EQ_I(t.w, 1280);
    CHECK_EQ_I(t.h, 960);
}

/* An unofferable mode is never chosen - on .246 that made RTCW draw into a
 * window with r_fullscreen still 1. */
TEST(t_only_offered_modes)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    modes_lcd1080(&l);

    /* Take 1152x864 away, as .246's driver does. */
    {
        gr_modes_t l2; int i;
        gr_modes_reset(&l2);
        for (i = 0; i < l.n; i++)
            if (!(l.m[i].w == 1152 && l.m[i].h == 864))
                gr_modes_add(&l2, l.m[i].w, l.m[i].h, l.m[i].hz);
        l = l2;
    }
    CHECK_EQ_I(gr_modes_have(&l, 1152, 864), 0);

    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);
    CHECK((gr_q3tab[t.q3mode].w) != (1152),
          "gr_q3tab[t.q3mode].w must not equal 1152");
    CHECK_EQ_I(gr_q3tab[t.q3mode].w, 1024);
    CHECK_EQ_I(t.w43, 1280);          /* 1152 is gone, 1280 remains */
}

/* A driver that enumerates nothing means "could not ask". */
TEST(t_empty_list_is_not_a_refusal)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_none();
    gr_modes_reset(&l);
    gr_modes_add(&l, 1024, 768, 0);             /* only the desktop is known */
    gr_decide(&pan, &l, 1024, 768, 100, 32, 0, 0, &t);

    CHECK_EQ_I(t.w, 1024);
    CHECK_EQ_I(t.h, 768);
    CHECK_EQ_I(t.w43, 1024);
    /* NOT the 640x480 floor: with an unusable list every table entry that
     * FITS is treated as available. */
    CHECK_EQ_I(gr_q2tab[t.q2mode].w, 1024);
    CHECK_EQ_I(gr_q3tab[t.q3mode].w, 1024);
    /* The driver told us the resolution but no rate for it, so the refresh is
     * NOT known: `hz` must be 0, meaning "leave it alone". Answering 100 here
     * by borrowing the persisted mode's rate would be claiming a measurement
     * that was never taken. FR_HZ still reports it, because "the mode it is
     * persisted at" IS a measurement - of the mode, not of the ceiling. */
    CHECK_EQ_I(t.hz, 0);
    CHECK_EQ_I(t.fr_hz, 100);
}

/* .171: an Intel 865G driving the desktop and a Voodoo 2 doing the 3D, with a
 * hard 800x600 ceiling that no display-class scan can see. */
TEST(t_per_box_cap)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_crt43();
    modes_crt(&l);
    gr_decide(&pan, &l, 1280, 1024, 75, 32, 800, 600, &t);

    CHECK_EQ_I(t.w, 800);
    CHECK_EQ_I(t.h, 600);
    CHECK_EQ_I(t.w43, 800);
    CHECK_EQ_I(gr_q3tab[t.q3mode].w, 800);
}

/* ------------------------------------------------------------------ */
/* substitution and the rule table                                      */
/* ------------------------------------------------------------------ */

TEST(t_expand)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    char out[256];
    modes_lcd1080(&l);
    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);

    CHECK_EQ_I(gr_expand("dword:%W%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "dword:1920") == 0,
          "expansion should produce dword:1920");
    CHECK_EQ_I(gr_expand("%W%x%H%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "1920x1080") == 0,
          "expansion should produce 1920x1080");
    CHECK_EQ_I(gr_expand("%W43%x%H43%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "1280x960") == 0,
          "expansion should produce 1280x960");

    /* DOSBox: `original` retargets the WHOLE DESKTOP to the DOS mode, which on
     * a 16:9 LCD is a stretched upscale left behind after a crash. */
    CHECK_EQ_I(gr_expand("%DOSFULLRES%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "desktop") == 0,
          "expansion should produce desktop");

    /* Turok 2 keeps one boolean per mode; exactly the 4:3 target is on. */
    CHECK_EQ_I(gr_expand("%SEL43:1280x960%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "1") == 0,
          "expansion should produce 1");
    CHECK_EQ_I(gr_expand("%SEL43:1024x768%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "0") == 0,
          "expansion should produce 0");

    /* An unknown token is left ALONE rather than silently becoming empty - a
     * value that quietly loses its number is how a config ends up saying
     * `r_customwidth ""`. */
    CHECK_EQ_I(gr_expand("%NOSUCH%", &t, out, sizeof(out)), 0);
    CHECK(strcmp(out, "%NOSUCH%") == 0,
          "expansion should produce %NOSUCH%");

    /* Overflow is an ERROR, never a truncation. */
    CHECK((gr_expand("%W%x%H%", &t, out, 4)) != (0),
          "gr_expand(%W%x%H%, &t, out, 4) must not equal 0");
}

TEST(t_rules_wellformed)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    int i;
    modes_lcd1080(&l);
    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);

    CHECK((GR_RULE_COUNT) > (0),
          "GR_RULE_COUNT must exceed 0");
    for (i = 0; i < GR_RULE_COUNT; i++) {
        const gr_rule_t *r = &gr_rules[i];
        char out[1024];

        CHECK((r->title) != NULL,
              "r->title must be present");
        CHECK((r->file) != NULL,
              "r->file must be present");
        CHECK((r->op) <= (GR_OP_CFG),
              "r->op must not exceed GR_OP_CFG");

        /* Every argument a rule carries must expand, and must FIT. A rule that
         * silently truncated would ship half a resolution. */
        if (r->arg1)
            CHECK_EQ_I(gr_expand(r->arg1, &t, out, sizeof(out)), 0);
        if (r->arg2)
            CHECK_EQ_I(gr_expand(r->arg2, &t, out, sizeof(out)), 0);
        if (r->arg3)
            CHECK_EQ_I(gr_expand(r->arg3, &t, out, sizeof(out)), 0);

        if (r->op == GR_OP_CFG) {
            const char *body = gr_cfg_body(r->arg1);
            CHECK((body) != NULL, "body must be present");        /* a named kind must exist */
            CHECK_EQ_I(gr_expand(body, &t, out, sizeof(out)), 0);
        }
        if (r->op == GR_OP_REG) {
            CHECK(strcmp(r->file, "HKLM") == 0
                           || strcmp(r->file, "HKCU") == 0, "must hold");
            CHECK((r->arg3) != NULL,
                  "r->arg3 must be present");
            /* A typo in the type prefix would make gr_w_reg refuse the write
             * and the value would never be set. */
            CHECK(strncmp(r->arg3, "dword:", 6) == 0
                           || strncmp(r->arg3, "sz:", 3) == 0, "must hold");
        } else {
            /* Never point a config rule at a launcher: those belong to
             * stage-fleetres.py, and two mechanisms owning one .bat has
             * already destroyed a generated disc-mount launcher once. */
            CHECK(!(ends_with_bat(r->file)),
                  "must not hold");
        }
    }
}

/*
 * THE DEFECT THIS PASS EXISTS FOR.
 *
 * HalfLife1/install.reg pins HKCU\Software\Valve\Half-Life\Settings
 * ScreenWidth=0x400 (1024), and gs_merge_reg() re-applies it on every sync on
 * every box. That one key is the mode for every GoldSrc title on the machine -
 * there is no Software\Valve\CounterStrike key at all - and Counter-Strike
 * cannot override it from its command line. So on a 1080p panel the rule must
 * expand to 1920x1080, or the pass runs, logs success and changes nothing.
 */
TEST(t_goldsrc_reaches_the_panel)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    int i, w = 0, h = 0, engw = 0, engh = 0, shared_key = 0;
    modes_lcd1080(&l);
    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);

    for (i = 0; i < GR_RULE_COUNT; i++) {
        const gr_rule_t *r = &gr_rules[i];
        char out[64];
        if (r->op != GR_OP_REG) continue;
        if (strcmp(r->arg1, "Software\\Valve\\Half-Life\\Settings") != 0)
            continue;
        shared_key = 1;
        gr_expand(r->arg3, &t, out, sizeof(out));
        if (!strcmp(r->arg2, "ScreenWidth"))   w    = atoi(out + 6);
        if (!strcmp(r->arg2, "ScreenHeight"))  h    = atoi(out + 6);
        if (!strcmp(r->arg2, "EngineModeW"))   engw = atoi(out + 6);
        if (!strcmp(r->arg2, "EngineModeH"))   engh = atoi(out + 6);
    }
    CHECK_EQ_I(shared_key, 1);
    CHECK_EQ_I(w, 1920);
    CHECK_EQ_I(h, 1080);
    /* BOTH halves. The launcher's Screen* and the engine's EngineMode* are two
     * different sets and the engine comes up at its own 400x300 default unless
     * they agree - A/B'd on .133. */
    CHECK_EQ_I(engw, 1920);
    CHECK_EQ_I(engh, 1080);
    /* And it must NOT be the value install.reg ships. */
    CHECK((w) != (1024),
          "w must not equal 1024");
}

/* Every id Tech 3 title with the custom-mode branch reaches the panel's full
 * mode; the forks without it fall back to an index and must NOT claim -1. */
TEST(t_idtech3_split)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_1080p();
    char out[1024];
    modes_lcd1080(&l);
    gr_decide(&pan, &l, 1920, 1080, 60, 32, 0, 0, &t);

    CHECK_EQ_I(gr_expand(gr_cfg_body(NULL), &t, out, sizeof(out)), 0);
    CHECK(strstr(out, "seta r_mode \"-1\"") != NULL,
          "an engine with the custom-mode branch must be given r_mode -1");
    CHECK(strstr(out, "seta r_customwidth \"1920\"") != NULL,
          "and the panel's full width");
    CHECK(strstr(out, "seta r_customheight \"1080\"") != NULL,
          "and the panel's full height");
    CHECK(strstr(out, "seta cg_fov \"106\"") != NULL,
          "id Tech 3 is vert-, so 16:9 needs the hor+ FOV or you see LESS");

    /* SoF2's fork has no -1 branch: measured, it renders 640x480 rather than
     * erroring. It gets an INDEX, and from the id Tech 3 table. */
    CHECK_EQ_I(gr_expand(gr_cfg_body("idtech3-index"), &t, out, sizeof(out)), 0);
    CHECK(strstr(out, "\"-1\"") == NULL,
          "a fork without the custom-mode branch must NOT be handed -1");
    CHECK(strstr(out, "seta r_mode \"7\"") != NULL,
          "it gets id Tech 3 index 7 = 1152x864, not index 8 = 1280x1024");

    /* id Tech 2 has neither: a fixed table, no custom mode, no 16:9 entry. */
    CHECK_EQ_I(gr_expand(gr_cfg_body("idtech2"), &t, out, sizeof(out)), 0);
    CHECK(strstr(out, "r_custom") == NULL,
          "id Tech 2 has no custom-mode cvars at all");
    CHECK(strstr(out, "set gl_mode \"8\"") != NULL,
          "id Tech 2 index 8 is 1280x960 - the SAME index means a different "
          "mode in the two engines, which is why there are two selectors");
}

/*
 * THE BUG THIS CAUGHT ON HARDWARE. The KV writer replaces a whole LINE, so it
 * has to be given "ResolutionX=1024". Given the bare "1024" it replaced the
 * line with "1024", which then no longer parses as key=value - so the next
 * pass matched nothing and APPENDED another "1024". Three GAMERES runs on .191
 * left Descent 2's DESCENT.CFG with six junk lines and no resolution, every
 * run reporting success.
 */
TEST(t_kv_line_is_composed)
{
    char out[64];

    CHECK_EQ_I(gr_kv_line("ResolutionX", "1024", out, sizeof(out)), 0);
    CHECK(strcmp(out, "ResolutionX=1024") == 0,
          "a KV rule writes key=value, never the bare value");
    CHECK(strchr(out, '=') != NULL,
          "without the = the line stops being key=value and the next pass "
          "appends instead of replacing");

    /* Overflow is an error, not a truncated line - a half-written key would
     * match nothing and append forever, which is the same fault again. */
    CHECK(gr_kv_line("ResolutionX", "1024", out, 8) != 0,
          "gr_kv_line must refuse rather than truncate");

    /* And no rule may carry its own '=', or composing would double it. */
    {
        int i;
        for (i = 0; i < GR_RULE_COUNT; i++)
            if (gr_rules[i].op == GR_OP_KV)
                CHECK(strchr(gr_rules[i].arg2, '=') == NULL,
                      "a KV rule supplies the VALUE only; the writer adds the =");
    }
}


/*
 * THE HIGHEST REFRESH THE MONITOR SUPPORTS, PER RESOLUTION.
 *
 * A rate is offered per mode, not per box: this tube does 100 Hz at 1024x768
 * and only 75 at 1280x960. One `hz` for the machine would therefore be wrong
 * for one of the two engines running at those two targets, which is the same
 * shape as one resolution for eight monitors.
 */
TEST(t_refresh_is_per_resolution)
{
    gr_modes_t l;
    modes_crt(&l);

    CHECK_EQ_I(gr_best_hz(&l, 1024, 768), 100);
    CHECK_EQ_I(gr_best_hz(&l, 1280, 960), 75);
    CHECK_EQ_I(gr_best_hz(&l, 640, 480), 120);

    /* The 0 Hz "use the driver default" sentinel is not a rate. 640x480 was
     * added with 0 first and must still answer 120, not 0. */
    CHECK(gr_hz_is_real(0) == 0, "0 Hz is a sentinel, never a refresh rate");
    CHECK(gr_hz_is_real(1) == 0, "1 Hz is a sentinel too");
    CHECK(gr_hz_is_real(60) == 1, "60 Hz is real");

    /* A resolution the driver never offered has no rate, and 0 must be read
     * as "leave it alone" - never as 60, which is the staged-constant mistake
     * one field to the right. */
    CHECK_EQ_I(gr_best_hz(&l, 1920, 1080), 0);
}

/*
 * THE PANEL'S OWN CEILING BOUNDS IT. Drivers list rates an analogue monitor
 * cannot sync, and on a tube the good outcome of asking is "out of range" on
 * a screen nobody is standing in front of.
 */
TEST(t_refresh_is_clamped_to_the_edid)
{
    gr_modes_t l;
    size_t i;
    static const int t[][3] = {
        {1024,768,60},{1024,768,85},{1024,768,100},{1024,768,120}
    };

    /* A panel that says it tops out at 100 Hz. */
    gr_modes_reset(&l);
    l.hz_cap = 100;
    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        gr_modes_add(&l, t[i][0], t[i][1], t[i][2]);
    CHECK_EQ_I(gr_best_hz(&l, 1024, 768), 100);

    /* THE CLAMP MUST BITE ON THE WAY IN. Only the best rate per resolution is
     * kept, so a clamp applied at read time would have stored 120 and had
     * nothing left to fall back to - it would answer 0 and lose the 100 Hz
     * this monitor really does support. */
    gr_modes_reset(&l);
    l.hz_cap = 90;
    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        gr_modes_add(&l, t[i][0], t[i][1], t[i][2]);
    CHECK_EQ_I(gr_best_hz(&l, 1024, 768), 85);

    /* No EDID means no measurement and therefore no clamp - not a clamp of 0,
     * which would refuse every rate on the box. */
    gr_modes_reset(&l);
    l.hz_cap = 0;
    for (i = 0; i < sizeof(t)/sizeof(t[0]); i++)
        gr_modes_add(&l, t[i][0], t[i][1], t[i][2]);
    CHECK_EQ_I(gr_best_hz(&l, 1024, 768), 120);
}

/* A file BOTH the agent and the title's launcher write must carry the SAME
 * number, or each rewrites the other's copy on every sync and launch forever -
 * and the "0 value(s) changed" contract that catches real faults dies with it.
 * The id Tech 3 bodies therefore use %FRHZ%, the persisted desktop rate, which
 * is exactly what FLEETRES publishes as FR_HZ. */
TEST(t_shared_files_use_the_launchers_number)
{
    gr_modes_t l; gr_target_t t; gr_panel_t pan = panel_crt43();
    char out[1024];
    modes_crt(&l);
    gr_decide(&pan, &l, 1280, 1024, 85, 32, 0, 0, &t);

    CHECK_EQ_I(t.fr_hz, 85);                /* the mode it is persisted at */
    CHECK_EQ_I(t.hz, 75);                   /* the best at the 1280x960 target */

    CHECK_EQ_I(gr_expand(gr_cfg_body(NULL), &t, out, sizeof(out)), 0);
    CHECK(strstr(out, "seta r_displayRefresh \"85\"") != NULL,
          "the shared cfg carries FR_HZ, the number the launcher also writes");
    CHECK(strstr(out, "seta r_displayRefresh \"75\"") == NULL,
          "using the per-target rate here would fight the launcher forever");

    /* Serious Engine's Game_startup.ini is written by both, too. */
    CHECK_EQ_I(gr_expand(gr_cfg_body("ssam"), &t, out, sizeof(out)), 0);
    CHECK(strstr(out, "gfx_iRefreshRate=85;") != NULL,
          "Game_startup.ini is a shared file and takes the same number");

    /* FR_HZ reproduces FLEETRES exactly, including its 60 fallback for a mode
     * whose rate the driver did not report. */
    gr_decide(&pan, &l, 1280, 1024, 0, 32, 0, 0, &t);
    CHECK_EQ_I(t.fr_hz, 60);
}

MUNIT_MAIN("gameres (per-box monitor detection and per-title resolution)",
    RUN(t_lcd_gets_native);
    RUN(t_q2_q3_tables_differ);
    RUN(t_crt_never_5_4);
    RUN(t_no_edid_assumes_4_3);
    RUN(t_only_offered_modes);
    RUN(t_empty_list_is_not_a_refusal);
    RUN(t_per_box_cap);
    RUN(t_expand);
    RUN(t_rules_wellformed);
    RUN(t_goldsrc_reaches_the_panel);
    RUN(t_idtech3_split);
    RUN(t_kv_line_is_composed);
    RUN(t_refresh_is_per_resolution);
    RUN(t_refresh_is_clamped_to_the_edid);
    RUN(t_shared_files_use_the_launchers_number);
)
