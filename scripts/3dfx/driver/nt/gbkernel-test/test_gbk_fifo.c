/*
 * test_gbk_fifo.c - host tests for gbk_fifo.c (CMDFIFO ring protocol).
 * Deterministic walks are hand-computed from the fifo.c protocol; the
 * randomized section models the hardware reader byte-for-byte and fails
 * if the software state ever grants a write that would overwrite unread
 * bytes or leave the ring - the exact invariants whose violation is a
 * kernel hang / FIFO scribble on the Voodoo3.
 */
#include "../gbk/gbk.h"
#include "test_common.h"

/* ---- randomized-model plumbing ---------------------------------------- */

#define RING_BASE 0x4000UL
#define RING_SIZE 0x200UL            /* 512 B: usable C = 512-32 = 480 */
#define MAX_SEGS  256                /* live segments >= 4 B each: ample */

static unsigned char ringBusy[RING_SIZE];   /* 1 = written, unread     */

typedef struct seg {
    unsigned long ofs, len, consumed;
    int isJmp;
} seg_t;
static seg_t segs[MAX_SEGS];                /* circular queue          */
static unsigned long segHead, segTail;      /* [head, tail), mod SEGS  */
static unsigned long readPos;               /* model hw read ptr (ring) */
static unsigned long unread;                /* total unconsumed bytes  */

/* deterministic LCG (numerical recipes) so failures reproduce */
static unsigned long rngState = 0x3DF5C7E1UL;
static unsigned long
rng(void)
{
    rngState = (rngState * 1664525UL + 1013904223UL) & 0xFFFFFFFFUL;
    return rngState >> 8;
}

/* record a write the DUT just granted; fails the test on any overlap */
static void
model_write(unsigned long ofs, unsigned long len, int isJmp)
{
    unsigned long i;

    CHECK(ofs + len <= RING_SIZE);           /* never leaves the ring   */
    for (i = 0; i < len; i++) {
        CHECK(ringBusy[ofs + i] == 0);       /* never overwrites unread */
        ringBusy[ofs + i] = 1;
    }
    CHECK(segTail - segHead < MAX_SEGS);     /* queue capacity          */
    segs[segTail % MAX_SEGS].ofs = ofs;
    segs[segTail % MAX_SEGS].len = len;
    segs[segTail % MAX_SEGS].consumed = 0;
    segs[segTail % MAX_SEGS].isJmp = isJmp;
    segTail++;
    unread += len;
}

/* hardware reader: consume up to want bytes in write order, following
 * JMP packets back to ring start exactly as the CMDFIFO does           */
static void
model_consume(unsigned long want)
{
    while (want > 0 && segHead < segTail) {
        seg_t *sg = &segs[segHead % MAX_SEGS];
        unsigned long avail = sg->len - sg->consumed;
        unsigned long t = (want < avail) ? want : avail;
        unsigned long i;

        for (i = 0; i < t; i++)
            ringBusy[sg->ofs + sg->consumed + i] = 0;
        sg->consumed += t;
        want -= t;
        unread -= t;
        if (sg->consumed == sg->len) {
            readPos = sg->isJmp ? 0 : (sg->ofs + sg->len);
            segHead++;
        } else {
            readPos = sg->ofs + sg->consumed;
        }
    }
}

int
main(void)
{
    /* cmdFifo0 register byte offsets within SstCRegs (h3regs.h:117-160;
     * cmdFifo0 at +0x20, design sec 0) */
    CHECK_EQ(SSTC_OFF_cmdFifo0_baseAddrL, 0x20);
    CHECK_EQ(SSTC_OFF_cmdFifo0_baseSize, 0x24);
    CHECK_EQ(SSTC_OFF_cmdFifo0_readPtrL, 0x2C);
    CHECK_EQ(SSTC_OFF_cmdFifo0_aMin, 0x34);
    CHECK_EQ(SSTC_OFF_cmdFifo0_aMax, 0x3C);
    CHECK_EQ(SSTC_OFF_cmdFifo0_depth, 0x44);
    CHECK_EQ(SSTC_OFF_cmdFifo0_holeCount, 0x48);
    CHECK_EQ(SSTC_OFF_cmdFifoThresh, 0x80);
    CHECK_EQ(SST_EN_CMDFIFO, 1UL << 8);
    CHECK_EQ(SST_CMDFIFO_DISABLE_HOLES, 1UL << 10);
    CHECK_EQ(H3HW_CMDFIFO_THRESH_AVENGER, (0xFUL << 5) | 0x8UL);
    CHECK_EQ(H3HW_FIFO_END_ADJUST, 32);

    /* status bits (h3defs.h:97-110) + helpers */
    CHECK_EQ(SST_BUSY, 1UL << 9);
    CHECK(gbk_status_busy(SST_BUSY));
    CHECK(!gbk_status_busy(SST_FBI_BUSY));   /* only BIT9 = all-idle gate */
    CHECK_EQ(gbk_swaps_pending(3UL << 28), 3);
    CHECK_EQ(gbk_swaps_pending(0), 0);

    /* ---- the 10 ordered init stores (hwcInitFifo minihwc.c:1634-1663) -
     * for the 16MB layout's FIFO (fifoStart 0xD3D000, length 0xFD000)   */
    {
        gbk_regwrite_t w[GBK_FIFO_INIT_NWRITES];
        gbk_fifo_init_regs(0xD3D000UL, 0xFD000UL, w);
        CHECK_EQ(w[0].barOffset, 0x80000UL + 0x24UL);  /* baseSize        */
        CHECK_EQ(w[0].value, 0);                       /* disable first   */
        CHECK_EQ(w[1].barOffset, 0x80000UL + 0x20UL);  /* baseAddrL       */
        CHECK_EQ(w[1].value, 0xD3DUL);                 /* fifoStart>>12   */
        CHECK_EQ(w[2].barOffset, 0x80000UL + 0x2CUL);  /* readPtrL        */
        CHECK_EQ(w[2].value, 0xD3D000UL);
        CHECK_EQ(w[3].barOffset, 0x80000UL + 0x30UL);  /* readPtrH        */
        CHECK_EQ(w[3].value, 0);
        CHECK_EQ(w[4].barOffset, 0x80000UL + 0x34UL);  /* aMin            */
        CHECK_EQ(w[4].value, 0xD3CFFCUL);              /* fifoStart-4     */
        CHECK_EQ(w[5].barOffset, 0x80000UL + 0x3CUL);  /* aMax            */
        CHECK_EQ(w[5].value, 0xD3CFFCUL);
        CHECK_EQ(w[6].barOffset, 0x80000UL + 0x44UL);  /* depth           */
        CHECK_EQ(w[6].value, 0);
        CHECK_EQ(w[7].barOffset, 0x80000UL + 0x48UL);  /* holeCount       */
        CHECK_EQ(w[7].value, 0);
        CHECK_EQ(w[8].barOffset, 0x80000UL + 0x80UL);  /* cmdFifoThresh   */
        CHECK_EQ(w[8].value, (0xFUL << 5) | 0x8UL);    /* Avenger :1653   */
        CHECK_EQ(w[9].barOffset, 0x80000UL + 0x24UL);  /* baseSize arm    */
        /* ((len>>12)-1) | EN_CMDFIFO, holes ON (:1657-1661) */
        CHECK_EQ(w[9].value, 0xFCUL | (1UL << 8));
        CHECK((w[9].value & SST_CMDFIFO_DISABLE_HOLES) == 0);
    }

    /* ---- software init (gsst.c:1402-1451) ----------------------------- */
    {
        gbk_fifo_t f;
        gbk_fifo_init(&f, 0x0FE000UL, 0x3E000UL);
        CHECK_EQ(f.jmpHdr, SSTCP_PKT0_JMP_LOCAL | (0x0FE000UL << 4));
        CHECK_EQ(f.fifoOffset, 0x0FE000UL);
        CHECK_EQ(f.fifoSize, 0x3E000UL);
        CHECK_EQ(f.writeOfs, 0);
        /* roomToEnd = size - FIFO_END_ADJUST (:1419) */
        CHECK_EQ(f.roomToEnd, 0x3E000L - 32L);
        /* the -4 margin: roomToReadPtr = roomToEnd - 4 (:1420) */
        CHECK_EQ(f.roomToReadPtr, f.roomToEnd - 4);
    }

    /* ---- deterministic wrap walk (ring 0x100 @ 0x1000, C=224) --------- */
    {
        gbk_fifo_t f;
        gbk_fifo_init(&f, 0x1000UL, 0x100UL);
        CHECK_EQ(f.roomToEnd, 224);
        CHECK_EQ(f.roomToReadPtr, 220);

        /* three 64-byte writes fit without wrapping */
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL), GBK_FIFO_OK);
        gbk_fifo_advance(&f, 64);
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL), GBK_FIFO_OK);
        gbk_fifo_advance(&f, 64);
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL), GBK_FIFO_OK);
        gbk_fifo_advance(&f, 64);
        CHECK_EQ(f.writeOfs, 192);
        CHECK_EQ(f.roomToEnd, 32);
        CHECK_EQ(f.roomToReadPtr, 28);

        /* 4th write crosses the usable end: reader has consumed 128 so
         * the stall gate passes, then roomToEnd(32) <= 64 -> wrap
         * (fifo.c:960); after the JMP the room accounting is
         * roomToReadPtr -= roomToEnd; roomToEnd = C (fifo.c:1010-1013) */
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL + 128UL),
                 GBK_FIFO_NEED_WRAP);
        CHECK_EQ(f.roomToReadPtr, 156);      /* 28 + 128 consumed       */
        CHECK_EQ(f.writeOfs, 192);           /* JMP goes here           */
        gbk_fifo_wrap(&f);
        CHECK_EQ(f.writeOfs, 0);
        CHECK_EQ(f.roomToEnd, 224);
        CHECK_EQ(f.roomToReadPtr, 124);      /* 156 - 32 skipped        */
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL + 128UL), GBK_FIFO_OK);
        gbk_fifo_advance(&f, 64);
        CHECK_EQ(f.roomToReadPtr, 60);
    }

    /* ---- stall + reader-wrap credit ----------------------------------- */
    {
        gbk_fifo_t f;
        gbk_fifo_init(&f, 0x1000UL, 0x100UL);

        CHECK_EQ(gbk_fifo_reserve(&f, 200, 0x1000UL), GBK_FIFO_OK);
        gbk_fifo_advance(&f, 200);
        CHECK_EQ(f.roomToReadPtr, 20);
        /* nothing consumed -> must STALL, never wrap onto unread bytes */
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL), GBK_FIFO_STALL);
        /* reader consumed 100 -> room; end is short -> wrap */
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL + 100UL),
                 GBK_FIFO_NEED_WRAP);
        CHECK_EQ(f.roomToReadPtr, 120);
        gbk_fifo_wrap(&f);                   /* JMP written at 200      */
        CHECK_EQ(f.roomToReadPtr, 96);       /* 120 - roomToEnd(24)     */
        CHECK_EQ(gbk_fifo_reserve(&f, 64, 0x1000UL + 100UL), GBK_FIFO_OK);
        gbk_fifo_advance(&f, 64);
        CHECK_EQ(f.roomToReadPtr, 32);

        /* reader finishes the old lap, takes the JMP at 200, consumes
         * the new 64 bytes -> readPtrL = base+64, behind our last poll
         * (100): update credits (64-100) + one lap C=224 (fifo.c:944)
         * => 32 - 36 + 224 = 220 = the empty-ring -4 margin value      */
        gbk_fifo_update_read(&f, 0x1000UL + 64UL);
        CHECK_EQ(f.roomToReadPtr, 220);
        CHECK_EQ(f.readOfs, 64);
    }

    /* ---- randomized protocol fuzz against the byte-exact reader model -
     * 20000 operations; any grant that would scribble on unread bytes,
     * leave the ring, run the counters negative, or deadlock fails.     */
    {
        gbk_fifo_t f;
        unsigned long iter, nWrites = 0, nWraps = 0, nStalls = 0;

        gbk_fifo_init(&f, RING_BASE, RING_SIZE);
        segHead = segTail = 0;
        readPos = 0;
        unread = 0;

        for (iter = 0; iter < 20000; iter++) {
            if (rng() % 3 != 0) {
                /* attempt a write of 4..120 bytes (word multiples) */
                unsigned long n = 4UL * (1UL + rng() % 30UL);
                int act = gbk_fifo_reserve(&f, n, RING_BASE + readPos);

                if (act == GBK_FIFO_OK) {
                    /* grant must stay inside the usable ring...        */
                    CHECK(f.writeOfs + n <= RING_SIZE - 32UL);
                    /* ...and never touch an unread byte               */
                    model_write(f.writeOfs, n, 0);
                    gbk_fifo_advance(&f, n);
                    nWrites++;
                } else if (act == GBK_FIFO_NEED_WRAP) {
                    /* the 4-byte JMP packet obeys the same rules      */
                    model_write(f.writeOfs, 4, 1);
                    gbk_fifo_wrap(&f);
                    nWraps++;
                } else {
                    CHECK_EQ(act, GBK_FIFO_STALL);
                    /* stalling with nothing left to consume = deadlock */
                    CHECK(unread > 0);
                    nStalls++;
                    model_consume(4UL * (1UL + rng() % 16UL));
                }
            } else if (unread > 0) {
                /* reader makes word-granular progress */
                model_consume(4UL * (rng() % (unread / 4UL + 1UL)));
            }
            /* glide's own live invariant (fifo.c:885-886) */
            CHECK(f.roomToEnd >= 0);
            CHECK(f.roomToReadPtr >= 0);
            if (gbk_test_failures)
                break;                        /* stop at first scribble */
        }
        /* the run must exercise all three protocol paths */
        CHECK(nWrites > 1000);
        CHECK(nWraps > 20);
        CHECK(nStalls > 20);
    }

    TEST_END("test_gbk_fifo");
}
