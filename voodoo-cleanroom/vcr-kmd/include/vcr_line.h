/*
 * vcr_line.h - the pure arithmetic behind the display driver's accelerated
 * cosmetic lines and 8x8 monochrome pattern fills (vcrdd_punt.c). Integer
 * only and Win32-free: the host tests compile it as is.
 *
 * Lines: GDI draws a cosmetic solid line from (x1,y1) to (x2,y2) EXCLUDING
 * the last pixel. Only a horizontal or vertical line is taken here - it is a
 * rectangle, so the 2D engine's rectangle fill draws exactly GDI's pixels by
 * construction. A slanted line is GDI's (its diamond-exit rule decides the
 * pixels; the engine's own line command is not assumed to agree).
 *
 * Patterns: a brush realized from an 8x8 1 bpp bitmap - GDI's hatch brushes
 * and the 50 % grey of every drag rectangle and dotted focus frame. The
 * engine's pattern is two dwords, rows 0-3 then rows 4-7, one byte per row,
 * the leftmost pixel in the byte's most significant bit - the order of a
 * Windows 1 bpp bitmap row (xf86-video-tdfx's Mono8x8 fill declares
 * BIT_ORDER_IN_BYTE_MSBFIRST with the pattern dwords programmed as is).
 */
#ifndef VCR_LINE_H
#define VCR_LINE_H

typedef struct vcr_hrect { long left, top, right, bottom; } vcr_hrect;   /* half-open */

/* the pixels of a horizontal or vertical cosmetic line, last pixel excluded,
 * as a half-open rectangle. 1 = a rectangle (possibly empty: a zero-length
 * line draws nothing), 0 = not axis-aligned (the caller hands it to GDI). */
static __inline int vcr_line_rect(long x1, long y1, long x2, long y2, vcr_hrect *r)
{
    if (y1 == y2) {
        r->top = y1;
        r->bottom = y1 + 1;
        if (x2 >= x1) { r->left = x1; r->right = x2; }          /* x1 .. x2-1 */
        else          { r->left = x2 + 1; r->right = x1 + 1; }  /* x1 down to x2+1 */
        return 1;
    }
    if (x1 == x2) {
        r->left = x1;
        r->right = x1 + 1;
        if (y2 >= y1) { r->top = y1; r->bottom = y2; }
        else          { r->top = y2 + 1; r->bottom = y1 + 1; }
        return 1;
    }
    return 0;
}

/* r clipped to c; 0 when nothing is left */
static __inline int vcr_hrect_clip(vcr_hrect *r, const vcr_hrect *c)
{
    if (r->left < c->left) r->left = c->left;
    if (r->top < c->top) r->top = c->top;
    if (r->right > c->right) r->right = c->right;
    if (r->bottom > c->bottom) r->bottom = c->bottom;
    return r->right > r->left && r->bottom > r->top;
}

/* a PATHOBJ point in 28.4 fixed point that is a whole pixel; its pixel */
static __inline int vcr_fix_whole(long fix, long *px)
{
    if (fix & 0xf)
        return 0;
    *px = fix >> 4;
    return 1;
}

/* an 8x8 1 bpp brush bitmap (8 rows, `delta` bytes apart, may be negative)
 * as the engine's two pattern dwords */
static __inline void vcr_pat_pack(const unsigned char *row0, long delta, unsigned long *p0,
                                  unsigned long *p1)
{
    unsigned long a = 0, b = 0;
    int i;
    for (i = 0; i < 4; i++) {
        a |= (unsigned long)row0[i * delta] << (8 * i);
        b |= (unsigned long)row0[(i + 4) * delta] << (8 * i);
    }
    *p0 = a;
    *p1 = b;
}

/* the pattern's x (or y) offset for a brush origin: GDI paints pixel x with
 * brush column (x - org) & 7; the engine's pattern is aligned to its own
 * coordinate origin (xf86-video-tdfx: HARDWARE_PATTERN_SCREEN_ORIGIN), which
 * sits `skew` pixels left of the surface (the 16-byte base rounding) - so the
 * offset is (-org - skew) & 7 */
static __inline unsigned vcr_pat_offset(long org, unsigned long skew)
{
    return (unsigned)((0 - org - (long)skew) & 7);
}

/* the ROP3s a mono pattern fill takes, and whether the background is
 * transparent (ROP4 0xAAF0: the pattern's 0 bits leave the destination) -
 * 0 = not ours */
#define VCR_PATROP_COPY     0xf0u       /* PATCOPY */
#define VCR_PATROP_INVERT   0x5au       /* PATINVERT */
static __inline int vcr_pat_rop(unsigned long rop4, unsigned *rop3, int *transparent)
{
    unsigned fg = (unsigned)(rop4 & 0xff), bg = (unsigned)((rop4 >> 8) & 0xff);
    if (fg != VCR_PATROP_COPY && fg != VCR_PATROP_INVERT)
        return 0;
    if (bg == fg) { *rop3 = fg; *transparent = 0; return 1; }
    if (bg == 0xaa && fg == VCR_PATROP_COPY) { *rop3 = fg; *transparent = 1; return 1; }
    return 0;
}

#endif /* VCR_LINE_H */
