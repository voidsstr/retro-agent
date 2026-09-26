/*
 * vcr_probe.h - the vcrprobe.sys interface (probe/vcrprobe.c), shared with
 * vcrctl. \\.\VcrProbe, METHOD_BUFFERED, FILE_ANY_ACCESS.
 */
#ifndef VCR_PROBE_H
#define VCR_PROBE_H

#include "vcr_types.h"

#define VCRPROBE_CTL(fn)        ((0x22u << 16) | ((fn) << 2))   /* FILE_DEVICE_UNKNOWN */
#define IOCTL_VCRPROBE_VGA      VCRPROBE_CTL(0x900)
#define IOCTL_VCRPROBE_PCI      VCRPROBE_CTL(0x901)
#define IOCTL_VCRPROBE_MEM      VCRPROBE_CTL(0x902)

typedef struct vcr_probe_vga {
    vcr_u8 misc;
    vcr_u8 is1;
    vcr_u8 seq[8];
    vcr_u8 crtc[0x40];
    vcr_u8 gfx[9];
    vcr_u8 attr[0x15];
} vcr_probe_vga;

typedef struct vcr_probe_pci {
    vcr_u32 bus, dev, fn, offset, len, got;
    vcr_u8  data[256];
} vcr_probe_pci;

typedef struct vcr_probe_mem {
    vcr_u32 phys;               /* dword aligned, >= 1 MB */
    vcr_u32 len;                /* <= 4096, multiple of 4 */
    vcr_u8  data[4096];
} vcr_probe_mem;

#endif /* VCR_PROBE_H */
