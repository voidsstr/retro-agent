/*
 * vcr_modes.c - timings table, PLL search and the per-mode register recipe.
 * See include/vcr_modes.h. Integer arithmetic only (runs in kernel mode).
 */
#include "../include/vcr_modes.h"
#include "../include/vcr_regs.h"

/*
 * VESA DMT (and CEA-861 / CVT where DMT has no entry) timings.
 *   w, h, Hz, pixclk kHz, hfp, hsync, hbp, vfp, vsync, vbp, flags
 * Doublescan rows give LOGICAL lines; the hardware scans each twice.
 * Order matters only for presentation: modes are listed in this order.
 */
#define N VCR_T_HNEG
#define V VCR_T_VNEG
#define D VCR_T_DBLSCAN
const vcr_timing vcr_timings[] = {
    /* low resolution, doublescanned (Glide 320x240 / 400x300 / 512x384) */
    { 320,  200, 70,  12587,   8,  48,  24,  6, 1, 18, N | D },
    { 320,  240, 60,  12587,   8,  48,  24,  5, 1, 17, N | V | D },
    { 400,  300, 60,  20000,  20,  64,  44,  1, 2, 11, D },
    { 512,  384, 60,  32500,  12,  68,  80,  2, 3, 14, N | V | D },
    { 640,  400, 70,  25175,  16,  96,  48, 12, 2, 35, N },
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
    { 1600,1200, 60, 162000,  64, 192, 304,  1, 3, 46, 0 },
    { 1600,1200, 65, 175500,  64, 192, 304,  1, 3, 46, 0 },
    { 1600,1200, 70, 189000,  64, 192, 304,  1, 3, 46, 0 },
    { 1600,1200, 75, 202500,  64, 192, 304,  1, 3, 46, 0 },
    { 1600,1200, 85, 229500,  64, 192, 304,  1, 3, 46, 0 },
    { 1920,1440, 60, 234000, 128, 208, 344,  1, 3, 56, N },
    { 1920,1440, 75, 297000, 144, 224, 352,  1, 3, 56, N },
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

vcr_u32 vcr_pll_khz(vcr_u32 pll)
{
    vcr_u32 n = (pll >> VCR_PLL_N_SHIFT) & 0xff;
    vcr_u32 m = (pll >> VCR_PLL_M_SHIFT) & 0x3f;
    vcr_u32 k = (pll >> VCR_PLL_K_SHIFT) & 0x3;
    return (VCR_PLL_REF_KHZ * (n + 2) / (m + 2)) >> k;
}

vcr_u32 vcr_pll_calc(vcr_u32 khz, vcr_u32 *actual)
{
    vcr_u32 best = 0, best_err = 0xffffffffu;
    int k, m, n;

    for (k = 3; k >= 0; k--) {
        for (m = 63; m >= 0; m--) {
            int est = (int)(((khz * (vcr_u32)(m + 2)) << k) / VCR_PLL_REF_KHZ) - 2;
            for (n = est < 0 ? 0 : est; n <= est + 1 && n <= 255; n++) {
                vcr_u32 f = (VCR_PLL_REF_KHZ * (vcr_u32)(n + 2) / (vcr_u32)(m + 2)) >> k;
                vcr_u32 err = f > khz ? f - khz : khz - f;
                if (err < best_err) {
                    best_err = err;
                    best = VCR_PLL((vcr_u32)n, (vcr_u32)m, (vcr_u32)k);
                }
            }
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
    /* Doublescan (HALF mode) only at <= 16 bpp, as the vendor driver does. */
    if ((t->flags & VCR_T_DBLSCAN) && bpp > 16)
        return VCR_MODE_E_BPP;
    if (t->pixclk_khz > hw->max_pixclk_khz)
        return VCR_MODE_E_PIXCLK;
    /* CRTC field widths: 9-bit horizontal chars, 11-bit vertical lines */
    if (t->w + t->hfp + t->hsync + t->hbp > 4096 ||
        (vcr_u32)(t->h + t->vfp + t->vsync + t->vbp) * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) > 2048)
        return VCR_MODE_E_RANGE;
    need = (vcr_u32)t->w * t->h * (bpp / 8);
    if (need + hw->fb_reserved > hw->fb_bytes)
        return VCR_MODE_E_MEMORY;
    return 0;
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
    if (t->pixclk_khz > hw->twox_above_khz) {
        o->twox = 1;
        o->dacmode |= VCR_DAC_MODE_2X;
        o->vidproccfg |= VCR_VPC_2X_MODE_EN;
        hdisp >>= 1;
        hss >>= 1;
        hse >>= 1;
        htot >>= 1;
    }
    /* CRTC values with VGA / X.org vgaHW semantics (xf86-video-tdfx
     * TDFXSetMode): sync start/end are the character or line on which the
     * pulse begins/ends, totals carry the VGA -5 / -2 bias. (tdfxfb programs
     * both sync starts one unit early; the golden captures from the vendor
     * driver arbitrate - see tools/vcrprobe and test_vcr_kmd_modes.c.) */
    ht = (htot >> 3) - 5;
    hd = (hdisp >> 3) - 1;
    hbs = hd;
    hbe = (htot >> 3) - 1;
    hs = hss >> 3;
    he = hse >> 3;

    {
        vcr_u32 k = (t->flags & VCR_T_DBLSCAN) ? 2 : 1;
        vcr_u32 vdisp = (vcr_u32)t->h * k;
        vcr_u32 vss = vdisp + (vcr_u32)t->vfp * k;
        vcr_u32 vse = vss + (vcr_u32)t->vsync * k;
        vcr_u32 vtot = vse + (vcr_u32)t->vbp * k;
        vd = vdisp - 1;
        vbs = vdisp - 1;
        vs = vss;
        ve = vse;
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

    /* misc output: colour I/O, RAM on, clock select 3 (the PLL), page bit,
     * and the sync polarities the monitor uses to identify the mode */
    o->misc = 0x2f | ((t->flags & VCR_T_HNEG) ? 0x40 : 0) |
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
    o->crtc[0x13] = (vcr_u8)hd;
    o->crtc[0x15] = (vcr_u8)vbs;
    o->crtc[0x16] = (vcr_u8)vbe;
    o->crtc[0x17] = 0xc3;
    o->crtc[0x18] = 0xff;

    /* 3dfx horizontal / vertical overflow registers (CR1A / CR1B), bit
     * layout as both open drivers (tdfxfb, xf86-video-tdfx) program it */
    o->crtc_ext[0] = (vcr_u8)(((ht & 0x100) >> 8) | ((hd & 0x100) >> 6) |
                              ((hbs & 0x100) >> 4) | ((hbe & 0x40) >> 1) |
                              ((hs & 0x100) >> 2) | ((he & 0x20) << 2));
    o->crtc_ext[1] = (vcr_u8)(((vt & 0x400) >> 10) | ((vd & 0x400) >> 8) |
                              ((vbs & 0x400) >> 6) | ((vbe & 0x400) >> 4));

    /* 0x1140, as the vendor driver leaves it in every captured mode (tdfxfb
     * also sets the 8-bit-DAC and alt-readback bits; the vendor does not) */
    o->vgainit0_set = VCR_VGA0_EXTENSIONS | VCR_VGA0_WAKEUP_3C3 |
                      VCR_VGA0_EXTSHIFTOUT;

    o->stride = (vcr_u32)t->w * (bpp / 8);

    vtot_lines = ((vcr_u32)t->h + t->vfp + t->vsync + t->vbp) *
                 ((t->flags & VCR_T_DBLSCAN) ? 2 : 1);
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
