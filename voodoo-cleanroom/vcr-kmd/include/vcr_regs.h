/*
 * vcr_regs.h - Banshee / Voodoo 3 / VSA-100 (Voodoo 4/5) register map used by
 * the vcr-kmd kernel driver pair.
 *
 * Offsets and bit values are the ones published in the 3dfx Glide GPL release
 * (glide3x/h5/incsrc/h3regs.h `SstIORegs`, h3defs.h, h3regs.h cfg* offsets),
 * which is the same licence our Glide fork carries. Names are ours.
 *
 * The "IO registers" are the first 256 bytes of memBase0 (BAR0). They are ALSO
 * the 256-byte I/O BAR, and the legacy VGA ports 0x3B0-0x3DF are aliased at
 * IO-register offsets 0xB0-0xDF (h3regs.h `vgaRegister[12]`).
 */
#ifndef VCR_REGS_H
#define VCR_REGS_H

/* ---- PCI identity ------------------------------------------------------- */
#define VCR_PCI_VENDOR_3DFX     0x121a
#define VCR_DEV_BANSHEE         0x0003
#define VCR_DEV_VOODOO3         0x0005
#define VCR_DEV_VSA100          0x0009   /* Voodoo 4 / 5 */
#define VCR_IS_NAPALM(did)      ((did) >= 0x0006 && (did) <= 0x000f)

#define VCR_PCI_VENDOR_BOCHS    0x1234   /* QEMU std-vga: chassis test backend */
#define VCR_DEV_BOCHS_VGA       0x1111

#define VCR_PCI_VENDOR_HINT     0x3388   /* V5 6000 PCI-PCI bridge */
#define VCR_DEV_HINT_HB1        0x0021

/* ---- memBase0 layout (Glide h3defs.h / minihwc) ------------------------- */
#define VCR_MB0_IOREGS          0x0000000
#define VCR_MB0_CMDAGP          0x0080000
#define VCR_MB0_2D              0x0100000
#define VCR_MB0_3D              0x0200000
#define VCR_MB0_TEX             0x0600000
#define VCR_MB0_LFB3D           0x1000000
#define VCR_MB0_SIZE            0x2000000   /* 32 MB, the whole BAR0 */

/* ---- IO registers (byte offsets into memBase0 / the I/O BAR) ------------ */
#define VCR_R_STATUS                    0x00
#define VCR_R_PCIINIT0                  0x04
#define VCR_R_SIPMONITOR                0x08
#define VCR_R_LFBMEMORYCONFIG           0x0c
#define VCR_R_MISCINIT0                 0x10
#define VCR_R_MISCINIT1                 0x14
#define VCR_R_DRAMINIT0                 0x18
#define VCR_R_DRAMINIT1                 0x1c
#define VCR_R_AGPINIT                   0x20
#define VCR_R_TMUGBEINIT                0x24
#define VCR_R_VGAINIT0                  0x28
#define VCR_R_VGAINIT1                  0x2c
#define VCR_R_DRAMCOMMAND               0x30
#define VCR_R_DRAMDATA                  0x34
#define VCR_R_STRAPINFO                 0x38
#define VCR_R_VIDTVOUTBLANKVCOUNT       0x3c
#define VCR_R_PLLCTRL0                  0x40   /* video (pixel) clock */
#define VCR_R_PLLCTRL1                  0x44   /* graphics core clock */
#define VCR_R_PLLCTRL2                  0x48   /* memory clock */
#define VCR_R_DACMODE                   0x4c
#define VCR_R_DACADDR                   0x50
#define VCR_R_DACDATA                   0x54
#define VCR_R_VIDMAXRGBDELTA            0x58
#define VCR_R_VIDPROCCFG                0x5c
#define VCR_R_HWCURPATADDR              0x60
#define VCR_R_HWCURLOC                  0x64
#define VCR_R_HWCURC0                   0x68
#define VCR_R_HWCURC1                   0x6c
#define VCR_R_VIDINFORMAT               0x70
#define VCR_R_VIDTVOUTBLANKHCOUNT       0x74
#define VCR_R_VIDSERIALPARALLELPORT     0x78
#define VCR_R_VIDPIXELBUFTHOLD          0x88
#define VCR_R_VIDCHROMAMIN              0x8c
#define VCR_R_VIDCHROMAMAX              0x90
#define VCR_R_VIDCURRENTLINE            0x94
#define VCR_R_VIDSCREENSIZE             0x98
#define VCR_R_VIDOVERLAYSTARTCOORDS     0x9c
#define VCR_R_VIDOVERLAYENDSCREENCOORD  0xa0
#define VCR_R_VIDOVERLAYDUDX            0xa4
#define VCR_R_VIDOVERLAYDUDXOFFSETSRCWIDTH 0xa8
#define VCR_R_VIDOVERLAYDVDY            0xac
#define VCR_R_VGA_BASE                  0xb0   /* 0xb0..0xdf = VGA 0x3b0..0x3df */
#define VCR_R_VIDOVERLAYDVDYOFFSET      0xe0
#define VCR_R_VIDDESKTOPSTARTADDR       0xe4
#define VCR_R_VIDDESKTOPOVERLAYSTRIDE   0xe8
#define VCR_R_VIDINADDR0                0xec
#define VCR_R_VIDCURROVERLAYSTARTADDR   0xfc
#define VCR_IOREGS_SIZE                 0x100

/* VGA port -> IO-register offset (port 0x3c0 -> 0xc0) */
#define VCR_VGA(port)                   ((port) - 0x300)
#define VCR_VGA_ATTR_W      0x3c0
#define VCR_VGA_ATTR_R      0x3c1
#define VCR_VGA_MISC_W      0x3c2
#define VCR_VGA_WAKEUP      0x3c3
#define VCR_VGA_SEQ_I       0x3c4
#define VCR_VGA_SEQ_D       0x3c5
#define VCR_VGA_DAC_MASK    0x3c6
#define VCR_VGA_DAC_RI      0x3c7
#define VCR_VGA_DAC_WI      0x3c8
#define VCR_VGA_DAC_D       0x3c9
#define VCR_VGA_MISC_R      0x3cc
#define VCR_VGA_GFX_I       0x3ce
#define VCR_VGA_GFX_D       0x3cf
#define VCR_VGA_CRTC_I      0x3d4
#define VCR_VGA_CRTC_D      0x3d5
#define VCR_VGA_IS1_R       0x3da

/* ---- 2D / 3D / CMD registers we touch -------------------------------------- */
#define VCR_2D_STATUS       (VCR_MB0_2D + 0x00)
#define VCR_3D_STATUS       (VCR_MB0_3D + 0x00)
#define VCR_3D_SLICTRL      (VCR_MB0_3D + 0x20c)
#define VCR_3D_AACTRL       (VCR_MB0_3D + 0x210)
#define VCR_CMD_BASEADDR0   (VCR_MB0_CMDAGP + 0x20)
#define VCR_CMD_BASESIZE0   (VCR_MB0_CMDAGP + 0x24)

/* status */
#define VCR_STATUS_FIFOLEVEL_MASK   0x1f
#define VCR_STATUS_BUSY             (1u << 9)

/* ---- bits ------------------------------------------------------------------ */
/* vidProcCfg */
#define VCR_VPC_VIDEO_PROCESSOR_EN  (1u << 0)
#define VCR_VPC_CURSOR_X11          (1u << 1)
#define VCR_VPC_INTERLACED_EN       (1u << 3)
#define VCR_VPC_HALF_MODE           (1u << 4)
#define VCR_VPC_DESKTOP_EN          (1u << 7)
#define VCR_VPC_OVERLAY_EN          (1u << 8)
#define VCR_VPC_DESKTOP_CLUT_BYPASS (1u << 10)
#define VCR_VPC_OVERLAY_CLUT_BYPASS (1u << 11)
#define VCR_VPC_DESKTOP_CLUT_SELECT (1u << 12)
#define VCR_VPC_OVERLAY_CLUT_SELECT (1u << 13)
#define VCR_VPC_DESKTOP_FMT_SHIFT   18
#define VCR_VPC_DESKTOP_FMT_MASK    (7u << 18)
#define   VCR_VPC_FMT_PAL8          0u
#define   VCR_VPC_FMT_RGB565        1u
#define   VCR_VPC_FMT_RGB24         2u
#define   VCR_VPC_FMT_RGB32         3u
#define VCR_VPC_DESKTOP_TILED_EN    (1u << 24)
#define VCR_VPC_OVERLAY_TILED_EN    (1u << 25)
#define VCR_VPC_2X_MODE_EN          (1u << 26)
#define VCR_VPC_CURSOR_EN           (1u << 27)

/* dacMode */
#define VCR_DAC_MODE_2X             (1u << 0)
#define VCR_DAC_DPMS_ON_VSYNC       (1u << 1)
#define VCR_DAC_FORCE_VSYNC         (1u << 2)
#define VCR_DAC_DPMS_ON_HSYNC       (1u << 3)
#define VCR_DAC_FORCE_HSYNC         (1u << 4)

/* pllCtrlN: f = 14.31818 MHz * (N+2) / ((M+2) * 2^K) */
#define VCR_PLL_K_SHIFT             0
#define VCR_PLL_M_SHIFT             2
#define VCR_PLL_N_SHIFT             8
#define VCR_PLL(n, m, k)            (((n) << 8) | ((m) << 2) | (k))
#define VCR_PLL_REF_KHZ             14318

/* miscInit0 (h3defs.h) - resets */
#define VCR_MI0_GRX_RESET           (1u << 0)
#define VCR_MI0_FBI_FIFO_RESET      (1u << 1)
#define VCR_MI0_VIDEO_RESET         (1u << 4)
#define VCR_MI0_2D_RESET            (1u << 5)

/* miscInit1 */
#define VCR_MI1_CLUT_INVERT         (1u << 0)
#define VCR_MI1_POWERDOWN_DAC       (1u << 8)
#define VCR_MI1_DISABLE_2D_BLOCK_WRITE (1u << 15)
#define VCR_MI1_CMDSTREAM_RESET     (1u << 19)

/* dramInit0 / dramInit1 (memory sizing, tdfxfb do_lfb_size) */
#define VCR_DI0_SGRAM_NUM_CHIPSETS  (1u << 26)
#define VCR_DI0_SGRAM_TYPE_SHIFT    27
#define VCR_DI0_H4_SGRAM_TYPE       (1u << 27)
#define VCR_DI0_H5_SGRAM_TYPE_MASK  (7u << 27)
#define VCR_DI1_DRAM_REFRESH_EN     (1u << 0)
#define VCR_DI1_MCTL_TYPE_SDRAM     (1u << 30)

/* vgaInit0 */
#define VCR_VGA0_DISABLE            (1u << 0)
#define VCR_VGA0_EXTERNAL_TIMING    (1u << 1)
#define VCR_VGA0_8BIT_DAC           (1u << 2)
#define VCR_VGA0_EXTENSIONS         (1u << 6)
#define VCR_VGA0_WAKEUP_3C3         (1u << 8)
#define VCR_VGA0_LEGACY_DECODE      (1u << 9)   /* 1 = VGA decode OFF */
#define VCR_VGA0_ALT_READBACK       (1u << 10)
#define VCR_VGA0_EXTSHIFTOUT        (1u << 12)

/* ---- PCI config space (h3regs.h cfg*) --------------------------------------- */
#define VCR_CFG_INITENABLE          0x40   /* cfgInitEnable_FabID */
#define VCR_CFG_PCIDECODE           0x48
/* cfgPciDecode fields (h3defs.h SST_PCI_*_DECODE): 0=128MB 1=256MB 2=512MB
 * 3=1GB 4=64MB 5=32MB 6=16MB 7=8MB 8=4MB; ioBase0 bits 9:8, 0 = 256 bytes */
#define VCR_PCIDEC_MB0_MASK         0xfu
#define VCR_PCIDEC_MB1_SHIFT        4
#define VCR_PCIDEC_MB1_MASK         (0xfu << 4)
#define VCR_PCIDEC_IO_MASK          (0x3u << 8)
#define VCR_PCIDEC_32MB             5u
#define VCR_PCIDEC_64MB             4u
#define VCR_CFG_VIDEOCTRL0          0x80
#define VCR_CFG_VIDEOCTRL1          0x84
#define VCR_CFG_VIDEOCTRL2          0x88
#define VCR_CFG_SLILFBCTRL          0x8c
#define VCR_CFG_AADEPTHBUFAPERTURE  0x90
#define VCR_CFG_AALFBCTRL           0x94
#define VCR_CFG_SLIAAMISC           0xac

#endif /* VCR_REGS_H */
