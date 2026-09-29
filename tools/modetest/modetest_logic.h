/*
 * modetest_logic.h - every decision modetest.exe makes BEFORE it touches the
 * display, and the arithmetic that turns vertical-blank timestamps into a
 * refresh rate. Free of <windows.h> so tests/native/test_modetest_logic.c
 * compiles the same code the exe runs (the refreshlogic.h / gameres.h
 * arrangement).
 *
 * WHAT THE TOOL IS FOR. The fleet wants every Quake-era title at the highest
 * refresh the monitor supports AT THE TITLE'S OWN RESOLUTION, and most of those
 * engines set their mode with ChangeDisplaySettings / SetDisplayMode naming NO
 * refresh (WON hl.exe: dmFields 0x1C0000, CDS_FULLSCREEN; Quake II ref_gl:
 * 0x180000). What a no-rate request lands on is a property of each box's
 * OS + driver, and the one measurement this project has (agent/tools/
 * refreshlogic.h, .124 GeForce2 GTS / ForceWare 71.89, 2026-08-25) says XP
 * gave 60 Hz even with the desktop already at 100 Hz at the same resolution.
 * Whether a LAUNCHER could pre-switch to the best rate and have the game's
 * no-rate request keep it is exactly the kind of thing that must be measured
 * per driver, not assumed - hence this tool.
 *
 * THE RULES HERE ARE SAFETY RULES, each one a way a mode test can hurt a box:
 *   - a mode must be one the driver ENUMERATES (width, height AND depth);
 *   - never bigger than the PERSISTED desktop - the mode the box is set up to
 *     run; on a CRT that is the tube's comfortable size, on an LCD its native;
 *   - an explicit rate must be listed for that exact mode, and real (50..199;
 *     0/1 are "driver default" sentinels, never a rate);
 *   - "max" needs a CEILING: the EDID's vertical maximum, or an operator's
 *     -maxhz for a tube with no EDID. No ceiling, no "max" - with no EDID the
 *     good outcome of guessing on an analogue CRT is "out of range" on a
 *     screen nobody is standing in front of (the gameres.h rule);
 *   - refuse to start when the LIVE mode is not the PERSISTED one: something
 *     else (a game, another session) owns the screen right now;
 *   - pace: >= MT_PACE_MIN_S between switches (plus vcr_pace.h's cross-process
 *     floor in the exe), and a bounded number of TEST switches per run - a
 *     give-back is never refused by that count (mt_switch_allowed);
 *   - give back only what this run took (mt_should_giveback);
 *   - -nobpp only at the persisted depth (mt_check_nobpp).
 */
#ifndef MODETEST_LOGIC_H
#define MODETEST_LOGIC_H

/* Real refresh band - keep in step with agent/shared/gameres.h GR_HZ_MIN /
 * GR_HZ_MAX and agent/tools/refreshlogic.h (derived on .124 from the driver's
 * real enumeration). */
#define MT_HZ_MIN       50
#define MT_HZ_MAX       200             /* exclusive */

#define MT_MAX_MODES    1024
#define MT_PACE_MIN_S   5               /* the task's floor; vcr_pace.h adds its own 3 s */
#define MT_PACE_MAX_S   30
#define MT_PACE_DEF_S   6
#define MT_HOLD_MIN_S   2
#define MT_HOLD_MAX_S   20
#define MT_HOLD_DEF_S   4
#define MT_MAX_SWITCHES 12              /* a run that plans more is refused */

typedef struct { int w, h, bpp, hz; } mt_mode_t;

/* refusal reasons (exit code 2 with one of these named in the log) */
enum {
    MT_OK = 0,
    MT_E_NOT_LISTED,        /* W x H x BPP is not in the driver's list      */
    MT_E_BIGGER,            /* bigger than the persisted desktop            */
    MT_E_RATE_NOT_REAL,     /* an explicit rate outside 50..199             */
    MT_E_RATE_NOT_LISTED,   /* explicit rate not listed for that exact mode */
    MT_E_RATE_OVER_CEILING, /* explicit rate above the EDID / -maxhz ceiling */
    MT_E_NO_CEILING,        /* "max" asked for with no EDID and no -maxhz   */
    MT_E_NO_RATE,           /* "max": nothing real listed under the ceiling */
    MT_E_LIVE_NOT_PERSISTED,/* the screen is not in its persisted mode      */
    MT_E_BPP,               /* depth other than 8/16/24/32                  */
    MT_E_TOO_MANY_SWITCHES, /* the plan exceeds MT_MAX_SWITCHES             */
    MT_E_NOBPP_DEPTH,       /* -nobpp with a depth that is not the persisted
                             * one: the OS fills the depth in itself, so the
                             * mode it lands on is not the one checked      */
    MT_E_LAST = MT_E_NOBPP_DEPTH
};

static const char *mt_reason(int r)
{
    switch (r) {
    case MT_OK:                   return "ok";
    case MT_E_NOT_LISTED:         return "mode not in the driver's list";
    case MT_E_BIGGER:             return "bigger than the persisted desktop mode";
    case MT_E_RATE_NOT_REAL:      return "rate is not a real refresh (50..199 Hz)";
    case MT_E_RATE_NOT_LISTED:    return "rate not listed by the driver for that exact mode";
    case MT_E_RATE_OVER_CEILING:  return "rate above the monitor ceiling (EDID vmax/hmax, -maxhz)";
    case MT_E_NO_CEILING:         return "'max' needs a ceiling: no EDID and no -maxhz";
    case MT_E_NO_RATE:            return "'max': no real listed rate under the ceiling";
    case MT_E_LIVE_NOT_PERSISTED: return "live mode is not the persisted mode - the screen is in use";
    case MT_E_BPP:                return "depth must be 8, 16, 24 or 32";
    case MT_E_TOO_MANY_SWITCHES:  return "plan exceeds the per-run switch limit";
    case MT_E_NOBPP_DEPTH:        return "-nobpp needs BPP = the persisted depth (the OS keeps "
                                         "that depth, so any other BPP checks the wrong mode)";
    default:                      return "unknown";
    }
}

static int mt_hz_is_real(int hz) { return hz >= MT_HZ_MIN && hz < MT_HZ_MAX; }

/* W x H x BPP offered at any rate (sentinels included - a Win9x driver lists
 * every mode at 0 Hz and the mode is still real). */
static int mt_has_mode(const mt_mode_t *l, int n, int w, int h, int bpp)
{
    int i;
    for (i = 0; i < n; i++)
        if (l[i].w == w && l[i].h == h && l[i].bpp == bpp)
            return 1;
    return 0;
}

/* this exact rate listed for W x H x BPP */
static int mt_has_rate(const mt_mode_t *l, int n, int w, int h, int bpp, int hz)
{
    int i;
    for (i = 0; i < n; i++)
        if (l[i].w == w && l[i].h == h && l[i].bpp == bpp && l[i].hz == hz)
            return 1;
    return 0;
}

/* The ceiling "max" and explicit rates are checked against: the lower of the
 * EDID vertical maximum and an operator's -maxhz, 0 when neither exists. */
static int mt_ceiling(int edid_vmax, int op_maxhz)
{
    if (edid_vmax > 0 && op_maxhz > 0)
        return edid_vmax < op_maxhz ? edid_vmax : op_maxhz;
    return edid_vmax > 0 ? edid_vmax : (op_maxhz > 0 ? op_maxhz : 0);
}

/*
 * The HORIZONTAL half of the monitor's range. A CRT is driven out of range by
 * its line rate as surely as by its frame rate: .124's old Sony CPD-G200
 * (96 kHz) could not take 1600x1200@85 (106 kHz) although 85 Hz was inside
 * its vertical range - vcr-kmd's vcr_mode_check filters on BOTH for that
 * reason. The line rate is estimated as h * hz * 1.06: the vertical total of
 * the DMT/GTF timings at 60..120 Hz is 1.04..1.06 of the visible lines at the
 * resolutions where a tube's limit is reached (1024x768@85 808/768 = 1.052,
 * 1280x960@85 1011/960 = 1.053, 1600x1200 1250/1200 = 1.042; GTF at 100 Hz
 * about 1.058). hmax_khz 0 = no horizontal limit known: 1.
 */
static int mt_hfreq_ok(int h, int hz, int hmax_khz)
{
    long est_hz;
    if (hmax_khz <= 0 || hz <= 0)
        return 1;
    est_hz = (long)h * (long)hz * 106l / 100l;      /* lines per second */
    return est_hz <= (long)hmax_khz * 1000l;
}

/* The highest REAL rate the driver lists for W x H x BPP at or under `cap`
 * (cap 0 = no cap) and inside a known horizontal limit. 0 = none. */
static int mt_best_rate(const mt_mode_t *l, int n, int w, int h, int bpp, int cap,
                        int hmax_khz)
{
    int i, best = 0;
    for (i = 0; i < n; i++) {
        if (l[i].w != w || l[i].h != h || l[i].bpp != bpp)
            continue;
        if (!mt_hz_is_real(l[i].hz))
            continue;
        if (cap > 0 && l[i].hz > cap)
            continue;
        if (!mt_hfreq_ok(h, l[i].hz, hmax_khz))
            continue;
        if (l[i].hz > best)
            best = l[i].hz;
    }
    return best;
}

/*
 * The live screen must be the persisted one before anything switches.
 * Resolution and depth must match. The rate must match too WHEN BOTH ARE
 * REAL: Win9x reports 0 for the live mode (measured on .243: DISPLAYCFG get
 * -> refresh 0, registry_refresh 75), and that is "unknown", not "different".
 */
static int mt_live_is_persisted(const mt_mode_t *live, const mt_mode_t *reg)
{
    if (live->w != reg->w || live->h != reg->h || live->bpp != reg->bpp)
        return 0;
    if (mt_hz_is_real(live->hz) && mt_hz_is_real(reg->hz) && live->hz != reg->hz)
        return 0;
    return 1;
}

/*
 * Check one request. want_hz: 0 = no rate (the game's own request), > 0 an
 * explicit rate, -1 = "max" (the best listed under the ceiling). *out_hz gets
 * the rate the tool will name (0 for a no-rate request). Returns MT_OK or a
 * refusal.
 */
static int mt_check_request(const mt_mode_t *l, int n, const mt_mode_t *reg,
                            int edid_vmax, int edid_hmax, int op_maxhz,
                            int w, int h, int bpp, int want_hz, int *out_hz)
{
    int ceil = mt_ceiling(edid_vmax, op_maxhz);

    *out_hz = 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
        return MT_E_BPP;
    if (!mt_has_mode(l, n, w, h, bpp))
        return MT_E_NOT_LISTED;
    if (w > reg->w || h > reg->h)
        return MT_E_BIGGER;
    if (want_hz == 0)
        return MT_OK;
    if (want_hz < 0) {
        if (!ceil)
            return MT_E_NO_CEILING;
        *out_hz = mt_best_rate(l, n, w, h, bpp, ceil, edid_hmax);
        return *out_hz ? MT_OK : MT_E_NO_RATE;
    }
    if (!mt_hz_is_real(want_hz))
        return MT_E_RATE_NOT_REAL;
    if (!mt_has_rate(l, n, w, h, bpp, want_hz))
        return MT_E_RATE_NOT_LISTED;
    /* an explicit rate above a KNOWN ceiling is refused; with no ceiling the
     * driver's own list is the only evidence and the rate must be on it */
    if (ceil && want_hz > ceil)
        return MT_E_RATE_OVER_CEILING;
    if (!mt_hfreq_ok(h, want_hz, edid_hmax))
        return MT_E_RATE_OVER_CEILING;
    *out_hz = want_hz;
    return MT_OK;
}

/*
 * -nobpp sends W|H only (Quake II ref_gl's 0x180000) and the OS fills in the
 * depth itself - from the persisted mode. mt_check_request() validated
 * W x H x BPP, so unless BPP IS the persisted depth the mode the OS lands on
 * is one nothing checked against the driver's list (review 2026-09-29).
 */
static int mt_check_nobpp(int nobpp, int bpp, const mt_mode_t *reg)
{
    return (nobpp && bpp != reg->bpp) ? MT_E_NOBPP_DEPTH : MT_OK;
}

/*
 * THE PER-RUN SWITCH LIMIT COUNTS TEST SWITCHES ONLY. A give-back (a restore,
 * RestoreDisplayMode, a device release) is never refused by a count: refusing
 * it strands the screen in a test mode - on Win9x for good, because nothing
 * reverts a CDS_FULLSCREEN mode when the process ends there. It still goes
 * through the pace gate. (Until 2026-09-29 one counter covered both and the
 * restore could be refused after MT_MAX_SWITCHES + 2 switches, contradicting
 * the comment above it.)
 */
static int mt_switch_allowed(int test_switches_made, int giveback)
{
    return giveback || test_switches_made < MT_MAX_SWITCHES;
}

/*
 * Should a watchdog or the final check put the persisted mode back? Only when
 * THIS RUN switched something and the screen is not in its persisted mode. A
 * run that never switched gives nothing back: a mode it did not make belongs
 * to whoever made it (a game the 1080p workflow started mid-run), and yanking
 * it is the fault, not the fix.
 */
static int mt_should_giveback(int switches_made, const mt_mode_t *live, const mt_mode_t *reg)
{
    return switches_made > 0 && !mt_live_is_persisted(live, reg);
}

/* ------------------------------------------------------------------ */
/* the plan                                                             */
/* ------------------------------------------------------------------ */

#define MT_T_A   0x01   /* CDS W/H/BPP, no rate, CDS_FULLSCREEN          */
#define MT_T_B   0x02   /* CDS at a rate, then CDS no rate                */
#define MT_T_C   0x04   /* DirectDraw SetDisplayMode(W,H,BPP,0)           */
#define MT_T_CB  0x08   /* CDS at a rate, then DirectDraw rate 0          */
#define MT_T_D   0x10   /* Direct3D 9 fullscreen, RefreshRateInHz 0       */

/* Mode switches a test makes, counting every give-back: A = in, restore;
 * B = at-rate, no-rate, restore; C = SetDisplayMode, RestoreDisplayMode (+1
 * corrective restore budgeted); CB = at-rate, SetDisplayMode, Restore, restore;
 * D = CreateDevice, Release (+1 corrective). */
static int mt_plan_switches(unsigned tests)
{
    int n = 0;
    if (tests & MT_T_A)  n += 2;
    if (tests & MT_T_B)  n += 3;
    if (tests & MT_T_C)  n += 3;
    if (tests & MT_T_CB) n += 4;
    if (tests & MT_T_D)  n += 3;
    return n;
}

/* ------------------------------------------------------------------ */
/* the measured refresh                                                 */
/* ------------------------------------------------------------------ */

/*
 * Refresh from vertical-blank timestamps: the MEDIAN interval between
 * consecutive vblank starts, as milli-Hz. A driver can report a rate it does
 * not scan out (Win9x reports 0 for everything; a "default" entry means
 * nothing), so the tool times the real scanout with IDirectDraw::
 * WaitForVerticalBlank and QueryPerformanceCounter. Returns 0 when the
 * samples are not trustworthy:
 *   - fewer than 10 intervals;
 *   - under 70% of intervals within 3% of the median (missed blanks, a busy
 *     driver, a WaitForVerticalBlank that returns at once);
 *   - a result outside 40..250 Hz.
 * iv[] is sorted in place. `freq` is QueryPerformanceFrequency (ticks/s).
 * Plain double arithmetic on purpose: no 64-bit division helper is pulled
 * into a CRT-free exe that must run on a Pentium without CMOV.
 */
static unsigned long mt_vblank_mhz(double *iv, int n, double freq)
{
    int i, j, close_n;
    double med, lo, hi, mhz;

    if (n < 10 || freq <= 0.0)
        return 0;
    for (i = 1; i < n; i++) {                   /* insertion sort, n <= ~200 */
        double v = iv[i];
        for (j = i - 1; j >= 0 && iv[j] > v; j--)
            iv[j + 1] = iv[j];
        iv[j + 1] = v;
    }
    med = (n & 1) ? iv[n / 2] : (iv[n / 2 - 1] + iv[n / 2]) / 2.0;
    if (med <= 0.0)
        return 0;
    lo = med * 0.97;
    hi = med * 1.03;
    for (i = 0, close_n = 0; i < n; i++)
        if (iv[i] >= lo && iv[i] <= hi)
            close_n++;
    if (close_n * 10 < n * 7)
        return 0;
    mhz = freq * 1000.0 / med;
    if (mhz < 40000.0 || mhz > 250000.0)
        return 0;
    return (unsigned long)(mhz + 0.5);
}

/* nearest whole Hz of a milli-Hz measurement (0 stays 0) */
static int mt_mhz_to_hz(unsigned long mhz) { return (int)((mhz + 500ul) / 1000ul); }

/*
 * Did a switch LAND on the rate we can see? `eds_hz` is what
 * EnumDisplaySettings(ENUM_CURRENT_SETTINGS) says, `vb_mhz` the timed
 * scanout (0 = unmeasured). The verdict prefers the measurement:
 *   2 = measured and it agrees with eds (within 1.5 Hz)
 *   1 = only one of the two is available
 *   0 = neither
 *  -1 = measured and it DISAGREES with eds - the driver reports one rate and
 *       scans out another; the log must say so loudly.
 */
static int mt_rate_agreement(int eds_hz, unsigned long vb_mhz)
{
    int have_eds = mt_hz_is_real(eds_hz), have_vb = vb_mhz != 0;
    long d;
    if (have_eds && have_vb) {
        d = (long)vb_mhz - (long)eds_hz * 1000l;
        if (d < 0) d = -d;
        return d <= 1500 ? 2 : -1;
    }
    return (have_eds || have_vb) ? 1 : 0;
}

/* The rate an observation vouches for: the timed scanout when there is one
 * (a driver can report one rate and scan out another; Win9x reports 0), else
 * a REAL reported rate, else 0 = unmeasured. */
static int mt_observed_hz(int eds_hz, unsigned long vb_mhz)
{
    if (vb_mhz)
        return mt_mhz_to_hz(vb_mhz);
    return mt_hz_is_real(eds_hz) ? eds_hz : 0;
}

/*
 * Test B / CB: did the no-rate request KEEP the pre-switched rate? The answer
 * is only meaningful when the PRE-SWITCH ITSELF landed on the rate it asked
 * for - a driver that turned "@75" into 60 makes "the no-rate request then
 * gave 60" a statement about nothing. (Until 2026-09-29 the verdict compared
 * the no-rate result with the REQUESTED rate and would have printed "DID NOT
 * KEEP" for a pre-switch that never happened.)
 */
#define MT_V_UNMEASURED  0      /* one of the two observations has no rate  */
#define MT_V_PRE_MISSED  1      /* the pre-switch did not land on want_hz   */
#define MT_V_KEPT        2
#define MT_V_NOT_KEPT    3

static int mt_keep_verdict(int want_hz, int pre_hz, int post_hz)
{
    if (!pre_hz || !post_hz)
        return MT_V_UNMEASURED;
    if (pre_hz != want_hz)
        return MT_V_PRE_MISSED;
    return post_hz == pre_hz ? MT_V_KEPT : MT_V_NOT_KEPT;
}

static const char *mt_keep_verdict_name(int v)
{
    switch (v) {
    case MT_V_KEPT:       return "KEPT";
    case MT_V_NOT_KEPT:   return "DID NOT KEEP";
    case MT_V_PRE_MISSED: return "INVALID - the pre-switch itself did not land on its rate";
    default:              return "UNMEASURED - no rate from the driver or the scanout";
    }
}

#endif /* MODETEST_LOGIC_H */
