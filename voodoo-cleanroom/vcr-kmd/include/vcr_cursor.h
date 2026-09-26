/*
 * vcr_cursor.h - the Banshee / Voodoo 3 / VSA-100 hardware cursor.
 *
 * 64x64, two bit planes, in video memory at hwCurPatAddr; vidProcCfg bit 27
 * enables it, bit 1 picks the format. The vendor driver runs the MICROSOFT
 * format (bit 1 clear; golden: vidProcCfg 0x090c0081 at the desktop) with
 * hwCurC0 black and hwCurC1 white, which is exactly a Windows monochrome
 * pointer: per pixel (AND, XOR) = (0,0) colour 0, (0,1) colour 1,
 * (1,0) transparent, (1,1) inverted screen.
 *
 * Memory layout (VCR_CURSOR_ROW_BYTES per row, 64 rows): the AND plane's 64
 * bits then the XOR plane's, leftmost pixel in the most significant bit of
 * the first byte - checked against the vendor's pattern for the standard
 * arrow (golden/cursor_*.json).
 */
#ifndef VCR_CURSOR_H
#define VCR_CURSOR_H

#include "vcr_types.h"

#define VCR_CURSOR_DIM          64
#define VCR_CURSOR_ROW_BYTES    16
#define VCR_CURSOR_BYTES        (VCR_CURSOR_DIM * VCR_CURSOR_ROW_BYTES)
#define VCR_CURSOR_OFFSET       64      /* hwCurLoc names the pixel 64 right/below the top-left */

/* Fill out[] from a Windows monochrome pointer (AND rows, then XOR rows, each
 * `stride` bytes, MSB = leftmost). Pixels outside w x h are transparent.
 * 0 when the pointer does not fit the 64x64 cursor. */
int     vcr_cursor_from_mono(const vcr_u8 *and_rows, const vcr_u8 *xor_rows, vcr_u32 w,
                             vcr_u32 h, vcr_u32 stride, vcr_u8 out[VCR_CURSOR_BYTES]);
/* hwCurLoc for a cursor whose top-left pixel is at (x, y); x, y >= -63. */
vcr_u32 vcr_cursor_loc(int x, int y);

#endif /* VCR_CURSOR_H */
