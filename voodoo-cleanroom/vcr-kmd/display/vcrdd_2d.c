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
#define G_COLORFORE         R2D(0x64)
#define G_DSTSIZE           R2D(0x68)
#define G_DSTXY             R2D(0x6c)
#define G_COMMAND           R2D(0x70)

#define CMD_BLT             1u
#define CMD_RECTFILL        5u
#define CMD_GO              (1u << 8)
#define CMD_XDIR            (1u << 14)      /* right to left */
#define CMD_YDIR            (1u << 15)      /* bottom to top */
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
    if (!VcrIoctl(pd->hDriver, IOCTL_VCR_INFO, NULL, 0, &info, sizeof info, NULL)) {
        pd->g2d_disabled = (info.flags & VCR_INFO_F_NO_ACCEL2D) ? 1 : 0;
        pd->d3d_disabled = (info.flags & VCR_INFO_F_NO_D3D) ? 1 : 0;
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
}
