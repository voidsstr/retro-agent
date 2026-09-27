/*
 * vcr_flip.h - when has the chip taken a DirectDraw flip? The completion rule
 * of the display DLL's Dd_Flip / GetFlipStatus / Lock / Blt, as pure logic:
 * no OS headers, no floating point, no hardware access. The display DLL
 * (display/vcrdd_ddraw.c) is only the glue - the vblank IOCTL and
 * EngQueryPerformanceCounter - and the host test (tests/native/
 * test_vcr_kmd_flip.c) drives this same code over a CRT timeline.
 *
 * THE MODEL. A flip writes vidDesktopStartAddr; the chip latches it at the
 * next vertical sync start, and status[6] reads "in retrace" (bit clear) for
 * the vsync pulse. 86Box, the only bed that can run this driver's Voodoo
 * path, does exactly that on the same scanline: the flag is set at
 * vc == vsyncstart (vid_svga.c svga_poll) and the latch is taken in the
 * vsync callback of that same line (vid_voodoo_banshee.c:
 * desktop_addr = vidDesktopStartAddr). So a flip is DONE once a retrace has
 * been seen that began after the write: active display seen, then the flag.
 *
 * THE ORDER THAT MAKES IT SAFE. The "active display seen" sample is taken
 * AFTER the start-address write (vcr_flip_begin gets the retrace state read
 * after the flip IOCTL). A write racing into a retrace can then only be
 * reported one frame LATE, never early: sampled in the pulse, the flip waits
 * for the next one. Sampled BEFORE the write, a write that lands just after
 * the vsync start (and so misses this latch) would be declared done in the
 * same pulse - a whole frame early (the host test demonstrates it).
 * The one unproven silicon assumption, shared by every retrace-flag rule:
 * status[6] does not assert before the latch. If the flag covered the blank
 * from display end, done could come up to the front porch early.
 *
 * THE DEADLINE. A retrace the polls never see (none lands in a short pulse,
 * or the app does not poll between a flip and its next wait - the D3D
 * pattern) is covered by a timeout counted from the flip:
 *   refresh_mhz == 0 (the default): frame + frame / 8 of the NOMINAL rate,
 *       today's rule, exactly: at QueryPerformanceFrequency 3579545 that is
 *       42112 + 5264 = 47376 ticks at 85 Hz, 59659 + 7457 = 67116 at 60 Hz
 *       (and the 60 Hz rule when the rate is unknown, 0 or 1).
 *   refresh_mhz != 0: frame + frame / 32 of the ACHIEVED rate the miniport
 *       computed from the PLL's achieved clock over the timing TABLE's totals
 *       (vcr_modeset.refresh_mhz, sent in vcr_dd_vblank.refresh_mhz only when
 *       Diag\FlipDeadline = 1). The chip scans the CRTC's totals, which are
 *       the table's except in nine 2X modes, where the halved total is
 *       truncated to whole characters and the line is < 16 px SHORTER: the
 *       chip runs 0.31-0.39% faster than the rate sent, so the deadline is
 *       that much late, never early (pinned for every timing by the host
 *       test, the_rate_sent_is_never_above_the_rate_the_chip_scans).
 *       The latch is at most one REAL frame after the write, and the clock
 *       starts after it, so the rule is never early while the sent rate is at
 *       most 1/32 above the real scan rate, and a D3D-pattern app at 1.05
 *       refreshes a frame flips at ~0.95 of the refresh instead of 0.889.
 *   The trust band (VCR_FLIP_TRUST_SHIFT) is NARROWER than that margin: a sent
 *       rate more than 1/64 away from the NOMINAL rate is not trusted
 *       (nominal rule). Every timing in vcr_modes.c computes within +1.12% /
 *       -0.29% of its nominal rate, so all of them keep the achieved rule. A
 *       band as wide as 1/16 trusted a rate 4-6% above the real one - a PLL
 *       that did not take its value, a formula error - and that completed
 *       flips up to ~3% of a frame EARLY (tests/native/test_vcr_kmd_flip.c,
 *       the_trust_band_is_narrower_than_the_margin). The band
 *       compares with the nominal rate, not the real one, so it does not
 *       bound a model error by itself: never early holds while the real scan
 *       rate is at least 65/66 of the nominal (1 + 1/64 = 65/64, times 32/33).
 *       Nothing measures the real rate here; before Diag\FlipDeadline = 1 on
 *       a box, ddlab's vblank_hz at the target mode must agree with
 *       refresh_mhz / 1000 to well under 1%.
 *
 * vidCurrentLine plays NO part here, by design: open Glide gives the field no
 * semantics, 86Box does not emulate it (reads 0x7ff), and on the VSA-100 it
 * very probably reads 0 through the blank - see GetScanLine for the one place
 * it is reported.
 */
#ifndef VCR_FLIP_H
#define VCR_FLIP_H

#include "vcr_types.h"

typedef long long vcr_ticks;        /* a QueryPerformanceCounter value or span */

/* the achieved rate is trusted within 1/64 of the nominal one - narrower
 * than the 1/32 margin of the deadline it feeds (was 1/16: see THE DEADLINE) */
#define VCR_FLIP_TRUST_SHIFT    6

typedef struct vcr_flip_state {
    vcr_u32   pending;          /* a vsync'd flip the chip may not have latched */
    vcr_u32   seen_active;      /* active display seen since (and after) the write */
    vcr_ticks t0;               /* when the flip was begun (after its sample) */
    vcr_ticks deadline;         /* ticks after t0 at which it counts as done anyway */
    vcr_u32   waiting;          /* a poll found it pending: a wait began at wait_t0 */
    vcr_ticks wait_t0;
    /* counters, per PDEV, until vcr_flip_stats_reset (logged when exclusive
     * mode ends, the mode leaves the screen, DirectDraw is disabled, or the
     * flipping process's DirectDraw object goes away - once per session) */
    vcr_u32   flips;            /* vsync'd flips begun */
    vcr_u32   by_retrace;       /* ... done by a retrace seen after the write */
    vcr_u32   by_deadline;      /* ... done because the deadline passed */
    vcr_u32   superseded;       /* ... still pending when another flip replaced it */
    vcr_u32   polls;            /* vblank reads made for flip completion */
    vcr_ticks max_poll;         /* the longest single vblank read (the IOCTL) */
    vcr_ticks max_wait;         /* the longest a caller was told "still drawing" */
    vcr_u32   carried;          /* flips counted by the last reset: one still in flight */
    vcr_u32   owner;            /* the process of the last vsync'd flip since the reset
                                 * (0: none) - whose session the counters are */
} vcr_flip_state;

/* The achieved rate to use, or 0 for the nominal rule. */
static inline vcr_u32 vcr_flip_trusted_mhz(vcr_u32 refresh_mhz, vcr_u32 nominal_hz)
{
    vcr_u32 nom;
    if (!refresh_mhz || nominal_hz <= 1 || nominal_hz > 1000)
        return 0;
    nom = nominal_hz * 1000u;
    if (refresh_mhz > nom + (nom >> VCR_FLIP_TRUST_SHIFT) ||
        refresh_mhz < nom - (nom >> VCR_FLIP_TRUST_SHIFT))
        return 0;
    return refresh_mhz;
}

/* Ticks after the flip at which it is done even with no retrace seen. */
static inline vcr_ticks vcr_flip_deadline(vcr_ticks qpf, vcr_u32 refresh_mhz, vcr_u32 nominal_hz)
{
    vcr_ticks frame;
    vcr_u32 mhz = vcr_flip_trusted_mhz(refresh_mhz, nominal_hz);
    if (mhz) {
        frame = qpf * 1000 / (vcr_ticks)mhz;
        return frame + frame / 32;
    }
    frame = qpf / (vcr_ticks)(nominal_hz > 1 ? nominal_hz : 60);
    return frame + frame / 8;
}

/* A flip was written. have_sample / in_vblank: the retrace state read AFTER
 * the start-address write; now: the clock read after that. A NOVSYNC flip is
 * never pending: the app asked not to wait, and the chip latches whichever
 * address was written last. */
static inline void vcr_flip_begin(vcr_flip_state *s, int novsync, int have_sample, int in_vblank,
                                  vcr_ticks now, vcr_ticks deadline)
{
    if (s->pending)
        s->superseded++;
    s->pending = novsync ? 0 : 1;
    s->seen_active = have_sample && !in_vblank;
    s->t0 = now;
    s->deadline = deadline;
    s->waiting = 0;
    if (!novsync)
        s->flips++;
}

/* One vblank read took `ticks` (the glue brackets the IOCTL with the clock). */
static inline void vcr_flip_note_poll(vcr_flip_state *s, vcr_ticks ticks)
{
    s->polls++;
    if (ticks > s->max_poll)
        s->max_poll = ticks;
}

/* Has the chip taken it? have_sample / in_vblank: a retrace state read just
 * now; now: the clock read after it. Returns 1 when done. */
static inline int vcr_flip_poll(vcr_flip_state *s, int have_sample, int in_vblank, vcr_ticks now)
{
    if (!s->pending)
        return 1;
    if (have_sample) {
        if (!in_vblank) {
            s->seen_active = 1;
        } else if (s->seen_active) {
            s->pending = 0;
            s->by_retrace++;
        }
    }
    if (s->pending && now - s->t0 > s->deadline) {
        s->pending = 0;
        s->by_deadline++;
    }
    if (s->pending) {
        if (!s->waiting) {
            s->waiting = 1;
            s->wait_t0 = now;
        }
        return 0;
    }
    if (s->waiting && now - s->wait_t0 > s->max_wait)
        s->max_wait = now - s->wait_t0;
    s->waiting = 0;
    return 1;
}

/* Clear the counters. A flip still in flight stays in flight and is counted
 * again, so flips == by_retrace + by_deadline + superseded + pending holds
 * after a reset too. */
static inline void vcr_flip_stats_reset(vcr_flip_state *s)
{
    s->flips = s->carried = s->pending ? 1 : 0;
    s->by_retrace = s->by_deadline = s->superseded = s->polls = 0;
    s->max_poll = s->max_wait = 0;
    s->owner = 0;
}

/* The process that made a vsync'd flip - the glue calls this right after
 * vcr_flip_begin with the calling process's id (a NOVSYNC flip is not
 * counted, so it does not name the session either). */
static inline void vcr_flip_note_owner(vcr_flip_state *s, int novsync, vcr_u32 pid)
{
    if (!novsync)
        s->owner = pid;
}

/* Has anything been counted since the last reset? The counters are logged at
 * more than one point of a PDEV's life (exclusive mode ends, the mode leaves
 * the screen, DirectDraw is disabled) so that a session which changed the
 * display mode is not lost; the flip carried over a reset still pending is
 * not news, and must not make the next point log the same flip again. */
static inline int vcr_flip_stats_any(const vcr_flip_state *s)
{
    return s->flips != s->carried || s->by_retrace || s->by_deadline || s->superseded ||
           s->polls;
}

/* A process's DirectDraw local object is going away (DestroyDDLocal; `pid` is
 * the process it runs in - the releasing process's own, as dxg is expected to
 * call it from that process's release or exit: UNVERIFIED on XP. Were it
 * another context, the pid would never match and the other points would log
 * the session as before). Log the counters now? The END of a session that stayed
 * in the desktop's own mode is seen nowhere else - XP does not call
 * SetExclusiveMode(0) when such a session releases DirectDraw, and no mode
 * leaves the screen - so its counters used to wait for the next mode change
 * and merge with every same-mode session in between (the 86Box bed,
 * 2026-09-27: "flips 661" = 1 carried + a 60-frame run + a 600-frame run).
 * Yes when something is new since the last log (vcr_flip_stats_any: a
 * session is logged once, whichever point comes first) and the flips were
 * this process's - or nobody's (polls of a flip carried over a reset). The
 * teardown of ANOTHER process's DirectDraw object does not cut a live
 * session in two. A second DirectDraw object of the SAME process going away
 * mid-session (a D3D8 enumeration object, say) does: the session is then
 * logged in two parts, never merged with another and never lost. */
static inline int vcr_flip_local_gone_logs(const vcr_flip_state *s, vcr_u32 pid)
{
    return vcr_flip_stats_any(s) && (!s->owner || s->owner == pid);
}

#endif /* VCR_FLIP_H */
