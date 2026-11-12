/* Re-run the SHIPPED decision (agent/shared/gameres.h, the port of FLEETRES.EXE)
 * against each box's own GAMERES mode list, with the -cap each launcher passes,
 * and report the resolution every launcher asks for plus gr_best_hz there.
 * Input lines:  box ok native_w native_h native_hz digital vmax hcm vcm reg_w reg_h reg_hz  WxH@hz ... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "/home/voidsstr/development/retro-agent/agent/shared/gameres.h"

static void run(const char *box, gr_panel_t *p, gr_modes_t *l, int rw, int rh, int rhz,
                const char *label, int capw, int caph)
{
    gr_target_t t;
    gr_decide(p, l, rw, rh, rhz, 32, capw, caph, &t);
    printf("%-6s %-26s cap=%4dx%-4d  FR_W/H=%4dx%-4d(best %3d Hz)  FR_W43/H43=%4dx%-4d(best %3d Hz)  Q2MODE=%d (%dx%d best %d Hz)  FR_HZ=%d DESKHZ=%d\n",
           box, label, capw, caph, t.w, t.h, gr_best_hz(l, t.w, t.h), t.w43, t.h43,
           gr_best_hz(l, t.w43, t.h43), t.q2mode, gr_q2tab[t.q2mode].w, gr_q2tab[t.q2mode].h,
           gr_best_hz(l, gr_q2tab[t.q2mode].w, gr_q2tab[t.q2mode].h), t.fr_hz, t.desk_hz);
}

int main(void)
{
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char box[32]; gr_panel_t p; gr_modes_t l; int rw, rh, rhz, n = 0;
        char *tok;
        memset(&p, 0, sizeof p); gr_modes_reset(&l);
        if (line[0] == '#' || line[0] == '\n') continue;
        tok = strtok(line, " \t\n"); strcpy(box, tok);
        p.ok = atoi(strtok(NULL, " \t\n")); p.native_w = atoi(strtok(NULL, " \t\n"));
        p.native_h = atoi(strtok(NULL, " \t\n")); p.native_hz = atoi(strtok(NULL, " \t\n"));
        p.digital = atoi(strtok(NULL, " \t\n")); p.vmax = atoi(strtok(NULL, " \t\n"));
        p.hcm = atoi(strtok(NULL, " \t\n")); p.vcm = atoi(strtok(NULL, " \t\n"));
        rw = atoi(strtok(NULL, " \t\n")); rh = atoi(strtok(NULL, " \t\n")); rhz = atoi(strtok(NULL, " \t\n"));
        /* The listed rates are ALREADY gr_best_hz output (EDID-capped at insert
         * on the box), so they are re-added with no further cap. */
        l.hz_cap = 0;
        while ((tok = strtok(NULL, " \t\n")) != NULL) {
            int w, h, hz;
            if (sscanf(tok, "%dx%d@%d", &w, &h, &hz) == 3) { gr_modes_add(&l, w, h, hz); n++; }
        }
        /* gameres.c also adds the live and persisted modes */
        gr_modes_add(&l, rw, rh, rhz);
        run(box, &p, &l, rw, rh, rhz, "no cap (Q2/SiN/SoF)", 0, 0);
        run(box, &p, &l, rw, rh, rhz, "Quake1 -cap 1280 960", 1280, 960);
        run(box, &p, &l, rw, rh, rhz, "HexenII -cap 1024 768", 1024, 768);
    }
    return 0;
}
