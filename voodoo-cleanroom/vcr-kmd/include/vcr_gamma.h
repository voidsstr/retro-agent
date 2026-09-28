/*
 * vcr_gamma.h - GDI's gamma ramp as the VSA-100's colour table
 * (display/vcrdd.c DrvIcmSetDeviceGammaRamp). Win32-free:
 * tests/native/test_vcr_kmd_gamma.c compiles it as is.
 *
 * Why (2026-09-28, the LAN-party pass on .124): Jedi Academy logged
 * "SetDeviceGammaRamp failed." at every start. Our display driver never
 * offered GCAPS2_CHANGEGAMMARAMP, so win32k refused every SetDeviceGammaRamp
 * and every game that sets its brightness that way - the id Tech 3 family
 * (Jedi Academy, RtCW, ioquake3), UT's OpenGLDrv - ran with no hardware gamma:
 * id Tech 3 then drops to software gamma and forces r_overBrightBits to 0,
 * which is the dark picture. The chip already reads its colour table at
 * 16 and 32 bpp (vidProcCfg: CLUT bank 0, not bypassed - common/vcr_modes.c),
 * and IOCTL_VIDEO_SET_COLOR_REGISTERS writes it at any depth, so a ramp is
 * only a conversion away. A fullscreen Glide game is scanned out through the
 * overlay, which reads the same bank 0 (Glide's own grLoadGammaTable writes
 * the same registers), so its ramp reaches the screen too.
 *
 * GDI's ramp (IGRF_RGB_256WORDS) is 3 x 256 16-bit words, red then green then
 * blue. The DAC takes 8 bits a channel: the HIGH byte of each word (0xffff is
 * 0xff, not 0xff & 0xffff = 0xff by accident only at the top of the ramp).
 * At 8 bpp the colour table IS the palette, so a ramp is refused there
 * (GDI does not offer one at 8 bpp either without GCAPS2_CHANGEGAMMARAMP).
 */
#ifndef VCR_GAMMA_H
#define VCR_GAMMA_H

#define VCR_GAMMA_ENTRIES 256u

/* entry i of the ramp as the 0x00RRGGBB the DAC data register takes */
static __inline unsigned long vcr_gamma_entry(const unsigned short *ramp, unsigned i)
{
    return ((unsigned long)(ramp[i] >> 8) << 16) |
           ((unsigned long)(ramp[VCR_GAMMA_ENTRIES + i] >> 8) << 8) |
           (unsigned long)(ramp[2 * VCR_GAMMA_ENTRIES + i] >> 8);
}

/* May a ramp be loaded at this depth? Only where the colour table is not
 * the palette. */
static __inline int vcr_gamma_depth_ok(unsigned bpp)
{
    return bpp == 16 || bpp == 24 || bpp == 32;
}

#endif /* VCR_GAMMA_H */
