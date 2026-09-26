/* vcr_fog.c - see include/vcr_fog.h */
#include "../include/vcr_fog.h"

float vcr_fog_index_w(int i)
{
    float p = (float)(1u << (3 + (i >> 2)));
    return p / (float)(8 - (i & 3));
}

/* e^x for x <= 0 (fog only needs that side): 2^(x log2 e) = 2^n * 2^f,
 * n the integer part through repeated halving, 2^f from a short series */
float vcr_fog_exp(float x)
{
    float y, r = 1.0f, t, f;
    int n, k;
    if (x > 0.0f)
        x = 0.0f;
    if (x < -87.0f)
        return 0.0f;
    y = x * 1.44269504f;                    /* log2(e) */
    n = (int)y;                             /* truncates toward 0: y - n in (-1, 0] */
    f = y - (float)n;
    for (k = 0; k < -n; k++)
        r *= 0.5f;
    /* 2^f = e^(f ln2), f in (-1, 0]: 7 terms of the series */
    t = 1.0f;
    y = 1.0f;
    f *= 0.69314718f;
    for (k = 1; k < 8; k++) {
        t *= f / (float)k;
        y += t;
    }
    return r * y;
}

void vcr_fog_table(int mode, float start, float end, float density,
                   unsigned char out[VCR_FOG_TABLE_ENTRIES])
{
    int i;
    for (i = 0; i < VCR_FOG_TABLE_ENTRIES; i++) {
        float w = vcr_fog_index_w(i), f;        /* f: D3D's fog factor, 1 = no fog */
        switch (mode) {
        case VCR_FOG_LINEAR:
            f = end > start ? (end - w) / (end - start) : (w < end ? 1.0f : 0.0f);
            break;
        case VCR_FOG_EXP:
            f = vcr_fog_exp(-density * w);
            break;
        case VCR_FOG_EXP2:
            f = vcr_fog_exp(-(density * w) * (density * w));
            break;
        default:
            f = 1.0f - (float)i / (float)(VCR_FOG_TABLE_ENTRIES - 1);
            break;
        }
        if (f < 0.0f)
            f = 0.0f;
        if (f > 1.0f)
            f = 1.0f;
        out[i] = (unsigned char)((1.0f - f) * 255.0f + 0.5f);
    }
}

/* two entries per register, each with the delta to the next in .2 format
 * (Glide grFogTable: delta = (next - cur) << 2, the last entry's delta 0) */
unsigned vcr_fog_reg(const unsigned char t[VCR_FOG_TABLE_ENTRIES], int n)
{
    int i0 = 2 * n, i1 = i0 + 1;
    unsigned e0 = t[i0], e1 = t[i1];
    unsigned n0 = e1, n1 = i1 + 1 < VCR_FOG_TABLE_ENTRIES ? t[i1 + 1] : e1;
    unsigned d0 = ((n0 - e0) << 2) & 0xff, d1 = ((n1 - e1) << 2) & 0xff;
    return (e1 << 24) | (d1 << 16) | (e0 << 8) | d0;
}

float vcr_fog_ramp_oow(float amount)
{
    float p, w0, w1, fr, w;
    int i;
    if (amount < 0.0f)
        amount = 0.0f;
    if (amount > 1.0f)
        amount = 1.0f;
    p = amount * (float)(VCR_FOG_TABLE_ENTRIES - 1);
    i = (int)p;
    if (i >= VCR_FOG_TABLE_ENTRIES - 1)
        return 1.0f / vcr_fog_index_w(VCR_FOG_TABLE_ENTRIES - 1);
    fr = p - (float)i;
    w0 = vcr_fog_index_w(i);
    w1 = vcr_fog_index_w(i + 1);
    w = w0 + (w1 - w0) * fr;
    return 1.0f / w;
}
