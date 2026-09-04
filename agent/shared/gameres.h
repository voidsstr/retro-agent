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

/* A mode the DRIVER offers, with the best real refresh seen for it. */
typedef struct { int w, h, hz; } gr_mode_t;

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
     * them. 0 means "not known - leave the refresh alone", never 60. */
    int  hz;                    /* best real rate at w x h                   */
    int  hz43;                  /* best real rate at w43 x h43               */
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
            return;
        }
    if (l->n < GR_MAX_MODES) {
        l->m[l->n].w = w;
        l->m[l->n].h = h;
        l->m[l->n].hz = hz;
        l->n++;
    }
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

    /* Refresh is asked PER RESOLUTION. gr_best_hz already refuses a rate the
     * driver does not offer at that mode and one past the panel's own EDID
     * ceiling, and answers 0 rather than guessing - so a caller that finds 0
     * must leave the refresh alone. Falling back to the persisted desktop's
     * rate here would be the "hardcoded 60" mistake wearing a better hat: it
     * would claim a rate for a resolution nobody measured it at. */
    t->hz      = gr_best_hz(l, t->w, t->h);
    t->hz43    = gr_best_hz(l, t->w43, t->h43);
    t->desk_hz = gr_best_hz(l, reg_w, reg_h);
    if (!t->desk_hz && reg_hz >= GR_HZ_MIN && reg_hz < GR_HZ_MAX)
        t->desk_hz = reg_hz;    /* the mode it is persisted at IS a measurement */
    /* What FLEETRES.EXE publishes as FR_HZ, reproduced exactly - including its
     * 60 fallback. Any file BOTH writers touch has to use this, or the two
     * disagree by one number and each rewrites the other's copy forever. */
    t->fr_hz = (reg_hz >= GR_HZ_MIN && reg_hz < GR_HZ_MAX) ? reg_hz : 60;
    t->bpp    = (bpp >= 16) ? bpp : 32;
    t->fov    = gr_horplus_fov(tgt_w, tgt_h);
    t->q2mode = gr_q2_mode_for(l, t->w43, t->h43);
    t->q3mode = gr_q3_mode_for(l, t->w43, t->h43);
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
 *   %HZ% %HZ43%      the highest refresh the monitor supports AT that target
 *                    - two numbers, because a rate is offered per resolution
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
                    else if (!strcmp(tok, "HZ"))      sprintf(val, "%d", t->hz);
                    else if (!strcmp(tok, "HZ43"))    sprintf(val, "%d", t->hz43);
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
{ "SeriousSamFirstEncounter",  GR_OP_CFG, "Scripts\\Game_startup.ini", "ssam", NULL, NULL },
{ "SeriousSamSecondEncounter", GR_OP_CFG, "Scripts\\Game_startup.ini", "ssam", NULL, NULL },

/* --- Unreal Engine 1 / 2. The engine rewrites its .ini on exit, so the
 *     launcher writes it too; this makes it right before the first launch. */
{ "UnrealGold",          GR_OP_INI, "System\\Unreal.ini",            "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UnrealGold",          GR_OP_INI, "System\\Unreal.ini",            "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UnrealGold",          GR_OP_INI, "System\\Unreal.ini",            "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UnrealTournament",    GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
{ "UnrealTournament436", GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UnrealTournament436", GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UnrealTournament436", GR_OP_INI, "System\\UnrealTournament.ini",  "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "WinDrv.WindowsClient", "FullscreenViewportX", "%W%" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "WinDrv.WindowsClient", "FullscreenViewportY", "%H%" },
{ "UT2004",              GR_OP_INI, "System\\UT2004.ini",            "WinDrv.WindowsClient", "StartupFullscreen",   "True" },
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
 * NOTE ON REFRESH IN THESE BODIES. They use %FRHZ% - the PERSISTED desktop
 * mode's own rate, which is exactly what FLEETRES publishes as FR_HZ - and
 * NOT %HZ%, the highest rate the panel supports at the target. That looks like
 * the weaker choice and is the correct one: the title's launcher rewrites this
 * same file at every start, so a body carrying a different number from the
 * launcher's would be rewritten by each writer in turn, forever, and the
 * "0 value(s) changed" contract that catches real faults would be dead.
 *
 * The highest supported rate is delivered instead by raising the PERSISTED
 * DESKTOP refresh itself (gameres_raise_refresh) - after which FR_HZ *is* the
 * highest the monitor supports, for every consumer at once, including the
 * engines that have no refresh setting at all. Quake II and GoldSrc were
 * checked: their binaries carry no refresh cvar, only `timerefresh` and
 * `r_norefresh`.
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
               "seta r_displayRefresh \"%FRHZ%\"\n";
    if (!strcmp(kind, "idtech3-index"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "// r_mode -1 DOES NOT EXIST IN THIS ENGINE - a plain index,\n"
               "// and Q3MODE not Q2MODE: idTech3 mode 8 is 1280x1024.\n"
               "seta r_mode \"%Q3MODE%\"\n"
               "seta r_fullscreen \"1\"\n"
               "seta cg_fov \"%FOV%\"\n"
               "seta r_displayRefresh \"%FRHZ%\"\n";
    if (!strcmp(kind, "idtech3-index-nofov"))
        return "// written by GAMESYNC for this box's monitor - do not edit\n"
               "// r_mode -1 DOES NOT EXIST IN THIS ENGINE - a plain index,\n"
               "// and Q3MODE not Q2MODE: idTech3 mode 8 is 1280x1024.\n"
               "seta r_mode \"%Q3MODE%\"\n"
               "seta r_fullscreen \"1\"\n"
               "seta r_displayRefresh \"%FRHZ%\"\n";
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
               "gfx_iRefreshRate=%FRHZ%;\n";
    return NULL;
}

#endif /* RETRO_GAMERES_H */
