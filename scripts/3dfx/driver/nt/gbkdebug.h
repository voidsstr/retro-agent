/*
 * gbkdebug.h - the fxD3D on-card BRING-UP LADDER contract (M4c-1).
 *
 * A private DrvEscape opcode namespace the retro agent drives from user mode
 * (ExtEscape on the display DC, via a tiny tool at M4d) to validate the
 * kernel-mode Glide backend (gbkernel.c) on REAL Voodoo3 hardware ONE RUNG AT A
 * TIME - each rung proves a deeper layer of the transport and NONE of them
 * needs Direct3D. This is the safety net the design demands (docs/
 * 3dfx-gbkernel-design.md sec 6 "Bring-up ladder"): every card-touch in
 * gbkernel.c is tagged "M4d: validated on-card via the escape ladder", and this
 * is that ladder. It runs before D3D ever drives the backend.
 *
 * Two sides share this header:
 *   - gbkdebug.c  : the DrvEscape handler that unpacks the escape buffers
 *                   (needs the DDK: windows.h + winddi.h).
 *   - gbkernel.c  : the gbkernel_dbg_* implementations that touch g_dev / the
 *                   card (needs the gbk transport, no DDK headers).
 * So this header is DDK-agnostic: only fixed-width `unsigned long` fields
 * (== ULONG, 32-bit on the i386 target), C89, no Win32 / no gbk types.
 *
 * Escape ABI: the agent tool calls
 *     ExtEscape(hdc, FXDBG_<rung>, cjIn, pvIn, cjOut, pvOut)
 * and reads back the fixed struct below. Opcodes live in a private 0x3DF0..
 * range so they never collide with a standard GDI escape.
 */
#ifndef FXD3D_GBKDEBUG_H
#define FXD3D_GBKDEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- private escape opcodes (bring-up ladder rungs) ---------------------- */
#define FXDBG_ESC_BASE   0x00003DF0UL
#define FXDBG_PROBE      (FXDBG_ESC_BASE + 0x0UL)   /* 0x3DF0 */
#define FXDBG_CLEAR      (FXDBG_ESC_BASE + 0x1UL)   /* 0x3DF1 */
#define FXDBG_TRI        (FXDBG_ESC_BASE + 0x2UL)   /* 0x3DF2 */
#define FXDBG_TEX        (FXDBG_ESC_BASE + 0x3UL)   /* 0x3DF3 */
#define FXDBG_READBACK   (FXDBG_ESC_BASE + 0x4UL)   /* 0x3DF4 */

/* echoed into every result struct so the agent can confirm it reached OUR
 * DrvEscape (and not some other driver's) - ASCII "FXDB". */
#define FXDBG_MAGIC      0x46584442UL

/* readback source selector (FXDBG_READBACK.which) */
#define FXDBG_RB_BACK      0UL   /* the back 3D color buffer (fresh render)   */
#define FXDBG_RB_DESKTOP   1UL   /* the desktop primary at offset 0 (post-swap)*/

/* ------------------------------------------------------------------------- *
 * FXDBG_PROBE (rung 1): no draw, no FIFO submit. Reads BAR0 status + cmdFifo0
 * registers and returns the computed layout + attach/fault flags - proves the
 * BAR0 mapping and the CMDFIFO init WITHOUT touching the draw path.
 * pvIn: ignored. pvOut: fxdbg_probe_t. returns sizeof(fxdbg_probe_t) or 0.
 * ------------------------------------------------------------------------- */
typedef struct fxdbg_probe {
    unsigned long magic;         /* FXDBG_MAGIC                              */
    unsigned long attached;      /* g_dev.attached                          */
    unsigned long faulted;       /* g_dev.faulted (FIFO wedge latched)       */
    unsigned long status3d;      /* SstRegs.status  (BAR0+0x200000+0x000)    */
    unsigned long statusio;      /* SstIORegs.status(BAR0+0x000000+0x000)    */
    unsigned long fifoReadPtr;   /* cmdFifo0.readPtrL  (hw ring consume ptr) */
    unsigned long fifoBaseSize;  /* cmdFifo0.baseSize  (EN_CMDFIFO + size)   */
    unsigned long fifoDepth;     /* cmdFifo0.depth     (unconsumed words)    */
    unsigned long vramBytes;     /* echoed attach inputs / computed layout   */
    unsigned long desktopEnd;
    unsigned long desktopStride;
    unsigned long tramOffset;
    unsigned long tramSize;
    unsigned long fifoOffset;
    unsigned long fifoLength;
    unsigned long stride;        /* 16bpp 3D color-buffer pitch              */
    unsigned long colBuf0;
    unsigned long colBuf1;
    unsigned long colBuf2;
    unsigned long nColBuffers;
    unsigned long auxOffset;
    unsigned long nAuxBuffers;
    unsigned long backIdx;       /* which colBuf we render into              */
    unsigned long width;
    unsigned long height;
    unsigned long depthBytes;
    unsigned long swWriteOfs;    /* gbk_fifo software ring write cursor      */
    unsigned long swReadOfs;     /* gbk_fifo software last-known read cursor */
} fxdbg_probe_t;

/* FXDBG_CLEAR (rung 2): gb_clear(rgb) + gb_swap - proves FIFO execution
 * end-to-end (FASTFILL + 2D blit-present).
 * pvIn: fxdbg_clear_in_t. pvOut: fxdbg_status_out_t (optional). */
typedef struct fxdbg_clear_in {
    unsigned long r, g, b;       /* 0..255 each                              */
} fxdbg_clear_in_t;

/* FXDBG_TRI (rung 3): one hardcoded gouraud triangle + gb_swap - proves the
 * PKT3 + state-flush + submit path. pvIn: ignored. pvOut: fxdbg_status_out_t. */

/* FXDBG_TEX (rung 4): checker texture upload + bind + textured quad + gb_swap -
 * proves PKT5 + the TMU. pvIn: ignored. pvOut: fxdbg_status_out_t. */

/* the small result buffer rungs 2-4 write back. */
typedef struct fxdbg_status_out {
    unsigned long magic;         /* FXDBG_MAGIC                              */
    unsigned long ok;            /* 1 = rung ran, 0 = not attached           */
    unsigned long faulted;       /* g_dev.faulted after the rung             */
} fxdbg_status_out_t;

/* FXDBG_READBACK (rung 5): copy a rectangle of framebuffer pixels (BAR1) back
 * to the agent so rendering is verified WITHOUT a GDI screenshot of the 3D
 * surface. pvIn: fxdbg_readback_in_t. pvOut: raw 16bpp (565) pixels, row-major,
 * 2 bytes/pixel, clamped to fit cjOut; returns bytes written. */
typedef struct fxdbg_readback_in {
    unsigned long which;         /* FXDBG_RB_BACK / FXDBG_RB_DESKTOP         */
    unsigned long x, y, w, h;    /* rect in pixels (clamped to the surface)  */
} fxdbg_readback_in_t;

/* ------------------------------------------------------------------------- *
 * gbkernel-side implementations (gbkernel.c). Declared unconditionally so the
 * DrvEscape handler links; they touch the card only in the HAVE_DDK build.
 * ------------------------------------------------------------------------- */
int  gbkernel_dbg_probe(fxdbg_probe_t *out);
int  gbkernel_dbg_clear(unsigned long r, unsigned long g, unsigned long b);
int  gbkernel_dbg_tri(void);
int  gbkernel_dbg_tex(void);
unsigned long gbkernel_dbg_readback(unsigned long which,
                                    unsigned long x, unsigned long y,
                                    unsigned long w, unsigned long h,
                                    void *out, unsigned long outMax);
int  gbkernel_dbg_faulted(void);

#ifdef __cplusplus
}
#endif
#endif /* FXD3D_GBKDEBUG_H */
