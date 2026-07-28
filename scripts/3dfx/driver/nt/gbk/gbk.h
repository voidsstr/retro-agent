/*
 * gbk.h - pure-logic core of the fxD3D kernel Glide backend (gbkernel, M4b).
 *
 * These modules are the host-testable half of the design in
 * docs/3dfx-gbkernel-design.md: all packet encoding, memory-layout math,
 * shadow-register state math, and FIFO bookkeeping - everything that can be
 * verified without hardware. The kernel half (gbkernel.c: MMIO plumbing,
 * attach, actual FIFO stores) calls into these.
 *
 * Constraints (enforced by gbkernel-test/Makefile flags):
 *   - C89, no CRT includes, no Win32, no floats in module logic
 *     (layout/packet/fifo math is integer; state math is integer bitops).
 *   - Only include hw/h3hw.h (self-contained, fixed-width types).
 *
 * Numeric conventions at this seam (so the core needs no glide.h):
 *   - compare funcs  = GR_CMP_* values 0..7 (NEVER..ALWAYS); the SST fbzMode
 *     SST_ZFUNC / alphaMode SST_ALPHAFNC fields use the same encoding
 *     (gglide.c:1802-1817, :662-682).
 *   - blend factors  = GR_BLEND_* values, identical to SST_A_* codes
 *     (h3defs.h:445-453; _grAlphaBlendFunction gglide.c:472-514).
 *   - cull           = gb_cull_t from d3dhal/glidebackend.h (0 none, 1 CW,
 *     2 CCW) mapped to PKT3 smode bits (design sec 2/3).
 */
#ifndef GBK_H
#define GBK_H

#include "../hw/h3hw.h"

/* ==================================================================== */
/* gbk_packet.c - CMDFIFO packet header builders (design sec 2, 4)      */
/* All take BYTE register offsets within the chip's own space           */
/* (e.g. SST3D_OFF_fbzMode), exactly what SSTCP_REGBASE_FROM_ADDR eats. */
/* ==================================================================== */

/* PKT1: nWords same-register write header (STORE_FIFO fxcmd.h:737-756;
 * no SSTCP_INC - every data word goes to the one register). */
h3u32 gbk_pkt1_hdr(h3u32 regByteOff, h3u32 nWords, h3u32 chipField, int is2d);

/* PKT1 burst: nWords to SEQUENTIAL registers starting at regByteOff
 * (REG_GROUP_LONG_BEGIN fxcmd.h:608-620: hdr | SSTCP_INC; used for the
 * 32-word fogTable download, gglide.c:2091-2117). 3D space only. */
h3u32 gbk_pkt1_burst_hdr(h3u32 regByteOff, h3u32 nWords, h3u32 chipField);

/* PKT3: triangle-setup header (fxcmd.h:898-917; h3gdefs.h:225-237).
 * smode = kSetup* bits, pmask = SST_SETUP_* bits, cmd = SSTCP_PKT3_BDDBDD.. */
h3u32 gbk_pkt3_hdr(h3u32 smode, h3u32 pmask, h3u32 nVerts, h3u32 cmd);

/* PKT4: masked register-group write header, base register + 14-bit mask
 * (fxcmd.h:573-608; h3gdefs.h:240-243). Data words follow, one per set bit,
 * ascending. */
h3u32 gbk_pkt4_hdr(h3u32 baseRegByteOff, h3u32 mask14, h3u32 chipField,
                   int is2d);
/* number of data words a PKT4 with this mask carries (popcount of mask14) */
h3u32 gbk_pkt4_nwords(h3u32 mask14);

/* PKT5: linear-write headers (h3gdefs.h:245-257; FIFO_LINEAR_WRITE_BEGIN
 * fxcmd.h:1046-1079). space = SSTCP_PKT5_LFB etc. */
h3u32 gbk_pkt5_hdr1(h3u32 space, h3u32 nWords, h3u32 byteEnW2, h3u32 byteEnWN);
h3u32 gbk_pkt5_hdr2(h3u32 destByteAddr);

/* PKT0 JMP_LOCAL: the FIFO wrap packet, precomputed at init
 * (gsst.c:1446-1448: SSTCP_PKT0_JMP_LOCAL | fifoOfs<<(ADDR_SHIFT-2)). */
h3u32 gbk_pkt0_jmp_local(h3u32 fifoByteOff);

/* ==================================================================== */
/* gbk_layout.c - board-memory carve-out (design sec 1a;                */
/* hwcAllocBuffers minihwc.c:1397-1610, calc* :3678-3745)               */
/* Our variant: GDI desktop at bottom, then tram, FIFO, color bufs      */
/* (even-page parity), aux (odd-page parity) at top. All LINEAR.        */
/* ==================================================================== */

#define GBK_MAX_COL_BUFFERS 3

typedef struct gbk_layout {
    /* inputs echoed */
    h3u32 vramBytes;             /* total board memory                   */
    h3u32 desktopEnd;            /* first byte above the GDI surface     */
    h3u32 xres, yres;
    /* results (all byte offsets into BAR1 / board memory) */
    h3u32 tramOffset, tramSize;  /* texture region                       */
    h3u32 fifoOffset, fifoLength;/* CMDFIFO (length already -0x2000)     */
    h3u32 stride;                /* color/aux buffer stride (xres<<1)    */
    h3u32 bufSize;               /* single linear buffer size, stride*y  */
    h3u32 nColBuffers;
    h3u32 colBufOffset[GBK_MAX_COL_BUFFERS]; /* [0] lowest = fbOffset    */
    h3u32 nAuxBuffers;
    h3u32 auxOffset;             /* depth/aux buffer (or 0 if none)      */
} gbk_layout_t;

/* Linear stride/size (calcBufferStride/calcBufferSize minihwc.c:3678-3745) */
h3u32 gbk_calc_stride(h3u32 xres);
h3u32 gbk_calc_buf_size(h3u32 xres, h3u32 yres);

/* Compute the full carve-out. Returns 0 on success, nonzero if the mode
 * does not fit (minimum-memory check as gsst.c:1043-1048 plus the
 * hwcAllocBuffers fifoSize<0 check, minihwc.c:1528-1537). On failure the
 * struct must NOT be trusted. minTramBytes 0 = H3HW_MIN_TEXTURE_STORE. */
int gbk_layout_compute(gbk_layout_t *out,
                       h3u32 vramBytes, h3u32 desktopEnd,
                       h3u32 xres, h3u32 yres,
                       h3u32 nColBuffers, h3u32 nAuxBuffers,
                       h3u32 minTramBytes);

/* Enforce desktopEnd <= tram < fifo < colBuf[0] < .. < aux <= vram with no
 * region overlaps (design sec 6 risk 2). Returns 0 if sane. */
int gbk_layout_validate(const gbk_layout_t *l);

/* Readback-rectangle bounds solver (used by gbkernel_dbg_readback, the
 * FXDBG_READBACK escape rung). Clamps a CALLER-SUPPLIED rect (x,y,w,h) to the
 * uw x uh surface AND to what the output buffer holds (outMax bytes, 2 bytes/
 * pixel); returns the number of full rows to copy and writes the clamped width
 * to *clampedW (0 when it returns 0). SECURITY: the extent clamp is
 * overflow-safe (`w > uw - x`, never `x + w > uw`, which wraps at 2^32 with an
 * attacker-supplied w/h and would SKIP the clamp -> out-of-bounds BAR1
 * aperture read). Returns 0 for any degenerate / out-of-range rect. */
h3u32 gbk_readback_rows(h3u32 x, h3u32 y, h3u32 w, h3u32 h,
                        h3u32 uw, h3u32 uh, h3u32 outMax, h3u32 *clampedW);

/* ==================================================================== */
/* gbk_surf.c - DDraw surface layout math + the 5a present-convert      */
/* (design docs/3dfx-d3d-hal-design.md sec 5a; consumed by the M4c-2    */
/* DDraw surface path in driver/nt/enable.c and by gb_swap's dst-format */
/* selection in gbkernel.c). Pure integer, host-tested.                 */
/* ==================================================================== */

/* Byte pitch for a linear offscreen/texture surface: width * bytespp
 * rounded up to 16 (SST texture-address granularity, h3defs.h:561; also
 * a safe GDI/DDraw alignment). Returns 0 for a degenerate/oversized
 * request (w 1..2048, bytespp 1/2/4 only) so callers fail cleanly.     */
h3u32 gbk_surf_pitch(h3u32 w, h3u32 bytespp);

/* Total byte size pitch*h, guarded the same way (h 1..2048, pitch from
 * gbk_surf_pitch). Returns 0 on a degenerate request - never wraps.    */
h3u32 gbk_surf_bytes(h3u32 pitch, h3u32 h);

/* Bump allocator over the DDraw offscreen-surface VRAM region that
 * gbkernel reserves out of tram (gbkernel_get_meminfo surfOffset/
 * surfSize). Offsets are absolute board-byte offsets (BAR1). A bump
 * cursor cannot free mid-stream; a live-count reset (all surfaces
 * destroyed -> cursor rewinds) matches the DDraw surface lifecycle
 * (full teardown on mode switch / exclusive-mode exit).               */
#define GBK_SURF_FAIL 0xFFFFFFFFUL

typedef struct gbk_surfheap {
    h3u32 base;                  /* region start (board byte offset)     */
    h3u32 size;                  /* region length in bytes               */
    h3u32 cursor;                /* next allocation offset               */
    h3u32 live;                  /* surfaces currently allocated         */
} gbk_surfheap_t;

void  gbk_surfheap_init(gbk_surfheap_t *hp, h3u32 base, h3u32 size);
/* 16-byte-aligned allocation; GBK_SURF_FAIL when it does not fit
 * (overflow-safe: compares against remaining room, never base+size).   */
h3u32 gbk_surfheap_alloc(gbk_surfheap_t *hp, h3u32 bytes);
/* one surface destroyed; when the live count hits 0 the cursor rewinds.
 * Returns 0 on a balanced free, -1 (cursor untouched) for an unbalanced
 * free with nothing live - callers gate frees on an explicit ownership
 * tag, and the allocator refuses a spurious extra free (review #3).    */
int   gbk_surfheap_free(gbk_surfheap_t *hp);

/* The 5a 16->32 present-convert pixel math: RGB565 -> XRGB8888 with
 * bit-replicated low bits (0x1F->0xFF full range; X byte = 0). This is
 * the CPU convert the design allows at bring-up; the hw path is the 2D
 * blt with a 32bpp dst format (gbk_present_dstformat below).           */
h3u32 gbk_expand565(h3u32 p565);
void  gbk_conv565_row_8888(h3u32 *dst, const h3u16 *src, h3u32 n);
/* ARGB1555 / ARGB4444 -> ARGB8888 (review #11): the other two 16bpp
 * formats CanCreateSurface accepts. Callers dispatch by pixel-format
 * masks, NEVER by byte count - a 1555/4444 pixel decoded with 565 field
 * widths is hue-shifted.                                               */
h3u32 gbk_expand1555(h3u32 p1555);
void  gbk_conv1555_row_8888(h3u32 *dst, const h3u16 *src, h3u32 n);
h3u32 gbk_expand4444(h3u32 p4444);
void  gbk_conv4444_row_8888(h3u32 *dst, const h3u16 *src, h3u32 n);

/* DP2 buffer-window clamp (review #4/#6): nonzero when the window
 * [off, off + len*stride) fits a bufBytes buffer. stride 1 = plain byte
 * window (command buffer); stride = FVF vertex stride bounds a vertex
 * window. Overflow-safe; bufBytes 0 (unknown size) always fails.       */
int   gbk_dp2_window_ok(h3u32 bufBytes, h3u32 off, h3u32 len, h3u32 stride);

/* dstFormat register word for the blit-present (gb_swap dst): desktop
 * stride packed in the linear-stride field + the pixel format matching
 * the desktop depth (16 -> SSTG_PIXFMT_16BPP plain copy; 32 ->
 * SSTG_PIXFMT_32BPP = the hardware 565->8888 convert-blit of design
 * 5a). Returns 0 for an unsupported depth or a stride that does not
 * fit SSTG_DST_LINEAR_STRIDE - callers must then skip the present.     */
h3u32 gbk_present_dstformat(h3u32 desktopStride, h3u32 desktopBpp);

/* ==================================================================== */
/* gbk_fifo.c - CMDFIFO software state + init register list             */
/* (design sec 1c/1e/2; hwcInitFifo minihwc.c:1613-1665;                */
/*  gsst.c:1402-1451; space/wrap protocol fifo.c:838-1056)              */
/* ==================================================================== */

typedef struct gbk_regwrite {
    h3u32 barOffset;             /* byte offset within BAR0              */
    h3u32 value;
} gbk_regwrite_t;

/* hwcInitFifo (minihwc.c:1634-1663) performs 10 ordered stores:
 * baseSize=0(disable); baseAddrL; readPtrL; readPtrH; aMin; aMax; depth;
 * holeCount; cmdFifoThresh; baseSize=enable. (The design sec 1c prose
 * says "9 writes" because it lists aMin=aMax as one item.) */
#define GBK_FIFO_INIT_NWRITES 10

void gbk_fifo_init_regs(h3u32 fifoStart, h3u32 fifoLength,
                        gbk_regwrite_t out[GBK_FIFO_INIT_NWRITES]);

typedef struct gbk_fifo {
    h3u32 fifoOffset;            /* byte offset of ring in board memory  */
    h3u32 fifoSize;              /* usable length (layout fifoLength)    */
    h3u32 writeOfs;              /* next write, byte offset in ring      */
    h3u32 readOfs;               /* last known hw read, ring offset      */
    h3i32 roomToEnd;             /* bytes to (size - FIFO_END_ADJUST)    */
    h3i32 roomToReadPtr;         /* bytes until we'd hit read ptr - 4    */
    h3u32 jmpHdr;                /* precomputed PKT0 wrap packet         */
} gbk_fifo_t;

/* gbk_fifo_reserve outcome (one pass of _grMakeRoom, fifo.c:896-1032) */
#define GBK_FIFO_OK        0     /* write nBytes at writeOfs, then advance */
#define GBK_FIFO_NEED_WRAP 1     /* write jmpHdr at writeOfs, call
                                  * gbk_fifo_wrap, then reserve again      */
#define GBK_FIFO_STALL     2     /* hw hasn't consumed enough: re-poll
                                  * readPtrL and reserve again (kernel
                                  * caller bounds the retries, design 6.1) */

void  gbk_fifo_init(gbk_fifo_t *f, h3u32 fifoStart, h3u32 fifoLength);
/* One protocol step for an nBytes write. readPtrL = current hw
 * cmdFifo0.readPtrL readback (board byte address, twice-stable per
 * _grHwFifoPtr fifo.c:1068-1090); only consulted when short of room. */
int   gbk_fifo_reserve(gbk_fifo_t *f, h3u32 nBytes, h3u32 readPtrL);
/* would writing nBytes run past the usable end? (=> emit jmpHdr + wrap) */
int   gbk_fifo_need_wrap(const gbk_fifo_t *f, h3u32 nBytes);
/* would writing nBytes cross the hw read pointer? (=> poll readPtrL)    */
int   gbk_fifo_need_room(const gbk_fifo_t *f, h3u32 nBytes);
/* account for nBytes just written                                       */
void  gbk_fifo_advance(gbk_fifo_t *f, h3u32 nBytes);
/* reset software state to ring start after the JMP packet was written   */
void  gbk_fifo_wrap(gbk_fifo_t *f);
/* fold a fresh readPtrL readback into roomToReadPtr                     */
void  gbk_fifo_update_read(gbk_fifo_t *f, h3u32 readPtrL);
/* sec 5: status helpers */
int   gbk_status_busy(h3u32 status3d);           /* SST_BUSY set?        */
int   gbk_swaps_pending(h3u32 status3d);         /* SWAPBUFPENDING count */

/* ==================================================================== */
/* gbk_state.c - shadow registers + lifted glide state math             */
/* (design sec 3; all pure bit-math on the shadow)                      */
/* ==================================================================== */

/* dirty-bit ids for the PKT4 flush group (base fbzColorPath, design 2) */
#define GBK_DIRTY_fbzColorPath  H3BIT(0)
#define GBK_DIRTY_fogMode       H3BIT(1)
#define GBK_DIRTY_alphaMode     H3BIT(2)
#define GBK_DIRTY_fbzMode       H3BIT(3)
#define GBK_DIRTY_fogColor      H3BIT(4)
#define GBK_DIRTY_zaColor       H3BIT(5)
#define GBK_DIRTY_chromaKey     H3BIT(6)
#define GBK_DIRTY_c0            H3BIT(7)
#define GBK_DIRTY_c1            H3BIT(8)
#define GBK_DIRTY_textureMode   H3BIT(9)
#define GBK_DIRTY_tLOD          H3BIT(10)
#define GBK_DIRTY_texBaseAddr   H3BIT(11)

typedef struct gbk_state {
    /* FBI shadow */
    h3u32 fbzColorPath, fogMode, alphaMode, fbzMode;
    h3u32 fogColor, zaColor, chromaKey, c0, c1;
    /* TMU shadow (filter/clamp/combine; bind-time bits - TPERSP_ST,
     * TCLAMPW, format, base, tLOD - are merged by gbktex in M4b-2) */
    h3u32 textureMode, tLOD, texBaseAddr;
    /* PKT3 header ingredients (not registers - design sec 2/3 cull row) */
    h3u32 smode;                 /* kSetup* cull bits                    */
    h3u32 triHdrUntextured;      /* precomputed, pmask RGB|A|Z|Wfbi      */
    h3u32 triHdrTextured;        /* + ST0                                */
    /* bookkeeping */
    h3u32 dirty;                 /* GBK_DIRTY_* bits                     */
    int   texEnabled;            /* ENTEXTUREMAP state as last FLUSHED:
                                  * a flip makes flush prepend nopCMD=0
                                  * (distate.c:910-915)                  */
    int   hasAux;                /* aux buffer allocated - depth enable
                                  * is refused without one ("they could
                                  * stomp on the cmd fifo",
                                  * gglide.c:1830-1833)                  */
    int   ccRequiresTex;         /* _grColorCombine texture demand       */
    int   acRequiresTex;         /* _grAlphaCombine texture demand       */
} gbk_state_t;

/* assertDefaultState() replayed as gbkernel defaults (gsst.c:602-672);
 * initial fbzMode shadow = ENRECTCLIP|ENZBIAS (gsst.c:1499). Marks all
 * shadow registers dirty so the first flush pushes the full state.     */
void gbk_state_defaults(gbk_state_t *s, int hasDepthBuffer);

/* gb_set_* lifts (see design sec 3 table for the glide source of each) */
void gbk_state_set_depth(gbk_state_t *s, int enable, int func, int write);
void gbk_state_set_blend(gbk_state_t *s, int enable, int srcFact, int dstFact);
void gbk_state_set_alphatest(gbk_state_t *s, int enable, int func, h3u32 ref);
void gbk_state_set_fog(gbk_state_t *s, int enable, h3u32 rgb);
void gbk_state_set_cull(gbk_state_t *s, int cull /* 0 none,1 CW,2 CCW */);
void gbk_state_set_dither(gbk_state_t *s, int enable);
void gbk_state_set_chromakey(gbk_state_t *s, int enable, h3u32 rgb);
void gbk_state_set_shade(gbk_state_t *s, int gouraud);
void gbk_state_set_texfactor(gbk_state_t *s, h3u32 argb);
void gbk_state_set_texfilter(gbk_state_t *s, int bilinear,
                             int clampU, int clampV);
/* returns nonzero if SST_ENTEXTUREMAP flipped vs the current shadow
 * (info only - gbk_state_flush emits the required nopCMD=0 itself)      */
int  gbk_state_set_texenable(gbk_state_t *s, int enable);

/* pack one fogTable register word from three consecutive 8-bit fog-table
 * entries (grFogTable gglide.c:2096-2113: delta = (next-cur)<<2 in .2
 * format, truncated to 8 bits; word = e1<<24|d1<<16|e0<<8|d0). For the
 * last word pass eNext == e1 (":don't access beyond end of table").     */
h3u32 gbk_fog_pack_entry(h3u32 e0, h3u32 e1, h3u32 eNext);

/* current PKT3 triangle header for the draw path (design sec 3 tail);
 * nVerts <= SSTCP_PKT3_MAXVERTS is the caller's contract               */
h3u32 gbk_state_tri_hdr(const gbk_state_t *s, int textured, h3u32 nVerts);

/* Serialize dirty shadow registers into out[] as FIFO words:
 *   [PKT1 nopCMD=0 if ENTEXTUREMAP flipped since last flush]
 *   PKT4 group base fbzColorPath (_grFlushCommonStateRegs
 *     gglide.c:2200-2246), PKT4 group base c0, PKT4 TMU group
 *     chip-field TMU0 (gglide.c:2287-2289).
 * Returns word count written (0 if clean), clears the dirty mask.
 * maxWords guards overrun (returns -1 if out[] is too small).          */
int gbk_state_flush(gbk_state_t *s, h3u32 *out, int maxWords);

/* worst-case flush size: 2 (nopCMD) + 1+7 (common) + 1+2 (c0/c1)
 * + 1+3 (TMU) */
#define GBK_STATE_FLUSH_MAXWORDS 17

#endif /* GBK_H */
