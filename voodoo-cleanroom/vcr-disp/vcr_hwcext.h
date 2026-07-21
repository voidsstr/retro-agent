/*
 * retro3dfx_hwcext.h - the HWCEXT escape contract between retro3dfx-glide and
 * retro3dfx-disp.
 *
 * This mirrors the escape opcodes + request/response structs that our Glide
 * (retro3dfx-glide, minihwc/hwcext.h) SENDS via ExtEscape. retro3dfx-disp is
 * the SERVER: its DrvEscape answers exactly these. Because we own both sides,
 * this header is the single source of truth for the ABI.
 *
 * The installed retail 2001 3dfx driver on .124 answers NONE of these escapes
 * (verified on-hardware) -> our Glide's hwcInit finds no board. retro3dfx-disp
 * closes that gap while cooperating with Windows (unlike a direct PCI grab).
 */
#ifndef RETRO3DFX_HWCEXT_H
#define RETRO3DFX_HWCEXT_H

/* ExtEscape nEscape codes our Glide tries, in order. We register EXT_HWC. */
#define R3DFX_EXT_HWC          0x3df3
#define R3DFX_EXT_HWC_OLD      0x0fd3
#define R3DFX_EXT_HWC_WXP      0x13df3

#define R3DFX_HWCEXT_PROTOCOLREV     0x1
#define R3DFX_HWCEXT_MAX_BASEADDR    0x9

/* request opcodes (hwcExtRequest_t.which) */
#define R3DFX_HWCEXT_GETDRIVERVERSION 0x0
#define R3DFX_HWCEXT_ALLOCCONTEXT     0x1
#define R3DFX_HWCEXT_GETDEVICECONFIG  0x2   /* "are you a 3dfx Glide device?" */
#define R3DFX_HWCEXT_GETLINEARADDR    0x3   /* map + return the card's BARs   */
#define R3DFX_HWCEXT_ALLOCFIFO        0x4
#define R3DFX_HWCEXT_EXECUTEFIFO      0x5
#define R3DFX_HWCEXT_QUERYCONTEXT     0x6
#define R3DFX_HWCEXT_RELEASECONTEXT   0x7
#define R3DFX_HWCEXT_HWCSETEXCLUSIVE  0x8
#define R3DFX_HWCEXT_HWCRLSEXCLUSIVE  0x9
#define R3DFX_HWCEXT_LINEAR_MAP_OFFSET 0x11
#define R3DFX_HWCEXT_RESTORE_DESKTOP   0x13

/* 3dfx PCI IDs we serve */
#define R3DFX_PCI_VENDOR   0x121a
#define R3DFX_DEV_VOODOO3  0x0005
#define R3DFX_DEV_VOODOO45 0x0009   /* VSA-100 */
#define R3DFX_DEV_BANSHEE  0x0003

typedef unsigned long  R3DFX_U32;

/* --- request/response payloads (must match retro3dfx-glide hwcext.h) ------- */
typedef struct { R3DFX_U32 which; R3DFX_U32 devNo; void *dc; } r3dfx_req_hdr;

typedef struct { R3DFX_U32 major, minor; } r3dfx_res_driverversion;

typedef struct { R3DFX_U32 protocolRev, appType; } r3dfx_req_alloccontext;
typedef struct { R3DFX_U32 contextID; } r3dfx_res_alloccontext;

typedef struct { R3DFX_U32 devNum, vendorID, deviceID, fbRam, chipRev;
                 R3DFX_U32 pciStride, hwStride, tileMark; } r3dfx_res_deviceconfig;

typedef struct { R3DFX_U32 devNum, pHandle; } r3dfx_req_linearaddr;
typedef struct { R3DFX_U32 numBaseAddrs;
                 R3DFX_U32 baseAddresses[R3DFX_HWCEXT_MAX_BASEADDR];
               } r3dfx_res_linearaddr;

/* Glide reads resStatus==1 as success on every escape. */
#define R3DFX_STATUS_OK   1
#define R3DFX_STATUS_FAIL 0

#endif /* RETRO3DFX_HWCEXT_H */
