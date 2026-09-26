/*
 * vcr_ioctl.h - the private contract of the vcr-kmd driver pair:
 *   miniport <-> display DLL : IOCTL_VCR_* (EngDeviceIoControl)
 *   user tools -> display DLL: VCR_ESC_* (ExtEscape), forwarded to the IOCTLs
 *
 * Escape codes sit above 0x10000 (Microsoft reserves 0..0x10000; the Glide
 * fork's hwcext.h records XP refusing low third-party codes). Structures are
 * fixed-width and identical for the kernel and for 32-bit user tools.
 */
#ifndef VCR_IOCTL_H
#define VCR_IOCTL_H

#include "vcr_types.h"
#include "vcr_log.h"

#define VCR_KMD_VERSION_MAJOR   0
#define VCR_KMD_VERSION_MINOR   1
#define VCR_KMD_VERSION_NUM     ((VCR_KMD_VERSION_MAJOR << 16) | VCR_KMD_VERSION_MINOR)
#ifndef VCR_KMD_BUILD
#define VCR_KMD_BUILD           0
#endif

/* CTL_CODE(FILE_DEVICE_VIDEO, fn, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define VCR_CTL(fn)             ((0x23u << 16) | ((fn) << 2))
#define IOCTL_VCR_INFO          VCR_CTL(0xa00)
#define IOCTL_VCR_LOG_WRITE     VCR_CTL(0xa01)
#define IOCTL_VCR_LOG_READ      VCR_CTL(0xa02)
#define IOCTL_VCR_MAP_GLIDE     VCR_CTL(0xa03)
#define IOCTL_VCR_UNMAP_GLIDE   VCR_CTL(0xa04)
#define IOCTL_VCR_PCI_OP        VCR_CTL(0xa05)
#define IOCTL_VCR_REG           VCR_CTL(0xa06)
#define IOCTL_VCR_SLI           VCR_CTL(0xa07)
#define IOCTL_VCR_BOOT_OK       VCR_CTL(0xa08)
#define IOCTL_VCR_SNAPSHOT      VCR_CTL(0xa09)
#define IOCTL_VCR_CTX_DWORD     VCR_CTL(0xa0a)
#define IOCTL_VCR_RESTORE_MODE  VCR_CTL(0xa0b)
#define IOCTL_VCR_RESET_ENGINE  VCR_CTL(0xa0c)
#define IOCTL_VCR_DDFLIP        VCR_CTL(0xa0d)
#define IOCTL_VCR_VBLANK        VCR_CTL(0xa0e)

#define VCR_ESC_BASE            0x56430000u     /* 'VC' */
#define VCR_ESC_INFO            (VCR_ESC_BASE + 1)
#define VCR_ESC_LOG_READ        (VCR_ESC_BASE + 2)
#define VCR_ESC_LOG_MARK        (VCR_ESC_BASE + 3)
#define VCR_ESC_REG             (VCR_ESC_BASE + 4)
#define VCR_ESC_PCI             (VCR_ESC_BASE + 5)
#define VCR_ESC_SNAPSHOT        (VCR_ESC_BASE + 6)
#define VCR_ESC_BOOT_OK         (VCR_ESC_BASE + 7)
#define VCR_ESC_DD_STATS        (VCR_ESC_BASE + 8)
#define VCR_ESC_RESET_ENGINE    (VCR_ESC_BASE + 9)

/* backends */
#define VCR_HW_NONE             0
#define VCR_HW_VOODOO           1   /* Banshee / Voodoo 3 / VSA-100 */
#define VCR_HW_BOCHS            2   /* QEMU std-vga, for chassis tests only */

#define VCR_MAX_CHIPS           4

/* IOCTL_VCR_INFO / VCR_ESC_INFO: what the miniport found. */
typedef struct vcr_info {
    vcr_u32 size;               /* sizeof(vcr_info) */
    vcr_u32 version;            /* VCR_KMD_VERSION_NUM */
    vcr_u32 build;
    vcr_u32 backend;            /* VCR_HW_* */
    vcr_u32 vendor, device, subsys, revision;
    vcr_u32 bus, slot;          /* slot = PCI_SLOT_NUMBER.u.AsULONG */
    vcr_u32 nchips;
    vcr_u32 chip_slot[VCR_MAX_CHIPS];
    vcr_u32 mmio_phys[VCR_MAX_CHIPS];
    vcr_u32 lfb_phys, lfb_len;  /* BAR1 of the master */
    vcr_u32 mmio_len;
    vcr_u32 io_base, io_len;
    vcr_u32 fb_per_chip;        /* bytes */
    vcr_u32 desktop_offset;     /* desktop start in video memory */
    vcr_u32 cur_mode;           /* index into the mode list, ~0 = none */
    vcr_u32 cur_w, cur_h, cur_bpp, cur_hz, cur_stride;
    vcr_u32 nmodes;
    vcr_u32 boot_attempts;
    vcr_u32 boot_good;          /* 1 once the stable-boot timer fired */
    vcr_u32 exclusive_pid;      /* Glide owner, 0 = none */
    vcr_u32 sli_active;
    vcr_u32 log_next_seq;
    vcr_u32 flags;              /* VCR_INFO_F_* */
    vcr_u32 ogl_version, ogl_driver_version;
    vcr_u16 ogl_name[32];       /* OpenGLDrivers key name (UTF-16) */
    /* multi-chip (appended - older tools read a shorter struct) */
    vcr_u32 glide_chips;        /* what GETDEVICECONFIG tells Glide: 1, 2 or 4 */
    vcr_u32 sli_chips;          /* chips in the live SLI/AA session, 0 = none */
    vcr_u32 sli_result;         /* last vcr_sli_set() result (int) */
    vcr_u32 clock_6k_hz;        /* last V5 6000 external clock programmed, 0 = never */
    vcr_u32 slave_bar0[VCR_MAX_CHIPS];
    /* the monitor (appended) */
    vcr_u32 edid_ok;            /* a valid EDID came over DDC */
    vcr_u32 mon_filter;         /* the mode list honours its range limits */
    vcr_u32 mon_hmin_khz, mon_hmax_khz, mon_vmin_hz, mon_vmax_hz, mon_max_pixclk_khz;
    char    mon_pnp[4];         /* "SNY" */
    vcr_u32 mon_product;
    char    mon_name[16];
    vcr_u8  edid[128];          /* the block as read */
} vcr_info;
#define VCR_INFO_F_ALLOW_POKE   0x1
#define VCR_INFO_F_NO_D3D       0x4     /* Diag\\D3D = 0: no Direct3D HAL */
#define VCR_INFO_F_NO_ACCEL2D   0x2     /* Diag\\Accel2D = 0: the display driver draws in software */

/* IOCTL_VCR_LOG_WRITE */
typedef struct vcr_log_write_req {
    vcr_u32 code, level, src, pid, a, b, c, d;
    char    msg[VCR_LOG_MSG_LEN];
} vcr_log_write_req;

/* IOCTL_VCR_LOG_READ: in {after_seq, max}, out {hdr, count, last_seq, e[]} */
typedef struct vcr_log_read_req {
    vcr_u32 after_seq;
    vcr_u32 max;
} vcr_log_read_req;
typedef struct vcr_log_read_res {
    vcr_log_header hdr;
    vcr_u32 count;
    vcr_u32 last_seq;
    vcr_log_entry e[1];         /* count of them */
} vcr_log_read_res;
#define VCR_LOG_READ_RES_BYTES(n) (sizeof(vcr_log_header) + 8 + (n) * sizeof(vcr_log_entry))

/* IOCTL_VCR_MAP_GLIDE: map the BARs into the CALLING process. Every address is
 * the start of its own view (Glide validates AllocationBase == address). */
typedef struct vcr_glide_map {
    vcr_u32 status;             /* 0 = ok, else an NTSTATUS-ish VP error */
    vcr_u32 base0;              /* memBase0, the whole 32 MB */
    vcr_u32 base1;              /* memBase1, the LFB */
    vcr_u32 base1_len;
    vcr_u32 nchips;
    vcr_u32 slave[VCR_MAX_CHIPS][4];    /* [chip][IO, CMD, 2D, 3D]; chip 0 unused */
} vcr_glide_map;

/* IOCTL_VCR_SLI: in = Glide's SLI_AA_REQUEST payload (vcr_sli_aa_req,
 * vcr_hwcext.h), out = this. result: < 0 refused (nothing written), 0 done,
 * > 0 done with VCR_SLI_W_* warnings (vcr_sli.h). */
typedef struct vcr_sli_res {
    vcr_u32 result;             /* int */
    vcr_u32 sli_chips;          /* chips now in SLI/AA, 0 = off */
    vcr_u32 clock_6k_hz;
    vcr_u32 reserved;
} vcr_sli_res;

/* IOCTL_VCR_DDFLIP (in): scan out from this byte offset of video memory - a
 * DirectDraw flip. The primary itself is at vcr_info.desktop_offset. */
typedef struct vcr_dd_flip {
    vcr_u32 offset;
} vcr_dd_flip;

/* IOCTL_VCR_VBLANK (out) */
typedef struct vcr_dd_vblank {
    vcr_u32 in_vblank;          /* 1 while in vertical retrace */
    vcr_u32 scanline;           /* the line being scanned, 0 when unknown */
    vcr_u32 scan_offset;        /* the offset now being scanned out */
    vcr_u32 reserved;
} vcr_dd_vblank;

/* IOCTL_VCR_PCI_OP */
#define VCR_PCI_TARGET_BRIDGE   0x10    /* the V5 6000 HiNT bridge */
typedef struct vcr_pci_op {
    vcr_u32 target;             /* 0..3 = chip, VCR_PCI_TARGET_BRIDGE */
    vcr_u32 write;
    vcr_u32 offset;
    vcr_u32 value;
    vcr_u32 size;               /* 1, 2 or 4 */
} vcr_pci_op;

/* IOCTL_VCR_REG: MMIO peek/poke on memBase0 of a chip (poke gated). */
typedef struct vcr_reg_op {
    vcr_u32 chip;
    vcr_u32 write;
    vcr_u32 offset;             /* byte offset into memBase0, dword aligned */
    vcr_u32 value;
    vcr_u32 vga_index;          /* for VCR_REG_VGA_*: the index register value */
    vcr_u32 kind;               /* VCR_REG_* */
} vcr_reg_op;
#define VCR_REG_MMIO32          0
#define VCR_REG_VGA_CRTC        1
#define VCR_REG_VGA_SEQ         2
#define VCR_REG_VGA_GFX         3
#define VCR_REG_VGA_ATTR        4
#define VCR_REG_VGA_PORT        5   /* offset = port (0x3c0..0x3df), 8 bit */

/* IOCTL_VCR_SNAPSHOT: every video register of every chip, for golden
 * comparisons against the vendor driver. */
typedef struct vcr_chip_snapshot {
    vcr_u32 ioregs[64];         /* memBase0 + 0x00 .. 0xfc */
    vcr_u8  crtc[0x40];
    vcr_u8  seq[8];
    vcr_u8  gfx[16];
    vcr_u8  attr[0x20];
    vcr_u8  misc;
    vcr_u8  pad[3];
    vcr_u32 cfg[64];            /* PCI config space dwords 0..0xfc */
} vcr_chip_snapshot;
typedef struct vcr_snapshot {
    vcr_u32 nchips;
    vcr_u32 reserved;
    vcr_chip_snapshot chip[VCR_MAX_CHIPS];
} vcr_snapshot;

/* IOCTL_VCR_CTX_DWORD: a per-process DWORD the driver can set to tell Glide
 * its context was lost. out: user address. */
typedef struct vcr_ctx_dword {
    vcr_u32 user_va;
    vcr_u32 status;
} vcr_ctx_dword;

#endif /* VCR_IOCTL_H */
