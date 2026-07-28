/*
 * gbk_mmio.h - thin volatile MMIO accessors for the fxD3D kernel Glide
 *              backend transport (gbkernel.c, M4b-2).
 *
 * These are the ONLY places the transport touches the card. Everything else
 * in gbk_*.c is pure integer math on shadows; gbkernel.c turns those words
 * into real 32-bit stores/loads against the driver-mapped BAR0 (register
 * aperture) and BAR1 (linear framebuffer) through these three primitives.
 *
 * Addressing model (design sec 0): a base pointer (BAR0 or BAR1 virtual
 * address) plus a BYTE offset. Pointer arithmetic is done on a char* so the
 * offset is exact bytes, then the access is a volatile 32-bit load/store -
 * the compiler may neither elide nor reorder it relative to another volatile
 * access (ISO C 5.1.2.3), which is what a memory-mapped register requires.
 *
 * Constraints: C89, no CRT, no floats, kernel-clean (VC6 /Zp8 + mingw + gcc).
 * Only depends on h3u32 (via gbk.h -> hw/h3hw.h).
 */
#ifndef GBK_MMIO_H
#define GBK_MMIO_H

#include "gbk.h"

/* 32-bit volatile store: *(BAR + byteoff) = val.
 * base is a mapped BAR virtual address; byteoff is an exact byte offset. */
#define GBK_WR32(base, byteoff, val) \
    (*(volatile h3u32 *)((char *)(base) + (byteoff)) = (h3u32)(val))

/* 32-bit volatile load: returns *(BAR + byteoff). */
#define GBK_RD32(base, byteoff) \
    (*(volatile h3u32 *)((char *)(base) + (byteoff)))

/*
 * GBK_WMB - write-ordering fence hook (design risk #3).
 *
 * Every access through GBK_WR32/GBK_RD32 is a *volatile* store/load, and the C
 * standard forbids the compiler from reordering volatile accesses relative to
 * one another. On the NON-CACHED mapping used for bring-up (design risk #3:
 * "map non-cached first") that volatile ordering is also the hardware ordering,
 * so GBK_WMB is a documented compiler-barrier no-op: it marks the exact points
 * where write ordering matters (after a batch of FIFO word stores, before a
 * dependent register read/write) without emitting an instruction.
 *
 * When the FIFO is later remapped WRITE-COMBINING for throughput, this hook is
 * the single place to grow a real store fence (an `sfence` / P6FENCE, as glide
 * does in fifo.c:972-989 before it lets the hole-counter see the writes). Left
 * as a macro so that upgrade is one edit and every fence site is already in
 * place. Per the design, a plain compiler barrier is fine for bring-up.
 *
 * ***HARD REQUIREMENT (M4c chassis + gbkernel_attach): BAR1 (the FIFO ring's
 * backing) MUST be mapped NON-CACHED.*** This no-op fence is only correct on a
 * non-cached mapping. When M4c maps video memory for gbkernel_attach it must
 * request non-cached (e.g. VIDEO_MODE_MAP_MEM non-cached / MmNonCached); mapping
 * BAR1 cached or write-combining WITHOUT first upgrading GBK_WMB to a real store
 * fence lets FIFO word stores reorder/coalesce past gbk_fifo_advance -> the
 * hole-counter consumes a partial packet -> CMDFIFO desync -> kernel hang. This
 * requirement is stated here so it is a checked contract, not a silent
 * assumption (M4b-2 review finding).
 */
#define GBK_WMB()   ((void)0)

#endif /* GBK_MMIO_H */
