/*
 * disp_hw.c - retro3dfx-disp hardware layer: find the Voodoo, read its PCI BARs,
 * and map them into the Glide process (what HWCEXT_GETLINEARADDR hands back).
 *
 * This is the Windows equivalent of the open Linux Device3Dfx driver: Device3Dfx
 * reads PCI_BASE_ADDRESS_0/1/2 and mmaps the register + framebuffer regions to
 * user space so Glide drives the card directly. We do the same via the display
 * driver / miniport (VideoPortGetAccessRanges + MmMapIoSpace + mapping into the
 * calling process). Provides r3dfx_hw_map_linear()/r3dfx_hw_set_exclusive() that
 * disp_escape.c calls.
 *
 * Voodoo3 (H3) PCI layout (from our Glide source, minihwc/linhwc):
 *   BAR0 (0x10) = memory-mapped registers, 32MB window:
 *                 SST regs, 2D, 3D (HW_REGISTER_OFFSET), LFB, command FIFO.
 *   BAR1 (0x14) = linear framebuffer.
 *   BAR2 (0x18) = I/O port base.
 * Glide wants baseAddresses[0] = mapped BAR0 (registers), [1] = mapped BAR1 (fb).
 *
 * REQUIRES the DDK (winddi.h / wdm.h / video.h). Structure + logic are written
 * against the Device3Dfx model; compile/validate in the DDK env (fleet DDK).
 */
#include "retro3dfx_hwcext.h"

/* mirror of the adapter struct disp_escape.c uses (kept in sync there). */
typedef struct {
    R3DFX_U32 vendorID, deviceID, chipRev, fbRam;
    R3DFX_U32 pciStride, hwStride, tileMark;
    R3DFX_U32 linear[R3DFX_HWCEXT_MAX_BASEADDR];
    R3DFX_U32 numLinear, nextContext;
    int       exclusive;
} r3dfx_adapter;

/* Voodoo3 BAR window sizes */
#define R3DFX_BAR0_REG_SIZE   0x02000000UL   /* 32 MB register aperture       */
#define R3DFX_BAR1_FB_SIZE    0x02000000UL   /* 32 MB framebuffer aperture    */

#ifdef HAVE_DDK
#include <windows.h>
#include <wdm.h>
#include <winddi.h>
#include <video.h>          /* VideoPortGetAccessRanges, VIDEO_ACCESS_RANGE   */

/* Filled at StartDevice from the PnP-assigned resources. */
typedef struct {
    PHYSICAL_ADDRESS bar0Phys, bar1Phys;   /* registers, framebuffer          */
    ULONG            bar0Len,  bar1Len;
    USHORT           ioPort;
    PVOID            bar0Kernel;           /* our kernel-mode mapping          */
} r3dfx_hw;

/* --- StartDevice: locate the card + its BARs (Device3Dfx does the PCI reads;
 *     on Windows the miniport already has the PnP-translated resources) ----- */
BOOLEAN r3dfx_hw_start(r3dfx_adapter *a, r3dfx_hw *hw,
                       PVIDEO_ACCESS_RANGE ranges, ULONG nRanges,
                       ULONG pciVendor, ULONG pciDevice, ULONG chipRev)
{
    /* ranges[0] = BAR0 (registers), ranges[1] = BAR1 (framebuffer),
     * ranges[2] = BAR2 (I/O) - as the miniport enumerated them. */
    if (nRanges < 2) return FALSE;
    hw->bar0Phys.QuadPart = ranges[0].RangeStart.QuadPart;
    hw->bar0Len           = ranges[0].RangeLength ? ranges[0].RangeLength
                                                  : R3DFX_BAR0_REG_SIZE;
    hw->bar1Phys.QuadPart = ranges[1].RangeStart.QuadPart;
    hw->bar1Len           = ranges[1].RangeLength ? ranges[1].RangeLength
                                                  : R3DFX_BAR1_FB_SIZE;
    if (nRanges >= 3) hw->ioPort = (USHORT)ranges[2].RangeStart.LowPart;

    /* kernel map of the registers (uncached - MMIO) for our own use (mode-set) */
    hw->bar0Kernel = MmMapIoSpace(hw->bar0Phys, hw->bar0Len, MmNonCached);

    a->vendorID = pciVendor;
    a->deviceID = pciDevice;
    a->chipRev  = chipRev;
    /* fbRam: probe via the FBI init/memory-size register, or trust the BAR/PCI;
     * Voodoo3 ships 16MB. Report in bytes (Glide does fbRam>>20 for MB). */
    a->fbRam    = 16UL << 20;
    /* strides/tiling: Voodoo3 tiled framebuffer. pciStride is the linear pitch
     * the FB aperture uses; hwStride is the hardware x-translation; tileMark is
     * the tile watermark. Set from the mode at exclusive time; defaults here. */
    a->pciStride = 4096;
    a->hwStride  = 4096;
    a->tileMark  = 0;
    return (hw->bar0Kernel != NULL);
}

/* --- GETLINEARADDR: map BAR0/BAR1 into the calling process, return user VAs.
 *     This is exactly what lets user-mode Glide reach the card. We map the
 *     physical BAR ranges into the app via ZwMapViewOfSection over a physical-
 *     memory section (\\Device\\PhysicalMemory), the classic display-driver
 *     technique the retail 3dfx driver used for HWCEXT_GETLINEARADDR. --------- */
static NTSTATUS map_phys_to_user(PHYSICAL_ADDRESS phys, ULONG len, PVOID *userVa)
{
    UNICODE_STRING       name;
    OBJECT_ATTRIBUTES    oa;
    HANDLE               hSec;
    NTSTATUS             st;
    PHYSICAL_ADDRESS     base = phys;
    SIZE_T               view = len;

    RtlInitUnicodeString(&name, L"\\Device\\PhysicalMemory");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwOpenSection(&hSec, SECTION_MAP_READ|SECTION_MAP_WRITE, &oa);
    if (!NT_SUCCESS(st)) return st;

    *userVa = NULL;
    st = ZwMapViewOfSection(hSec, ZwCurrentProcess(), userVa, 0, len, &base,
                            &view, ViewUnmap, 0, PAGE_READWRITE|PAGE_NOCACHE);
    ZwClose(hSec);
    return st;
}

int r3dfx_hw_map_linear_impl(r3dfx_adapter *a, r3dfx_hw *hw, R3DFX_U32 pHandle)
{
    PVOID regsVa = NULL, fbVa = NULL;
    (void)pHandle;   /* mapping into the current (calling) process context      */
    if (!NT_SUCCESS(map_phys_to_user(hw->bar0Phys, hw->bar0Len, &regsVa))) return 0;
    if (!NT_SUCCESS(map_phys_to_user(hw->bar1Phys, hw->bar1Len, &fbVa)))   return 0;
    a->linear[0] = (R3DFX_U32)(ULONG_PTR)regsVa;   /* registers                */
    a->linear[1] = (R3DFX_U32)(ULONG_PTR)fbVa;     /* framebuffer              */
    a->numLinear = 2;
    return 1;
}

void r3dfx_hw_set_exclusive_impl(r3dfx_adapter *a, r3dfx_hw *hw, int on)
{
    /* Cooperative fullscreen: on grab, save the desktop CRTC (disp_modeset)
     * and let Glide own the 2D/3D engine; on release, restore the desktop. */
    (void)a; (void)hw; (void)on;
}

#else /* !HAVE_DDK - keep the logic reviewable/host-linkable for disp_escape's test */

/* Without the DDK, the escape server's host test provides its own stub of
 * r3dfx_hw_map_linear()/r3dfx_hw_set_exclusive(). This TU only compiles the
 * DDK-independent constants above. */
int  r3dfx_hw_stub_marker = 1;

#endif /* HAVE_DDK */
