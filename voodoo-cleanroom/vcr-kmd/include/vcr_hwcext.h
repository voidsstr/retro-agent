/*
 * vcr_hwcext.h - the HWCEXT escape protocol our h5 Glide speaks to the
 * display driver (ExtEscape 0x3df3 / 0xfd3 / 0x13df3).
 *
 * A byte-exact mirror of glide3x/h5/minihwc/hwcext.h as the Win32 build
 * compiles it (no ENABLE_V3_W2K_GLIDE_CHANGES, 32-bit pointers), restated with
 * fixed-width fields so the driver, the tools and the host tests agree on
 * every offset. The contract and which ops are fatal:
 * docs/survey-glide-kernel-contract.md.
 */
#ifndef VCR_HWCEXT_H
#define VCR_HWCEXT_H

#include "vcr_types.h"

#define VCR_EXT_HWC             0x3df3
#define VCR_EXT_HWC_OLD         0x0fd3
#define VCR_EXT_HWC_WXP         0x13df3
#define VCR_EXT_HWC_CPUTYPE     0x3df4
#define VCR_EXT_HWC_CPUTYPE_WXP 0x13df4

#define VCR_HWC_PROTOCOLREV     1
#define VCR_HWC_MAX_BASEADDR    9

#define VCR_HWC_GETDRIVERVERSION   0x00
#define VCR_HWC_ALLOCCONTEXT       0x01
#define VCR_HWC_GETDEVICECONFIG    0x02
#define VCR_HWC_GETLINEARADDR      0x03
#define VCR_HWC_ALLOCFIFO          0x04
#define VCR_HWC_EXECUTEFIFO        0x05
#define VCR_HWC_QUERYCONTEXT       0x06
#define VCR_HWC_RELEASECONTEXT     0x07
#define VCR_HWC_HWCSETEXCLUSIVE    0x08
#define VCR_HWC_HWCRLSEXCLUSIVE    0x09
#define VCR_HWC_GETAGPINFO         0x0e
#define VCR_HWC_VIDTIMING          0x0f
#define VCR_HWC_FIFOINFO           0x10
#define VCR_HWC_LINEAR_MAP_OFFSET  0x11
#define VCR_HWC_DOWNLOAD_GAMMA     0x12
#define VCR_HWC_SHARE_CONTEXT_DWORD 0x15
#define VCR_HWC_UNMAP_MEMORY       0x16
#define VCR_HWC_CONTEXT_DWORD_NT   0x17
#define VCR_HWC_PCI_OP             0x18
#define VCR_HWC_GET_SLAVE_REGS     0x19
#define VCR_HWC_SLI_AA_REQUEST     0x1b

#define VCR_HWC_FIFO_FB            1
#define VCR_HWC_PCI_READ           0
#define VCR_HWC_PCI_WRITE          1
#define VCR_HWC_MAX_SLAVE_REGS     4

/* Glide reads resStatus == 1 as success */
#define VCR_HWC_OK                 1
#define VCR_HWC_FAIL               0

typedef struct { vcr_u32 dwChips, dwsliEn, dwaaEn, dwaaSampleHigh, dwsliAaAnalog,
                         dwsli_nlines, dwCfgSwapAlgorithm; } vcr_sli_chipinfo;
typedef struct { vcr_u32 dwTotalMemory, dwTileMark, dwTileCmpMark,
                         dwaaSecondaryColorBufBegin, dwaaSecondaryDepthBufBegin,
                         dwaaSecondaryDepthBufEnd, dwBpp; } vcr_sli_meminfo;
typedef struct { vcr_sli_chipinfo ChipInfo; vcr_sli_meminfo MemInfo; } vcr_sli_aa_req;

typedef struct vcr_hwc_req {
    vcr_u32 contextID;
    vcr_u32 which;
    union {
        struct { vcr_u32 protocolRev, appType; } allocContext;
        struct { vcr_u32 dc; vcr_u32 devNo; } deviceConfig;     /* dc = HDC */
        struct { vcr_u32 devNum, pHandle; } linearAddr;
        struct { vcr_u32 devNum; } exclusive;
        struct { vcr_u32 vidTiming; } vidTiming;                /* user ptr */
        struct { vcr_u32 procHandle; } unmapMemory;
        struct { vcr_u32 procId, codeSegment, dataSegment; } contextDwordNT;
        struct { vcr_u32 DeviceId, Operation, Offset, Value; } pciOp;
        struct { vcr_u32 DeviceId; } slaveReg;
        struct { vcr_u32 remapAddr; } mapInfo;
        vcr_sli_aa_req sliAA;
        vcr_u32 raw[14];
    } opt;
} vcr_hwc_req;

typedef struct vcr_hwc_res {
    vcr_i32 resStatus;
    union {
        struct { vcr_u32 major, minor; } driverVersion;
        struct { vcr_u32 contextID; } allocContext;
        struct { vcr_u32 devNum, vendorID, deviceID, fbRam, chipRev,
                         pciStride, hwStride, tileMark, isMaster, numChips; } deviceConfig;
        struct { vcr_u32 numBaseAddrs; vcr_u32 baseAddresses[VCR_HWC_MAX_BASEADDR]; } linearAddr;
        struct { vcr_u32 lAddr, pAddr, size; } agpInfo;
        struct { vcr_u32 fifoType; } fifoInfo;
        struct { vcr_u32 linOffset; } mapInfo;
        struct { vcr_u32 dwordOffset; } contextDwordNT;
        struct { vcr_u32 Value; } pciOp;
        struct { vcr_u32 Regs[VCR_HWC_MAX_SLAVE_REGS]; } slaveReg;
        vcr_u32 raw[10];
    } opt;
} vcr_hwc_res;

/* Sizes and offsets the survey measured on Glide's own structs. */
VCR_STATIC_ASSERT(hwc_req_size, sizeof(vcr_hwc_req) == 64);
VCR_STATIC_ASSERT(hwc_res_size, sizeof(vcr_hwc_res) == 44);

#endif /* VCR_HWCEXT_H */
