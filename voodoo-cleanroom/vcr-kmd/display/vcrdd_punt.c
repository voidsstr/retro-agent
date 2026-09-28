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
 * rectangle, and text - opaque rectangle, 1 bpp glyphs by monochrome
 * expansion, underline/strike-out (include/vcr_text.h; the text part is OFF
 * unless Diag\Accel2DText = 1). Anything else - a ROP with a pattern, a
 * translate, a mask, a source in system memory, anti-aliased glyphs - is
 * punted exactly as before.
 */
#include "vcrdd.h"
#include "../include/vcr_line.h"

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

/* ---- 8x8 mono pattern brushes (Diag\Accel2DPattern = 1) ------------------------- */

#define VCR_RB_MAGIC 0x42524356u         /* 'VCRB' */
typedef struct VCR_RBRUSH {
    ULONG magic;
    ULONG pat0, pat1;                   /* the engine's pattern (vcr_line.h vcr_pat_pack) */
    ULONG fore, back;                   /* colours of the 1 and the 0 bits */
} VCR_RBRUSH;

/* Only an 8x8 1 bpp brush is realized here - the 50 % grey dither every drag
 * rectangle uses (no mask), and hatch brushes, which GDI hands over WITH a
 * mask that carries exactly the pattern's bits (measured on the 86Box bed:
 * every hatch style arrived that way; the transparent/opaque choice then
 * comes in DrvBitBlt's ROP4, which vcr_pat_rop reads). Anything else is not
 * realized: GDI then draws the call itself (DrvBitBlt sees no rbrush).
 *
 * the mask is an 8x8 1 bpp bitmap with exactly the pattern's bits */
static int mask_is_pattern(SURFOBJ *pat, SURFOBJ *msk)
{
    unsigned long p0, p1, m0, m1;
    if (msk->iBitmapFormat != BMF_1BPP || msk->sizlBitmap.cx != 8 || msk->sizlBitmap.cy != 8 ||
        !msk->pvScan0 || pat->iBitmapFormat != BMF_1BPP || pat->sizlBitmap.cx != 8 ||
        pat->sizlBitmap.cy != 8 || !pat->pvScan0)
        return 0;
    vcr_pat_pack((const unsigned char *)pat->pvScan0, pat->lDelta, &p0, &p1);
    vcr_pat_pack((const unsigned char *)msk->pvScan0, msk->lDelta, &m0, &m1);
    return p0 == m0 && p1 == m1;
}

BOOL APIENTRY DrvRealizeBrush(BRUSHOBJ *bo, SURFOBJ *target, SURFOBJ *pat, SURFOBJ *msk,
                              XLATEOBJ *xo, ULONG hatch)
{
    VCR_PDEV *pd = target ? (VCR_PDEV *)target->dhpdev : NULL;
    VCR_RBRUSH *rb;
    /* a mask that is not the pattern's own bits is not ours */
    if (pd && pd->pat_on && pat && msk && !mask_is_pattern(pat, msk))
        return FALSE;
    if (!pd || !pd->pat_on || !pat || pat->iBitmapFormat != BMF_1BPP ||
        pat->sizlBitmap.cx != 8 || pat->sizlBitmap.cy != 8 || !pat->pvScan0) {
        static ULONG said;
        if (pd && pd->pat_on && pat && said++ < 8)
            VcrDd(VCR_LV_INFO, VCR_EV_DD_2D, 14, pat->iBitmapFormat,
                  (pat->sizlBitmap.cx << 16) | (pat->sizlBitmap.cy & 0xffff), hatch,
                  "brush not realized: format %u, %ux%u, mask %s, hatch %u",
                  pat->iBitmapFormat, pat->sizlBitmap.cx, pat->sizlBitmap.cy, msk ? "yes" : "no",
                  hatch);
        return FALSE;
    }
    rb = (VCR_RBRUSH *)BRUSHOBJ_pvAllocRbrush(bo, sizeof *rb);
    if (!rb)
        return FALSE;
    rb->magic = VCR_RB_MAGIC;
    vcr_pat_pack((const unsigned char *)pat->pvScan0, pat->lDelta, &rb->pat0, &rb->pat1);
    /* a 1 bpp bitmap's bit 1 is palette entry 1, bit 0 entry 0 */
    rb->fore = xo ? XLATEOBJ_iXlate(xo, 1) : 0xffffffffu;
    rb->back = xo ? XLATEOBJ_iXlate(xo, 0) : 0;
    return TRUE;
}

static BOOL accel_patfill(VCR_PDEV *pd, CLIPOBJ *co, RECTL *rd, BRUSHOBJ *bo, POINTL *pb,
                          ROP4 rop)
{
    ULONG bpp = pd->bpp / 8, off = desk_off(pd), i;
    struct { ULONG c; RECTL r[ENUM_MAX]; } e;
    VCR_RBRUSH *rb;
    unsigned rop3, px, py;
    int transparent;
    BOOL more;
    if (!pd->pat_on || !pd->g2d_ok || pd->exclusive_pid || pd->bpp == 24 || !bo ||
        bo->iSolidColor != 0xffffffffu || !vcr_pat_rop(rop, &rop3, &transparent))
        return FALSE;
    rb = (VCR_RBRUSH *)(bo->pvRbrush ? bo->pvRbrush : BRUSHOBJ_pvGetRbrush(bo));
    if (!rb || rb->magic != VCR_RB_MAGIC) {
        pd->pat_punts++;
        return FALSE;
    }
    px = vcr_pat_offset(pb ? pb->x : 0, 0);
    py = vcr_pat_offset(pb ? pb->y : 0, 0);
    if (!co || co->iDComplexity == DC_TRIVIAL) {
        if (!VcrDd2dPatFill(pd, off, pd->lDelta, bpp, rd->left, rd->top, rd->right - rd->left,
                            rd->bottom - rd->top, rb->pat0, rb->pat1, rb->fore, rb->back, rop3,
                            transparent, px, py))
            return FALSE;
        pd->pat_fills++;
        return TRUE;
    }
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
            if (!VcrDd2dPatFill(pd, off, pd->lDelta, bpp, r.left, r.top, r.right - r.left,
                                r.bottom - r.top, rb->pat0, rb->pat1, rb->fore, rb->back, rop3,
                                transparent, px, py))
                return FALSE;
        }
    } while (more);
    pd->pat_fills++;
    return TRUE;
}

/* ---- axis-aligned cosmetic solid lines (Diag\Accel2DLine = 1) ---------------------
 * a horizontal or vertical line is a rectangle (vcr_line.h), so the engine's
 * fill draws exactly GDI's pixels; anything slanted, styled, wide, or not
 * R2_COPYPEN is GDI's own. */
#define VCR_R2_COPYPEN 13
static int line_mix_ok(MIX mix)
{
    return (mix & 0xff) == VCR_R2_COPYPEN;
}

static BOOL accel_hline(VCR_PDEV *pd, CLIPOBJ *co, LONG x1, LONG y1, LONG x2, LONG y2,
                        ULONG color)
{
    vcr_hrect h;
    RECTL r;
    if (!vcr_line_rect(x1, y1, x2, y2, &h))
        return FALSE;
    if (h.right <= h.left || h.bottom <= h.top)
        return TRUE;                    /* a zero-length line draws nothing */
    r.left = h.left;
    r.top = h.top;
    r.right = h.right;
    r.bottom = h.bottom;
    return accel_fill(pd, co, &r, color);
}

/* A partly-drawn accelerated operation can only come back FALSE when the
 * engine gave up in the middle (and switched itself off): redrawing the whole
 * operation in software is then correct, because both halves write the same
 * pixels from an unchanged source - except for an overlapping copy, whose
 * source the engine may already have overwritten, and a PATINVERT pattern
 * fill clipped to several rectangles, where GDI's redraw inverts the ones the
 * engine already did back again; both are accepted as a damaged frame rather
 * than a hung engine. */

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
        /* an 8x8 mono pattern brush (PATCOPY / PATINVERT, opaque or transparent) */
        if (pd->pat_on && bo && bo->iSolidColor == 0xffffffffu && !src &&
            accel_patfill(pd, co, rd, bo, pb, rop))
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

/* ---- text: glyphs by monochrome expansion (include/vcr_text.h) ------------ */

/* the DDI values vcr_text_gate compares with are winddi.h's */
VCR_STATIC_ASSERT(fo_gray16, VCR_FO_GRAY16 == FO_GRAY16);
VCR_STATIC_ASSERT(fo_ct_x, VCR_FO_CLEARTYPE_X == FO_CLEARTYPE_X);
VCR_STATIC_ASSERT(fo_ct_y, VCR_FO_CLEARTYPE_Y == FO_CLEARTYPE_Y);
VCR_STATIC_ASSERT(so_vertical, VCR_SO_VERTICAL == SO_VERTICAL);
VCR_STATIC_ASSERT(so_reversed, VCR_SO_REVERSED == SO_REVERSED);
VCR_STATIC_ASSERT(dc_trivial, VCR_DC_TRIVIAL == DC_TRIVIAL);
VCR_STATIC_ASSERT(dc_rect, VCR_DC_RECT == DC_RECT);
VCR_STATIC_ASSERT(dc_complex, VCR_DC_COMPLEX == DC_COMPLEX);
VCR_STATIC_ASSERT(copypen, VCR_MIX_COPYPEN_BOTH == ((R2_COPYPEN << 8) | R2_COPYPEN));
VCR_STATIC_ASSERT(rectl_is_vcr_rect, sizeof(RECTL) == sizeof(vcr_rect) &&
                  offsetof(RECTL, right) == offsetof(vcr_rect, r) &&
                  offsetof(RECTL, bottom) == offsetof(vcr_rect, b));
VCR_STATIC_ASSERT(punt_slots, VCR_2DS_PUNT_SLOTS == VCR_TEXT_R_MAX);

static vcr_rect vr(const RECTL *r)
{
    vcr_rect v;
    v.l = r->left;
    v.t = r->top;
    v.r = r->right;
    v.b = r->bottom;
    return v;
}

/* the string's glyphs in order, whatever way GDI hands them over */
typedef struct text_iter {
    STROBJ     *str;
    GLYPHPOS   *pgp;
    ULONG       c, i, k;
    BOOL        more;
    vcr_i32     x0, y0;
} text_iter;

static void ti_start(text_iter *t, STROBJ *str)
{
    t->str = str;
    t->i = t->k = 0;
    if (str->pgp) {                 /* the whole string in one array */
        t->pgp = str->pgp;
        t->c = str->cGlyphs;
        t->more = FALSE;
    } else {
        STROBJ_vEnumStart(str);
        t->more = STROBJ_bEnum(str, &t->c, &t->pgp);
    }
}

/* 1: *gb is the next glyph, its bitmap's top-left at (*gx, *gy); 0: no more;
 * -1: GDI failed or a glyph has no bits */
static int ti_next(text_iter *t, GLYPHBITS **gb, vcr_i32 *gx, vcr_i32 *gy)
{
    vcr_i32 x, y;
    GLYPHPOS *g;
    while (t->i >= t->c) {
        if (t->more == (BOOL)DDI_ERROR)
            return -1;
        if (!t->more)
            return 0;
        t->more = STROBJ_bEnum(t->str, &t->c, &t->pgp);
        t->i = 0;
    }
    g = &t->pgp[t->i++];
    if (!g->pgdf || !(*gb = g->pgdf->pgb))
        return -1;
    if (t->k == 0) {
        t->x0 = g->ptl.x;
        t->y0 = g->ptl.y;
    }
    vcr_text_pos(t->str->ulCharInc, t->k++, t->x0, t->y0, g->ptl.x, g->ptl.y, &x, &y);
    *gx = x + (*gb)->ptlOrigin.x;
    *gy = y + (*gb)->ptlOrigin.y;
    return 1;
}

/* a glyph the engine can take: 0 = blank (skip), 1 = yes, -1 = too large */
static int glyph_ok(const GLYPHBITS *gb)
{
    if (gb->sizlBitmap.cx <= 0 || gb->sizlBitmap.cy <= 0)
        return 0;
    return gb->sizlBitmap.cx <= (LONG)VCR_TEXT_MAX_GLYPH &&
           gb->sizlBitmap.cy <= (LONG)VCR_TEXT_MAX_GLYPH ? 1 : -1;
}

/* Every glyph of the string that shows in clip, in the text colour: the
 * visible parts OR-ed into the PDEV's mask and sent as ONE blit per band of
 * rows (vcr_text.h, "a whole string as ONE blit"); one blit per glyph when
 * there is no mask buffer. VCR_TEXT_OK, or why it stopped (the caller
 * redraws the whole call in software). */
static ULONG text_glyphs(VCR_PDEV *pd, STROBJ *str, const vcr_rect *clip, ULONG color)
{
    VCR_MONO m;
    text_iter t;
    GLYPHBITS *gb;
    vcr_glyph_part part, whole;
    vcr_rect u, band;
    vcr_i32 gx, gy, y0;
    ULONG n = 0, cut = 0, rows, mst;
    int r, ok;
    /* pass 1: which glyphs show, and the union of what shows */
    u.l = u.t = u.r = u.b = 0;
    ti_start(&t, str);
    while ((r = ti_next(&t, &gb, &gx, &gy)) > 0) {
        if ((ok = glyph_ok(gb)) < 0)
            return VCR_TEXT_R_GLYPH;
        if (!ok || !vcr_glyph_clip(gx, gy, gb->sizlBitmap.cx, gb->sizlBitmap.cy, clip, &part))
            continue;
        if (!n++) {
            u.l = part.x;
            u.t = part.y;
            u.r = part.x + (vcr_i32)part.w;
            u.b = part.y + (vcr_i32)part.h;
        } else {
            u.l = part.x < u.l ? part.x : u.l;
            u.t = part.y < u.t ? part.y : u.t;
            u.r = part.x + (vcr_i32)part.w > u.r ? part.x + (vcr_i32)part.w : u.r;
            u.b = part.y + (vcr_i32)part.h > u.b ? part.y + (vcr_i32)part.h : u.b;
        }
        if (part.w != (ULONG)gb->sizlBitmap.cx || part.h != (ULONG)gb->sizlBitmap.cy)
            cut++;
    }
    if (r < 0)
        return VCR_TEXT_R_GLYPH;
    if (!n)
        return VCR_TEXT_OK;
    if (!VcrDd2dMonoBegin(pd, &m, desk_off(pd), pd->lDelta, pd->bpp / 8, color))
        return VCR_TEXT_R_GAVEUP;
    rows = pd->text_mask ? vcr_mask_rows((ULONG)(u.r - u.l), VCR_TEXT_MASK_BYTES) : 0;
    if (!rows) {
        /* no mask: one blit per visible glyph part */
        ti_start(&t, str);
        while (ti_next(&t, &gb, &gx, &gy) > 0) {
            if (glyph_ok(gb) <= 0 ||
                !vcr_glyph_clip(gx, gy, gb->sizlBitmap.cx, gb->sizlBitmap.cy, clip, &part))
                continue;
            if (!VcrDd2dMonoGlyph(&m, gb->aj, gb->sizlBitmap.cx, &part))
                return pd->g2d_ok ? VCR_TEXT_R_GLYPH : VCR_TEXT_R_GAVEUP;
            pd->text_blits++;
        }
    } else {
        /* pass 2, per band of rows: the parts that show in it, OR-ed into
         * the mask at their place in the union, then the mask as one blit */
        mst = (ULONG)(u.r - u.l + 7) >> 3;
        for (y0 = u.t; y0 < u.b; y0 += (vcr_i32)rows) {
            band.l = u.l;
            band.r = u.r;
            band.t = y0;
            band.b = y0 + (vcr_i32)rows < u.b ? y0 + (vcr_i32)rows : u.b;
            memset(pd->text_mask, 0, mst * (ULONG)(band.b - band.t));
            ti_start(&t, str);
            while (ti_next(&t, &gb, &gx, &gy) > 0) {
                if (glyph_ok(gb) <= 0 ||
                    !vcr_glyph_clip(gx, gy, gb->sizlBitmap.cx, gb->sizlBitmap.cy, &band, &part))
                    continue;
                vcr_mask_or(pd->text_mask, mst, (ULONG)(part.x - u.l), (ULONG)(part.y - band.t),
                            gb->aj, gb->sizlBitmap.cx, &part);
            }
            whole.x = u.l;
            whole.y = band.t;
            whole.w = (ULONG)(u.r - u.l);
            whole.h = (ULONG)(band.b - band.t);
            whole.col0 = whole.row0 = 0;
            if (!VcrDd2dMonoGlyph(&m, pd->text_mask, whole.w, &whole))
                return pd->g2d_ok ? VCR_TEXT_R_GLYPH : VCR_TEXT_R_GAVEUP;
            pd->text_blits++;
        }
    }
    pd->text_glyphs += n;
    pd->text_clipped += cut;
    return VCR_TEXT_OK;
}

/* One clip rectangle: the opaque rectangle, the glyphs, then the underline /
 * strike-out rectangles - GDI's order - each cut to the clip. */
static ULONG text_rect(VCR_PDEV *pd, STROBJ *str, const vcr_rect *clip, const RECTL *extra,
                       const RECTL *opaque, ULONG fg, ULONG bg)
{
    ULONG bpp = pd->bpp / 8, off = desk_off(pd), why;
    vcr_rect a, o;
    pd->text_rects++;
    if (opaque) {
        o = vr(opaque);
        if (vcr_rect_isect(&o, clip, &a) &&
            !VcrDd2dFill(pd, off, pd->lDelta, bpp, a.l, a.t, a.r - a.l, a.b - a.t, bg))
            return VCR_TEXT_R_GAVEUP;
    }
    why = text_glyphs(pd, str, clip, fg);
    if (why)
        return why;
    for (; extra && (extra->left || extra->top || extra->right || extra->bottom); extra++) {
        o = vr(extra);
        if (vcr_rect_isect(&o, clip, &a) &&
            !VcrDd2dFill(pd, off, pd->lDelta, bpp, a.l, a.t, a.r - a.l, a.b - a.t, fg))
            return VCR_TEXT_R_GAVEUP;
    }
    return VCR_TEXT_OK;
}

/* the whole call on the engine, clip rectangle by clip rectangle; FALSE =
 * nothing (or part) was drawn - the caller hands ALL of it to EngTextOut,
 * which is correct after a partial draw because every accepted call is
 * R2_COPYPEN (vcr_text.h) */
static BOOL accel_text(VCR_PDEV *pd, STROBJ *str, FONTOBJ *fo, CLIPOBJ *co, RECTL *extra,
                       RECTL *opaque, BRUSHOBJ *fore, BRUSHOBJ *back, MIX mix)
{
    struct { ULONG c; RECTL r[ENUM_MAX]; } e;
    vcr_text_req q;
    vcr_rect surf, clip, rb;
    ULONG why, i, cplx = co ? co->iDComplexity : DC_TRIVIAL;
    BOOL more;
    q.off = pd->text_off;
    q.engine = pd->g2d_ok && pd->pjRegs && !pd->exclusive_pid;
    q.bpp = pd->bpp;
    q.mix = mix;
    q.fore = fore ? fore->iSolidColor : VCR_NO_BRUSH;
    q.has_opaque = opaque != NULL;
    q.opaque = opaque && back ? back->iSolidColor : VCR_NO_BRUSH;
    q.font_type = fo ? fo->flFontType : 0;
    q.char_inc = str ? str->ulCharInc : 0;
    q.accel = str ? str->flAccel : 0;
    q.extras = vcr_text_extra_count((const vcr_rect *)extra, VCR_TEXT_MAX_EXTRA);
    why = str ? vcr_text_gate(&q) : VCR_TEXT_R_GLYPH;
    surf.l = surf.t = 0;
    surf.r = (LONG)pd->cx;
    surf.b = (LONG)pd->cy;
    if (!why) {
        if (cplx != DC_COMPLEX) {
            rb = co ? vr(&co->rclBounds) : surf;
            why = vcr_text_clip_rect(cplx, &rb, &surf, &clip)
                      ? text_rect(pd, str, &clip, extra, opaque, q.fore, q.opaque) : VCR_TEXT_OK;
        } else {
            CLIPOBJ_cEnumStart(co, FALSE, CT_RECTANGLES, CD_ANY, 0);
            do {
                more = CLIPOBJ_bEnum(co, sizeof e, (ULONG *)&e);
                for (i = 0; i < e.c && !why; i++) {
                    rb = vr(&e.r[i]);
                    if (vcr_rect_isect(&rb, &surf, &clip))
                        why = text_rect(pd, str, &clip, extra, opaque, q.fore, q.opaque);
                }
            } while (more && !why);
        }
    }
    if (!why) {
        pd->text_calls++;
        return TRUE;
    }
    pd->text_punts++;
    if (why < VCR_TEXT_R_MAX && pd->text_punt_why[why]++ == 0 && why != VCR_TEXT_R_OFF)
        VcrDd(VCR_LV_DEBUG, VCR_EV_DD_2D, 10, why, mix, q.font_type,
              "text to software (first time on this PDEV): %s", vcr_text_reason(why));
    return FALSE;
}

BOOL APIENTRY DrvTextOut(SURFOBJ *pso, STROBJ *str, FONTOBJ *fo, CLIPOBJ *co, RECTL *extra,
                         RECTL *opaque, BRUSHOBJ *fore, BRUSHOBJ *back, POINTL *org, MIX mix)
{
    VCR_PDEV *pd = dev_of(pso);
    if (pd && accel_text(pd, str, fo, co, extra, opaque, fore, back, mix))
        return TRUE;
    /* as before: the DIB engine on the frame buffer, after the engine is idle
     * (bits() syncs) - also the redraw of a call the engine left half done */
    return EngTextOut(bits(pso), str, fo, co, extra, opaque, fore, back, org, mix);
}

/* every segment of the path axis-aligned between whole pixels, no curves;
 * pass 0 only checks, pass 1 draws (a path is taken whole or not at all) */
static BOOL stroke_axis_path(VCR_PDEV *pd, PATHOBJ *po, CLIPOBJ *co, ULONG color, int draw)
{
    PATHDATA d;
    BOOL more;
    ULONG i;
    long sx = 0, sy = 0, lx = 0, ly = 0, x, y;
    int have = 0;
    PATHOBJ_vEnumStart(po);
    do {
        more = PATHOBJ_bEnum(po, &d);
        if (d.flags & PD_BEZIERS)
            return FALSE;
        for (i = 0; i < d.count; i++) {
            if (!vcr_fix_whole(d.pptfx[i].x, &x) || !vcr_fix_whole(d.pptfx[i].y, &y))
                return FALSE;
            if (i == 0 && (d.flags & PD_BEGINSUBPATH)) {
                sx = lx = x;
                sy = ly = y;
                have = 1;
                continue;
            }
            if (!have)
                return FALSE;
            if (x != lx && y != ly)
                return FALSE;           /* slanted: GDI's */
            if (draw && !accel_hline(pd, co, lx, ly, x, y, color))
                return FALSE;
            lx = x;
            ly = y;
        }
        if ((d.flags & PD_ENDSUBPATH) && (d.flags & PD_CLOSEFIGURE) && have) {
            if (sx != lx && sy != ly)
                return FALSE;
            if (draw && !accel_hline(pd, co, lx, ly, sx, sy, color))
                return FALSE;
        }
    } while (more);
    return TRUE;
}

BOOL APIENTRY DrvStrokePath(SURFOBJ *pso, PATHOBJ *po, CLIPOBJ *co, XFORMOBJ *xo, BRUSHOBJ *bo,
                            POINTL *org, LINEATTRS *la, MIX mix)
{
    VCR_PDEV *pd = dev_of(pso);
    if (pd && pd->line_on && pd->g2d_ok && !pd->exclusive_pid && pd->bpp != 24 && po && bo &&
        bo->iSolidColor != 0xffffffffu && la && !(la->fl & (LA_GEOMETRIC | LA_ALTERNATE)) &&
        !la->pstyle && line_mix_ok(mix)) {
        if (stroke_axis_path(pd, po, co, bo->iSolidColor, 0)) {
            if (stroke_axis_path(pd, po, co, bo->iSolidColor, 1)) {
                pd->line_fills++;
                return TRUE;
            }
            /* only an engine that gave up mid-path lands here: GDI redraws it
             * whole - the same pixels, COPYPEN */
        }
        pd->line_punts++;
    }
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
    VCR_PDEV *pd = dev_of(pso);
    if (pd && pd->line_on && pd->g2d_ok && !pd->exclusive_pid && pd->bpp != 24 && bo &&
        bo->iSolidColor != 0xffffffffu && line_mix_ok(mix) && (x1 == x2 || y1 == y2)) {
        if (accel_hline(pd, co, x1, y1, x2, y2, bo->iSolidColor)) {
            pd->line_fills++;
            return TRUE;
        }
        pd->line_punts++;
    }
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
