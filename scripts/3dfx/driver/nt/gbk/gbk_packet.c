/*
 * gbk_packet.c - CMDFIFO packet header builders (pure integer math).
 * Design: docs/3dfx-gbkernel-design.md sec 2 (PKT1/3/4) and sec 4 (PKT5).
 * Kernel-clean: C89, no CRT, no floats.
 *
 * The header formulas below are direct transcriptions of the cited
 * open-glide encodings; the kernel half (gbkernel.c, M4b-2) adds the
 * vertex-copy / group-emit helpers on top.
 */
#include "gbk.h"

/* PKT1 header, same-register write (STORE_FIFO fxcmd.h:737-756;
 * h3gdefs.h:208-218): (nWords<<16) | 2D? | chip[13:11] | regBase[12:3] | 1.
 * No SSTCP_INC: all data words land on the one register.              */
h3u32
gbk_pkt1_hdr(h3u32 regByteOff, h3u32 nWords, h3u32 chipField, int is2d)
{
    h3u32 hdr = (nWords << SSTCP_PKT1_NWORDS_SHIFT)
              | SSTCP_REGBASE_FROM_ADDR(regByteOff)
              | (chipField << kChipFieldShift)
              | SSTCP_PKT1;
    if (is2d)
        hdr |= SSTCP_PKT1_2D;
    return hdr;
}

/* PKT1 burst header, sequential registers (REG_GROUP_LONG_BEGIN
 * fxcmd.h:608-620): same as above | SSTCP_INC. Used for the 32-word
 * fogTable download (gglide.c:2091-2117). 3D register space only.     */
h3u32
gbk_pkt1_burst_hdr(h3u32 regByteOff, h3u32 nWords, h3u32 chipField)
{
    return gbk_pkt1_hdr(regByteOff, nWords, chipField, 0) | SSTCP_INC;
}

/* PKT3 header (fxcmd.h:898-917; h3gdefs.h:225-237):
 * (smode<<22) | (pmask<<10) | (nVerts<<6) | cmd[5:3] | 3             */
h3u32
gbk_pkt3_hdr(h3u32 smode, h3u32 pmask, h3u32 nVerts, h3u32 cmd)
{
    /* numVertex is a 4-bit field [9:6]. The M4b-2 draw seam MUST split batches
     * at SSTCP_PKT3_MAXVERTS (15); mask here as defense-in-depth so an
     * out-of-range count can never leak a bit into SSTCP_PKT3_PMASK and desync
     * the CMDFIFO (a kernel hang). See test_gbk_packet nVerts=16 case. */
    return (smode << SSTCP_PKT3_SMODE_SHIFT)
         | (pmask << SSTCP_PKT3_PMASK_SHIFT)
         | ((nVerts << SSTCP_PKT3_NUMVERTEX_SHIFT) & SSTCP_PKT3_NUMVERTEX)
         | cmd
         | SSTCP_PKT3;
}

/* PKT4 header (fxcmd.h:573-608; h3gdefs.h:240-243):
 * (mask14<<15) | 2D? | chip[13:11] | regBase[12:3] | 4               */
h3u32
gbk_pkt4_hdr(h3u32 baseRegByteOff, h3u32 mask14, h3u32 chipField, int is2d)
{
    h3u32 hdr = (mask14 << SSTCP_PKT4_MASK_SHIFT)
              | SSTCP_REGBASE_FROM_ADDR(baseRegByteOff)
              | (chipField << kChipFieldShift)
              | SSTCP_PKT4;
    if (is2d)
        hdr |= SSTCP_PKT4_2D;
    return hdr;
}

/* data words carried by a PKT4 = popcount(mask14) - one per set bit   */
h3u32
gbk_pkt4_nwords(h3u32 mask14)
{
    h3u32 n = 0;
    mask14 &= 0x3FFFUL;
    while (mask14) {
        mask14 &= mask14 - 1;
        n++;
    }
    return n;
}

/* PKT5 hdr1 (h3gdefs.h:245-257; FIFO_LINEAR_WRITE fxcmd.h:1046-1079):
 * space[31:30] | byteEnW2[29:26] | byteEnWN[25:22] | (nWords<<3) | 5  */
h3u32
gbk_pkt5_hdr1(h3u32 space, h3u32 nWords, h3u32 byteEnW2, h3u32 byteEnWN)
{
    /* nWords is a 19-bit field [21:3] (max 0x7FFFF words ~= 2 MB). The M4b-2
     * linear-write helper MUST chunk larger writes; mask here so a too-large
     * count can never overflow into the byte-enable fields and desync the FIFO.
     * See test_gbk_packet nWords>=0x80000 case. */
    return space
         | (byteEnW2 << SSTCP_PKT5_BYTEN_W2_SHIFT)
         | (byteEnWN << SSTCP_PKT5_BYTEN_WN_SHIFT)
         | ((nWords << SSTCP_PKT5_NWORDS_SHIFT) & SSTCP_PKT5_NWORDS)
         | SSTCP_PKT5;
}

/* PKT5 hdr2: destination base byte address (h3gdefs.h:257)            */
h3u32
gbk_pkt5_hdr2(h3u32 destByteAddr)
{
    return destByteAddr & SSTCP_PKT5_BASEADDR;
}

/* PKT0 JMP_LOCAL wrap packet (gsst.c:1446-1448; h3gdefs.h:197-205):
 * fifoJmpHdr = SSTCP_PKT0_JMP_LOCAL | (fifoByteOff<<(ADDR_SHIFT-2))
 * (the address field is in words; byteOff>>2 words << ADDR_SHIFT)     */
h3u32
gbk_pkt0_jmp_local(h3u32 fifoByteOff)
{
    return SSTCP_PKT0_JMP_LOCAL
         | (fifoByteOff << (SSTCP_PKT0_ADDR_SHIFT - 2));
}
