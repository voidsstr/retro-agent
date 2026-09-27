/*
 * vcr_3dseq.h - the exact 3D register writes the display driver makes to
 * point the engine at a Direct3D target, and to clear what a Glide session
 * left on chip 0 when it gives the chip back. Integer only and Win32-free:
 * display/vcrdd_3d.c and display/vcrdd_2d.c write what these build, and
 * tests/native/test_vcr_kmd_d3dseq.c compiles the same header and asserts
 * the sequences write for write - the Voodoo3 one and the 16 bpp one against
 * what the driver wrote before they existed.
 *
 * Offsets are chip 0's 3D block (BAR0 + 0x200000, vcr_3dregs.h) with the
 * chip field (address bits [13:10]) 0: the FBI and every TMU at once - what
 * 3dfx's h5 Glide calls BROADCAST_ID (fxcmd.h eChipBroadcast = 0) and uses
 * for every register below (distate.c stencilMode/stencilOp, gsst.c
 * _grChipMask/_grAAOffsetValue, gtex.c _grTex2ppc's FBI combineMode).
 */
#ifndef VCR_3DSEQ_H
#define VCR_3DSEQ_H

#include "vcr_3dregs.h"
#include "vcr_rtfmt.h"

typedef struct vcr_regw {
    unsigned off, val;
} vcr_regw;

/* ---- the target ------------------------------------------------------------------
 * VSA-100: renderMode, then stencilMode = stencilOp = 0 (SST_STENCIL_MODE_
 * DISABLE: test off, write mask 0 - h3defs.h: "we must also clear the write
 * mask, to avoid reading or writing the stencil planes when stencil is
 * disabled"; 3dfx's V5 D3D HAL does the same at every context and clear).
 * Then the buffers and the clip, as a Banshee/Voodoo3 has always written
 * them. Returns the number of writes, 0 when the format is refused. */
#define VCR_3D_TARGET_MAX   9

static __inline unsigned vcr_3d_target_seq(unsigned napalm, unsigned rt32, unsigned fmt,
                                           unsigned rt_off, unsigned rt_pitch,
                                           unsigned z_off, unsigned z_pitch,
                                           unsigned width, unsigned height, vcr_regw *w)
{
    unsigned n = 0;
    if (!vcr_rt_programmable(napalm && rt32, fmt))
        return 0;
    if (napalm) {
        w[n].off = V3D_RENDERMODE;  w[n++].val = vcr_rt_rendermode(fmt);
        w[n].off = V3D_STENCILMODE; w[n++].val = 0;
        w[n].off = V3D_STENCILOP;   w[n++].val = 0;
    }
    w[n].off = V3D_COLBUFFERADDR;   w[n++].val = rt_off;
    w[n].off = V3D_COLBUFFERSTRIDE; w[n++].val = BS_LINEAR_STRIDE(rt_pitch);
    w[n].off = V3D_AUXBUFFERADDR;   w[n++].val = z_off;
    w[n].off = V3D_AUXBUFFERSTRIDE; w[n++].val = BS_LINEAR_STRIDE(z_pitch ? z_pitch : rt_pitch);
    w[n].off = V3D_CLIPLEFTRIGHT;   w[n++].val = (0u << 16) | (width & 0xfff);
    w[n].off = V3D_CLIPBOTTOMTOP;   w[n++].val = (0u << 16) | (height & 0xfff);
    return n;
}

/* ---- after a Glide session (VSA-100 only) ------------------------------------------
 * A Glide client that closes cleanly puts most of this back itself (gsst.c
 * grSstWinClose: _grChipMask(ALL), _grTex2ppc(FALSE), _grAAOffsetValue when
 * it anti-aliased, _grDisableSliCtrl); one that was killed, or wedged, does
 * not - and stencil state and an extended colour combine outlive even a clean
 * close. What the next Direct3D context would inherit, and what it assumes:
 *
 *   chipMask    = 1   FIRST: a chip whose bit is clear ignores the writes
 *                     after it (Glide's _grAAOffsetValue walks the chips with
 *                     _grChipMask(1 << chip)), and chipMask itself is always
 *                     taken. 1 = chip 0, the only chip the HAL draws on (the
 *                     vendor V5 HAL's own 1-unit value).
 *   nopCMD x 12       Glide's rule before leaving two-pixels-per-clock
 *                     (gtex.c _grTex2ppc: "flush the tmu pipeline going from
 *                     2ppc to 1ppc by sending 12 nopCMD"); its own init runs
 *                     the same sequence to reach a known state (gsst.c). The
 *                     value 0: SST_NOP_RESET_*_STATS clear. Harmless when 2PPC
 *                     was already off - the HAL's texture flush sends one.
 *   combineMode = 0   FBI and TMUs: SST_CM_USE_COMBINE_MODE off (the legacy
 *                     fbzColorPath / textureMode combine the HAL programs),
 *                     SST_CM_ENABLE_TWO_PIXELS_PER_CLOCK off. 0 is Glide's
 *                     own TMU shadow at init (gsst.c).
 *   aaCtrl      = 0   no AA_ENABLE, no jitter offsets - what the vendor HAL
 *                     sets per context and a single-sample Glide leaves.
 *   stencilMode = 0, stencilOp = 0   as at every target setup above.
 *
 * sliCtrl is NOT here: the miniport owns SLI (every mode set runs VcrSliOff,
 * RESTORE_MODE included, before this runs). Nothing here is written to a
 * slave chip. */
#define VCR_3D_RESET_NOPS   12
#define VCR_3D_RESET_MAX    (1 + VCR_3D_RESET_NOPS + 4)

static __inline unsigned vcr_3d_glide_reset_seq(vcr_regw *w)
{
    unsigned n = 0, i;
    w[n].off = V3D_CHIPMASK;    w[n++].val = 1;
    for (i = 0; i < VCR_3D_RESET_NOPS; i++) {
        w[n].off = V3D_NOPCMD;  w[n++].val = 0;
    }
    w[n].off = V3D_COMBINEMODE; w[n++].val = 0;
    w[n].off = V3D_AACTRL;      w[n++].val = 0;
    w[n].off = V3D_STENCILMODE; w[n++].val = 0;
    w[n].off = V3D_STENCILOP;   w[n++].val = 0;
    return n;
}

#endif /* VCR_3DSEQ_H */
