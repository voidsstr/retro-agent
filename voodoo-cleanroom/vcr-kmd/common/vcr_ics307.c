/*
 * vcr_ics307.c - see include/vcr_ics307.h. Integer-only (kernel code).
 */
#include "../include/vcr_ics307.h"

int vcr_ics307_accurate(const vcr_ics307 *c)
{
    vcr_u32 d = c->actual_hz > c->target_hz ? c->actual_hz - c->target_hz
                                             : c->target_hz - c->actual_hz;
    return c->target_hz && (unsigned long long)d * 200u <= c->target_hz;
}

vcr_u32 vcr_ics307_target_from_pll(vcr_u32 pll)
{
    /* Glide computes 14318180 * n / m / 2^k and then >> 2, in that order */
    vcr_u32 n = ((pll >> 8) & 0xff) + 2;
    vcr_u32 m = ((pll >> 2) & 0x3f) + 2;
    vcr_u32 k = pll & 3;
    unsigned long long f = (unsigned long long)VCR_ICS307_FREF_HZ * n / m;
    return (vcr_u32)((f >> k) >> 2);
}

int vcr_ics307_calc(vcr_u32 target, vcr_ics307 *o)
{
    static const vcr_u8 od_list[8] = { 2, 3, 4, 5, 6, 7, 8, 10 };
    static const vcr_u8 s_of_od[11] = { 15, 15, 1, 6, 3, 4, 7, 5, 2, 15, 0 };
    /* errors are compared in 1/2^20 Hz units to decide exactly as the
     * double-precision search does, including its first-found tie rule */
    unsigned long long best = ~0ull;
    vcr_u32 i, rdw, vdw, found = 0;

    for (i = 0; i < 8; i++) {
        vcr_u32 od = od_list[i];
        for (rdw = 1; rdw < 128; rdw++) {
            unsigned long long den = (unsigned long long)(rdw + 2) * od;
            /* Fref / (RDW + 2) > 200 kHz */
            if (VCR_ICS307_FREF_HZ / (rdw + 2) <= 200000u)
                continue;
            for (vdw = 4; vdw < 512; vdw++) {
                unsigned long long num = 2ull * VCR_ICS307_FREF_HZ * (vdw + 8);
                /* clk in 2^-20 Hz: num * 2^20 / den */
                unsigned long long clk = (num << 20) / den;
                unsigned long long tgt = (unsigned long long)target << 20;
                unsigned long long err = clk > tgt ? clk - tgt : tgt - clk;
                unsigned long long vco;
                if (err >= best)
                    continue;
                /* 55 MHz < VCO < 400 MHz */
                vco = num / (rdw + 2);
                if (vco <= 55000000ull || vco >= 400000000ull)
                    continue;
                best = err;
                found = 1;
                o->vdw = vdw;
                o->rdw = rdw;
                o->od = od;
                o->actual_hz = (vcr_u32)(num / den);
            }
        }
    }
    o->target_hz = target;
    if (!found)
        return 0;
    o->s = s_of_od[o->od];
    /* C1 C0 = 0, TTL = 1, F1 F0 = 0 (as Glide sends it) */
    o->byte[0] = (vcr_u8)((1u << 5) | (o->s & 7));
    o->byte[1] = (vcr_u8)((o->vdw & 0x1fe) >> 1);
    o->byte[2] = (vcr_u8)(((o->vdw & 1) << 7) | (o->rdw & 0x7f));
    return 1;
}

/* ---- the wire (see vcr_ics307.h) ------------------------------------------ */

static void gpio_bit(const vcr_gpio_io *io, vcr_u32 mask, vcr_u32 on)
{
    vcr_u32 v = io->rd(io->ctx) & ~mask;
    io->wr(io->ctx, on ? v | mask : v);
}

static void dly(const vcr_gpio_io *io, vcr_u32 us)
{
    if (io->stall_us)
        io->stall_us(io->ctx, us);
}

vcr_u32 vcr_ics307_send(const vcr_gpio_io *io, const vcr_ics307 *c,
                        vcr_u32 *before, vcr_u32 *after)
{
    vcr_u32 i, b, n, stretch = 0, v = io->rd(io->ctx);

    if (before)
        *before = v;
    io->wr(io->ctx, (v & VCR_ICS307_KEEP) | VCR_ICS307_OE_ALL);
    for (i = 0; i < 3; i++)
        for (b = 0; b < 8; b++) {
            dly(io, VCR_ICS307_DTIME_US);
            gpio_bit(io, VCR_ICS307_DATA_OUT, (c->byte[i] >> (7 - b)) & 1);
            dly(io, VCR_ICS307_DTIME_US);
            gpio_bit(io, VCR_ICS307_SCLK_OUT, 1);
            dly(io, VCR_ICS307_DTIME_US);
            for (n = 0; !(io->rd(io->ctx) & VCR_ICS307_SCLK_IN) && n < VCR_ICS307_STRETCH_MAX; n++)
                dly(io, VCR_ICS307_DTIME_US);
            stretch += n;
            gpio_bit(io, VCR_ICS307_SCLK_OUT, 0);
        }
    dly(io, VCR_ICS307_DTIME_US);
    gpio_bit(io, VCR_ICS307_STRB_OUT, 1);
    dly(io, 2 * VCR_ICS307_DTIME_US);
    gpio_bit(io, VCR_ICS307_STRB_OUT, 0);
    dly(io, VCR_ICS307_DTIME_US);
    if (after)
        *after = io->rd(io->ctx);
    return stretch;
}
