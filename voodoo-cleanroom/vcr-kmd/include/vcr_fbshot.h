/*
 * vcr_fbshot.h - turn what the video processor is SCANNING OUT into pixels
 * (tools/vcrctl.c `fbshot`). Win32-free: tests/native/test_vcr_kmd_fbshot.c
 * compiles it as is.
 *
 * Why (2026-09-28, the LAN-party sweep on .124): the agent's SCREENSHOT is a
 * GDI BitBlt of the desktop surface. On our driver an exclusive-fullscreen
 * Glide, OpenGL or Direct3D game renders into buffers of its own, so GDI
 * photographs the old desktop memory reinterpreted - garbage or black - and a
 * sweep can only say "still running". The scanout registers say which memory
 * is on the monitor: vidDesktopStartAddr (a flip writes it), the stride, the
 * size and the pixel format in vidProcCfg. Reading that memory is the frame.
 *
 * Two layers: the DESKTOP (GDI, DirectDraw, Direct3D) and the OVERLAY - a
 * fullscreen Glide game turns the desktop off and scans its front buffer out
 * through the overlay (tiled). Only one is read: a desktop+overlay composite
 * (DOSBox `output=overlay`, Glide's AA secondary buffer) is reported, not
 * composited. 8 bpp goes through the CLUT bank vidProcCfg selects; 15/16/24/
 * 32 bpp are raw (the identity/gamma CLUT is not applied).
 *
 * Tiles (Banshee/Voodoo3/VSA-100): 128 bytes x 32 lines, row-major, and in
 * tiled mode the stride register counts TILES (bits 6:0), not bytes.
 *
 * ---- memBase1 is NOT raw memory above the tile aperture (2026-09-28) --------
 * lfbMemoryConfig (IO 0x0c) opens a TILE APERTURE: from its begin page up,
 * memBase1 is a LINEAR picture of the tiled memory - byte o past the begin
 * page is line o / lfbStride, byte o % lfbStride, of a buffer laid out
 * lfbStride bytes a line - and the chip converts that (x, y) to its tile.
 * Glide programs it so for every fullscreen session (minihwc.c hwcInitVideo):
 * begin page = colBuffStart0[0], lfbStride 8 KB (HWC_RAW_LFB_STRIDE), tile
 * stride = the buffers' stride in tiles. Our kernel pushes it past the end of
 * memory for a linear desktop (0x01803fff, the vendor's value), which is why
 * the desktop read was right and the Glide read was not.
 *
 * The first overlay build read the tiled buffer's RAW tile offsets through
 * that aperture: Quake III on .124 (4-chip SLI, 1280x960x32, vidCurrOverlay-
 * StartAddr 03c3e000 = colBuffStart0[0] = the begin page) came out as narrow
 * column blocks with gaps. A 4 KB tile is half an 8 KB aperture line, so the
 * even tile columns showed a whole screen line folded into 32x32 px and the
 * odd ones mostly ran past the 5120-byte line into nothing. Inverting exactly
 * that mapping on the saved PNG gives back the Quake III main menu - the
 * mechanism is proven on the picture (test: the_first_overlay_read_...).
 *
 * The aperture is also what makes SLI simple: with cfgSliLfbCtrl READ_EN set
 * (our kernel sets it for every SLI session), a read of aperture line y is
 * answered by the chip that owns line y, so the WHOLE frame reads from the
 * master's memBase1 - no slave LFB view needed (there is none: GET_SLAVE_REGS
 * maps 4 KB register pages only). Glide's own grab does exactly this for the
 * 32 bpp case ("let the hardware do the rest", minihwc.c hwcAAReadRegion16).
 * In SLI a buffer's aperture line is its chip-local line << log2(units)
 * (minihwc.c hwcBufferLfbAddr): the second of Quake III's buffers starts 1024
 * aperture lines up, not 256.
 *
 * NEVER read memBase1 while a multi-chip AA session is live: "on the V5 6000
 * in cfg 3 the first such read froze the box" (our Glide fork's h5sliaa.h
 * rule 11, which refuses Glide's own READ locks there). vcr_fb_mb1_gate_board():
 * on a multi-chip board the master's SLI/AA registers are read whatever the
 * kernel's session count, and vcr_fb_mb1_recheck() asks again at every flip
 * and every 64 lines of a read, which stops the moment it says no.
 */
#ifndef VCR_FBSHOT_H
#define VCR_FBSHOT_H

#define VCR_FB_TILE_W       128u       /* bytes */
#define VCR_FB_TILE_H       32u        /* lines */
#define VCR_FB_TILE_BYTES   4096u

/* vidDesktopStartAddr -> a byte offset in the chip's memory. NOT 24 bits: the
 * VSA-100 with 64 MB a chip puts .124's desktop at 0x3b00000, and the first
 * fbshot build masked 0xffffff, read 0xb00000 - texture memory - and wrote a
 * picture of RtCW's textures as "the desktop" (2026-09-28 06:52). */
#define VCR_FB_START_MASK   0x0fffffffUL
static __inline unsigned long vcr_fb_start(unsigned long reg)
{
    return reg & VCR_FB_START_MASK;
}

/* Bytes per pixel for vidProcCfg's desktop format (bits 20:18), 0 = unknown.
 * 4 is RGB1555 on a VSA-100 (h3defs.h SST_DESKTOP_PIXEL_RGB1555U) - the code
 * the overlay's 15 bpp formats are decoded as (vcr_fb_overlay_fmt). */
static __inline unsigned vcr_fb_bytespp(unsigned fmt)
{
    switch (fmt) {
    case 0: return 1;       /* 8 bpp palettized */
    case 1: return 2;       /* RGB565 */
    case 2: return 3;       /* RGB24 */
    case 3: return 4;       /* RGB32 */
    case 4: return 2;       /* RGB1555 */
    default: return 0;
    }
}

/* the row pitch in bytes the stride register means: bytes when linear,
 * tiles (bits 6:0) x 128 when tiled */
static __inline unsigned long vcr_fb_pitch(unsigned long stride_reg, int tiled)
{
    return tiled ? (stride_reg & 0x7fu) * VCR_FB_TILE_W : (stride_reg & 0x7fffu);
}

/* the byte offset, from the start address, of byte `xb` of line `y` - in the
 * chip's RAW memory (below the tile aperture: vcr_fb_plan decides) */
static __inline unsigned long vcr_fb_offset(unsigned long xb, unsigned long y,
                                            unsigned long stride_reg, int tiled)
{
    if (!tiled)
        return y * (stride_reg & 0x7fffu) + xb;
    return ((y / VCR_FB_TILE_H) * (stride_reg & 0x7fu) + xb / VCR_FB_TILE_W) *
               (VCR_FB_TILE_W * VCR_FB_TILE_H) +
           (y % VCR_FB_TILE_H) * VCR_FB_TILE_W + xb % VCR_FB_TILE_W;
}

/* the last byte the frame touches in raw memory, +1 */
static __inline unsigned long vcr_fb_extent(unsigned w, unsigned h, unsigned bpp,
                                            unsigned long stride_reg, int tiled)
{
    if (!w || !h || !bpp)
        return 0;
    if (!tiled)
        return (unsigned long)(h - 1) * (stride_reg & 0x7fffu) + (unsigned long)w * bpp;
    return ((unsigned long)((h - 1) / VCR_FB_TILE_H) + 1) * (stride_reg & 0x7fu) *
           VCR_FB_TILE_W * VCR_FB_TILE_H;
}

/* one pixel's bytes (little-endian, as the chip stores them) -> 0xRRGGBB */
static __inline unsigned long vcr_fb_rgb(unsigned fmt, const unsigned char *p,
                                         const unsigned long *clut)
{
    unsigned long v, r, g, b;
    switch (fmt) {
    case 0:
        return clut ? clut[p[0]] & 0xffffffUL : (unsigned long)p[0] * 0x010101UL;
    case 1:
        v = p[0] | (unsigned long)p[1] << 8;
        r = v >> 11 & 31; g = v >> 5 & 63; b = v & 31;
        return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    case 2:
        return (unsigned long)p[2] << 16 | (unsigned long)p[1] << 8 | p[0];
    case 3:
        return (unsigned long)p[2] << 16 | (unsigned long)p[1] << 8 | p[0];
    case 4:
        v = p[0] | (unsigned long)p[1] << 8;
        r = v >> 10 & 31; g = v >> 5 & 31; b = v & 31;
        return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    default:
        return 0;
    }
}

/* ---- the overlay layer ------------------------------------------------------
 * Quake III on .124 (4-chip SLI, 2026-09-28 06:55): vidProcCfg 026c0101 =
 * desktop OFF, overlay ON and tiled, overlay format RGB32; stride register
 * 00280028 (the overlay's is the high half, 0x28 = 40 tiles = 5120 bytes); the
 * scanned-out buffer is vidCurrOverlayStartAddr. */
static __inline unsigned long vcr_fb_overlay_stride(unsigned long stride_reg)
{
    return (stride_reg >> 16) & 0x7fffu;
}

/* bytes per pixel the overlay's pitch implies over its width: 2 or 4, else 0.
 * A cross-check of the format code, which is what decides. */
static __inline unsigned vcr_fb_overlay_bytespp(unsigned long pitch, unsigned w)
{
    if (!w)
        return 0;
    if (pitch >= (unsigned long)w * 4 && pitch < (unsigned long)w * 4 + VCR_FB_TILE_W)
        return 4;
    if (pitch >= (unsigned long)w * 2 && pitch < (unsigned long)w * 2 + VCR_FB_TILE_W)
        return 2;
    return 0;
}

/* vidProcCfg's overlay format (bits 23:21, h3defs.h SST_OVERLAY_PIXEL_*) as
 * the desktop code vcr_fb_rgb decodes: RGB32U -> 3, RGB565U/565D -> 1,
 * RGB1555U/1555D -> 4. YUV (4-6) -> 7, which vcr_fb_bytespp refuses. */
#define VCR_FB_OVL_FMT_SHIFT    21
static __inline unsigned vcr_fb_overlay_fmt(unsigned long vpc)
{
    switch ((unsigned)(vpc >> VCR_FB_OVL_FMT_SHIFT & 7)) {
    case 0: case 2: return 4;
    case 1: case 7: return 1;
    case 3: return 3;
    default: return 7;
    }
}

/* ---- the tile aperture (lfbMemoryConfig, IO 0x0c) -----------------------------
 * begin page bits 12:0 at 12:0 and 14:13 at 24:23 (h3defs.h
 * SST_RAW_LFB_TILE_BEGIN_PAGE_MUNGE), the aperture's line stride 1 KB << n at
 * 15:13 (n <= 4), the tile stride at 22:16. A read returns what was written:
 * .124 boots with 00003fff and our linear desktop writes 01803fff. */
typedef struct vcr_fb_aperture {
    unsigned long base;         /* first byte of memory seen through the aperture */
    unsigned long lfb_stride;   /* bytes a line in the aperture; 0 = a reserved code */
    unsigned long tile_stride;  /* tiles a row of the buffers it converts */
    unsigned sli_shift;         /* log2 of the SLI units lines are dealt over; 0 = none */
} vcr_fb_aperture;

static __inline unsigned long vcr_fb_ap_base(unsigned long lmc)
{
    return ((lmc & 0x1fffUL) | ((lmc >> 23 & 3UL) << 13)) << 12;
}

static __inline unsigned long vcr_fb_ap_lfb_stride(unsigned long lmc)
{
    unsigned n = (unsigned)(lmc >> 13 & 7);
    return n <= 4 ? 1024UL << n : 0;
}

static __inline unsigned long vcr_fb_ap_tile_stride(unsigned long lmc)
{
    return lmc >> 16 & 0x7fUL;
}

/* ---- SLI (cfgSliLfbCtrl, PCI config 0x8c) ---------------------------------------
 * render mask 7:0, compare 15:8, scan mask (band lines - 1) 23:16, log2 of the
 * units 25:24, READ_EN 28 (vcr_sli.h VCR_SLILFB_*). A chip owns line y when
 * (y & render) == compare; it keeps its lines packed, so its own line is y
 * with the render bits squeezed out. Quake III's 4-way SLI on .124: 1e1f0060
 * on chip 0 (silicon, the SLI_STEP 406 log): 32-line bands dealt 0,1,2,3 - the
 * master holds 0-31, 128-159, ..., 896-927: 8 of the 30 bands. */
#define VCR_FB_SLILFB_READ_EN   (1UL << 28)
static __inline unsigned vcr_fb_sli_shift(unsigned long sli_lfb_ctrl)
{
    return (unsigned)(sli_lfb_ctrl >> 24 & 3);
}

static __inline int vcr_fb_sli_owns(unsigned long y, unsigned long chip_sli_lfb_ctrl)
{
    return (y & (chip_sli_lfb_ctrl & 0xff)) == (chip_sli_lfb_ctrl >> 8 & 0xff);
}

/* screen line y (< 65536) -> the line it is in its owner's memory */
static __inline unsigned long vcr_fb_sli_local_line(unsigned long y, unsigned long sli_lfb_ctrl)
{
    unsigned long render = sli_lfb_ctrl & 0xff, out = 0, bit = 1;
    unsigned i;
    for (i = 0; i < 16; i++) {
        if (!(render >> i & 1)) {
            if (y >> i & 1)
                out |= bit;
            bit <<= 1;
        }
    }
    return out;
}

/* Glide's hwcBufferLfbAddr (minihwc.c): the aperture address of a TILED buffer
 * at physical `phys` (>= the begin page). Its chip-local line is shifted by the
 * SLI units - in the aperture every line of the frame has an address. With no
 * SLI the same arithmetic is the aperture address of any raw byte. */
static __inline unsigned long vcr_fb_ap_addr(unsigned long phys, const vcr_fb_aperture *ap)
{
    unsigned long off = phys - ap->base, tile = off >> 12, row, col;
    if (!ap->tile_stride)
        return phys;
    row = tile / ap->tile_stride;
    col = tile % ap->tile_stride;
    {
        unsigned long line = (row * VCR_FB_TILE_H + (off >> 7 & 31)) << ap->sli_shift;
        return ap->base + line * ap->lfb_stride + col * VCR_FB_TILE_W + (off & 127);
    }
}

/* ---- the read plan ----------------------------------------------------------------
 * How a layer is read through memBase1. Every method reads, none writes. */
#define VCR_FB_M_LINEAR     1   /* raw linear memory below the aperture */
#define VCR_FB_M_TILED      2   /* raw tiled memory below the aperture (one chip's) */
#define VCR_FB_M_APERTURE   3   /* a tiled buffer through the aperture, whole frame */
#define VCR_FB_M_LINEAR_AP  4   /* a linear surface inside the aperture, no SLI */
/* refusals (negative) */
#define VCR_FB_R_FORMAT     (-1)    /* no size or format, or a pitch under a line */
#define VCR_FB_R_AP_CODE    (-2)    /* the aperture's line-stride code is reserved */
#define VCR_FB_R_AP_STRIDE  (-3)    /* aperture tile stride != the layer's */
#define VCR_FB_R_AP_LINE    (-4)    /* a line does not fit the aperture's line stride */
#define VCR_FB_R_STRADDLE   (-5)    /* a raw tiled buffer runs into the aperture */
#define VCR_FB_R_SLI_LINEAR (-6)    /* a linear surface in the aperture under SLI */
#define VCR_FB_R_RANGE      (-7)    /* past the mapped view or memBase1's decode */
#define VCR_FB_R_SLI_RAW    (-8)    /* raw memory while the chips deal lines in SLI bands */

typedef struct vcr_fb_layer {
    unsigned long start;        /* the start register, vcr_fb_start()ed */
    unsigned long stride;       /* the layer's stride field: bytes, or tiles if tiled */
    int tiled;
    unsigned w, h, bpp;
} vcr_fb_layer;

typedef struct vcr_fb_plan {
    int method;                 /* VCR_FB_M_*, or a VCR_FB_R_* refusal */
    vcr_fb_layer l;
    vcr_fb_aperture ap;
    unsigned long first;        /* memBase1 offset of line 0, byte 0 */
    unsigned long line;         /* memBase1 bytes between lines (LINEAR, APERTURE) */
    unsigned long rowbytes;     /* w * bpp */
    unsigned long end;          /* the last memBase1 byte read, +1 */
} vcr_fb_plan;

/* `limit` = the bytes of memBase1 that may be read (the smaller of the mapped
 * view and the decode - twice the memory a chip); 0 = no bound (tests). */
static __inline int vcr_fb_plan_make(vcr_fb_plan *p, const vcr_fb_layer *l,
                                     const vcr_fb_aperture *ap, unsigned long limit)
{
    unsigned long raw_end;
    p->l = *l;
    p->ap = *ap;
    p->first = p->line = p->end = 0;
    p->rowbytes = (unsigned long)l->w * l->bpp;
    p->method = VCR_FB_R_FORMAT;
    if (!l->w || !l->h || !l->bpp || l->w > 2048 || l->h > 2048)
        return p->method;
    if (l->tiled ? (l->stride & 0x7f) * VCR_FB_TILE_W < p->rowbytes
                 : (l->stride & 0x7fff) < p->rowbytes)
        return p->method;
    raw_end = l->start + vcr_fb_extent(l->w, l->h, l->bpp, l->stride, l->tiled);

    if (raw_end <= ap->base) {
        /* all of it below the aperture: the chip's raw memory - ONE chip's.
         * Under SLI the master's raw memory holds its own bands only (8 of
         * Quake III's 30), so a raw read is not the frame; the aperture is the
         * only whole-frame read (integration review, 2026-09-28) */
        if (ap->sli_shift)
            return p->method = VCR_FB_R_SLI_RAW;
        p->method = l->tiled ? VCR_FB_M_TILED : VCR_FB_M_LINEAR;
        p->first = l->start;
        p->line = l->tiled ? 0 : (l->stride & 0x7fff);
        p->end = raw_end;
    } else if (!ap->lfb_stride) {
        return p->method = VCR_FB_R_AP_CODE;
    } else if (l->tiled) {
        if (l->start < ap->base)
            return p->method = VCR_FB_R_STRADDLE;
        if (ap->tile_stride != (l->stride & 0x7f))
            return p->method = VCR_FB_R_AP_STRIDE;
        if (ap->lfb_stride < p->rowbytes)
            return p->method = VCR_FB_R_AP_LINE;
        p->method = VCR_FB_M_APERTURE;
        p->first = vcr_fb_ap_addr(l->start, ap);
        p->line = ap->lfb_stride;
        p->end = p->first + (unsigned long)(l->h - 1) * p->line + p->rowbytes;
    } else {
        if (ap->sli_shift)
            return p->method = VCR_FB_R_SLI_LINEAR;
        if (!ap->tile_stride || ap->tile_stride * VCR_FB_TILE_W > ap->lfb_stride)
            return p->method = VCR_FB_R_AP_LINE;
        p->method = VCR_FB_M_LINEAR_AP;
        p->first = l->start;
        p->line = l->stride & 0x7fff;
        /* within a tile row the aperture address is NOT monotonic in the raw
         * one (line 31 of an early tile outranks line 3 of the last): bound
         * it by the aperture line after the last tile row touched */
        p->end = ap->base + (((raw_end - 1 - ap->base) >> 12) / ap->tile_stride + 1) *
                                VCR_FB_TILE_H * ap->lfb_stride;
    }
    if (limit && p->end > limit)
        return p->method = VCR_FB_R_RANGE;
    return p->method;
}

/* memBase1 offset of byte `xb` of line `y`; *run = the bytes readable from
 * there in one piece (never past the line, a raw tile or a 128-byte group) */
static __inline unsigned long vcr_fb_plan_addr(const vcr_fb_plan *p, unsigned long xb,
                                               unsigned long y, unsigned long *run)
{
    unsigned long left = p->rowbytes - xb, phys, n;
    switch (p->method) {
    case VCR_FB_M_TILED:
        n = VCR_FB_TILE_W - xb % VCR_FB_TILE_W;
        *run = n < left ? n : left;
        return p->first + vcr_fb_offset(xb, y, p->l.stride, 1);
    case VCR_FB_M_LINEAR_AP:
        phys = p->first + y * p->line + xb;
        n = VCR_FB_TILE_W - phys % VCR_FB_TILE_W;
        *run = n < left ? n : left;
        return phys < p->ap.base ? phys : vcr_fb_ap_addr(phys, &p->ap);
    default:                    /* LINEAR, APERTURE */
        *run = left;
        return p->first + y * p->line + xb;
    }
}

static __inline const char *vcr_fb_method_name(int m)
{
    switch (m) {
    case VCR_FB_M_LINEAR:     return "raw-linear";
    case VCR_FB_M_TILED:      return "raw-tiled";
    case VCR_FB_M_APERTURE:   return "aperture";
    case VCR_FB_M_LINEAR_AP:  return "linear-in-aperture";
    case VCR_FB_R_FORMAT:     return "refused: no size/format, or the pitch is under a line";
    case VCR_FB_R_AP_CODE:    return "refused: lfbMemoryConfig's line-stride code is reserved";
    case VCR_FB_R_AP_STRIDE:  return "refused: the aperture converts another tile stride";
    case VCR_FB_R_AP_LINE:    return "refused: a line does not fit the aperture's line stride";
    case VCR_FB_R_STRADDLE:   return "refused: a raw tiled buffer runs into the aperture";
    case VCR_FB_R_SLI_LINEAR: return "refused: a linear surface inside the aperture under SLI";
    case VCR_FB_R_RANGE:      return "refused: past the mapped view or memBase1's decode";
    case VCR_FB_R_SLI_RAW:    return "refused: raw memory under SLI is one chip's bands, not the frame";
    }
    return "?";
}

/* ---- may memBase1 be read at all? --------------------------------------------------
 * 0 = yes. A live session of more than one chip must be SLI alone with the
 * hardware's SLI read on - never AA: an LFB read through the chips' AA read
 * hand-off froze the V5 6000 (cfg 3; h5sliaa.h rule 11). Any AA enable writes
 * cfgAALfbCtrl's CPU/dispatch write enables (vcrmp_sli.c), every disable
 * writes 0. Unknown = refuse. */
#define VCR_FB_AALFB_ACTIVE     ((1UL << 26) | (1UL << 27) | (1UL << 28))
#define VCR_FB_G_CFG_UNKNOWN    1   /* the master's config space could not be read */
#define VCR_FB_G_AA             2   /* multi-chip AA is live */
#define VCR_FB_G_SLI_NOREAD     3   /* SLI without the hardware's SLI read */
#define VCR_FB_G_SLI_CHANGED    4   /* the SLI units changed under a read (re-check) */

/* The gate by the BOARD (integration review, 2026-09-28): on a multi-chip
 * board the master's cfgSliLfbCtrl / cfgAALfbCtrl are ALWAYS read and an AA
 * write/read enable refuses - whatever the kernel's session count says.
 * `sli_chips` is what OUR kernel set up; Glide can also program the chips
 * through PCI_OP, another driver says nothing at all, and an AA read hand-off
 * the kernel does not know about freezes the board just the same.
 * board_chips: the chips on the card (vcr_info.nchips, or the HWC device's
 * count); sli_chips: the kernel's live session (0 = none, ~0 = unknown). */
static __inline int vcr_fb_mb1_gate_board(unsigned long board_chips, unsigned long sli_chips,
                                          int have_cfg, unsigned long sli_lfb_ctrl,
                                          unsigned long aa_lfb_ctrl)
{
    if (board_chips <= 1 && sli_chips <= 1)
        return 0;
    if (!have_cfg)
        return VCR_FB_G_CFG_UNKNOWN;
    if (aa_lfb_ctrl & VCR_FB_AALFB_ACTIVE)
        return VCR_FB_G_AA;
    /* SLI - the kernel's, or units the registers say are dealt - with the
     * hardware's SLI read off; with sli_chips > 1 this includes "neither SLI
     * nor AA": not a state we know */
    if ((sli_chips > 1 || vcr_fb_sli_shift(sli_lfb_ctrl)) &&
        !(sli_lfb_ctrl & VCR_FB_SLILFB_READ_EN))
        return VCR_FB_G_SLI_NOREAD;
    return 0;
}

/* the original form: the kernel's session count alone (the board = the
 * session). Kept for the answers pinned before the board gate. */
static __inline int vcr_fb_mb1_gate(unsigned long sli_chips, int have_cfg,
                                    unsigned long sli_lfb_ctrl, unsigned long aa_lfb_ctrl)
{
    return vcr_fb_mb1_gate_board(sli_chips, sli_chips, have_cfg, sli_lfb_ctrl, aa_lfb_ctrl);
}

/* A read takes a whole frame, and a game can enable AA (or leave SLI) in the
 * middle of it. So the gate is asked AGAIN - the master's two registers read
 * afresh - before the first line, at every flip the read follows and every
 * VCR_FB_RECHECK_LINES lines, and the read STOPS the moment it is not 0.
 * `planned_shift` is the SLI units the plan was made with: a change means
 * the aperture lines no longer mean what the plan says. */
#define VCR_FB_RECHECK_LINES    64u
static __inline int vcr_fb_recheck_due(unsigned long y, int flipped)
{
    return flipped || y % VCR_FB_RECHECK_LINES == 0;
}

static __inline int vcr_fb_mb1_recheck(unsigned long board_chips, unsigned long sli_chips,
                                       int have_cfg, unsigned long sli_lfb_ctrl,
                                       unsigned long aa_lfb_ctrl, unsigned planned_shift)
{
    int g = vcr_fb_mb1_gate_board(board_chips, sli_chips, have_cfg, sli_lfb_ctrl, aa_lfb_ctrl);
    if (g)
        return g;
    if ((board_chips > 1 || sli_chips > 1) && vcr_fb_sli_shift(sli_lfb_ctrl) != planned_shift)
        return VCR_FB_G_SLI_CHANGED;
    return 0;
}

static __inline const char *vcr_fb_gate_name(int g)
{
    switch (g) {
    case 0:                     return "ok";
    case VCR_FB_G_CFG_UNKNOWN:  return "refused: SLI/AA session live and its config unreadable";
    case VCR_FB_G_AA:           return "refused: multi-chip AA is live - an LFB read froze the V5 6000 (h5sliaa.h rule 11)";
    case VCR_FB_G_SLI_NOREAD:   return "refused: a multi-chip session without cfgSliLfbCtrl READ_EN";
    case VCR_FB_G_SLI_CHANGED:  return "stopped: the SLI units changed during the read";
    }
    return "?";
}

#endif /* VCR_FBSHOT_H */
