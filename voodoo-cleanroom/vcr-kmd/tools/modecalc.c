/*
 * modecalc.c - host build of the miniport's own mode math (common/vcr_modes.c)
 * printing what it would program for every timing, as JSON lines.
 * tools/golden_compare.py compiles and runs it: the comparison with the vendor
 * captures runs the exact code the driver runs.
 */
#include <stdio.h>
#include "../common/vcr_modes.c"

int main(void)
{
    vcr_hwcaps h = { VCR_DEV_VSA100, 350000, 262000, 261, 32u << 20, 0x110000, 0 };
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
            printf("\"}\n");
        }
    return 0;
}
