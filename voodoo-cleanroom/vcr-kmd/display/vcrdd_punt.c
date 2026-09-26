/*
 * vcrdd_punt.c - the primary is a DEVICE surface; GDI's drawing on it comes
 * here and is handed to the DIB engine on the frame buffer bitmap behind it.
 *
 * Why not just give GDI the frame buffer as the primary (what this driver did
 * until 2026-09-26): DirectDraw on XP will not run over a primary GDI draws
 * into on its own. It probed our DirectDraw HAL on every PDEV and switched it
 * off again; the drivers it does accept - the DDK's samples, VirtualBox's XPDM
 * driver - all present an opaque device surface and hook the drawing calls,
 * so that GDI's access to the screen goes through the driver. Same result on
 * the screen, one call deeper - and the hooks below are exactly where the 2D
 * engine takes over, one call at a time, later.
 *
 * Every hook: swap the device surface for the engine bitmap over the same
 * bits (pd->psoBits), call the Eng* routine, return its answer.
 */
#include "vcrdd.h"

static SURFOBJ *bits(SURFOBJ *pso)
{
    if (pso && pso->iType != STYPE_BITMAP && pso->dhpdev) {
        VCR_PDEV *pd = (VCR_PDEV *)pso->dhpdev;
        if (pd->psoBits)
            return pd->psoBits;
    }
    return pso;
}

BOOL APIENTRY DrvBitBlt(SURFOBJ *dst, SURFOBJ *src, SURFOBJ *mask, CLIPOBJ *co, XLATEOBJ *xo,
                        RECTL *rd, POINTL *ps, POINTL *pm, BRUSHOBJ *bo, POINTL *pb, ROP4 rop)
{
    return EngBitBlt(bits(dst), bits(src), mask, co, xo, rd, ps, pm, bo, pb, rop);
}

BOOL APIENTRY DrvCopyBits(SURFOBJ *dst, SURFOBJ *src, CLIPOBJ *co, XLATEOBJ *xo, RECTL *rd,
                          POINTL *ps)
{
    return EngCopyBits(bits(dst), bits(src), co, xo, rd, ps);
}

BOOL APIENTRY DrvTextOut(SURFOBJ *pso, STROBJ *str, FONTOBJ *fo, CLIPOBJ *co, RECTL *extra,
                         RECTL *opaque, BRUSHOBJ *fore, BRUSHOBJ *back, POINTL *org, MIX mix)
{
    return EngTextOut(bits(pso), str, fo, co, extra, opaque, fore, back, org, mix);
}

BOOL APIENTRY DrvStrokePath(SURFOBJ *pso, PATHOBJ *po, CLIPOBJ *co, XFORMOBJ *xo, BRUSHOBJ *bo,
                            POINTL *org, LINEATTRS *la, MIX mix)
{
    return EngStrokePath(bits(pso), po, co, xo, bo, org, la, mix);
}

BOOL APIENTRY DrvFillPath(SURFOBJ *pso, PATHOBJ *po, CLIPOBJ *co, BRUSHOBJ *bo, POINTL *org,
                          MIX mix, FLONG fl)
{
    return EngFillPath(bits(pso), po, co, bo, org, mix, fl);
}

BOOL APIENTRY DrvLineTo(SURFOBJ *pso, CLIPOBJ *co, BRUSHOBJ *bo, LONG x1, LONG y1, LONG x2,
                        LONG y2, RECTL *bounds, MIX mix)
{
    return EngLineTo(bits(pso), co, bo, x1, y1, x2, y2, bounds, mix);
}

BOOL APIENTRY DrvStretchBlt(SURFOBJ *dst, SURFOBJ *src, SURFOBJ *mask, CLIPOBJ *co,
                            XLATEOBJ *xo, COLORADJUSTMENT *ca, POINTL *ht, RECTL *rd,
                            RECTL *rs, POINTL *pm, ULONG mode)
{
    return EngStretchBlt(bits(dst), bits(src), mask, co, xo, ca, ht, rd, rs, pm, mode);
}

BOOL APIENTRY DrvStretchBltROP(SURFOBJ *dst, SURFOBJ *src, SURFOBJ *mask, CLIPOBJ *co,
                               XLATEOBJ *xo, COLORADJUSTMENT *ca, POINTL *ht, RECTL *rd,
                               RECTL *rs, POINTL *pm, ULONG mode, BRUSHOBJ *bo, DWORD rop)
{
    return EngStretchBltROP(bits(dst), bits(src), mask, co, xo, ca, ht, rd, rs, pm, mode, bo,
                            rop);
}

BOOL APIENTRY DrvAlphaBlend(SURFOBJ *dst, SURFOBJ *src, CLIPOBJ *co, XLATEOBJ *xo, RECTL *rd,
                            RECTL *rs, BLENDOBJ *blend)
{
    return EngAlphaBlend(bits(dst), bits(src), co, xo, rd, rs, blend);
}

BOOL APIENTRY DrvGradientFill(SURFOBJ *pso, CLIPOBJ *co, XLATEOBJ *xo, TRIVERTEX *v, ULONG nv,
                              PVOID mesh, ULONG nm, RECTL *ext, POINTL *org, ULONG mode)
{
    return EngGradientFill(bits(pso), co, xo, v, nv, mesh, nm, ext, org, mode);
}

BOOL APIENTRY DrvTransparentBlt(SURFOBJ *dst, SURFOBJ *src, CLIPOBJ *co, XLATEOBJ *xo,
                                RECTL *rd, RECTL *rs, ULONG color, ULONG reserved)
{
    return EngTransparentBlt(bits(dst), bits(src), co, xo, rd, rs, color, reserved);
}
