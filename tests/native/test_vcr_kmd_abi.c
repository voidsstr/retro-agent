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

MUNIT_MAIN("vcr-kmd HWCEXT ABI", {
    RUN(request_and_result_sizes);
    RUN(request_offsets_match_glide);
    RUN(result_offsets_match_glide);
    RUN(escape_codes_and_ioctls);
})
