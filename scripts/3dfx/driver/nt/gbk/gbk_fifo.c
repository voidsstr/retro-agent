/*
 * gbk_fifo.c - CMDFIFO software state + init register list (pure integer).
 * Design: docs/3dfx-gbkernel-design.md sec 1c (hwcInitFifo,
 * H3/minihwc/minihwc.c:1613-1665), sec 1e (FIFO software state,
 * gsst.c:1402-1451), sec 2 transport (space/wrap, fifo.c:838-1056),
 * sec 5 (status polls).
 *
 * The ring protocol invariants this module owns (a violation is a kernel
 * hang or FIFO scribble on real hardware):
 *   - the hardware never auto-wraps; software emits the PKT0 JMP and
 *     resets to ring start when roomToEnd <= blockSize (gsst.c:1406-1415;
 *     fifo.c:960-1013),
 *   - the writer never reaches the hardware read pointer: roomToReadPtr
 *     carries a permanent -4 margin (gsst.c:1419-1421),
 *   - the last FIFO_END_ADJUST (32) bytes of the ring are never used for
 *     data, so the JMP packet always has room (fxcmd.h:180-189).
 * Kernel-clean: C89, no CRT, no floats.
 */
#include "gbk.h"

/* The 10 ordered SstCRegs stores that arm CMDFIFO 0 with hole-counting ON
 * (design sec 1c; hwcInitFifo minihwc.c:1634-1663, Avenger threshold
 * branch :1652-1655). Order is load-bearing: the FIFO must be disabled
 * (baseSize=0) while the pointers move, and armed last.                 */
void
gbk_fifo_init_regs(h3u32 fifoStart, h3u32 fifoLength,
                   gbk_regwrite_t out[GBK_FIFO_INIT_NWRITES])
{
#define GBK_CREG(o) ((h3u32)SST_CMDAGP_OFFSET + (h3u32)(o))
    /* disable the CMD fifo (minihwc.c:1634) */
    out[0].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_baseSize);
    out[0].value     = 0;
    /* base page + read pointer at ring start (:1636-1639) */
    out[1].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_baseAddrL);
    out[1].value     = fifoStart >> 12;
    out[2].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_readPtrL);
    out[2].value     = fifoStart;
    out[3].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_readPtrH);
    out[3].value     = 0;
    /* hole-counting extents just below the ring (:1640-1641) */
    out[4].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_aMin);
    out[4].value     = fifoStart - 4;
    out[5].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_aMax);
    out[5].value     = fifoStart - 4;
    out[6].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_depth);
    out[6].value     = 0;
    out[7].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_holeCount);
    out[7].value     = 0;
    /* Avenger low/high-water thresholds (:1652-1655) */
    out[8].barOffset = GBK_CREG(SSTC_OFF_cmdFifoThresh);
    out[8].value     = H3HW_CMDFIFO_THRESH_AVENGER;
    /* arm: size in 4K pages minus one, enable, holes ON = no
     * DISABLE_HOLES bit, no AGP (:1657-1661)                            */
    out[9].barOffset = GBK_CREG(SSTC_OFF_cmdFifo0_baseSize);
    out[9].value     = ((fifoLength >> 12) - 1) | SST_EN_CMDFIFO;
#undef GBK_CREG
}

/* software state init (gsst.c:1402-1451):
 * roomToEnd = fifoSize - FIFO_END_ADJUST (:1419);
 * fifoRoom = roomToReadPtr = roomToEnd - sizeof(FxU32) (:1420) - the -4
 * margin so we never actually write onto the read ptr; write and read
 * pointers both at ring start (:1425-1427); jmpHdr precomputed
 * (:1446-1448).                                                         */
void
gbk_fifo_init(gbk_fifo_t *f, h3u32 fifoStart, h3u32 fifoLength)
{
    f->fifoOffset = fifoStart;
    f->fifoSize = fifoLength;
    f->writeOfs = 0;
    f->readOfs = 0;
    f->roomToEnd = (h3i32)fifoLength - H3HW_FIFO_END_ADJUST;
    f->roomToReadPtr = f->roomToEnd - 4;
    f->jmpHdr = gbk_pkt0_jmp_local(fifoStart);
}

/* hardware never auto-wraps: if roomToEnd <= nBytes the caller writes
 * jmpHdr at writeOfs and calls gbk_fifo_wrap (fifo.c:960: "if
 * (gc->cmdTransportInfo.roomToEnd <= blockSize)").                      */
int
gbk_fifo_need_wrap(const gbk_fifo_t *f, h3u32 nBytes)
{
    return f->roomToEnd <= (h3i32)nBytes;
}

/* if roomToReadPtr < nBytes the caller polls cmdFifo0.readPtrL
 * (twice-stable, _grHwFifoPtr fifo.c:1068-1090) and folds it in with
 * gbk_fifo_update_read (fifo.c:900: "while (roomToReadPtr < blockSize)").*/
int
gbk_fifo_need_room(const gbk_fifo_t *f, h3u32 nBytes)
{
    return f->roomToReadPtr < (h3i32)nBytes;
}

/* account for nBytes just written: both room counters shrink together
 * (fifo.c:862-866: writes since last check are subtracted from
 * roomToReadPtr AND roomToEnd).                                         */
void
gbk_fifo_advance(gbk_fifo_t *f, h3u32 nBytes)
{
    f->writeOfs += nBytes;
    f->roomToEnd -= (h3i32)nBytes;
    f->roomToReadPtr -= (h3i32)nBytes;
}

/* reset software state after the caller stored jmpHdr at writeOfs
 * (fifo.c:1010-1022): the skipped tail is charged against roomToReadPtr
 * ("roomToReadPtr -= roomToEnd"), roomToEnd resets to the full usable
 * length, write pointer returns to ring start. The charge is exact: when
 * the hardware read pointer later takes the same jump,
 * gbk_fifo_update_read credits a full lap (fifoSize - FIFO_END_ADJUST)
 * and the two errors cancel.                                            */
void
gbk_fifo_wrap(gbk_fifo_t *f)
{
    f->roomToReadPtr -= f->roomToEnd;
    f->roomToEnd = (h3i32)f->fifoSize - H3HW_FIFO_END_ADJUST;
    f->writeOfs = 0;
}

/* fold a fresh (twice-stable) readPtrL readback into roomToReadPtr
 * (fifo.c:922-948): credit the distance the hw consumed since the last
 * poll; if the read pointer moved backwards it wrapped, credit one lap
 * ("if (lastHwRead > curReadPtr) roomToReadPtr += fifoSize -
 * FIFO_END_ADJUST"). readPtrL is the raw board byte address the register
 * holds (init writes readPtrL = fifoStart, minihwc.c:1638).             */
void
gbk_fifo_update_read(gbk_fifo_t *f, h3u32 readPtrL)
{
    h3u32 cur = readPtrL - f->fifoOffset;

    f->roomToReadPtr += (h3i32)cur - (h3i32)f->readOfs;
    if (f->readOfs > cur)
        f->roomToReadPtr += (h3i32)f->fifoSize - H3HW_FIFO_END_ADJUST;
    f->readOfs = cur;
}

/* One pass of the _grMakeRoom protocol (fifo.c:896-1032) for an nBytes
 * write. Mirrors glide's ordering exactly: stall gate first (poll loop,
 * :900-948), then the wrap check (:960), then the write. The 'goto again'
 * re-check after a wrap (:1032) happens naturally because the caller must
 * call reserve again after gbk_fifo_wrap.                               */
int
gbk_fifo_reserve(gbk_fifo_t *f, h3u32 nBytes, h3u32 readPtrL)
{
    if (gbk_fifo_need_room(f, nBytes)) {
        gbk_fifo_update_read(f, readPtrL);
        if (gbk_fifo_need_room(f, nBytes))
            return GBK_FIFO_STALL;
    }
    if (gbk_fifo_need_wrap(f, nBytes))
        return GBK_FIFO_NEED_WRAP;
    return GBK_FIFO_OK;
}

/* gb_finish poll condition (grFinish gsst.c:1914-1940) */
int
gbk_status_busy(h3u32 status3d)
{
    return (status3d & SST_BUSY) != 0;
}

/* swap-throttle helper (_grBufferNumPending gglide.c:1304-1386) */
int
gbk_swaps_pending(h3u32 status3d)
{
    return (int)((status3d & SST_SWAPBUFPENDING) >> SST_SWAPBUFPENDING_SHIFT);
}
