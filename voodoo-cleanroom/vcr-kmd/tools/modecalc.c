/*
 * modecalc.c - host build of the miniport's own mode math (common/vcr_modes.c)
 * printing what it would program for every timing, as JSON lines.
 * tools/golden_compare.py compiles and runs it: the comparison with the vendor
 * captures runs the exact code the driver runs.
 *
 * It also prints each mode's totals and the scan rates it PROGRAMS (from the
 * PLL's actual clock, not the table's nominal one): the live tools
 * (mode_sweep.py and the others that switch) check every mode against the
 * monitor's EDID ranges on the host before asking the box for it. The
 * 2026-09-26 sweep on .124 drove a 1998 CRT through every mode; a mode the
 * host cannot prove in range is never sent.
 */
#include <stdio.h>
#include "../common/vcr_modes.c"

int main(void)
{
    /* no monitor ranges (0 = unknown): every timing is computed, and the
     * caller applies the box's EDID ranges itself - with no slack at the top */
    vcr_hwcaps h = { VCR_DEV_VSA100, 350000, 262000, 261, 32u << 20, 0x110000, 0,
                     0, 0, 0, 0, 0 };
    unsigned i, b, k;
    static const unsigned depths[3] = { 8, 16, 32 };
    for (i = 0; i < vcr_ntimings; i++)
        for (b = 0; b < 3; b++) {
            vcr_modeset m;
            const vcr_timing *t = &vcr_timings[i];
            if (vcr_mode_compute(&h, t, depths[b], &m))
                continue;
            printf("{\"mode\":\"%ux%ux%u@%u\",\"pll\":\"%04x\",\"khz\":%u,\"dacmode\":%u,"
                   "\"vidproccfg\":\"%08x\",\"screensize\":\"%08x\",\"misc\":\"%02x\",\"crtc\":\"",
                   t->w, t->h, depths[b], t->refresh, m.pllctrl0, m.pix_khz_actual, m.dacmode,
                   m.vidproccfg, m.vidscreensize, m.misc);
            for (k = 0; k < 25; k++)
                printf("%02x", m.crtc[k]);
            printf("\",\"crtc1a\":\"%02x\",\"crtc1b\":\"%02x\",\"seq\":\"", m.crtc_ext[0], m.crtc_ext[1]);
            for (k = 0; k < 5; k++)
                printf("%02x", m.seq[k]);
            /* totals in pixels / PHYSICAL lines (a doublescan row is two);
             * hfreq_hz and refresh_mhz are what the chip will scan out */
            printf("\",\"htotal\":%u,\"vtotal\":%u,\"hkhz_x1000\":%u,\"vhz_x1000\":%u}\n",
                   (unsigned)(t->w + t->hfp + t->hsync + t->hbp),
                   (unsigned)(t->h * ((t->flags & VCR_T_DBLSCAN) ? 2 : 1) + t->vfp + t->vsync +
                              t->vbp),
                   (unsigned)m.hfreq_hz, (unsigned)m.refresh_mhz);
        }
    return 0;
}
