/*
 * vcr_modes.h - display timings and the register values that realise them on
 * Banshee / Voodoo 3 / VSA-100. Pure computation, no hardware access: the
 * miniport programs the result, and the host tests (tests/native/
 * test_vcr_kmd_modes.c) pin it, including against register dumps captured
 * from a known-good driver on the real card (tools/vcrprobe).
 *
 * The register recipe follows the open Linux tdfxfb driver (GPL):
 * standard VGA CRTC stuffing plus the two 3dfx CRTC extension registers
 * 0x1a/0x1b, pllCtrl0 from an exhaustive N/M/K search, and dacMode/vidProcCfg
 * 2X mode (two pixels per CRTC clock, horizontal CRTC values halved) above a
 * pixel-clock threshold.
 */
#ifndef VCR_MODES_H
#define VCR_MODES_H

#include "vcr_types.h"

/* timing flags */
#define VCR_T_HNEG      0x01    /* hsync active low */
#define VCR_T_VNEG      0x02    /* vsync active low */
#define VCR_T_DBLSCAN   0x04    /* each line scanned twice (low-res modes) */

typedef struct vcr_timing {
    vcr_u16 w, h;           /* visible pixels / lines */
    vcr_u16 refresh;        /* nominal Hz (the number a mode list shows) */
    vcr_u32 pixclk_khz;     /* dot clock */
    vcr_u16 hfp, hsync, hbp;    /* pixels */
    vcr_u16 vfp, vsync, vbp;    /* lines (before doublescan) */
    vcr_u8  flags;
} vcr_timing;

/* What the chip family can do - decided by the miniport from the PCI id. */
typedef struct vcr_hwcaps {
    vcr_u32 device_id;          /* VCR_DEV_* */
    vcr_u32 max_pixclk_khz;     /* RAMDAC limit */
    vcr_u32 twox_above_khz;     /* 2X above this dot clock at width >= 1280 */
    vcr_u32 twox_htotal_chars;  /* ... or above this htotal (VSA-100: 261), 0 = none */
    vcr_u32 fb_bytes;           /* local memory of the chip driving the display */
    vcr_u32 fb_reserved;        /* bytes the display cannot use (cursor, etc.) */
    vcr_u32 napalm_vpc_extra;   /* extra vidProcCfg bits on VSA-100 (see .c) */
    /* the monitor's scan limits. mon_hmax_khz 0 = none: vcr_mode_check then
     * filters nothing - but the miniport never leaves it 0 on real hardware:
     * it sets the EDID's range, else a same-monitor / envelope / default one
     * (vcr_mon_select, vcr_edid.h). Only Diag\EdidFilter = 0 and the QEMU
     * bed's virtual display run with 0. */
    vcr_u32 mon_hmin_khz, mon_hmax_khz, mon_vmin_hz, mon_vmax_hz, mon_max_pixclk_khz;
} vcr_hwcaps;

/* Everything a mode set writes. VGA arrays are in register-index order. */
typedef struct vcr_modeset {
    vcr_u8  misc;               /* 0x3c2 */
    vcr_u8  seq[5];
    vcr_u8  crtc[25];           /* 0x00 - 0x18 */
    vcr_u8  crtc_ext[2];        /* 0x1a, 0x1b */
    vcr_u8  gfx[9];
    vcr_u8  attr[21];
    vcr_u32 pllctrl0;
    vcr_u32 dacmode;
    vcr_u32 vidproccfg;
    vcr_u32 vidscreensize;
    vcr_u32 stride;             /* bytes per line (desktop half of the stride reg) */
    vcr_u32 vgainit0_set;       /* bits to OR into vgaInit0 */
    vcr_u32 pix_khz_target;
    vcr_u32 pix_khz_actual;
    vcr_u32 refresh_mhz;        /* achieved vertical refresh, milli-Hz */
    vcr_u32 hfreq_hz;           /* achieved horizontal frequency */
    vcr_u32 twox;               /* 1 if 2X mode is used */
} vcr_modeset;

/* A mode the driver offers: a timing and a pixel depth. */
typedef struct vcr_mode {
    vcr_u16 timing;             /* index into vcr_timings[] */
    vcr_u8  bpp;                /* 8, 16, 32 */
    vcr_u8  valid;
} vcr_mode;

extern const vcr_timing vcr_timings[];
extern const vcr_u32    vcr_ntimings;

/* pllCtrl0 value nearest to `khz`; *actual receives the frequency it gives. */
vcr_u32 vcr_pll_calc(vcr_u32 khz, vcr_u32 *actual);
vcr_u32 vcr_pll_khz(vcr_u32 pllctrl);

/* 0 = ok, else a VCR_MODE_E_* reason. */
#define VCR_MODE_E_BPP      1
#define VCR_MODE_E_PIXCLK   2
#define VCR_MODE_E_MEMORY   3
#define VCR_MODE_E_RANGE    4
#define VCR_MODE_E_MONITOR  5   /* outside the monitor's EDID range limits */
int vcr_mode_check(const vcr_hwcaps *hw, const vcr_timing *t, unsigned bpp);
int vcr_mode_compute(const vcr_hwcaps *hw, const vcr_timing *t, unsigned bpp,
                     vcr_modeset *out);

/* Fill out[] with every (timing, bpp) the hardware can show, in table order.
 * Returns how many were written (<= max). */
vcr_u32 vcr_modes_build(const vcr_hwcaps *hw, vcr_mode *out, vcr_u32 max);

/* A timing's scan rates from its own numbers (nominal dot clock). */
vcr_u32 vcr_timing_hfreq_hz(const vcr_timing *t);
vcr_u32 vcr_timing_vfreq_mhz(const vcr_timing *t);

/* Where the desktop goes in video memory: the TOP, like the vendor driver
 * (fb - stride * height, rounded down to 4 KB). Glide owns the bottom - its
 * command FIFO spans 96 KB .. ~1116 KB - and a GDI write that lands while a
 * game runs must hit a colour buffer, never the FIFO. Returns 0 if the
 * desktop does not fit. */
vcr_u32 vcr_desktop_offset(vcr_u32 fb_bytes, vcr_u32 stride, vcr_u32 height);

/* Index into vcr_timings[] of the timing matching w x h @ refresh, or of the
 * lowest refresh at w x h when refresh is 0/1 ("default"), or -1. */
int vcr_timing_find(vcr_u32 w, vcr_u32 h, vcr_u32 refresh);

#endif /* VCR_MODES_H */
