/*
 * vcr_ddheap.h - where the DirectDraw heap lies in video memory: the range
 * the display DLL's DrvGetDirectDrawInfo declares (display/vcrdd_ddraw.c).
 * Pure arithmetic - no OS headers - so the host test (tests/native/
 * test_vcr_kmd_ddheap.c) runs this same code.
 *
 * THE LAYOUT. On the Voodoo the desktop sits at the TOP of video memory
 * (vcr_desktop_offset) and the hardware cursor's pattern one page under it
 * (miniport/vcrmp_cursor.c), so the heap is everything below the cursor page
 * - from offset 0 up. On the VM test bed (Bochs VGA) the desktop sits at 0
 * and the heap is everything above it.
 *
 * OFFSET 0 IS DIRECTDRAW'S "NO MEMORY". HeapVidMemAllocAligned - the heap
 * manager the runtime places surfaces with, and the one the D3D HAL's
 * mipmap-chain allocator calls itself (vcrdd_d3d.c VcrDdD3dCreateMipChain)
 * - returns 0 for a failed allocation. A heap that starts at 0 hands its
 * first block out AT 0: the block leaves the free list and the caller reads
 * the answer as a failure, so it is lost until the heap is rebuilt (the next
 * mode set). Measured on .124 (evidence/silicon/256mb/ddlab_vidmem.txt,
 * desktop 1024x768x32, heap 0 .. 0x3CFF000): 120 surfaces of 512 KB where
 * 121 fit, the lowest at +512 KB - the slot at 0 never handed out. The
 * runtime itself survives it (its second pass through the heaps takes the
 * next block), a single-pass caller does not: a mipmap chain that is the
 * first block of a fresh heap gets DDERR_OUTOFVIDEOMEMORY.
 *
 * THE FLOOR. `floor` (bytes, rounded up to a page) is where the heap may
 * begin at the earliest; VCR_DD_HEAP_FLOOR - one page - is the smallest that
 * keeps every block off 0. It is applied only with Diag\DdHeapFloor = 1
 * (vcr_info.flags VCR_INFO_F_DDHEAPFLOOR): unproven on silicon, so the
 * default (floor 0) is the layout every silicon run so far used, to the byte.
 * With it on, ddlab vidmem on the same desktop should report 121 surfaces,
 * the lowest at +4 KB.
 */
#ifndef VCR_DDHEAP_H
#define VCR_DDHEAP_H

#include "vcr_types.h"

#define VCR_DD_HEAP_FLOOR   0x1000u     /* one page: no block at offset 0 */
#define VCR_DD_PAGE_MASK    0xfffu

/* [*start, *end) for a desktop at `desk` of `desk_bytes` bytes, in `vram`
 * bytes of video memory. An empty heap has *end == *start (never below). */
static inline void vcr_dd_heap_range(vcr_u32 desk, vcr_u32 desk_bytes, vcr_u32 vram,
                                     vcr_u32 floor, vcr_u32 *start, vcr_u32 *end)
{
    vcr_u32 lo = floor ? (floor + VCR_DD_PAGE_MASK) & ~VCR_DD_PAGE_MASK : 0;
    if (desk >= 0x2000) {
        /* the desktop at the top: below it and the cursor page */
        *start = 0;
        *end = (desk - 0x1000) & ~VCR_DD_PAGE_MASK;
    } else {
        /* the desktop at 0 (the VM): above it */
        *start = (desk + desk_bytes + VCR_DD_PAGE_MASK) & ~VCR_DD_PAGE_MASK;
        *end = vram & ~VCR_DD_PAGE_MASK;
    }
    if (*start < lo)
        *start = lo;
    if (*end < *start)
        *end = *start;
}

#endif /* VCR_DDHEAP_H */
