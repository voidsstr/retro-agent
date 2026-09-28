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

/* ---- the VSA-100 texture path (include/vcr_texlod.h) ------------------------------ */

vcr_u32 vcr_tex_level_bytes(vcr_u32 kind, vcr_u32 w, vcr_u32 h)
{
    if (!w)
        w = 1;
    if (!h)
        h = 1;
    switch (kind) {
    case VCR_TEXK_RGB16:
        return w * h * 2;
    case VCR_TEXK_ARGB32:
        return w * h * 4;
    case VCR_TEXK_DXT1:                         /* 4 bits a texel, in 8x4 units */
        return (w < 8 ? 8 : w) * (h < 4 ? 4 : h) / 2;
    case VCR_TEXK_DXT35:                        /* 8 bits a texel, in 4x4 units */
        return (w < 4 ? 4 : w) * (h < 4 ? 4 : h);
    default:
        return 0;
    }
}

vcr_u32 vcr_tex_chain_offset(vcr_u32 kind, vcr_u32 w, vcr_u32 h, vcr_u32 k)
{
    vcr_u32 off = 0, i;
    for (i = 0; i < k && i < 12; i++)
        off += vcr_tex_level_bytes(kind, w >> i, h >> i);
    return off;
}

vcr_u32 vcr_tex_chain_usable(vcr_u32 kind, vcr_u32 w, vcr_u32 h, vcr_u32 have)
{
    vcr_u32 n = 0;
    (void)h;
    if (kind != VCR_TEXK_DXT1)
        return have;
    while (n < have && n < 12 && (w >> n) >= 8)
        n++;
    return n;
}

vcr_u32 vcr_tex_munge(vcr_u32 lin)
{
    return (lin & 0x01fffff0u) | ((lin >> 24) & 0x2u);     /* SST_TEXTURE_MUNGE_ADDRESS */
}

vcr_u32 vcr_tex_unmunge(vcr_u32 reg)
{
    return (reg & 0x01fffff0u) | ((reg & 0x2u) << 24);
}

vcr_u32 vcr_tex_writeback_addr(vcr_u32 kind, vcr_u32 tbig, vcr_u32 addr)
{
    return kind == VCR_TEXK_RGB16 && !tbig && addr < VCR_TEX_WRITEBACK_LIMIT ? addr : ~0u;
}

int vcr_texlod_compute_ext(vcr_u32 w, vcr_u32 h, vcr_u32 kind, vcr_u32 pitch, vcr_u32 offset,
                           vcr_u32 flags, vcr_texlod_ext *out)
{
    vcr_u32 napalm = flags & VCR_TEXF_NAPALM, big_ok = napalm && (flags & VCR_TEXF_BIG);
    vcr_u32 big, small, top, l, adj = 0, lin;
    if (!w || !h || (w & (w - 1)) || (h & (h - 1)) || kind > VCR_TEXK_DXT35)
        return -1;
    if (kind != VCR_TEXK_RGB16 && !napalm)
        return -1;                              /* 32-bit and compressed: VSA-100 only */
    big = w > h ? w : h;
    small = w > h ? h : w;
    if (big > (big_ok ? 2048u : 256u) || log2u(big / small) > 3)
        return -1;
    if ((kind == VCR_TEXK_RGB16 && pitch != w * 2) || (kind == VCR_TEXK_ARGB32 && pitch != w * 4))
        return -1;
    if (kind == VCR_TEXK_DXT1 && w < 8)
        return -1;                              /* the TMU's 8x4 unit is not D3D's layout */
    top = log2u(big);
    out->t.aspect = log2u(big / small);
    out->t.s_is_wider = w > h;
    out->tbig = top > 8;
    out->lod_limit = out->tbig ? 11u : 8u;
    out->t.lod = out->lod_limit - top;
    if (out->tbig) {
        /* the levels wider than 256 come BEFORE the one texBaseAddr names */
        for (l = top; l > 8; l--) {
            vcr_u32 lb = 1u << l, ls = lb >> out->t.aspect;
            adj += vcr_tex_level_bytes(kind, out->t.s_is_wider ? lb : ls,
                                       out->t.s_is_wider ? ls : lb);
        }
        lin = offset + adj;
    } else {
        /* the levels this texture does not have: 256 down to twice its size */
        for (l = 8; l > top; l--) {
            vcr_u32 lb = 1u << l, ls = lb >> out->t.aspect;
            if (!ls)
                ls = 1;
            adj += vcr_tex_level_bytes(kind, out->t.s_is_wider ? lb : ls,
                                       out->t.s_is_wider ? ls : lb);
        }
        lin = offset - adj;
    }
    if (napalm) {
        out->lin_base = lin & 0x03fffff0u;      /* 64 MB */
        out->t.base = vcr_tex_munge(out->lin_base);
    } else {
        out->t.base = lin & 0x00fffff0u;        /* the Voodoo3's 16 MB field */
        out->lin_base = out->t.base;
    }
    out->t.tlod = (out->t.lod << 2) | (out->t.lod << 8) | (out->t.aspect << 21) |
                  (out->t.s_is_wider ? (1u << 20) : 0) | (out->tbig ? VCR_TEX_TBIG : 0);
    return 0;
}
