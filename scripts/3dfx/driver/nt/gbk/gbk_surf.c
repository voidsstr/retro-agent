/*
 * gbk_surf.c - DDraw surface layout math + the 5a present-convert (pure
 * integer). Design: docs/3dfx-d3d-hal-design.md sec 5a ("32-bit compatibility
 * mode: render 16, present 32") and the M4c-2 DDraw surface path.
 *
 * Consumed by:
 *   - driver/nt/enable.c   Dd_CreateSurface/Dd_Blt (pitch/size math, the bump
 *                          allocator over the VRAM region gbkernel reserves,
 *                          and the CPU 565->8888 convert rows for windowed /
 *                          non-present blits), and
 *   - driver/nt/gbkernel.c gb_swap (dstFormat word selection: the 16bpp plain
 *                          copy vs the 32bpp hardware convert-blit).
 *
 * Kernel-clean: C89, no CRT, no floats, overflow-guarded throughout (these
 * values position writes inside the BAR1 aperture; a wrapped offset would be
 * a kernel scribble over the desktop or the CMDFIFO - design sec 6 risk 2).
 */
#include "gbk.h"

/* Sanity ceilings: the Voodoo3 mode space tops out at 2048 on a side, and
 * bytespp is 1 (P8), 2 (565/1555/4444/Z16) or 4 (the 5a XRGB8888 proxy). */
#define GBK_SURF_MAXDIM  2048UL

/* pitch = w * bytespp rounded up to 16 (SST texture-address granularity,
 * h3defs.h:561). Max result 2048*4 = 8192, well inside 32 bits. */
h3u32
gbk_surf_pitch(h3u32 w, h3u32 bytespp)
{
    if (w == 0 || w > GBK_SURF_MAXDIM)
        return 0;
    if (bytespp != 1UL && bytespp != 2UL && bytespp != 4UL)
        return 0;
    return ((w * bytespp) + 15UL) & ~15UL;
}

/* bytes = pitch * h. pitch is bounded by gbk_surf_pitch (<= 8192) and h by
 * the mode space, so the product is <= 16 MB and cannot wrap; anything
 * outside those bounds fails rather than risking a wrapped size. */
h3u32
gbk_surf_bytes(h3u32 pitch, h3u32 h)
{
    if (pitch == 0 || pitch > (GBK_SURF_MAXDIM * 4UL) + 15UL)
        return 0;
    if (h == 0 || h > GBK_SURF_MAXDIM)
        return 0;
    return pitch * h;
}

/* ==================================================================== */
/* Bump allocator over the reserved DDraw surface region.               */
/* ==================================================================== */

void
gbk_surfheap_init(gbk_surfheap_t *hp, h3u32 base, h3u32 size)
{
    hp->base   = base;
    hp->size   = size;
    hp->cursor = base;
    hp->live   = 0;
    /* a region whose end would wrap 32 bits is poisoned dead: every alloc
     * fails (the guard below compares against remaining = size-derived
     * room, so a 0 size is the natural "no region" state). */
    if (size > 0xFFFFFFFFUL - base)
        hp->size = 0;
}

h3u32
gbk_surfheap_alloc(gbk_surfheap_t *hp, h3u32 bytes)
{
    h3u32 a, used, room;

    if (bytes == 0 || hp->size == 0)
        return GBK_SURF_FAIL;
    a = (hp->cursor + 15UL) & ~15UL;
    if (a < hp->cursor)                 /* align wrapped 32 bits          */
        return GBK_SURF_FAIL;
    if (a < hp->base || a > hp->base + hp->size)
        return GBK_SURF_FAIL;           /* cursor out of region (corrupt) */
    used = a - hp->base;
    room = hp->size - used;             /* overflow-safe remaining bytes  */
    if (bytes > room)
        return GBK_SURF_FAIL;
    hp->cursor = a + bytes;
    hp->live++;
    return a;
}

int
gbk_surfheap_free(gbk_surfheap_t *hp)
{
    /* An unbalanced free (live already 0) is REFUSED without touching the
     * cursor (M4c-2 review #3): the old void version rewound the cursor on
     * every zero-crossing, so one spurious free for a surface the heap never
     * owned could rewind while real surfaces were still allocated - the next
     * alloc then aliased live VRAM. Callers gate frees on an explicit
     * ownership tag; this guard is the allocator's own last line. */
    if (hp->live == 0)
        return -1;
    hp->live--;
    if (hp->live == 0)
        hp->cursor = hp->base;          /* all surfaces gone: rewind      */
    return 0;
}

/* ==================================================================== */
/* The 5a present-convert.                                              */
/* ==================================================================== */

/* RGB565 -> XRGB8888, bit-replicated ("straight 565->888 expansion", design
 * 5a): each channel's high bits are replicated into the low bits so full
 * scale maps to full scale (0x1F -> 0xFF), X byte = 0. */
h3u32
gbk_expand565(h3u32 p565)
{
    h3u32 r = (p565 >> 11) & 0x1FUL;
    h3u32 g = (p565 >> 5)  & 0x3FUL;
    h3u32 b =  p565        & 0x1FUL;

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* One surface row of the CPU convert (the bring-up path design 5a allows
 * for windowed / partial-rect presents; the full-screen present uses the
 * 2D engine via gbk_present_dstformat instead). */
void
gbk_conv565_row_8888(h3u32 *dst, const h3u16 *src, h3u32 n)
{
    h3u32 i;
    for (i = 0; i < n; i++)
        dst[i] = gbk_expand565((h3u32)src[i]);
}

/* ARGB1555 -> ARGB8888 (M4c-2 review #11): 5-bit channels bit-replicated
 * like the 565 expansion (0x1F -> 0xFF), the 1-bit alpha replicated to a
 * full byte (0x00 / 0xFF). Decoding a 1555 pixel with 565 field widths
 * hue-shifts it (opaque red 0xFC00 came out orange 0x00FF8200), which is
 * exactly the mis-dispatch this expander exists to prevent. */
h3u32
gbk_expand1555(h3u32 p1555)
{
    h3u32 a = (p1555 >> 15) & 0x1UL;
    h3u32 r = (p1555 >> 10) & 0x1FUL;
    h3u32 g = (p1555 >> 5)  & 0x1FUL;
    h3u32 b =  p1555        & 0x1FUL;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return ((a ? 0xFFUL : 0x00UL) << 24) | (r << 16) | (g << 8) | b;
}

void
gbk_conv1555_row_8888(h3u32 *dst, const h3u16 *src, h3u32 n)
{
    h3u32 i;
    for (i = 0; i < n; i++)
        dst[i] = gbk_expand1555((h3u32)src[i]);
}

/* ARGB4444 -> ARGB8888 (M4c-2 review #11): every 4-bit channel replicated
 * into both nibbles (0xF -> 0xFF), alpha included. */
h3u32
gbk_expand4444(h3u32 p4444)
{
    h3u32 a = (p4444 >> 12) & 0xFUL;
    h3u32 r = (p4444 >> 8)  & 0xFUL;
    h3u32 g = (p4444 >> 4)  & 0xFUL;
    h3u32 b =  p4444        & 0xFUL;

    a = (a << 4) | a;
    r = (r << 4) | r;
    g = (g << 4) | g;
    b = (b << 4) | b;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void
gbk_conv4444_row_8888(h3u32 *dst, const h3u16 *src, h3u32 n)
{
    h3u32 i;
    for (i = 0; i < n; i++)
        dst[i] = gbk_expand4444((h3u32)src[i]);
}

/* DP2 buffer-window clamp (M4c-2 review #4/#6; used by enable.c
 * d3d_DrawPrimitives2). Nonzero when the window [off, off + len*stride)
 * lies inside a bufBytes-byte buffer; stride 1 gives the plain byte-window
 * form (the command buffer), stride = the FVF vertex stride bounds the
 * vertex window. Overflow-safe: len*stride is never formed (len is compared
 * against the remaining room DIVIDED by stride), and off is bounded before
 * the subtraction. bufBytes 0 (surface size unknown) fails - the caller
 * must reject the batch rather than walk an unbounded window. */
int
gbk_dp2_window_ok(h3u32 bufBytes, h3u32 off, h3u32 len, h3u32 stride)
{
    if (bufBytes == 0 || stride == 0)
        return 0;
    if (off > bufBytes)
        return 0;
    return len <= (bufBytes - off) / stride;
}

/* dstFormat word for the blit-present (gb_swap): stride in the linear-stride
 * field + the pixel format for the desktop depth. A 32bpp desktop makes the
 * 2D engine do the 565->8888 expansion during the SRCCOPY blt ("via the V3
 * 2D blt (stretch/copy with format convert)", design 5a). 0 = unsupported
 * (callers skip the present rather than program a corrupt format). */
h3u32
gbk_present_dstformat(h3u32 desktopStride, h3u32 desktopBpp)
{
    h3u32 fmt;

    if (desktopStride == 0 ||
        (desktopStride & ~(h3u32)SSTG_DST_LINEAR_STRIDE) != 0)
        return 0;
    if (desktopBpp == 16UL)
        fmt = SSTG_PIXFMT_16BPP;
    else if (desktopBpp == 32UL)
        fmt = SSTG_PIXFMT_32BPP;
    else
        return 0;
    return (desktopStride & SSTG_DST_LINEAR_STRIDE) | fmt;
}
