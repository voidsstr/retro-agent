/*
 * vcr_clock.h - the VSA-100 graphics clock, set LIVE (IOCTL_VCR_CLOCK,
 * miniport/vcrmp_clock.c VcrCoreClock, 2026-09-29). Win32-free:
 * tests/native/test_vcr_clock.c compiles it as is.
 *
 * WHAT IS CLOCKED. A VSA-100 has three PLLs - pllCtrl0 (the pixel clock),
 * pllCtrl1 (the graphics core) and pllCtrl2 - and its SDRAM runs from the
 * core clock. Measured on .124 (V5 6000, chip 0, as the VBIOS left it,
 * 2026-09-29): pllCtrl1 0xE721 = N 231 M 8 K 1 = 166.8 MHz (the stock clock),
 * pllCtrl0 0x4005 = 157.5 MHz (the 1280x1024@85 pixel clock), and pllCtrl2
 * 0xBF01 = 691 MHz by the same formula - not a memory clock. The vintage H5
 * miniport programs only pllCtrl1 for Napalm ("pllCtrl2 - we don't need
 * this") and Glide's h4InitPlls takes one grx clock for a Voodoo 4/5. So ONE
 * clock: core and memory move together.
 *
 * WHICH CHIPS. The live write goes to the MASTER. A slave's pllCtrl1 is
 * copied from the master's at every SLI enable (vcrmp_sli.c init_slave, as
 * the vendor's InitializeSlaveChipsInitRegs and Glide's initSlave do) - on
 * .124 at boot chips 1-3 read their reset word 0x0C01 (50.1 MHz) with reset
 * DRAM timings, uninitialized and idle. So a new clock reaches all four
 * chips when the next game turns SLI on, and never mid-game: a change is
 * refused while a Glide program holds the board.
 *
 * THE RULE (vendor guidance, read not copied - clean-room lane): across
 * its whole 51-219 MHz range the vendor's MHz table uses K = 1 (VCO = 2 f,
 * 100-440 MHz);
 * this board's VBIOS picked M = 8 for finer steps (0.72 MHz vs 2.39 MHz at
 * M = 1). vcr_pll_calc() - the PIXEL clock's rule - takes K = 2 below
 * 150 MHz, a 570 MHz VCO the vendor never ran the core at, so the core clock
 * has its own: K = 1, M 1..10, N+2 <= 257, lowest error, the smallest M on a
 * tie.
 *
 * THE LIMITS: 120-219 MHz, refused outside (the panel offers 133-200); 219
 * is the vendor's own ceiling (its check is "< 220"). The floor is not the
 * vendor's 51: the SDRAM refresh interval is counted in clocks (dramInit1
 * bits 9:1, 0x18 here), so a slower clock refreshes less often in real time.
 * The vendor ran that same 0x18 from Banshee's 100 MHz to the Voodoo3 3500's
 * 183 MHz (cinit h3InitSgram, xf86-video-tdfx) - 120 stays inside it.
 *
 * THE RAMP: a change moves in steps of at most VCR_CLOCK_STEP_KHZ, each
 * written while every chip is idle (vcr_clock_plan).
 */
#ifndef VCR_CLOCK_H
#define VCR_CLOCK_H

#include "vcr_types.h"

#define VCR_CLOCK_MIN_KHZ       120000u
#define VCR_CLOCK_MAX_KHZ       219000u
#define VCR_CLOCK_STEP_KHZ      5000u
#define VCR_CLOCK_MAX_STEPS     32
#define VCR_CLOCK_FREF_X100     1431818u        /* 14.31818 MHz in 10 Hz units */

/* the frequency a pllCtrl word gives, in kHz */
static __inline vcr_u32 vcr_clock_pll_khz(vcr_u32 pll)
{
    vcr_u32 n = (pll >> 8) & 0xff;
    vcr_u32 m = (pll >> 2) & 0x3f;
    vcr_u32 k = pll & 0x3;
    return (vcr_u32)(((unsigned long long)VCR_CLOCK_FREF_X100 * (n + 2)) /
                     ((m + 2) * 100u)) >> k;
}

static __inline int vcr_clock_in_range(vcr_u32 khz)
{
    return khz >= VCR_CLOCK_MIN_KHZ && khz <= VCR_CLOCK_MAX_KHZ;
}

/* pllCtrl1 for a core clock (K = 1, M 1..10, lowest error); 0 = out of
 * range. *actual gets the frequency the word really gives. */
static __inline vcr_u32 vcr_clock_pll(vcr_u32 khz, vcr_u32 *actual)
{
    vcr_u32 m, best = 0, best_err = 0xffffffffu;

    if (actual)
        *actual = 0;
    if (!vcr_clock_in_range(khz))
        return 0;
    for (m = 1; m <= 10; m++) {
        unsigned long long num = (unsigned long long)khz * (m + 2) * 2u * 100u;
        vcr_u32 np2 = (vcr_u32)((num + VCR_CLOCK_FREF_X100 / 2) / VCR_CLOCK_FREF_X100);
        vcr_u32 word, f, err;
        if (np2 < 2 || np2 > 257)
            continue;
        word = ((np2 - 2) << 8) | (m << 2) | 1u;
        f = vcr_clock_pll_khz(word);
        err = f > khz ? f - khz : khz - f;
        if (err < best_err) {
            best_err = err;
            best = word;
        }
    }
    if (actual && best)
        *actual = vcr_clock_pll_khz(best);
    return best;
}

/* The ramp from `from` to `to`: the intermediate targets, each at most
 * VCR_CLOCK_STEP_KHZ from the one before, the last exactly `to`. Returns the
 * count written to out[] (0 when from == to), -1 when it would not fit. */
static __inline int vcr_clock_plan(vcr_u32 from, vcr_u32 to, vcr_u32 *out, int max)
{
    int n = 0;
    vcr_u32 cur = from;

    while (cur != to) {
        if (n >= max)
            return -1;
        if (to > cur)
            cur = to - cur > VCR_CLOCK_STEP_KHZ ? cur + VCR_CLOCK_STEP_KHZ : to;
        else
            cur = cur - to > VCR_CLOCK_STEP_KHZ ? cur - VCR_CLOCK_STEP_KHZ : to;
        out[n++] = cur;
    }
    return n;
}

#endif /* VCR_CLOCK_H */
