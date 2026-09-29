/*
 * gameres.h - which resolution does THIS box's monitor want, and where does
 *             each staged game keep it?
 *
 * ONE staged library deploys to every machine on the fleet, and the fleet is
 * four 1920x1080 LCDs and four CRTs - one of which is a 4:3 tube that was
 * being driven at 5:4. A resolution written into a staged config is therefore
 * wrong somewhere BY CONSTRUCTION, which is why the answer has to be computed
 * on the box.
 *
 * WHY THIS IS IN THE AGENT AND NOT ONLY IN FLEETRES.EXE. FLEETRES runs from a
 * title's "Play <Game>.bat" at launch, and that covers the titles whose mode
 * lives on a COMMAND LINE. It does not cover the state GAMESYNC itself
 * writes: gs_merge_reg() applies each title's staged install.reg on every
 * single sync, and Half-Life's install.reg pins
 *
 *     HKCU\Software\Valve\Half-Life\Settings  ScreenWidth=0x400 ScreenHeight=0x300
 *
 * - 1024x768 - on every box, and that ONE registry key is shared by every
 * GoldSrc title on the machine (there is no Software\Valve\CounterStrike key
 * at all; read live on .240). Its own comment records that Counter-Strike
 * "ignores -w/-h on the command line for the same reason". So on a 1080p box
 * the staged library actively re-pins Counter-Strike to 1024x768 at every
 * sync, and no launcher can undo it. A resolution pass that runs at the END
 * of a title's sync - after the tree is copied and after install.reg is
 * merged - is the only place that can.
 *
 * Header-only and free of Win32 so the code the agent runs is exactly the code
 * the regression test compiles (tests/native/test_gameres.c), the same
 * arrangement gamegate.h uses.
 *
 * THE DECISION IS A PORT OF provisioning/fleetres/fleetres.c, which was
 * developed against the real hardware. Keep the two in step; the details that
 * look arbitrary are each a measurement:
 *
 *   - the target comes from the PERSISTED desktop mode, never the live one. A
 *     game that exits without restoring leaves the desktop at 640x480, and
 *     .123 and .240 were both found sitting there. A tool that trusts the live
 *     mode and then WRITES its conclusion pins the box at 640x480 for good.
 *   - MODE aspect bands are tight and PHYSICAL aspect bands are wide. Using
 *     the physical bands on a mode put 1280x1024 (1.250) inside the 4:3 band
 *     and kept handing .171 a 5:4 mode for a 4:3 tube.
 *   - no EDID at all means assume a 4:3 TUBE, not "believe the desktop". .133
 *     lost its EDID across a reboot and was instantly handed 1280x1024 back.
 *   - a mode must be one the driver actually OFFERS. Fitting inside the target
 *     is not enough: on .246 an unofferable 1152x864 made RTCW set the desktop
 *     to 1280x960 and then draw into a window with r_fullscreen still 1 - it
 *     neither errored nor did what was asked.
 */
#ifndef RETRO_GAMERES_H
#define RETRO_GAMERES_H

#include <string.h>
#include <stdio.h>
#include <math.h>

#if defined(__GNUC__)
#  define GR_FN   static __attribute__((unused))
#  define GR_DATA static __attribute__((unused))
#else
#  define GR_FN   static
#  define GR_DATA static
#endif

#define GR_MAX_MODES 512

/* A mode the DRIVER offers: the best real refresh seen for it, and every real
 * rate it was listed at (up to GR_MODE_RATES). The full set exists for the
 * no-EDID rule in gr_target_hz(): "is the persisted desktop's own rate listed
 * at THIS resolution" cannot be answered from the best rate alone. */
#define GR_MODE_RATES 12
typedef struct {
    int w, h, hz;
    unsigned char nr;                   /* how many of r[] are filled        */
    unsigned char r[GR_MODE_RATES];     /* real rates, 50..199, as listed    */
} gr_mode_t;

/* A bare resolution. The fixed engine tables and the 4:3 ladder are lists of
 * resolutions, not of driver modes - a refresh field there would be
 * meaningless and would have to be initialised in thirty places. */
typedef struct { int w, h; } gr_res_t;

typedef struct {
    gr_mode_t m[GR_MAX_MODES];
    int       n;
    /* The EDID vertical-refresh ceiling, set ONCE before the modes are added.
     * It has to be applied at INSERT time, not at read time: only the best
     * rate per resolution is kept, so a driver that offers 60/85/120 where the
     * panel tops out at 100 would otherwise store 120 and leave nothing to
     * fall back to. 0 = no EDID, so no measurement and no clamp. */
    int       hz_cap;
} gr_modes_t;

/* The panel, as agent/shared/edid.h reports it - repeated here as plain ints
 * so this header stays Win32-free and the native test can build a panel by
 * hand without a windows.h. */
typedef struct {
    int ok;                     /* an EDID was actually read                 */
    int native_w, native_h;     /* preferred detailed timing                 */
    int native_hz;
    int digital;                /* digital-input bit                         */
    int vmax;                   /* max vertical refresh, 0xFD descriptor     */
    int hcm, vcm;               /* physical size, cm                         */
} gr_panel_t;

/* Everything a per-title rule can substitute. */
typedef struct {
    int  w, h;                  /* widescreen-capable engines                */
    int  w43, h43;              /* engines with no widescreen mode           */
    /* THE HIGHEST REFRESH THE MONITOR SUPPORTS AT EACH TARGET - not one
     * number for the box. A rate is offered per resolution, so an engine
     * running at the 4:3 target may have a different ceiling from one running
     * at the panel's native mode, and a single `hz` would be wrong for one of
     * them. 0 means "not known - leave the refresh alone", never 60.
     * gr_target_hz() decides each one - the SAME function FLEETRES.EXE calls
     * for FR_HZW / FR_HZ43 / FR_HZQ2 / FR_HZQ3, so the two writers of a
     * shared cfg can never disagree about the number. */
    int  hz;                    /* rate for a title at w x h     (FR_HZW)    */
    int  hz43;                  /* rate for a title at w43 x h43 (FR_HZ43)   */
    int  hzq2, hzq3;            /* the same at the id Tech 2 / id Tech 3
                                 * INDEX mode (gr_q2tab[q2mode],
                                 * gr_q3tab[q3mode]) - SoF2 and RTCW render
                                 * there, not at w43 x h43 (1152x864 vs
                                 * 1280x960 on the 1080p boxes)             */
    int  hz_src;                /* where hz/hz43/hzq2/hzq3 came from:
                                 * GR_HZSRC_EDID / _PERSISTED / _NONE         */
    int  desk_hz;               /* best real rate at the persisted desktop   */
    int  fr_hz;                 /* the persisted mode's OWN rate - what
                                 * FLEETRES publishes as FR_HZ. Used only
                                 * where a launcher writes the same file, so
                                 * the two writers agree byte for byte.       */
    int  bpp;
    int  fov;                   /* hor+ FOV preserving the 4:3 vertical FOV  */
    int  q2mode, q3mode;        /* id Tech 2 / id Tech 3 mode-table indices  */
    int  d3ar;                  /* id Tech 4 r_aspectRatio 0=4:3 1=16:9 2=16:10 */
    int  wide;                  /* 1 when the target is wider than 4:3       */
    int  lcd;                   /* 1 = flat panel, 0 = tube                  */
    int  edid;                  /* 1 = measured, 0 = inferred                */
    char aspect[16];            /* "16:9", "4:3", ...                        */
} gr_target_t;

/* ------------------------------------------------------------------ */
/* mode list                                                            */
/* ------------------------------------------------------------------ */

GR_FN void gr_modes_reset(gr_modes_t *l) { l->n = 0; l->hz_cap = 0; }

GR_FN int gr_modes_have(const gr_modes_t *l, int w, int h)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->m[i].w == w && l->m[i].h == h)
            return 1;
    return 0;
}

/*
 * Is this a real refresh rate, or one of the sentinels?
 *
 * EnumDisplaySettings reports 0 and 1 Hz "use the driver default" entries and
 * they are not rates - picking one asks the monitor for nothing at all. The
 * band is the one agent/tools/refreshlogic.h already uses, derived on .124
 * from setrefresh.exe's real enumeration (60 70 72 75 85 100 at 1024x768x32).
 * Keep the two in step.
 */
#define GR_HZ_MIN 50
#define GR_HZ_MAX 200
GR_FN int gr_hz_is_real(int hz) { return hz >= GR_HZ_MIN && hz < GR_HZ_MAX; }

/* Add a mode, keeping the HIGHEST real refresh seen for that resolution. A
 * driver enumerates one entry per (resolution, depth, rate), so the same WxH
 * arrives many times and only the best rate is worth remembering. */
/* remember one more REAL rate a mode was listed at (duplicates ignored) */
GR_FN void gr_mode_note_rate(gr_mode_t *m, int hz)
{
    int k;
    if (!gr_hz_is_real(hz))
        return;
    for (k = 0; k < m->nr; k++)
        if (m->r[k] == (unsigned char)hz)
            return;
    if (m->nr < GR_MODE_RATES)
        m->r[m->nr++] = (unsigned char)hz;
}

GR_FN void gr_modes_add(gr_modes_t *l, int w, int h, int hz)
{
    int i;
    if (w < 320 || h < 200)
        return;
    if (!gr_hz_is_real(hz) || (l->hz_cap > 0 && hz > l->hz_cap))
        hz = 0;                 /* a sentinel, or past what the panel syncs */
    for (i = 0; i < l->n; i++)
        if (l->m[i].w == w && l->m[i].h == h) {
            if (hz > l->m[i].hz) l->m[i].hz = hz;
            gr_mode_note_rate(&l->m[i], hz);
            return;
        }
    if (l->n < GR_MAX_MODES) {
        l->m[l->n].w = w;
        l->m[l->n].h = h;
        l->m[l->n].hz = hz;
        l->m[l->n].nr = 0;
        gr_mode_note_rate(&l->m[l->n], hz);
        l->n++;
    }
}

/* Was w x h listed at exactly this real rate (after the insert-time cap)? */
GR_FN int gr_has_rate(const gr_modes_t *l, int w, int h, int hz)
{
    int i, k;
    if (!gr_hz_is_real(hz))
        return 0;
    for (i = 0; i < l->n; i++)
        if (l->m[i].w == w && l->m[i].h == h) {
            for (k = 0; k < l->m[i].nr; k++)
                if (l->m[i].r[k] == (unsigned char)hz)
                    return 1;
            return 0;
        }
    return 0;
}

/*
 * THE HIGHEST REFRESH THIS MONITOR SUPPORTS AT THIS RESOLUTION.
 *
 * Two ceilings, and both are load-bearing:
 *
 *  - the DRIVER's own list FOR THAT RESOLUTION. A rate is only offered at
 *    some modes; asking for one the driver does not enumerate is how you get
 *    a black screen on a CRT.
 *  - the EDID vertical-refresh MAXIMUM, applied as `hz_cap` when the modes
 *    were added. Drivers list modes an analogue monitor cannot sync, and on a
 *    tube "out of range" is the GOOD outcome. With no EDID there is no
 *    measurement, so there is no clamp and no claim.
 *
 * Returns 0 when nothing real is known, which callers must read as "leave the
 * refresh alone" - never as "60". A hardcoded 60 is a staged constant like any
 * other and is wrong on every CRT here: .143 runs 100 Hz, .133 85, .124 75.
 */
GR_FN int gr_best_hz(const gr_modes_t *l, int w, int h)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->m[i].w == w && l->m[i].h == h)
            return l->m[i].hz;
    return 0;
}

/* where a per-target rate came from (gr_target_t.hz_src, FR_HZSRC) */
#define GR_HZSRC_NONE      0    /* nothing vouches for a rate: leave it alone  */
#define GR_HZSRC_EDID      1    /* the driver's list under the EDID ceiling    */
#define GR_HZSRC_PERSISTED 2    /* no ceiling: the desktop's own, shown rate   */

/* A MEASURED ceiling: an EDID that states a vertical range. An EDID without a
 * range descriptor (legal in 1.4) has no ceiling to cap a list with - the
 * list was built uncapped - so it is treated like no EDID at all. */
GR_FN int gr_have_ceiling(const gr_panel_t *p)
{
    return p->ok && p->vmax > 0;
}

/*
 * THE REFRESH A TITLE RUNNING AT w x h IS TOLD TO ASK FOR - one function,
 * called by BOTH writers of a shared cfg: GAMERES (gr_decide below, the
 * %HZW% %HZ43% %HZQ2% %HZQ3% tokens) and FLEETRES.EXE (FR_HZW FR_HZ43 FR_HZQ2
 * FR_HZQ3). Two copies of this rule would be two answers; the id Tech 3
 * fleetres.cfg is rewritten by each writer whenever it disagrees with the
 * other, and the "0 value(s) changed" signal dies with it.
 *
 * WITH A MEASURED CEILING: gr_best_hz - the best rate the driver lists at
 * w x h, the EDID maximum already applied as the list was built.
 *
 * WITHOUT ONE there is no measurement of the MONITOR - only of the mode the
 * tube is showing right now, the persisted desktop. That vouches for exactly
 * one rate: its own, and only at a resolution no bigger than itself (the same
 * vertical rate over fewer lines is a lower line rate - inside what the tube
 * is demonstrably syncing) and only where the driver LISTS that exact rate.
 * Anything else answers 0: leave the refresh alone. This keeps what FR_HZ
 * already gave a no-EDID tube at its own desktop mode (.143's 100 Hz at
 * 1024x768) without ever claiming a rate the tube has not been seen to take -
 * and never the driver's unclamped best, which is what the card can do, not
 * what the monitor can.
 */
GR_FN int gr_target_hz(const gr_panel_t *p, const gr_modes_t *l, int w, int h,
                       int reg_w, int reg_h, int reg_hz, int *src)
{
    int hz;
    if (gr_have_ceiling(p)) {
        hz = gr_best_hz(l, w, h);
        if (src) *src = hz ? GR_HZSRC_EDID : GR_HZSRC_NONE;
        return hz;
    }
    if (gr_hz_is_real(reg_hz) && w <= reg_w && h <= reg_h &&
        gr_has_rate(l, w, h, reg_hz)) {
        if (src) *src = GR_HZSRC_PERSISTED;
        return reg_hz;
    }
    if (src) *src = GR_HZSRC_NONE;
    return 0;
}

GR_FN const char *gr_hz_src_name(int src)
{
    return src == GR_HZSRC_EDID ? "edid" : src == GR_HZSRC_PERSISTED ? "persisted" : "none";
}

/*
 * FR_HZ / %FRHZ%: the persisted desktop's own rate, 60 when that is not a
 * real rate - ONE formula, called by gr_decide AND by FLEETRES.EXE's FR_HZ
 * line. Until 2026-09-29 each had its own: FLEETRES took reg_hz in 50..240,
 * else the LIVE rate, else 60; this header took reg_hz in 50..199, else 60.
 * On a box whose registry holds the "default" rate (0/1) they disagree by the
 * live rate, and the id Tech 3 fleetres.cfg (r_displayRefresh %FRHZ%) is then
 * rewritten by each writer in turn, forever. The live mode is not a
 * measurement of the monitor (a game can leave it anywhere), so the header's
 * formula is the one both use.
 */
GR_FN int gr_fr_hz(int reg_hz)
{
    return gr_hz_is_real(reg_hz) ? reg_hz : 60;
}

/*
 * FR_HZWHI / %HZWHI%: a per-target rate, but only when it is ABOVE 60 - else
 * 0, "name no rate". For an engine where naming a rate can HURT and naming 60
 * gains nothing. Serious Engine 1 is that engine: its mode set passes
 * DM_DISPLAYFLAGS with DMDISPLAYFLAGS_TEXTMODE (CDS_FULLSCREEN misused) and
 * adds DM_DISPLAYFREQUENCY when gfx_/gap_iRefreshRate > 0; on .195 (Windows
 * 7) TFE with a 60 there got BADMODE for every mode and never started, while
 * TSE - whose line was dead (wrong cvar name) - started fine
 * (evidence-refresh rverify-contemporaries, 2026-09-29). On XP a no-rate mode
 * set lands on 60 anyway (measured: the build VM, ForceWare 71.89, vcr-kmd),
 * so dropping a 60 costs nothing anywhere.
 */
GR_FN int gr_hz_hi(int hz)
{
    return hz > 60 ? hz : 0;
}

/*
 * THE REFRESH A GLIDE TITLE IS HANDED - FR_GLIDEHZ (FLEETRES.EXE), per launch
 * as an environment variable only, never a 3dfx registry value:
 *   Voodoo Banshee/3/4/5 (Glide 3, h3/h5)   FX_GLIDE_REFRESH=<hz>
 *   Voodoo 1/2 (Glide 2.56, SST-1/SST-2)    SSTV2_SCREENREFRESH=<hz>
 * Both REPLACE the rate the application passed to grSstWinOpen (h5
 * minihwc.c hwcInitVideo; cvg video.c sst1InitFindVideoTimingStruct), and
 * getenv beats the 3dfx panel's registry copy of the same name.
 *
 * WHY A RULE OF ITS OWN and not gr_target_hz(): a pass-through Voodoo 1/2
 * generates its OWN video timing, so the 2D card's mode list says nothing
 * about which rates the Voodoo can drive at its mode - on .243 (Win98, Cirrus
 * 5436 + Voodoo 2) that list is 640x480@0 800x600@0 1024x768@75. And the
 * operator can declare the answer outright. In order:
 *
 *  1. declared >= 0 - HKLM\Software\RetroAgent GlideRefreshHz (REG_DWORD),
 *     read by FLEETRES; -1 = absent. 0 = "leave the refresh to Glide and the
 *     3dfx panel" (the kill switch). A real rate is used as given, still
 *     capped at a measured EDID maximum: an operator may lower a claim, never
 *     push one past what the monitor says it syncs.
 *  2. with an EDID ceiling: the best rate the driver lists at w x h (already
 *     capped at insert). For a Voodoo 4/5 that list IS the Glide list; for a
 *     pass-through Voodoo it is the same monitor's.
 *  3. otherwise the persisted desktop's own rate, when w x h is no bigger than
 *     the desktop. The tube is demonstrably syncing that VERTICAL rate at a
 *     HIGHER horizontal frequency (more lines at the same rate), so the same
 *     rate over fewer lines is inside what it has been seen to take: .243
 *     shows 1024x768@75 (~60 kHz) all day, and 640x480@75 is ~37.5 kHz.
 *     Unlike gr_target_hz, no "listed at w x h" test: the Voodoo's timings are
 *     not in any Windows list, which is the whole reason for this function.
 *  4. else 0 - leave it alone, never 60.
 *
 * Then SNAPPED DOWN to what the card accepts - Glide 2.56 honours only
 * 75/85/120 in SSTV2_SCREENREFRESH (anything else silently means 60), and
 * h3/h5 Glide 3 knows 60/70/72/75/80/85/90/100/120 (refConstToRefreshHz) - so
 * a rate is never rounded UP past what was vouched for. A snap that lands
 * below the card's lowest (a Voodoo 2 at 72) is 0: Glide's own 60 stays.
 */
#define GR_GLIDE_SST 1          /* Voodoo Graphics / Voodoo 2 - Glide 2.x    */
#define GR_GLIDE_H3  3          /* Banshee / Voodoo 3 / 4 / 5 - Glide 3.x    */

GR_FN int gr_glide_snap(int hz, int fam)
{
    static const int sst[] = { 120, 85, 75 };
    static const int h3[]  = { 120, 100, 90, 85, 80, 75, 72, 70, 60 };
    const int *t = (fam == GR_GLIDE_SST) ? sst : h3;
    int n = (fam == GR_GLIDE_SST) ? 3 : 9, i;
    if (!gr_hz_is_real(hz))
        return 0;
    for (i = 0; i < n; i++)
        if (t[i] <= hz)
            return t[i];
    return 0;
}

GR_FN int gr_glide_hz(const gr_panel_t *p, const gr_modes_t *l, int w, int h,
                      int reg_w, int reg_h, int reg_hz, int declared, int fam)
{
    int hz = 0;
    if (declared == 0)
        return 0;                               /* the operator's "hands off" */
    if (declared > 0) {
        hz = gr_hz_is_real(declared) ? declared : 0;
    } else {
        if (gr_have_ceiling(p))
            hz = gr_best_hz(l, w, h);
        if (!hz && gr_hz_is_real(reg_hz) && w <= reg_w && h <= reg_h)
            hz = reg_hz;
    }
    if (hz && gr_have_ceiling(p) && hz > p->vmax)
        hz = p->vmax;
    return gr_glide_snap(hz, fam);
}

/*
 * Is a mode one we may ask for?
 *
 * A SHORT LIST IS TREATED AS NO LIST, DELIBERATELY. Some drivers answer
 * ENUM_CURRENT_SETTINGS and then return FALSE at index 0 for the NULL device
 * (measured on .143's GeForce 6800), so an empty enumeration means "could not
 * ask", not "this adapter offers nothing". Refusing every mode there would be
 * far worse than answering approximately - it would drive every title to the
 * 640x480 floor on exactly the boxes whose driver is least cooperative.
 */
GR_FN int gr_mode_offered(const gr_modes_t *l, int w, int h)
{
    return l->n < 4 ? 1 : gr_modes_have(l, w, h);
}

/* ------------------------------------------------------------------ */
/* aspect                                                               */
/* ------------------------------------------------------------------ */

/* Physical sizes are reported in whole centimetres, so the ratio is coarse -
 * a 17" 4:3 tube reads 33x24 = 1.375. The bands are wide on purpose. */
GR_FN int gr_aspect_phys(double r)
{
    if (r > 1.15 && r < 1.45) return 43;    /* a 5:4 PANEL is physically 4:3 */
    if (r >= 1.45 && r < 1.68) return 1610;
    if (r >= 1.68 && r < 2.10) return 169;
    return 0;
}

/* MODE aspects are exact, so their bands must be TIGHT. Using the physical
 * bands here was a real bug: 1280x1024 (1.250) fell inside the 4:3 band and
 * .171 kept being handed a 5:4 mode for its 4:3 tube. */
GR_FN int gr_aspect_mode(double r)
{
    if (r > 1.320 && r < 1.348) return 43;
    if (r > 1.240 && r < 1.260) return 54;
    if (r > 1.580 && r < 1.620) return 1610;
    if (r > 1.760 && r < 1.790) return 169;
    return 0;
}

GR_FN void gr_aspect_str(int w, int h, char *out, size_t cap)
{
    double r = (h > 0) ? (double)w / (double)h : 0.0;
    const char *s = 0;
    if      (r > 1.760 && r < 1.790) s = "16:9";
    else if (r > 1.590 && r < 1.610) s = "16:10";
    else if (r > 1.320 && r < 1.345) s = "4:3";
    else if (r > 1.240 && r < 1.260) s = "5:4";
    if (s) {
        strncpy(out, s, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    sprintf(out, "%.2f", r);
    out[cap - 1] = 0;
}

/*
 * id Tech 3 and GoldSrc are vert-: at 16:9 with the default FOV you see LESS
 * vertically, not more horizontally. This is the horizontal FOV that preserves
 * the 4:3 vertical field of view - 90 at 4:3, 106 at 16:9.
 */
GR_FN int gr_horplus_fov(int w, int h)
{
    double aspect, vhalf, hhalf;
    int f;
    if (w <= 0 || h <= 0)
        return 90;
    aspect = (double)w / (double)h;
    vhalf  = atan(0.75);                    /* tan(vfov/2) at 4:3, fov 90 */
    hhalf  = atan(tan(vhalf) * aspect);
    f = (int)(hhalf * 2.0 * 180.0 / 3.14159265358979 + 0.5);
    if (f < 90)  f = 90;
    if (f > 130) f = 130;
    return f;
}

/* id Tech 4's r_aspectRatio is a SEPARATE cvar from the pixel count and the
 * engine derives horizontal FOV from it, so a 16:9 panel at the right pixel
 * count with the default 0 is still stretched. 0=4:3, 1=16:9, 2=16:10. */
GR_FN int gr_d3_aspect(int w, int h)
{
    switch (gr_aspect_mode((h > 0) ? (double)w / (double)h : 0.0)) {
    case 169:  return 1;
    case 1610: return 2;
    default:   return 0;
    }
}

/* ------------------------------------------------------------------ */
/* the two fixed engine mode tables                                     */
/* ------------------------------------------------------------------ */

/* Quake II / SiN / Soldier of Fortune share id Tech 2's table. No custom mode
 * and no 16:9 entry anywhere - 1600x1200 is the ceiling. */
GR_DATA const gr_res_t gr_q2tab[] = {
    {320,240},{400,300},{512,384},{640,480},{800,600},
    {960,720},{1024,768},{1152,864},{1280,960},{1600,1200}
};
#define GR_Q2TAB_N 10

/*
 * id TECH 3'S TABLE IS NOT id TECH 2'S, AND THE DIFFERENCE BITES AT INDEX 8:
 * id Tech 2's mode 8 is 1280x960 (4:3), id Tech 3's is 1280x1024 (5:4).
 * Handing a Quake III-family engine the id Tech 2 index therefore asks a 16:9
 * panel for a squashed picture - measured on SoF2 on .123.
 *
 * Index 8 and index 11 (856x480) are skipped: one is 5:4 and the other is a
 * 16:9 mode so small that a correctly proportioned 4:3 one beats it on every
 * fleet panel.
 */
GR_DATA const gr_res_t gr_q3tab[] = {
    {320,240},{400,300},{512,384},{640,480},{800,600},{960,720},
    {1024,768},{1152,864},{1280,1024},{1600,1200},{2048,1536},{856,480}
};
#define GR_Q3TAB_N 12

GR_FN int gr_q2_mode_for(const gr_modes_t *l, int w, int h)
{
    int i, best = 3, best_fit = 3;          /* 640x480 floor */
    for (i = 0; i < GR_Q2TAB_N; i++) {
        if (gr_q2tab[i].w > w || gr_q2tab[i].h > h) continue;
        best_fit = i;
        if (gr_mode_offered(l, gr_q2tab[i].w, gr_q2tab[i].h)) best = i;
    }
    if (best > 3) return best;
    return gr_mode_offered(l, gr_q2tab[best_fit].w, gr_q2tab[best_fit].h)
         ? best : best_fit;
}

GR_FN int gr_q3_mode_for(const gr_modes_t *l, int w, int h)
{
    int i, best = 3, best_fit = 3;          /* 640x480 floor */
    for (i = 0; i < GR_Q3TAB_N; i++) {
        if (i == 8 || i == 11) continue;    /* 5:4, and a tiny 16:9 */
        if (gr_q3tab[i].w > w || gr_q3tab[i].h > h) continue;
        best_fit = i;
        if (gr_mode_offered(l, gr_q3tab[i].w, gr_q3tab[i].h)) best = i;
    }
    if (best > 3) return best;
    return gr_mode_offered(l, gr_q3tab[best_fit].w, gr_q3tab[best_fit].h)
         ? best : best_fit;
}

/* ------------------------------------------------------------------ */
/* the decision                                                         */
/* ------------------------------------------------------------------ */

/*
 * The classic 4:3 ladder, and it is a fixed list ON PURPOSE. A free scan of
 * the driver's own mode list picks up vendor oddballs - .240's ATI offers
 * 1360x1024 (1.328, inside any sane 4:3 tolerance) which no 1999 engine has
 * ever heard of. These six are what the era's mode tables actually contain.
 */
GR_DATA const gr_res_t gr_ladder43[] = {
    {640,480},{800,600},{1024,768},{1152,864},{1280,960},{1600,1200}
};
#define GR_LADDER43_N 6

/*
 * reg_w/reg_h  the PERSISTED desktop mode  (ENUM_REGISTRY_SETTINGS)
 * reg_hz       its refresh, 0 when unknown
 * cap_w/cap_h  a ceiling: the per-box ResCapW/ResCapH, or an engine's own
 *              measured limit. 0 = none.
 */
GR_FN void gr_decide(const gr_panel_t *p, const gr_modes_t *l,
                     int reg_w, int reg_h, int reg_hz, int bpp,
                     int cap_w, int cap_h, gr_target_t *t)
{
    int i, tgt_w, tgt_h, lcd;

    memset(t, 0, sizeof(*t));
    if (reg_w < 320 || reg_h < 200) { reg_w = 1024; reg_h = 768; }

    /* LCD test, validated against all eight fleet panels: the digital-input
     * bit, OR a vertical-refresh ceiling of 76 Hz with a 60 Hz preferred
     * timing. Every CRT here quotes 85-180 Hz; every LCD quotes <= 76 at 60. */
    lcd = 0;
    if (p->ok)
        lcd = p->digital || (p->vmax && p->vmax <= 76 && p->native_hz <= 61);

    if (lcd && p->ok &&
        (gr_modes_have(l, p->native_w, p->native_h) || l->n <= 2)) {
        /* The panel's NATIVE mode. Anything else is resampled by the panel's
         * own scaler and looks soft, and a 4:3 mode on a 16:9 panel is
         * additionally stretched or pillarboxed. */
        tgt_w = p->native_w;
        tgt_h = p->native_h;
    } else if (lcd && p->ok) {
        double want = (double)p->native_w / (double)p->native_h;
        int bw = 0, bh = 0;
        for (i = 0; i < l->n; i++) {
            double r = (double)l->m[i].w / (double)l->m[i].h;
            if (r > want - 0.02 && r < want + 0.02 &&
                l->m[i].w <= p->native_w && l->m[i].w > bw) {
                bw = l->m[i].w; bh = l->m[i].h;
            }
        }
        if (bw) { tgt_w = bw; tgt_h = bh; }
        else    { tgt_w = reg_w; tgt_h = reg_h; }
    } else {
        /* A CRT: the largest mode MATCHING THE TUBE'S ASPECT that does not
         * exceed the mode the box is set up to run. A tube has no pixel grid
         * so sharpness is not the issue - geometry is. */
        int cls = 0, bw = 0, bh = 0;
        if (p->ok && p->hcm && p->vcm)
            cls = gr_aspect_phys((double)p->hcm / (double)p->vcm);
        if (!cls && p->ok)
            cls = gr_aspect_mode((double)p->native_w / (double)p->native_h);
        /* NO EDID AT ALL -> ASSUME A 4:3 TUBE. Falling through to the
         * persisted mode's own aspect is what this exists to stop. It is safe
         * because a 5:4 CRT essentially does not exist, and a widescreen LCD
         * cannot reach this branch - the LCD test itself needs EDID. */
        if (!cls) cls = 43;
        for (i = 0; i < l->n; i++) {
            if (gr_aspect_mode((double)l->m[i].w / (double)l->m[i].h) != cls)
                continue;
            if (l->m[i].w > reg_w || l->m[i].h > reg_h) continue;
            if (l->m[i].w > bw) { bw = l->m[i].w; bh = l->m[i].h; }
        }
        if (bw) { tgt_w = bw; tgt_h = bh; }
        else    { tgt_w = reg_w; tgt_h = reg_h; }
    }

    if (cap_w && cap_h && (tgt_w > cap_w || tgt_h > cap_h)) {
        double want = (double)tgt_w / (double)tgt_h;
        int bw = 0, bh = 0;
        for (i = 0; i < l->n; i++) {
            double r = (double)l->m[i].w / (double)l->m[i].h;
            if (r > want - 0.02 && r < want + 0.02 &&
                l->m[i].w <= cap_w && l->m[i].h <= cap_h && l->m[i].w > bw) {
                bw = l->m[i].w; bh = l->m[i].h;
            }
        }
        if (bw) { tgt_w = bw; tgt_h = bh; }
        else    { tgt_w = cap_w; tgt_h = cap_h; }
    }

    t->w = tgt_w;
    t->h = tgt_h;

    /* The largest CLASSIC 4:3 mode that fits inside the target, for the
     * engines with a fixed 4:3 table and no widescreen support at all. */
    t->w43 = 640; t->h43 = 480;
    for (i = 0; i < GR_LADDER43_N; i++) {
        if (gr_ladder43[i].w > tgt_w || gr_ladder43[i].h > tgt_h) continue;
        if (!gr_mode_offered(l, gr_ladder43[i].w, gr_ladder43[i].h)) continue;
        t->w43 = gr_ladder43[i].w;
        t->h43 = gr_ladder43[i].h;
    }

    /* Refresh is asked PER RESOLUTION, through gr_target_hz(): with an EDID
     * ceiling, the best rate the driver lists at that exact mode under it;
     * WITHOUT one, the persisted desktop's own rate - but only at a mode no
     * bigger than the desktop AND where the driver lists exactly that rate -
     * else 0, which a caller must read as "leave the refresh alone". (Before
     * 2026-09-29 the no-EDID answer was always 0. The persisted rate is the one
     * rate the tube is demonstrably syncing, so it is the only one claimed, and
     * never at a size where the same rate means a higher line rate.) */
    {
        int s1 = GR_HZSRC_NONE, s2 = GR_HZSRC_NONE;
        t->hz   = gr_target_hz(p, l, t->w, t->h, reg_w, reg_h, reg_hz, &s1);
        t->hz43 = gr_target_hz(p, l, t->w43, t->h43, reg_w, reg_h, reg_hz, &s2);
        t->hz_src = s1 > s2 ? s1 : s2;
    }
    t->desk_hz = gr_best_hz(l, reg_w, reg_h);
    if (!t->desk_hz && reg_hz >= GR_HZ_MIN && reg_hz < GR_HZ_MAX)
        t->desk_hz = reg_hz;    /* the mode it is persisted at IS a measurement */
    /* What FLEETRES.EXE publishes as FR_HZ - the SAME function it calls
     * (gr_fr_hz). Any file BOTH writers touch has to use this, or the two
     * disagree by one number and each rewrites the other's copy forever. */
    t->fr_hz = gr_fr_hz(reg_hz);
    t->bpp    = (bpp >= 16) ? bpp : 32;
    t->fov    = gr_horplus_fov(tgt_w, tgt_h);
    t->q2mode = gr_q2_mode_for(l, t->w43, t->h43);
    t->q3mode = gr_q3_mode_for(l, t->w43, t->h43);
    /* the index engines render at their TABLE's mode, which is not always the
     * 4:3 target (q3mode 7 = 1152x864 where w43 is 1280x960) */
    t->hzq2 = gr_target_hz(p, l, gr_q2tab[t->q2mode].w, gr_q2tab[t->q2mode].h,
                           reg_w, reg_h, reg_hz, NULL);
    t->hzq3 = gr_target_hz(p, l, gr_q3tab[t->q3mode].w, gr_q3tab[t->q3mode].h,
                           reg_w, reg_h, reg_hz, NULL);
    t->d3ar   = gr_d3_aspect(tgt_w, tgt_h);
    t->wide   = (tgt_w * 3 > tgt_h * 4 + tgt_h / 8) ? 1 : 0;
    t->lcd    = lcd;
    t->edid   = p->ok ? 1 : 0;
    gr_aspect_str(tgt_w, tgt_h, t->aspect, sizeof(t->aspect));
}

/* ------------------------------------------------------------------ */
/* substitution                                                         */
/* ------------------------------------------------------------------ */

/*
 * Expand %TOKEN% in a rule's value. The token names deliberately match the
 * FR_* variables FLEETRES.BAT publishes, minus the prefix, so a rule here and
 * the launcher line it mirrors read the same.
 *
 *   %W% %H%          the widescreen-capable target
 *   %W43% %H43%      the 4:3-only target
 *   %HZW% %HZ43%     the refresh a title AT that target asks for
 *   %HZQ2% %HZQ3%    ... at the id Tech 2 / id Tech 3 index mode
 *                    - per target, because a rate is offered per resolution;
 *                    0 = leave it alone (gr_target_hz). %HZ% = %HZW%.
 *   %HZWHI%          %HZW% when it is above 60, else 0 (gr_hz_hi) - for an
 *                    engine where naming a rate can hurt (Serious Engine)
 *   %HZSRC%          edid | persisted | none - what vouches for them
 *   %DESKHZ%         the same, at the persisted desktop mode
 *   %FRHZ%           the persisted mode's OWN rate - exactly what FLEETRES
 *                    publishes as FR_HZ, for a file both writers touch
 *   %HZOVERRIDE%     "True" when a rate is known, else "False"
 *   %BPP%
 *   %FOV%            hor+ FOV
 *   %Q2MODE% %Q3MODE%
 *   %D3AR%           id Tech 4 r_aspectRatio
 *   %DOSFULLRES%     DOSBox [sdl] fullresolution: desktop on an LCD,
 *                    original on a CRT
 *   %SEL43:WxH%      "1" when WxH is exactly the 4:3 target, else "0" - for
 *                    an engine that keeps one BOOLEAN PER MODE (Turok 2)
 *
 * Returns 0 on success, non-zero when the output would not fit (which is a
 * bug in the rule, not a runtime condition, so the caller must treat it as a
 * failure rather than shipping a truncated value).
 */
GR_FN int gr_expand(const char *tmpl, const gr_target_t *t,
                    char *out, size_t cap)
{
    size_t o = 0;
    const char *s = tmpl;

    if (!cap) return 1;
    out[0] = 0;
    while (*s) {
        if (*s == '%') {
            const char *e = strchr(s + 1, '%');
            if (e) {
                char tok[48];
                size_t n = (size_t)(e - s - 1);
                if (n < sizeof(tok)) {
                    char val[32];
                    int have = 1;
                    memcpy(tok, s + 1, n);
                    tok[n] = 0;
                    if      (!strcmp(tok, "W"))       sprintf(val, "%d", t->w);
                    else if (!strcmp(tok, "H"))       sprintf(val, "%d", t->h);
                    else if (!strcmp(tok, "W43"))     sprintf(val, "%d", t->w43);
                    else if (!strcmp(tok, "H43"))     sprintf(val, "%d", t->h43);
                    /* HZ is kept as the old name of HZW */
                    else if (!strcmp(tok, "HZ") || !strcmp(tok, "HZW"))
                                                      sprintf(val, "%d", t->hz);
                    else if (!strcmp(tok, "HZWHI"))   sprintf(val, "%d", gr_hz_hi(t->hz));
                    else if (!strcmp(tok, "HZ43"))    sprintf(val, "%d", t->hz43);
                    else if (!strcmp(tok, "HZQ2"))    sprintf(val, "%d", t->hzq2);
                    else if (!strcmp(tok, "HZQ3"))    sprintf(val, "%d", t->hzq3);
                    else if (!strcmp(tok, "HZSRC"))   strcpy(val, gr_hz_src_name(t->hz_src));
                    else if (!strcmp(tok, "DESKHZ"))  sprintf(val, "%d", t->desk_hz);
                    else if (!strcmp(tok, "FRHZ"))    sprintf(val, "%d", t->fr_hz);
                    /* True only when a rate is actually KNOWN. An engine told
                     * to override the desktop refresh with 0 overrides it with
                     * nothing, which is worse than not overriding. */
                    else if (!strcmp(tok, "HZOVERRIDE"))
                        strcpy(val, t->hz > 0 ? "True" : "False");
                    else if (!strcmp(tok, "BPP"))     sprintf(val, "%d", t->bpp);
                    else if (!strcmp(tok, "FOV"))     sprintf(val, "%d", t->fov);
                    else if (!strcmp(tok, "Q2MODE"))  sprintf(val, "%d", t->q2mode);
                    else if (!strcmp(tok, "Q3MODE"))  sprintf(val, "%d", t->q3mode);
                    else if (!strcmp(tok, "D3AR"))    sprintf(val, "%d", t->d3ar);
                    else if (!strcmp(tok, "DOSFULLRES"))
                        strcpy(val, t->lcd ? "desktop" : "original");
                    else if (!strncmp(tok, "SEL43:", 6)) {
                        char want[24];
                        sprintf(want, "%dx%d", t->w43, t->h43);
                        strcpy(val, strcmp(tok + 6, want) == 0 ? "1" : "0");
                    } else have = 0;
                    if (have) {
                        size_t vl = strlen(val);
                        if (o + vl >= cap) return 1;
                        memcpy(out + o, val, vl);
                        o += vl;
                        s = e + 1;
                        continue;
                    }
                }
            }
        }
        if (o + 1 >= cap) return 1;
        out[o++] = *s++;
    }
    out[o] = 0;
    return 0;
}

/*
 * Compose the line a GR_OP_KV rule writes.
 *
 * THIS EXISTS BECAUSE OMITTING IT WAS A REAL BUG, FOUND ON HARDWARE. The KV
 * writer replaces a whole LINE, so it must be handed "ResolutionX=1024" and
 * not "1024". Handed the bare value it replaced `ResolutionX=1024` with
 * `1024` - and then, on the next pass, `1024` no longer parses as key=value,
 * so nothing matched and another `1024` was APPENDED. Three GAMERES passes on
 * .191 left Descent 2's DESCENT.CFG carrying six junk lines and no resolution
 * at all, while every pass reported success.
 *
 * It was caught in seconds only because the pass reports how many values it
 * CHANGED and a settled box must report zero: Descent 1 and Descent 2 kept
 * reporting 2 apiece. That is the same signal `files_written` provides for
 * GAMESYNC, and this is what it is for.
 */
GR_FN int gr_kv_line(const char *key, const char *value, char *out, size_t cap)
{
    size_t k = strlen(key), v = strlen(value);
    if (k + 1 + v + 1 > cap) return 1;
    memcpy(out, key, k);
    out[k] = '=';
    memcpy(out + k + 1, value, v);
    out[k + 1 + v] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* the per-title rules                                                  */
/* ------------------------------------------------------------------ */

enum {
    GR_OP_INI = 0,   /* WritePrivateProfileString(file, arg1, arg2, arg3)   */
    GR_OP_SETLINE,   /* replace the line whose FIRST TOKEN is arg1 with arg2 */
    GR_OP_KV,        /* replace/append  arg1=arg2  (no [section] header)     */
    GR_OP_REG,       /* file = "HKLM"|"HKCU", arg1 = subkey, arg2 = value    */
                     /* name, arg3 = "dword:<data>" or "sz:<data>"           */
    GR_OP_CFG        /* rewrite the whole file; arg1 is the body, '\n'-sep   */
};

typedef struct {
    const char *title;      /* library title directory, exact case          */
    unsigned char op;
    const char *file;       /* path relative to the title root, or reg root */
    const char *arg1;
    const char *arg2;
    const char *arg3;
} gr_rule_t;

/*
 * The one place a title's resolution recipe is written down for the agent.
 *
 * IT COVERS PERSISTENT CONFIG ONLY - a file in the tree or a registry value.
 * A title whose mode is set purely on a COMMAND LINE (Quake 1's GLQUAKE.EXE
 * -width, Hexen II, Descent 3, Halo, Doom 3, Jedi Academy's +set) is NOT
 * here and must not be: there is nothing on disk for this pass to write, and
 * inventing a config file the engine does not read would be a change that
 * looks like a fix and is not. Those titles are served by FLEETRES.BAT in
 * their launcher, which is why that mechanism stays.
 *
 * Every row mirrors a launcher line that stage-fleetres.py generates, and
 * tests/python/test_gameres_mirror.py fails if the two disagree - so this is
 * a second COPY of one decision, never a second decision.
 */
GR_DATA const gr_rule_t gr_rules[] = {

/* --- GoldSrc: Half-Life, Counter-Strike 1.6, and every mod on the box ---
 *
 * THE REASON THIS WHOLE PASS EXISTS. There is no Software\Valve\CounterStrike
 * key - read live on .240 - so every GoldSrc title on a machine shares this
 * one. HalfLife1/install.reg pins it to 1024x768 and gs_merge_reg() re-applies
 * that on EVERY sync, so a 1080p box is actively re-pinned to 1024x768 and the
 * launcher cannot undo it (install.reg's own comment records that CS "ignores
 * -w/-h on the command line for the same reason").
 *
 * THE PAIR IS THE WIDESCREEN ONE, AND THAT IS A DELIBERATE CHOICE BETWEEN TWO
 * ENGINES SHARING ONE KEY:
 *   * Counter-Strike 1.6 renders true widescreen and CANNOT override this from
 *     its command line. If the shared key is 4:3, CS is 4:3, full stop.
 *   * WON Half-Life is 4:3-only - handed a 16:9 mode it falls to the BOTTOM of
 *     its table, 400x300, and takes the desktop with it (measured on .240) -
 *     but its launcher passes `-w %FR_W43% -h %FR_H43%` explicitly and that
 *     DOES win for that engine (same measurement, .240: -w 1280 -h 960 gave a
 *     correct 1280x960 desktop and window).
 * So the key carries the value only one of the two can use, and the other
 * corrects itself per launch. Getting this backwards is silent on a 4:3 box
 * and wrong on every widescreen one.
 *
 * ScreenWidth/ScreenHeight are the WON LAUNCHER's values and EngineModeW/H the
 * ENGINE's; both halves have to agree or the engine comes up at its own 400x300
 * default (A/B'd on .133). EngineType 1 = hardware/OpenGL, from that same A/B.
 */
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "ScreenWidth",      "dword:%W%" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "ScreenHeight",     "dword:%H%" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "ScreenBPP",        "dword:32" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "ScreenWindowed",   "dword:0" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineModeW",      "dword:%W%" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineModeH",      "dword:%H%" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineModeBPP",    "dword:32" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineModeWindowed", "dword:0" },
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineType",       "dword:1" },
/* EngineGLDriver: "Default" means the system opengl32. On .143 Half-Life came
 * up in-game at 400x300 with a leftover "3dfxgl.dll" here - a dead GL driver
 * name on that box's GeForce 6800. Any machine that ever ran a 3dfx card can
 * carry that poison, so it is pinned rather than left alone. */
{ "CounterStrike16", GR_OP_REG, "HKCU", "Software\\Valve\\Half-Life\\Settings", "EngineGLDriver",   "sz:Default" },

/* --- id Tech 3, r_mode -1 branch present (measured: quake3.exe, ioquake3,
 *     jasp.exe, jamp.exe all reach 1920x1080 this way) --------------------
 *
 * These cvars are CVAR_LATCH - read once at renderer init - and the staged
 * autoexec.cfg execs fleetres.cfg as its LAST line, so this file overrides
 * whatever the box's own config wrote earlier in the same pass. Writing it at
 * sync time means it is already correct before the title's first launch. */
{ "Quake3-TeamArena", GR_OP_CFG, "baseq3\\fleetres.cfg", NULL, NULL, NULL },
{ "Quake3-TeamArena", GR_OP_CFG, "missionpack\\fleetres.cfg", NULL, NULL, NULL },
{ "JediAcademy",      GR_OP_CFG, "base\\fleetres.cfg", NULL, NULL, NULL },

/* --- id Tech 3 forks with NO r_mode -1 BRANCH ---------------------------
 * The cvar table is NOT evidence: SoF2 and RTCW both carry r_customwidth and
 * honour neither. Measured on .145 with an identical config, quake3.exe/jasp/
 * jamp gave 1920x1080 and sof2mp.exe gave 640x480 - it does not error, it
 * renders small. So these get a plain mode INDEX, and it must be Q3MODE:
 * id Tech 3's table entry 8 is 1280x1024 where id Tech 2's is 1280x960. */
{ "SoldierOfFortune2",          GR_OP_CFG, "base\\fleetres.cfg", "idtech3-index", NULL, NULL },
{ "ReturnToCastleWolfenstein",  GR_OP_CFG, "Main\\fleetres.cfg", "idtech3-index-nofov", NULL, NULL },

/* --- id Tech 2: a FIXED 4:3 table indexed by gl_mode, no custom mode and no
 *     16:9 entry anywhere, so the honest best is a correctly proportioned 4:3
 *     mode. Every mod directory needs its own copy - covering base/ alone left
 *     Wages of SiN pinned at 1024x768 on every box. */
{ "Quake2Complete", GR_OP_CFG, "baseq2\\fleetres.cfg", "idtech2", NULL, NULL },
{ "Quake2Complete", GR_OP_CFG, "xatrix\\fleetres.cfg", "idtech2", NULL, NULL },
{ "Quake2Complete", GR_OP_CFG, "rogue\\fleetres.cfg",  "idtech2", NULL, NULL },
{ "Quake2Complete", GR_OP_CFG, "ctf\\fleetres.cfg",    "idtech2", NULL, NULL },
{ "SiNGold",          GR_OP_CFG, "base\\fleetres.cfg", "idtech2", NULL, NULL },
{ "SiNGold",          GR_OP_CFG, "2015\\fleetres.cfg", "idtech2", NULL, NULL },
{ "SoldierOfFortune", GR_OP_CFG, "base\\fleetres.cfg", "idtech2", NULL, NULL },

/* --- Serious Engine 1. The mode lives in two files and only one is ours:
 * PersistentSymbols.ini is where the engine SAVES on exit, so anything staged
 * there is overwritten by the first box that runs the game. Game_startup.ini
 * is the engine's own documented hook. sam_iDriver is deliberately NOT written
 * - that is a renderer choice the engine makes for itself (.246 cannot open
 * OpenGL at all and runs on Direct3D). */
/* ...and the two Encounters do NOT share a refresh cvar: TSE's Engine.dll
 * declares only `gap_iRefreshRate`, and answers `gfx_iRefreshRate=...;` with
 * "not declared" (the .123/.195/.240 logs), so TSE gets its own body. */
{ "SeriousSamFirstEncounter",  GR_OP_CFG, "Scripts\\Game_startup.ini", "ssam", NULL, NULL },
{ "SeriousSamSecondEncounter", GR_OP_CFG, "Scripts\\Game_startup.ini", "ssam-tse", NULL, NULL },

/* --- Unreal Engine 1 / 2. The engine rewrites its .ini on exit, so the
 *     launcher writes it too; this makes it right before the first launch. */
{ "UnrealGold",          GR_OP_INI, "System\\Unreal.ini",            "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UnrealGold",          GR_OP_INI, "System\\Unreal.ini",            "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UnrealGold",          GR_OP_INI, "System\\Unreal.ini",            "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
/* UT 469e names a rate in BOTH its hardware devices (D3D9Drv: pp.
 * FullScreen_RefreshRateInHz when non-zero; OpenGLDrv: DM_DISPLAYFREQUENCY
 * when non-zero, and each retries at the default if the driver refuses).
 * %HZW% is the rate AT the viewport above; 0 = the default. The launcher
 * writes the same two values (stage-fleetres.py ut469_refresh). */
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "D3D9Drv.D3D9RenderDevice",     "RefreshRate", "%HZW%" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "OpenGLDrv.OpenGLRenderDevice", "RefreshRate", "%HZW%" },
{ "UnrealTournament436", GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UnrealTournament436", GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UnrealTournament436", GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
/* UT2004's D3DDrv wants a rate only when DesiredRefreshRate > 60 OR
 * OverrideDesktopRefreshRate is set, and then matches it among the ENUMERATED
 * modes. %HZOVERRIDE% is True exactly when %HZW% is non-zero, so 0 is still
 * "the default". Both launchers write the same (stage-fleetres.py
 * ut2004_refresh). There is no D3D9Drv.dll in this tree. */
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "D3DDrv.D3DRenderDevice", "DesiredRefreshRate",         "%HZW%" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "D3DDrv.D3DRenderDevice", "OverrideDesktopRefreshRate", "%HZOVERRIDE%" },
{ "DeusEx",              GR_OP_INI, "SYSTEM\\DeusEx.ini",            "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "DeusEx",              GR_OP_INI, "SYSTEM\\DeusEx.ini",            "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "DeusEx",              GR_OP_INI, "SYSTEM\\DeusEx.ini",            "WinDrv.WindowsClient", "StartupFullscreen",   "True" },

/* --- Westwood. Tiberian Sun's own Display Options list stops at 800x600 and
 *     the engine renders 1920x1080 anyway, because the CnCNet patch reads
 *     SUN.INI directly and bypasses that list. */
{ "TiberianSun", GR_OP_INI, "SUN.INI",   "Video", "ScreenWidth",     "%W%" },
{ "TiberianSun", GR_OP_INI, "SUN.INI",   "Video", "ScreenHeight",    "%H%" },
{ "TiberianSun", GR_OP_INI, "SUN.INI",   "Video", "AllowHiResModes", "yes" },
{ "RedAlert2",   GR_OP_INI, "RA2.INI",   "Video", "ScreenWidth",     "%W%" },
{ "RedAlert2",   GR_OP_INI, "RA2.INI",   "Video", "ScreenHeight",    "%H%" },
{ "RedAlert2",   GR_OP_INI, "RA2MD.INI", "Video", "ScreenWidth",     "%W%" },
{ "RedAlert2",   GR_OP_INI, "RA2MD.INI", "Video", "ScreenHeight",    "%H%" },

/* --- Engines that keep the mode ONLY in the registry, where nothing that
 *     ships in the tree can reach it and a box inherits whatever stale hive it
 *     happens to have. Both had install.reg pinning 800x600 on all eight. */
{ "MaxPayne",           GR_OP_REG, "HKCU", "Software\\Remedy Entertainment\\Max Payne\\Video Settings", "Display Width",  "dword:%W%" },
{ "MaxPayne",           GR_OP_REG, "HKCU", "Software\\Remedy Entertainment\\Max Payne\\Video Settings", "Display Height", "dword:%H%" },
{ "RedFaction",         GR_OP_REG, "HKLM", "SOFTWARE\\Volition\\Red Faction", "Resolution Width",     "dword:%W%" },
{ "RedFaction",         GR_OP_REG, "HKLM", "SOFTWARE\\Volition\\Red Faction", "Resolution Height",    "dword:%H%" },
{ "RedFaction",         GR_OP_REG, "HKLM", "SOFTWARE\\Volition\\Red Faction", "Resolution Bit Depth", "dword:32" },
{ "HiddenAndDangerous", GR_OP_REG, "HKLM", "Software\\Lonely Cat Games\\Hidden and Dangerous Deluxe\\Config", "Display width",    "dword:%W%" },
{ "HiddenAndDangerous", GR_OP_REG, "HKLM", "Software\\Lonely Cat Games\\Hidden and Dangerous Deluxe\\Config", "Display height",   "dword:%H%" },
{ "HiddenAndDangerous", GR_OP_REG, "HKLM", "Software\\Lonely Cat Games\\Hidden and Dangerous Deluxe\\Config", "Display bitdepth", "dword:32" },
{ "HiddenAndDangerous", GR_OP_REG, "HKLM", "Software\\Lonely Cat Games\\Hidden and Dangerous Deluxe\\Config", "Fullscreen",       "dword:1" },

/* --- Line-oriented configs that are not INI-shaped. */
/* Dark engine WITH NewDark (Thief 2 has D3DX9_43.dll and NVScript.osm; System
 * Shock 2 and Thief Gold do not, and vanilla Dark is 640x480 with no cvar). */
{ "Thief2", GR_OP_SETLINE, "cam.cfg", "game_screen_size", "game_screen_size %W% %H%", NULL },
/* LithTech 1.0 needs the double quotes its own format uses. */
{ "Shogo",  GR_OP_SETLINE, "autoexec.cfg", "screenwidth",  "\"screenwidth\" \"%W%\"",  NULL },
{ "Shogo",  GR_OP_SETLINE, "autoexec.cfg", "screenheight", "\"screenheight\" \"%H%\"", NULL },
/* Refractor 1. A box that has opened the video menu reads Custom, so writing
 * only Default is a silent half-fix. */
{ "BF1942", GR_OP_SETLINE, "Mods\\bf1942\\Settings\\Profiles\\Default\\Video.con", "game.setGameDisplayMode", "game.setGameDisplayMode %W% %H% 32 0", NULL },
{ "BF1942", GR_OP_SETLINE, "Mods\\bf1942\\Settings\\Profiles\\Custom\\Video.con",  "game.setGameDisplayMode", "game.setGameDisplayMode %W% %H% 32 0", NULL },
/* CryEngine 1. */
{ "FarCry", GR_OP_SETLINE, "System.cfg", "r_Width",  "r_Width = \"%W%\"",  NULL },
{ "FarCry", GR_OP_SETLINE, "System.cfg", "r_Height", "r_Height = \"%H%\"", NULL },

/* --- DXX-Rebirth writes DESCENT.CFG as bare `ResolutionX=1024` with NO
 *     [section] header, so WritePrivateProfileString cannot address it and a
 *     first-token match cannot either - the whole "ResolutionX=1024" is one
 *     whitespace token. Split at the '=' instead. Descent 1's DOSBox
 *     launchers are a different engine in the same tree; only the native
 *     Rebirth build reads this file. */
{ "Descent1", GR_OP_KV, "DESCENT.CFG", "ResolutionX", "%W%", NULL },
{ "Descent1", GR_OP_KV, "DESCENT.CFG", "ResolutionY", "%H%", NULL },
{ "Descent2", GR_OP_KV, "DESCENT.CFG", "ResolutionX", "%W%", NULL },
{ "Descent2", GR_OP_KV, "DESCENT.CFG", "ResolutionY", "%H%", NULL },

/* --- DOSBox. `fullresolution=original` changes the WHOLE DESKTOP to the DOS
 *     mode - on a 16:9 LCD that is a stretched 640x480 upscale left behind
 *     after a crash (measured on .145 with DISPLAYCFG). `desktop` keeps the
 *     desktop mode and lets DOSBox pillarbox correctly with aspect=true. On a
 *     CRT `original` is still right, which is exactly why it cannot be a
 *     staged constant. */
{ "Carmageddon1",          GR_OP_INI, "dosboxCarma.conf",       "sdl", "fullresolution", "%DOSFULLRES%" },
{ "RedneckRampage",        GR_OP_INI, "dosboxRR.conf",          "sdl", "fullresolution", "%DOSFULLRES%" },
{ "Descent1",              GR_OP_INI, "dosboxD1.conf",          "sdl", "fullresolution", "%DOSFULLRES%" },
{ "MasterOfOrionII",       GR_OP_INI, "dosboxMOO2.conf",        "sdl", "fullresolution", "%DOSFULLRES%" },
{ "Daggerfall",            GR_OP_INI, "dosbox_daggerfall.conf", "sdl", "fullresolution", "%DOSFULLRES%" },
{ "ShadowWarrior",         GR_OP_INI, "dosbox_swarrior.conf",   "sdl", "fullresolution", "%DOSFULLRES%" },
{ "WarcraftOrcsAndHumans", GR_OP_INI, "dosboxWC1.conf",         "sdl", "fullresolution", "%DOSFULLRES%" },

/* --- Turok 2 keeps ONE BOOLEAN PER MODE in Data\config.ned, chosen from a
 *     fixed list compiled into Video_D3D.dll. There is no width/height pair
 *     and no 1080p entry, so the honest best is the largest 4:3 mode on that
 *     list the box can drive - which is exactly "offer the resolutions the
 *     monitor supports" for an engine that enumerates rather than accepts.
 *     1280x1024 is deliberately never selected: it is 5:4, and a 5:4 mode on
 *     a 4:3 or 16:9 panel is the squashed picture this mechanism removes. */
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\320^x^240",   "Acclaim\\Turok\\VideoD3D\\320^x^240 %SEL43:320x240%",   NULL },
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\512^x^384",   "Acclaim\\Turok\\VideoD3D\\512^x^384 %SEL43:512x384%",   NULL },
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\640^x^480",   "Acclaim\\Turok\\VideoD3D\\640^x^480 %SEL43:640x480%",   NULL },
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\800^x^600",   "Acclaim\\Turok\\VideoD3D\\800^x^600 %SEL43:800x600%",   NULL },
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\1024^x^768",  "Acclaim\\Turok\\VideoD3D\\1024^x^768 %SEL43:1024x768%", NULL },
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\1280^x^1024", "Acclaim\\Turok\\VideoD3D\\1280^x^1024 0",               NULL },
{ "Turok2", GR_OP_SETLINE, "Data\\config.ned", "Acclaim\\Turok\\VideoD3D\\Windowed",    "Acclaim\\Turok\\VideoD3D\\Windowed 0",                  NULL }
};

#define GR_RULE_COUNT ((int)(sizeof(gr_rules) / sizeof(gr_rules[0])))

/*
 * The bodies for GR_OP_CFG. Kept out of the table because they are multi-line
 * and shared between titles; arg1 names which one (NULL = the standard
 * id Tech 3 custom-mode file).
 */
/*
 * NOTE ON REFRESH IN THESE BODIES (2026-09-29, refresh phase 2).
 *
 * Each body names the rate AT THE RESOLUTION THAT TITLE RUNS AT - gr_target_hz
 * via %HZW% (r_mode -1 at %W%x%H%) or %HZQ3% (the id Tech 3 index mode) - and
 * FLEETRES.EXE publishes the same numbers as FR_HZW / FR_HZQ3 from the same
 * function over a list built the same way (test_fleetres_refresh_mirror.py).
 * That is what lets a file BOTH writers touch carry a per-target rate: the
 * title's launcher rewrites it at every start, the agent at every sync, and a
 * one-number difference would make each rewrite the other's copy forever and
 * kill the "0 value(s) changed" signal. Until phase 2 these bodies used %FRHZ%,
 * the persisted desktop's own rate, as the one number both could reproduce -
 * which undershot every title that does not run at the desktop's size (SoF2
 * and RTCW at 1152x864 on the 1080p boxes: 60 where 75 is listed).
 *
 * "RAISE THE DESKTOP AND EVERY ENGINE INHERITS IT" IS FALSE. This note used to
 * say Quake II and GoldSrc "take whatever the desktop is on". Measured: a mode
 * set that names no rate lands on the ADAPTER DEFAULT, 60, whatever the
 * desktop, the registry or the current mode says - ForceWare 71.89 (.124
 * 2026-08-25: desktop 1024x768@100 -> Quake II at the same size -> 60), the
 * XP SP3 build VM (CDS with no rate: 60 at every size that lists 60, and
 * DISP_CHANGE_FAILED where 60 is not listed), and vcr-kmd (.124 2026-09-29:
 * Quake 2, GLQuake and Half-Life at 1280x960@60 on a 1280x1024@85 desktop).
 * gameres_raise_refresh still raises the persisted desktop, which serves the
 * desktop itself and the titles that run AT it; a title with no refresh knob
 * running at another size needs the launcher's refreshkeep watcher
 * (stage-fleetres.py refreshkeep_line) - and a title WITH a knob gets its
 * per-target rate from these bodies and its launcher.
 */
GR_FN const char *gr_cfg_body(const char *kind)
{
    if (!kind)
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "seta r_mode \"-1\"\n"
               "seta r_customwidth \"%W%\"\n"
               "seta r_customheight \"%H%\"\n"
               "seta r_customaspect \"1\"\n"
               "seta r_customPixelAspect \"1\"\n"
               "seta r_fullscreen \"1\"\n"
               "seta cg_fov \"%FOV%\"\n"
               "seta r_displayRefresh \"%HZW%\"\n";
    if (!strcmp(kind, "idtech3-index"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "// r_mode -1 DOES NOT EXIST IN THIS ENGINE - a plain index,\n"
               "// and Q3MODE not Q2MODE: idTech3 mode 8 is 1280x1024.\n"
               "seta r_mode \"%Q3MODE%\"\n"
               "seta r_fullscreen \"1\"\n"
               "seta cg_fov \"%FOV%\"\n"
               "seta r_displayRefresh \"%HZQ3%\"\n";
    if (!strcmp(kind, "idtech3-index-nofov"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "// r_mode -1 DOES NOT EXIST IN THIS ENGINE - a plain index,\n"
               "// and Q3MODE not Q2MODE: idTech3 mode 8 is 1280x1024.\n"
               "seta r_mode \"%Q3MODE%\"\n"
               "seta r_fullscreen \"1\"\n"
               "seta r_displayRefresh \"%HZQ3%\"\n";
    if (!strcmp(kind, "idtech2"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "set gl_mode \"%Q2MODE%\"\n"
               "set vid_fullscreen \"1\"\n";
    if (!strcmp(kind, "ssam"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "// PersistentSymbols.ini is NOT the place for this: the engine\n"
               "// rewrites that file on exit and would overwrite the mode.\n"
               "sam_bFullScreen=1;\n"
               "sam_iScreenSizeI=%W%;\n"
               "sam_iScreenSizeJ=%H%;\n"
               "gfx_iRefreshRate=%HZWHI%;\n";
    /* TSE: the same file and mode, and the refresh under ITS name. */
    if (!strcmp(kind, "ssam-tse"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "// PersistentSymbols.ini is NOT the place for this: the engine\n"
               "// rewrites that file on exit and would overwrite the mode.\n"
               "sam_bFullScreen=1;\n"
               "sam_iScreenSizeI=%W%;\n"
               "sam_iScreenSizeJ=%H%;\n"
               "gap_iRefreshRate=%HZWHI%;\n";
    return NULL;
}

#endif /* RETRO_GAMERES_H */
