/*
 * vcr_fbshot.h - turn what the video processor is SCANNING OUT into pixels
 * (tools/vcrctl.c `fbshot`). Win32-free: tests/native/test_vcr_kmd_fbshot.c
 * compiles it as is.
 *
 * Why (2026-09-28, the LAN-party sweep on .124): the agent's SCREENSHOT is a
 * GDI BitBlt of the desktop surface. On our driver an exclusive-fullscreen
 * Glide, OpenGL or Direct3D game renders into buffers of its own, so GDI
 * photographs the old desktop memory reinterpreted - garbage or black - and a
 * sweep can only say "still running". The scanout registers say which memory
 * is on the monitor: vidDesktopStartAddr (a flip writes it), the stride, the
 * size and the pixel format in vidProcCfg. Reading that memory is the frame.
 *
 * Scope, stated so a picture is not over-read:
 *   - the DESKTOP layer only. An overlay (vidProcCfg bit 8 - DOSBox's
 *     `output=overlay`) is reported, not composited.
 *   - one chip. In SLI each chip holds only its own bands, so a 4-chip Glide
 *     frame read from the master shows every 4th band; the caller says so.
 *   - 8 bpp through the CLUT bank vidProcCfg selects; 16/32 bpp raw (the
 *     identity/gamma CLUT is not applied).
 *
 * Tiles (Banshee/Voodoo3/VSA-100): 128 bytes x 32 lines, row-major, and in
 * tiled mode the stride register counts TILES (bits 6:0), not bytes.
 */
#ifndef VCR_FBSHOT_H
#define VCR_FBSHOT_H

#define VCR_FB_TILE_W       128u       /* bytes */
#define VCR_FB_TILE_H       32u        /* lines */

/* bytes per pixel for vidProcCfg's desktop format (bits 20:18), 0 = unknown */
static __inline unsigned vcr_fb_bytespp(unsigned fmt)
{
    switch (fmt) {
    case 0: return 1;       /* 8 bpp palettized */
    case 1: return 2;       /* RGB565 */
    case 2: return 3;       /* RGB24 */
    case 3: return 4;       /* RGB32 */
    default: return 0;
    }
}

/* the row pitch in bytes the stride register means: bytes when linear,
 * tiles (bits 6:0) x 128 when tiled */
static __inline unsigned long vcr_fb_pitch(unsigned long stride_reg, int tiled)
{
    return tiled ? (stride_reg & 0x7fu) * VCR_FB_TILE_W : (stride_reg & 0x7fffu);
}

/* the byte offset, from the start address, of byte `xb` of line `y` */
static __inline unsigned long vcr_fb_offset(unsigned long xb, unsigned long y,
                                            unsigned long stride_reg, int tiled)
{
    if (!tiled)
        return y * (stride_reg & 0x7fffu) + xb;
    return ((y / VCR_FB_TILE_H) * (stride_reg & 0x7fu) + xb / VCR_FB_TILE_W) *
               (VCR_FB_TILE_W * VCR_FB_TILE_H) +
           (y % VCR_FB_TILE_H) * VCR_FB_TILE_W + xb % VCR_FB_TILE_W;
}

/* the last byte the frame touches, +1: the caller bounds its reads with it */
static __inline unsigned long vcr_fb_extent(unsigned w, unsigned h, unsigned bpp,
                                            unsigned long stride_reg, int tiled)
{
    if (!w || !h || !bpp)
        return 0;
    if (!tiled)
        return (unsigned long)(h - 1) * (stride_reg & 0x7fffu) + (unsigned long)w * bpp;
    return ((unsigned long)((h - 1) / VCR_FB_TILE_H) + 1) * (stride_reg & 0x7fu) *
           VCR_FB_TILE_W * VCR_FB_TILE_H;
}

/* one pixel's bytes (little-endian, as the chip stores them) -> 0xRRGGBB */
static __inline unsigned long vcr_fb_rgb(unsigned fmt, const unsigned char *p,
                                         const unsigned long *clut)
{
    unsigned long v, r, g, b;
    switch (fmt) {
    case 0:
        return clut ? clut[p[0]] & 0xffffffUL : (unsigned long)p[0] * 0x010101UL;
    case 1:
        v = p[0] | (unsigned long)p[1] << 8;
        r = v >> 11 & 31; g = v >> 5 & 63; b = v & 31;
        return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    case 2:
        return (unsigned long)p[2] << 16 | (unsigned long)p[1] << 8 | p[0];
    case 3:
        return (unsigned long)p[2] << 16 | (unsigned long)p[1] << 8 | p[0];
    default:
        return 0;
    }
}

#endif /* VCR_FBSHOT_H */
