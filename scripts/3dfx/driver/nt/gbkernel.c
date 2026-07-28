/*
 * gbkernel.c - kernel-mode MMIO transport + real gb_* backend for the fxD3D
 *              NT display driver (fxd3ddd.dll). Milestone M4b-2.
 *
 * This file replaces the gbstub.c placeholder with the REAL driver-side Glide
 * backend: it drives the Voodoo3 (Avenger/H3) directly through driver-mapped
 * MMIO - no grSstWinOpen, no user-mode glide3x.dll. It is the untestable-
 * until-hardware half of docs/3dfx-gbkernel-design.md; the register-correct,
 * host-verified pure-logic half lives in gbk/gbk_*.c (packet encoders, memory
 * layout, shadow-register state math, FIFO ring bookkeeping) and is CALLED
 * here, never reimplemented.
 *
 * What this file owns (design sec 1,2,4,5):
 *   - gbkernel_attach()/detach(): the single entry the M4c chassis calls to
 *     bring the board up once BAR0/BAR1 are mapped by the paired miniport -
 *     layout carve-out, the 10-store CMDFIFO init, Y-origin, initial buffer
 *     registers, assertDefaultState replay, first FASTFILL clear.
 *   - the FIFO write engine: gbk_emit() reserves ring space via the verified
 *     gbk_fifo protocol (OK / NEED_WRAP / STALL), writes the PKT0 wrap packet,
 *     polls the read pointer, then stores the words into BAR1. The read-pointer
 *     poll and the STALL retries in one gbk_fifo_make_room() call share ONE
 *     iteration budget, so they can never compose multiplicatively - a wedged
 *     or tearing card can never hang win32k (design risk #1).
 *   - the gb_* seam (glidebackend.h): state setters feed gbk_state shadows;
 *     draws flush dirty state as a PKT4 group then emit PKT3 triangles; clear
 *     is FASTFILL; swap is a 2D WAX blit-present; textures bump-allocate tram
 *     and download via PKT5 wrapped in the Avenger errata flush.
 *
 * MMIO discipline: every store/load against the card is tagged
 *   "M4d: validated on-card via the escape ladder"
 * because it cannot be exercised without the paired miniport + real hardware
 * (design sec 6 bring-up ladder). The arithmetic that FEEDS each store is the
 * host-verified gbk_* logic.
 *
 * Kernel-clean: C89, no CRT, and the TRANSPORT is pure integer - vertex floats
 * are copied as raw 32-bit words (union / dword load), never recomputed. The
 * ONLY float arithmetic is the perspective-ST scale on the TEXTURED draw path
 * (v->s * texW * oow); the draw functions bracket it themselves with
 * EngSaveFloatingPointState/EngRestoreFloatingPointState (gbk_fpu_enter/leave)
 * per design risk #4 - not trusting an external caller. Untextured draws AND
 * texture bind touch no FPU at all (colors and the ST size-scales are built as
 * IEEE-754 bit patterns with integer math).
 *
 * Clean-room: built only on the design doc, the open 3dfx-GPL glide h3 tree,
 * and our host-verified gbk_* modules. The proprietary H5 tree was not opened.
 */
#include "../../d3dhal/glidebackend.h"

#ifdef HAVE_DDK

#include "gbk/gbk.h"
#include "gbk/gbk_mmio.h"
#include "gbkernel.h"    /* the chassis entry contract: attach/detach/meminfo  */
#include "gbkdebug.h"    /* bring-up-ladder impls: gbkernel_dbg_* (M4c-1)      */

/* ======================================================================== */
/* Absolute BAR0 byte offsets for the register blocks we touch directly.    */
/* (the pure-logic modules speak in per-chip offsets; the transport adds    */
/*  the block base to reach the real aperture - design sec 0.)              */
/* ======================================================================== */
#define GBK_IO(off)   ((h3u32)SST_IO_OFFSET     + (h3u32)(off))
#define GBK_CREG(off) ((h3u32)SST_CMDAGP_OFFSET + (h3u32)(off))
#define GBK_2D(off)   ((h3u32)SST_2D_OFFSET     + (h3u32)(off))
#define GBK_3D(off)   ((h3u32)SST_3D_OFFSET     + (h3u32)(off))

/* SINGLE shared iteration budget for a whole gbk_fifo_make_room() call
 * (design risk #1). BOTH the twice-stable read-pointer poll (gbk_hw_readptr)
 * AND the outer STALL retries draw down this ONE counter, so the two waits can
 * never compose multiplicatively: total uncached MMIO reads across make_room
 * stay bounded by ~GBK_FIFO_ROOM_BUDGET even when readPtrL tears on every
 * sample (a half-dead PCI card returning bus noise, r1!=r2 forever - the exact
 * failure mode the cap must survive). Exhaustion -> gbk_fault(), never a spin
 * inside win32k. ~1e7 => a bounded sub-second stall, then fail safe. */
#define GBK_FIFO_ROOM_BUDGET 10000000L
/* gb_finish drain / swap-throttle status polls are single, NON-nested loops;
 * each is independently capped at the same order of magnitude. */
#define GBK_STATUS_POLL_CAP  10000000L

/* Fixed vertex-word counts in the PKT3 stream (design sec 2): the setup unit
 * reads params in pmask order X,Y,R,G,B,A,Z(ooz),Wfbi(oow),[S,T]. */
#define GBK_VWORDS_UNTEX   8
#define GBK_VWORDS_TEX     10
/* Independent triangles per PKT3 (BDDBDD): 15 verts / 3 = 5 (design sec 2). */
#define GBK_TRIS_PER_PKT   (SSTCP_PKT3_MAXVERTS / 3)

/* Static texture-handle pool (no CRT allocator in the transport). */
#define GBK_MAX_TEX  512

/* ======================================================================== */
/* Concrete texture handle (the core treats gb_tex_t as opaque).            */
/* ======================================================================== */
struct gb_tex {
    int    inuse;
    int    w, h;
    int    fmt;          /* gb_texfmt_t                                     */
    h3u32  addr;         /* tram byte offset (16-byte aligned)              */
    h3u32  bytes;        /* texel byte count                                */
    h3u32  tformat;      /* SST_* format bits for textureMode               */
    h3u32  tlod;         /* SST_LODMIN/MAX/ASPECT bits for tLOD             */
};

/* ======================================================================== */
/* The one static device context (design sec 6: TLS+GrGC -> single ctx).    */
/* ======================================================================== */
typedef struct gbk_dev {
    char        *bar0;        /* register aperture VA (32 MB)               */
    char        *bar1;        /* linear framebuffer VA (32 MB, NON-CACHED) */
    unsigned     vramBytes;
    unsigned     desktopEnd;
    unsigned     desktopStride; /* GDI primary pitch in bytes (blit dst)    */
    unsigned     desktopBpp;  /* GDI primary depth: 16 plain-copy present,
                               * 32 = the 5a hw 565->8888 convert-blit      */
    int          w, h, depthBytes;

    gbk_layout_t layout;      /* verified carve-out (gbk_layout.c)          */
    gbk_fifo_t   fifo;        /* verified ring bookkeeping (gbk_fifo.c)     */
    gbk_state_t  state;       /* verified shadow registers (gbk_state.c)    */

    int          attached;    /* emit is a no-op until attach completes     */
    int          faulted;     /* FIFO timeout latched: refuse all further
                               * MMIO so a wedged card can't be re-poked    */
    h3u32        tramNext;    /* texture bump-allocator cursor              */
    h3u32        tramLimit;   /* gb_tex_create ceiling: tram ABOVE this is
                               * the reserved DDraw surface region (M4c-2)  */
    h3u32        surfOffset;  /* DDraw offscreen-surface region start       */
    h3u32        surfSize;    /* DDraw offscreen-surface region length      */
    int          backIdx;     /* colBuffer index we draw into (double-buf)  */

    /* perspective-ST scale, set at bind time (design sec 4). float lives
     * only on the textured draw path (design risk #4). */
    float        sscale, tscale;
} gbk_dev_t;

static gbk_dev_t g_dev;                 /* zero-initialized (static)         */
static struct gb_tex g_texPool[GBK_MAX_TEX];
static h3u32 g_ub2f[256];               /* uchar -> IEEE-754 bits (integer)  */

/* forward decls */
static void  gbk_emit(const h3u32 *words, int n);
static void  gbk_emit_reg(h3u32 regOff, h3u32 val, h3u32 chip, int is2d);

/* ======================================================================== */
/* Integer whole-number -> IEEE-754 single-precision bit pattern, computed   */
/* with PURE INTEGER math (no FPU - design risk #4). Exact for any n in      */
/* [0, 2^24) (all integers representable as a float): e = floor(log2 n),     */
/* bits = ((127+e)<<23) | mantissa. Used to build both the vertex-color      */
/* table (0..255) and the perspective-ST texture-size scales (w,h <= 2048)   */
/* as raw float bits, so neither the untextured draw path NOR texture bind   */
/* ever computes a float (the only float ARITHMETIC in this file is the      */
/* bracketed per-vertex ST multiply in gbk_vertex_words).                    */
/* ======================================================================== */
static h3u32
gbk_u_to_f32bits(h3u32 n)
{
    h3u32 e, t, mant;

    if (n == 0)
        return 0UL;
    e = 0;
    t = n;
    while (t > 1UL) { t >>= 1; e++; }          /* floor(log2 n)             */
    mant = (n << (23 - e)) & 0x7FFFFFUL;        /* drop the implicit 1 bit   */
    return ((127UL + e) << 23) | mant;
}

static void
gbk_init_color_table(void)
{
    h3u32 i;
    for (i = 0; i < 256UL; i++)
        g_ub2f[i] = gbk_u_to_f32bits(i);
}

/* Raw 32-bit pattern of a float lvalue, FPU-free (a plain aligned dword
 * load). This is the "copied as raw words, not recomputed" path the design
 * requires for x/y/ooz/oow. gb_vtx_t begins with floats so &fld is 4-aligned;
 * the driver builds at /Od (no TBAA) so the type pun is well-defined here. */
#define GBK_F32(fld)  (*(const h3u32 *)&(fld))

/* ======================================================================== */
/* Fault path (design sec 6 risk 1): latch the fault, disable the CMDFIFO    */
/* (baseSize=0, as hwcRestoreVideo), and refuse all further emits. The DDI   */
/* call unwinds; win32k keeps running.                                      */
/* ======================================================================== */
static void
gbk_fault(void)
{
    g_dev.faulted = 1;
    /* M4d: validated on-card via the escape ladder */
    GBK_WR32(g_dev.bar0, GBK_CREG(SSTC_OFF_cmdFifo0_baseSize), 0);
    GBK_WMB();
}

/* ======================================================================== */
/* Read the hardware CMDFIFO read pointer, twice-stable (_grHwFifoPtr        */
/* fifo.c:1068-1090): sample readPtrL, read status in between, re-sample,    */
/* and only accept a pair that agrees - the register can tear mid-update.    */
/* On a stable pair, stores the raw board byte address the register holds    */
/* (init wrote it = fifoStart, exactly what gbk_fifo_update_read consumes)   */
/* in *out and returns 1.                                                    */
/*                                                                           */
/* Every re-sample (r1 != r2) charges the caller's SHARED *budget instead of */
/* an independent inner cap: this is what stops the poll from composing      */
/* multiplicatively with make_room's STALL loop (design risk #1). When the   */
/* shared budget is exhausted mid-tear it returns 0 so the caller faults -   */
/* NEVER a torn pointer that would re-enter the STALL loop.                  */
/* ======================================================================== */
static int
gbk_hw_readptr(h3u32 *out, long *budget)
{
    h3u32 r1, r2;

    for (;;) {
        /* M4d: validated on-card via the escape ladder */
        r1 = GBK_RD32(g_dev.bar0, GBK_CREG(SSTC_OFF_cmdFifo0_readPtrL));
        /* glide reads status between the two samples (fifo.c:1082-1086); the
         * intervening bus cycle lets an in-flight readPtrL update settle. */
        (void)GBK_RD32(g_dev.bar0, GBK_IO(SSTIO_OFF_status));
        r2 = GBK_RD32(g_dev.bar0, GBK_CREG(SSTC_OFF_cmdFifo0_readPtrL));
        if (r1 == r2) {
            *out = r2;
            return 1;                   /* twice-stable pair                 */
        }
        /* register still tearing: charge the SHARED make_room budget. */
        if (--*budget <= 0)
            return 0;                   /* budget exhausted: caller faults   */
    }
}

/* ======================================================================== */
/* Reserve nBytes of contiguous ring space and return the BAR1 byte offset   */
/* to write at (design sec 2 transport; gbk_fifo protocol fifo.c:838-1056).  */
/* Handles the two ways the ring can be short:                               */
/*   NEED_WRAP - store the precomputed PKT0 JMP-to-start at the write cursor  */
/*               and wrap (hardware never auto-wraps, gsst.c:1406-1415),      */
/*   STALL     - the hw hasn't consumed enough yet: re-poll the read pointer  */
/*               and re-reserve, all under ONE shared iteration budget.       */
/* Returns (h3u32)-1 after faulting on: an over-large single packet (can't    */
/* fit one lap - would loop NEED_WRAP forever), a second wrap in one call,    */
/* or exhausting the shared iteration budget (read-pointer tear-poll + stall  */
/* retries combined). Caller must gbk_fifo_advance(nBytes) after writing.    */
/* ======================================================================== */
static h3u32
gbk_fifo_make_room(h3u32 nBytes)
{
    long  budget = GBK_FIFO_ROOM_BUDGET; /* ONE budget shared by every poll  */
    int   wraps = 0;
    int   rc;
    h3u32 rp;

    /* A single packet must fit within one usable lap, or NEED_WRAP could
     * recur without ever hitting the budget (design risk #1: fail safe,
     * never hang). roomToReadPtr carries a permanent -4 margin, so require
     * nBytes + 4 <= fifoSize - END_ADJUST. */
    if (nBytes + 4UL > g_dev.fifo.fifoSize - (h3u32)H3HW_FIFO_END_ADJUST) {
        gbk_fault();
        return (h3u32)-1;
    }

    for (;;) {
        /* the read-pointer poll spends from the SAME budget as the STALL
         * retries below, so its inner tear-spins can't nest inside them. */
        if (!gbk_hw_readptr(&rp, &budget)) {
            gbk_fault();                /* tore past the shared budget        */
            return (h3u32)-1;
        }
        rc = gbk_fifo_reserve(&g_dev.fifo, nBytes, rp);
        if (rc == GBK_FIFO_OK)
            return g_dev.fifo.fifoOffset + g_dev.fifo.writeOfs;
        if (rc == GBK_FIFO_NEED_WRAP) {
            if (++wraps > 1) {          /* one wrap per reserve is the max   */
                gbk_fault();
                return (h3u32)-1;
            }
            /* M4d: validated on-card via the escape ladder */
            GBK_WR32(g_dev.bar1,
                     g_dev.fifo.fifoOffset + g_dev.fifo.writeOfs,
                     g_dev.fifo.jmpHdr);
            GBK_WMB();
            gbk_fifo_wrap(&g_dev.fifo);
            continue;
        }
        /* GBK_FIFO_STALL: charge the SAME shared budget, so the total MMIO
         * reads across make_room stay bounded by ~GBK_FIFO_ROOM_BUDGET no
         * matter how the read-pointer poll and the stall retries interleave. */
        if (--budget <= 0) {
            gbk_fault();
            return (h3u32)-1;
        }
    }
}

/* ======================================================================== */
/* The FIFO write engine: push n words into the ring (design sec 2).        */
/* ======================================================================== */
static void
gbk_emit(const h3u32 *words, int n)
{
    h3u32 base, nBytes, i;

    if (!g_dev.attached || g_dev.faulted || n <= 0)
        return;

    nBytes = (h3u32)n * 4UL;
    base = gbk_fifo_make_room(nBytes);
    if (base == (h3u32)-1)
        return;                          /* faulted inside make_room         */

    for (i = 0; i < (h3u32)n; i++) {
        /* M4d: validated on-card via the escape ladder */
        GBK_WR32(g_dev.bar1, base + (i * 4UL), words[i]);
    }
    GBK_WMB();                           /* order the writes before advance  */
    gbk_fifo_advance(&g_dev.fifo, nBytes);
}

/* one PKT1 single-register write (design sec 2 "Single-register = PKT1"). */
static void
gbk_emit_reg(h3u32 regOff, h3u32 val, h3u32 chip, int is2d)
{
    h3u32 w[2];
    w[0] = gbk_pkt1_hdr(regOff, 1, chip, is2d);
    w[1] = val;
    gbk_emit(w, 2);
}

/* a PKT4 masked-group write from ascending (regByteOff,value) pairs: the
 * mask is popcount-implied by each reg's slot = (off-base)>>2, and the data
 * words follow in ascending slot order (h3gdefs.h:240-243). Callers pass the
 * pairs already sorted by offset (the natural register order below). */
typedef struct gbk_rw { h3u32 off; h3u32 val; } gbk_rw_t;

static void
gbk_emit_group(h3u32 baseOff, h3u32 chip, int is2d, const gbk_rw_t *rw, int n)
{
    h3u32 words[1 + 8];
    h3u32 mask = 0;
    int   i, k;

    if (n <= 0 || n > 8)
        return;
    for (i = 0; i < n; i++)
        mask |= 1UL << ((rw[i].off - baseOff) >> 2);
    words[0] = gbk_pkt4_hdr(baseOff, mask, chip, is2d);
    k = 1;
    for (i = 0; i < n; i++)
        words[k++] = rw[i].val;
    gbk_emit(words, k);
}

/* flush any dirty shadow state as the PKT4 groups gbk_state produces. */
static void
gbk_flush_state(void)
{
    h3u32 words[GBK_STATE_FLUSH_MAXWORDS];
    int   n = gbk_state_flush(&g_dev.state, words, GBK_STATE_FLUSH_MAXWORDS);
    if (n > 0)
        gbk_emit(words, n);
}

/* ======================================================================== */
/* Clear (design sec 5): FASTFILL, SGRAM 2-packet form (grBufferClear        */
/* gglide.c:820-852). PKT4 {zaColor, c1=fill}; PKT4 {fastfillCMD=1, restore   */
/* zaColor, restore c1}. Fill honors SST_ENRECTCLIP + the fbzMode masks.    */
/* ======================================================================== */
static void
gbk_do_clear(unsigned r, unsigned g, unsigned b)
{
    gbk_rw_t set[2], fin[3];
    h3u32    fill, za;

    if (!g_dev.attached || g_dev.faulted)
        return;

    /* internal ARGB (swizzle = identity for GR_COLORFORMAT_ARGB, design 3) */
    fill = (0xFFUL << 24) | ((h3u32)(r & 0xFF) << 16)
         | ((h3u32)(g & 0xFF) << 8) | (h3u32)(b & 0xFF);
    /* clear depth to far (0xFFFF), keeping the shadow alpha field */
    za = (g_dev.state.zaColor & ~SST_ZACOLOR_DEPTH) | (0xFFFFUL & SST_ZACOLOR_DEPTH);

    /* group base zaColor (0x130): zaColor slot0, c1 slot6 -> mask 0x41 */
    set[0].off = SST3D_OFF_zaColor; set[0].val = za;
    set[1].off = SST3D_OFF_c1;      set[1].val = fill;
    gbk_emit_group(SST3D_OFF_zaColor, eChipBroadcast, 0, set, 2);

    /* group base fastfillCMD (0x124): fastfillCMD slot0, zaColor slot3,
     * c1 slot9 -> mask 0x209. Trigger the fill, then restore the shadows. */
    fin[0].off = SST3D_OFF_fastfillCMD; fin[0].val = 1UL;
    fin[1].off = SST3D_OFF_zaColor;     fin[1].val = g_dev.state.zaColor;
    fin[2].off = SST3D_OFF_c1;          fin[2].val = g_dev.state.c1;
    gbk_emit_group(SST3D_OFF_fastfillCMD, eChipBroadcast, 0, fin, 3);
}

/* ======================================================================== */
/* attach / detach (design sec 1) - the single entry the chassis calls.     */
/* ======================================================================== */
int
gbkernel_attach(void *bar0va, void *bar1va, unsigned vramBytes,
                unsigned desktopEnd, unsigned desktopStride,
                unsigned desktopBpp, int w, int h, int depthBytes)
{
    gbk_regwrite_t initw[GBK_FIFO_INIT_NWRITES];
    h3u32 miscInit0, stride, colLR, colBT;
    int   i, hasAux;

    if (bar0va == 0 || bar1va == 0 || w <= 0 || h <= 0)
        return -1;

    /* fresh context (static storage is already zero; be explicit for a
     * re-attach after a mode change). */
    g_dev.bar0          = (char *)bar0va;
    g_dev.bar1          = (char *)bar1va;
    g_dev.vramBytes     = vramBytes;
    g_dev.desktopEnd    = desktopEnd;
    /* desktop pitch in bytes for the blit-present dst (M4b-2 minor #2); fall
     * back to the 16bpp 3D stride if the chassis passed 0. */
    g_dev.desktopStride = desktopStride ? desktopStride : (unsigned)(w << 1);
    /* desktop depth selects the present dst format (design 5a): 32 = the hw
     * 565->8888 convert-blit, 16 = the plain copy. Stored VERBATIM (M4c-2
     * review #7): the old `!=32 -> 16` coercion made gbk_present_dstformat's
     * unsupported-depth guard unreachable, so an 8/15/24bpp desktop was
     * presented as 16bpp - 1280-byte rows on a 640-byte pitch, a scribble
     * past the primary into tram. With the real depth stored, gb_swap's
     * gbk_present_dstformat(dstFmt==0) check SKIPS the present instead. */
    g_dev.desktopBpp    = desktopBpp;
    g_dev.w          = w;
    g_dev.h          = h;
    g_dev.depthBytes = depthBytes;
    g_dev.attached   = 0;
    g_dev.faulted    = 0;
    g_dev.backIdx    = 1;               /* buffer 0 front, 1 back            */
    g_dev.sscale     = 1.0f;
    g_dev.tscale     = 1.0f;
    hasAux = (depthBytes > 0) ? 1 : 0;

    for (i = 0; i < GBK_MAX_TEX; i++)
        g_texPool[i].inuse = 0;
    gbk_init_color_table();

    /* 1. Board-memory carve-out (verified gbk_layout): double-buffered,
     *    optional aux. Fails (negative) if the mode doesn't fit. */
    if (gbk_layout_compute(&g_dev.layout, vramBytes, desktopEnd,
                           (h3u32)w, (h3u32)h,
                           2, (h3u32)hasAux, 0) != 0)
        return -1;
    g_dev.tramNext = g_dev.layout.tramOffset;
    stride = g_dev.layout.stride;

    /* 1b. DDraw offscreen-surface carve (M4c-2): reserve the TOP half of
     *     tram (page-rounded) for the DDraw surface path in enable.c -
     *     app-lockable texture surfaces and the design-5a 32bpp RT proxy.
     *     gb_tex_create's bump allocator is ceilinged at tramLimit so the
     *     core's own TMU downloads and the DDraw surfaces can never
     *     overlap. A tram too small to split leaves the whole of it to
     *     gb_tex (surfSize 0 -> enable.c refuses offscreen allocation).  */
    {
        h3u32 tTop = g_dev.layout.tramOffset + g_dev.layout.tramSize;
        h3u32 half = (g_dev.layout.tramSize >> 1) & ~(h3u32)(H3HW_PAGE - 1);
        if (half == 0) {
            g_dev.tramLimit  = tTop;
            g_dev.surfOffset = 0;
            g_dev.surfSize   = 0;
        } else {
            g_dev.tramLimit  = g_dev.layout.tramOffset + half;
            g_dev.surfOffset = g_dev.tramLimit;
            g_dev.surfSize   = tTop - g_dev.tramLimit;
        }
    }

    /* 2. CMDFIFO enable - the 10 ordered SstCRegs stores (design sec 1c;
     *    verified gbk_fifo_init_regs). These are DIRECT PIO to BAR0: the
     *    FIFO transport isn't up yet, and SstCRegs are never FIFO-routed. */
    gbk_fifo_init_regs(g_dev.layout.fifoOffset, g_dev.layout.fifoLength, initw);
    for (i = 0; i < GBK_FIFO_INIT_NWRITES; i++) {
        /* M4d: validated on-card via the escape ladder */
        GBK_WR32(g_dev.bar0, initw[i].barOffset, initw[i].value);
    }
    GBK_WMB();

    /* 3. FIFO software state mirrors the just-armed hardware ring. */
    gbk_fifo_init(&g_dev.fifo, g_dev.layout.fifoOffset, g_dev.layout.fifoLength);
    g_dev.attached = 1;                 /* emits are live from here on       */

    /* 4. Y-origin flip (design sec 1b): miscInit0 top = yRes-1. Direct PIO
     *    (IO-space register, not FIFO-routed). */
    /* M4d: validated on-card via the escape ladder */
    miscInit0 = GBK_RD32(g_dev.bar0, GBK_IO(SSTIO_OFF_miscInit0));
    miscInit0 = (miscInit0 & ~SST_YORIGIN_TOP)
              | (((h3u32)(h - 1)) << SST_YORIGIN_TOP_SHIFT);
    GBK_WR32(g_dev.bar0, GBK_IO(SSTIO_OFF_miscInit0), miscInit0);
    GBK_WMB();

    /* 5. Initial 3D buffer registers via the FIFO (design sec 1d). The draw
     *    buffer is the back color buffer; leftOverlayBuf is the front. */
    gbk_emit_reg(SST3D_OFF_colBufferAddr,
                 g_dev.layout.colBufOffset[g_dev.backIdx], eChipBroadcast, 0);
    gbk_emit_reg(SST3D_OFF_colBufferStride, stride, eChipBroadcast, 0);
    if (hasAux) {
        gbk_emit_reg(SST3D_OFF_auxBufferAddr,   g_dev.layout.auxOffset,
                     eChipBroadcast, 0);
        gbk_emit_reg(SST3D_OFF_auxBufferStride, stride, eChipBroadcast, 0);
    }
    gbk_emit_reg(SST3D_OFF_leftOverlayBuf,
                 g_dev.layout.colBufOffset[0], eChipBroadcast, 0);
    gbk_emit_reg(SST3D_OFF_swapbufferCMD, 0, eChipBroadcast, 0);

    /* full-target rect clip (design risk #2: always ENRECTCLIP + clip inside
     * the target, which the default fbzMode carries). Clip register packing
     * per h3hw.h SST_CLIP*_SHIFT; M4d: validated on-card via the escape ladder. */
    colLR = ((h3u32)0 << SST_CLIPLEFT_SHIFT) | ((h3u32)w << SST_CLIPRIGHT_SHIFT);
    colBT = ((h3u32)h << SST_CLIPBOTTOM_SHIFT) | ((h3u32)0 << SST_CLIPTOP_SHIFT);
    gbk_emit_reg(SST3D_OFF_clipLeftRight, colLR, eChipBroadcast, 0);
    gbk_emit_reg(SST3D_OFF_clipBottomTop, colBT, eChipBroadcast, 0);

    /* 6. assertDefaultState replay (verified gbk_state_defaults) + first
     *    flush - pushes the whole shadow to the chip as PKT4 groups. */
    gbk_state_defaults(&g_dev.state, hasAux);
    gbk_flush_state();

    /* 7. initial FASTFILL clear to black. */
    gbk_do_clear(0, 0, 0);

    return g_dev.faulted ? -1 : 0;
}

void
gbkernel_detach(void)
{
    if (!g_dev.attached)
        return;
    /* disable the CMDFIFO (baseSize=0) - as hwcRestoreVideo (design sec 1c). */
    /* M4d: validated on-card via the escape ladder */
    GBK_WR32(g_dev.bar0, GBK_CREG(SSTC_OFF_cmdFifo0_baseSize), 0);
    GBK_WMB();
    g_dev.attached = 0;
}

/* Layout query for the M4c-2 DDraw surface path (enable.c). Pure state copy,
 * no MMIO - safe from any DDI entry. Refuses when the backend is down so the
 * caller never builds surfaces on a stale carve-out. */
int
gbkernel_get_meminfo(gbk_meminfo_t *out)
{
    if (out == 0)
        return -1;
    if (!g_dev.attached) {
        out->vramBytes = 0;
        out->surfSize  = 0;
        return -1;
    }
    out->vramBytes  = g_dev.vramBytes;
    out->desktopEnd = g_dev.desktopEnd;
    out->stride3d   = (unsigned)g_dev.layout.stride;
    out->backOffset = (unsigned)g_dev.layout.colBufOffset[g_dev.backIdx];
    out->auxOffset  = (g_dev.layout.nAuxBuffers > 0)
                    ? (unsigned)g_dev.layout.auxOffset : 0;
    out->surfOffset = (unsigned)g_dev.surfOffset;
    out->surfSize   = (unsigned)g_dev.surfSize;
    out->width      = (unsigned)g_dev.w;
    out->height     = (unsigned)g_dev.h;
    out->desktopBpp = g_dev.desktopBpp;
    return 0;
}

/* ======================================================================== */
/* gb_* fixed-function state (design sec 3) - thin wrappers over the         */
/* verified gbk_state setters: update shadow + mark dirty. The dirty state   */
/* is flushed lazily at the next draw (one PKT4 group).                     */
/* ======================================================================== */
void gb_set_shade(gb_shade_t s)
{
    gbk_state_set_shade(&g_dev.state, s == GB_SHADE_GOURAUD);
}
void gb_set_depth(int enable, int func, int write)
{
    gbk_state_set_depth(&g_dev.state, enable, func, write);
}
void gb_set_blend(int enable, int src, int dst)
{
    gbk_state_set_blend(&g_dev.state, enable, src, dst);
}
void gb_set_alphatest(int enable, int func, unsigned char ref)
{
    gbk_state_set_alphatest(&g_dev.state, enable, func, (h3u32)ref);
}
void gb_set_fog(int enable, unsigned rgb, float density)
{
    /* density is the user-mode generator's linear coefficient; the fog TABLE
     * is downloaded separately (design sec 3) - the mode/color lift ignores
     * it. Cast to void WITHOUT touching the value (no FPU). */
    (void)density;
    gbk_state_set_fog(&g_dev.state, enable, (h3u32)rgb);
}
void gb_set_cull(gb_cull_t c)
{
    gbk_state_set_cull(&g_dev.state, (int)c);   /* 0 none,1 CW,2 CCW         */
}
void gb_set_dither(int enable)
{
    gbk_state_set_dither(&g_dev.state, enable);
}
void gb_set_chromakey(int enable, unsigned rgb)
{
    gbk_state_set_chromakey(&g_dev.state, enable, (h3u32)rgb);
}
void gb_set_texfactor(unsigned argb)
{
    gbk_state_set_texfactor(&g_dev.state, (h3u32)argb);
}
void gb_set_texfilter(int bilinear, int clamp_u, int clamp_v)
{
    gbk_state_set_texfilter(&g_dev.state, bilinear, clamp_u, clamp_v);
}

/* ======================================================================== */
/* Kernel FPU bracket (design risk #4). The ONLY float ARITHMETIC in this    */
/* transport is the perspective-ST scale on the TEXTURED draw path           */
/* (gbk_vertex_words: v->s * sscale * v->w). It dirties the interrupted       */
/* thread's x87/MMX/SSE state, so every DDI entry that can reach it MUST save */
/* the FP state first and restore it after, or a preempted user thread's FP  */
/* state is corrupted. We bracket INSIDE the draw functions themselves rather */
/* than trusting an external caller (M4a/M4d), so the guarantee holds no      */
/* matter who drives the seam. (Texture bind is a DISTINCT DDI op a per-draw  */
/* bracket would NOT cover, so gb_tex_bind is kept FPU-free instead - it      */
/* builds sscale/tscale as raw float bits via gbk_u_to_f32bits.)             */
/*                                                                           */
/* EngSaveFloatingPointState/EngRestoreFloatingPointState are the public DDK */
/* graphics-engine services (winddi.h), declared locally - as crtshim.c does */
/* for EngAllocMem - so the transport stays header-isolated (no <windows.h>, */
/* no <glide.h>). APIENTRY == __stdcall. The save area is a fixed 512-byte    */
/* (>= x86 FXSAVE), 16-byte-aligned buffer; if the save fails the caller     */
/* SKIPS the float work rather than corrupting FP state.                     */
/* ======================================================================== */
extern unsigned long __stdcall
    EngSaveFloatingPointState(void *pBuffer, unsigned long cjBufferSize);
extern int __stdcall
    EngRestoreFloatingPointState(void *pBuffer);

#define GBK_FPSTATE_BYTES  512UL        /* >= x86 FXSAVE (512) save region    */

typedef struct gbk_fpstate {
    unsigned char raw[GBK_FPSTATE_BYTES + 16]; /* +16 slack to 16-byte align  */
    void         *ptr;                  /* aligned save area within raw[]     */
    int           saved;                /* 1 while a save is live             */
} gbk_fpstate_t;

/* Enter the bracket: save the interrupted thread's FP state. Returns 1 on
 * success (float math is now safe), 0 if the engine could not save it (caller
 * MUST NOT touch the FPU). */
static int
gbk_fpu_enter(gbk_fpstate_t *fp)
{
    h3u32 a = (h3u32)fp->raw;                  /* i386 target: ptr is 32-bit  */
    fp->ptr = (void *)((a + 15UL) & ~15UL);    /* 16-byte align               */
    /* M4d: validated on-card via the escape ladder */
    if (EngSaveFloatingPointState(fp->ptr, GBK_FPSTATE_BYTES) == 0) {
        fp->saved = 0;
        return 0;
    }
    fp->saved = 1;
    return 1;
}

static void
gbk_fpu_leave(gbk_fpstate_t *fp)
{
    if (fp->saved) {
        /* M4d: validated on-card via the escape ladder */
        EngRestoreFloatingPointState(fp->ptr);
        fp->saved = 0;
    }
}

/* ======================================================================== */
/* Geometry (design sec 2). Flush dirty state, then emit PKT3 triangle       */
/* packets (batched, split at SSTCP_PKT3_MAXVERTS) with the fixed per-vertex */
/* float layout. Colors come from the integer bit table (FPU-free); x/y/ooz/ */
/* oow are raw dword copies; only the TEXTURED ST scale is float (risk #4),  */
/* and the draw functions bracket it with gbk_fpu_enter/leave.               */
/* ======================================================================== */

/* write one vertex's words at w[], return the count (8 untex / 10 tex). */
static int
gbk_vertex_words(const gb_vtx_t *v, h3u32 *w, int textured)
{
    int k = 0;

    /* raw dword copies - no arithmetic, no FPU (design: copied as words) */
    w[k++] = GBK_F32(v->x);
    w[k++] = GBK_F32(v->y);
    /* colors 0-255 as floats, from the integer-built bit table */
    w[k++] = g_ub2f[v->r];
    w[k++] = g_ub2f[v->g];
    w[k++] = g_ub2f[v->b];
    w[k++] = g_ub2f[v->a];
    w[k++] = GBK_F32(v->ooz);       /* Z param (SST_SETUP_Z)                 */
    w[k++] = GBK_F32(v->w);         /* Wfbi param, oow = 1/w (SST_SETUP_Wfbi)*/

    if (textured) {
        /* --- design risk #4: the ONLY float ARITHMETIC in this file ---
         * perspective-correct ST: s * texW * oow (mirrors the on-card-
         * validated glidebackend.c:322-326). Untextured draws never reach
         * here. The caller (gb_draw_tris/lines/points) has already entered the
         * gbk_fpu_enter/leave bracket around this whole loop when textured, so
         * this FPU use can't corrupt the interrupted thread's x87/SSE state. */
        float s = v->s * g_dev.sscale * v->w;
        float t = v->t * g_dev.tscale * v->w;
        w[k++] = GBK_F32(s);
        w[k++] = GBK_F32(t);
    }
    return k;
}

void
gb_draw_tris(const gb_vtx_t *v, int count)
{
    /* hdr + up to 15 verts * 10 words */
    h3u32 buf[1 + SSTCP_PKT3_MAXVERTS * GBK_VWORDS_TEX];
    gbk_fpstate_t fp;
    int   textured, ntri, tri, batchTris, bv, i, k;

    if (!g_dev.attached || g_dev.faulted || count < 3)
        return;

    gbk_flush_state();
    textured = (g_dev.state.fbzColorPath & SST_ENTEXTUREMAP) != 0;

    /* design risk #4: only the TEXTURED path does float ARITHMETIC
     * (gbk_vertex_words). Bracket the whole batch in the FP save/restore;
     * untextured draws touch no FPU, so skip it. A failed save => skip the
     * draw rather than corrupt the interrupted thread's FP state. */
    if (textured && !gbk_fpu_enter(&fp))
        return;

    ntri = count / 3;
    tri = 0;
    while (tri < ntri) {
        batchTris = ntri - tri;
        if (batchTris > GBK_TRIS_PER_PKT)
            batchTris = GBK_TRIS_PER_PKT;
        bv = batchTris * 3;

        k = 0;
        buf[k++] = gbk_state_tri_hdr(&g_dev.state, textured, (h3u32)bv);
        for (i = 0; i < bv; i++)
            k += gbk_vertex_words(&v[(tri * 3) + i], &buf[k], textured);
        gbk_emit(buf, k);

        tri += batchTris;
    }

    if (textured)
        gbk_fpu_leave(&fp);
}

/* Lines and points use the same PKT3 setup engine with a 2- / 1-vertex
 * primitive (SSTCP_PKT3 nVerts carries the count; the setup unit rasterizes
 * lines/points from the same window-space vertices). The fxD3D core only
 * reaches these for D3DPT_LINELIST / D3DPT_POINTLIST, which games rarely emit;
 * they share gbk_vertex_words so the vertex float layout stays identical.
 * M4d: primitive-type rasterization validated on-card via the escape ladder. */
void
gb_draw_lines(const gb_vtx_t *v, int count)
{
    h3u32 buf[1 + 2 * GBK_VWORDS_TEX];
    gbk_fpstate_t fp;
    int   textured, i, k, pair;

    if (!g_dev.attached || g_dev.faulted || count < 2)
        return;

    gbk_flush_state();
    textured = (g_dev.state.fbzColorPath & SST_ENTEXTUREMAP) != 0;

    /* design risk #4: bracket the textured float path (see gb_draw_tris). */
    if (textured && !gbk_fpu_enter(&fp))
        return;

    for (pair = 0; pair + 1 < count; pair += 2) {
        k = 0;
        buf[k++] = gbk_state_tri_hdr(&g_dev.state, textured, 2);
        for (i = 0; i < 2; i++)
            k += gbk_vertex_words(&v[pair + i], &buf[k], textured);
        gbk_emit(buf, k);
    }

    if (textured)
        gbk_fpu_leave(&fp);
}

void
gb_draw_points(const gb_vtx_t *v, int count)
{
    h3u32 buf[1 + GBK_VWORDS_TEX];
    gbk_fpstate_t fp;
    int   textured, i, k;

    if (!g_dev.attached || g_dev.faulted || count < 1)
        return;

    gbk_flush_state();
    textured = (g_dev.state.fbzColorPath & SST_ENTEXTUREMAP) != 0;

    /* design risk #4: bracket the textured float path (see gb_draw_tris). */
    if (textured && !gbk_fpu_enter(&fp))
        return;

    for (i = 0; i < count; i++) {
        k = 0;
        buf[k++] = gbk_state_tri_hdr(&g_dev.state, textured, 1);
        k += gbk_vertex_words(&v[i], &buf[k], textured);
        gbk_emit(buf, k);
    }

    if (textured)
        gbk_fpu_leave(&fp);
}

void gb_set_vertex_layout(void)
{
    /* no-op: the PKT3 vertex order is fixed by pmask (design sec 2), not a
     * programmable grVertexLayout. gbk_vertex_words emits that fixed order. */
}

/* ======================================================================== */
/* Clear + present (design sec 5).                                          */
/* ======================================================================== */
void
gb_clear(unsigned r, unsigned g, unsigned b)
{
    gbk_do_clear(r, g, b);
}

/* Blit-present (design sec 5A): a 2D WAX screen-to-screen copy of the back
 * color buffer to the desktop primary (offset 0). GDI survives; works
 * windowed + fullscreen; no vsync at bring-up (grDRIBufferSwap gglide.c:
 * 1216-1257). PKT4-WAX groups; the trailing `command` write initiates. */
void
gb_swap(int vsync)
{
    gbk_rw_t dst[2], src[2], clip[2], rect[4];
    h3u32    backOff, stride, dstFmt;
    long     it;

    (void)vsync;                        /* no vsync/flip at bring-up         */
    if (!g_dev.attached || g_dev.faulted)
        return;

    /* present dst format (host-verified gbk_present_dstformat): the desktop
     * stride packed with the pixfmt for the desktop depth. On a 32bpp
     * desktop the 2D engine expands 565->8888 during the SRCCOPY blit -
     * this IS the design-5a 16->32 present-convert. 0 = unpresentable
     * combination; skip rather than program a corrupt format. */
    dstFmt = gbk_present_dstformat(g_dev.desktopStride, g_dev.desktopBpp);
    if (dstFmt == 0)
        return;

    /* throttle in-flight swaps (design sec 5): wait until the pending-swap
     * count in status drains, capped so a stuck engine can't hang. */
    for (it = 0; it < GBK_STATUS_POLL_CAP; it++) {
        /* M4d: validated on-card via the escape ladder */
        h3u32 st = GBK_RD32(g_dev.bar0, GBK_IO(SSTIO_OFF_status));
        if (gbk_swaps_pending(st) == 0)
            break;
    }

    backOff = g_dev.layout.colBufOffset[g_dev.backIdx];
    stride  = g_dev.layout.stride;          /* 16bpp 3D color-buffer pitch    */

    /* dst = desktop primary at offset 0 (BIT31 = FB-offset flag, as
     * _grBufferClear2D gglide.c:744). group base dstBaseAddr(0x010). The dst
     * PITCH is the DESKTOP stride (M4b-2 minor #2), not the 3D color-buffer
     * stride: when the GDI primary is a different width/bpp than the 3D render
     * buffer the two pitches differ, and the blit must step the desktop by its
     * own pitch or it tears. The dst PIXFMT tracks the desktop depth (dstFmt,
     * computed above): 16bpp = plain 565 copy, 32bpp = the design-5a 16->32
     * present-convert done by the 2D engine's format conversion. */
    dst[0].off = SSTG_OFF_dstBaseAddr; dst[0].val = 0UL | H3BIT(31);
    dst[1].off = SSTG_OFF_dstFormat;   dst[1].val = dstFmt;
    gbk_emit_group(SSTG_OFF_dstBaseAddr, eChipBroadcast, 1, dst, 2);

    /* full-surface 2D clip. group base clip0min(0x008). */
    clip[0].off = SSTG_OFF_clip0min; clip[0].val = 0UL;
    clip[1].off = SSTG_OFF_clip0max;
    clip[1].val = ((h3u32)g_dev.w & 0x1FFFUL)
                | (((h3u32)g_dev.h & 0x1FFFUL) << 16);
    gbk_emit_group(SSTG_OFF_clip0min, eChipBroadcast, 1, clip, 2);

    /* src = back color buffer, 565. group base srcBaseAddr(0x034):
     * srcBaseAddr slot0, srcFormat slot8 (gglide.c:1224-1228). */
    src[0].off = SSTG_OFF_srcBaseAddr; src[0].val = backOff | H3BIT(31);
    src[1].off = SSTG_OFF_srcFormat;   src[1].val = stride | SSTG_PIXFMT_16BPP;
    gbk_emit_group(SSTG_OFF_srcBaseAddr, eChipBroadcast, 1, src, 2);

    /* the copy rect (single full-screen rect). group base srcXY(0x05C):
     * srcXY slot0, dstSize slot3, dstXY slot4, command slot5 (gglide.c:
     * 1237-1246). command = SRCCOPY | BLT | GO -> initiates the blit. */
    rect[0].off = SSTG_OFF_srcXY;   rect[0].val = 0UL;      /* src (0,0)     */
    rect[1].off = SSTG_OFF_dstSize;
    rect[1].val = ((h3u32)g_dev.w & 0x1FFFUL)
                | (((h3u32)g_dev.h & 0x1FFFUL) << 16);
    rect[2].off = SSTG_OFF_dstXY;   rect[2].val = 0UL;      /* dst (0,0)     */
    rect[3].off = SSTG_OFF_command;
    rect[3].val = SSTG_ROP_SRCCOPY | SSTG_BLT | SSTG_GO;
    gbk_emit_group(SSTG_OFF_srcXY, eChipBroadcast, 1, rect, 4);
}

/* gb_finish (design sec 5): drain the FIFO - nopCMD=0 then poll status&BUSY
 * with the kernel iteration cap (design risk #1). */
void
gb_finish(void)
{
    long it;

    if (!g_dev.attached || g_dev.faulted)
        return;

    gbk_emit_reg(SST3D_OFF_nopCMD, 0, eChipBroadcast, 0);
    for (it = 0; it < GBK_STATUS_POLL_CAP; it++) {
        /* M4d: validated on-card via the escape ladder */
        h3u32 st = GBK_RD32(g_dev.bar0, GBK_IO(SSTIO_OFF_status));
        if (!gbk_status_busy(st))
            return;
    }
    gbk_fault();                        /* timeout: fail safe, never hang    */
}

/* ======================================================================== */
/* Textures (design sec 4).                                                 */
/* ======================================================================== */

static struct gb_tex *
gbk_tex_alloc_slot(void)
{
    int i;
    for (i = 0; i < GBK_MAX_TEX; i++) {
        if (!g_texPool[i].inuse) {
            g_texPool[i].inuse = 1;
            return &g_texPool[i];
        }
    }
    return (struct gb_tex *)0;
}

/* SST texture-format bits for textureMode (h3hw.h SST_TFORMAT_*). P8 is
 * deferred (needs an nccTable palette download, design sec 4): treated as a
 * raw 8bpp format for the download; convert P8->565 host-side for bring-up. */
static h3u32
gbk_tex_format_bits(gb_texfmt_t fmt)
{
    switch (fmt) {
    case GB_TF_ARGB1555: return SST_ARGB1555;
    case GB_TF_ARGB4444: return SST_ARGB4444;
    case GB_TF_P8:       return SST_P8;
    case GB_TF_RGB565:
    default:             return SST_RGB565;
    }
}

/* tLOD lodmin=lodmax + aspect for a single-level texture, reduced from
 * grTexSource (gtex.c:1352-1369): LOD = log2 of the larger dimension in the
 * 4.2 fixed field, aspect = |log2 w - log2 h| (0..3), S_IS_WIDER when w>h.
 * M4d: exact LOD encoding validated on-card via the escape ladder. */
static h3u32
gbk_ilog2(h3u32 v)
{
    h3u32 r = 0;
    while (v > 1UL) { v >>= 1; r++; }
    return r;
}

static h3u32
gbk_tex_lod_bits(int w, int h)
{
    h3u32 lw = gbk_ilog2((h3u32)w);
    h3u32 lh = gbk_ilog2((h3u32)h);
    h3u32 large = (lw > lh) ? lw : lh;
    h3u32 small = (lw < lh) ? lw : lh;
    h3u32 aspect = large - small;               /* 0..N aspect steps         */
    h3u32 tlod;

    if (aspect > 0x3UL)                          /* SST_LOD_ASPECT is 2 bits  */
        aspect = 0x3UL;
    /* lodmin == lodmax == large, in the 4.2 fixed field (<<2 = *4 per LOD). */
    tlod = ((large << 2) << SST_LODMIN_SHIFT) & SST_LODMIN;
    tlod |= ((large << 2) << SST_LODMAX_SHIFT) & SST_LODMAX;
    tlod |= (aspect << SST_LOD_ASPECT_SHIFT) & SST_LOD_ASPECT;
    if (lw > lh)
        tlod |= SST_LOD_S_IS_WIDER;
    return tlod;
}

gb_tex_t *
gb_tex_create(int w, int h, gb_texfmt_t fmt)
{
    struct gb_tex *t;
    h3u32 bpp, bytes, addr, tramTop;

    /* Bound to the Voodoo3 TMU maximum (256x256). Besides being the real HW
     * cap, this makes `bytes = w*h*bpp` (below) provably non-overflowing in
     * 32-bit (256*256*2 = 128 KB), so the tram-fit guard can't be defeated by a
     * multiply that wraps to a small value. */
    if (!g_dev.attached || g_dev.faulted ||
        w <= 0 || h <= 0 || w > 256 || h > 256)
        return (gb_tex_t *)0;

    t = gbk_tex_alloc_slot();
    if (t == (struct gb_tex *)0)
        return (gb_tex_t *)0;

    bpp = (fmt == GB_TF_P8) ? 1UL : 2UL;
    bytes = (h3u32)w * (h3u32)h * bpp;

    /* 16-byte-aligned bump allocation in tram (SST_TEXTURE_ADDRESS
     * granularity, design sec 4). The ceiling is tramLimit, NOT the top of
     * tram: everything above it is the reserved DDraw surface region
     * (M4c-2 carve in gbkernel_attach). */
    addr = (g_dev.tramNext + 15UL) & ~15UL;
    tramTop = g_dev.tramLimit;
    if (addr + bytes > tramTop || addr + bytes < addr) {
        t->inuse = 0;                    /* out of tram: hand back the slot   */
        return (gb_tex_t *)0;
    }
    g_dev.tramNext = addr + bytes;

    t->w       = w;
    t->h       = h;
    t->fmt     = (int)fmt;
    t->addr    = addr;
    t->bytes   = bytes;
    t->tformat = gbk_tex_format_bits(fmt);
    t->tlod    = gbk_tex_lod_bits(w, h);
    return t;
}

/* Upload straight into board memory at the texture's tram offset via a PKT5
 * LFB linear-write, wrapped in the MANDATORY Avenger download-coherency
 * errata flush (design sec 4): pre 2D NOP; then texBaseAddr toggle + 3D nop;
 * 2D NOP again. The texel stream is copied as raw 32-bit words (no float). */
void
gb_tex_upload(gb_tex_t *th, const void *pixels)
{
    struct gb_tex *t = (struct gb_tex *)th;
    const h3u32 *src;
    h3u32 nWords, nBytes, base, i;

    if (!g_dev.attached || g_dev.faulted || t == 0 || pixels == 0)
        return;

    src = (const h3u32 *)pixels;
    nWords = t->bytes >> 2;             /* power-of-2 texel arrays are dword- */
    if (nWords == 0)                    /* aligned; a 1x1 (2 B) is skipped    */
        return;

    /* pre-errata: 2D command NOP|GO (GR_TEX_FLUSH_PRE, fxglide.h:1105-1121). */
    gbk_emit_reg(SSTG_OFF_command, SSTG_NOP | SSTG_GO, eChipBroadcast, 1);

    /* PKT5 linear write: reserve hdr(2) + nWords, then stream the texels
     * directly into the ring (too large for a stack word buffer). */
    nBytes = (2UL + nWords) * 4UL;
    base = gbk_fifo_make_room(nBytes);
    if (base == (h3u32)-1)
        return;
    /* M4d: validated on-card via the escape ladder */
    GBK_WR32(g_dev.bar1, base,
             gbk_pkt5_hdr1(SSTCP_PKT5_LFB, nWords, 0xFUL, 0xFUL));
    GBK_WR32(g_dev.bar1, base + 4UL, gbk_pkt5_hdr2(t->addr));
    for (i = 0; i < nWords; i++) {
        /* M4d: validated on-card via the escape ladder */
        GBK_WR32(g_dev.bar1, base + ((2UL + i) * 4UL), src[i]);
    }
    GBK_WMB();
    gbk_fifo_advance(&g_dev.fifo, nBytes);

    /* post-errata: force a texBaseAddr toggle + nop so the TMU refetches the
     * just-written texels (design sec 4; gtexdl.c:1237-1244,1355-1368). */
    gbk_emit_reg(SST3D_OFF_texBaseAddr, ~t->addr, eChipTMU0, 0);
    gbk_emit_reg(SST3D_OFF_nopCMD, 0, eChipBroadcast, 0);
    gbk_emit_reg(SST3D_OFF_texBaseAddr, t->addr, eChipTMU0, 0);
    gbk_emit_reg(SSTG_OFF_command, SSTG_NOP | SSTG_GO, eChipBroadcast, 1);
}

/* Bind (design sec 4): merge the texture's format + persp/clampW into the
 * textureMode shadow (keeping filter/clamp from gb_set_texfilter), set
 * texBaseAddr + tLOD, and switch the color-combine to textured. The TMU PKT4
 * group + fbzColorPath change flush at the next draw. */
void
gb_tex_bind(gb_tex_t *th)
{
    struct gb_tex *t = (struct gb_tex *)th;

    if (t == 0)
        return;

    g_dev.state.texBaseAddr = t->addr & SST_TEXTURE_ADDRESS;
    g_dev.state.textureMode = (g_dev.state.textureMode & ~SST_TFORMAT)
                            | SST_TPERSP_ST | SST_TCLAMPW | t->tformat;
    g_dev.state.tLOD = (g_dev.state.tLOD & ~(SST_LODMIN | SST_LODMAX |
                                             SST_LOD_ASPECT | SST_LOD_S_IS_WIDER))
                     | t->tlod;
    g_dev.state.dirty |= GBK_DIRTY_textureMode | GBK_DIRTY_tLOD |
                         GBK_DIRTY_texBaseAddr;

    /* color-combine -> textured (verified gbk_state_set_texenable; its
     * ENTEXTUREMAP transition makes the next flush prepend nopCMD=0). */
    (void)gbk_state_set_texenable(&g_dev.state, 1);

    /* design risk #4: the ST scales are floats consumed by the (bracketed)
     * per-vertex multiply in gbk_vertex_words. Build (float)texW / (float)texH
     * as raw IEEE-754 bits with PURE INTEGER math (gbk_u_to_f32bits; w,h are
     * powers of two <= 2048) and store them via the bit pattern, so bind
     * itself touches NO FPU. Bind is a DISTINCT DDI op from a draw batch - a
     * per-draw FP bracket would not cover a (float) cast here - so keeping it
     * FPU-free is what makes the "only float math is the bracketed vertex
     * loop" invariant actually hold. (Type-pun is well-defined at /Od, as the
     * GBK_F32 reads elsewhere in this file rely on.) */
    *(h3u32 *)&g_dev.sscale = gbk_u_to_f32bits((h3u32)t->w);
    *(h3u32 *)&g_dev.tscale = gbk_u_to_f32bits((h3u32)t->h);
}

void
gb_tex_none(void)
{
    (void)gbk_state_set_texenable(&g_dev.state, 0);
}

void
gb_tex_destroy(gb_tex_t *th)
{
    struct gb_tex *t = (struct gb_tex *)th;
    /* the bump allocator does not reclaim mid-stream (a frame's textures are
     * freed together at context teardown / re-attach, which resets tramNext);
     * just release the handle slot. */
    if (t != 0)
        t->inuse = 0;
}

/* ======================================================================== */
/* Lifecycle (design sec 6: "thin - the chassis drives mode via attach").   */
/* In the driver, gb_startup/open/close/shutdown are minimal; the real board */
/* bring-up is gbkernel_attach, called from DrvEnableSurface/EnableDirectDraw.*/
/* ======================================================================== */
int
gb_startup(char *name_out, int name_len, int *num_chips_out)
{
    static const char nm[] = "Voodoo3 (fxD3D kernel backend)";
    int i;
    if (name_out != 0 && name_len > 0) {
        for (i = 0; i < name_len - 1 && nm[i] != 0; i++)
            name_out[i] = nm[i];
        name_out[i] = 0;
    }
    if (num_chips_out != 0)
        *num_chips_out = 1;
    return 0;
}

void
gb_shutdown(void)
{
    /* board teardown is gbkernel_detach (driven by the chassis on disable). */
}

int
gb_open(const gb_mode_t *m)
{
    /* record the requested mode; the real work (layout + FIFO + state) is
     * gbkernel_attach once the chassis has BAR0/BAR1 mapped. */
    if (m != 0) {
        g_dev.w = m->width;
        g_dev.h = m->height;
        g_dev.depthBytes = m->z_buffer ? 2 : 0;
    }
    return 0;
}

void
gb_close(void)
{
    /* nothing to release here; teardown is gbkernel_detach. */
}

int
gb_enum_modes(gb_mode_t *out, int max)
{
    /* the paired miniport enumerates real modes (chassis DrvGetModes); the
     * backend reports none of its own. */
    (void)out;
    (void)max;
    return 0;
}

/* ======================================================================== */
/* Bring-up ladder implementations (design sec 6). These are driven from the */
/* DrvEscape handler (gbkdebug.c) so the agent validates each transport layer */
/* on real hardware BEFORE D3D does - every "M4d: validated on-card via the   */
/* escape ladder" tag in this file is discharged by one of these rungs. They  */
/* touch g_dev + the card directly (same TU as the transport) and reuse the   */
/* iteration-capped FIFO/fault path, so a wedged card fails safe, never hangs.*/
/* ======================================================================== */

int
gbkernel_dbg_faulted(void)
{
    return g_dev.faulted ? 1 : 0;
}

/* Rung 1 - PROBE: read status + cmdFifo0 registers (DIRECT PIO, no FIFO
 * submit, no draw) and report the computed layout + flags. Proves the BAR0
 * mapping and that the CMDFIFO init took, WITHOUT exercising the draw path. */
int
gbkernel_dbg_probe(fxdbg_probe_t *out)
{
    if (out == 0)
        return 0;

    /* zero everything first so unread fields are deterministic */
    {
        char *p = (char *)out;
        unsigned i;
        for (i = 0; i < sizeof(*out); i++)
            p[i] = 0;
    }
    out->magic         = FXDBG_MAGIC;
    out->attached      = (unsigned long)g_dev.attached;
    out->faulted       = (unsigned long)g_dev.faulted;
    out->vramBytes     = g_dev.vramBytes;
    out->desktopEnd    = g_dev.desktopEnd;
    out->desktopStride = g_dev.desktopStride;
    out->tramOffset    = g_dev.layout.tramOffset;
    out->tramSize      = g_dev.layout.tramSize;
    out->fifoOffset    = g_dev.layout.fifoOffset;
    out->fifoLength    = g_dev.layout.fifoLength;
    out->stride        = g_dev.layout.stride;
    out->colBuf0       = g_dev.layout.colBufOffset[0];
    out->colBuf1       = g_dev.layout.colBufOffset[1];
    out->colBuf2       = g_dev.layout.colBufOffset[2];
    out->nColBuffers   = g_dev.layout.nColBuffers;
    out->auxOffset     = g_dev.layout.auxOffset;
    out->nAuxBuffers   = g_dev.layout.nAuxBuffers;
    out->backIdx       = (unsigned long)g_dev.backIdx;
    out->width         = (unsigned long)g_dev.w;
    out->height        = (unsigned long)g_dev.h;
    out->depthBytes    = (unsigned long)g_dev.depthBytes;
    out->swWriteOfs    = g_dev.fifo.writeOfs;
    out->swReadOfs     = g_dev.fifo.readOfs;

    /* register reads only when a mapping exists; reading a wedged (faulted)
     * card is still safe - the registers answer even after baseSize=0. */
    if (g_dev.attached && g_dev.bar0 != 0) {
        /* M4d: validated on-card via the escape ladder */
        out->status3d     = GBK_RD32(g_dev.bar0, GBK_3D(SST3D_OFF_status));
        out->statusio     = GBK_RD32(g_dev.bar0, GBK_IO(SSTIO_OFF_status));
        out->fifoReadPtr  = GBK_RD32(g_dev.bar0,
                                     GBK_CREG(SSTC_OFF_cmdFifo0_readPtrL));
        out->fifoBaseSize = GBK_RD32(g_dev.bar0,
                                     GBK_CREG(SSTC_OFF_cmdFifo0_baseSize));
        out->fifoDepth    = GBK_RD32(g_dev.bar0,
                                     GBK_CREG(SSTC_OFF_cmdFifo0_depth));
    }
    return 1;
}

/* Rung 2 - CLEAR: FASTFILL to <rgb> then blit-present. Proves FIFO execution
 * end-to-end without any triangle setup. No FPU (clear/swap are pure integer). */
int
gbkernel_dbg_clear(unsigned long r, unsigned long g, unsigned long b)
{
    if (!g_dev.attached || g_dev.faulted)
        return 0;
    gb_clear((unsigned)r, (unsigned)g, (unsigned)b);
    gb_swap(0);
    return g_dev.faulted ? 0 : 1;
}

/* Rung 3 - TRI: clear to a dark background, then draw ONE hardcoded gouraud
 * triangle (state via gb_set_*, geometry via gb_draw_tris) and present. Proves
 * the PKT3 + state-flush + submit path. The (float)int vertex-coordinate casts
 * are x87 work, so the WHOLE rung runs inside ONE EngSave/RestoreFloatingPoint
 * region (design risk #4 / M4b-2 minor #9) established here at the ladder
 * entry - gb_draw_tris' own internal bracket is for the textured path and
 * would not cover these casts. */
int
gbkernel_dbg_tri(void)
{
    gbk_fpstate_t fp;
    gb_vtx_t v[3];
    float fw, fh;

    if (!g_dev.attached || g_dev.faulted)
        return 0;

    /* fixed-function state for a flat untextured gouraud triangle */
    gb_tex_none();
    gb_set_shade(GB_SHADE_GOURAUD);
    gb_set_cull(GB_CULL_NONE);
    gb_set_depth(0, GR_CMP_ALWAYS, 0);       /* z off for the bring-up tri    */
    gb_set_blend(0, GR_BLEND_ONE, GR_BLEND_ZERO);
    gb_set_alphatest(0, GR_CMP_ALWAYS, 0);

    /* dark-blue background so the triangle reads clearly on readback */
    gb_clear(0, 0, 40);

    /* One FP region for the whole rung (the casts below are the only x87). A
     * failed save => skip the render rather than corrupt the interrupted
     * thread's FP state. */
    if (!gbk_fpu_enter(&fp))
        return 0;

    fw = (float)g_dev.w;
    fh = (float)g_dev.h;

    /* window-space vertices (origin top-left; Y flipped by miscInit0). RGBA
     * as bytes; ooz=0.5 (z param), w=1.0 (oow, 1/w). */
    v[0].x = fw * 0.25f; v[0].y = fh * 0.80f;
    v[0].r = 255; v[0].g = 0;   v[0].b = 0;   v[0].a = 255;
    v[1].x = fw * 0.50f; v[1].y = fh * 0.20f;
    v[1].r = 0;   v[1].g = 255; v[1].b = 0;   v[1].a = 255;
    v[2].x = fw * 0.75f; v[2].y = fh * 0.80f;
    v[2].r = 0;   v[2].g = 0;   v[2].b = 255; v[2].a = 255;
    v[0].ooz = v[1].ooz = v[2].ooz = 0.5f;
    v[0].w   = v[1].w   = v[2].w   = 1.0f;
    v[0].s = v[0].t = v[1].s = v[1].t = v[2].s = v[2].t = 0.0f;

    gb_draw_tris(v, 3);
    gb_swap(0);

    gbk_fpu_leave(&fp);
    return g_dev.faulted ? 0 : 1;
}

/* Rung 4 - TEX: build a tiny 32x32 RGB565 checker (integer fill, no FPU),
 * upload it (PKT5 + Avenger errata flush), bind it, and draw a textured quad
 * (two tris). Proves the PKT5 texture download + TMU + the textured PKT3 path.
 * The textured vertex ST is x87 work (gbk_vertex_words); the whole rung runs
 * inside ONE FP region here at the ladder entry (design risk #4). */
int
gbkernel_dbg_tex(void)
{
    static h3u16 checker[32 * 32];
    gbk_fpstate_t fp;
    gb_tex_t *tex;
    gb_vtx_t  q[6];
    float     fw, fh;
    int       xx, yy;

    if (!g_dev.attached || g_dev.faulted)
        return 0;

    /* 32x32 RGB565 checker: 4x4 blocks, white / magenta. Pure integer. */
    for (yy = 0; yy < 32; yy++) {
        for (xx = 0; xx < 32; xx++) {
            int on = (((xx >> 2) ^ (yy >> 2)) & 1);
            checker[(yy * 32) + xx] = on ? (h3u16)0xFFFFU   /* white  */
                                         : (h3u16)0xF81FU;  /* magenta*/
        }
    }

    tex = gb_tex_create(32, 32, GB_TF_RGB565);
    if (tex == 0)
        return 0;
    gb_tex_upload(tex, checker);
    gb_set_texfilter(1, 1, 1);               /* bilinear, clamp S+T           */
    gb_tex_bind(tex);                         /* FPU-free by design            */
    gb_set_shade(GB_SHADE_GOURAUD);
    gb_set_cull(GB_CULL_NONE);
    gb_set_depth(0, GR_CMP_ALWAYS, 0);
    gb_set_blend(0, GR_BLEND_ONE, GR_BLEND_ZERO);

    gb_clear(0, 20, 0);                       /* dark-green background         */

    if (!gbk_fpu_enter(&fp)) {
        gb_tex_destroy(tex);
        return 0;
    }

    fw = (float)g_dev.w;
    fh = (float)g_dev.h;

    /* centered quad, UV 0..1, as two BDDBDD triangles (0,1,2) (0,2,3). */
    {
        float x0 = fw * 0.30f, x1 = fw * 0.70f;
        float y0 = fh * 0.25f, y1 = fh * 0.75f;
        gb_vtx_t c0, c1, c2, c3;
        c0.x = x0; c0.y = y0; c0.s = 0.0f; c0.t = 0.0f;
        c1.x = x1; c1.y = y0; c1.s = 1.0f; c1.t = 0.0f;
        c2.x = x1; c2.y = y1; c2.s = 1.0f; c2.t = 1.0f;
        c3.x = x0; c3.y = y1; c3.s = 0.0f; c3.t = 1.0f;
        c0.r = c1.r = c2.r = c3.r = 255;
        c0.g = c1.g = c2.g = c3.g = 255;
        c0.b = c1.b = c2.b = c3.b = 255;
        c0.a = c1.a = c2.a = c3.a = 255;
        c0.ooz = c1.ooz = c2.ooz = c3.ooz = 0.5f;
        c0.w   = c1.w   = c2.w   = c3.w   = 1.0f;
        q[0] = c0; q[1] = c1; q[2] = c2;
        q[3] = c0; q[4] = c2; q[5] = c3;
    }

    gb_draw_tris(q, 6);   /* gb_draw_tris also brackets internally (textured) */
    gb_swap(0);

    gbk_fpu_leave(&fp);
    gb_tex_destroy(tex);
    return g_dev.faulted ? 0 : 1;
}

/* Rung 5 - READBACK: copy a rectangle of framebuffer pixels (BAR1) back to the
 * caller so rendering is verified WITHOUT a GDI screenshot of the 3D surface.
 * which=BACK reads the freshly-rendered back color buffer at the 3D pitch;
 * which=DESKTOP reads the desktop primary (offset 0) at the desktop pitch.
 * 16bpp (565) source assumed - at bring-up the desktop is 16bpp too. Reads
 * are plain volatile 16-bit loads from the NON-CACHED BAR1 (no FIFO, no FPU).
 * Returns the number of bytes written (<= outMax), clamped to the surface. */
unsigned long
gbkernel_dbg_readback(unsigned long which,
                      unsigned long x, unsigned long y,
                      unsigned long w, unsigned long h,
                      void *out, unsigned long outMax)
{
    volatile h3u16 *fb;
    h3u16   *dst = (h3u16 *)out;
    h3u32    base, stride, uw, uh, row, col, rows, cw, written;

    if (out == 0 || outMax < 2 || !g_dev.attached || g_dev.bar1 == 0)
        return 0;
    if (g_dev.w <= 0 || g_dev.h <= 0)
        return 0;

    if (which == FXDBG_RB_DESKTOP) {
        base   = 0;                             /* desktop primary at offset 0 */
        stride = g_dev.desktopStride;
    } else {
        base   = g_dev.layout.colBufOffset[g_dev.backIdx];  /* back buffer     */
        stride = g_dev.layout.stride;
    }

    /* Clamp the ATTACKER-SUPPLIED rect to the surface AND the output buffer with
     * overflow-safe arithmetic (gbk_readback_rows: `w > uw-x`, never `x+w>uw`
     * which wraps and would walk the loop past the BAR1 aperture -> kernel OOB
     * read). rows==0 => nothing safe/room to copy. cw is the clamped width. */
    uw = (h3u32)g_dev.w;
    uh = (h3u32)g_dev.h;
    rows = gbk_readback_rows((h3u32)x, (h3u32)y, (h3u32)w, (h3u32)h,
                             uw, uh, (h3u32)outMax, &cw);
    if (rows == 0)
        return 0;

    written = 0;
    for (row = 0; row < rows; row++) {
        fb = (volatile h3u16 *)(g_dev.bar1 + base + (((h3u32)y + row) * stride)
                                + ((h3u32)x * 2UL));
        for (col = 0; col < cw; col++) {
            /* M4d: validated on-card via the escape ladder */
            dst[written] = fb[col];
            written++;
        }
    }
    return written * 2UL;
}

#endif /* HAVE_DDK */
