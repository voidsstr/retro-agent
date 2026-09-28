/*
 * vcrdd_2d.c - the 2D engine of the Banshee / Voodoo3 / VSA-100, driven from
 * the display driver.
 *
 * Registers: chip 0's window, mapped for us by the miniport
 * (IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES). Every write goes straight to the
 * chip's PCI FIFO (no command FIFO): wait for enough free slots in status[4:0],
 * write, and let the command register's GO bit start the operation. The layout
 * and bit encodings are the open 3dfx Glide release's (h3regs.h SstGRegs,
 * h3gdefs.h SSTG_*); the use - screen-to-screen copy with the direction bits
 * for an overlapping copy, a solid rectangle fill - is the classic one.
 *
 * The one rule everything else depends on: the CPU may not touch video memory
 * the engine is still writing. VcrDd2dSync() is called before every CPU access
 * (the GDI punt layer, a DirectDraw Lock, a software Blt) and costs one status
 * read when the engine was not used.
 *
 * Bounded: no wait here can hang win32k. A FIFO or idle wait that runs out
 * logs the status it saw and turns acceleration OFF for the PDEV - the
 * software paths are always there to fall back to.
 */
#include "vcrdd.h"
#include "../include/vcr_3dseq.h"

#define R2D(off)            (0x100000 + (off))
#define G_CLIP0MIN          R2D(0x08)
#define G_CLIP0MAX          R2D(0x0c)
#define G_DSTBASE           R2D(0x10)
#define G_DSTFORMAT         R2D(0x14)
#define G_SRCCKMIN          R2D(0x18)
#define G_SRCCKMAX          R2D(0x1c)
#define G_ROP               R2D(0x30)
#define G_SRCBASE           R2D(0x34)
#define G_COMMANDEX         R2D(0x38)
#define G_SRCFORMAT         R2D(0x54)
#define G_SRCXY             R2D(0x5c)
#define G_COLORBACK         R2D(0x60)
#define G_COLORFORE         R2D(0x64)
#define G_PATTERN0          R2D(0x100)      /* mono pattern rows 0-3 */
#define G_PATTERN1          R2D(0x104)      /* mono pattern rows 4-7 */
#define G_DSTSIZE           R2D(0x68)
#define G_DSTXY             R2D(0x6c)
#define G_COMMAND           R2D(0x70)
#define G_LAUNCH            R2D(0x80)       /* launch area: a host blit's data */

#define CMD_BLT             1u
#define CMD_RECTFILL        5u
#define CMD_GO              (1u << 8)
#define CMD_XDIR            (1u << 14)      /* right to left */
#define CMD_YDIR            (1u << 15)      /* bottom to top */
#define CMD_MONO_PATTERN    (1u << 13)      /* the pattern is 8x8 1 bpp: colorFore / colorBack */
#define CMD_TRANSPARENT     (1u << 16)      /* a mono source/pattern's 0 bits leave the dest */
#define CMD_PATX(x)         ((ULONG)((x) & 7) << 17)
#define CMD_PATY(y)         ((ULONG)((y) & 7) << 20)
#define CMD_ROP(r)          ((ULONG)(r) << 24)
/* ROP operands on this engine: S is the source pixel - for a RECTFILL, the
 * colorFore register - and P the pattern (colorPattern[], or colorFore/Back
 * through a mono pattern). A fill is therefore SRCCOPY with colorFore as its
 * source; PATCOPY (0xF0) fills with whatever the pattern registers hold. */
#define ROP_SRCCOPY         0xccu
#define ROP_DSTCOPY         0xaau
/* the rop register: the ROP used instead of the command's when a colour key
 * matches - [7:0] destination key, [15:8] source key, [23:16] both. A source
 * key means "leave the destination": D. */
#define ROP_KEYED           (ROP_SRCCOPY | (ROP_DSTCOPY << 8) | (ROP_DSTCOPY << 16))
#define CMDEX_SRC_CKEY      (1u << 0)

#define ST_FIFO_FREE        0x1fu
#define ST_BUSY             (1u << 9)

#define SPIN_CAP            2000000         /* status reads; ~a second of PCI reads */

static ULONG rd(VCR_PDEV *pd, ULONG off)
{
    return *(volatile ULONG *)(pd->pjRegs + off);
}

static void wr(VCR_PDEV *pd, ULONG off, ULONG v)
{
    *(volatile ULONG *)(pd->pjRegs + off) = v;
}

static void give_up(VCR_PDEV *pd, ULONG why, ULONG status)
{
    VcrDd(VCR_LV_ERROR, VCR_EV_DD_2D, 9, why, status, pd->g2d_ops,
          "2D engine %s (status %08x) - acceleration off for this PDEV",
          why == 1 ? "FIFO never drained" : "never went idle", status);
    pd->g2d_ok = 0;
}

/* room for n register writes in the PCI FIFO (the 3D engine uses it too) */
BOOL VcrDdRoom(VCR_PDEV *pd, ULONG n)
{
    ULONG i, s = 0;
    /* the engine was given up (a FIFO that never drained or never went idle):
     * every caller checked g2d_ok when its batch began, so without this the
     * rest of a D3D batch spun a whole SPIN_CAP on EVERY triangle after the
     * give-up - seconds per batch on a wedged chip */
    if (!pd->g2d_ok)
        return FALSE;
    for (i = 0; i < SPIN_CAP; i++) {
        s = rd(pd, 0);
        if ((s & ST_FIFO_FREE) >= n)
            return TRUE;
    }
    give_up(pd, 1, s);
    return FALSE;
}

/* the one guard before the CPU touches video memory.
 *
 * Idle = the engine reports not busy AND the PCI FIFO has drained back to its
 * full free count. Busy alone is not enough: an operation still sitting in the
 * FIFO has not started, and a status that only counts started work reads idle
 * while it waits (86Box's Voodoo3 does exactly that for 2D writes - measured
 * with gdilab, 2026-09-26: copies queued behind a sync that had already
 * returned). The full free count is learned at init, not assumed. */
void VcrDd2dSync(VCR_PDEV *pd)
{
    ULONG i, s = 0, idle = 0;
    if (!pd->g2d_busy)
        return;
    /* idle must read more than once: busy can drop for a cycle between two
     * queued operations */
    for (i = 0; i < SPIN_CAP && idle < 3; i++) {
        s = rd(pd, 0);
        idle = ((s & ST_BUSY) || (s & ST_FIFO_FREE) < pd->g2d_fifo_full) ? 0 : idle + 1;
    }
    if (idle < 3)
        give_up(pd, 2, s);
    pd->g2d_busy = 0;
}

/* the same "idle" as VcrDd2dSync, as a question asked BEFORE writing
 * anything (the Glide 3D reset's pre-check): vcr_3dseq.h's vcr_idle_run is
 * that rule, one status read at a time (the host tests pin the two
 * together; VcrDd2dSync itself is left exactly as proven). Reads only, and
 * bounded; unlike VcrDd2dSync it does not turn acceleration off - a chip
 * that is not idle here is simply not reset. Returns the run it reached
 * (VCR_IDLE_READS = idle) and the last status. */
static ULONG idle_before_write(VCR_PDEV *pd, ULONG *last)
{
    ULONG i, s = 0, run = 0;
    for (i = 0; i < SPIN_CAP && run < VCR_IDLE_READS; i++) {
        s = rd(pd, 0);
        run = vcr_idle_run(s, pd->g2d_fifo_full, run);
    }
    *last = s;
    return run;
}

/* may the engine be used right now? */
static BOOL usable(VCR_PDEV *pd)
{
    return pd->g2d_ok && pd->pjRegs && !pd->exclusive_pid;
}

static ULONG fmt_bits(ULONG bytespp)
{
    return bytespp == 1 ? 1 : bytespp == 2 ? 3 : bytespp == 3 ? 4 : 5;
}

/* a base the engine takes (16-byte aligned) plus the pixels it skipped */
static ULONG base_of(ULONG off, ULONG bytespp, ULONG *xskip)
{
    ULONG b = off & ~15u;
    *xskip = (off - b) / bytespp;
    return b;
}

BOOL VcrDd2dCopy(VCR_PDEV *pd, ULONG dst_off, LONG dst_stride, ULONG src_off, LONG src_stride,
                 ULONG bytespp, LONG sx, LONG sy, LONG dx, LONG dy, LONG w, LONG h,
                 ULONG ckey_flags, ULONG ck_lo, ULONG ck_hi)
{
    ULONG cmd = CMD_BLT | CMD_GO | CMD_ROP(ROP_SRCCOPY), dbase, sbase, dsk, ssk;
    if (!usable(pd) || w <= 0 || h <= 0 || w > 8191 || h > 8191 ||
        dst_stride <= 0 || src_stride <= 0 || dst_stride > 0x3fff || src_stride > 0x3fff ||
        (bytespp != 1 && bytespp != 2 && bytespp != 4))
        return FALSE;
    dbase = base_of(dst_off, bytespp, &dsk);
    sbase = base_of(src_off, bytespp, &ssk);
    sx += ssk;
    dx += dsk;
    if (sx + w > 8191 || dx + w > 8191 || sy + h > 8191 || dy + h > 8191)
        return FALSE;
    /* overlap inside one surface: copy away from the destination */
    if (dbase == sbase && dst_stride == src_stride) {
        if (sy < dy) {
            cmd |= CMD_YDIR;
            sy += h - 1;
            dy += h - 1;
        }
        if (sy == dy && sx < dx) {
            cmd |= CMD_XDIR;
            sx += w - 1;
            dx += w - 1;
        }
    }
    if (!VcrDdRoom(pd, 15))
        return FALSE;
    wr(pd, G_CLIP0MIN, 0);
    wr(pd, G_CLIP0MAX, 0x1fff1fff);
    wr(pd, G_DSTBASE, dbase);
    wr(pd, G_DSTFORMAT, (ULONG)dst_stride | (fmt_bits(bytespp) << 16));
    wr(pd, G_SRCBASE, sbase);
    wr(pd, G_SRCFORMAT, (ULONG)src_stride | (fmt_bits(bytespp) << 16));
    wr(pd, G_COMMANDEX, ckey_flags ? CMDEX_SRC_CKEY : 0);
    wr(pd, G_ROP, ROP_KEYED);
    if (ckey_flags) {
        wr(pd, G_SRCCKMIN, ck_lo);
        wr(pd, G_SRCCKMAX, ck_hi);
    }
    wr(pd, G_SRCXY, ((ULONG)sy << 16) | ((ULONG)sx & 0x1fff));
    wr(pd, G_DSTSIZE, ((ULONG)h << 16) | ((ULONG)w & 0x1fff));
    wr(pd, G_DSTXY, ((ULONG)dy << 16) | ((ULONG)dx & 0x1fff));
    wr(pd, G_COMMAND, cmd);
    pd->g2d_busy = 1;
    pd->g2d_ops++;
    return TRUE;
}

BOOL VcrDd2dFill(VCR_PDEV *pd, ULONG dst_off, LONG dst_stride, ULONG bytespp, LONG x, LONG y,
                 LONG w, LONG h, ULONG color)
{
    ULONG dbase, dsk;
    if (!usable(pd) || w <= 0 || h <= 0 || dst_stride <= 0 || dst_stride > 0x3fff ||
        (bytespp != 1 && bytespp != 2 && bytespp != 4))
        return FALSE;
    dbase = base_of(dst_off, bytespp, &dsk);
    x += dsk;
    if (x + w > 8191 || y + h > 8191)
        return FALSE;
    if (!VcrDdRoom(pd, 9))
        return FALSE;
    wr(pd, G_CLIP0MIN, 0);
    wr(pd, G_CLIP0MAX, 0x1fff1fff);
    wr(pd, G_DSTBASE, dbase);
    wr(pd, G_DSTFORMAT, (ULONG)dst_stride | (fmt_bits(bytespp) << 16));
    wr(pd, G_COMMANDEX, 0);
    wr(pd, G_COLORFORE, color);
    wr(pd, G_DSTSIZE, ((ULONG)h << 16) | ((ULONG)w & 0x1fff));
    wr(pd, G_DSTXY, ((ULONG)y << 16) | ((ULONG)x & 0x1fff));
    wr(pd, G_COMMAND, CMD_RECTFILL | CMD_GO | CMD_ROP(ROP_SRCCOPY));
    pd->g2d_busy = 1;
    pd->g2d_ops++;
    return TRUE;
}

/* ---- monochrome expansion: text (include/vcr_text.h) -----------------------
 *
 * One host-to-screen blit per visible glyph part: dstSize, dstXY and the
 * command (no GO - the first data write starts it), then exactly
 * vcr_glyph_dwords() dwords to the launch area. The source rows are cut and
 * shifted on the CPU (vcr_glyph_stream), so neither clip register nor source
 * offset is involved: the engine only ever expands a whole bitmap.
 *
 * FIFO: a run of glyphs writes many dwords, so instead of a status read per
 * write (VcrDdRoom) the free count one read reported is spent as credit and
 * the status is read again only when it runs out - the engine only drains
 * the FIFO in between, so what it said was free still is. Every wait is
 * VcrDdRoom's: SPIN_CAP reads, then give_up() - acceleration off for the
 * PDEV and FALSE, and the caller redraws the whole call in software. */
static BOOL mono_room(VCR_MONO *m, ULONG n)
{
    VCR_PDEV *pd = m->pd;
    ULONG i, s = 0, f;
    if (m->credit >= n) {
        m->credit -= n;
        return TRUE;
    }
    for (i = 0; i < SPIN_CAP; i++) {
        s = rd(pd, 0);
        f = s & ST_FIFO_FREE;
        if (f >= n) {
            m->credit = f - n;
            if (i)
                pd->text_fifo_waits++;      /* the first read was not enough: we waited */
            return TRUE;
        }
    }
    m->credit = 0;
    give_up(pd, 1, s);
    return FALSE;
}

static int mono_put(void *ctx, vcr_u32 dw)
{
    VCR_MONO *m = (VCR_MONO *)ctx;
    if (!mono_room(m, 1))
        return 0;
    wr(m->pd, G_LAUNCH, dw);
    return 1;
}

BOOL VcrDd2dMonoBegin(VCR_PDEV *pd, VCR_MONO *m, ULONG dst_off, LONG dst_stride, ULONG bytespp,
                      ULONG color)
{
    ULONG dbase;
    m->pd = pd;
    m->credit = 0;
    if (!usable(pd) || dst_stride <= 0 || dst_stride > 0x3fff ||
        (bytespp != 1 && bytespp != 2 && bytespp != 4))
        return FALSE;
    dbase = base_of(dst_off, bytespp, &m->xskip);
    if (!VcrDdRoom(pd, 8))
        return FALSE;
    wr(pd, G_CLIP0MIN, 0);
    wr(pd, G_CLIP0MAX, 0x1fff1fff);
    wr(pd, G_DSTBASE, dbase);
    wr(pd, G_DSTFORMAT, (ULONG)dst_stride | (fmt_bits(bytespp) << 16));
    wr(pd, G_SRCFORMAT, VCR_TEXT_SRCFMT);
    wr(pd, G_SRCXY, 0);
    wr(pd, G_COMMANDEX, 0);                     /* no colour keys */
    wr(pd, G_COLORFORE, color);                 /* a pixel in the destination's format */
    return TRUE;
}

BOOL VcrDd2dMonoGlyph(VCR_MONO *m, const BYTE *bits, ULONG cx, const vcr_glyph_part *p)
{
    VCR_PDEV *pd = m->pd;
    LONG x = p->x + (LONG)m->xskip;
    ULONG want = vcr_glyph_dwords(p);
    if (!pd->g2d_ok || x < 0 || p->y < 0 || !p->w || !p->h ||
        x + (LONG)p->w > VCR_2D_MAX_XY || p->y + (LONG)p->h > VCR_2D_MAX_XY)
        return FALSE;
    if (!mono_room(m, 3))
        return FALSE;
    wr(pd, G_DSTSIZE, (p->h << 16) | p->w);
    wr(pd, G_DSTXY, ((ULONG)p->y << 16) | ((ULONG)x & 0x1fff));
    wr(pd, G_COMMAND, VCR_TEXT_CMD);
    pd->g2d_busy = 1;
    pd->g2d_ops++;
    /* short = the FIFO wait gave up in the middle: acceleration is off, and
     * the engine is left waiting for the rest - as stuck as the FIFO was */
    return vcr_glyph_stream(bits, cx, p, mono_put, m) == want;
}

/* an 8x8 monochrome pattern fill (a realized hatch/dither brush): the
 * rectangle fill with the mono pattern on, the pattern's 1 bits in `fore`,
 * its 0 bits in `back` or - `transparent` - left alone. rop3 is the pattern
 * ROP (PATCOPY 0xF0, PATINVERT 0x5A); patx/paty the pattern offsets
 * (vcr_line.h vcr_pat_offset) relative to the engine's origin, which this
 * adds the surface's 16-byte base skew to. xf86-video-tdfx drives the same
 * registers for its Mono8x8PatternFill (MIT; register usage only). */
BOOL VcrDd2dPatFill(VCR_PDEV *pd, ULONG dst_off, LONG dst_stride, ULONG bytespp, LONG x, LONG y,
                    LONG w, LONG h, ULONG pat0, ULONG pat1, ULONG fore, ULONG back, ULONG rop3,
                    BOOL transparent, ULONG patx, ULONG paty)
{
    ULONG dbase, dsk, cmd;
    if (!usable(pd) || w <= 0 || h <= 0 || dst_stride <= 0 || dst_stride > 0x3fff ||
        (bytespp != 1 && bytespp != 2 && bytespp != 4))
        return FALSE;
    dbase = base_of(dst_off, bytespp, &dsk);
    x += dsk;
    if (x + w > 8191 || y + h > 8191)
        return FALSE;
    if (!VcrDdRoom(pd, 12))
        return FALSE;
    cmd = CMD_RECTFILL | CMD_GO | CMD_MONO_PATTERN | CMD_ROP(rop3 & 0xff) |
          CMD_PATX(patx - dsk) | CMD_PATY(paty) | (transparent ? CMD_TRANSPARENT : 0);
    wr(pd, G_CLIP0MIN, 0);
    wr(pd, G_CLIP0MAX, 0x1fff1fff);
    wr(pd, G_DSTBASE, dbase);
    wr(pd, G_DSTFORMAT, (ULONG)dst_stride | (fmt_bits(bytespp) << 16));
    wr(pd, G_COMMANDEX, 0);
    wr(pd, G_PATTERN0, pat0);
    wr(pd, G_PATTERN1, pat1);
    wr(pd, G_COLORBACK, back);
    wr(pd, G_COLORFORE, fore);
    wr(pd, G_DSTSIZE, ((ULONG)h << 16) | ((ULONG)w & 0x1fff));
    wr(pd, G_DSTXY, ((ULONG)y << 16) | ((ULONG)x & 0x1fff));
    wr(pd, G_COMMAND, cmd);
    pd->g2d_busy = 1;
    pd->g2d_ops++;
    return TRUE;
}

/* What a Glide session leaves in chip 0's 3D block - a chip mask that shuts
 * chip 0 out, AA jitter, an extended or two-pixels-per-clock combine,
 * stencil state - cleared for whoever draws next (vcr_3dseq.h has the list,
 * each register's source and the order). Called once the chip is ours again:
 * the OWNER released exclusive mode AND its desktop mode was re-programmed
 * (which turned SLI off) - never for an owner DrvAssertMode merely assumes is
 * gone (vcr_3dseq.h says why). VSA-100 only, only with Diag\\Reset3D = 1 - it
 * has not run on silicon - and only while Direct3D is on: the HAL is the only
 * user of what it resets (the 2D engine and DirectDraw never touch the 3D
 * block, and the next Glide session programs all of it itself), so with
 * Diag\\D3D = 0 it would be register traffic for nobody.
 *
 * NOTHING is written unless the chip is idle FIRST - the bounded idle rule
 * VcrDd2dSync uses - and Glide's command FIFO is off (cmdFifo0.baseSize,
 * one read): a release can follow a Glide close whose own idle wait failed,
 * and a kernel mode set that left the engine STILL BUSY reports success
 * (vcr_3dseq.h). Then through the PCI FIFO like every other write here, in
 * chunks the FIFO takes (VcrDdRoom is bounded: a chip that never drains turns
 * acceleration off, it does not hang), then a bounded idle wait so a chip
 * that does not settle is named HERE, not by the next GDI call.
 * Returns VCR_R3D_*; *detail = the status (BUSY), cmdFifo0.baseSize
 * (CMDFIFO) or g2d_ok, for the caller's log line. */
ULONG VcrDdGlideReset3d(VCR_PDEV *pd, ULONG *detail)
{
    vcr_regw w[VCR_3D_RESET_MAX];
    ULONG n, i, s, run, cmd = 0, why;
    *detail = pd->g2d_ok;
    if (!pd->reset3d || pd->d3d_disabled || !pd->napalm || !pd->pjRegs || !pd->g2d_ok)
        return VCR_R3D_OFF;
    /* the pre-check: reads only, before any write */
    run = idle_before_write(pd, &s);
    if (run >= VCR_IDLE_READS)
        cmd = rd(pd, VCR_CMD_BASESIZE0);
    why = vcr_3d_reset_blocked(run, cmd);
    if (why) {
        *detail = why == VCR_R3D_CMDFIFO ? cmd : s;
        return why;
    }
    n = vcr_3d_glide_reset_seq(w);
    for (i = 0; i < n; i++) {
        if ((i & 7) == 0 && !VcrDdRoom(pd, n - i < 8 ? n - i : 8)) {
            *detail = pd->g2d_ok;
            return VCR_R3D_GAVEUP;
        }
        wr(pd, V3D_BASE + w[i].off, w[i].val);          /* chip 0's 3D block */
    }
    pd->g2d_busy = 1;
    VcrDd2dSync(pd);
    *detail = pd->g2d_ok;
    return pd->g2d_ok ? VCR_R3D_DONE : VCR_R3D_GAVEUP;
}

/* the registers, from the miniport - Voodoo only (the Bochs test backend
 * has no engine, and answers ERROR_INVALID_FUNCTION) */
void VcrDd2dInit(VCR_PDEV *pd)
{
    VIDEO_MEMORY req;
    VIDEO_PUBLIC_ACCESS_RANGES r;
    vcr_info info;
    DWORD rc;
    pd->pjRegs = NULL;
    pd->g2d_ok = pd->g2d_busy = 0;
    pd->text_off = 1;               /* engine text only when the miniport says so */
    pd->pat_on = pd->line_on = 0;   /* likewise patterns and lines */
    if (!VcrIoctl(pd->hDriver, IOCTL_VCR_INFO, NULL, 0, &info, sizeof info, NULL)) {
        pd->g2d_disabled = (info.flags & VCR_INFO_F_NO_ACCEL2D) ? 1 : 0;
        pd->d3d_disabled = (info.flags & VCR_INFO_F_NO_D3D) ? 1 : 0;
        pd->no_texport = (info.flags & VCR_INFO_F_NO_TEXPORT) ? 1 : 0;
        pd->text_off = (info.flags & VCR_INFO_F_TEXT2D) ? 0 : 1;   /* default OFF */
        pd->pat_on = (info.flags & VCR_INFO_F_PAT2D) ? 1 : 0;       /* the miniport's call */
        pd->line_on = (info.flags & VCR_INFO_F_LINE2D) ? 1 : 0;
        pd->gamma_off = (info.flags & VCR_INFO_F_NO_GDIGAMMA) ? 1 : 0;   /* default ON */
        pd->napalm = info.device == 0x0009;
        /* default OFF (positive flags - an older miniport never sets them) */
        pd->rt32 = pd->napalm && (info.flags & VCR_INFO_F_D3D32) ? 1 : 0;
        pd->reset3d = pd->napalm && (info.flags & VCR_INFO_F_RESET3D) ? 1 : 0;
        if (pd->rt32 || pd->reset3d)
            VcrDd(VCR_LV_INFO, VCR_EV_DD_D3D, 16, pd->rt32, pd->reset3d, info.flags,
                  "armed: 32 bpp Direct3D %s (Diag\\D3D32), Glide 3D reset %s (Diag\\Reset3D)",
                  pd->rt32 ? "ON" : "off", pd->reset3d ? "ON" : "off");
    }
    req.RequestedVirtualAddress = NULL;
    rc = VcrIoctl(pd->hDriver, IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES, &req, sizeof req, &r,
                  sizeof r, NULL);
    if (rc || !r.VirtualAddress) {
        VcrDd(VCR_LV_INFO, VCR_EV_DD_2D, 1, rc, 0, 0, "no register window - 2D in software");
        return;
    }
    pd->pjRegs = (PUCHAR)r.VirtualAddress;
    /* the PCI FIFO's free count when empty: nothing of ours is queued yet */
    {
        ULONG i, f;
        pd->g2d_fifo_full = 0;
        for (i = 0; i < 64; i++) {
            f = rd(pd, 0) & ST_FIFO_FREE;
            if (f > pd->g2d_fifo_full)
                pd->g2d_fifo_full = f;
        }
    }
    pd->g2d_ok = pd->g2d_disabled || !pd->g2d_fifo_full ? 0 : 1;
    VcrDd(VCR_LV_INFO, VCR_EV_DD_2D, 1, pd->g2d_fifo_full, (ULONG)(ULONG_PTR)pd->pjRegs,
          pd->g2d_ok, "2D engine %s (registers at %p, FIFO free when empty %u)",
          pd->g2d_ok ? "on" : pd->g2d_disabled ? "off by registry" : "off: FIFO reads full",
          pd->pjRegs, pd->g2d_fifo_full);
    if (pd->g2d_ok && !pd->text_off && !pd->text_mask)
        pd->text_mask = (PUCHAR)EngAllocMem(0, VCR_TEXT_MASK_BYTES, VCRDD_TAG);
    if (pd->g2d_ok)
        VcrDd(VCR_LV_INFO, VCR_EV_DD_2D, 11, !pd->text_off, pd->bpp, pd->text_mask != NULL,
              "text on the engine: %s", pd->text_off ? "off (Diag\\Accel2DText not 1, the default) - EngTextOut"
              : pd->text_mask ? "on" : "on, one blit per glyph (no mask buffer)");
}

void VcrDd2dTerm(VCR_PDEV *pd)
{
    VIDEO_MEMORY req;
    if (!pd->pjRegs)
        return;
    VcrDd2dSync(pd);
    req.RequestedVirtualAddress = pd->pjRegs;
    VcrIoctl(pd->hDriver, IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES, &req, sizeof req, NULL, 0,
             NULL);
    VcrDd(VCR_LV_INFO, VCR_EV_DD_2D, 2, pd->g2d_ops, 0, 0, "2D engine released after %u ops",
          pd->g2d_ops);
    pd->pjRegs = NULL;
    pd->g2d_ok = 0;
    if (pd->text_mask)
        EngFreeMem(pd->text_mask);
    pd->text_mask = NULL;
}
