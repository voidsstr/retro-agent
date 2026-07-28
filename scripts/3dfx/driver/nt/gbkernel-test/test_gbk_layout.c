/*
 * test_gbk_layout.c - host tests for gbk_layout.c (buffer carve-out math).
 * The 16MB expected offsets are hand-walked from the hwcAllocBuffers rules
 * (minihwc.c:1397-1610) in the comments below - derived from the glide
 * source, not from the code under test.
 */
#include "../gbk/gbk.h"
#include "test_common.h"

/* interval overlap helper for the fuzz check */
static int
overlaps(unsigned long a0, unsigned long a1, unsigned long b0,
         unsigned long b1)
{
    return (a0 < b1) && (b0 < a1);   /* [a0,a1) vs [b0,b1) */
}

int
main(void)
{
    /* linear stride = xres<<1 (calcBufferStride minihwc.c:3691-3693) */
    CHECK_EQ(gbk_calc_stride(640), 1280);
    CHECK_EQ(gbk_calc_stride(1024), 2048);

    /* linear buffer size = stride*yres, NOT page-rounded (calcBufferSize
     * minihwc.c:3725-3745; page discipline comes from the allocation
     * parity adjustment, not the size) */
    CHECK_EQ(gbk_calc_buf_size(640, 480), 640UL * 480UL * 2UL);
    CHECK_EQ(gbk_calc_buf_size(800, 600), 800UL * 600UL * 2UL);
    CHECK_EQ(gbk_calc_buf_size(1024, 768), 1024UL * 768UL * 2UL);

    /* carve-out constants (minihwc.c:628-629, :1541, :1461-1492) */
    CHECK_EQ(H3HW_MAXFIFOSIZE, 0x40000UL);
    CHECK_EQ(H3HW_MAXFIFOSIZE_16MB, 0xFF000UL);
    CHECK_EQ(H3HW_FIFO_RESERVE, 0x2000UL);
    CHECK_EQ(H3HW_PAGE, 0x1000UL);
    CHECK_EQ(H3HW_MIN_TEXTURE_STORE, 0x200000UL);   /* gsst.c:953 */
    CHECK_EQ(H3HW_MIN_FIFO_SIZE, 0x10000UL);        /* gsst.c:954 */

    /* texture base must be expressible: 16-byte aligned (h3defs.h:561) */
    CHECK_EQ(SST_TEXTURE_ADDRESS, 0xFFFFFUL << 4);

    /* ---- 640x480x16, 2 color + 1 aux, 16MB board, desktop 1MB --------
     * Hand-walk of the rules (minihwc.c:1449-1541):
     *   bufSize = 1280*480 = 0x96000
     *   aux   = 0x1000000-0x96000 = 0xF6A000, bit12 even -> -0x1000
     *         = 0xF69000
     *   col1  = 0xF69000-0x96000 = 0xED3000, bit12 odd  -> -0x1000
     *         = 0xED2000
     *   col0  = 0xED2000-0x96000 = 0xE3C000, bit12 even -> keep
     *   fifoSize = 0xFF000 (>8MB board), tramOffset = desktopEnd = 0x100000
     *   tramSize = 0xE3C000-0x100000-0xFF000 = 0xC3D000 (>= 2MB min, no
     *   shrink); fifoStart = 0xE3C000-0xFF000 = 0xD3D000;
     *   fifoLength = 0xFF000-0x2000 = 0xFD000 */
    {
        gbk_layout_t l;
        CHECK_EQ(gbk_layout_compute(&l, 16UL << 20, 0x100000UL, 640, 480,
                                    2, 1, 0), 0);
        CHECK_EQ(l.stride, 1280);
        CHECK_EQ(l.bufSize, 0x96000UL);
        CHECK_EQ(l.auxOffset, 0xF69000UL);
        CHECK_EQ(l.colBufOffset[1], 0xED2000UL);
        CHECK_EQ(l.colBufOffset[0], 0xE3C000UL);
        CHECK_EQ(l.tramOffset, 0x100000UL);
        CHECK_EQ(l.tramSize, 0xC3D000UL);
        CHECK_EQ(l.fifoOffset, 0xD3D000UL);
        CHECK_EQ(l.fifoLength, 0xFD000UL);
        /* ordering invariant desktop<=tram<fifo<col<aux<=vram holds */
        CHECK_EQ(gbk_layout_validate(&l), 0);
        /* page parity: color even, aux odd (minihwc.c:1455-1461,1489-92) */
        CHECK((l.colBufOffset[0] & 0x1000UL) == 0);
        CHECK((l.colBufOffset[1] & 0x1000UL) == 0);
        CHECK((l.auxOffset & 0x1000UL) == 0x1000UL);
        /* FIFO pad: fifoOffset+fifoLength+0x2000 == colBuf[0] exactly */
        CHECK_EQ(l.fifoOffset + l.fifoLength + H3HW_FIFO_RESERVE,
                 l.colBufOffset[0]);
    }

    /* ---- FIFO-shrink path: 4MB board, no desktop, 640x480 2+1 --------
     * col0 = 0x23C000 (walk as above from 0x400000); fifoSize would be
     * 0x40000 but tramSize 0x1FC000 < 2MB min -> shrink (minihwc.c:1526):
     * fifoSize = 0x23C000-0x200000 = 0x3C000, fifoLength = 0x3A000 */
    {
        gbk_layout_t l;
        CHECK_EQ(gbk_layout_compute(&l, 4UL << 20, 0, 640, 480, 2, 1, 0), 0);
        CHECK_EQ(l.colBufOffset[0], 0x23C000UL);
        CHECK_EQ(l.tramOffset, 0);
        CHECK_EQ(l.tramSize, 0x200000UL);
        CHECK_EQ(l.fifoOffset, 0x200000UL);
        CHECK_EQ(l.fifoLength, 0x3A000UL);
        CHECK_EQ(gbk_layout_validate(&l), 0);
    }

    /* ---- must fail: undersized board (min-memory check gsst.c:1043) -- */
    {
        gbk_layout_t l;
        /* 4MB board with a 1MB desktop cannot host 3 buffers + minimums */
        CHECK(gbk_layout_compute(&l, 4UL << 20, 0x100000UL, 640, 480,
                                 2, 1, 0) != 0);
        /* huge desktop squeezes everything out on 16MB too */
        CHECK(gbk_layout_compute(&l, 16UL << 20, 0xF00000UL, 640, 480,
                                 2, 1, 0) != 0);
        /* shrink-path failure: enlarged tram minimum can't fit
         * (fifoSize would go negative, minihwc.c:1531-1537) */
        CHECK(gbk_layout_compute(&l, 16UL << 20, 0xB00000UL, 640, 480,
                                 2, 1, 0x400000UL) != 0);
        /* degenerate parameters */
        CHECK(gbk_layout_compute(&l, 16UL << 20, 0, 0, 480, 2, 1, 0) != 0);
        CHECK(gbk_layout_compute(&l, 16UL << 20, 0, 640, 480, 0, 1, 0) != 0);
        CHECK(gbk_layout_compute(&l, 16UL << 20, 0, 640, 480, 4, 1, 0) != 0);
        CHECK(gbk_layout_compute(&l, 16UL << 20, (16UL << 20) + 1, 640, 480,
                                 2, 1, 0) != 0);
    }

    /* ---- fuzz: ok=0 or the full invariant - never a silent overlap --- */
    {
        static const unsigned long vrams[] =
            { 4UL << 20, 8UL << 20, 16UL << 20, 32UL << 20 };
        static const unsigned long modes[][2] =
            { {320, 240}, {640, 480}, {800, 600}, {1024, 768} };
        unsigned long nOk = 0;
        unsigned iv, im, nc, na;
        unsigned long desk;

        for (iv = 0; iv < 4; iv++)
        for (im = 0; im < 4; im++)
        for (nc = 1; nc <= 3; nc++)
        for (na = 0; na <= 1; na++)
        for (desk = 0; desk <= vrams[iv]; desk += 0x2B000UL) {
            gbk_layout_t l;
            unsigned long seg0[8], seg1[8];   /* interval starts/ends */
            unsigned long n = 0, i, j;

            if (gbk_layout_compute(&l, vrams[iv], desk,
                                   modes[im][0], modes[im][1],
                                   nc, na, 0) != 0)
                continue;                      /* refusing is always safe */
            nOk++;

            /* the validator must agree with success */
            CHECK_EQ(gbk_layout_validate(&l), 0);

            /* build every live region and check pairwise disjointness:
             * desktop, tram, fifo(+reserve pad), colBufs, aux */
            seg0[n] = 0;             seg1[n] = l.desktopEnd;        n++;
            seg0[n] = l.tramOffset;  seg1[n] = l.tramOffset + l.tramSize;
            n++;
            seg0[n] = l.fifoOffset;
            seg1[n] = l.fifoOffset + l.fifoLength + H3HW_FIFO_RESERVE;
            n++;
            for (i = 0; i < l.nColBuffers; i++) {
                seg0[n] = l.colBufOffset[i];
                seg1[n] = l.colBufOffset[i] + l.bufSize;
                n++;
            }
            if (l.nAuxBuffers) {
                seg0[n] = l.auxOffset;
                seg1[n] = l.auxOffset + l.bufSize;
                n++;
            }
            for (i = 0; i < n; i++) {
                CHECK(seg1[i] <= vrams[iv]);
                for (j = i + 1; j < n; j++)
                    CHECK(!overlaps(seg0[i], seg1[i], seg0[j], seg1[j]));
            }
        }
        /* the sweep must actually exercise successful layouts */
        CHECK(nOk > 100);
    }

    /* ---- gbk_readback_rows: overflow-safe rect clamp (FXDBG_READBACK) ----
     * SECURITY regression for the 32-bit integer-overflow OOB read in
     * gbkernel_dbg_readback (gbkernel.c). x,y,w,h come straight off the
     * ExtEscape input struct (attacker-controlled). The OLD-BUGGY clamp formed
     * `y + h > uh`, which WRAPS at 2^32: y=1,h=0xFFFFFFFF -> y+h=0, 0>uh is
     * false, so the height clamp was SKIPPED and h stayed 0xFFFFFFFF -> the
     * read loop marched far past the surface (kernel OOB read of the BAR1
     * aperture: info-leak / bugcheck). The FIX compares against the remaining
     * extent (`h > uh - y`), which cannot wrap. */
    {
        h3u32 cw, rows;

        /* normal in-bounds rect, ample buffer: rows==h, width unchanged */
        rows = gbk_readback_rows(0, 0, 640, 480, 640, 480,
                                 640UL * 480UL * 2UL, &cw);
        CHECK_EQ(rows, 480);
        CHECK_EQ(cw, 640);

        /* THE ATTACK: y=1, h=0xFFFFFFFF. BUGGY code left h=0xFFFFFFFF (y+h
         * wrapped to 0); the FIX clamps h to uh-y=479. With an ample buffer,
         * rows must be EXACTLY 479, and y+rows must never exceed uh so the
         * read cannot advance past the surface. */
        cw = 0xdeadUL;
        rows = gbk_readback_rows(0, 1, 640, 0xFFFFFFFFUL, 640, 480,
                                 640UL * 480UL * 2UL, &cw);
        CHECK_EQ(rows, 479);                       /* NOT 0xFFFFFFFF */
        CHECK_EQ(cw, 640);
        CHECK(1UL + (unsigned long)rows <= 480UL); /* stays within height */

        /* width overflow is self-limiting, but the clamp must still bound w to
         * uw-x: x=1,w=0xFFFFFFFF (x+w wraps to 0) -> cw=uw-1=639, not
         * 0xFFFFFFFF. */
        rows = gbk_readback_rows(1, 0, 0xFFFFFFFFUL, 1, 640, 480,
                                 640UL * 480UL * 2UL, &cw);
        CHECK_EQ(rows, 1);
        CHECK_EQ(cw, 639);
        CHECK(1UL + (unsigned long)cw <= 640UL);   /* stays within width */

        /* output-buffer cap: room for only 3 full 640px rows though the
         * surface has 480 -> destination byte count and row walk agree. */
        rows = gbk_readback_rows(0, 0, 640, 480, 640, 480,
                                 3UL * 640UL * 2UL, &cw);
        CHECK_EQ(rows, 3);
        CHECK_EQ(cw, 640);

        /* degenerate / out-of-range inputs all return 0 AND clear *clampedW */
        cw = 0xdeadUL;
        CHECK_EQ(gbk_readback_rows(640, 0, 1, 1, 640, 480, 4096, &cw), 0);/*x>=uw*/
        CHECK_EQ(cw, 0);
        CHECK_EQ(gbk_readback_rows(0, 480, 1, 1, 640, 480, 4096, &cw), 0);/*y>=uh*/
        CHECK_EQ(gbk_readback_rows(0, 0, 0, 1, 640, 480, 4096, &cw), 0);  /*w==0 */
        CHECK_EQ(gbk_readback_rows(0, 0, 1, 0, 640, 480, 4096, &cw), 0);  /*h==0 */
        CHECK_EQ(gbk_readback_rows(0, 0, 1, 1, 640, 480, 1, &cw), 0);     /*out<2*/
        CHECK_EQ(gbk_readback_rows(0, 0, 1, 1, 0, 480, 4096, &cw), 0);    /*uw==0*/
        CHECK_EQ(gbk_readback_rows(0, 0, 1, 1, 640, 0, 4096, &cw), 0);    /*uh==0*/
    }

    TEST_END("test_gbk_layout");
}
