/*
 * vcr_modeorder.h - the order the display driver LISTS its modes in
 * (display/vcrdd.c DrvGetModes). Win32-free: the host tests compile it as is.
 *
 * Only the listing order changes: every mode is still listed, and a mode is
 * still set by its fields and the miniport's own ModeIndex, never by its
 * position in this list.
 *
 * Why the order matters (2026-09-28, .124): GLQuake keeps only the first 30
 * entries of its mode list (gl_vidnt.c MAX_MODE_LIST, 3 of them its windowed
 * modes) and fills it from EnumDisplaySettings in the order the driver gives,
 * skipping < 15 bpp and duplicates. Listed depth-major - every 16 bpp mode,
 * the widescreen and TV ones included, before any 32 bpp one - 1280x960x32
 * was the 31st distinct mode, so the fleet's own "Quake" shortcut
 * (-width 1280 -height 960 -bpp 32) died with "Specified video mode not
 * available". Listing the STANDARD modes (4:3, 5:4 - what a CRT is driven at)
 * first at every depth, and the rest after, puts every standard mode at 16 and
 * 32 bpp inside that window.
 *
 * rank = tier * 65 + bpp: tier 0 = standard, 1 = the rest; within a tier by
 * depth, then in the miniport's order (resolution, then refresh).
 */
#ifndef VCR_MODEORDER_H
#define VCR_MODEORDER_H

#define VCR_MODE_RANKS  130u        /* 2 tiers x bpp 0..64 */

static __inline int vcr_mode_standard(unsigned long w, unsigned long h)
{
    return w * 3 == h * 4 || w * 4 == h * 5;
}

static __inline unsigned vcr_mode_rank(unsigned long w, unsigned long h, unsigned long bpp)
{
    return (vcr_mode_standard(w, h) ? 0u : 65u) + (unsigned)(bpp > 64 ? 64 : bpp);
}

#endif /* VCR_MODEORDER_H */
