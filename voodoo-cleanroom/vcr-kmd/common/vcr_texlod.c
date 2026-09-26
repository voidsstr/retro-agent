/* vcr_texlod.c - see include/vcr_texlod.h */
#include "../include/vcr_texlod.h"

static vcr_u32 log2u(vcr_u32 v)
{
    vcr_u32 l = 0;
    while (v > 1) {
        v >>= 1;
        l++;
    }
    return l;
}

int vcr_texlod_compute(vcr_u32 w, vcr_u32 h, vcr_u32 bytes, vcr_u32 pitch, vcr_u32 offset,
                       vcr_texlod *out)
{
    vcr_u32 big, small, l, pre = 0;
    if (!w || !h || (w & (w - 1)) || (h & (h - 1)) || w > 256 || h > 256 || !bytes ||
        pitch != w * bytes)
        return -1;
    big = w > h ? w : h;
    small = w > h ? h : w;
    out->aspect = log2u(big / small);
    if (out->aspect > 3)
        return -1;
    out->lod = 8 - log2u(big);
    out->s_is_wider = w > h;
    for (l = 0; l < out->lod; l++) {            /* the levels this texture does not have */
        vcr_u32 lb = 256u >> l, ls = lb >> out->aspect;
        pre += lb * (ls ? ls : 1) * bytes;
    }
    out->base = (offset - pre) & 0x00fffff0u;
    out->tlod = (out->lod << 2) | (out->lod << 8) | (out->aspect << 21) |
                (out->s_is_wider ? (1u << 20) : 0);
    return 0;
}
