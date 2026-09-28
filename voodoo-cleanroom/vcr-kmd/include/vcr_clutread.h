/*
 * vcr_clutread.h - READ one CLUT entry through the dacAddr/dacData pair and
 * put dacAddr back as it was. The miniport runs it for IOCTL_VCR_REG kind
 * VCR_REG_CLUT (vcrmp.c reg_op -> vcrmp_hw.c VcrHwClutRead); Win32-free, so
 * tests/native/test_vcr_kmd_clutread.c runs it against a simulated DAC.
 *
 * Why (2026-09-28, .124): `vcrctl fbshot` photographed Warcraft II's 640x480x8
 * menu in GREYSCALE because the CLUT read was refused. It was not the game's
 * DirectDraw exclusive mode: VCR_ESC_REG goes straight to IOCTL_VCR_REG
 * (vcrdd_escape.c), and reg_op (vcrmp.c) refuses EVERY write without
 * Diag\AllowPoke - and a CLUT read has to WRITE dacAddr first. AllowPoke is 0
 * on a box in use, and it opens every MMIO/VGA write, so it is the wrong key
 * for a read.
 *
 * What makes this read safe enough to need no AllowPoke:
 *   - it runs in the miniport's StartIO, which the video port serialises with
 *     IOCTL_VIDEO_SET_COLOR_REGISTERS and the mode set - every CLUT write the
 *     kernel pair makes (DrvSetPalette, DirectDraw palettes, clut_identity);
 *   - dacData is never written, and dacAddr goes back to what it held;
 *   - every loop is bounded (VCR_CLUT_TRIES), and the entry is taken only if
 *     dacAddr still holds the index after the data read - a writer that moved
 *     it in between is seen, never averaged into the answer.
 * What it cannot guard: a writer OUTSIDE the kernel pair. Glide writes its
 * gamma ramp through its own MMIO mapping, and a Glide write landing between
 * our dacAddr and its dacData would put its value in OUR entry. fbshot reads
 * the CLUT only for an 8 bpp desktop - never while a Glide session owns the
 * overlay - and the kind is refused outright unless Diag\ClutRead = 1 (read
 * at boot; DEFAULT OFF since the integration of 2026-09-28 - no kernel
 * behaviour of this lane is on by default before it has run on the card).
 */
#ifndef VCR_CLUTREAD_H
#define VCR_CLUTREAD_H

#include "vcr_types.h"

#define VCR_CLUT_DACADDR    0x50        /* = VCR_R_DACADDR */
#define VCR_CLUT_DACDATA    0x54        /* = VCR_R_DACDATA */
#define VCR_CLUT_ENTRIES    512         /* two banks of 256 */
#define VCR_CLUT_INDEX_MASK 0x1ffu
#define VCR_CLUT_TRIES      100         /* xf86-video-tdfx / Glide: the pair drops writes */

typedef struct vcr_dac_io {
    void *ctx;
    vcr_u32 (*rd)(void *ctx, vcr_u32 off);      /* memBase0 dword of the master */
    void    (*wr)(void *ctx, vcr_u32 off, vcr_u32 v);
} vcr_dac_io;

/* results */
#define VCR_CLUT_OK         0
#define VCR_CLUT_E_INDEX    1   /* index past the two banks: nothing touched */
#define VCR_CLUT_E_ADDR     2   /* dacAddr never took the index (or kept being moved) */
#define VCR_CLUT_E_RESTORE  4   /* dacAddr would not go back to what it held */

/* ---- who may do what through IOCTL_VCR_REG (vcrmp.c reg_op) ----------------
 * kind: VCR_REG_MMIO32 = 0 ... VCR_REG_VGA_PORT = 5, VCR_REG_CLUT = 6
 * (vcr_ioctl.h). Every WRITE of the old kinds needs Diag\AllowPoke - which is
 * exactly what refused vcrctl's CLUT read: its first step was a dacAddr WRITE.
 * The CLUT kind is a read only: no AllowPoke, chip 0, a Voodoo, 0..511, and
 * Diag\ClutRead = 1 (default 0). */
#define VCR_REGOP_OK        0
#define VCR_REGOP_DENIED    1   /* ERROR_ACCESS_DENIED */
#define VCR_REGOP_INVALID   2   /* ERROR_INVALID_PARAMETER */
#define VCR_REGOP_KIND_CLUT 6   /* = VCR_REG_CLUT */
static __inline int vcr_regop_gate(vcr_u32 kind, vcr_u32 write, vcr_u32 chip, vcr_u32 offset,
                                   int allow_poke, int clut_read, int is_voodoo)
{
    if (kind == VCR_REGOP_KIND_CLUT) {
        if (write || chip != 0 || offset >= VCR_CLUT_ENTRIES || !is_voodoo)
            return VCR_REGOP_INVALID;
        return clut_read ? VCR_REGOP_OK : VCR_REGOP_DENIED;
    }
    if (write && !allow_poke)
        return VCR_REGOP_DENIED;
    return VCR_REGOP_OK;        /* the kind's own checks follow in reg_op */
}

/* dacAddr <- v until it reads back (bounded); 1 = it did */
static __inline int vcr_clut_set_addr(const vcr_dac_io *io, vcr_u32 v)
{
    vcr_u32 tries = 0;
    do {
        io->wr(io->ctx, VCR_CLUT_DACADDR, v);
        if ((io->rd(io->ctx, VCR_CLUT_DACADDR) & VCR_CLUT_INDEX_MASK) == v)
            return 1;
    } while (++tries < VCR_CLUT_TRIES);
    return 0;
}

/* VCR_CLUT_OK: *rgb = entry `index` (0x00RRGGBB) and dacAddr is back.
 * Otherwise a VCR_CLUT_E_* mask; *rgb is untouched. */
static __inline int vcr_clut_read_entry(const vcr_dac_io *io, vcr_u32 index, vcr_u32 *rgb)
{
    vcr_u32 saved, v = 0, tries = 0;
    int got = 0, rc = VCR_CLUT_OK;
    if (index >= VCR_CLUT_ENTRIES)
        return VCR_CLUT_E_INDEX;
    saved = io->rd(io->ctx, VCR_CLUT_DACADDR) & VCR_CLUT_INDEX_MASK;
    do {
        if (!vcr_clut_set_addr(io, index))
            break;
        v = io->rd(io->ctx, VCR_CLUT_DACDATA);
        /* still our index after the read: nobody moved it under us */
        got = (io->rd(io->ctx, VCR_CLUT_DACADDR) & VCR_CLUT_INDEX_MASK) == index;
    } while (!got && ++tries < VCR_CLUT_TRIES);
    if (!got)
        rc |= VCR_CLUT_E_ADDR;
    if (!vcr_clut_set_addr(io, saved))
        rc |= VCR_CLUT_E_RESTORE;
    if (got)
        *rgb = v & 0xffffffu;
    return rc;
}

#endif /* VCR_CLUTREAD_H */
