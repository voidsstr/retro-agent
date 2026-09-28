/* test_vcr_kmd_abi.c
 *
 * The HWCEXT escape ABI between our h5 Glide and the vcr-kmd display driver
 * (voodoo-cleanroom/vcr-kmd/include/vcr_hwcext.h). Glide's structs have no
 * fixed-width fields and are compiled 32-bit on the target; ours are restated
 * with vcr_u32. Every offset below was measured on Glide's own structs
 * (docs/survey-glide-kernel-contract.md). A field that drifts by four bytes
 * is a GETDEVICECONFIG that reports zero chips, or a PCI_OP that writes the
 * wrong register - on silicon that answers every mistake with a hang.
 */
#include <stddef.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_hwcext.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_ioctl.h"

#define OFF(type, member) ((unsigned)offsetof(type, member))

TEST(request_and_result_sizes) {
    CHECK_EQ_U(sizeof(vcr_hwc_req), 64);
    CHECK_EQ_U(sizeof(vcr_hwc_res), 44);
}

TEST(request_offsets_match_glide) {
    CHECK_EQ_U(OFF(vcr_hwc_req, contextID), 0);
    CHECK_EQ_U(OFF(vcr_hwc_req, which), 4);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.deviceConfig.dc), 8);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.deviceConfig.devNo), 12);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.linearAddr.pHandle), 12);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.pciOp.DeviceId), 8);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.pciOp.Operation), 12);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.pciOp.Offset), 16);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.pciOp.Value), 20);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.contextDwordNT.dataSegment), 16);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.sliAA.ChipInfo.dwChips), 8);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.sliAA.ChipInfo.dwsli_nlines), 28);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.sliAA.ChipInfo.dwCfgSwapAlgorithm), 32);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.sliAA.MemInfo.dwTotalMemory), 36);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.sliAA.MemInfo.dwaaSecondaryDepthBufEnd), 56);
    CHECK_EQ_U(OFF(vcr_hwc_req, opt.sliAA.MemInfo.dwBpp), 60);
}

TEST(result_offsets_match_glide) {
    CHECK_EQ_U(OFF(vcr_hwc_res, resStatus), 0);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.devNum), 4);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.vendorID), 8);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.deviceID), 12);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.fbRam), 16);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.chipRev), 20);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.tileMark), 32);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.isMaster), 36);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.deviceConfig.numChips), 40);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.linearAddr.numBaseAddrs), 4);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.linearAddr.baseAddresses[0]), 8);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.linearAddr.baseAddresses[1]), 12);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.slaveReg.Regs[3]), 16);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.pciOp.Value), 4);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.contextDwordNT.dwordOffset), 4);
    CHECK_EQ_U(OFF(vcr_hwc_res, opt.agpInfo.size), 12);
}

TEST(escape_codes_and_ioctls) {
    /* Glide probes 0x3df3, 0xfd3, 0x13df3 in that order (minihwc.c) */
    CHECK_EQ_U(VCR_EXT_HWC, 0x3df3);
    CHECK_EQ_U(VCR_EXT_HWC_OLD, 0xfd3);
    CHECK_EQ_U(VCR_EXT_HWC_WXP, 0x13df3);
    /* our escapes stay above the range Microsoft reserves */
    CHECK(VCR_ESC_INFO > 0x10000u, "private escapes above 0x10000");
    /* CTL_CODE(FILE_DEVICE_VIDEO=0x23, fn, METHOD_BUFFERED, FILE_ANY_ACCESS) */
    CHECK_EQ_U(IOCTL_VCR_INFO, (0x23u << 16) | (0xa00u << 2));
    /* vendor-defined function codes start at 0x800 */
    CHECK(((IOCTL_VCR_INFO >> 2) & 0xfff) >= 0x800, "vendor function range");
}

/* VCR_ESC_2D_STATS grew four counters on 2026-09-27 (pattern fills, lines);
 * a gdilab built before asks for the old size and the display driver answers
 * exactly that much (vcrdd_escape.c), so the old layout must stay a prefix */
TEST(the_2d_stats_struct_only_ever_grows_at_the_end) {
    CHECK_EQ_U(VCR_2DS_SIZE_V1, 92);
    CHECK_EQ_U(OFF(vcr_2d_stats, pat_fills), VCR_2DS_SIZE_V1);
    CHECK_EQ_U(OFF(vcr_2d_stats, text_punt_why[0]), 52);
    CHECK_EQ_U(OFF(vcr_2d_stats, line_punts), VCR_2DS_SIZE_V1 + 12);
    CHECK_EQ_U(sizeof(vcr_2d_stats), VCR_2DS_SIZE_V1 + 16);
    /* the flags: new bits, the old ones where they were */
    CHECK_EQ_U(VCR_2DS_F_ENGINE, 0x1);
    CHECK_EQ_U(VCR_2DS_F_TEXT, 0x2);
    CHECK_EQ_U(VCR_2DS_F_PAT, 0x4);
    CHECK_EQ_U(VCR_2DS_F_LINE, 0x8);
    CHECK_EQ_U(VCR_INFO_F_TEXT2D, 0x40);
    CHECK_EQ_U(VCR_INFO_F_PAT2D, 0x80);
    CHECK_EQ_U(VCR_INFO_F_LINE2D, 0x100);
}

/* 2026-09-28: the read-only CLUT kind (include/vcr_clutread.h). A new kind
 * and a new positive flag - vcr_reg_op itself must not move, or an old vcrctl
 * talking to a new miniport (or the reverse) reads the wrong fields. */
TEST(the_clut_read_kind_leaves_the_reg_op_as_it_was) {
    CHECK_EQ_U(sizeof(vcr_reg_op), 24);
    CHECK_EQ_U(OFF(vcr_reg_op, value), 12);
    CHECK_EQ_U(OFF(vcr_reg_op, vga_index), 16);
    CHECK_EQ_U(OFF(vcr_reg_op, kind), 20);
    CHECK_EQ_U(VCR_REG_VGA_PORT, 5);
    CHECK_EQ_U(VCR_REG_CLUT, 6);
    /* 0x200 on its own branch; renumbered at the integration (2026-09-28):
     * D3DBigTex holds 0x200-0x800 and DdHeapFloor 0x1000 */
    CHECK_EQ_U(VCR_INFO_F_CLUT_READ, 0x2000);
    CHECK((VCR_INFO_F_CLUT_READ & (VCR_INFO_F_ALLOW_POKE | VCR_INFO_F_NO_ACCEL2D | VCR_INFO_F_NO_D3D |
                                   VCR_INFO_F_NO_TEXPORT | VCR_INFO_F_D3D32 | VCR_INFO_F_RESET3D |
                                   VCR_INFO_F_TEXT2D | VCR_INFO_F_PAT2D | VCR_INFO_F_LINE2D)) == 0,
          "a bit of its own");
}

/* 2026-09-28 integration of three branches that each took 0x200 for their own
 * switch (D3DBigTex, DdHeapFloor, ClutRead). vcr_info.flags crosses the
 * miniport -> display driver -> vcrctl boundary: two switches on one bit would
 * arm one of them whenever the other is set - DdHeapFloor = 1 would have
 * turned on 2048 textures. The whole map is pinned, and every flag must own
 * exactly one bit no other flag has. */
TEST(every_vcr_info_flag_owns_one_bit) {
    static const unsigned f[] = {
        VCR_INFO_F_ALLOW_POKE, VCR_INFO_F_NO_ACCEL2D, VCR_INFO_F_NO_D3D, VCR_INFO_F_NO_TEXPORT,
        VCR_INFO_F_D3D32, VCR_INFO_F_RESET3D, VCR_INFO_F_TEXT2D, VCR_INFO_F_PAT2D,
        VCR_INFO_F_LINE2D, VCR_INFO_F_BIGTEX, VCR_INFO_F_TEXDXT, VCR_INFO_F_TEX32,
        VCR_INFO_F_DDHEAPFLOOR, VCR_INFO_F_CLUT_READ, VCR_INFO_F_NO_GDIGAMMA,
    };
    unsigned i, seen = 0;
    CHECK_EQ_U(VCR_INFO_F_BIGTEX, 0x200);
    CHECK_EQ_U(VCR_INFO_F_TEXDXT, 0x400);
    CHECK_EQ_U(VCR_INFO_F_TEX32, 0x800);
    CHECK_EQ_U(VCR_INFO_F_DDHEAPFLOOR, 0x1000);
    CHECK_EQ_U(VCR_INFO_F_CLUT_READ, 0x2000);
    CHECK_EQ_U(VCR_INFO_F_NO_GDIGAMMA, 0x4000);
    for (i = 0; i < sizeof f / sizeof f[0]; i++) {
        CHECK(f[i] != 0 && (f[i] & (f[i] - 1)) == 0, "a flag is one bit");
        CHECK((seen & f[i]) == 0, "no two flags share a bit");
        seen |= f[i];
    }
    CHECK_EQ_U(seen, 0x7fffu);
}

MUNIT_MAIN("vcr-kmd HWCEXT ABI", {
    RUN(request_and_result_sizes);
    RUN(request_offsets_match_glide);
    RUN(result_offsets_match_glide);
    RUN(escape_codes_and_ioctls);
    RUN(the_2d_stats_struct_only_ever_grows_at_the_end);
    RUN(the_clut_read_kind_leaves_the_reg_op_as_it_was);
    RUN(every_vcr_info_flag_owns_one_bit);
})
