/* test_icd_gamma_report.c
 *
 * Guards the 0.1.77 fix in OUR clean-room ICD (MesaFX fork retro3dfx-gl,
 * src/mesa/drivers/glide/fxwgl.c -> wglGetDeviceGammaRamp3DFX(),
 * fxWglNoteGamma(); fxapi.c -> fxMesaCreateContext()/fxMesaDestroyContext()).
 * Clean-room lane, NOT the vintage SGL ICD.
 *
 * The bug: wglGetDeviceGammaRamp3DFX returned a static table that stayed
 * zero-filled until the game's first wglSetDeviceGammaRamp3DFX, while the DAC
 * really held the ICD's own FX_GAMMA ramp (fxapi.c, default g=1.3). id Tech 3
 * engines save what Get returns as the "original" ramp and load it back when
 * the renderer shuts down. Soldier of Fortune II MP (sof2mp.exe):
 *   0x4de050 WG_CheckHardwareGamma: deviceSupportsGamma =
 *            qwglGetDeviceGammaRamp3DFX(GetDC(desktop), s_oldHardwareGamma)
 *   0x4de330 WG_RestoreGamma (first thing GLimp_Shutdown 0x4dfea0 does):
 *            qwglSetDeviceGammaRamp3DFX(glw_state.hDC, s_oldHardwareGamma)
 * so every vid_restart / quit loaded an ALL-ZERO CLUT (black screen).
 *
 * The contract, mirrored exactly:
 *   - Get reports identity before anything loaded a ramp, never zeros;
 *   - every ramp the ICD loads itself is reported (fxWglNoteGamma: 8-bit Glide
 *     entry i*n/256 -> 16-bit v*0x101);
 *   - Set records what it was handed, as before.
 * The last test pins the ramp SoF2 itself builds (R_SetColorMappings 0x4b80a0
 * + GLimp_SetGamma 0x4de180), i.e. what `vcrctl clut 0 256` must read on the
 * card while the game runs - the on-silicon check for "does the ramp reach
 * the DAC".
 */
#include "munit.h"
#include <math.h>
#include <string.h>

typedef unsigned short u16;
typedef unsigned int   u32;

/* ---- the hardware: chip 0's CLUT bank 0, one grey channel ---------------- */
static u32 dac[256];

/* h5 grLoadGammaTable -> hwcGammaTable: adjustBrightnessAndContrast_m forces
 * entry 0 to 0, then dacAddr/dacData per entry (minihwc.c) */
static void gr_load_gamma(int n, const u32 *r) {
    int i;
    for (i = 0; i < n; i++)
        dac[i] = i == 0 ? 0 : (r[i] & 0xff);
}

/* ---- the ICD -------------------------------------------------------------- */
struct icd { u16 table[3 * 256]; int known; int fixed; };

/* fxWglNoteGamma(), 0.1.77 */
static void note_gamma(struct icd *s, int n, const u32 *r) {
    int i, k;
    if (!s->fixed || n <= 0 || n > 256)
        return;
    for (i = 0; i < 256; i++) {
        k = i * n / 256;
        s->table[i] = s->table[256 + i] = s->table[512 + i] = (u16)((r[k] & 0xff) * 0x101);
    }
    s->known = 1;
}

/* wglGetDeviceGammaRamp3DFX(): 0.1.77 fills identity when nothing is known;
 * 0.1.76 copied the (zero) static table as it was */
static void icd_get(struct icd *s, u16 *out) {
    int i;
    if (s->fixed && !s->known) {
        for (i = 0; i < 256; i++)
            s->table[i] = s->table[256 + i] = s->table[512 + i] = (u16)(i * 0x101);
        s->known = 1;
    }
    memcpy(out, s->table, sizeof s->table);
}

/* wglSetDeviceGammaRamp3DFX(): record, then red[i*inc] >> 8 into Glide */
static void icd_set(struct icd *s, const u16 *in, int n) {
    u32 r[256];
    int i, idx, inc = 256 / n;
    memcpy(s->table, in, sizeof s->table);
    s->known = 1;
    for (i = 0, idx = 0; i < n; i++, idx += inc)
        r[i] = in[idx] >> 8;
    gr_load_gamma(n, r);
}

/* fxMesaCreateContext(): the FX_GAMMA pow(1/g) ramp, default g = 1.3 */
static void icd_create(struct icd *s, int n, double g) {
    u32 r[256];
    int i, idx, inc = 256 / n;
    for (i = 0, idx = 0; i < n; i++, idx += inc) {
        int v = (int)(pow((double)idx / 255.0, 1.0 / g) * 255.0 + 0.5);
        r[i] = (u32)(v > 255 ? 255 : v < 0 ? 0 : v);
    }
    gr_load_gamma(n, r);
    note_gamma(s, n, r);
}

/* fxMesaDestroyContext(): identity restore when a default ramp was loaded */
static void icd_destroy(struct icd *s, int n) {
    u32 r[256];
    int i, idx, inc = 256 / n;
    for (i = 0, idx = 0; i < n; i++, idx += inc)
        r[i] = (u32)idx;
    gr_load_gamma(n, r);
    note_gamma(s, n, r);
}

/* ---- the game: sof2mp.exe ------------------------------------------------- */
/* R_SetColorMappings 0x4b80a0 (gamma loop 0x4b81a0) + GLimp_SetGamma 0x4de180
 * (the XP "W2K clamp" 0x4de250 and the monotonic pass 0x4de2b1) */
static void sof2_ramp(int overbright, double gamma, u16 *t) {
    int i, j, c;
    for (i = 0; i < 256; i++) {
        int inf = gamma == 1.0 ? i : (int)(255.0 * pow(i / 255.0, 1.0 / gamma) + 0.5);
        inf <<= overbright;
        if (inf < 0) inf = 0;
        if (inf > 255) inf = 255;
        for (c = 0; c < 3; c++)
            t[c * 256 + i] = (u16)((inf << 8) | inf);
    }
    for (c = 0; c < 3; c++) {
        u16 *ch = t + c * 256;
        for (j = 0; j < 128; j++)
            if (ch[j] > (u16)((128 + j) << 8))
                ch[j] = (u16)((128 + j) << 8);
        if (ch[127] > 0xfe00)
            ch[127] = 0xfe00;
        for (j = 1; j < 256; j++)
            if (ch[j] < ch[j - 1])
                ch[j] = ch[j - 1];
    }
}

/* ---- tests ---------------------------------------------------------------- */

TEST(get_before_anything_loaded_is_identity_not_zero) {
    struct icd s; u16 got[768];
    memset(&s, 0, sizeof s); s.fixed = 1;
    icd_get(&s, got);
    CHECK_EQ_U(got[1], 0x0101);
    CHECK_EQ_U(got[128], 0x8080);
    CHECK_EQ_U(got[512 + 255], 0xffff);

    memset(&s, 0, sizeof s);                      /* 0.1.76 */
    icd_get(&s, got);
    CHECK_EQ_U(got[128], 0);                      /* the bug */
}

TEST(get_reports_the_ramp_the_icd_loaded_at_context_create) {
    struct icd s; u16 got[768];
    memset(&s, 0, sizeof s); s.fixed = 1;
    icd_create(&s, 256, 1.3);
    icd_get(&s, got);
    CHECK_EQ_U(got[64] >> 8, dac[64]);            /* what the DAC holds */
    CHECK_EQ_U(dac[64], 0x58);                    /* pow(64/255, 1/1.3) */
}

TEST(sof2_vid_restart_restores_a_real_ramp_not_black) {
    int fixed;
    for (fixed = 0; fixed <= 1; fixed++) {
        struct icd s; u16 saved[768], ramp[768];
        int i, lit = 0;
        memset(&s, 0, sizeof s); s.fixed = fixed;
        icd_create(&s, 256, 1.3);                 /* wglCreateContext */
        icd_get(&s, saved);                       /* WG_CheckHardwareGamma */
        sof2_ramp(1, 1.0, ramp);
        icd_set(&s, ramp, 256);                   /* R_SetColorMappings */
        icd_set(&s, saved, 256);                  /* vid_restart: WG_RestoreGamma */
        for (i = 1; i < 256; i++)
            lit += dac[i] != 0;
        if (fixed) {
            CHECK_EQ_U(lit, 255);
            CHECK_EQ_U(dac[64], 0x58);            /* back to the pre-game ramp */
        } else {
            CHECK_EQ_U(lit, 0);                   /* 0.1.76: an all-zero CLUT */
        }
        icd_destroy(&s, 256);                     /* wglDeleteContext */
        CHECK_EQ_U(dac[64], 64);                  /* identity either way */
        icd_create(&s, 256, 1.3);                 /* the restarted renderer */
        icd_get(&s, saved);
        CHECK_EQ_U(saved[64] >> 8, fixed ? 0x58 : 0);
    }
}

TEST(note_and_set_agree_for_short_glide_tables) {
    /* a 32-entry table (Voodoo 1/2 Glide): Set samples in[i*8], Note expands
     * entry k to 256-entries i with i*32/256 == k - Set(Get()) is lossless */
    struct icd s; u32 r[32]; u16 got[768]; u32 before[32];
    int i;
    memset(&s, 0, sizeof s); s.fixed = 1;
    for (i = 0; i < 32; i++) r[i] = (u32)(i * 8 + 3);
    gr_load_gamma(32, r);
    note_gamma(&s, 32, r);
    memcpy(before, dac, sizeof before);
    icd_get(&s, got);
    icd_set(&s, got, 32);
    for (i = 0; i < 32; i++)
        CHECK_EQ_U(dac[i], before[i]);
    CHECK_EQ_U(got[8 * 5 + 7] >> 8, r[5]);       /* 256-entry i=47 -> k=5 */
}

TEST(what_vcrctl_clut_reads_while_sof2_runs) {
    /* the on-card check: overbright 1 doubles, overbright 0 is identity,
     * and an untouched ICD default (the game's Set never landed) is 1.3 */
    struct icd s; u16 ramp[768];
    memset(&s, 0, sizeof s); s.fixed = 1;
    sof2_ramp(1, 1.0, ramp);
    icd_set(&s, ramp, 256);
    CHECK_EQ_U(dac[0], 0);
    CHECK_EQ_U(dac[64], 0x80);
    CHECK_EQ_U(dac[127], 0xfe);                   /* the XP clamp */
    CHECK_EQ_U(dac[128], 0xff);
    CHECK_EQ_U(dac[255], 0xff);
    sof2_ramp(0, 1.0, ramp);
    icd_set(&s, ramp, 256);
    CHECK_EQ_U(dac[64], 0x40);
    icd_create(&s, 256, 1.3);
    CHECK_EQ_U(dac[64], 0x58);
}

MUNIT_MAIN("MesaFX ICD reports the real DAC ramp to WGL_3DFX_gamma_control (fix 0.1.77)", {
    RUN(get_before_anything_loaded_is_identity_not_zero);
    RUN(get_reports_the_ramp_the_icd_loaded_at_context_create);
    RUN(sof2_vid_restart_restores_a_real_ramp_not_black);
    RUN(note_and_set_agree_for_short_glide_tables);
    RUN(what_vcrctl_clut_reads_while_sof2_runs);
})
