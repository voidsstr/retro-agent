#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "jklogic.h"
/* stdin: "filter F" then lines "pass w h pitch pf bits"; prints the sorted list like emu_modelist.py */
int main(void) {
    static jk_dev d; char cmd[16]; int f = 0, pass, w, h, pitch; unsigned pf, bits, i;
    memset(&d, 0, sizeof d); d.target_seen_at = -1;
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "filter")) { scanf("%d", &f); continue; }
        pass = atoi(cmd); scanf("%d %d %d %x %u", &w, &h, &pitch, &pf, &bits);
        jk_mode_offer(&d, pass, w, h, pitch, pf, bits, 1920, 1080, f);
    }
    jk_sort_modes(&d);
    for (i = 0; i < (unsigned)d.nmodes; i++)
        printf("%3u %s %4dx%-4d %2dbpp\n", i, d.modes[i].modex ? "X" : " ", d.modes[i].w, d.modes[i].h, d.modes[i].bpp);
    printf("RESULT stored=%d idx1080x16=%d\n", d.nmodes, jk_find_mode(&d, 1920, 1080));
    return 0;
}
