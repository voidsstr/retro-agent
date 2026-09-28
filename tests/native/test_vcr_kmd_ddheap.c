/* test_vcr_kmd_ddheap.c
 *
 * Where vcr-kmd's DirectDraw heap lies (voodoo-cleanroom/vcr-kmd/include/
 * vcr_ddheap.h vcr_dd_heap_range, called by display/vcrdd_ddraw.c heap_range
 * for DrvGetDirectDrawInfo). 2026-09-28, while taking Descent's DOSBox
 * output=ddraw path apart on .124.
 *
 * The desktop sits at the top of video memory, so the heap started at video-
 * memory offset 0 - the one address DirectDraw's heap manager answers for
 * "no memory". The first block cut from such a heap is handed out AT 0, read
 * as a failure by its caller and lost until the next mode set: ddlab vidmem
 * on .124 (desktop 1024x768x32) made 120 surfaces of 512 KB where 121 fit,
 * the lowest at +512 KB. The runtime survives it on its second pass; the D3D
 * HAL's single-pass mipmap-chain allocator does not.
 *
 * Diag\DdHeapFloor = 1 starts the heap one page up. The default (switch
 * absent) must stay the layout every silicon run used, byte for byte - both
 * are pinned here, with the silicon numbers.
 */
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_ddheap.h"

#define MB (1024u * 1024u)
#define VRAM_V5_6000 (64u * MB)             /* a VSA-100 chip in the 256 MB VBIOS mode */

/* the miniport's placement (common/vcr_modes.c vcr_desktop_offset) */
static vcr_u32 desk_at_top(vcr_u32 vram, vcr_u32 stride, vcr_u32 h)
{
    vcr_u32 size = (stride * h + 0xfffu) & ~0xfffu;
    return vram - size;
}

TEST(the_default_layout_is_unchanged_and_starts_at_zero) {
    /* .124's recorder: "DirectDraw HAL: heap 0-3aff000 (60412 KB), primary at
     * 3b00000" at 1280x1024x32, "heap 0-3f69000 ... primary at 3f6a000" at
     * 640x480x16 - what every silicon run so far had */
    vcr_u32 s, e, d;
    d = desk_at_top(VRAM_V5_6000, 1280 * 4, 1024);
    CHECK_EQ_U(d, 0x3b00000u);
    vcr_dd_heap_range(d, 1280 * 4 * 1024, VRAM_V5_6000, 0, &s, &e);
    CHECK_EQ_U(s, 0u);
    CHECK_EQ_U(e, 0x3aff000u);
    d = desk_at_top(VRAM_V5_6000, 640 * 2, 480);
    CHECK_EQ_U(d, 0x3f6a000u);
    vcr_dd_heap_range(d, 640 * 2 * 480, VRAM_V5_6000, 0, &s, &e);
    CHECK_EQ_U(s, 0u);
    CHECK_EQ_U(e, 0x3f69000u);
}

TEST(descents_modes_put_the_primary_where_fbshot_found_it) {
    /* Descent through DOSBox: 640x400x32 scanned out from 03f06000 with
     * stride 0a00 (vcrctl fbshot, 06:57) - the primary at the top, exactly;
     * 640x480x32 from 03ed4000 (the 06:53 run, read before the fbshot mask
     * fix as 00ed4000). The 640x400 scan-out address was right. */
    vcr_u32 s, e, d400 = desk_at_top(VRAM_V5_6000, 0xa00, 400);
    vcr_u32 d480 = desk_at_top(VRAM_V5_6000, 0xa00, 480);
    CHECK_EQ_U(d400, 0x3f06000u);
    CHECK_EQ_U(d480, 0x3ed4000u);
    vcr_dd_heap_range(d400, 0xa00 * 400, VRAM_V5_6000, 0, &s, &e);
    CHECK_EQ_U(e, 0x3f05000u);                  /* the cursor page under the desktop */
    CHECK(e + 0x1000u == d400, "heap, cursor page, desktop - back to back");
}

TEST(offset_zero_is_the_heap_managers_failure_value) {
    /* ddlab vidmem, evidence/silicon/256mb/ddlab_vidmem.txt: desktop
     * 1024x768x32, 512 KB surfaces. Old layout: 121 slots from 0, one of
     * them AT 0 - which HeapVidMemAllocAligned returns as "no memory" - so
     * 120 made it (from_primary_lo -63438848 = the slot at +512 KB). */
    vcr_u32 s, e, d = desk_at_top(VRAM_V5_6000, 1024 * 4, 768), slots;
    const vcr_u32 surf = 512u * 1024u;
    CHECK_EQ_U(d, 0x3d00000u);
    vcr_dd_heap_range(d, 1024 * 4 * 768, VRAM_V5_6000, 0, &s, &e);
    CHECK_EQ_U(e - s, 63959040u);               /* its RESULT's vidmem_total */
    slots = (e - s) / surf;
    CHECK_EQ_U(slots, 121u);
    CHECK_EQ_U(s, 0u);                          /* the bug: slot 0 is AT 0 */
    CHECK_EQ_U(d - (s + surf), 63438848u);      /* the lowest that worked: slot 1 */
    /* With the floor: no block can be at 0, and the page it costs still
     * leaves 121 slots - the lowest at +4 KB. */
    vcr_dd_heap_range(d, 1024 * 4 * 768, VRAM_V5_6000, VCR_DD_HEAP_FLOOR, &s, &e);
    CHECK_EQ_U(s, 0x1000u);
    CHECK(s != 0, "no heap block starts at the failure value");
    CHECK_EQ_U(e, 0x3cff000u);                  /* the top is untouched */
    CHECK_EQ_U((e - s) / surf, 121u);
}

TEST(the_floor_is_rounded_to_a_page_and_never_moves_the_top) {
    vcr_u32 s, e, d = desk_at_top(VRAM_V5_6000, 0xa00, 480);
    vcr_dd_heap_range(d, 0xa00 * 480, VRAM_V5_6000, 1, &s, &e);
    CHECK_EQ_U(s, 0x1000u);
    vcr_dd_heap_range(d, 0xa00 * 480, VRAM_V5_6000, 0x1001, &s, &e);
    CHECK_EQ_U(s, 0x2000u);
    CHECK_EQ_U(e, (d - 0x1000u) & ~0xfffu);
}

TEST(the_vm_layout_is_above_the_desktop_either_way) {
    /* the QEMU bed: Bochs VGA, desktop at 0, 16 MB */
    vcr_u32 s, e;
    vcr_dd_heap_range(0, 1024 * 4 * 768, 16u * MB, 0, &s, &e);
    CHECK_EQ_U(s, 0x300000u);
    CHECK_EQ_U(e, 16u * MB);
    vcr_dd_heap_range(0, 1024 * 4 * 768, 16u * MB, VCR_DD_HEAP_FLOOR, &s, &e);
    CHECK_EQ_U(s, 0x300000u);                   /* already off 0: unchanged */
    CHECK_EQ_U(e, 16u * MB);
    /* an odd-sized desktop rounds up to the next page */
    vcr_dd_heap_range(0, 800 * 3 * 600 + 5, 16u * MB, 0, &s, &e);
    CHECK_EQ_U(s, 0x160000u);
}

TEST(no_room_is_an_empty_heap_never_a_reversed_one) {
    vcr_u32 s, e;
    /* the smallest top layout: one page of heap room at most */
    vcr_dd_heap_range(0x2000, 0x1000, 0x3000, VCR_DD_HEAP_FLOOR, &s, &e);
    CHECK_EQ_U(s, 0x1000u);
    CHECK_EQ_U(e, 0x1000u);                     /* empty: DrvGetDirectDrawInfo reports 0 heaps */
    vcr_dd_heap_range(0x2000, 0x1000, 0x3000, 0, &s, &e);
    CHECK_EQ_U(s, 0u);
    CHECK_EQ_U(e, 0x1000u);
    /* a desktop that fills the VM's memory */
    vcr_dd_heap_range(0, 16u * MB, 16u * MB, 0, &s, &e);
    CHECK(e == s, "no room above a full-memory desktop");
    /* a floor above the top: empty, not reversed */
    vcr_dd_heap_range(0x3000, 0x1000, 0x4000, 0x10000, &s, &e);
    CHECK(e >= s, "never reversed");
    CHECK(e == s, "and empty");
}

MUNIT_MAIN("vcr-kmd DirectDraw heap range", {
    RUN(the_default_layout_is_unchanged_and_starts_at_zero);
    RUN(descents_modes_put_the_primary_where_fbshot_found_it);
    RUN(offset_zero_is_the_heap_managers_failure_value);
    RUN(the_floor_is_rounded_to_a_page_and_never_moves_the_top);
    RUN(the_vm_layout_is_above_the_desktop_either_way);
    RUN(no_room_is_an_empty_heap_never_a_reversed_one);
})
