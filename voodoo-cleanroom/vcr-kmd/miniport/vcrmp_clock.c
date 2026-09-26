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
