/*
 * vcr_fog.h - the Voodoo fog unit, as the Direct3D HAL drives it.
 *
 * The fog unit blends toward fogColor by a fog amount it looks up in a 64-entry
 * table indexed by the iterated 1/W (the setup unit's Wfbi). Entry i stands
 * for w = 2^(3 + i/4) / (8 - i%4) (Glide's guFogTableIndexToW: 1 ... 52428.8).
 *
 *   table fog (D3D FOGTABLEMODE LINEAR/EXP/EXP2): the table holds D3D's fog
 *     function of that w, and Wfbi is the vertex's own 1/w (rhw).
 *   vertex fog (FOGTABLEMODE NONE - the fog factor arrives in the specular
 *     colour's alpha): the table is an identity ramp and each vertex's Wfbi is
 *     a SYNTHETIC 1/w chosen to land on its own fog amount. Across a triangle
 *     the chip interpolates 1/w, not the factor, so this is an approximation -
 *     the one a fog unit that only knows W allows.
 *
 * Floating point: compiled with the x87 into the display driver (inside the
 * HAL's FPU bracket) and natively for the host test. No CRT: exp is our own.
 */
#ifndef VCR_FOG_H
#define VCR_FOG_H

#define VCR_FOG_TABLE_ENTRIES   64

#define VCR_FOG_RAMP            0       /* vertex fog: entry i = i/63 */
#define VCR_FOG_EXP             1
#define VCR_FOG_EXP2            2
#define VCR_FOG_LINEAR          3

float vcr_fog_index_w(int i);
float vcr_fog_exp(float x);
/* the table, as fog AMOUNTS 0..255 (255 = all fog colour) */
void vcr_fog_table(int mode, float start, float end, float density,
                   unsigned char out[VCR_FOG_TABLE_ENTRIES]);
/* register n (0..31) of the fogTable block: entries 2n and 2n+1 with deltas */
unsigned vcr_fog_reg(const unsigned char t[VCR_FOG_TABLE_ENTRIES], int n);
/* vertex fog: the 1/w to send for a fog AMOUNT a (0: none, 1: all fog) */
float vcr_fog_ramp_oow(float amount);

#endif /* VCR_FOG_H */
