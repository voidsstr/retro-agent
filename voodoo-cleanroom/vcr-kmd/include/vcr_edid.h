/*
 * vcr_edid.h - the monitor's EDID (VESA E-EDID 1.3/1.4 base block): what the
 * miniport reads over DDC, and what the mode list is filtered by.
 *
 * A CRT driven outside its horizontal range goes blank, whines, or - on old
 * tubes - is damaged; the ranges the monitor declares (the Display Range
 * Limits descriptor, tag 0xFD) are the contract. Without an EDID (no DDC, a
 * KVM, a checksum failure) nothing is filtered and the log says so: an
 * unfiltered list is the pre-EDID behaviour, not a new risk.
 *
 * Pure code (kernel, display DLL, host tests). Integer only.
 */
#ifndef VCR_EDID_H
#define VCR_EDID_H

#include "vcr_types.h"

#define VCR_EDID_BLOCK  128

typedef struct vcr_edid_info {
    vcr_u32 valid;              /* header and checksum good */
    char    pnpid[4];           /* manufacturer, e.g. "SNY" */
    vcr_u16 product;            /* product code */
    vcr_u8  version, revision;  /* 1.3, 1.4 ... */
    vcr_u8  digital;            /* video input definition bit 7 */
    vcr_u8  extensions;
    char    name[14];           /* Monitor Name descriptor (0xFC), trimmed */
    vcr_u32 width_cm, height_cm;
    /* Display Range Limits (0xFD); has_range = 0 when the EDID carries none */
    vcr_u32 has_range;
    vcr_u32 vmin_hz, vmax_hz, hmin_khz, hmax_khz, max_pixclk_khz;
    /* the first detailed timing: the preferred mode */
    vcr_u32 pref_w, pref_h, pref_pixclk_khz, pref_hfreq_hz, pref_refresh_mhz;
} vcr_edid_info;

/* 1 = a valid base block, parsed into *out; 0 = not an EDID (out->valid 0). */
int vcr_edid_parse(const vcr_u8 *e, vcr_u32 len, vcr_edid_info *out);

/* Copy the range limits into the mode builder's caps; 0 (and caps cleared)
 * when the EDID is invalid or declares no ranges. */
struct vcr_hwcaps;
int vcr_hwcaps_set_monitor(struct vcr_hwcaps *hw, const vcr_edid_info *e);

#endif /* VCR_EDID_H */
