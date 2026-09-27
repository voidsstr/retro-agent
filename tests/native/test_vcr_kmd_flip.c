/* test_vcr_kmd_flip.c - TRUE-SOURCE test of voodoo-cleanroom/vcr-kmd/include/vcr_flip.h,
 * the rule the display DLL (display/vcrdd_ddraw.c flip_done / Dd_Flip) uses to
 * decide that the chip has taken a DirectDraw flip.
 *
 * The header is driven over a CRT timeline with 86Box's semantics - the only
 * bed that runs this driver's Voodoo path:
 *   - status[6] reads "in retrace" for the VSYNC PULSE only: set at
 *     vc == vsyncstart, cleared at the vsync-end match (/home/voidsstr/
 *     retro-vm/86box/vid_svga.c svga_poll: `cgastat |= 8` at vc == vsyncstart,
 *     `cgastat &= ~8` at the CR11 match; vid_voodoo_banshee.c: status bit 6 =
 *     !(cgastat & 8));
 *   - vidDesktopStartAddr is LATCHED at vsync start, on the same line
 *     (banshee_vsync_callback: desktop_addr = vidDesktopStartAddr);
 *   - vsyncstart = CR10 + 1 (svga_recalctimings), and our CR10 is written one
 *     line early (vcr_modes.c, the vendor's value), so the pulse starts at
 *     vdisp + vfp - the timing table's own line;
 *   - a write that lands exactly on the vsync start is counted as MISSING that
 *     latch (the conservative reading for a never-early test).
 * The CRT runs at the PLL's ACHIEVED clock (vcr_mode_compute on the driver's
 * own timing table), and the driver's clock is QueryPerformanceCounter at
 * 3579545 Hz (the ACPI PM timer XP uses), quantised as the real one is.
 *
 * What it proves (each against the real header):
 *   - NEVER EARLY: the write swept over every line and sub-line offset at
 *     800x600@85, 640x480@60 and 1600x1200@75, with a spinning caller, with
 *     polls that never land in the pulse (the deadline path), with pulses
 *     from 1 line to 16 lines longer - both deadline rules;
 *   - the order matters: the SAME sweep with the retrace state sampled BEFORE
 *     the write completes flips a frame early;
 *   - a DDFLIP_WAIT spinner completes exactly once per refresh, all by
 *     retrace, none by deadline;
 *   - the D3D pattern (no poll between a flip and the next wait, 1.0 - 1.12
 *     refreshes a frame): the nominal 1/8 rule caps it at 0.889 of the
 *     refresh, the achieved 1 + 1/32 rule reaches >= 0.95 at 1.05;
 *   - DDFLIP_NOVSYNC is never pending;
 *   - the default deadline is TODAY'S to the tick: 47376 at 85 Hz and 67116
 *     at 60 Hz for QPF 3579545, and the old expression for every rate/QPF;
 *   - the one silicon assumption: a retrace flag that asserted BEFORE the
 *     latch (from display end) would complete flips early - which is why
 *     vidCurrentLine (the report's P2, "in blank = line >= vdisp") is not
 *     used, and why it needs measuring on the VSA-100;
 *   - the achieved rule's trust band (1/64 of the nominal rate) is narrower
 *     than its 1/32 margin: a sent rate skewed anywhere inside the band
 *     against the real CRT is never early, for every timing in the table,
 *     while the old 1/16 band trusted rates 4-5.5% high that were early
 *     (review of the flip track, 2026-09-27);
 *   - the counters are logged once per session although three points of a
 *     PDEV's life may log them (a mode-setting session's flips live in a PDEV
 *     that is gone before exclusive mode ends).
 * The flip report's scratch model (flipsim*.c, a deleted scratchpad) is
 * preserved here.
 */
#include <stddef.h>
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_modes.c"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_flip.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_ioctl.h"

#define QPF        3579545LL        /* ticks per second */
typedef long long ns_t;

/* ---- the CRT ------------------------------------------------------------------ */

typedef struct crt {
    const char *name;
    ns_t    line, frame;            /* ns */
    int     vtot, vdisp, vss;       /* lines: active 0..vdisp-1, vsync (and latch) at vss */
    int     pulse;                  /* lines the retrace flag is up from vss */
    int     lead;                   /* lines the flag rises BEFORE the latch (86Box: 0) */
    vcr_u32 refresh_mhz;            /* what the miniport computes and would send */
    vcr_u32 nominal;                /* the mode's nominal rate: pd->freq */
} crt;

static vcr_hwcaps v5caps(void)
{
    vcr_hwcaps h;
    memset(&h, 0, sizeof h);
    h.device_id = VCR_DEV_VSA100;
    h.max_pixclk_khz = 350000;
    h.twox_above_khz = 262000;
    h.twox_htotal_chars = 261;
    h.fb_bytes = 32u << 20;
    h.fb_reserved = 64u << 10;
    return h;
}

static crt crt_make(const char *name, unsigned w, unsigned h, unsigned hz)
{
    crt c;
    vcr_hwcaps hw = v5caps();
    vcr_modeset m;
    int i = vcr_timing_find(w, h, hz);
    const vcr_timing *t;
    unsigned htot;
    memset(&c, 0, sizeof c);
    c.name = name;
    if (i < 0 || vcr_mode_compute(&hw, &vcr_timings[i], 16, &m))
        return c;                           /* line 0: the caller's CHECK fails */
    t = &vcr_timings[i];
    htot = (unsigned)t->w + t->hfp + t->hsync + t->hbp;
    c.line = (ns_t)htot * 1000000 / m.pix_khz_actual;
    c.vdisp = t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1);
    c.vss = c.vdisp + t->vfp;
    c.vtot = c.vss + t->vsync + t->vbp;
    c.pulse = t->vsync;
    c.frame = c.line * c.vtot;
    c.refresh_mhz = m.refresh_mhz;
    c.nominal = t->refresh;
    return c;
}

static int flag_at(const crt *c, ns_t t)
{
    ns_t pos = t % c->frame;
    return pos >= (ns_t)(c->vss - c->lead) * c->line && pos < (ns_t)(c->vss + c->pulse) * c->line;
}

/* the first latch strictly after a write at w */
static ns_t latch_after(const crt *c, ns_t w)
{
    ns_t base = (w / c->frame) * c->frame + (ns_t)c->vss * c->line;
    return base > w ? base : base + c->frame;
}

/* latches in (a, b] */
static long long latches_between(const crt *c, ns_t a, ns_t b)
{
    ns_t o = (ns_t)c->vss * c->line;
    return (b - o) / c->frame - (a - o) / c->frame;
}

static vcr_ticks ticks(ns_t t)
{
    return (vcr_ticks)(t * QPF / 1000000000LL);
}

/* ---- the driver, as vcrdd_ddraw.c drives the header -------------------------- */

/* an IOCTL_VCR_VBLANK: the status read IN_NS into it, returned OUT_NS later */
#define IN_NS      3000
#define OUT_NS     3000
#define GAP_NS     9000             /* the runtime's retry loop: a poll every 15 us */
#define WRITE_NS   4000             /* the start-address write, inside IOCTL_VCR_DDFLIP */
#define FLIPIO_NS  13000            /* the flip IOCTL and the TRACE log IOCTL after it */
#define PRE_NS     3000             /* the pre-write counterfactual: sampled this far BEFORE */
#define POLL_NS    (IN_NS + OUT_NS + GAP_NS)

/* RULE_RAW: frame + 1/32 of the SENT rate with no trust band - what a band
 * too wide lets through (the counterfactual for the_trust_band_...) */
enum { RULE_NOMINAL, RULE_ACHIEVED, RULE_ORACLE, RULE_RAW };

typedef struct drv {
    vcr_flip_state s;
    const crt *c;
    int     rule;
    int     prewrite;               /* sample BEFORE the write - the order that is wrong */
    int     avoid_pulse;            /* no poll ever reads the flag up: the deadline path */
    ns_t    last_w;                 /* the pending flip's write */
    ns_t    oracle_latch;
    int     oracle_pending;
    /* what the model saw */
    long    done, early, retrace_done;
    ns_t    worst_late;             /* told done this long after the latch, at most */
    ns_t    worst_early;            /* told done this long BEFORE the latch, at most */
} drv;

static void drv_init(drv *d, const crt *c, int rule)
{
    memset(d, 0, sizeof *d);
    d->c = c;
    d->rule = rule;
}

static int drv_pending(const drv *d)
{
    return d->rule == RULE_ORACLE ? d->oracle_pending : (int)d->s.pending;
}

/* flip_done(): no IOCTL when nothing is pending; else one timed read */
static int drv_poll(drv *d, ns_t *t)
{
    ns_t sample, latch;
    int in, r;
    vcr_u32 by_retrace0 = d->s.by_retrace;
    if (!drv_pending(d))
        return 1;
    if (d->avoid_pulse)
        while (flag_at(d->c, *t + IN_NS))
            *t += 1000;             /* descheduled across the whole pulse */
    sample = *t + IN_NS;
    in = flag_at(d->c, sample);
    vcr_flip_note_poll(&d->s, ticks(*t + IN_NS + OUT_NS) - ticks(*t));
    *t += IN_NS + OUT_NS;
    if (d->rule == RULE_ORACLE) {
        r = sample >= d->oracle_latch;
        d->oracle_pending = !r;
    } else {
        r = vcr_flip_poll(&d->s, 1, in, ticks(*t));
    }
    if (r) {
        latch = latch_after(d->c, d->last_w);
        d->done++;
        if (sample < latch) {
            d->early++;
            if (latch - sample > d->worst_early)
                d->worst_early = latch - sample;
        } else if (sample - latch > d->worst_late)
            d->worst_late = sample - latch;
        if (d->s.by_retrace != by_retrace0)
            d->retrace_done++;
    }
    return r;
}

static vcr_ticks drv_deadline(const drv *d)
{
    if (d->rule == RULE_RAW) {
        vcr_ticks frame = QPF * 1000 / (vcr_ticks)d->c->refresh_mhz;
        return frame + frame / 32;
    }
    return vcr_flip_deadline(QPF, d->rule == RULE_ACHIEVED ? d->c->refresh_mhz : 0,
                             d->c->nominal);
}

/* Dd_Flip at *t with DDFLIP_WAIT: retried until the last flip is done, then
 * the write, then (post-write order) the timed sample and the clock. Returns
 * the write's time. */
static ns_t drv_flip(drv *d, ns_t *t, int novsync)
{
    ns_t w, sample;
    if (!novsync)
        while (!drv_poll(d, t))
            *t += GAP_NS;
    w = *t + WRITE_NS;
    *t += FLIPIO_NS;
    sample = d->prewrite ? w - PRE_NS : *t + IN_NS;
    vcr_flip_note_poll(&d->s, ticks(*t + IN_NS + OUT_NS) - ticks(*t));
    *t += IN_NS + OUT_NS;
    vcr_flip_begin(&d->s, novsync, 1, flag_at(d->c, sample), ticks(*t), drv_deadline(d));
    d->last_w = w;
    d->oracle_latch = latch_after(d->c, w);
    d->oracle_pending = !novsync;
    return w;
}

/* ---- scenarios ------------------------------------------------------------------ */

static ns_t g_sweep_worst_early;     /* the last sweep's, for the band test */

/* The write swept over every line and 5 offsets inside it; after each flip a
 * caller polls until done. Returns the early completions. */
static long sweep(const crt *c, int rule, int prewrite, int avoid_pulse, long *retrace, long *n,
                  ns_t *worst_late)
{
    long early = 0;
    int l, k;
    *retrace = *n = 0;
    *worst_late = 0;
    g_sweep_worst_early = 0;
    for (l = 0; l < c->vtot; l++)
        for (k = 0; k < 5; k++) {
            drv d;
            ns_t sub[5] = { 0, c->line / 4, c->line / 2, 3 * c->line / 4, c->line - 1 };
            ns_t t = 4 * c->frame + (ns_t)l * c->line + sub[k] - WRITE_NS;
            drv_init(&d, c, rule);
            d.prewrite = prewrite;
            d.avoid_pulse = avoid_pulse;
            drv_flip(&d, &t, 0);
            while (!drv_poll(&d, &t))
                t += GAP_NS;
            early += d.early;
            *retrace += d.retrace_done;
            *n += d.done;
            if (d.worst_late > *worst_late)
                *worst_late = d.worst_late;
            if (d.worst_early > g_sweep_worst_early)
                g_sweep_worst_early = d.worst_early;
        }
    return early;
}

/* ddlab's loop: Lock(back) waits for the flip, the pattern, Flip, Lock(front) */
static double spinner(const crt *c, int rule, int frames, long *per_refresh_bad, drv *out)
{
    drv d;
    ns_t t = 3 * c->frame + 12345, w, w0 = 0, wp = 0;
    int f;
    *per_refresh_bad = 0;
    drv_init(&d, c, rule);
    for (f = 0; f < frames; f++) {
        while (!drv_poll(&d, &t))           /* Lock(back, DDLOCK_WAIT) */
            t += GAP_NS;
        t += 100000;                        /* the pattern */
        w = drv_flip(&d, &t, 0);
        if (f && latches_between(c, wp, w) != 1)
            (*per_refresh_bad)++;
        if (!f)
            w0 = w;
        wp = w;
        t += 50000;                         /* Lock(front) and the check: no flip poll */
    }
    *out = d;
    return (double)(frames - 1) / (double)(wp - w0) * (double)c->frame;
}

/* D3D's Present: Flip (DDFLIP_WAIT) then render r frames with no DirectDraw call */
static double d3d_pattern(const crt *c, int rule, double r, int frames, drv *out)
{
    drv d;
    ns_t t = 3 * c->frame + 777777, w, w0 = 0, wp = 0;
    int f;
    drv_init(&d, c, rule);
    for (f = 0; f < frames; f++) {
        w = drv_flip(&d, &t, 0);
        if (!f)
            w0 = w;
        wp = w;
        t += (ns_t)((double)c->frame * r);
    }
    *out = d;
    return (double)(frames - 1) / (double)(wp - w0) * (double)c->frame;
}

static crt MODES[3];

static void modes_init(void)
{
    MODES[0] = crt_make("800x600@85", 800, 600, 85);
    MODES[1] = crt_make("640x480@60", 640, 480, 60);
    MODES[2] = crt_make("1600x1200@75", 1600, 1200, 75);
}

/* ---- tests ---------------------------------------------------------------------- */

TEST(the_timeline_is_the_drivers_own_modes) {
    int m;
    for (m = 0; m < 3; m++) {
        const crt *c = &MODES[m];
        CHECK(c->line > 0, c->name);
        /* the model's frame and the rate the miniport would send agree to 0.05% */
        CHECK(c->refresh_mhz > 0, "refresh computed");
        {
            ns_t f_from_mhz = (ns_t)(1000000000000LL / c->refresh_mhz);
            ns_t diff = f_from_mhz > c->frame ? f_from_mhz - c->frame : c->frame - f_from_mhz;
            CHECK(diff * 2000 < c->frame, "model frame vs refresh_mhz");
        }
    }
    /* 800x600@85: htotal 1048, vtotal 631, vsync 3 lines from line 601 */
    CHECK_EQ_I(MODES[0].vtot, 631);
    CHECK_EQ_I(MODES[0].vss, 601);
    CHECK_EQ_I(MODES[0].pulse, 3);
    CHECK(MODES[0].refresh_mhz > 84900 && MODES[0].refresh_mhz < 85200, "~85 Hz achieved");
    CHECK_EQ_I(MODES[1].vtot, 525);
    CHECK_EQ_I(MODES[1].vss, 490);      /* CR10 0xe9 = 489, and 86Box adds one */
    CHECK_EQ_I(MODES[2].vtot, 1250);
}

TEST(the_default_deadline_is_todays_to_the_tick) {
    /* today's rule (vcrdd_ddraw.c before vcr_flip.h): f / (freq > 1 ? freq : 60),
     * plus an eighth */
    static const vcr_ticks qpfs[] = { 3579545, 1193182, 14318180, 2400000000LL, 1000000000LL };
    unsigned q, hz;
    CHECK_EQ_I(vcr_flip_deadline(QPF, 0, 85), 47376);       /* 42112 + 5264 */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 0, 60), 67116);       /* 59659 + 7457 */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 0, 0), 67116);        /* unknown: 60 */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 0, 1), 67116);        /* "hardware default" */
    for (q = 0; q < sizeof qpfs / sizeof qpfs[0]; q++)
        for (hz = 0; hz <= 240; hz++) {
            vcr_ticks f = qpfs[q], frame = f / (hz > 1 ? hz : 60);
            if (vcr_flip_deadline(f, 0, hz) != frame + frame / 8) {
                CHECK(0, "the nominal rule drifted from today's");
                return;
            }
        }
}

TEST(the_achieved_deadline_is_one_real_frame_plus_a_32nd) {
    int m;
    /* 800x600@85: the miniport's number, to the tick */
    CHECK_EQ_I(vcr_flip_deadline(QPF, MODES[0].refresh_mhz, 85),
               QPF * 1000 / MODES[0].refresh_mhz + QPF * 1000 / MODES[0].refresh_mhz / 32);
    for (m = 0; m < 3; m++) {
        const crt *c = &MODES[m];
        vcr_ticks newdl = vcr_flip_deadline(QPF, c->refresh_mhz, c->nominal);
        vcr_ticks olddl = vcr_flip_deadline(QPF, 0, c->nominal);
        vcr_ticks frame = ticks(c->frame);
        CHECK(newdl < olddl, "tighter than the nominal 1/8");
        /* more than one real frame by at least 3% of one */
        CHECK(newdl > frame + frame / 34, "never under a real frame");
    }
    /* an achieved rate that disagrees with the nominal one is not trusted */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 170000, 85), 47376);   /* double */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 79000, 85), 47376);    /* 7% low */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 91000, 85), 47376);    /* 7% high */
    CHECK_EQ_I(vcr_flip_deadline(QPF, 85000, 0), 67116);     /* no nominal to check it by */
    CHECK_EQ_I(vcr_flip_trusted_mhz(85061, 85), 85061);
    CHECK_EQ_I(vcr_flip_trusted_mhz(0, 85), 0);
}

TEST(never_early_with_the_write_swept_over_every_line) {
    static const int rules[2] = { RULE_NOMINAL, RULE_ACHIEVED };
    int m, r, p;
    for (m = 0; m < 3; m++)
        for (r = 0; r < 2; r++) {
            crt c = MODES[m];
            int pulses[3];
            pulses[0] = 1;
            pulses[1] = MODES[m].pulse;
            pulses[2] = MODES[m].pulse + 16;
            for (p = 0; p < 3; p++) {
                long retrace, n;
                ns_t late;
                c.pulse = pulses[p];
                /* a caller spinning on the flip */
                CHECK_EQ_I(sweep(&c, rules[r], 0, 0, &retrace, &n, &late), 0);
                CHECK_EQ_I(n, (long)c.vtot * 5);
                if ((ns_t)c.pulse * c.line > POLL_NS) {
                    /* every pulse holds a poll: all by the retrace, and at
                     * most one frame late - a write racing into the retrace
                     * waits for the next one, never more */
                    CHECK(retrace == n, "a spinner is done by the retrace every time");
                    CHECK(late < c.frame + (ns_t)c.pulse * c.line + 2 * POLL_NS,
                          "more than a frame late");
                } else {
                    /* a 1-line pulse at 1600x1200 (10.7 us) can fall between
                     * two 15 us polls: those flips go by the deadline -
                     * latency, not rate (a_spinner_completes_...) */
                    CHECK(retrace < n, "a pulse shorter than the poll period is missed");
                }
                /* no poll ever reads the flag up: everything by the deadline */
                CHECK_EQ_I(sweep(&c, rules[r], 0, 1, &retrace, &n, &late), 0);
                CHECK_EQ_I(retrace, 0);
                CHECK_EQ_I(n, (long)c.vtot * 5);
            }
        }
}

TEST(a_sample_taken_before_the_write_completes_flips_early) {
    int m;
    for (m = 0; m < 3; m++) {
        long retrace, n, pre, post;
        ns_t late;
        post = sweep(&MODES[m], RULE_NOMINAL, 0, 0, &retrace, &n, &late);
        pre = sweep(&MODES[m], RULE_NOMINAL, 1, 0, &retrace, &n, &late);
        CHECK_EQ_I(post, 0);                /* the order the driver uses */
        CHECK(pre > 0, "the pre-write order must show its early completions");
    }
}

TEST(a_spinner_completes_exactly_once_per_refresh) {
    int m, r;
    for (m = 0; m < 3; m++)
        for (r = RULE_NOMINAL; r <= RULE_ACHIEVED; r++) {
            long bad;
            drv d;
            double rate = spinner(&MODES[m], r, 2000, &bad, &d);
            CHECK_EQ_I(bad, 0);                     /* one latch between flips */
            CHECK(rate > 0.999 && rate < 1.001, "one flip per refresh");
            CHECK_EQ_U(d.s.by_deadline, 0);
            CHECK_EQ_U(d.s.by_retrace, 1999);
            CHECK_EQ_I(d.early, 0);
            /* the counters add up: flips = retrace + deadline + superseded + pending */
            CHECK_EQ_U(d.s.flips, d.s.by_retrace + d.s.by_deadline + d.s.superseded + d.s.pending);
            CHECK_EQ_U(d.s.flips, 2000);
            /* every read took IN_NS + OUT_NS (21-22 ticks); the longest wait is
             * under a frame plus the pulse */
            CHECK(d.s.max_poll >= 21 && d.s.max_poll <= 22, "longest read");
            CHECK(d.s.max_wait > 0 && d.s.max_wait < ticks(MODES[m].frame), "longest wait");
        }
}

TEST(the_d3d_pattern_old_rule_0_889_new_rule_0_95) {
    drv d;
    double oldr, newr, orc;
    int m, i;
    static const double rs[] = { 1.00, 1.01, 1.03, 1.05, 1.08, 1.10, 1.12 };
    /* the report's case: 1.05 refreshes of work at 800x600@85 */
    oldr = d3d_pattern(&MODES[0], RULE_NOMINAL, 1.05, 3000, &d);
    CHECK(oldr > 0.885 && oldr < 0.893, "the nominal 1/8 rule caps it at 0.889");
    CHECK_EQ_I(d.early, 0);
    /* most flips wait out the deadline (1.125 frames); the few done by a
     * retrace land on the same 9-frames-per-8-flips cadence */
    CHECK(d.s.by_deadline > 2500, "the deadline decides the rate");
    newr = d3d_pattern(&MODES[0], RULE_ACHIEVED, 1.05, 3000, &d);
    CHECK(newr >= 0.95, "the achieved 1 + 1/32 rule: >= 0.95");
    CHECK_EQ_I(d.early, 0);
    for (m = 0; m < 3; m++)
        for (i = 0; i < (int)(sizeof rs / sizeof rs[0]); i++) {
            drv dn, da, dor;
            oldr = d3d_pattern(&MODES[m], RULE_NOMINAL, rs[i], 2000, &dn);
            newr = d3d_pattern(&MODES[m], RULE_ACHIEVED, rs[i], 2000, &da);
            orc = d3d_pattern(&MODES[m], RULE_ORACLE, rs[i], 2000, &dor);
            CHECK(oldr > 0.885 && oldr < 0.893, "old: 0.889 across 1.00-1.12");
            CHECK(newr >= oldr, "new never slower");
            CHECK(newr <= orc + 0.001, "nothing beats done-at-the-latch");
            /* the gap is widest at exactly one refresh (0.968 vs 0.998) */
            CHECK(newr >= orc - 0.035, "new within 3.5% of done-at-the-latch");
            if (rs[i] <= 1.05)
                CHECK(newr >= 0.95, "new >= 0.95 up to 1.05 refreshes of work");
            CHECK_EQ_I(dn.early, 0);
            CHECK_EQ_I(da.early, 0);
        }
    /* under one refresh of work every rule keeps up with the refresh */
    for (m = 0; m < 3; m++) {
        CHECK(d3d_pattern(&MODES[m], RULE_NOMINAL, 0.95, 2000, &d) > 0.999, "old, r 0.95");
        CHECK(d3d_pattern(&MODES[m], RULE_ACHIEVED, 0.95, 2000, &d) > 0.999, "new, r 0.95");
        CHECK(d3d_pattern(&MODES[m], RULE_NOMINAL, 0.5, 2000, &d) > 0.999, "old, r 0.5");
    }
}

TEST(a_novsync_flip_is_never_pending) {
    vcr_flip_state s;
    drv d;
    ns_t t;
    int f;
    memset(&s, 0, sizeof s);
    vcr_flip_begin(&s, 1, 1, 0, 1000, 47376);
    CHECK_EQ_U(s.pending, 0);
    CHECK_EQ_I(vcr_flip_poll(&s, 1, 0, 1001), 1);
    CHECK_EQ_U(s.flips, 0);
    CHECK_EQ_U(s.by_retrace + s.by_deadline, 0);
    /* over a vsync'd flip still pending: that one is replaced, and counted so */
    vcr_flip_begin(&s, 0, 1, 0, 2000, 47376);
    CHECK_EQ_U(s.pending, 1);
    vcr_flip_begin(&s, 1, 1, 1, 3000, 47376);
    CHECK_EQ_U(s.pending, 0);
    CHECK_EQ_U(s.superseded, 1);
    CHECK_EQ_U(s.flips, 1);
    /* a run of them: no flip call waits, nothing is ever pending */
    drv_init(&d, &MODES[0], RULE_NOMINAL);
    t = 5 * MODES[0].frame;
    for (f = 0; f < 500; f++) {
        drv_flip(&d, &t, 1);
        CHECK(!d.s.pending, "NOVSYNC left a flip pending");
        t += 1000000;
    }
    CHECK_EQ_U(d.s.flips, 0);
    CHECK_EQ_U(d.s.by_deadline, 0);
}

TEST(the_rule_without_a_retrace_bit_is_the_deadline) {
    /* an IOCTL that fails (no sample) changes nothing but the clock */
    vcr_flip_state s;
    memset(&s, 0, sizeof s);
    vcr_flip_begin(&s, 0, 0, 0, 100000, 47376);
    CHECK_EQ_U(s.seen_active, 0);
    CHECK_EQ_I(vcr_flip_poll(&s, 0, 1, 100000 + 47376), 0);     /* not past it */
    CHECK_EQ_I(vcr_flip_poll(&s, 0, 0, 100000 + 47377), 1);     /* strictly past */
    CHECK_EQ_U(s.by_deadline, 1);
    CHECK_EQ_U(s.max_wait, 1);
    /* sampled in the pulse after the write: it waits for active, then a pulse */
    memset(&s, 0, sizeof s);
    vcr_flip_begin(&s, 0, 1, 1, 0, 47376);
    CHECK_EQ_I(vcr_flip_poll(&s, 1, 1, 10), 0);     /* the same pulse: not done */
    CHECK_EQ_I(vcr_flip_poll(&s, 1, 0, 20), 0);     /* active */
    CHECK_EQ_I(vcr_flip_poll(&s, 1, 1, 30), 1);     /* the next pulse */
    CHECK_EQ_U(s.by_retrace, 1);
    /* a reset mid-flip keeps the flip counted */
    vcr_flip_begin(&s, 0, 1, 0, 40, 47376);
    vcr_flip_stats_reset(&s);
    CHECK_EQ_U(s.flips, 1);
    CHECK_EQ_U(s.by_retrace, 0);
    CHECK_EQ_I(vcr_flip_poll(&s, 1, 1, 50), 1);
    CHECK_EQ_U(s.flips, s.by_retrace + s.by_deadline + s.superseded + s.pending);
}

TEST(the_one_assumption_the_flag_does_not_lead_the_latch) {
    /* If status[6] rose at display END (the whole blank) while the latch stays
     * at vsync start, the current rule would complete up to the front porch
     * early - 10 lines at 640x480@60. This is exactly what the scanline
     * proposal P2 ("in blank = vidCurrentLine >= vdisp") would build in on
     * purpose; the flag's real start on the VSA-100 is unmeasured. */
    crt c = MODES[1];
    long retrace, n;
    ns_t late;
    c.lead = c.vss - c.vdisp;              /* 10 lines */
    CHECK_EQ_I(c.lead, 10);
    CHECK(sweep(&c, RULE_NOMINAL, 0, 0, &retrace, &n, &late) > 0,
          "a flag that leads the latch must show early completions");
    c.lead = 0;
    CHECK_EQ_I(sweep(&c, RULE_NOMINAL, 0, 0, &retrace, &n, &late), 0);
}

/* the edges of the trust band around a nominal rate: the highest and lowest
 * sent rate vcr_flip_trusted_mhz accepts with a given shift */
static vcr_u32 band_hi(vcr_u32 nominal, int shift)
{
    vcr_u32 nom = nominal * 1000u;
    return nom + (nom >> shift);
}

static vcr_u32 band_lo(vcr_u32 nominal, int shift)
{
    vcr_u32 nom = nominal * 1000u;
    return nom - (nom >> shift);
}

TEST(the_trust_band_is_narrower_than_the_margin) {
    /* The review's harness: the CRT runs at the real (computed) rate, the
     * miniport SENDS a skewed one - a PLL that did not take its value, a
     * formula error. Deadline-only polling (no poll ever sees the pulse), so
     * the deadline alone decides. */
    int m;
    CHECK_EQ_I(VCR_FLIP_TRUST_SHIFT, 6);                    /* was 4 (1/16) */
    for (m = 0; m < 3; m++) {
        crt c = MODES[m];
        long retrace, n, early;
        ns_t late;
        vcr_u32 real = MODES[m].refresh_mhz, hi = band_hi(c.nominal, VCR_FLIP_TRUST_SHIFT),
                lo = band_lo(c.nominal, VCR_FLIP_TRUST_SHIFT), sk;
        /* every edge of the new band is trusted, and never early */
        CHECK_EQ_I(vcr_flip_trusted_mhz(hi, c.nominal), hi);
        CHECK_EQ_I(vcr_flip_trusted_mhz(lo, c.nominal), lo);
        CHECK_EQ_I(vcr_flip_trusted_mhz(hi + 1, c.nominal), 0);
        CHECK_EQ_I(vcr_flip_trusted_mhz(lo - 1, c.nominal), 0);
        c.refresh_mhz = hi;
        CHECK_EQ_I(sweep(&c, RULE_ACHIEVED, 0, 1, &retrace, &n, &late), 0);
        CHECK_EQ_I(n, (long)c.vtot * 5);
        c.refresh_mhz = lo;
        CHECK_EQ_I(sweep(&c, RULE_ACHIEVED, 0, 1, &retrace, &n, &late), 0);
        /* the review's rows: +3%, +4%, +5.5% of the REAL rate */
        sk = real + real * 3 / 100;
        c.refresh_mhz = sk;
        CHECK_EQ_I(sweep(&c, RULE_ACHIEVED, 0, 1, &retrace, &n, &late), 0);
        CHECK_EQ_I(sweep(&c, RULE_RAW, 0, 1, &retrace, &n, &late), 0);   /* inside the margin */
        for (sk = real + real * 4 / 100; sk <= real + real * 55 / 1000; sk += real * 15 / 1000) {
            c.refresh_mhz = sk;
            /* OLD: the 1/16 band trusted it ... */
            CHECK(sk <= band_hi(c.nominal, 4), "the old band trusted this rate");
            /* ... and with the rate trusted, flips completed before the latch */
            early = sweep(&c, RULE_RAW, 0, 1, &retrace, &n, &late);
            CHECK(early > 0, "a rate 4-5.5% high, trusted, is early");
            CHECK(g_sweep_worst_early > 0 && g_sweep_worst_early < c.frame * 3 / 100,
                  "early by under 3% of a frame");
            /* NEW: not trusted, the nominal rule decides - never early */
            CHECK_EQ_I(vcr_flip_trusted_mhz(sk, c.nominal), 0);
            CHECK_EQ_I(sweep(&c, RULE_ACHIEVED, 0, 1, &retrace, &n, &late), 0);
        }
    }
}

TEST(every_timing_keeps_the_achieved_rule_and_its_band_is_never_early) {
    /* Arithmetic over the WHOLE timing table, Voodoo 3 and VSA-100, at five
     * clock rates: every computed rate lies inside the band (the opt-in rule
     * applies to every mode), and the band's top edge still gives a deadline
     * of at least one real frame. The old 1/16 edge gives LESS than one real
     * frame on every timing. */
    static const vcr_ticks qpfs[] = { 3579545, 1193182, 14318180, 2400000000LL, 1000000000LL };
    vcr_hwcaps hw = v5caps();
    vcr_u32 i, checked = 0, old_short = 0;
    int dev, q;
    for (dev = 0; dev < 2; dev++) {
        if (dev) {
            hw.device_id = VCR_DEV_VOODOO3;
            hw.twox_above_khz = 160000;
            hw.twox_htotal_chars = 0;
        }
        for (i = 0; i < vcr_ntimings; i++) {
            const vcr_timing *t = &vcr_timings[i];
            vcr_modeset mm;
            long long htot, vtot;
            if (vcr_mode_compute(&hw, t, 16, &mm))
                continue;
            htot = (long long)t->w + t->hfp + t->hsync + t->hbp;
            vtot = (long long)t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) + t->vfp + t->vsync +
                   t->vbp;
            CHECK(vcr_flip_trusted_mhz(mm.refresh_mhz, t->refresh) == mm.refresh_mhz,
                  "a table timing falls outside the trust band");
            for (q = 0; q < (int)(sizeof qpfs / sizeof qpfs[0]); q++) {
                long long den = (long long)mm.pix_khz_actual * 1000;
                vcr_ticks real = (htot * vtot * qpfs[q] + den - 1) / den;   /* rounded up */
                vcr_ticks f_new = qpfs[q] * 1000 / band_hi(t->refresh, VCR_FLIP_TRUST_SHIFT);
                vcr_ticks f_old = qpfs[q] * 1000 / band_hi(t->refresh, 4);
                CHECK(vcr_flip_deadline(qpfs[q], band_hi(t->refresh, VCR_FLIP_TRUST_SHIFT),
                                        t->refresh) == f_new + f_new / 32, "the band edge");
                if (f_new + f_new / 32 < real) {
                    CHECK(0, "the band's top edge gives less than one real frame");
                    return;
                }
                if (f_old + f_old / 32 < real)
                    old_short++;
                checked++;
            }
        }
    }
    CHECK(checked > 400, "the whole table, both chips, five clocks");
    CHECK_EQ_I(old_short, checked);         /* the old band: short on every one */
}

TEST(the_counters_are_logged_once_although_three_points_may_log) {
    /* flip_stats_log runs when exclusive mode ends, when the PDEV's mode
     * leaves the screen and when its DirectDraw is disabled. It logs only when
     * vcr_flip_stats_any says something is new since the last log. */
    vcr_flip_state s;
    memset(&s, 0, sizeof s);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 0);          /* a PDEV that never flipped */
    vcr_flip_note_poll(&s, 20);
    vcr_flip_begin(&s, 0, 1, 0, 1000, 47376);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 1);          /* a session: log it */
    /* logged (DrvAssertMode FALSE) with the last flip still in flight */
    vcr_flip_stats_reset(&s);
    CHECK_EQ_U(s.flips, 1);
    CHECK_EQ_U(s.carried, 1);
    /* OLD check (`!flips && !polls` = nothing): it would log that same flip
     * again at DrvDisableDirectDraw, as a second "session" */
    CHECK_EQ_I(s.flips || s.polls, 1);
    /* NEW: nothing new */
    CHECK_EQ_I(vcr_flip_stats_any(&s), 0);
    /* a poll of the carried flip is new; its completing is new */
    vcr_flip_note_poll(&s, 20);
    CHECK_EQ_I(vcr_flip_poll(&s, 1, 0, 2000), 0);   /* active display: still pending */
    CHECK_EQ_I(vcr_flip_stats_any(&s), 1);
    vcr_flip_stats_reset(&s);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 0);
    CHECK_EQ_I(vcr_flip_poll(&s, 0, 0, 1000 + 47377), 1);   /* by the deadline */
    CHECK_EQ_U(s.by_deadline, 1);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 1);
    CHECK_EQ_U(s.flips, s.by_retrace + s.by_deadline + s.superseded + s.pending);
    vcr_flip_stats_reset(&s);                       /* nothing in flight: all zero */
    CHECK_EQ_U(s.carried, 0);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 0);
    /* a carried flip replaced by a new one is new */
    vcr_flip_begin(&s, 0, 1, 0, 3000, 47376);
    vcr_flip_stats_reset(&s);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 0);
    vcr_flip_begin(&s, 0, 1, 0, 4000, 47376);
    CHECK_EQ_I(vcr_flip_stats_any(&s), 1);
    CHECK_EQ_U(s.superseded, 1);
}

TEST(the_vblank_ioctl_keeps_its_size_and_offsets) {
    /* refresh_mhz is the word that was `reserved`: either half of the driver
     * pair may be the older one */
    CHECK_EQ_U(sizeof(vcr_dd_vblank), 16);
    CHECK_EQ_U(offsetof(vcr_dd_vblank, in_vblank), 0);
    CHECK_EQ_U(offsetof(vcr_dd_vblank, scanline), 4);
    CHECK_EQ_U(offsetof(vcr_dd_vblank, scan_offset), 8);
    CHECK_EQ_U(offsetof(vcr_dd_vblank, refresh_mhz), 12);
    CHECK_EQ_U(sizeof(vcr_flip_state) > 0, 1);
}

MUNIT_MAIN("vcr-kmd flip completion", {
    modes_init();
    RUN(the_timeline_is_the_drivers_own_modes);
    RUN(the_default_deadline_is_todays_to_the_tick);
    RUN(the_achieved_deadline_is_one_real_frame_plus_a_32nd);
    RUN(never_early_with_the_write_swept_over_every_line);
    RUN(a_sample_taken_before_the_write_completes_flips_early);
    RUN(a_spinner_completes_exactly_once_per_refresh);
    RUN(the_d3d_pattern_old_rule_0_889_new_rule_0_95);
    RUN(a_novsync_flip_is_never_pending);
    RUN(the_rule_without_a_retrace_bit_is_the_deadline);
    RUN(the_one_assumption_the_flag_does_not_lead_the_latch);
    RUN(the_trust_band_is_narrower_than_the_margin);
    RUN(every_timing_keeps_the_achieved_rule_and_its_band_is_never_early);
    RUN(the_counters_are_logged_once_although_three_points_may_log);
    RUN(the_vblank_ioctl_keeps_its_size_and_offsets);
})
