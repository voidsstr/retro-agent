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
 * bits (pd->psoBits), call the Eng* routine, return its answer - after
 * waiting for the 2D engine, which may still be writing the same memory.
 *
 * Taken over by the 2D engine (vcrdd_2d.c) so far: screen-to-screen copies
 * (window moves, scrolling) and solid fills, clip rectangle by clip
 * rectangle. Anything else - a ROP with a pattern, a translate, a mask, a
 * source in system memory - is punted exactly as before.
 */
#include "vcrdd.h"

static VCR_PDEV *dev_of(SURFOBJ *pso)
{
    return (pso && pso->iType != STYPE_BITMAP && pso->dhpdev) ? (VCR_PDEV *)pso->dhpdev : NULL;
}

static SURFOBJ *bits(SURFOBJ *pso)
{
    VCR_PDEV *pd = dev_of(pso);
    if (pd && pd->psoBits) {
        VcrDd2dSync(pd);            /* the CPU draws next */
        return pd->psoBits;
    }
    return pso;
}

static ULONG desk_off(VCR_PDEV *pd)
{
    return (ULONG)(pd->pjScreen - (PUCHAR)pd->pvRamBase);
}

#define ENUM_MAX 16

/* screen-to-screen copy through the engine, every clip rectangle; FALSE =
 * nothing was done, punt it */
static BOOL accel_copy(VCR_PDEV *pd, CLIPOBJ *co, RECTL *rd, POINTL *ps)
{
    LONG dx = ps->x - rd->left, dy = ps->y - rd->top;   /* source = dest + (dx, dy) */
    ULONG bpp = pd->bpp / 8, off = desk_off(pd);
    struct { ULONG c; RECTL r[ENUM_MAX]; } e;
    BOOL more;
    ULONG i, dir;
    if (!pd->g2d_ok || pd->exclusive_pid || pd->bpp == 24)
        return FALSE;
    if (!co || co->iDComplexity == DC_TRIVIAL) {
        if (!VcrDd2dCopy(pd, off, pd->lDelta, off, pd->lDelta, bpp, rd->left + dx,
                         rd->top + dy, rd->left, rd->top, rd->right - rd->left,
                         rd->bottom - rd->top, 0, 0, 0))
            return FALSE;
        pd->g2d_gdi_copies++;
        return TRUE;
    }
    /* several rectangles: visit them in the order the copy moves, so no
     * rectangle reads what an earlier one already overwrote */
    dir = dy < 0 ? (dx < 0 ? CD_LEFTUP : CD_RIGHTUP) : (dx < 0 ? CD_LEFTDOWN : CD_RIGHTDOWN);
    CLIPOBJ_cEnumStart(co, FALSE, CT_RECTANGLES, dir, 0);
    do {
        more = CLIPOBJ_bEnum(co, sizeof e, (ULONG *)&e);
        for (i = 0; i < e.c; i++) {
            RECTL r = e.r[i];
            if (r.left < rd->left) r.left = rd->left;
            if (r.top < rd->top) r.top = rd->top;
            if (r.right > rd->right) r.right = rd->right;
            if (r.bottom > rd->bottom) r.bottom = rd->bottom;
            if (r.right <= r.left || r.bottom <= r.top)
                continue;
            if (!VcrDd2dCopy(pd, off, pd->lDelta, off, pd->lDelta, bpp, r.left + dx, r.top + dy,
                             r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, 0))
                return FALSE;   /* only possible when the engine just gave up: see below */
        }
    } while (more);
    pd->g2d_gdi_copies++;
    return TRUE;
}

static BOOL accel_fill(VCR_PDEV *pd, CLIPOBJ *co, RECTL *rd, ULONG color)
{
    ULONG bpp = pd->bpp / 8, off = desk_off(pd);
    struct { ULONG c; RECTL r[ENUM_MAX]; } e;
    BOOL more;
    ULONG i;
    if (!pd->g2d_ok || pd->exclusive_pid || pd->bpp == 24)
        return FALSE;
    if (!co || co->iDComplexity == DC_TRIVIAL)
        return VcrDd2dFill(pd, off, pd->lDelta, bpp, rd->left, rd->top, rd->right - rd->left,
                           rd->bottom - rd->top, color) && ++pd->g2d_gdi_fills;
    CLIPOBJ_cEnumStart(co, FALSE, CT_RECTANGLES, CD_ANY, 0);
    do {
        more = CLIPOBJ_bEnum(co, sizeof e, (ULONG *)&e);
        for (i = 0; i < e.c; i++) {
            RECTL r = e.r[i];
            if (r.left < rd->left) r.left = rd->left;
            if (r.top < rd->top) r.top = rd->top;
            if (r.right > rd->right) r.right = rd->right;
            if (r.bottom > rd->bottom) r.bottom = rd->bottom;
            if (r.right <= r.left || r.bottom <= r.top)
                continue;
            if (!VcrDd2dFill(pd, off, pd->lDelta, bpp, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, color))
                return FALSE;
        }
    } while (more);
    pd->g2d_gdi_fills++;
    return TRUE;
}

/* A partly-drawn accelerated operation can only come back FALSE when the
 * engine gave up in the middle (and switched itself off): redrawing the whole
 * operation in software is then correct, because both halves write the same
 * pixels from an unchanged source - except for an overlapping copy, whose
 * source the engine may already have overwritten; that one is accepted as a
 * damaged frame rather than a hung engine. */

BOOL APIENTRY DrvBitBlt(SURFOBJ *dst, SURFOBJ *src, SURFOBJ *mask, CLIPOBJ *co, XLATEOBJ *xo,
                        RECTL *rd, POINTL *ps, POINTL *pm, BRUSHOBJ *bo, POINTL *pb, ROP4 rop)
{
    VCR_PDEV *pd = dev_of(dst);
    if (pd && !mask) {
        if (rop == 0xcccc && dev_of(src) == pd && ps && (!xo || (xo->flXlate & XO_TRIVIAL)) &&
            accel_copy(pd, co, rd, ps))
            return TRUE;
        if (rop == 0xf0f0 && bo && bo->iSolidColor != 0xffffffff &&
            accel_fill(pd, co, rd, bo->iSolidColor))
            return TRUE;
        /* BLACKNESS / WHITENESS: every bit of the pixel 0 / 1 */
        if ((rop == 0x0000 || rop == 0xffff) &&
            accel_fill(pd, co, rd, rop ? (pd->bpp >= 32 ? 0xffffffffu : (1u << pd->bpp) - 1) : 0))
            return TRUE;
    }
    return EngBitBlt(bits(dst), bits(src), mask, co, xo, rd, ps, pm, bo, pb, rop);
}

BOOL APIENTRY DrvCopyBits(SURFOBJ *dst, SURFOBJ *src, CLIPOBJ *co, XLATEOBJ *xo, RECTL *rd,
                          POINTL *ps)
{
    VCR_PDEV *pd = dev_of(dst);
    if (pd && dev_of(src) == pd && ps && (!xo || (xo->flXlate & XO_TRIVIAL)) &&
        accel_copy(pd, co, rd, ps))
        return TRUE;
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
