/*
 * vcrmp_clock.c - program the Voodoo 5 6000's external clock (ICS307-style
 * synthesizer on the HiNT bridge's GPIO) for analog 4-chip SLI.
 *
 * The divider search and the wire sequence are pure code in
 * common/vcr_ics307.c (host-tested: tests/native/test_vcr_kmd_ics307.c);
 * this file supplies the bridge's config dword 0xC4 through the HAL - the
 * bridge (3388:0021, bus 2 dev 0 on .124) is an ordinary function 0 the HAL
 * does see, unlike the slave chips below it.
 */
#include "vcrmp.h"
#include "../include/vcr_ics307.h"
#include "../include/vcr_clock.h"

static vcr_u32 k_gpio_rd(void *ctx)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    ULONG v = 0xffffffffu;
    HalGetBusDataByOffset(VCR_PCIConfiguration, x->bridge_bus, x->bridge_slot, &v,
                          VCR_ICS307_GPIO_REG, 4);
    return v;
}

static void k_gpio_wr(void *ctx, vcr_u32 v)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    ULONG w = v;
    HalSetBusDataByOffset(VCR_PCIConfiguration, x->bridge_bus, x->bridge_slot, &w,
                          VCR_ICS307_GPIO_REG, 4);
}

static void k_gpio_stall(void *ctx, vcr_u32 us)
{
    (void)ctx;
    VideoPortStallExecution(us);
}

/* Returns the programmed frequency in Hz, 0 if it was not programmed. */
ULONG VcrClock6k(VCR_EXT *x, ULONG pllctrl0)
{
    vcr_gpio_io io;
    vcr_ics307 c;
    vcr_u32 target, before = 0, after = 0, stretch;

    if (!x->bridge_found) {
        VLOG(VCR_LV_ERROR, VCR_EV_CLOCK_6K, 0, 0, 0, 0,
             "no HiNT bridge above the chips - cannot program the 6000 clock");
        return 0;
    }
    target = vcr_ics307_target_from_pll(pllctrl0);
    if (!vcr_ics307_calc(target, &c)) {
        VLOG(VCR_LV_ERROR, VCR_EV_CLOCK_6K, target, 0, 0, pllctrl0,
             "no synthesizer setting for %u Hz", target);
        return 0;
    }
    /* Below ~5.5 MHz (the doublescan low-resolution modes) the part cannot
     * follow. Glide programs the nearest it can make regardless, and so do
     * we - the vendor's behaviour - but say so. */
    if (!vcr_ics307_accurate(&c))
        VLOG(VCR_LV_WARN, VCR_EV_CLOCK_6K, target, c.actual_hz, 0, pllctrl0,
             "clock %u Hz is below what the synthesizer can make - nearest %u Hz",
             target, c.actual_hz);
    io.ctx = x;
    io.rd = k_gpio_rd;
    io.wr = k_gpio_wr;
    io.stall_us = k_gpio_stall;
    stretch = vcr_ics307_send(&io, &c, &before, &after);
    VLOG(stretch ? VCR_LV_WARN : VCR_LV_INFO, VCR_EV_CLOCK_6K, target, c.actual_hz,
         (c.byte[0] << 16) | (c.byte[1] << 8) | c.byte[2], after,
         "6000 clock V%u R%u OD%u, gpio %08x -> %08x, stretch %u", c.vdw, c.rdw,
         c.od, before, after, stretch);
    return c.actual_hz;
}

/* ---- the core clock, LIVE (IOCTL_VCR_CLOCK) -----------------------------------------
 * include/vcr_clock.h has the rule and the reasons; this is the hardware half.
 *
 * ONLY THE MASTER (chip 0) IS WRITTEN. The slaves are clocked by the SLI
 * enable: every time a Glide game turns SLI on, vcrmp_sli.c sli_enable ->
 * init_slave copies the master's pllCtrl1 into every slave together with its
 * DRAM timings (the vendor's InitializeSlaveChipsInitRegs and Glide's
 * initSlave do the same). Measured on .124 at boot (2026-09-29): chips 1-3
 * sat at their reset word 0x0C01 (50.1 MHz) with their reset DRAM timings,
 * idle - nothing initializes them until a game asks for SLI. A PLL written
 * under an uninitialized slave gains nothing, and a SET is refused while a
 * Glide program holds the board (display/vcrdd_escape.c), so no slave ever
 * renders at another clock than the master's: the next game carries the new
 * clock to all four.
 *
 * A step waits - at PASSIVE_LEVEL - until every chip reports idle, then raises
 * to DISPATCH_LEVEL for the last idle check and the write, so on a 1-CPU box
 * no thread (a game queueing its next frame) runs between the check and the
 * write; the PLL is given a millisecond to relock and is read back. Anything
 * unexpected stops the ramp where it is and says so - the board is never left
 * at a clock nobody asked for without a VCR_CLOCK_R_* explaining it.
 * Diag\CoreClock = 0 refuses every write. */

#define VCR_CLK_IDLE_LOOPS  200000      /* status reads per chip per wait */
#define VCR_CLK_TRIES       5           /* a busy board is waited for again this often */
#define VCR_CLK_SETTLE_US   1000        /* after each step, before the read-back */

void VcrCoreClockCapture(VCR_EXT *x, const char *when)
{
    ULONG c;
    if (x->backend != VCR_HW_VOODOO || x->core_changed)
        return;                         /* after a SET the master's word is ours, not the VBIOS's */
    for (c = 0; c < x->nchips && c < VCR_MAX_CHIPS; c++) {
        ULONG w;
        if (x->core_boot_pll[c] || !x->chip[c].regs)
            continue;
        w = VcrRd(x, c, VCR_R_PLLCTRL1);
        if (w == 0xffffffffu || !w)
            continue;                   /* the bus answered nothing: try again later */
        x->core_boot_pll[c] = w;
        VLOG(VCR_LV_INFO, VCR_EV_CORE_CLOCK, 0, c, w, vcr_clock_pll_khz(w),
             "chip %u core clock at %s: pllCtrl1 %08x = %u kHz%s", c, when, w,
             vcr_clock_pll_khz(w), c ? " (a slave: set from the master at each SLI enable)" : "");
    }
}

/* every chip idle, waiting up to VCR_CLK_IDLE_LOOPS reads each */
static ULONG clk_all_idle(VCR_EXT *x)
{
    ULONG c;
    for (c = 0; c < x->nchips; c++)
        if (!VcrHwWaitIdle(x, c, VCR_CLK_IDLE_LOOPS))
            return 0;
    return 1;
}

/* every chip idle NOW: three reads each (the vendor's rule), no waiting and
 * no logging - this runs at DISPATCH_LEVEL */
static ULONG clk_all_idle_now(VCR_EXT *x)
{
    ULONG c, i;
    for (c = 0; c < x->nchips; c++)
        for (i = 0; i < 3; i++) {
            ULONG st = VcrRd(x, c, VCR_R_STATUS);
            if (st == 0xffffffffu || (st & VCR_STATUS_BUSY))
                return 0;
        }
    return 1;
}

/* one step: the master's pllCtrl1 = word, written while the board is idle */
static ULONG clk_write_step(VCR_EXT *x, ULONG word, vcr_clock_res *rs)
{
    ULONG tries, us;
    UCHAR old;

    for (tries = 0; tries < VCR_CLK_TRIES; tries++) {
        if (!clk_all_idle(x)) {
            rs->idle_retries++;
            VLOG(VCR_LV_WARN, VCR_EV_CORE_CLOCK, 4, tries, word, vcr_clock_pll_khz(word),
                 "a chip is busy - waiting again (%u)", tries);
            continue;
        }
        old = KfRaiseIrql(VCR_DISPATCH_LEVEL);
        if (!clk_all_idle_now(x)) {
            KfLowerIrql(old);
            rs->idle_retries++;
            continue;
        }
        VcrWr(x, 0, VCR_R_PLLCTRL1, word);
        KfLowerIrql(old);
        for (us = 0; us < VCR_CLK_SETTLE_US; us += 50)
            VideoPortStallExecution(50);
        return VcrRd(x, 0, VCR_R_PLLCTRL1) == word ? VCR_CLOCK_R_OK : VCR_CLOCK_R_READBACK;
    }
    return VCR_CLOCK_R_BUSY;
}

static void clk_fill(VCR_EXT *x, vcr_clock_res *rs)
{
    ULONG c;
    for (c = 0; c < x->nchips && c < VCR_MAX_CHIPS; c++) {
        rs->boot_pll[c] = x->core_boot_pll[c];
        rs->cur_pll[c] = x->chip[c].regs ? VcrRd(x, c, VCR_R_PLLCTRL1) : 0;
    }
    rs->boot_khz = vcr_clock_pll_khz(rs->boot_pll[0]);
    rs->cur_khz = vcr_clock_pll_khz(rs->cur_pll[0]);
}

VP_STATUS VcrCoreClock(VCR_EXT *x, const vcr_clock_req *rq, ULONG rqlen, vcr_clock_res *rs)
{
    vcr_u32 plan[VCR_CLOCK_MAX_STEPS], from, target;
    int nsteps, i;
    ULONG r = VCR_CLOCK_R_OK;

    VideoPortZeroMemory(rs, sizeof *rs);
    rs->size = sizeof *rs;
    rs->min_khz = VCR_CLOCK_MIN_KHZ;
    rs->max_khz = VCR_CLOCK_MAX_KHZ;
    rs->step_khz = VCR_CLOCK_STEP_KHZ;
    if (rqlen < sizeof *rq || rq->op > VCR_CLOCK_OP_RESTORE) {
        rs->result = VCR_CLOCK_R_BAD_REQUEST;
        return NO_ERROR;
    }
    if (x->backend != VCR_HW_VOODOO) {
        rs->result = VCR_CLOCK_R_NOT_VOODOO;
        return NO_ERROR;
    }
    rs->nchips = x->nchips;
    VcrCoreClockCapture(x, "first request");
    clk_fill(x, rs);
    if (rq->op == VCR_CLOCK_OP_GET)
        return NO_ERROR;

    if (!VcrDiagGet(L"CoreClock", 1))
        r = VCR_CLOCK_R_DISABLED;
    else if (!x->chip[0].regs || !x->core_boot_pll[0])
        r = VCR_CLOCK_R_CHIPS;          /* the master unmapped, or its VBIOS word never read */
    target = rq->op == VCR_CLOCK_OP_RESTORE ? rs->boot_khz : rq->target_khz;
    rs->target_khz = target;
    if (r == VCR_CLOCK_R_OK && rq->op == VCR_CLOCK_OP_SET && !vcr_clock_in_range(target))
        r = VCR_CLOCK_R_RANGE;
    from = rs->cur_khz;
    nsteps = r == VCR_CLOCK_R_OK ? vcr_clock_plan(from, target, plan, VCR_CLOCK_MAX_STEPS) : 0;
    if (nsteps < 0)
        r = VCR_CLOCK_R_RANGE;
    if (r != VCR_CLOCK_R_OK) {
        rs->result = r;
        VLOG(VCR_LV_WARN, VCR_EV_CORE_CLOCK, 3, r, rs->cur_pll[0], target,
             "core clock %s to %u kHz refused: %u", rq->op == VCR_CLOCK_OP_RESTORE ?
             "restore" : "set", target, r);
        return NO_ERROR;
    }

    for (i = 0; i < nsteps; i++) {
        /* RESTORE ends on the VBIOS's own word, not a recomputed one */
        ULONG word = i == nsteps - 1 && rq->op == VCR_CLOCK_OP_RESTORE ? x->core_boot_pll[0]
                                                                        : vcr_clock_pll(plan[i], NULL);
        if (!word) {
            r = VCR_CLOCK_R_RANGE;      /* a step outside the range: never write it */
            break;
        }
        r = clk_write_step(x, word, rs);
        VLOG(r == VCR_CLOCK_R_OK ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_CORE_CLOCK, 1, i, word,
             vcr_clock_pll_khz(word), "core clock step %u: master pllCtrl1 %08x = %u kHz -> %u",
             i, word, vcr_clock_pll_khz(word), r);
        if (r != VCR_CLOCK_R_OK)
            break;
        x->core_changed = 1;
        rs->steps++;
    }
    clk_fill(x, rs);
    rs->result = r;
    VLOG(r == VCR_CLOCK_R_OK ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_CORE_CLOCK, 2, r,
         rs->cur_pll[0], rs->cur_khz, "core clock %s: %u -> %u kHz in %u steps (%u idle retries) "
         "-> %u; the slaves take it at the next SLI enable", rq->op == VCR_CLOCK_OP_RESTORE ?
         "restored" : "set", from, rs->cur_khz, rs->steps, rs->idle_retries, r);
    return NO_ERROR;
}
