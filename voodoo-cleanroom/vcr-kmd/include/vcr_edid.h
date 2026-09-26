/*
 * vcr_edid.h - the monitor's EDID (VESA E-EDID 1.3/1.4 base block): what the
 * miniport reads over DDC, and what the mode list is filtered by.
 *
 * A CRT driven outside its horizontal range goes blank, whines, or - on old
 * tubes - is damaged; the ranges the monitor declares (the Display Range
 * Limits descriptor, tag 0xFD) are the contract.
 *
 * The list is NEVER left unfiltered for want of an EDID. It used to be: with
 * the monitor off at boot, a KVM, a bad pair, a bad checksum or Diag\Ddc = 0,
 * the whole table became settable - 1600x1200@85 (106 kHz) and 1920x1440@75
 * (297 MHz) on .124's 96 kHz / 260 MHz Sony CPD-G200, the same tube the
 * 2026-09-26 battery put through ~250 re-syncs. So the limits in force come
 * from vcr_mon_select(): the EDID's own range; else, for an EDID that states
 * none, the persisted range only if it was persisted by this SAME monitor;
 * else, with no EDID at all, the ENVELOPE - the intersection of every monitor
 * whose EDID this box has read - bounded by the conservative default; else
 * the default alone. Never "the last monitor's range": the tube that answered
 * last week is not evidence about the one plugged in now, and handing a
 * 60 kHz tube a 96 kHz one's range is the very out-of-range drive the filter
 * exists to stop. And never the bare envelope by default either: it only
 * knows the monitors our DDC could read, and the tube with no DDC, or behind
 * a KVM that swallows it, is exactly the no-EDID case - by construction never
 * narrowed into it. Diag\MonTrustEnvelope = 1 (vcr_mon_trust_envelope) lifts
 * the bound for an operator who knows which tube sits behind the KVM.
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

/* A monitor's scan limits, whatever they came from. max_pixclk_khz 0 = the
 * monitor states no dot-clock limit (vcr_mode_check reads it the same way). */
typedef struct vcr_mon_range {
    vcr_u32 hmin_khz, hmax_khz, vmin_hz, vmax_hz, max_pixclk_khz;
} vcr_mon_range;

/* The monitor's identity, as Diag\MonId persists it and VCR_EV_EDID's `a`
 * carries it: the three 5-bit PnP letters (bits 0-14) | product << 16. The
 * serial number is left out on purpose: a model's range is the model's, and
 * two units of one model are the same tube as far as the filter cares.
 * 0 for NULL or an invalid EDID. */
vcr_u32 vcr_mon_id(const vcr_edid_info *e);

/* where the limits in force came from (vcr_mon_select) */
#define VCR_MON_SRC_NONE        0   /* none: Diag\EdidFilter = 0, or a virtual display */
#define VCR_MON_SRC_EDID        1   /* the EDID read this boot */
#define VCR_MON_SRC_SAME        2   /* EDID without ranges: persisted by this same monitor */
#define VCR_MON_SRC_ENVELOPE    3   /* no EDID: what every monitor seen on the box accepts,
                                     * bounded by VCR_MON_DEF_* (unless trusted) */
#define VCR_MON_SRC_DEFAULT     4   /* nothing usable known: VCR_MON_DEF_* */

/* "edid", "same-monitor persisted", "envelope", "default", "none" - for the
 * log line that names where a filter came from. */
const char *vcr_mon_src_name(vcr_u32 src);

/* The fallback when nothing usable is known, and the bound on the envelope
 * when no EDID answered: a conservative, X.org-style safe-for-any-monitor
 * range - H 30-48 kHz, V 50-75 Hz, 80 MHz. It keeps 640x480@60-75,
 * 800x600@56-75 and 1024x768@60 (48.4 kHz, inside the mode check's +0.5 kHz
 * slack; the widest mode left is CVT 1280x720@60 at 44.8 kHz / 74.5 MHz), and
 * refuses 1024x768@70 (56.5 kHz) and up and every 1280x1024. The first
 * default (H 30-70 kHz / V 50-85 Hz / 135 MHz, 1024x768@85) was documented as
 * what "any CRT of the era accepts", which is false: 14" and 15" CRTs of
 * 1995-98 stop between 38 and 60 kHz, so 70 kHz was a mid-range tube's
 * ceiling, not a floor every tube shares. Too narrow costs a mode the
 * operator gets back with an EDID (or MonTrustEnvelope); too wide costs the
 * tube. */
#define VCR_MON_DEF_HMIN_KHZ    30
#define VCR_MON_DEF_HMAX_KHZ    48
#define VCR_MON_DEF_VMIN_HZ     50
#define VCR_MON_DEF_VMAX_HZ     75
#define VCR_MON_DEF_PIXCLK_KHZ  80000

/* 1 = a range a monitor could state: both maxima set, each minimum <= its
 * maximum, inside what EDID 1.4 can encode. A persisted range is a registry
 * value an operator can edit, so it is checked before it is trusted - an
 * absurd one would switch the filter off as surely as none. */
int vcr_mon_range_valid(const vcr_mon_range *r);

/* 1 = a PERSISTED range the list may be built from: valid, and it still
 * carries 640x480@60 by the mode list's own rule (vcr_mode_check). Every tube
 * on this box showed the BIOS's 31.5 kHz text at POST, so an envelope without
 * VGA is bad data rather than a monitor - and a filter that leaves no mode at
 * all protects nothing, it is a black screen. An EMPTY envelope (monitors
 * that share no range: a minimum above its maximum) fails here too. */
int vcr_mon_range_usable(const vcr_mon_range *r);

/* The limits the mode list is filtered by:
 *   e valid, with ranges              -> e's own                  (EDID)
 *   e valid, no ranges, and
 *     env_id == vcr_mon_id(e) != 0    -> *env, if usable          (SAME)
 *   e NULL or invalid (no EDID)       -> *env n VCR_MON_DEF_*,
 *                                        if *env is usable        (ENVELOPE)
 *   anything else                     -> VCR_MON_DEF_*            (DEFAULT)
 * *env is the persisted envelope (vcr_mon_envelope_add), env_id the monitor
 * last narrowed into it. A range-less EDID from any monitor but env_id gets
 * the default, not the envelope: nothing says that monitor was ever narrowed
 * into it, so the envelope is no evidence about it. SAME is the bare
 * envelope, because it is inside that monitor's own range: the envelope only
 * ever narrows the ranges of the monitors in it. With no EDID the tube is
 * unknown - and a tube that cannot answer DDC was never narrowed into the
 * envelope at all - so the envelope may only NARROW the default there, never
 * widen it. env may be NULL.
 * Returns VCR_MON_SRC_* (never NONE: turning the filter off is the caller's). */
vcr_u32 vcr_mon_select(const vcr_edid_info *e, const vcr_mon_range *env, vcr_u32 env_id,
                       vcr_mon_range *out);

/* Diag\MonTrustEnvelope = 1: the operator knows the tube behind the KVM is
 * one this box has read, so a no-EDID boot may use the BARE envelope instead
 * of its intersection with the default. Only for src == ENVELOPE (anything
 * else is left alone) and a usable *env; then *out = *env and 1 is returned -
 * the caller logs that at WARN, every boot, because it is the one setting
 * that lets an unread tube be driven past the default. */
int vcr_mon_trust_envelope(vcr_u32 src, const vcr_mon_range *env, vcr_mon_range *out);

/* Narrow the envelope *env to what *seen also accepts: the larger of the
 * minima, the smaller of the maxima and of the dot clocks (a 0 dot clock is
 * "no limit", not the smallest). An envelope with hmax 0 - nothing persisted,
 * or just cleared by Diag\MonReset - becomes *seen. The result may be EMPTY
 * and is kept that way, so a box whose monitors share no range stays on the
 * default until an operator resets it, instead of forgetting a monitor. An
 * invalid *seen changes nothing. Returns 1 if *env changed. */
int vcr_mon_envelope_add(vcr_mon_range *env, const vcr_mon_range *seen);

/* One boot's whole decision, for the miniport and the tests alike: reset
 * (Diag\MonReset = 1, for a tube that has left the box for good) clears
 * *env / *env_id first; then the limits in force are chosen (vcr_mon_select);
 * then an EDID with ranges is narrowed into *env and *env_id becomes its
 * monitor. Returns VCR_MON_SRC_*. The caller persists *env / *env_id when
 * they changed. */
vcr_u32 vcr_mon_boot(const vcr_edid_info *e, vcr_u32 reset, vcr_mon_range *env,
                     vcr_u32 *env_id, vcr_mon_range *out);

/* Put r into the mode builder's caps; NULL clears them (nothing filtered). */
void vcr_hwcaps_set_range(struct vcr_hwcaps *hw, const vcr_mon_range *r);

#endif /* VCR_EDID_H */
