/*
 * vcr_cursor.c - see include/vcr_cursor.h.
 */
#include "../include/vcr_cursor.h"

int vcr_cursor_from_mono(const vcr_u8 *and_rows, const vcr_u8 *xor_rows, vcr_u32 w,
                         vcr_u32 h, vcr_u32 stride, vcr_u8 out[VCR_CURSOR_BYTES])
{
    vcr_u32 y, i;
    if (w > VCR_CURSOR_DIM || h > VCR_CURSOR_DIM || stride * 8 < w)
        return 0;
    for (y = 0; y < VCR_CURSOR_DIM; y++) {
        vcr_u8 *row = out + y * VCR_CURSOR_ROW_BYTES;
        for (i = 0; i < 8; i++) {
            vcr_u8 a = 0xff, x = 0x00;           /* transparent */
            if (y < h && i * 8 < w) {
                vcr_u32 n = w - i * 8;           /* pixels of this byte inside w */
                vcr_u8 keep = n >= 8 ? 0xff : (vcr_u8)(0xff << (8 - n));
                a = (vcr_u8)((and_rows[y * stride + i] & keep) | (vcr_u8)~keep);
                x = (vcr_u8)(xor_rows[y * stride + i] & keep);
            }
            row[i] = a;
            row[8 + i] = x;
        }
    }
    return 1;
}

vcr_u32 vcr_cursor_loc(int x, int y)
{
    int cx = x + VCR_CURSOR_OFFSET, cy = y + VCR_CURSOR_OFFSET;
    if (cx < 0)
        cx = 0;
    if (cy < 0)
        cy = 0;
    return ((vcr_u32)(cy & 0x7ff) << 16) | (vcr_u32)(cx & 0x7ff);
}
