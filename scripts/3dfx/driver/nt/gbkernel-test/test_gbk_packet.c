/*
 * test_gbk_packet.c - host tests for gbk_packet.c (CMDFIFO packet builders).
 * Every expected value is hand-derived from the design doc formulas
 * (docs/3dfx-gbkernel-design.md sec 2/4) and the open glide encodings
 * cited there, NOT from the code under test.
 */
#include "../gbk/gbk.h"
#include "test_common.h"

int
main(void)
{
    /* field placement invariants (h3gdefs.h:181-243; design sec 2:
     * "SSTCP_PKT_SIZE=3, PMASK shift 10, SMODE shift 22") */
    CHECK_EQ(SSTCP_PKT_SIZE, 3);
    CHECK_EQ(SSTCP_PKT3_NUMVERTEX_SHIFT, 6);
    CHECK_EQ(SSTCP_PKT3_PMASK_SHIFT, 10);
    CHECK_EQ(SSTCP_PKT3_SMODE_SHIFT, 22);
    CHECK_EQ(SSTCP_PKT4_MASK_SHIFT, 15);
    CHECK_EQ(SSTCP_PKT1_NWORDS_SHIFT, 16);
    CHECK_EQ(kChipFieldShift, 11);

    /* one gouraud triangle, pmask RGB|A|Z|Wfbi = 0xF, no cull:
     * hdr = (pmask<<10)|(3<<6)|BDDBDD|3 (design sec 2) */
    CHECK_EQ(gbk_pkt3_hdr(0,
                          SST_SETUP_RGB | SST_SETUP_A |
                          SST_SETUP_Z | SST_SETUP_Wfbi,
                          3, SSTCP_PKT3_BDDBDD),
             (0xFUL << 10) | (3UL << 6) | 3UL);

    /* textured version adds ST0 (pmask 0x2F), with hw cull-negative:
     * smode = kSetupCullEnable|kSetupCullNegative = 0x6 in [27:22] */
    CHECK_EQ(gbk_pkt3_hdr(kSetupCullEnable | kSetupCullNegative,
                          SST_SETUP_RGB | SST_SETUP_A | SST_SETUP_Z |
                          SST_SETUP_Wfbi | SST_SETUP_ST0,
                          3, SSTCP_PKT3_BDDBDD),
             (0x6UL << 22) | (0x2FUL << 10) | (3UL << 6) | 3UL);

    /* cull bits land in [27:22] as smode (fxcmd.h:544-551) */
    CHECK_EQ(gbk_pkt3_hdr(kSetupCullEnable | kSetupCullNegative |
                          kSetupPingPongDisable, 0, 0, 0) >> 22,
             0x02UL | 0x04UL | 0x08UL);

    /* GUARD: nVerts>15 (4-bit field [9:6]) must NOT leak into the pmask field
     * [21:10] - a leak corrupts both count and mask -> FIFO desync -> kernel
     * hang. The M4b-2 draw seam splits at SSTCP_PKT3_MAXVERTS; the encoder
     * masks defensively. */
    CHECK_EQ(gbk_pkt3_hdr(0, 0, 16, SSTCP_PKT3_BDDBDD) & SSTCP_PKT3_PMASK, 0UL);
    CHECK_EQ((gbk_pkt3_hdr(0, 0, 16, 0) & SSTCP_PKT3_NUMVERTEX)
                 >> SSTCP_PKT3_NUMVERTEX_SHIFT, 0UL);   /* 16 & 0xF == 0 */

    /* single-register PKT1: design sec 2 "hdr=(1<<16)|(regOffset<<1)|1"
     * (regOffset in bytes; REGBASE_FROM_ADDR(x) == x<<1 for aligned regs;
     * this exact value is the nopCMD=0 header, STORE_FIFO fxcmd.h:745-748) */
    CHECK_EQ(gbk_pkt1_hdr(SST3D_OFF_nopCMD, 1, eChipBroadcast, 0),
             (1UL << 16) | (0x120UL << 1) | 1UL);

    /* PKT1 burst for the 32-word fogTable download at 0x160
     * (REG_GROUP_LONG_BEGIN fxcmd.h:608-620: nWords<<16 | INC(bit15)) */
    CHECK_EQ(gbk_pkt1_burst_hdr(SST3D_OFF_fogTable, 32, eChipBroadcast),
             (32UL << 16) | (1UL << 15) | (0x160UL << 1) | 1UL);

    /* PKT1 to the 2D (WAX) space sets bit 14 (STORE_FIFO_WAX
     * fxcmd.h:760-780: hdr | FXBIT(14)); 2D command reg at 0x70 */
    CHECK_EQ(gbk_pkt1_hdr(SSTG_OFF_command, 1, eChipBroadcast, 1),
             (1UL << 16) | (1UL << 14) | (0x70UL << 1) | 1UL);

    /* PKT4 group at base fbzColorPath, broadcast chip field (design sec 2) */
    CHECK_EQ(gbk_pkt4_hdr(SST3D_OFF_fbzColorPath, 0x3FFF, eChipBroadcast, 0),
             (0x3FFFUL << 15) | (0x104UL << 1) | 4UL);
    /* a group writing just fbzMode+alphaMode: slots (0x10C-0x104)>>2 = 2
     * and (0x110-0x104)>>2 = 3 -> mask 0xC */
    CHECK_EQ(gbk_pkt4_hdr(SST3D_OFF_fbzColorPath, 0xC, eChipBroadcast, 0),
             (0xCUL << 15) | (0x104UL << 1) | 4UL);
    /* TMU write group uses chip field 0x2 (gglide.c:2287-2289) */
    CHECK((gbk_pkt4_hdr(SST3D_OFF_textureMode, 1, eChipTMU0, 0)
           >> kChipFieldShift & 0x7) == eChipTMU0);
    CHECK_EQ(gbk_pkt4_hdr(SST3D_OFF_textureMode, 0xB, eChipTMU0, 0),
             (0xBUL << 15) | (0x2UL << 11) | (0x300UL << 1) | 4UL);
    CHECK_EQ(gbk_pkt4_nwords(0x3FFF), 14);
    CHECK_EQ(gbk_pkt4_nwords(0x0005), 2);
    CHECK_EQ(gbk_pkt4_nwords(0), 0);

    /* PKT5 linear write, space=LFB (design sec 4:
     * "hdr1=(0<<30)|byteEn|(nWords<<3)|5; hdr2=destByteAddr&0x1FFFFFF") */
    CHECK_EQ(gbk_pkt5_hdr1(SSTCP_PKT5_LFB, 16, 0xF, 0xF),
             (0xFUL << 26) | (0xFUL << 22) | (16UL << 3) | 5UL);
    /* a full 64x64 565 mip level: 64*64*2 bytes = 2048 words, full byte
     * enables, downloaded to tram at 0x100000 (design sec 4) */
    CHECK_EQ(gbk_pkt5_hdr1(SSTCP_PKT5_LFB, 64UL * 64UL * 2UL / 4UL, 0xF, 0xF),
             (0xFUL << 26) | (0xFUL << 22) | (0x800UL << 3) | 5UL);
    CHECK_EQ(gbk_pkt5_hdr2(0x100000UL), 0x100000UL);
    CHECK_EQ(gbk_pkt5_hdr2(0x03FF0010UL), 0x03FF0010UL & 0x1FFFFFFUL);

    /* GUARD: nWords>=0x80000 (19-bit field [21:3] overflow) must NOT leak into
     * the byte-enable fields [29:22] -> FIFO desync. The M4b-2 linear-write
     * helper chunks; the encoder masks defensively. */
    CHECK_EQ(gbk_pkt5_hdr1(SSTCP_PKT5_LFB, 0x80000UL, 0, 0) & (0xFFUL << 22), 0UL);

    /* FIFO wrap packet (gsst.c:1446-1448): JMP_LOCAL(0x18) | word
     * address at bit 6 == byte offset << 4 */
    CHECK_EQ(gbk_pkt0_jmp_local(0x100000UL),
             SSTCP_PKT0_JMP_LOCAL | (0x100000UL << 4));
    CHECK_EQ(SSTCP_PKT0_JMP_LOCAL, (3UL << 3) | 0UL);

    TEST_END("test_gbk_packet");
}
