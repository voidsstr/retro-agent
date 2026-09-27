/*
 * vcr_3dseq.h - the exact 3D register writes the display driver makes to
 * point the engine at a Direct3D target, and to clear what a Glide session
 * left on chip 0 when it gives the chip back. Integer only and Win32-free:
 * display/vcrdd_3d.c and display/vcrdd_2d.c write what these build, and
 * tests/native/test_vcr_kmd_d3dseq.c compiles the same header and asserts
 * the sequences write for write - the Voodoo3 one and the 16 bpp one against
 * what the driver wrote before they existed.
 *
 * vcr_regs.h (the miniport's register map) is included for the status bits
 * and cmdFifo0.baseSize the reset's pre-check reads - one definition of each.
 *
 * Offsets are chip 0's 3D block (BAR0 + 0x200000, vcr_3dregs.h) with the
 * chip field (address bits [13:10]) 0: the FBI and every TMU at once - what
 * 3dfx's h5 Glide calls BROADCAST_ID (fxcmd.h eChipBroadcast = 0) and uses
 * for stencilMode/stencilOp (distate.c), chipMask/aaCtrl/sliCtrl (gsst.c
 * _grChipMask, _grAAOffsetValue, _grDisableSliCtrl) and the FBI combineMode
 * (gtex.c _grTex2ppc). The one exception is the 2PPC flush's nopCMDs, which
 * _grTex2ppc sends to eChipTMU0 | eChipTMU1 only: here they are broadcast
 * (see the reset below).
 */
#ifndef VCR_3DSEQ_H
#define VCR_3DSEQ_H

#include "vcr_3dregs.h"
#include "vcr_rtfmt.h"
#include "vcr_regs.h"

typedef struct vcr_regw {
    unsigned off, val;
} vcr_regw;

/* ---- the target ------------------------------------------------------------------
 * VSA-100: renderMode. A 32 bpp target then writes stencilMode = stencilOp =
 * 0 (SST_STENCIL_MODE_DISABLE: test off, write mask 0 - h3defs.h: "we must
 * also clear the write mask, to avoid reading or writing the stencil planes
 * when stencil is disabled"; 3dfx's V5 D3D HAL does the same at every
 * context and clear): its aux buffer is 24 bits of depth under 8 of LIVE
 * stencil. A 16 bpp target does NOT write them: the 16 bpp lane (40/40 on
 * .124) keeps the exact sequence it was proven with - renderMode + the six
 * writes below, whatever Diag\D3D32 says (4a9793b put the pair on every
 * VSA-100 target, 16 bpp included - new traffic on the proven lane with no
 * switch; review 2026-09-27). Its 16-bit aux buffer holds no stencil byte,
 * and the chip is ASSUMED to ignore stencilMode at 16 bpp - UNPROVEN: Glide
 * programs stencilMode with no pixel-size check (distate.c), and stencil
 * state outlives even a clean Glide close (below). If the VSA-100 does honour
 * a stale SST_STENCIL_ENABLE at 16 bpp, a 16 bpp D3D session after a 32 bpp
 * OpenGL/Glide one that used stencil fails or rewrites its pixels; the
 * remedy then is Diag\Reset3D (or a D3D-only stencil clear behind its own
 * switch), not a change to this default. The supervised check: a 32 bpp
 * OpenGL session with stencil on, then d3dprobe render --full at 16 bpp.
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
        if (fmt == VCR_RT_32) {
            w[n].off = V3D_STENCILMODE; w[n++].val = 0;
            w[n].off = V3D_STENCILOP;   w[n++].val = 0;
        }
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
 *   chipMask    = ALL FIRST: a chip whose bit is clear ignores the writes
 *                     after it (Glide narrows it per chip - _grAAOffsetValue,
 *                     _grDisableSliCtrl and the AA tLOD writes use
 *                     _grChipMask(1 << chip) - and a client killed in between
 *                     leaves it that way), and chipMask itself is always
 *                     taken. SST_CHIP_MASK_ALL_CHIPS (0xFFFFFFFF, h3defs.h) is
 *                     what Glide's own init and clean close write
 *                     (assertDefaultState, grSstWinClose) - the state every
 *                     proven D3D run so far started from - and it does not
 *                     depend on which ID chip 0 answers to after an AA
 *                     session whose teardown never ran. With snooping off
 *                     (the mode set before this turned SLI off) only chip 0
 *                     sees any of these writes. (4a9793b wrote 1: right only
 *                     while chip 0 still identifies as chip 0.)
 *   sliCtrl     = 0   SECOND, and chip 0's only: the mode set before this ran
 *                     the miniport's SLI disable, which writes sliCtrl = 0 per
 *                     chip - but WITHOUT a chipMask first, so a chip 0 a killed
 *                     client left masked out ignored it, kept SLI_ENABLE and
 *                     its compare mask, and every later D3D context would draw
 *                     only chip 0's scanline bands. The same value the
 *                     miniport writes; never a non-zero one (SLI stays the
 *                     miniport's).
 *   nopCMD x 12       Glide's rule before leaving two-pixels-per-clock
 *                     (gtex.c _grTex2ppc: "flush the tmu pipeline going from
 *                     2ppc to 1ppc by sending 12 nopCMD"). Glide sends those
 *                     to eChipTMU0 | eChipTMU1; these are BROADCAST (chip
 *                     field 0), which reaches both TMUs and the FBI - nopCMD 0
 *                     broadcast is what grFlush and grSstIsBusy send (gsst.c),
 *                     and what the HAL's own texture flush sends, so it is no
 *                     new traffic for the FBI. The value 0:
 *                     SST_NOP_RESET_*_STATS clear. Harmless when 2PPC was
 *                     already off.
 *   combineMode = 0   FBI and TMUs: SST_CM_USE_COMBINE_MODE off (the legacy
 *                     fbzColorPath / textureMode combine the HAL programs),
 *                     SST_CM_ENABLE_TWO_PIXELS_PER_CLOCK off. 0 is Glide's
 *                     own TMU shadow at init (gsst.c).
 *   aaCtrl      = 0   no AA_ENABLE, no jitter offsets - what the vendor HAL
 *                     sets per context and a single-sample Glide leaves.
 *   stencilMode = 0, stencilOp = 0   as at every 32 bpp target setup above,
 *                     here for whatever target comes next.
 *
 * Nothing here is written to a slave chip, and nothing but sliCtrl = 0
 * touches SLI.
 *
 * WHEN it runs (display/vcrdd_escape.c): only at the OWNER's own
 * HWCRLSEXCLUSIVE, after its RESTORE_MODE succeeded (that mode set is what
 * turned SLI and snooping off). Never where DrvAssertMode(TRUE) clears an
 * owner that did not release: a display driver cannot tell a killed client
 * from a live one that is only switched away (an alt-tabbed DirectDraw-
 * exclusive Glide game re-asserts its PDEV and resumes its command FIFO), so
 * a Glide client that was killed still leaves its state - the cold boot
 * before a 32 bpp D3D test stands for that case until the miniport can say
 * whether the old owner's process has exited.
 *
 * And only on an IDLE chip, checked BEFORE the first write (the gate below):
 * Glide's hwcRestoreVideo sends its release even when its own idle wait
 * failed, skipping its register restore and leaving its command FIFO on; the
 * kernel's RESTORE_MODE then resets a busy engine but reports success even
 * when that reset leaves it "STILL BUSY". Queuing writes into that chip is
 * the stuck-chip case this must not touch: a chip that is not idle, or whose
 * command FIFO is still enabled (cmdFifo0.baseSize SST_CMDFIFOEN, h3defs.h
 * bit 8), gets NOTHING written and the refusal is logged. */
#define VCR_3D_RESET_NOPS   12
#define VCR_3D_RESET_MAX    (2 + VCR_3D_RESET_NOPS + 4)
#define VCR_3D_CHIPMASK_ALL 0xffffffffu     /* h3defs.h SST_CHIP_MASK_ALL_CHIPS */

/* the reset's outcome (event 604 a=3, c) */
#define VCR_R3D_OFF         0u      /* not armed, Direct3D off, not a VSA-100, no engine */
#define VCR_R3D_DONE        1u      /* written, and the chip settled */
#define VCR_R3D_BUSY        2u      /* not idle before the first write: nothing written */
#define VCR_R3D_CMDFIFO     3u      /* Glide's command FIFO still on: nothing written */
#define VCR_R3D_GAVEUP      4u      /* FIFO room or the settle ran out mid-reset: accel off */

/* idle, one status read at a time - the rule VcrDd2dSync has always used:
 * the engine not busy AND the PCI FIFO back at its full free count (learned
 * at init, not assumed), VCR_IDLE_READS reads in a row (busy can drop for a
 * cycle between two queued operations). Returns the new run length. */
#define VCR_IDLE_READS      3u
#define VCR_CMDFIFO_EN      (1u << 8)   /* h3defs.h SST_CMDFIFOEN, in cmdFifo0.baseSize */

static __inline unsigned vcr_idle_run(unsigned status, unsigned fifo_full, unsigned run)
{
    return ((status & VCR_STATUS_BUSY) || (status & VCR_STATUS_FIFOLEVEL_MASK) < fifo_full)
           ? 0u : run + 1u;
}

/* before the reset's first write: 0 = go ahead, else why nothing is written
 * (idle_run: the longest run the bounded pre-check saw; cmd_basesize: chip
 * 0's cmdFifo0.baseSize, read once the chip read idle) */
static __inline unsigned vcr_3d_reset_blocked(unsigned idle_run, unsigned cmd_basesize)
{
    if (idle_run < VCR_IDLE_READS)
        return VCR_R3D_BUSY;
    if (cmd_basesize & VCR_CMDFIFO_EN)
        return VCR_R3D_CMDFIFO;
    return 0;
}

static __inline unsigned vcr_3d_glide_reset_seq(vcr_regw *w)
{
    unsigned n = 0, i;
    w[n].off = V3D_CHIPMASK;    w[n++].val = VCR_3D_CHIPMASK_ALL;
    w[n].off = V3D_SLICTRL;     w[n++].val = 0;
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
