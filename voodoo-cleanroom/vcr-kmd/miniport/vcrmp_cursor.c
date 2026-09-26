/*
 * vcrmp_cursor.c - the hardware cursor, through the standard XP pointer
 * IOCTLs (the display DLL's DrvSetPointerShape / DrvMovePointer send them).
 *
 * The pattern lives in video memory one page below the desktop (the desktop
 * sits at the top; vcr_desktop_offset) and a copy lives here, because every
 * Glide session owns all of video memory and turns the cursor off: after each
 * mode set - which is how the desktop comes back - VcrCursorApply() writes the
 * pattern and the registers again. Format and colours are the vendor's
 * (include/vcr_cursor.h). Diag\HwCursor = 0 reports no hardware pointer, so
 * GDI keeps its software one.
 */
#include "vcrmp.h"
#include "../include/vcr_cursor.h"

static ULONG pattern_addr(VCR_EXT *x)
{
    return x->desktop_offset >= 0x2000 ? (x->desktop_offset - 0x1000) & ~0xfffu : 0;
}

void VcrCursorApply(VCR_EXT *x)
{
    ULONG addr, vpc, i;
    PHYSICAL_ADDRESS pa;

    if (x->backend != VCR_HW_VOODOO || x->cur_mode < 0 || !x->cur_valid)
        return;
    addr = pattern_addr(x);
    if (!addr)
        return;
    pa.QuadPart = x->chip[0].lfb_phys.QuadPart + addr;
    if (x->cur_map && x->cur_map_phys.QuadPart != pa.QuadPart) {
        VideoPortFreeDeviceBase(x, x->cur_map);
        x->cur_map = NULL;
    }
    if (!x->cur_map) {
        x->cur_map = (PUCHAR)VideoPortGetDeviceBase(x, pa, 0x1000, FALSE);
        x->cur_map_phys = pa;
        if (!x->cur_map) {
            VLOG(VCR_LV_ERROR, VCR_EV_CURSOR, 0, addr, 0, 0, "cursor page not mapped");
            return;
        }
    }
    for (i = 0; i < VCR_CURSOR_BYTES / 4; i++)
        VideoPortWriteRegisterUlong((PULONG)(x->cur_map + i * 4),
                                    ((PULONG)x->cur_pat)[i]);
    VcrWr(x, 0, VCR_R_HWCURPATADDR, addr);
    VcrWr(x, 0, VCR_R_HWCURC0, 0x00000000);
    VcrWr(x, 0, VCR_R_HWCURC1, 0x00ffffff);
    VcrWr(x, 0, VCR_R_HWCURLOC, vcr_cursor_loc(x->cur_x, x->cur_y));
    vpc = VcrRd(x, 0, VCR_R_VIDPROCCFG) & ~(VCR_VPC_CURSOR_X11 | VCR_VPC_CURSOR_EN);
    VcrWr(x, 0, VCR_R_VIDPROCCFG, x->cur_on ? vpc | VCR_VPC_CURSOR_EN : vpc);
    x->cur_addr = addr;
    VLOG(VCR_LV_DEBUG, VCR_EV_CURSOR, x->cur_on, addr, vcr_cursor_loc(x->cur_x, x->cur_y),
         VcrRd(x, 0, VCR_R_VIDPROCCFG), "cursor applied");
}

static void cursor_enable(VCR_EXT *x, ULONG on)
{
    ULONG vpc;
    x->cur_on = on && x->cur_valid;
    if (x->backend != VCR_HW_VOODOO || x->cur_mode < 0 || !x->cur_addr)
        return;
    vpc = VcrRd(x, 0, VCR_R_VIDPROCCFG) & ~VCR_VPC_CURSOR_EN;
    VcrWr(x, 0, VCR_R_VIDPROCCFG, x->cur_on ? vpc | VCR_VPC_CURSOR_EN : vpc);
}

VP_STATUS VcrCursorIoctl(VCR_EXT *x, ULONG code, PVOID in, ULONG inlen, PVOID out,
                         ULONG outlen, PULONG info)
{
    *info = 0;
    if (x->backend != VCR_HW_VOODOO || !VcrDiagGet(L"HwCursor", 1)) {
        /* no hardware pointer here: say so as an ANSWER (size 0), not as a
         * failed IOCTL - the display DLL asks at every surface enable */
        if (code == IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES &&
            outlen >= sizeof(VIDEO_POINTER_CAPABILITIES)) {
            VideoPortZeroMemory(out, sizeof(VIDEO_POINTER_CAPABILITIES));
            *info = sizeof(VIDEO_POINTER_CAPABILITIES);
            return NO_ERROR;
        }
        return ERROR_INVALID_FUNCTION;
    }
    switch (code) {
    case IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES: {
        VIDEO_POINTER_CAPABILITIES *c = (VIDEO_POINTER_CAPABILITIES *)out;
        if (outlen < sizeof *c)
            return ERROR_INSUFFICIENT_BUFFER;
        c->Flags = VIDEO_MODE_MONO_POINTER;
        c->MaxWidth = VCR_CURSOR_DIM;
        c->MaxHeight = VCR_CURSOR_DIM;
        c->HWPtrBitmapStart = 0xffffffffu;
        c->HWPtrBitmapEnd = 0xffffffffu;
        *info = sizeof *c;
        return NO_ERROR;
    }
    case IOCTL_VIDEO_SET_POINTER_ATTR: {
        VIDEO_POINTER_ATTRIBUTES *a = (VIDEO_POINTER_ATTRIBUTES *)in;
        ULONG head = FIELD_OFFSET(VIDEO_POINTER_ATTRIBUTES, Pixels), plane;
        if (inlen < head)
            return ERROR_INSUFFICIENT_BUFFER;
        plane = a->Height * a->WidthInBytes;
        if (!(a->Flags & VIDEO_MODE_MONO_POINTER) || inlen < head + 2 * plane ||
            !vcr_cursor_from_mono(a->Pixels, a->Pixels + plane, a->Width, a->Height,
                                  a->WidthInBytes, x->cur_pat)) {
            VLOG(VCR_LV_DEBUG, VCR_EV_CURSOR, a->Flags, a->Width, a->Height, inlen,
                 "pointer shape declined");
            return ERROR_INVALID_PARAMETER;
        }
        x->cur_valid = 1;
        x->cur_x = a->Column;
        x->cur_y = a->Row;
        x->cur_on = a->Enable ? 1 : 0;
        VcrCursorApply(x);
        return NO_ERROR;
    }
    case IOCTL_VIDEO_SET_POINTER_POSITION: {
        VIDEO_POINTER_POSITION *p = (VIDEO_POINTER_POSITION *)in;
        if (inlen < sizeof *p)
            return ERROR_INSUFFICIENT_BUFFER;
        x->cur_x = p->Column;
        x->cur_y = p->Row;
        if (x->cur_mode >= 0 && x->cur_addr)
            VcrWr(x, 0, VCR_R_HWCURLOC, vcr_cursor_loc(x->cur_x, x->cur_y));
        if (!x->cur_on)
            cursor_enable(x, 1);
        return NO_ERROR;
    }
    case IOCTL_VIDEO_ENABLE_POINTER:
        cursor_enable(x, 1);
        return NO_ERROR;
    case IOCTL_VIDEO_DISABLE_POINTER:
        cursor_enable(x, 0);
        return NO_ERROR;
    }
    return ERROR_INVALID_FUNCTION;
}
