/*
 * vcrmp_dd.c - what DirectDraw needs from the miniport: video memory mapped
 * into the application (the standard SHARE / UNSHARE_VIDEO_MEMORY), a flip
 * (scan out from another offset), and the vertical blank / scan line.
 *
 * The display DLL's DirectDraw HAL (display/vcrdd_ddraw.c) declares the free
 * video memory below the desktop as its heap and lets the DirectDraw runtime
 * place surfaces in it; a flip is then just "scan out from fpVidMem".
 *   VOODOO: vidDesktopStartAddr (latched by the chip at vertical sync)
 *   BOCHS:  the VBE Y offset (the VM test bed, where this is proven first)
 */
#include "vcrmp.h"

static ULONG vram_bytes(VCR_EXT *x)
{
    return x->backend == VCR_HW_BOCHS ? x->bochs_vram : x->fb_per_chip;
}

VP_STATUS VcrDdIoctl(VCR_EXT *x, ULONG code, PVOID in, ULONG inlen, PVOID out, ULONG outlen,
                     PULONG info)
{
    *info = 0;
    switch (code) {
    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY: {
        VIDEO_SHARE_MEMORY req;
        VIDEO_SHARE_MEMORY_INFORMATION *res = (VIDEO_SHARE_MEMORY_INFORMATION *)out;
        PHYSICAL_ADDRESS pa;
        ULONG size, io = VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_USER_MODE |
                         VIDEO_MEMORY_SPACE_P6CACHE;
        PVOID va;
        VP_STATUS st;
        if (inlen < sizeof req || outlen < sizeof *res)
            return ERROR_INSUFFICIENT_BUFFER;
        /* METHOD_BUFFERED: `in` and `out` are one buffer - take the request first */
        VideoPortMoveMemory(&req, in, sizeof req);
        if (req.ViewOffset > vram_bytes(x) || req.ViewSize > vram_bytes(x) - req.ViewOffset)
            return ERROR_INVALID_PARAMETER;
        pa.QuadPart = x->chip[0].lfb_phys.QuadPart + req.ViewOffset;
        size = req.ViewSize;
        va = req.ProcessHandle;
        st = VideoPortMapMemory(x, pa, &size, &io, &va);
        VLOG(st == NO_ERROR ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_USER_MAP, 0xdd, pa.LowPart,
             size, st == NO_ERROR ? (ULONG)(ULONG_PTR)va : (ULONG)st,
             "DirectDraw video memory view: %s", st == NO_ERROR ? "ok" : "FAILED");
        if (st != NO_ERROR)
            return st;
        res->SharedViewOffset = req.ViewOffset;
        res->SharedViewSize = size;
        res->VirtualAddress = va;
        *info = sizeof *res;
        return NO_ERROR;
    }
    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY: {
        VIDEO_SHARE_MEMORY req;
        VP_STATUS st;
        if (inlen < sizeof req)
            return ERROR_INSUFFICIENT_BUFFER;
        VideoPortMoveMemory(&req, in, sizeof req);
        st = VideoPortUnmapMemory(x, req.RequestedVirtualAddress, req.ProcessHandle);
        VLOG(st == NO_ERROR ? VCR_LV_DEBUG : VCR_LV_WARN, VCR_EV_USER_UNMAP,
             (ULONG)(ULONG_PTR)req.RequestedVirtualAddress, st, 0xdd, 0,
             "DirectDraw view unmapped");
        return st;
    }
    case IOCTL_VCR_DDFLIP: {
        vcr_dd_flip f;
        if (inlen < sizeof f || x->cur_mode < 0)
            return ERROR_INVALID_PARAMETER;
        VideoPortMoveMemory(&f, in, sizeof f);
        if (f.offset >= vram_bytes(x) || !x->cur_stride)
            return ERROR_INVALID_PARAMETER;
        if (x->backend == VCR_HW_BOCHS) {
            x->dispi[8] = 0;                                 /* X offset */
            x->dispi[9] = (USHORT)(f.offset / x->cur_stride); /* Y offset */
        } else {
            VcrWr(x, 0, VCR_R_VIDDESKTOPSTARTADDR, f.offset);
        }
        x->dd_scan = f.offset;
        return NO_ERROR;
    }
    case IOCTL_VCR_VBLANK: {
        vcr_dd_vblank *v = (vcr_dd_vblank *)out;
        if (outlen < sizeof *v)
            return ERROR_INSUFFICIENT_BUFFER;
        VideoPortZeroMemory(v, sizeof *v);
        if (x->backend == VCR_HW_BOCHS) {
            /* input status 1 through the std-vga MMIO block (0x3c0 at +0) */
            v->in_vblank = x->bochs_vga ? (VideoPortReadRegisterUchar(x->bochs_vga + 0x1a) >> 3) & 1 : 0;
        } else {
            /* status[6] is CLEAR during the retrace (Glide: grSstVRetraceOn
             * returns (status & SST_VRETRACE) == 0) - reading it the other way
             * waited for the END of the blank and tore every flip */
            v->in_vblank = (VcrRd(x, 0, VCR_R_STATUS) & VCR_STATUS_VRETRACE) ? 0 : 1;
            v->scanline = VcrRd(x, 0, VCR_R_VIDCURRENTLINE) & 0x7ff;
        }
        v->scan_offset = x->dd_scan;
        *info = sizeof *v;
        return NO_ERROR;
    }
    }
    return ERROR_INVALID_FUNCTION;
}
