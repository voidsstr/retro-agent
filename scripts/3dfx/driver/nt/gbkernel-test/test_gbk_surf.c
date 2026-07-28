/*
 * test_gbk_surf.c - host tests for gbk_surf.c (DDraw surface layout math +
 * the 5a present-convert, docs/3dfx-d3d-hal-design.md sec 5a). The expected
 * pixel expansions are hand-computed from the 565 field widths in the
 * comments below - derived from the format definition, not from the code
 * under test.
 */
#include "../gbk/gbk.h"
#include "test_common.h"

int
main(void)
{
    gbk_surfheap_t hp;
    h3u16 srow[4];
    h3u32 drow[4];
    h3u32 a, b, c;

    /* ---- gbk_surf_pitch: w*bytespp rounded up to 16 ------------------- */
    CHECK_EQ(gbk_surf_pitch(640, 2), 1280);      /* already aligned        */
    CHECK_EQ(gbk_surf_pitch(1024, 4), 4096);
    CHECK_EQ(gbk_surf_pitch(33, 2), 80);         /* 66 -> 80               */
    CHECK_EQ(gbk_surf_pitch(1, 1), 16);          /* min alloc granule      */
    CHECK_EQ(gbk_surf_pitch(256, 2), 512);       /* max Voodoo3 texture    */
    /* degenerate / oversized requests fail cleanly */
    CHECK_EQ(gbk_surf_pitch(0, 2), 0);
    CHECK_EQ(gbk_surf_pitch(2049, 2), 0);        /* beyond the mode space  */
    CHECK_EQ(gbk_surf_pitch(64, 3), 0);          /* 24bpp not a surface fmt*/
    CHECK_EQ(gbk_surf_pitch(64, 0), 0);

    /* ---- gbk_surf_bytes: pitch*h, overflow-guarded -------------------- */
    CHECK_EQ(gbk_surf_bytes(1280, 480), 1280UL * 480UL);
    CHECK_EQ(gbk_surf_bytes(4096, 768), 4096UL * 768UL);
    CHECK_EQ(gbk_surf_bytes(0, 480), 0);
    CHECK_EQ(gbk_surf_bytes(1280, 0), 0);
    CHECK_EQ(gbk_surf_bytes(1280, 2049), 0);     /* h beyond mode space    */
    CHECK_EQ(gbk_surf_bytes(0x10000, 16), 0);    /* pitch beyond ceiling   */

    /* ---- bump allocator over a reserved region ------------------------ */
    gbk_surfheap_init(&hp, 0x400000, 0x1000);    /* 4 KB at 4 MB           */
    a = gbk_surfheap_alloc(&hp, 0x200);
    CHECK_EQ(a, 0x400000);                       /* base is 16-aligned     */
    b = gbk_surfheap_alloc(&hp, 0x10);
    CHECK_EQ(b, 0x400200);                       /* bumped, still aligned  */
    c = gbk_surfheap_alloc(&hp, 0x1);
    CHECK_EQ(c, 0x400210);                       /* 1-byte alloc, 16-step  */
    CHECK_EQ(hp.live, 3);
    /* exhaustion: room left is 0x1000-0x220 = 0xDE0 */
    CHECK_EQ(gbk_surfheap_alloc(&hp, 0xDE1), GBK_SURF_FAIL);
    CHECK_EQ(gbk_surfheap_alloc(&hp, 0xDE0), 0x400220);  /* exact fit      */
    CHECK_EQ(gbk_surfheap_alloc(&hp, 1), GBK_SURF_FAIL); /* full           */
    /* live-count rewind: all surfaces freed -> cursor back to base */
    CHECK_EQ(gbk_surfheap_free(&hp), 0);
    CHECK_EQ(gbk_surfheap_free(&hp), 0);
    CHECK_EQ(gbk_surfheap_free(&hp), 0);
    CHECK_EQ(hp.live, 1);
    CHECK(hp.cursor != hp.base);                 /* still one live surface */
    CHECK_EQ(gbk_surfheap_free(&hp), 0);
    CHECK_EQ(hp.live, 0);
    CHECK_EQ(hp.cursor, hp.base);                /* rewound                */
    CHECK_EQ(gbk_surfheap_alloc(&hp, 0x1000), 0x400000);  /* full region   */
    CHECK_EQ(gbk_surfheap_free(&hp), 0);
    /* zero-byte and dead-region allocs fail */
    CHECK_EQ(gbk_surfheap_alloc(&hp, 0), GBK_SURF_FAIL);
    gbk_surfheap_init(&hp, 0x400000, 0);
    CHECK_EQ(gbk_surfheap_alloc(&hp, 16), GBK_SURF_FAIL);
    /* a region whose end would wrap 32 bits is poisoned dead */
    gbk_surfheap_init(&hp, 0xFFFFF000UL, 0x2000);
    CHECK_EQ(gbk_surfheap_alloc(&hp, 16), GBK_SURF_FAIL);

    /* ---- unbalanced free refused (enable.c Dd_DestroySurface ownership
     * tags + gbk_surfheap_free guard, M4c-2 review #3) --------------------
     * A free with nothing live must be REFUSED (-1) and must NOT rewind the
     * cursor. The old-buggy void version silently "succeeded": one spurious
     * free (a system-memory fpVidMem numerically aliasing the heap range)
     * unbalanced the count, so the cursor rewound while a real surface was
     * still allocated and the next alloc returned an offset OVERLAPPING it.
     * Fixed: the replay below keeps the second alloc non-overlapping. */
    gbk_surfheap_init(&hp, 0x400000, 0x1000);
    CHECK_EQ(gbk_surfheap_free(&hp), -1);        /* nothing live: refused  */
    CHECK_EQ(hp.cursor, hp.base);
    /* replay of the review-#3 corruption scenario: ONE real surface live
     * when a spurious free arrives (only balanced pairs afterwards). Old
     * code: the spurious free took live 1->0 and rewound the cursor, so the
     * NEXT alloc returned base = an offset ALIASING the live surface. New
     * code: with only real frees the count reaches 0 exactly at the real
     * destroy, so the next alloc never overlaps a live surface, and the
     * extra free itself is refused with the cursor untouched. */
    a = gbk_surfheap_alloc(&hp, 0x200);          /* the REAL live surface  */
    CHECK_EQ(a, 0x400000);
    b = gbk_surfheap_alloc(&hp, 0x200);          /* second real surface    */
    CHECK_EQ(b, 0x400200);                       /* no alias of a          */
    CHECK_EQ(gbk_surfheap_free(&hp), 0);         /* destroy #1 (balanced)  */
    CHECK_EQ(gbk_surfheap_free(&hp), 0);         /* destroy #2 (balanced)  */
    CHECK_EQ(gbk_surfheap_free(&hp), -1);        /* the spurious extra one */
    CHECK_EQ(hp.live, 0);                        /* count never wraps      */
    CHECK_EQ(hp.cursor, hp.base);                /* clean rewind preserved */
    c = gbk_surfheap_alloc(&hp, 0x200);
    CHECK_EQ(c, 0x400000);                       /* heap still consistent  */
    CHECK_EQ(gbk_surfheap_free(&hp), 0);

    /* ---- the 5a 16->32 expansion --------------------------------------
     * RGB565: r=bits15..11, g=bits10..5, b=bits4..0. Bit replication:
     * r8 = r5<<3 | r5>>2, g8 = g6<<2 | g6>>4, b8 = b5<<3 | b5>>2, X=0.
     * Full-scale channels must map to full scale (0x1F/0x3F -> 0xFF). */
    CHECK_EQ(gbk_expand565(0x0000), 0x00000000UL);
    CHECK_EQ(gbk_expand565(0xFFFF), 0x00FFFFFFUL);   /* white stays white  */
    CHECK_EQ(gbk_expand565(0xF800), 0x00FF0000UL);   /* pure red           */
    CHECK_EQ(gbk_expand565(0x07E0), 0x0000FF00UL);   /* pure green         */
    CHECK_EQ(gbk_expand565(0x001F), 0x000000FFUL);   /* pure blue          */
    /* mid-gray 0x8410: r=0x10->0x84, g=0x20->0x82, b=0x10->0x84 */
    CHECK_EQ(gbk_expand565(0x8410), 0x00848284UL);

    /* row loop drives the same expansion per pixel */
    srow[0] = 0xF800; srow[1] = 0x07E0; srow[2] = 0x001F; srow[3] = 0x8410;
    gbk_conv565_row_8888(drow, srow, 4);
    CHECK_EQ(drow[0], 0x00FF0000UL);
    CHECK_EQ(drow[1], 0x0000FF00UL);
    CHECK_EQ(drow[2], 0x000000FFUL);
    CHECK_EQ(drow[3], 0x00848284UL);

    /* ---- ARGB1555 expansion (enable.c fxnt_blt_rect mask dispatch, M4c-2
     * review #11). Field widths from the format definition: a=bit15,
     * r=14..10, g=9..5, b=4..0; 5-bit channels bit-replicated, the alpha
     * bit widened to a full byte. Each case ALSO pins that the old-buggy
     * byte-count dispatch (565 decode of the same pixel) gave a DIFFERENT,
     * hue-shifted value - the defect this expander closes. */
    CHECK_EQ(gbk_expand1555(0x0000), 0x00000000UL);
    CHECK_EQ(gbk_expand1555(0xFFFF), 0xFFFFFFFFUL);  /* opaque white       */
    CHECK_EQ(gbk_expand1555(0x7FFF), 0x00FFFFFFUL);  /* transparent white  */
    CHECK_EQ(gbk_expand1555(0xFC00), 0xFFFF0000UL);  /* opaque pure red    */
    CHECK(gbk_expand565(0xFC00) == 0x00FF8200UL);    /* old bug: orange    */
    CHECK(gbk_expand1555(0xFC00) != gbk_expand565(0xFC00));
    CHECK_EQ(gbk_expand1555(0x83E0), 0xFF00FF00UL);  /* opaque pure green  */
    CHECK(gbk_expand1555(0x83E0) != gbk_expand565(0x83E0));
    CHECK_EQ(gbk_expand1555(0x801F), 0xFF0000FFUL);  /* opaque pure blue   */

    /* ---- ARGB4444 expansion (review #11): a=15..12, r=11..8, g=7..4,
     * b=3..0; every nibble replicated into both halves of its byte. */
    CHECK_EQ(gbk_expand4444(0x0000), 0x00000000UL);
    CHECK_EQ(gbk_expand4444(0xFFFF), 0xFFFFFFFFUL);  /* opaque white       */
    CHECK_EQ(gbk_expand4444(0xFF00), 0xFFFF0000UL);  /* opaque pure red    */
    CHECK(gbk_expand565(0xFF00) == 0x00FFE300UL);    /* old bug: yellow    */
    CHECK(gbk_expand4444(0xFF00) != gbk_expand565(0xFF00));
    CHECK_EQ(gbk_expand4444(0xF0F0), 0xFF00FF00UL);  /* opaque pure green  */
    CHECK_EQ(gbk_expand4444(0xF00F), 0xFF0000FFUL);  /* opaque pure blue   */
    CHECK_EQ(gbk_expand4444(0x8888), 0x88888888UL);  /* nibble replication */

    /* the row drivers apply the same per-pixel math */
    srow[0] = 0xFC00; srow[1] = 0x83E0; srow[2] = 0x801F; srow[3] = 0x7FFF;
    gbk_conv1555_row_8888(drow, srow, 4);
    CHECK_EQ(drow[0], 0xFFFF0000UL);
    CHECK_EQ(drow[1], 0xFF00FF00UL);
    CHECK_EQ(drow[2], 0xFF0000FFUL);
    CHECK_EQ(drow[3], 0x00FFFFFFUL);
    srow[0] = 0xFF00; srow[1] = 0xF0F0; srow[2] = 0xF00F; srow[3] = 0x8888;
    gbk_conv4444_row_8888(drow, srow, 4);
    CHECK_EQ(drow[0], 0xFFFF0000UL);
    CHECK_EQ(drow[1], 0xFF00FF00UL);
    CHECK_EQ(drow[2], 0xFF0000FFUL);
    CHECK_EQ(drow[3], 0x88888888UL);

    /* ---- DP2 buffer-window clamp (enable.c d3d_DrawPrimitives2, review
     * #4/#6): [off, off+len*stride) must fit bufBytes; the old code passed
     * dwCommandOffset/dwVertexLength through UNCHECKED, so every case that
     * must fail below used to reach the executor as an OOB window. */
    /* command-window form (stride 1) */
    CHECK(gbk_dp2_window_ok(1024, 0, 1024, 1));      /* exact fit          */
    CHECK(gbk_dp2_window_ok(1024, 512, 512, 1));
    CHECK(gbk_dp2_window_ok(1024, 1024, 0, 1));      /* empty at the end   */
    CHECK(!gbk_dp2_window_ok(1024, 0, 1025, 1));     /* one byte past      */
    CHECK(!gbk_dp2_window_ok(1024, 1025, 0, 1));     /* offset past        */
    CHECK(!gbk_dp2_window_ok(1024, 512, 513, 1));    /* off+len past       */
    CHECK(!gbk_dp2_window_ok(0, 0, 0, 1));           /* unknown size fails */
    /* vertex-window form (stride = FVF vertex size, e.g. 32) */
    CHECK(gbk_dp2_window_ok(8U * 32U, 0, 8, 32));    /* 8 verts, exact     */
    CHECK(!gbk_dp2_window_ok(8U * 32U, 0, 9, 32));   /* 9th vertex OOB     */
    CHECK(gbk_dp2_window_ok(8U * 32U, 32, 7, 32));   /* offset eats one    */
    CHECK(!gbk_dp2_window_ok(8U * 32U, 32, 8, 32));
    CHECK(!gbk_dp2_window_ok(8U * 32U, 0, 8, 0));    /* stride 0 refused   */
    /* overflow-safety: off+len*stride is NEVER formed, so a huge len that
     * would wrap 32 bits (4 verts short of 2^32 bytes) still fails */
    CHECK(!gbk_dp2_window_ok(1024, 0, 0x0FFFFFFFUL, 32));
    CHECK(!gbk_dp2_window_ok(1024, 0xFFFFFFF0UL, 8, 32));

    /* ---- present dstFormat word (gb_swap dst, design 5a) --------------
     * stride in the linear-stride field | pixfmt code at bit 16
     * (h3hw.h SSTG_DST_LINEAR_STRIDE / SSTG_PIXFMT_*). */
    CHECK_EQ(gbk_present_dstformat(1280, 16), 1280UL | SSTG_PIXFMT_16BPP);
    CHECK_EQ(gbk_present_dstformat(2048, 16), 2048UL | SSTG_PIXFMT_16BPP);
    CHECK_EQ(gbk_present_dstformat(4096, 32), 4096UL | SSTG_PIXFMT_32BPP);
    /* 16bpp plain copy and 32bpp convert-blit select DIFFERENT formats */
    CHECK(gbk_present_dstformat(1280, 16) != gbk_present_dstformat(1280, 32));
    /* unsupported depth / degenerate or oversized stride -> 0 (skip) */
    CHECK_EQ(gbk_present_dstformat(1280, 24), 0);
    CHECK_EQ(gbk_present_dstformat(1280, 0), 0);
    CHECK_EQ(gbk_present_dstformat(0, 16), 0);
    CHECK_EQ(gbk_present_dstformat(0x4000, 16), 0); /* > stride field      */
    /* review #7: gbkernel_attach now stores the REAL desktop depth, so an
     * 8/15/24bpp desktop reaches this guard AS ITSELF and the present is
     * skipped (return 0). The old-buggy attach coerced every non-32 depth
     * to 16, which made the guard pass (nonzero) and programmed a 16bpp
     * blit onto the shorter-stride primary - a scribble past desktopEnd.
     * Pin both halves: the unsupported depths fail, AND the value the old
     * coercion would have used differs (nonzero). */
    CHECK_EQ(gbk_present_dstformat(640, 8), 0);      /* 640x480x8 desktop  */
    CHECK_EQ(gbk_present_dstformat(1280, 15), 0);
    CHECK(gbk_present_dstformat(640, 16) != 0);      /* old coerced result */
    CHECK(gbk_present_dstformat(640, 8) != gbk_present_dstformat(640, 16));

    TEST_END("test_gbk_surf (DDraw surface math + 5a present-convert)");
}
