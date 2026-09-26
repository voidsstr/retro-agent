/*
 * vcr_ics307.h - the Voodoo 5 6000's external clock synthesizer.
 *
 * The 6000 clocks its four VSA-100s for analog SLI from an ICS307-style
 * serially programmable clock wired to GPIO pins of its HiNT HB1 PCI bridge
 * (config register 0xC4). Programming interface, from the part's public
 * datasheet: a 24-bit word, MSB first on DATA with a rising SCLK per bit,
 * latched by a STROBE pulse -
 *
 *   C1 C0 TTL F1 F0 S2 S1 S0 | V8..V1 | V0 R6..R0
 *
 *   CLK = 2 * Fref * (VDW + 8) / ((RDW + 2) * OD),  Fref = 14.31818 MHz
 *   55 MHz < 2 * Fref * (VDW + 8) / (RDW + 2) < 400 MHz   (the VCO)
 *   Fref / (RDW + 2) > 200 kHz
 *   OD is chosen by S2..S0: 2->1 3->6 4->3 5->4 6->7 7->5 8->2 10->0
 *
 * The target and the search order follow Glide GPL's gpio_6k_clock():
 * the master's pixel clock / 4, and the first best triple scanning OD in
 * {2,3,4,5,6,7,8,10}, RDW 1..127, VDW 4..511 - so our choice equals the one
 * Glide (and, measured, the vendor driver) makes. Integer arithmetic only.
 * The bit-level wiring on the bridge is in miniport/vcrmp_clock.c.
 */
#ifndef VCR_ICS307_H
#define VCR_ICS307_H

#include "vcr_types.h"

#define VCR_ICS307_FREF_HZ  14318180u

typedef struct vcr_ics307 {
    vcr_u32 target_hz;
    vcr_u32 actual_hz;
    vcr_u32 vdw, rdw, od, s;
    vcr_u8  byte[3];            /* what goes on the wire, first byte first */
} vcr_ics307;

/* 1 = found a legal setting, 0 = none. A setting is always found for any
 * positive target - like Glide, the nearest the part can make - so check
 * vcr_ics307_accurate() before trusting it: below ~5.5 MHz (VCO 55 MHz / OD
 * 10) the part cannot follow, which is every doublescan low-resolution mode
 * (320x240, 400x300: target ~3-5 MHz). */
int     vcr_ics307_calc(vcr_u32 target_hz, vcr_ics307 *out);
/* The setting is within 0.5 % of its target. */
int     vcr_ics307_accurate(const vcr_ics307 *c);
/* The clock Glide asks for: the master's pllCtrl0 frequency / 4, in Hz. */
vcr_u32 vcr_ics307_target_from_pll(vcr_u32 pllctrl0);

/*
 * The wire, on the V5 6000: GPIO pins of the HiNT HB1 bridge, config dword
 * 0xC4. Each signal has an input bit, an output-enable bit one above it and
 * an output-value bit two above it (as Glide GPL's gpio.c drives them):
 *      DATA in 8 / oe 9 / out 10      STRB in 12 / oe 13 / out 14
 *      SCLK in 16 / oe 17 / out 18    HIV  in 20 / oe 21 / out 22
 * Measured on .124: 0x00111101 idle (outputs off, inputs pulled high), and
 * 0x00222201 after the vendor driver - and ours - programmed it.
 */
#define VCR_ICS307_GPIO_REG     0xc4
#define VCR_ICS307_DATA_OUT     (1u << 10)
#define VCR_ICS307_STRB_OUT     (1u << 14)
#define VCR_ICS307_SCLK_IN      (1u << 16)
#define VCR_ICS307_SCLK_OUT     (1u << 18)
#define VCR_ICS307_OE_ALL       0x00222200u     /* DATA, STRB, SCLK, HIV driven */
#define VCR_ICS307_KEEP         0xff0000ffu     /* bits the sequence never touches */
#define VCR_ICS307_DTIME_US     5
#define VCR_ICS307_STRETCH_MAX  1000            /* polls of DTIME: 5 ms per bit, bounded */

typedef struct vcr_gpio_io {
    void    *ctx;
    vcr_u32 (*rd)(void *ctx);                   /* the GPIO dword */
    void    (*wr)(void *ctx, vcr_u32 v);
    void    (*stall_us)(void *ctx, vcr_u32 us);
} vcr_gpio_io;

/* Shift c->byte[0..2] out MSB first (DATA, then a rising SCLK honouring
 * clock stretching, bounded; SCLK low), then pulse STRB to latch it. Returns
 * the clock-stretch polls spent (0 = the part never held SCLK low); *before
 * and *after receive the GPIO dword around the sequence. */
vcr_u32 vcr_ics307_send(const vcr_gpio_io *io, const vcr_ics307 *c,
                        vcr_u32 *before, vcr_u32 *after);

#endif /* VCR_ICS307_H */
