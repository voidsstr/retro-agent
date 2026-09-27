/*
 * vcr_text.h - the 2D engine's text path: which DrvTextOut calls the engine
 * draws (display/vcrdd_punt.c), how a glyph is clipped, how the visible parts
 * of a string's glyphs become ONE 1 bpp mask, and the exact dword stream a
 * bitmap becomes (display/vcrdd_2d.c VcrDd2dMonoGlyph). Integer only and
 * Win32-free: tests/native/test_vcr_kmd_text.c compiles this header and
 * paints what the stream says the way the engine consumes it, against a
 * per-pixel software reference.
 *
 * The operation is a HOST-TO-SCREEN blit with a 1 bpp source - monochrome
 * expansion. Register semantics from the 3dfx Glide GPL release
 * (glide3x/h3/incsrc/h3gdefs.h SSTG_*, h3regs.h SstGRegs; offsets in
 * vcrdd_2d.c):
 *   command   SSTG_HOST_BLT (3) | SSTG_TRANSPARENT (bit 16: a 0 bit leaves the
 *             destination alone) | ROP 0xCC << 24 (S: a 1 bit takes
 *             colorFore, which is a pixel in the DESTINATION's format). No
 *             SSTG_GO: a host blit starts with its first data write.
 *   srcFormat SSTG_PIXFMT_1BPP (0 << 16) | SSTG_SRC_PACK_8 (1 << 22): every
 *             row starts on a byte and the rows follow each other - GDI's
 *             GLYPHBITS.aj exactly - and no byte or word swizzle.
 *   srcXY     0.
 *   dstSize   the VISIBLE part of the bitmap (w | h << 16); dstXY where it
 *             starts. Nothing is left to the clip registers: the part is cut
 *             out of the bitmap here (rows skipped, columns shifted), so
 *             the engine only ever expands a whole, unclipped bitmap. The
 *             bitmap is normally a whole string's mask (below): one blit
 *             per string and clip rectangle, not one per glyph.
 *   data      ceil(ceil(w / 8) * h / 4) dwords to the launch area (0x80),
 *             the FIRST byte of the stream in bits 7:0 (the host's
 *             little-endian dword), the first pixel of a byte in its bit 7
 *             (GDI's order). EXACTLY that many: the engine waits for the rest
 *             of a blit it was given too little for, and a dword too many is
 *             a launch write for a blit that already ended.
 * The 86Box Voodoo3 (tools/86box/, the only bed that runs this path)
 * implements the same semantics (vid_voodoo_banshee_blitter.c: the 1 bpp
 * byte-packed host blit, 0x80 >> (x & 7), TRANSPARENT = COMMAND_TRANS_MONO);
 * gdilab's text test compares the result with GDI's own software rendering.
 *
 * What is NOT drawn by the engine goes to EngTextOut exactly as before
 * (vcr_text_gate's reasons): the path is off (Diag\Accel2DText not 1 - the default), the
 * engine is not usable, a mix other than R2_COPYPEN both ways, a brush that
 * is not solid, anti-aliased or ClearType glyphs, a fixed-pitch string laid
 * out vertically or right to left, too many underline/strike-out rectangles;
 * and during drawing a glyph with no bits or too large, or an engine that
 * gave up. A software redraw after a partial engine draw is correct because
 * every accepted call is R2_COPYPEN: each pixel is written from its source
 * alone, so drawing it twice leaves the same pixels.
 */
#ifndef VCR_TEXT_H
#define VCR_TEXT_H

#include "vcr_types.h"

/* ---- the registers' values ------------------------------------------------ */
#define VCR_2D_CMD_HOST_BLT     3u              /* SSTG_HOST_BLT */
#define VCR_2D_CMD_GO           (1u << 8)       /* SSTG_GO - NOT used for a host blit */
#define VCR_2D_CMD_TRANSPARENT  (1u << 16)      /* SSTG_TRANSPARENT */
#define VCR_2D_ROP_SRCCOPY      0xccu
#define VCR_2D_SRC_1BPP         (0u << 16)      /* SSTG_PIXFMT_1BPP */
#define VCR_2D_SRC_PACK_8       (1u << 22)      /* SSTG_SRC_PACK_8 */
#define VCR_2D_SRC_BYTE_SWIZZLE (1u << 20)      /* SSTG_HOST_BYTE_SWIZZLE - NOT used */
#define VCR_2D_SRC_WORD_SWIZZLE (1u << 21)      /* SSTG_HOST_WORD_SWIZZLE - NOT used */

#define VCR_TEXT_CMD    (VCR_2D_CMD_HOST_BLT | VCR_2D_CMD_TRANSPARENT | (VCR_2D_ROP_SRCCOPY << 24))
#define VCR_TEXT_SRCFMT (VCR_2D_SRC_1BPP | VCR_2D_SRC_PACK_8)

/* ---- the DDI values the gate reads (winddi.h; vcrdd_punt.c asserts them
 * against the real header at compile time) -------------------------------- */
#define VCR_MIX_COPYPEN_BOTH    0x0d0du         /* R2_COPYPEN fore and back */
#define VCR_FO_GRAY16           0x00010000u
#define VCR_FO_CLEARTYPE_X      0x10000000u
#define VCR_FO_CLEARTYPE_Y      0x20000000u
#define VCR_SO_VERTICAL         0x00000004u
#define VCR_SO_REVERSED         0x00000008u
#define VCR_NO_BRUSH            0xffffffffu     /* iSolidColor of a brush that is not solid */
#define VCR_DC_TRIVIAL          0u
#define VCR_DC_RECT             1u
#define VCR_DC_COMPLEX          3u

#define VCR_TEXT_MAX_GLYPH      1024u   /* px, either side: a larger glyph goes to software */
#define VCR_TEXT_MAX_EXTRA      8u      /* underline / strike-out rectangles */
#ifndef VCR_TEXT_MASK_BYTES             /* (-D only for a test build that forces bands) */
#define VCR_TEXT_MASK_BYTES     16384u  /* the string mask (per PDEV): 81 rows of a 1600 px line */
#endif
#define VCR_2D_MAX_XY           8191    /* dstXY / dstSize fields are 13 bits */

/* why a call went to EngTextOut (vcr_text_gate, or during drawing) */
#define VCR_TEXT_OK             0u
#define VCR_TEXT_R_OFF          1u      /* Diag\Accel2DText not 1 (default off) */
#define VCR_TEXT_R_ENGINE       2u      /* no engine, a Glide owner, 24 bpp */
#define VCR_TEXT_R_MIX          3u      /* not R2_COPYPEN both ways */
#define VCR_TEXT_R_BRUSH        4u      /* a text or opaque brush that is not solid */
#define VCR_TEXT_R_FONT         5u      /* anti-aliased / ClearType glyphs */
#define VCR_TEXT_R_LAYOUT       6u      /* fixed pitch, vertical or right to left */
#define VCR_TEXT_R_EXTRA        7u      /* more than VCR_TEXT_MAX_EXTRA extra rectangles */
#define VCR_TEXT_R_GLYPH        8u      /* a glyph without bits, or too large (while drawing) */
#define VCR_TEXT_R_GAVEUP       9u      /* the engine gave up (while drawing) */
#define VCR_TEXT_R_MAX          10u

typedef struct vcr_text_req {
    vcr_u32 off;            /* the Diag switch said no */
    vcr_u32 engine;         /* the 2D engine may be used (g2d_ok, registers, no Glide owner) */
    vcr_u32 bpp;            /* the desktop */
    vcr_u32 mix;
    vcr_u32 fore;           /* pboFore->iSolidColor, VCR_NO_BRUSH when none */
    vcr_u32 has_opaque;     /* prclOpaque != NULL */
    vcr_u32 opaque;         /* pboOpaque->iSolidColor, VCR_NO_BRUSH when none */
    vcr_u32 font_type;      /* FONTOBJ.flFontType */
    vcr_u32 char_inc;       /* STROBJ.ulCharInc */
    vcr_u32 accel;          /* STROBJ.flAccel */
    vcr_u32 extras;         /* vcr_text_extra_count */
} vcr_text_req;

/* 0 = the engine draws it, else VCR_TEXT_R_* (checked in this order) */
static inline vcr_u32 vcr_text_gate(const vcr_text_req *q)
{
    if (q->off)
        return VCR_TEXT_R_OFF;
    if (!q->engine || (q->bpp != 8 && q->bpp != 16 && q->bpp != 32))
        return VCR_TEXT_R_ENGINE;
    if (q->mix != VCR_MIX_COPYPEN_BOTH)
        return VCR_TEXT_R_MIX;
    if (q->fore == VCR_NO_BRUSH || (q->has_opaque && q->opaque == VCR_NO_BRUSH))
        return VCR_TEXT_R_BRUSH;
    if (q->font_type & (VCR_FO_GRAY16 | VCR_FO_CLEARTYPE_X | VCR_FO_CLEARTYPE_Y))
        return VCR_TEXT_R_FONT;
    if (q->char_inc && (q->accel & (VCR_SO_VERTICAL | VCR_SO_REVERSED)))
        return VCR_TEXT_R_LAYOUT;
    if (q->extras > VCR_TEXT_MAX_EXTRA)
        return VCR_TEXT_R_EXTRA;
    return VCR_TEXT_OK;
}

static inline const char *vcr_text_reason(vcr_u32 r)
{
    switch (r) {
    case VCR_TEXT_OK:       return "engine";
    case VCR_TEXT_R_OFF:    return "off (Diag\\Accel2DText not 1)";
    case VCR_TEXT_R_ENGINE: return "engine not usable";
    case VCR_TEXT_R_MIX:    return "mix not COPYPEN";
    case VCR_TEXT_R_BRUSH:  return "brush not solid";
    case VCR_TEXT_R_FONT:   return "anti-aliased or ClearType glyphs";
    case VCR_TEXT_R_LAYOUT: return "fixed pitch, vertical or reversed";
    case VCR_TEXT_R_EXTRA:  return "too many extra rectangles";
    case VCR_TEXT_R_GLYPH:  return "glyph without bits or too large";
    case VCR_TEXT_R_GAVEUP: return "the engine gave up";
    }
    return "?";
}

/* ---- rectangles (RECTL's order and meaning: right and bottom exclusive) --- */
typedef struct vcr_rect {
    vcr_i32 l, t, r, b;
} vcr_rect;

/* a & b into *o; 0 when they do not overlap (then *o is untouched) */
static inline int vcr_rect_isect(const vcr_rect *a, const vcr_rect *b, vcr_rect *o)
{
    vcr_rect x;
    x.l = a->l > b->l ? a->l : b->l;
    x.t = a->t > b->t ? a->t : b->t;
    x.r = a->r < b->r ? a->r : b->r;
    x.b = a->b < b->b ? a->b : b->b;
    if (x.r <= x.l || x.b <= x.t)
        return 0;
    *o = x;
    return 1;
}

/* The clip rectangle of a DC_TRIVIAL or DC_RECT clip, on the surface:
 * TRIVIAL (or no CLIPOBJ) = the whole surface, RECT = its bounds on the
 * surface. 0 when nothing is visible, and for DC_COMPLEX - that one is
 * enumerated rectangle by rectangle (each through vcr_rect_isect with the
 * surface). */
static inline int vcr_text_clip_rect(vcr_u32 complexity, const vcr_rect *bounds,
                                     const vcr_rect *surface, vcr_rect *o)
{
    if (complexity == VCR_DC_TRIVIAL) {
        if (surface->r <= surface->l || surface->b <= surface->t)
            return 0;
        *o = *surface;
        return 1;
    }
    if (complexity == VCR_DC_RECT)
        return vcr_rect_isect(bounds, surface, o);
    return 0;
}

/* prclExtra: rectangles up to one whose corners are both (0,0). The count,
 * or max + 1 when there are more than max (the caller punts - the array is
 * GDI's, never read past its terminator). */
static inline vcr_u32 vcr_text_extra_count(const vcr_rect *e, vcr_u32 max)
{
    vcr_u32 n;
    if (!e)
        return 0;
    for (n = 0; n <= max; n++, e++)
        if (!e->l && !e->t && !e->r && !e->b)
            return n;
    return max + 1;
}

/* Where glyph k of a string starts: GDI's own position (GLYPHPOS.ptl), or for
 * a fixed-pitch string (STROBJ.ulCharInc != 0) the first glyph's plus k
 * increments along x - the DDI's rule for ulCharInc; the gate has already
 * sent vertical and right-to-left fixed-pitch strings to software. */
static inline void vcr_text_pos(vcr_u32 char_inc, vcr_u32 k, vcr_i32 x0, vcr_i32 y0,
                                vcr_i32 px, vcr_i32 py, vcr_i32 *x, vcr_i32 *y)
{
    if (char_inc) {
        *x = x0 + (vcr_i32)(k * char_inc);
        *y = y0;
    } else {
        *x = px;
        *y = py;
    }
}

/* ---- one glyph ------------------------------------------------------------- */
typedef struct vcr_glyph_part {
    vcr_i32 x, y;           /* where the visible part lands on the surface */
    vcr_u32 w, h;           /* its size: dstSize */
    vcr_u32 col0, row0;     /* where it starts in the glyph bitmap */
} vcr_glyph_part;

/* The part of a cx x cy glyph whose top-left pixel is at (gx, gy) that lies
 * inside clip; 0 when none does. */
static inline int vcr_glyph_clip(vcr_i32 gx, vcr_i32 gy, vcr_u32 cx, vcr_u32 cy,
                                 const vcr_rect *clip, vcr_glyph_part *p)
{
    vcr_rect g, v;
    if (!cx || !cy)
        return 0;
    g.l = gx;
    g.t = gy;
    g.r = gx + (vcr_i32)cx;
    g.b = gy + (vcr_i32)cy;
    if (!vcr_rect_isect(&g, clip, &v))
        return 0;
    p->x = v.l;
    p->y = v.t;
    p->w = (vcr_u32)(v.r - v.l);
    p->h = (vcr_u32)(v.b - v.t);
    p->col0 = (vcr_u32)(v.l - gx);
    p->row0 = (vcr_u32)(v.t - gy);
    return 1;
}

/* the dwords the engine takes for a part: rows of ceil(w/8) bytes, packed */
static inline vcr_u32 vcr_glyph_dwords(const vcr_glyph_part *p)
{
    return ((p->w + 7) / 8 * p->h + 3) / 4;
}

/* put(ctx, dword): 1 = written, 0 = stop (the engine gave up) */
typedef int (*vcr_put_fn)(void *ctx, vcr_u32 dw);

/* Byte k of a visible row: the part's pixels col0 + 8k .. col0 + 8k + 7 of
 * `row` (stride bytes, bit 7 = the leftmost pixel), shifted so the first is
 * bit 7, and - in the last byte (k == ob - 1) - the bits past the part's
 * width cleared (`last`). Reads row[b0 + k] and, only when it exists,
 * row[b0 + k + 1]: never outside the row. */
static inline vcr_u32 vcr_glyph_byte(const vcr_u8 *row, vcr_u32 stride, vcr_u32 b0, vcr_u32 s,
                                     vcr_u32 k, vcr_u32 ob, vcr_u32 last)
{
    vcr_u32 i = b0 + k, v = row[i];
    if (s) {
        v <<= s;
        if (i + 1 < stride)
            v |= (vcr_u32)row[i + 1] >> (8 - s);
    }
    return v & (k == ob - 1 ? last : 0xffu);
}

/* The dword stream for one part of a glyph whose bitmap is `bits` (cx wide:
 * rows of ceil(cx/8) bytes, bit 7 = the leftmost pixel): each visible row
 * shifted so that its first visible pixel is bit 7 of its first byte, the
 * bits past the part's width cleared (a stray bit could only ever be drawn,
 * never erase), packed into dwords first byte lowest. Reads only inside the
 * bitmap. Returns the dwords put - vcr_glyph_dwords(p) unless put stopped. */
static inline vcr_u32 vcr_glyph_stream(const vcr_u8 *bits, vcr_u32 cx, const vcr_glyph_part *p,
                                       vcr_put_fn put, void *ctx)
{
    vcr_u32 stride = (cx + 7) >> 3, ob = (p->w + 7) >> 3, s = p->col0 & 7, b0 = p->col0 >> 3;
    vcr_u32 tail = p->w & 7, last = tail ? (0xffu << (8 - tail)) & 0xffu : 0xffu;
    vcr_u32 acc = 0, nb = 0, n = 0, y, k;
    for (y = 0; y < p->h; y++) {
        const vcr_u8 *row = bits + (p->row0 + y) * stride;
        for (k = 0; k < ob; k++) {
            acc |= vcr_glyph_byte(row, stride, b0, s, k, ob, last) << (8 * nb);
            if (++nb == 4) {
                if (!put(ctx, acc))
                    return n;
                n++;
                acc = 0;
                nb = 0;
            }
        }
    }
    if (nb) {
        if (!put(ctx, acc))
            return n;
        n++;
    }
    return n;
}

/* ---- a whole string as ONE blit ------------------------------------------
 *
 * One host blit per glyph costs a command setup per glyph (dstSize, dstXY,
 * command: three FIFO writes, and the engine's own start-up) - more than the
 * data of an 8-point glyph. So the visible glyphs of a string are first OR-ed
 * into one 1 bpp mask covering their union - on the CPU, in system memory -
 * and the mask goes to the engine as a single bitmap: one command per string
 * (per clip rectangle, per band of rows that fits the mask buffer). Every
 * glyph of a call has the same colour, so the union of their pixels drawn
 * once is exactly what drawing them one after another leaves - overlapping
 * italic glyphs included.
 *
 * The mask: rows of mstride = ceil(width / 8) bytes, bit 7 = the leftmost
 * pixel - the same format as a glyph, so vcr_glyph_stream sends it. */

/* rows of a w-pixel-wide mask that fit in `bytes`; 0 when not even one does */
static inline vcr_u32 vcr_mask_rows(vcr_u32 w, vcr_u32 bytes)
{
    vcr_u32 st = (w + 7) >> 3;
    return st ? bytes / st : 0;
}

/* OR the part p of a glyph (bits, cx wide) into the mask at pixel (mx, my):
 * the part's rows, each shifted to bit mx. Reads only inside the glyph
 * (vcr_glyph_byte), writes only mask bytes of pixels mx .. mx + w - 1 of rows
 * my .. my + h - 1 - the caller has checked that they lie in the mask. */
static inline void vcr_mask_or(vcr_u8 *mask, vcr_u32 mstride, vcr_u32 mx, vcr_u32 my,
                               const vcr_u8 *bits, vcr_u32 cx, const vcr_glyph_part *p)
{
    vcr_u32 stride = (cx + 7) >> 3, ob = (p->w + 7) >> 3, s = p->col0 & 7, b0 = p->col0 >> 3;
    vcr_u32 tail = p->w & 7, last = tail ? (0xffu << (8 - tail)) & 0xffu : 0xffu;
    vcr_u32 ds = mx & 7, y, k;
    for (y = 0; y < p->h; y++) {
        const vcr_u8 *row = bits + (p->row0 + y) * stride;
        vcr_u8 *d = mask + (my + y) * mstride + (mx >> 3);
        for (k = 0; k < ob; k++) {
            vcr_u32 v = vcr_glyph_byte(row, stride, b0, s, k, ob, last);
            if (!v)
                continue;
            d[k] |= (vcr_u8)(v >> ds);
            /* the low bits spill into the next byte - only when they hold a
             * pixel, which then lies inside the part and so inside the mask */
            if (ds && (vcr_u8)(v << (8 - ds)))
                d[k + 1] |= (vcr_u8)(v << (8 - ds));
        }
    }
}

#endif /* VCR_TEXT_H */
