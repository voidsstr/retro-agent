/*
 * gbk_layout.c - board-memory carve-out math (pure integer).
 * Design: docs/3dfx-gbkernel-design.md sec 1a.
 * Lift sources: hwcAllocBuffers (H3/minihwc/minihwc.c:1397-1610),
 * calcBufferStride/calcBufferSize (:3678-3745), min-memory check
 * (gsst.c:1043-1048, terms :953-954). Our variant shifts glide's
 * fullscreen layout up past the GDI desktop:
 *   [0,desktopEnd) GDI -> tram -> FIFO -> color buffer(s) -> aux at top
 * with glide's exact page-PARITY alignment (bit 12 of the start address:
 * color buffers on even 4K pages, aux on odd - a DRAM-bank interleave
 * optimization, minihwc.c:1455-1461,1489-1492). LINEAR buffers only.
 * Kernel-clean: C89, no CRT, no floats.
 */
#include "gbk.h"

/* linear stride = xres * 2 bytes (calcBufferStride, minihwc.c:3691-3693) */
h3u32
gbk_calc_stride(h3u32 xres)
{
    return xres << 1;
}

/* linear buffer size = stride * yres, NOT page-rounded (calcBufferSize,
 * minihwc.c:3725-3745: "stride = xres << 1; height = yres;
 * bufSize = stride * height"). Page discipline comes from the parity
 * adjustment during allocation, never from the size itself.            */
h3u32
gbk_calc_buf_size(h3u32 xres, h3u32 yres)
{
    return gbk_calc_stride(xres) * yres;
}

/* Full carve-out per hwcAllocBuffers (minihwc.c:1397-1610), desktop-shifted.
 * All subtractions are guarded: on any underflow we return failure rather
 * than emit a wrapped offset (a bogus offset becomes a FIFO scribble over
 * the desktop => kernel hang; design sec 6 risk 2).                     */
int
gbk_layout_compute(gbk_layout_t *out,
                   h3u32 vramBytes, h3u32 desktopEnd,
                   h3u32 xres, h3u32 yres,
                   h3u32 nColBuffers, h3u32 nAuxBuffers,
                   h3u32 minTramBytes)
{
    h3u32 bufSize, top, fifoSize, fbOffset, tramOffset;
    h3u32 i;

    /* echo inputs, poison results so a failure can't be misread */
    out->vramBytes  = vramBytes;
    out->desktopEnd = desktopEnd;
    out->xres = xres;
    out->yres = yres;
    out->tramOffset = 0;
    out->tramSize = 0;
    out->fifoOffset = 0;
    out->fifoLength = 0;
    out->stride  = gbk_calc_stride(xres);
    out->bufSize = gbk_calc_buf_size(xres, yres);
    out->nColBuffers = nColBuffers;
    for (i = 0; i < GBK_MAX_COL_BUFFERS; i++)
        out->colBufOffset[i] = 0;
    out->nAuxBuffers = nAuxBuffers;
    out->auxOffset = 0;

    /* parameter sanity (keeps all math well inside 32 bits) */
    if (xres == 0 || yres == 0 || xres > 2048 || yres > 2048)
        return -1;
    if (nColBuffers < 1 || nColBuffers > GBK_MAX_COL_BUFFERS)
        return -1;
    if (nAuxBuffers > 1)
        return -1;
    if (vramBytes < (1UL << 20) || vramBytes > (256UL << 20))
        return -1;
    if (desktopEnd > vramBytes)
        return -1;
    if (minTramBytes == 0)
        minTramBytes = H3HW_MIN_TEXTURE_STORE;

    bufSize = out->bufSize;

    /* minimum-memory check (grSstWinOpen gsst.c:1043-1048):
     * bufSize*(nCol+nAux) + MIN_TEXTURE_STORE + MIN_FIFO_SIZE must fit.
     * Our variant also charges the GDI desktop region.                  */
    if (bufSize * (nColBuffers + nAuxBuffers)
            + H3HW_MIN_TEXTURE_STORE + H3HW_MIN_FIFO_SIZE + desktopEnd
        > vramBytes)
        return -1;

    /* aux buffer at the very top, odd-page parity (minihwc.c:1449-1464:
     * start = h3Mem - bufSize; if ((start & 0x1000) == 0) start -= 0x1000) */
    top = vramBytes;
    if (nAuxBuffers > 0) {
        if (top < bufSize + H3HW_PAGE)
            return -1;
        top -= bufSize;
        if ((top & 0x1000UL) == 0)
            top -= H3HW_PAGE;
        out->auxOffset = top;
    }

    /* color buffers top-down below that, even-page parity
     * (minihwc.c:1466-1492: start -= bufSize;
     *  if ((start & 0x1000) != 0) start -= 0x1000)                      */
    for (i = nColBuffers; i-- > 0; ) {
        if (top < bufSize + H3HW_PAGE)
            return -1;
        top -= bufSize;
        if ((top & 0x1000UL) != 0)
            top -= H3HW_PAGE;
        out->colBufOffset[i] = top;
    }
    fbOffset = out->colBufOffset[0];       /* minihwc.c:1497 */

    /* tram base: glide uses 0 (minihwc.c:1520); our variant starts above
     * the GDI desktop, rounded up to a 4K page (satisfies the 16-byte
     * texBaseAddr granularity, h3defs.h:561, with page discipline).     */
    tramOffset = (desktopEnd + (H3HW_PAGE - 1)) & ~(h3u32)(H3HW_PAGE - 1);

    /* FIFO between tram and fbOffset (minihwc.c:1521-1541):
     * fifoSize = h3Mem > 8MB ? MAXFIFOSIZE_16MB : MAXFIFOSIZE;
     * tramSize = fbOffset - tramOffset - fifoSize;
     * shrink the FIFO if tram would fall below the minimum.             */
    fifoSize = (vramBytes > (8UL << 20)) ? H3HW_MAXFIFOSIZE_16MB
                                         : H3HW_MAXFIFOSIZE;

    if (fbOffset < tramOffset + fifoSize
        || fbOffset - tramOffset - fifoSize < minTramBytes) {
        /* "Now we have to shrink the FIFO" (minihwc.c:1526-1537) */
        if (fbOffset < tramOffset + minTramBytes)
            return -1;                     /* fifoSize < 0 -> fail       */
        fifoSize = fbOffset - tramOffset - minTramBytes;
        out->tramSize = minTramBytes;
    } else {
        out->tramSize = fbOffset - tramOffset - fifoSize;
    }

    /* fifoLength -= 0x2000 pad below the color buffer (minihwc.c:1541);
     * the enable write needs at least one 4K page left (baseSize =
     * (len>>12)-1, minihwc.c:1658) plus the END_ADJUST slack.           */
    if (fifoSize < H3HW_FIFO_RESERVE + H3HW_PAGE)
        return -1;
    out->tramOffset = tramOffset;
    out->fifoOffset = fbOffset - fifoSize; /* minihwc.c:1540 */
    out->fifoLength = fifoSize - H3HW_FIFO_RESERVE;

    return gbk_layout_validate(out);       /* never hand out an overlap  */
}

/* Enforce desktopEnd <= tram < fifo < colBuf[0] < .. < aux <= vram and
 * pairwise non-overlap (design sec 6 risk 2). The FIFO region is charged
 * its full fifoLength + 0x2000 reserve (the hardware may prefetch into
 * the pad; nothing else must live there). Returns 0 if sane.            */
int
gbk_layout_validate(const gbk_layout_t *l)
{
    h3u32 i, prevEnd;

    if (l->nColBuffers < 1 || l->nColBuffers > GBK_MAX_COL_BUFFERS)
        return -1;

    /* desktop -> tram */
    if (l->tramOffset < l->desktopEnd)
        return -1;
    /* tram -> fifo */
    if (l->fifoOffset < l->tramOffset
        || l->fifoOffset - l->tramOffset < l->tramSize)
        return -1;
    /* fifo (+ reserve pad) -> colBuf[0] */
    if (l->colBufOffset[0] < l->fifoOffset
        || l->colBufOffset[0] - l->fifoOffset
           < l->fifoLength + H3HW_FIFO_RESERVE)
        return -1;
    /* color buffers ascending, bufSize apart, even-page parity */
    prevEnd = l->colBufOffset[0];
    for (i = 0; i < l->nColBuffers; i++) {
        if ((l->colBufOffset[i] & 0x1000UL) != 0)
            return -1;
        if (i > 0) {
            if (l->colBufOffset[i] < prevEnd)
                return -1;
        }
        if (l->colBufOffset[i] > 0xFFFFFFFFUL - l->bufSize)
            return -1;
        prevEnd = l->colBufOffset[i] + l->bufSize;
    }
    /* last col -> aux (odd parity) -> vram, or straight to vram */
    if (l->nAuxBuffers > 0) {
        if ((l->auxOffset & 0x1000UL) != 0x1000UL)
            return -1;
        if (l->auxOffset < prevEnd)
            return -1;
        prevEnd = l->auxOffset + l->bufSize;
    }
    if (prevEnd > l->vramBytes)
        return -1;

    return 0;
}

/* Readback rect -> row count solver for gbkernel_dbg_readback (FXDBG_READBACK).
 * x,y,w,h are ATTACKER-CONTROLLED (straight off the ExtEscape input struct), so
 * every bound is overflow-safe. The extent clamp compares against the REMAINING
 * extent (uw-x, uh-y) rather than forming x+w / y+h: the latter wrap at 2^32
 * (e.g. y=1,h=0xFFFFFFFF -> y+h=0 <= uh) and would leave h unclamped, marching
 * the read loop past the 32MB BAR1 aperture (kernel OOB read: info-leak or
 * bugcheck). Row cap uses the CLAMPED width so the destination byte count and
 * the framebuffer row walk agree. Kernel-clean: C89, integer only. */
h3u32
gbk_readback_rows(h3u32 x, h3u32 y, h3u32 w, h3u32 h,
                  h3u32 uw, h3u32 uh, h3u32 outMax, h3u32 *clampedW)
{
    h3u32 maxRows;

    if (clampedW != 0)
        *clampedW = 0;
    /* need a real surface, a start inside it, a non-empty rect, room for >=1px */
    if (uw == 0 || uh == 0 || outMax < 2)
        return 0;
    if (x >= uw || y >= uh)
        return 0;
    if (w == 0 || h == 0)
        return 0;
    /* x < uw and y < uh (just checked) => uw-x and uh-y are >= 1 and cannot
     * underflow; `w > uw - x` cannot overflow (unlike `x + w > uw`). */
    if (w > uw - x) w = uw - x;
    if (h > uh - y) h = uh - y;
    /* cap rows to the output buffer (2 bytes/pixel). w is now <= uw, so w*2
     * cannot overflow a 32-bit product for any real surface width, and w >= 1
     * so the divisor is never 0. */
    maxRows = outMax / (w * 2u);
    if (maxRows == 0)
        return 0;
    if (h > maxRows)
        h = maxRows;
    if (clampedW != 0)
        *clampedW = w;
    return h;
}
