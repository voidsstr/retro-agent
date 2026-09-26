/*
 * vcr_modes.c - timings table, PLL search and the per-mode register recipe.
 * See include/vcr_modes.h. Integer arithmetic only (runs in kernel mode).
 */
#include "../include/vcr_modes.h"
#include "../include/vcr_regs.h"

/*
 * VESA DMT (and CEA-861 / CVT where DMT has no entry) timings.
 *   w, h, Hz, pixclk kHz, hfp, hsync, hbp, vfp, vsync, vbp, flags
 * Doublescan rows: `h` is LOGICAL rows (each scanned twice), but vfp/vsync/vbp
 * are PHYSICAL lines - the vendor's 320x200 has an odd 449-line total.
 * Order matters only for presentation: modes are listed in this order.
 */
#define N VCR_T_HNEG
#define V VCR_T_VNEG
#define D VCR_T_DBLSCAN
const vcr_timing vcr_timings[] = {
    /* low resolution (Glide 320x240 / 400x300 / 512x384): the vendor driver's
     * timings, decoded from its CRTC on the V5 6000 - the parent VGA/DMT mode
     * with the horizontal halved and the vertical kept in physical lines */
    { 320,  200, 70,  12587,   8,  48,  24, 13, 2, 34, N | D },
    { 320,  240, 60,  12587,   8,  48,  24, 10, 2, 33, N | V | D },
    { 400,  300, 60,  20003,  24,  64,  40,  1, 4, 23, D },   /* the vendor's clock */
    { 512,  384, 60,  32500,  16,  64,  80,  3, 6, 29, N | V | D },
    { 640,  400, 70,  25175,  16,  96,  48, 13, 2, 34, N },
    /* 4:3 DMT */
    { 640,  480, 60,  25175,  16,  96,  48, 10, 2, 33, N | V },
    { 640,  480, 72,  31500,  24,  40, 128,  9, 3, 28, N | V },
    { 640,  480, 75,  31500,  16,  64, 120,  1, 3, 16, N | V },
    { 640,  480, 85,  36000,  56,  56,  80,  1, 3, 25, N | V },
    { 800,  600, 56,  36000,  24,  72, 128,  1, 2, 22, 0 },
    { 800,  600, 60,  40000,  40, 128,  88,  1, 4, 23, 0 },
    { 800,  600, 72,  50000,  56, 120,  64, 37, 6, 23, 0 },
    { 800,  600, 75,  49500,  16,  80, 160,  1, 3, 21, 0 },
    { 800,  600, 85,  56250,  32,  64, 152,  1, 3, 27, 0 },
    { 1024, 768, 60,  65000,  24, 136, 160,  3, 6, 29, N | V },
    { 1024, 768, 70,  75000,  24, 136, 144,  3, 6, 29, N | V },
    { 1024, 768, 75,  78750,  16,  96, 176,  1, 3, 28, 0 },
    { 1024, 768, 85,  94500,  48,  96, 208,  1, 3, 36, 0 },
    { 1152, 864, 75, 108000,  64, 128, 256,  1, 3, 32, 0 },
    { 1280, 960, 60, 108000,  96, 112, 312,  1, 3, 36, 0 },
    { 1280, 960, 85, 148500,  64, 160, 224,  1, 3, 47, 0 },
    { 1280,1024, 60, 108000,  48, 112, 248,  1, 3, 38, 0 },
    { 1280,1024, 75, 135000,  16, 144, 248,  1, 3, 38, 0 },
    { 1280,1024, 85, 157500,  64, 160, 224,  1, 3, 44, 0 },
    /* 1600x1200: NOT DMT (htotal 2160 = 270 chars). The vendor driver keeps
     * DMT's porches but shortens the back porch to htotal 2088 = 261 chars,
     * the most 1X mode allows (golden capture; see the 2X rule below). */
    { 1600,1200, 60, 156600,  64, 192, 232,  1, 3, 46, 0 },
    { 1600,1200, 65, 169650,  64, 192, 232,  1, 3, 46, 0 },
    { 1600,1200, 70, 182700,  64, 192, 232,  1, 3, 46, 0 },
    { 1600,1200, 75, 195750,  64, 192, 232,  1, 3, 46, 0 },
    { 1600,1200, 85, 221850,  64, 192, 232,  1, 3, 46, 0 },
    { 1920,1440, 60, 234000, 128, 208, 344,  1, 3, 56, N },
    { 1920,1440, 75, 297000, 144, 224, 352,  1, 3, 56, N },
    /* More modes the vendor driver offers on .124 and its monitor accepts,
     * decoded from its CRTC by tools/golden_timings.py (GTF-style timings at
     * 70-120 Hz, 720x480/576, 960x720, 1600x1024). */
{  320, 200,  85,  15749,  16,  32,  48,  1, 3, 41, N | D },
    {  320, 240,  72,  15749,  16,  16,  64,  9, 3, 28, N | V | D },
    {  320, 240,  75,  15901,   8,  32,  64,  1, 3, 16, N | V | D },
    {  320, 240,  85,  17996,  24,  32,  40,  1, 3, 25, N | V | D },
    {  400, 300,  72,  24957,  32,  56,  32, 37, 6, 23, D },
    {  400, 300,  75,  24758,   8,  40,  80,  1, 3, 21, D },
    {  400, 300,  85,  28337,  16,  32,  80,  1, 3, 27, D },
    {  512, 384,  70,  37435,  16,  64,  72,  3, 6, 29, N | V | D },
    {  512, 384,  75,  39374,   8,  48,  88,  1, 3, 28, N | V | D },
    {  512, 384,  85,  47249,  24,  48, 104,  1, 3, 36, N | V | D },
    {  640, 400,  85,  31499,  32,  64,  96,  1, 3, 41, N },
    {  640, 480, 100,  43152,  40,  64, 104,  1, 3, 25, N },
    {  640, 480, 120,  52414,  40,  64, 104,  1, 3, 31, N },
    {  720, 480,  60,  28188,  16, 120,  40, 10, 2, 33, N | V },
    {  720, 480,  72,  34831,  16, 136,  48,  9, 3, 28, N | V },
    {  720, 480,  85,  40493,  64,  64,  88,  1, 3, 25, N | V },
    {  720, 576,  60,  36306,  32,  96,  88,  2, 4, 60, 0 },
    {  720, 576,  72,  43252,  32,  96,  88,  2, 4, 60, 0 },
    {  720, 576, 100,  60084,  32,  96,  88,  2, 4, 60, 0 },
    {  800, 600, 100,  68308,  48,  88, 136,  1, 3, 32, N },
    {  800, 600, 120,  83919,  56,  88, 144,  1, 3, 39, N },
    {  960, 720,  60,  55840,  48,  96, 144,  1, 3, 22, N },
    {  960, 720,  75,  72186,  56, 104, 160,  1, 3, 28, N },
    { 1024, 768, 100, 113350,  72, 112, 184,  1, 3, 42, N },
    { 1152, 864,  60,  80050,  32,  96, 192,  1, 3, 37, 0 },
    { 1152, 864,  70,  94498,  32,  96, 200,  1, 3, 44, 0 },
    { 1152, 864,  85, 121703,  64, 128, 224,  1, 3, 43, 0 },
    { 1152, 864, 100, 143180,  80, 128, 208,  1, 3, 47, N },
    { 1280, 960,  75, 129884,  88, 136, 224,  1, 3, 38, N },
    { 1600,1024,  60, 133873,  32, 160, 296,  3, 3, 40, N | V },
    { 1600,1024,  76, 169770,  32, 160, 296,  3, 3, 40, N | V },
    { 1600,1024,  85, 189713,  32, 160, 296,  3, 3, 40, N | V },
    /* wide panels */
    { 1280, 720, 60,  74500,  64, 128, 192,  3, 5, 20, N },   /* CVT: CEA's 1650 px htotal is not whole characters */
    { 1280, 800, 60,  83500,  72, 128, 200,  3, 6, 22, N },
    { 1360, 768, 60,  85500,  64, 112, 256,  3, 6, 18, 0 },
    { 1440, 900, 60, 106500,  80, 152, 232,  3, 6, 25, N },
    { 1680,1050, 60, 146250, 104, 176, 280,  3, 6, 30, N },
    { 1920,1080, 60, 148500,  88,  44, 148,  4, 5, 36, 0 },
    { 1920,1200, 60, 154000,  48,  32,  80,  3, 6, 26, V },
};
#undef N
#undef V
#undef D
const vcr_u32 vcr_ntimings = sizeof(vcr_timings) / sizeof(vcr_timings[0]);

/* ------------------------------------------------------------------------ */

/* fref = 14.31818 MHz, kept exact: 1431818 / 100 kHz */
#define VCR_FREF_X100 1431818u

vcr_u32 vcr_pll_khz(vcr_u32 pll)
{
    vcr_u32 n = (pll >> VCR_PLL_N_SHIFT) & 0xff;
    vcr_u32 m = (pll >> VCR_PLL_M_SHIFT) & 0x3f;
    vcr_u32 k = (pll >> VCR_PLL_K_SHIFT) & 0x3;
    return (VCR_FREF_X100 * (n + 2) / ((m + 2) * 100u)) >> k;
}

/*
 * pllCtrl0 for a dot clock, chosen by the vendor's rules (H5 h3modeset.c),
 * which keep the PLL's VCO (f * 2^K) in a few hundred MHz:
 *   K from the clock: 1 above 150 MHz, 2 above 65 MHz, else 3;
 *   M from 1 (never 0 - the vendor found it misbehaves), at most 10 above
 *   36 MHz and at most 5 above 200 MHz;  N + 2 <= 257;
 *   the lowest error wins, the first found (smallest M) on a tie.
 * The open drivers' exhaustive search lands on the same frequency with a
 * wildly different VCO: for 157.5 MHz it picked N=174 M=0 K=3, a 1.26 GHz VCO,
 * where the vendor runs 315 MHz - measured on the V5 6000 with our first boot.
 */
vcr_u32 vcr_pll_calc(vcr_u32 khz, vcr_u32 *actual)
{
    vcr_u32 k = khz > 150000 ? 1 : khz > 65000 ? 2 : 3;
    vcr_u32 m, best = 0, best_err = 0xffffffffu;

    for (m = 1; m < 64; m++) {
        unsigned long long num;
        vcr_u32 np2, f, err, div;
        if (khz > 36000 && m > 10)
            break;
        if (khz > 200000 && m > 5)
            break;
        /* n + 2 = round(khz * (m + 2) * 2^k / fref) */
        num = (unsigned long long)khz * (m + 2) * (1u << k) * 100u;
        np2 = (vcr_u32)((num + VCR_FREF_X100 / 2) / VCR_FREF_X100);
        if (np2 > 257 || np2 < 2)
            continue;
        div = (m + 2) * 100u * (1u << k);
        f = (vcr_u32)(((unsigned long long)VCR_FREF_X100 * np2 + div / 2) / div);
        err = f > khz ? f - khz : khz - f;
        if (err < best_err) {
            best_err = err;
            best = VCR_PLL(np2 - 2, m, k);
        }
    }
    if (actual)
        *actual = vcr_pll_khz(best);
    return best;
}

/* ------------------------------------------------------------------------ */

int vcr_mode_check(const vcr_hwcaps *hw, const vcr_timing *t, unsigned bpp)
{
    vcr_u32 need;
    if (bpp != 8 && bpp != 16 && bpp != 32)
        return VCR_MODE_E_BPP;
    /* Doublescan (HALF mode) at every depth: the vendor programs 320x240x32
     * exactly like 320x240x16 (golden capture; its source comment suggesting
     * <= 16 bpp describes a different branch). */
    if (t->pixclk_khz > hw->max_pixclk_khz)
        return VCR_MODE_E_PIXCLK;
    /* CRTC field widths: 9-bit horizontal chars, 11-bit vertical lines */
    if (t->w + t->hfp + t->hsync + t->hbp > 4096 ||
        (vcr_u32)t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) + t->vfp + t->vsync + t->vbp > 2048)
        return VCR_MODE_E_RANGE;
    need = (vcr_u32)t->w * t->h * (bpp / 8);
    if (need + hw->fb_reserved > hw->fb_bytes)
        return VCR_MODE_E_MEMORY;
    /* The monitor's declared ranges, with half a unit of slack for rounding
     * (EDID stores whole kHz / Hz; a "75 Hz" timing runs at 75.03). */
    if (hw->mon_hmax_khz) {
        vcr_u32 hf = vcr_timing_hfreq_hz(t), vf = vcr_timing_vfreq_mhz(t);
        if (hf + 500 < hw->mon_hmin_khz * 1000u || hf > hw->mon_hmax_khz * 1000u + 500 ||
            vf + 500 < hw->mon_vmin_hz * 1000u || vf > hw->mon_vmax_hz * 1000u + 500 ||
            (hw->mon_max_pixclk_khz && t->pixclk_khz > hw->mon_max_pixclk_khz))
            return VCR_MODE_E_MONITOR;
    }
    return 0;
}

vcr_u32 vcr_timing_hfreq_hz(const vcr_timing *t)
{
    vcr_u32 htot = (vcr_u32)t->w + t->hfp + t->hsync + t->hbp;
    return htot ? t->pixclk_khz * 1000u / htot : 0;
}

vcr_u32 vcr_timing_vfreq_mhz(const vcr_timing *t)
{
    vcr_u32 vtot = (vcr_u32)t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) +
                   t->vfp + t->vsync + t->vbp;
    return vtot ? vcr_timing_hfreq_hz(t) * 1000u / vtot : 0;
}

int vcr_mode_compute(const vcr_hwcaps *hw, const vcr_timing *t, unsigned bpp,
                     vcr_modeset *o)
{
    vcr_u32 hdisp, hss, hse, htot;
    vcr_u32 hd, hs, he, ht, hbs, hbe;
    vcr_u32 vd, vs, ve, vt, vbs, vbe;
    vcr_u32 i, fmt, vtot_lines;
    int rc = vcr_mode_check(hw, t, bpp);
    if (rc)
        return rc;

    for (i = 0; i < sizeof(*o); i++)
        ((vcr_u8 *)o)[i] = 0;

    fmt = bpp == 8 ? VCR_VPC_FMT_PAL8 : bpp == 16 ? VCR_VPC_FMT_RGB565
                                                  : VCR_VPC_FMT_RGB32;
    o->vidproccfg = VCR_VPC_VIDEO_PROCESSOR_EN | VCR_VPC_DESKTOP_EN |
                    (fmt << VCR_VPC_DESKTOP_FMT_SHIFT);
    /* Every depth reads CLUT bank 0, not bypassed (so gamma ramps work at
     * 16/32 bpp): what the vendor driver programs - golden capture
     * golden/amigamerlin-3.1-r11_192.168.1.124.json, vidProcCfg 0x09040081. */
    if (VCR_IS_NAPALM(hw->device_id))
        o->vidproccfg |= hw->napalm_vpc_extra;

    o->pix_khz_target = t->pixclk_khz;
    o->pllctrl0 = vcr_pll_calc(t->pixclk_khz, &o->pix_khz_actual);

    hdisp = t->w;
    hss = hdisp + t->hfp;
    hse = hss + t->hsync;
    htot = hse + t->hbp;
    /* 2X exactly as the vendor decides it (H5 h3modeset.c): VSA-100 above
     * 262 MHz at width >= 1280, OR when htotal exceeds 261 characters,
     * because horizontal blank end is only 6 bits; older chips above 160 MHz
     * at width >= 1280. */
    if ((t->pixclk_khz > hw->twox_above_khz && t->w >= 1280) ||
        (hw->twox_htotal_chars && (htot >> 3) > hw->twox_htotal_chars)) {
        o->twox = 1;
        o->dacmode |= VCR_DAC_MODE_2X;
        o->vidproccfg |= VCR_VPC_2X_MODE_EN;
        hdisp >>= 1;
        hss >>= 1;
        hse >>= 1;
        htot >>= 1;
    }
    /* CRTC values. Totals carry the VGA -5 / -2 bias; the sync start and end
     * are programmed ONE character / line early - exactly what the vendor
     * driver writes on the V5 6000 (golden capture with vcrprobe, 51 modes:
     * CR04/05/10/11 one below standard VGA, everything else identical), and
     * what tdfxfb does. The X.org vgaHW convention (no -1) is off by one on
     * this silicon. */
    ht = (htot >> 3) - 5;
    hd = (hdisp >> 3) - 1;
    hbs = hd;
    hbe = (htot >> 3) - 1;
    hs = (hss >> 3) - 1;
    he = (hse >> 3) - 1;

    {
        vcr_u32 k = (t->flags & VCR_T_DBLSCAN) ? 2 : 1;
        vcr_u32 vdisp = (vcr_u32)t->h * k;
        vcr_u32 vss = vdisp + t->vfp;              /* porches: physical lines */
        vcr_u32 vse = vss + t->vsync;
        vcr_u32 vtot = vse + t->vbp;
        vd = vdisp - 1;
        vbs = vdisp - 1;
        vs = vss - 1;
        ve = vse - 1;
        vbe = vtot - 1;
        vt = vtot - 2;
    }
    if (t->flags & VCR_T_DBLSCAN) {
        o->vidscreensize = (vcr_u32)t->w | ((vcr_u32)t->h << 13);
        o->vidproccfg |= VCR_VPC_HALF_MODE;
        o->crtc[0x09] = 0x80;
    } else {
        o->vidscreensize = (vcr_u32)t->w | ((vcr_u32)t->h << 12);
    }

    /* misc output: colour I/O, RAM on, clock select 3 (the PLL) and the sync
     * polarities the monitor identifies the mode by. No page bit (0x20): the
     * vendor writes 0x0f | polarity in every captured mode. */
    o->misc = 0x0f | ((t->flags & VCR_T_HNEG) ? 0x40 : 0) |
              ((t->flags & VCR_T_VNEG) ? 0x80 : 0);

    o->seq[0] = 0x03;
    o->seq[1] = 0x01;
    o->seq[2] = 0x0f;
    o->seq[3] = 0x00;
    o->seq[4] = 0x0e;

    o->gfx[5] = 0x40;
    o->gfx[6] = 0x05;
    o->gfx[7] = 0x0f;
    o->gfx[8] = 0xff;

    for (i = 0; i < 16; i++)
        o->attr[i] = (vcr_u8)i;
    o->attr[0x10] = 0x41;
    o->attr[0x12] = 0x0f;

    o->crtc[0x00] = (vcr_u8)ht;
    o->crtc[0x01] = (vcr_u8)hd;
    o->crtc[0x02] = (vcr_u8)hbs;
    o->crtc[0x03] = (vcr_u8)(0x80 | (hbe & 0x1f));
    o->crtc[0x04] = (vcr_u8)hs;
    o->crtc[0x05] = (vcr_u8)(((hbe & 0x20) << 2) | (he & 0x1f));
    o->crtc[0x06] = (vcr_u8)vt;
    o->crtc[0x07] = (vcr_u8)(((vs & 0x200) >> 2) | ((vd & 0x200) >> 3) |
                             ((vt & 0x200) >> 4) | 0x10 | ((vbs & 0x100) >> 5) |
                             ((vs & 0x100) >> 6) | ((vd & 0x100) >> 7) |
                             ((vt & 0x100) >> 8));
    o->crtc[0x09] |= (vcr_u8)(0x40 | ((vbs & 0x200) >> 4));
    o->crtc[0x10] = (vcr_u8)vs;
    o->crtc[0x11] = (vcr_u8)((ve & 0x0f) | 0x20);
    o->crtc[0x12] = (vcr_u8)vd;
    /* CR13 (offset) and CR17 (mode control) do not drive the 3dfx desktop,
     * which the video processor fetches; the values are the vendor's */
    o->crtc[0x13] = 0x28;
    o->crtc[0x15] = (vcr_u8)vbs;
    o->crtc[0x16] = (vcr_u8)vbe;
    o->crtc[0x17] = 0x80;
    o->crtc[0x18] = 0xff;

    /* 3dfx horizontal / vertical overflow registers (CR1A / CR1B), bit
     * layout as both open drivers (tdfxfb, xf86-video-tdfx) program it */
    /* CR1A bit 5 is bit 6 of the blank-end value AS THE VENDOR FORMS IT:
     * (htotal - hdisp) + ((hdisp - 1) & 63) in characters. Its low six bits
     * equal (htotal - 1)'s, which is all CR03/CR05 hold, but bit 6 differs -
     * tdfxfb and X.org use htotal - 1 and disagree with the vendor driver in
     * 37 of 51 captured modes (golden_compare.py). */
    {
        vcr_u32 hbe_vendor = ((htot - hdisp) >> 3) + (hd & 0x3f);
        o->crtc_ext[0] = (vcr_u8)(((ht & 0x100) >> 8) | ((hd & 0x100) >> 6) |
                                  ((hbs & 0x100) >> 4) | ((hbe_vendor & 0x40) >> 1) |
                                  ((hs & 0x100) >> 2) | ((he & 0x20) << 2));
    }
    o->crtc_ext[1] = (vcr_u8)(((vt & 0x400) >> 10) | ((vd & 0x400) >> 8) |
                              ((vbs & 0x400) >> 6) | ((vbe & 0x400) >> 4));

    /* 0x1140, as the vendor driver leaves it in every captured mode (tdfxfb
     * also sets the 8-bit-DAC and alt-readback bits; the vendor does not) */
    o->vgainit0_set = VCR_VGA0_EXTENSIONS | VCR_VGA0_WAKEUP_3C3 |
                      VCR_VGA0_EXTSHIFTOUT;

    o->stride = (vcr_u32)t->w * (bpp / 8);

    vtot_lines = (vcr_u32)t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) +
                 t->vfp + t->vsync + t->vbp;
    {
        vcr_u32 htot_px = (vcr_u32)t->w + t->hfp + t->hsync + t->hbp;
        /* kHz * 1000 / px = Hz; keep the milli-Hz product inside 32 bits */
        o->hfreq_hz = (o->pix_khz_actual * 1000u) / htot_px;
        o->refresh_mhz = (o->hfreq_hz * 1000u) / vtot_lines;
    }
    return 0;
}

vcr_u32 vcr_modes_build(const vcr_hwcaps *hw, vcr_mode *out, vcr_u32 max)
{
    static const vcr_u8 depths[3] = { 8, 16, 32 };
    vcr_u32 n = 0, i, d;
    for (d = 0; d < 3; d++)
        for (i = 0; i < vcr_ntimings; i++) {
            if (n >= max)
                return n;
            if (vcr_mode_check(hw, &vcr_timings[i], depths[d]) == 0) {
                out[n].timing = (vcr_u16)i;
                out[n].bpp = depths[d];
                out[n].valid = 1;
                n++;
            }
        }
    return n;
}

vcr_u32 vcr_desktop_offset(vcr_u32 fb_bytes, vcr_u32 stride, vcr_u32 height)
{
    vcr_u32 size = (stride * height + 0xfffu) & ~0xfffu;
    return size > fb_bytes ? 0 : fb_bytes - size;
}

int vcr_timing_find(vcr_u32 w, vcr_u32 h, vcr_u32 refresh)
{
    vcr_u32 i;
    int best = -1;
    for (i = 0; i < vcr_ntimings; i++) {
        const vcr_timing *t = &vcr_timings[i];
        if (t->w != w || t->h != h)
            continue;
        if (refresh > 1) {
            if (t->refresh == refresh)
                return (int)i;
        } else if (best < 0 || t->refresh < vcr_timings[best].refresh) {
            best = (int)i;
        }
    }
    return best;
}
