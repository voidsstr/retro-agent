/*
 * h3hw.h - self-contained Voodoo3 (Avenger/H3) register map + bit definitions
 *          for the fxD3D kernel Glide backend's pure-logic core (gbk_*).
 *
 * EXTRACTED (not vendored verbatim) from the open 3dfx-GPL Glide3 h3 tree at
 *   voodoo-cleanroom/build/retro3dfx-glide/glide3x/h3   ("H3" in cites below)
 * per docs/3dfx-gbkernel-design.md. Verbatim vendoring was rejected because:
 *   - H3/incsrc/h3regs.h expresses registers as volatile structs of FxU32,
 *     and FxU32 is only typedef'd there under #ifdef _H2INC (h3regs.h:42-44);
 *     normally it comes from glide's type system (3dfx.h) - not self-contained.
 *   - H3/incsrc/h3gdefs.h does '#include "cmddefs.h"' (h3gdefs.h:27) which
 *     does not exist anywhere in the h3 tree.
 * So this header carries ONLY what the pure-logic modules need, every
 * definition cited to its H3 source file:line. Register BYTE offsets were
 * computed by walking the volatile-struct layouts (all fields are 32-bit, no
 * padding possible at /Zp8 or on any ia32 ABI) and cross-checked against the
 * offsets enumerated in docs/3dfx-gbkernel-design.md section 0.
 *
 * License: 3dfx GLIDE GENERAL PUBLIC LICENSE (our license base, FORKS.md).
 * Compiles identically on host gcc, mingw i686-w64-mingw32-gcc, and VC6 /Zp8:
 * fixed-width types defined locally, no glide deps, no Win32, no CRT.
 */
#ifndef H3HW_H
#define H3HW_H

/* ---- fixed-width types (local shim; ia32/ia32e ILP32-LL64 assumptions) ---- */
typedef unsigned int   h3u32;
typedef signed int     h3i32;
typedef unsigned short h3u16;
typedef unsigned char  h3u8;
/* compile-time size checks - fail the build if a target lies about widths */
typedef char h3hw_assert_u32_is_4[(sizeof(h3u32) == 4) ? 1 : -1];
typedef char h3hw_assert_u16_is_2[(sizeof(h3u16) == 2) ? 1 : -1];

/* ---- bit helpers (H3/incsrc/h3defs.h:72-73) ------------------------------ */
#define H3BIT(n)     (1UL << (n))
#define H3MASK(n)    (0xFFFFFFFFUL >> (32 - (n)))

/* ---- compile-time assertion helper (array-size trick; C89, no headers) ---- */
#define H3HW_STATIC_ASSERT(name, cexpr) typedef char name[(cexpr) ? 1 : -1]
/* A nonzero UNSIGNED integer constant U negates modularly, so -(U) stays > 0;
 * a signed constant does not. Used to prove a register-bit mask that reaches
 * bit 31 is unsigned, so shifting into bit 31 is well-defined where `long` is
 * 32-bit (mingw i686-w64-mingw32 / VC6 /Zp8). This holds on the LP64 host too
 * -- there the shifted value is representable, but a signed mask still carries
 * a signed type, so this is a portable proxy for "no ILP32 signed-shift UB"
 * and fails the strict module build the moment such a mask is written signed. */
#define H3HW_MASK_IS_UNSIGNED(m)  ((-(m)) > 0)

/* ======================================================================== */
/* 0. BAR0 sub-space offsets (H3/incsrc/h3defs.h:1273-1288)                 */
/* ======================================================================== */
#define SST_IO_OFFSET           0x000000L  /* SstIORegs               :1274 */
#define SST_CMDAGP_OFFSET       0x080000L  /* SstCRegs                :1275 */
#define SST_2D_OFFSET           0x100000L  /* SstGRegs (WAX 2D)       :1276 */
#define SST_3D_OFFSET           0x200000L  /* SstRegs                 :1277 */
#define SST_3D_ALT_OFFSET       0x400000L  /*                         :1278 */
#define SST_TEX_OFFSET          0x600000L  /* (unused - unified mem)  :1285 */
#define SST_YUV_OFFSET          0xC00000L  /*                         :1288 */
#define SST_LFB_OFFSET          0x1000000L /* 3D LFB space in BAR0    :1289 */

/* ======================================================================== */
/* SstIORegs byte offsets (BAR0 + SST_IO_OFFSET)                            */
/* struct layout H3/incsrc/h3regs.h:50-115 (sstioregs, all-FxU32)           */
/* ======================================================================== */
#define SSTIO_OFF_status                 0x000
#define SSTIO_OFF_pciInit0               0x004
#define SSTIO_OFF_lfbMemoryConfig        0x00C
#define SSTIO_OFF_miscInit0              0x010
#define SSTIO_OFF_miscInit1              0x014
#define SSTIO_OFF_dramInit0              0x018
#define SSTIO_OFF_dramInit1              0x01C
#define SSTIO_OFF_vidProcCfg             0x05C
#define SSTIO_OFF_vidScreenSize          0x098
#define SSTIO_OFF_vidDesktopStartAddr    0x0E4  /* flip-present later (design 5B) */
#define SSTIO_OFF_vidDesktopOverlayStride 0x0E8

/* miscInit0: Y-origin flip (H3/incsrc/h3defs.h:902-903; minihwc.c:3528-3534) */
#define SST_YORIGIN_TOP_SHIFT           18
#define SST_YORIGIN_TOP                 (0xFFFL << SST_YORIGIN_TOP_SHIFT)

/* dramInit1: memory type (H3/incsrc/h3defs.h:987) - picks fastfill flavor */
#define SST_MCTL_TYPE_SDRAM             H3BIT(30)

/* ======================================================================== */
/* SstCRegs byte offsets (BAR0 + SST_CMDAGP_OFFSET)                         */
/* struct layout H3/incsrc/h3regs.h:117-160 (cmdfifo + sstcregs)            */
/* cmdFifo0 begins at +0x20 (after 6 AGP regs + reservedL[2])               */
/* ======================================================================== */
#define SSTC_OFF_cmdFifo0_baseAddrL      0x020
#define SSTC_OFF_cmdFifo0_baseSize       0x024
#define SSTC_OFF_cmdFifo0_bump           0x028
#define SSTC_OFF_cmdFifo0_readPtrL       0x02C
#define SSTC_OFF_cmdFifo0_readPtrH       0x030
#define SSTC_OFF_cmdFifo0_aMin           0x034
#define SSTC_OFF_cmdFifo0_aMax           0x03C  /* unusedA at 0x038 */
#define SSTC_OFF_cmdFifo0_depth          0x044  /* unusedB at 0x040 */
#define SSTC_OFF_cmdFifo0_holeCount      0x048
#define SSTC_OFF_cmdFifoThresh           0x080  /* after cmdFifo1 0x50-0x7C */
#define SSTC_OFF_cmdHoleInit             0x084

/* cmdFifo*.baseSize bits (H3/incsrc/h3gdefs.h:175-179) */
#define SST_CMDFIFO_SIZE                0xFFL
#define SST_EN_CMDFIFO                  H3BIT(8)
#define SST_CMDFIFO_AGP                 H3BIT(9)
#define SST_CMDFIFO_DISABLE_HOLES       H3BIT(10)  /* keep CLEAR: holes ON */

/* cmdFifoThresh (H3/incsrc/h3defs.h:1190-1191; hwcInitFifo minihwc.c:1613-
 * 1665 writes (0xF<<SST_HIGHWATER_SHIFT)|0x8 on Avenger - design sec 1c)   */
#define SST_HIGHWATER_SHIFT     5
#define SST_HIGHWATER           (0xFL << SST_HIGHWATER_SHIFT)
#define H3HW_CMDFIFO_THRESH_AVENGER  ((0xFUL << SST_HIGHWATER_SHIFT) | 0x8UL)

/* FIFO carve-out constants (H3/minihwc/minihwc.c) */
#define H3HW_MAXFIFOSIZE        0x40000L   /* minihwc.c:628             */
#define H3HW_MAXFIFOSIZE_16MB   0xFF000L   /* minihwc.c:629 (>8MB board)*/
#define H3HW_FIFO_RESERVE       0x2000L    /* fifoLength -= 0x2000, :1541 */
#define H3HW_PAGE               0x1000L    /* color bufs even-page, aux
                                            * odd-page aligned, :1461-1492 */
/* end-of-fifo slack for the wrap JMP packet (fxcmd.h:189: sizeof(FxU32)<<3) */
#define H3HW_FIFO_END_ADJUST    32L

/* minimum-memory check terms (grSstWinOpen, gsst.c:953-954; used :1043-1048:
 * bufSize*(nCol+nAux) + MIN_TEXTURE_STORE + MIN_FIFO_SIZE must fit in VRAM) */
#define H3HW_MIN_TEXTURE_STORE  0x200000L  /* gsst.c:953 */
#define H3HW_MIN_FIFO_SIZE      0x10000L   /* gsst.c:954 */

/* ======================================================================== */
/* SstGRegs (WAX 2D) byte offsets (BAR0 + SST_2D_OFFSET)                    */
/* struct layout H3/incsrc/h3regs.h:163-199 (sstgregs, all-FxU32)           */
/* ======================================================================== */
#define SSTG_OFF_status          0x000
#define SSTG_OFF_clip0min        0x008
#define SSTG_OFF_clip0max        0x00C
#define SSTG_OFF_dstBaseAddr     0x010
#define SSTG_OFF_dstFormat       0x014
#define SSTG_OFF_rop             0x030
#define SSTG_OFF_srcBaseAddr     0x034
#define SSTG_OFF_commandEx       0x038
#define SSTG_OFF_srcFormat       0x054
#define SSTG_OFF_srcSize         0x058
#define SSTG_OFF_srcXY           0x05C
#define SSTG_OFF_dstSize         0x068
#define SSTG_OFF_dstXY           0x06C
#define SSTG_OFF_command         0x070
#define SSTG_OFF_launch          0x080

/* SSTG command bits (H3/incsrc/h3gdefs.h:44-73) */
#define SSTG_COMMAND_SHIFT      0
#define SSTG_NOP                (0 << SSTG_COMMAND_SHIFT)
#define SSTG_BLT                (1 << SSTG_COMMAND_SHIFT)  /* scr-to-scr    */
#define SSTG_RECTFILL           (5 << SSTG_COMMAND_SHIFT)
#define SSTG_GO                 H3BIT(8)   /* initiate                      */
#define SSTG_ROP0_SHIFT         24
#define SSTG_ROP0               (0xFFUL << SSTG_ROP0_SHIFT)
/* pre-shifted SRCCOPY rop (H3/incsrc/h3gdefs.h:162) */
#define SSTG_ROP_SRCCOPY        (0xCCUL << SSTG_ROP0_SHIFT)

/* SSTG src/dstFormat bits (H3/incsrc/h3gdefs.h:86-115) */
#define SSTG_SRC_STRIDE_SHIFT   0
#define SSTG_SRC_LINEAR_STRIDE  (0x3FFFUL << SSTG_SRC_STRIDE_SHIFT)
#define SSTG_DST_STRIDE_SHIFT   0
#define SSTG_DST_LINEAR_STRIDE  (0x3FFFUL << SSTG_DST_STRIDE_SHIFT)
#define SSTG_SRC_FORMAT_SHIFT   16
#define SSTG_DST_FORMAT_SHIFT   16
#define SSTG_PIXFMT_8BPP        (0x1UL << SSTG_SRC_FORMAT_SHIFT)
#define SSTG_PIXFMT_15BPP       (0x2UL << SSTG_SRC_FORMAT_SHIFT)
#define SSTG_PIXFMT_16BPP       (0x3UL << SSTG_SRC_FORMAT_SHIFT) /* 565:
                                 * the (3<<16) in grDRIBufferSwap's
                                 * srcFormat/dstFormat, gglide.c:1216-1257  */
#define SSTG_PIXFMT_24BPP       (0x4UL << SSTG_SRC_FORMAT_SHIFT)
#define SSTG_PIXFMT_32BPP       (0x5UL << SSTG_SRC_FORMAT_SHIFT)

/* ======================================================================== */
/* SstRegs (3D) byte offsets (BAR0 + SST_3D_OFFSET)                         */
/* struct layout H3/incsrc/h3regs.h:213-375 (sstregs, all 32-bit fields)    */
/* ======================================================================== */
#define SST3D_OFF_status         0x000
#define SST3D_OFF_intrCtrl       0x004
#define SST3D_OFF_FtriangleCMD   0x100
#define SST3D_OFF_fbzColorPath   0x104
#define SST3D_OFF_fogMode        0x108
#define SST3D_OFF_alphaMode      0x10C
#define SST3D_OFF_fbzMode        0x110
#define SST3D_OFF_lfbMode        0x114
#define SST3D_OFF_clipLeftRight  0x118
#define SST3D_OFF_clipBottomTop  0x11C
#define SST3D_OFF_nopCMD         0x120
#define SST3D_OFF_fastfillCMD    0x124
#define SST3D_OFF_swapbufferCMD  0x128
#define SST3D_OFF_fogColor       0x12C
#define SST3D_OFF_zaColor        0x130
#define SST3D_OFF_chromaKey      0x134
#define SST3D_OFF_chromaRange    0x138
#define SST3D_OFF_stipple        0x140
#define SST3D_OFF_c0             0x144
#define SST3D_OFF_c1             0x148
#define SST3D_OFF_fogTable       0x160  /* FxU32[32] = 64 entries, 0x160-0x1DC */
#define SST3D_OFF_colBufferAddr    0x1EC
#define SST3D_OFF_colBufferStride  0x1F0
#define SST3D_OFF_auxBufferAddr    0x1F4
#define SST3D_OFF_auxBufferStride  0x1F8
#define SST3D_OFF_swapBufferPend 0x24C
#define SST3D_OFF_leftOverlayBuf 0x250
#define SST3D_OFF_sSetupMode     0x260  /* TSU 0x260..sBeginTriCMD 0x2A4 */
#define SST3D_OFF_sDrawTriCMD    0x2A0
#define SST3D_OFF_sBeginTriCMD   0x2A4
#define SST3D_OFF_textureMode    0x300  /* TMU block */
#define SST3D_OFF_tLOD           0x304
#define SST3D_OFF_tDetail        0x308
#define SST3D_OFF_texBaseAddr    0x30C
#define SST3D_OFF_trexInit0      0x31C
#define SST3D_OFF_trexInit1      0x320
#define SST3D_OFF_nccTable0      0x324  /* [12] 0x324-0x350 */
#define SST3D_OFF_nccTable1      0x354  /* [12] 0x354-0x380 */

/* ======================================================================== */
/* status bits (H3/incsrc/h3defs.h:97-110)                                  */
/* ======================================================================== */
#define SST_FIFOLEVEL           0x3FL           /* :97  */
#define SST_VRETRACE            H3BIT(6)        /* :100 */
#define SST_FBI_BUSY            H3BIT(7)        /* :101 */
#define SST_TMU_BUSY            H3BIT(8)        /* :102 */
#define SST_BUSY                H3BIT(9)        /* :104 - gb_finish poll    */
#define SST_GUI_BUSY            H3BIT(10)       /* :105 - 2D engine         */
#define SST_CMD0_BUSY           H3BIT(11)       /* :106 */
#define SST_SWAPBUFPENDING_SHIFT 28             /* :108 */
#define SST_SWAPBUFPENDING      (0x7L << SST_SWAPBUFPENDING_SHIFT)

/* ======================================================================== */
/* fbzMode bits (H3/incsrc/h3defs.h:297-321)                                */
/* ======================================================================== */
#define SST_ENRECTCLIP          H3BIT(0)
#define SST_ENCHROMAKEY         H3BIT(1)
#define SST_ENSTIPPLE           H3BIT(2)
#define SST_WBUFFER             H3BIT(3)
#define SST_ENDEPTHBUFFER       H3BIT(4)
#define SST_ZFUNC_SHIFT         5               /* :306; func field [7:5] */
#define SST_ZFUNC               (0x7L << SST_ZFUNC_SHIFT)
#define SST_ENDITHER            H3BIT(8)
#define SST_RGBWRMASK           H3BIT(9)        /* :309; distate.c:967-1003 */
#define SST_ZAWRMASK            H3BIT(10)       /* :310 */
#define SST_DITHER2x2           H3BIT(11)       /* :311; h3 forces 2x2      */
#define SST_ENALPHAMASK         H3BIT(13)
#define SST_ENZBIAS             H3BIT(16)
#define SST_YORIGIN             H3BIT(17)
#define SST_ENALPHABUFFER       H3BIT(18)
#define SST_ENDITHERSUBTRACT    H3BIT(19)
#define SST_ZCOMPARE_TO_ZACOLOR H3BIT(20)  /* :320; _grDepthBufferMode      */
#define SST_DEPTH_FLOAT_SEL     H3BIT(21)  /* :321; W-buffer + fog coord    */

/* ======================================================================== */
/* fbzColorPath bits (H3/incsrc/h3defs.h:323-425)                           */
/* ======================================================================== */
#define SST_RGBSELECT_SHIFT     0
#define SST_RGBSELECT           (0x3L << SST_RGBSELECT_SHIFT)
#define SST_RGBSEL_RGBA         (0L << SST_RGBSELECT_SHIFT)  /* iterated  */
#define SST_RGBSEL_TMUOUT       (1L << SST_RGBSELECT_SHIFT)
#define SST_RGBSEL_C1           (2L << SST_RGBSELECT_SHIFT)
#define SST_ASELECT_SHIFT       2
#define SST_ASELECT             (0x3L << SST_ASELECT_SHIFT)
#define SST_ASEL_RGBA           (0L << SST_ASELECT_SHIFT)
#define SST_ASEL_TMUOUT         (1L << SST_ASELECT_SHIFT)
#define SST_ASEL_C1             (2L << SST_ASELECT_SHIFT)
#define SST_LOCALSELECT_SHIFT   4          /* :339 */
#define SST_LOCALSELECT         H3BIT(4)
#define SST_ALOCALSELECT_SHIFT  5
#define SST_ALOCALSELECT        (0x3L << SST_ALOCALSELECT_SHIFT)
#define SST_ALOCAL_ITERATOR     (0L << SST_ALOCALSELECT_SHIFT)
#define SST_ALOCAL_C0           (1L << SST_ALOCALSELECT_SHIFT)
/* color-combine unit fields (:347-360) */
#define SST_CC_ZERO_OTHER       H3BIT(8)
#define SST_CC_SUB_CLOCAL       H3BIT(9)
#define SST_CC_MSELECT_SHIFT    10
#define SST_CC_MSELECT          (0x7L << SST_CC_MSELECT_SHIFT)
#define SST_CC_MONE             (0L << SST_CC_MSELECT_SHIFT)
#define SST_CC_MCLOCAL          (1L << SST_CC_MSELECT_SHIFT)
#define SST_CC_MAOTHER          (2L << SST_CC_MSELECT_SHIFT)
#define SST_CC_MALOCAL          (3L << SST_CC_MSELECT_SHIFT)
#define SST_CC_MATMU            (4L << SST_CC_MSELECT_SHIFT)
#define SST_CC_REVERSE_BLEND    H3BIT(13)
#define SST_CC_ADD_CLOCAL       H3BIT(14)
#define SST_CC_ADD_ALOCAL       H3BIT(15)
#define SST_CC_INVERT_OUTPUT    H3BIT(16)
/* alpha-combine unit fields (:363-376) */
#define SST_CCA_ZERO_OTHER      H3BIT(17)
#define SST_CCA_SUB_CLOCAL      H3BIT(18)
#define SST_CCA_MSELECT_SHIFT   19
#define SST_CCA_MSELECT         (0x7L << SST_CCA_MSELECT_SHIFT)
#define SST_CCA_MONE            (0L << SST_CCA_MSELECT_SHIFT)
#define SST_CCA_MCLOCAL         (1L << SST_CCA_MSELECT_SHIFT)
#define SST_CCA_MAOTHER         (2L << SST_CCA_MSELECT_SHIFT)
#define SST_CCA_MALOCAL         (3L << SST_CCA_MSELECT_SHIFT)
#define SST_CCA_MATMU           (4L << SST_CCA_MSELECT_SHIFT)
#define SST_CCA_REVERSE_BLEND   H3BIT(22)
#define SST_CCA_ADD_CLOCAL      H3BIT(23)
#define SST_CCA_ADD_ALOCAL      H3BIT(24)
#define SST_CCA_INVERT_OUTPUT   H3BIT(25)
#define SST_PARMADJUST          H3BIT(26)
#define SST_ENTEXTUREMAP        H3BIT(27)  /* transitions need nopCMD=0,
                                            * distate.c:910-915             */
#define SST_RGBAZ_CLAMP         H3BIT(28)
/* abstract combine modes (:390-425) */
#define SST_CC_MZERO            (SST_CC_MONE | SST_CC_REVERSE_BLEND)
#define SST_CCA_MZERO           (SST_CCA_MONE | SST_CCA_REVERSE_BLEND)
#define SST_CCOMBINE_SHIFT      8
#define SST_CCOMBINE            (0x1FFL << SST_CCOMBINE_SHIFT)
#define SST_CC_REPLACE          (SST_CC_ZERO_OTHER | SST_CC_ADD_CLOCAL)
#define SST_CC_PASS             (SST_CC_MONE)
#define SST_CC_MULT             (SST_CC_MCLOCAL | SST_CC_REVERSE_BLEND)
#define SST_CC_ZERO             (SST_CC_MZERO)
#define SST_CC_ONE              (SST_CC_MZERO | SST_CC_INVERT_OUTPUT)
#define SST_CC_BLEND            (SST_CC_SUB_CLOCAL | SST_CC_ADD_CLOCAL)
#define SST_CC_BLEND_ATEX       (SST_CC_BLEND | SST_CC_MATMU)
#define SST_CACOMBINE_SHIFT     17
#define SST_CACOMBINE           (0x1FFL << SST_CACOMBINE_SHIFT)
#define SST_CCA_REPLACE         (SST_CCA_ZERO_OTHER | SST_CCA_ADD_CLOCAL)
#define SST_CCA_PASS            (SST_CCA_MONE)
#define SST_CCA_MULT            (SST_CCA_MCLOCAL | SST_CCA_REVERSE_BLEND)
#define SST_CCA_ZERO            (SST_CCA_MZERO)
#define SST_CCA_ONE             (SST_CCA_MZERO | SST_CCA_INVERT_OUTPUT)

/* ======================================================================== */
/* fogMode bits (H3/incsrc/h3defs.h:423-431)                                */
/* ======================================================================== */
#define SST_ENFOGGING           H3BIT(0)
#define SST_FOGADD              H3BIT(1)
#define SST_FOGMULT             H3BIT(2)
#define SST_FOG_ALPHA           H3BIT(3)
#define SST_FOG_Z               H3BIT(4)
#define SST_FOG_CONSTANT        H3BIT(5)
#define SST_FOG_DITHER          H3BIT(6)   /* _grFogMode adds, gglide.c:2012+ */
#define SST_FOG_ZONES           H3BIT(7)

/* ======================================================================== */
/* alphaMode bits (H3/incsrc/h3defs.h:433-465)                              */
/* ======================================================================== */
#define SST_ENALPHAFUNC         H3BIT(0)
#define SST_ALPHAFNC_SHIFT      1          /* func field [3:1] */
#define SST_ALPHAFNC            (0x7L << SST_ALPHAFNC_SHIFT)
#define SST_ENALPHABLEND        H3BIT(4)
#define SST_RGBSRCFACT_SHIFT    8          /* blend-factor fields, 4b each */
#define SST_RGBSRCFACT          (0xFL << SST_RGBSRCFACT_SHIFT)
#define SST_RGBDSTFACT_SHIFT    12
#define SST_RGBDSTFACT          (0xFL << SST_RGBDSTFACT_SHIFT)
#define SST_ASRCFACT_SHIFT      16
#define SST_ASRCFACT            (0xFL << SST_ASRCFACT_SHIFT)
#define SST_ADSTFACT_SHIFT      20
#define SST_ADSTFACT            (0xFL << SST_ADSTFACT_SHIFT)
#define SST_ALPHAREF_SHIFT      24
/* 8-bit alpha ref, bits [31:24]. The mask MUST be UNSIGNED: 0xFF<<24 reaches
 * bit 31, which is signed-overflow UB where `long` is 32-bit (mingw i686 /
 * VC6). H3/incsrc/h3defs.h:463 writes `(0xFF<<SST_ALPHAREF_SHIFT)` -- a plain
 * int, unsigned enough on MSVC; the UL keeps it unsigned across ABIs (used at
 * gbk_state.c: `alphamode &= ~SST_ALPHAREF`). Was 0xFFL (signed) -> UB. */
#define SST_ALPHAREF            (0xFFUL << SST_ALPHAREF_SHIFT)
H3HW_STATIC_ASSERT(h3hw_assert_alpharef_unsigned,
                   H3HW_MASK_IS_UNSIGNED(SST_ALPHAREF));
/* blend factor codes (:445-453) - match GR_BLEND_* numerically */
#define SST_A_ZERO              0
#define SST_A_SRCALPHA          1
#define SST_A_COLOR             2
#define SST_A_DSTALPHA          3
#define SST_A_ONE               4
#define SST_AOM_SRCALPHA        5
#define SST_AOM_COLOR           6
#define SST_AOM_DSTALPHA        7
#define SST_A_SATURATE          0xF        /* SRC factor only */
#define SST_A_COLORBEFOREFOG    0xF        /* DST factor only */

/* ---- zaColor bits (H3/incsrc/h3defs.h:467-471) --------------------------- */
#define SST_ZACOLOR_DEPTH_SHIFT 0
#define SST_ZACOLOR_DEPTH       (0xFFFFL << SST_ZACOLOR_DEPTH_SHIFT)
#define SST_ZACOLOR_ALPHA_SHIFT 24
/* same bit-31 hazard as SST_ALPHAREF: H3/incsrc/h3defs.h:469 is 0xFF<<24, so
 * the mask must be unsigned to avoid ILP32 signed-shift UB. Was 0xFFL. */
#define SST_ZACOLOR_ALPHA       (0xFFUL << SST_ZACOLOR_ALPHA_SHIFT)
H3HW_STATIC_ASSERT(h3hw_assert_zacolor_alpha_unsigned,
                   H3HW_MASK_IS_UNSIGNED(SST_ZACOLOR_ALPHA));

/* ---- clip bits (H3/incsrc/h3defs.h:483-495) ------------------------------ */
#define SST_CLIPLEFT_SHIFT      16
#define SST_CLIPRIGHT_SHIFT     0
#define SST_CLIPBOTTOM_SHIFT    16
#define SST_CLIPTOP_SHIFT       0

/* ======================================================================== */
/* sSetupMode / PKT3 pmask bits (H3/incsrc/h3defs.h:501-513)                */
/* ======================================================================== */
#define SST_SETUP_RGB           H3BIT(0)
#define SST_SETUP_A             H3BIT(1)
#define SST_SETUP_Z             H3BIT(2)
#define SST_SETUP_Wfbi          H3BIT(3)
#define SST_SETUP_W0            H3BIT(4)   /* never used - design sec 2 */
#define SST_SETUP_ST0           H3BIT(5)
#define SST_SETUP_FAN           H3BIT(16)
#define SST_SETUP_EN_CULLING    H3BIT(17)
#define SST_SETUP_CULL_NEGATIVE H3BIT(18)
#define SST_SETUP_DIS_PINGPONG  H3BIT(19)

/* PKT3-header smode nibble equivalents (H3/glide3/src/fxcmd.h:544-551) */
#define kSetupStrip             0x00L
#define kSetupFan               0x01L
#define kSetupCullEnable        0x02L
#define kSetupCullNegative      0x04L
#define kSetupPingPongDisable   0x08L

/* chip-select field in PKT1/PKT4 headers (H3/glide3/src/fxcmd.h:467-476) */
#define kChipFieldShift         11         /* (8UL + 3UL) */
#define eChipBroadcast          0x00L
#define eChipFBI                0x01L
#define eChipTMU0               0x02L

/* ======================================================================== */
/* textureMode bits (H3/incsrc/h3defs.h:160-266)                            */
/* ======================================================================== */
#define SST_TPERSP_ST           H3BIT(0)
#define SST_TMINFILTER          H3BIT(1)   /* 1 = bilinear min filter */
#define SST_TMAGFILTER          H3BIT(2)   /* 1 = bilinear mag filter */
#define SST_TCLAMPW             H3BIT(3)
#define SST_TCLAMPS             H3BIT(6)
#define SST_TCLAMPT             H3BIT(7)
#define SST_TFORMAT_SHIFT       8
#define SST_TFORMAT             (0xFL << SST_TFORMAT_SHIFT)
#define SST_RGB332              (0L  << SST_TFORMAT_SHIFT)
#define SST_A8                  (2L  << SST_TFORMAT_SHIFT)
#define SST_I8                  (3L  << SST_TFORMAT_SHIFT)
#define SST_AI44                (4L  << SST_TFORMAT_SHIFT)
#define SST_P8                  (5L  << SST_TFORMAT_SHIFT)
#define SST_ARGB8332            (8L  << SST_TFORMAT_SHIFT)
#define SST_RGB565              (10L << SST_TFORMAT_SHIFT)
#define SST_ARGB1555            (11L << SST_TFORMAT_SHIFT)
#define SST_ARGB4444            (12L << SST_TFORMAT_SHIFT)
#define SST_AI88                (13L << SST_TFORMAT_SHIFT)
#define SST_AP88                (14L << SST_TFORMAT_SHIFT)
/* texture-combine unit (:191-218); PASS/REPLACE abstractions (:234-247) */
#define SST_TC_ZERO_OTHER       H3BIT(12)
#define SST_TC_SUB_CLOCAL       H3BIT(13)
#define SST_TC_MSELECT_SHIFT    14
#define SST_TC_MONE             (0L << SST_TC_MSELECT_SHIFT)
#define SST_TC_MCLOCAL          (1L << SST_TC_MSELECT_SHIFT)
#define SST_TC_REVERSE_BLEND    H3BIT(17)
#define SST_TC_ADD_CLOCAL       H3BIT(18)
#define SST_TC_INVERT_OUTPUT    H3BIT(20)
#define SST_TCA_ZERO_OTHER      H3BIT(21)
#define SST_TCA_SUB_CLOCAL      H3BIT(22)
#define SST_TCA_MSELECT_SHIFT   23
#define SST_TCA_MONE            (0L << SST_TCA_MSELECT_SHIFT)
#define SST_TCA_MCLOCAL         (1L << SST_TCA_MSELECT_SHIFT)
#define SST_TCA_REVERSE_BLEND   H3BIT(26)
#define SST_TCA_ADD_CLOCAL      H3BIT(27)
#define SST_TCA_INVERT_OUTPUT   H3BIT(29)
#define SST_TCOMBINE_SHIFT      12
#define SST_TCOMBINE            (0x1FFL << SST_TCOMBINE_SHIFT)
#define SST_TC_REPLACE          (SST_TC_ZERO_OTHER | SST_TC_ADD_CLOCAL)
#define SST_TC_PASS             (SST_TC_MONE)
#define SST_TACOMBINE_SHIFT     21
#define SST_TACOMBINE           (0x1FFL << SST_TACOMBINE_SHIFT)
#define SST_TCA_REPLACE         (SST_TCA_ZERO_OTHER | SST_TCA_ADD_CLOCAL)
#define SST_TCA_PASS            (SST_TCA_MONE)

/* ---- tLOD bits (H3/incsrc/h3defs.h:264-284) ------------------------------ */
#define SST_LODMIN_SHIFT        0          /* 4.2 format */
#define SST_LODMIN              (0x3FL << SST_LODMIN_SHIFT)
#define SST_LODMAX_SHIFT        6
#define SST_LODMAX              (0x3FL << SST_LODMAX_SHIFT)
#define SST_LODBIAS_SHIFT       12
#define SST_LODBIAS             (0x3FL << SST_LODBIAS_SHIFT)
#define SST_LOD_ODD             H3BIT(18)
#define SST_LOD_S_IS_WIDER      H3BIT(20)
#define SST_LOD_ASPECT_SHIFT    21
#define SST_LOD_ASPECT          (0x3L << SST_LOD_ASPECT_SHIFT)

/* ---- texBaseAddr bits (H3/incsrc/h3defs.h:561-562) ----------------------- */
#define SST_TEXTURE_ADDRESS     (H3MASK(20) << 4)  /* 16-byte-aligned base */
#define SST_TEXTURE_FULL_ADDRESS H3MASK(24)

/* ======================================================================== */
/* CMDFIFO command packets (H3/incsrc/h3gdefs.h:181-257)                    */
/* ======================================================================== */
#define SSTCP_PKT_SIZE          3          /* type field width [2:0]        */
#define SSTCP_PKT               H3MASK(SSTCP_PKT_SIZE)
#define SSTCP_PKT0              0
#define SSTCP_PKT1              1
#define SSTCP_PKT2              2
#define SSTCP_PKT3              3
#define SSTCP_PKT4              4
#define SSTCP_PKT5              5

/* packet 0 (:197-205): jumps - the software FIFO wrap */
#define SSTCP_PKT0_FUNC_SHIFT   3
#define SSTCP_PKT0_NOP          ((0L << SSTCP_PKT0_FUNC_SHIFT) | SSTCP_PKT0)
#define SSTCP_PKT0_JMP_LOCAL    ((3L << SSTCP_PKT0_FUNC_SHIFT) | SSTCP_PKT0)
#define SSTCP_PKT0_ADDR_SHIFT   6
#define SSTCP_PKT0_ADDR         (0x7FFFFFL << SSTCP_PKT0_ADDR_SHIFT)

/* packet 1 (:208-218): single/burst register write.
 * regBase = (byteAddr>>2) & 0x3FF, placed at bit 3 (SSTCP_REGBASE_FROM_ADDR,
 * non-H4 variant :214-215); chip field overlays [13:11] (kChipFieldShift). */
#define SSTCP_REGBASE_SHIFT     SSTCP_PKT_SIZE
#define SSTCP_REGBASE           (0x3FFL << SSTCP_REGBASE_SHIFT)
#define SSTCP_REGBASE_FROM_ADDR(x) ((((x) >> 2) & 0x3FFL) << SSTCP_REGBASE_SHIFT)
#define SSTCP_PKT1_2D           H3BIT(14)
#define SSTCP_INC               H3BIT(15)
#define SSTCP_PKT1_NWORDS_SHIFT 16
#define SSTCP_PKT1_NWORDS       (0xFFFFUL << SSTCP_PKT1_NWORDS_SHIFT)

/* packet 3 (:225-237): triangle setup. hdr fields per fxcmd.h:898-917:
 * [27:22] smode, [21:10] pmask (+pack[28]), [9:6] nVerts (max 15,
 * fxcmd.h:882), [5:3] cmd, [2:0] = 3.                                       */
#define SSTCP_PKT3_CMD_SHIFT    SSTCP_PKT_SIZE
#define SSTCP_PKT3_BDDBDD       (0L << SSTCP_PKT3_CMD_SHIFT)  /* independent */
#define SSTCP_PKT3_BDDDDD       (1L << SSTCP_PKT3_CMD_SHIFT)  /* strip start */
#define SSTCP_PKT3_DDDDDD       (2L << SSTCP_PKT3_CMD_SHIFT)  /* strip cont  */
#define SSTCP_PKT3_NUMVERTEX_SHIFT (SSTCP_PKT_SIZE + 3)          /* = 6  */
#define SSTCP_PKT3_NUMVERTEX    (0xFL << SSTCP_PKT3_NUMVERTEX_SHIFT)
#define SSTCP_PKT3_PMASK_SHIFT  (SSTCP_PKT_SIZE + 3 + 4)         /* = 10 */
#define SSTCP_PKT3_PMASK        (0xFFFUL << SSTCP_PKT3_PMASK_SHIFT)
#define SSTCP_PKT3_SMODE_SHIFT  (SSTCP_PKT3_PMASK_SHIFT + 12)    /* = 22 */
#define SSTCP_PKT3_SMODE        (0x3FUL << SSTCP_PKT3_SMODE_SHIFT)
#define SSTCP_PKT3_PACKEDCOLOR  H3BIT(28)
#define SSTCP_PKT3_MAXVERTS     15         /* fxcmd.h:882 */

/* packet 4 (:240-243): masked register-group write, base reg + 14-bit mask */
#define SSTCP_PKT4_2D           H3BIT(14)
#define SSTCP_PKT4_MASK_SHIFT   15
#define SSTCP_PKT4_MASK         (0x3FFFUL << SSTCP_PKT4_MASK_SHIFT)

/* packet 5 (:245-257): linear write (texture download etc.) */
#define SSTCP_PKT5_NWORDS_SHIFT 3
#define SSTCP_PKT5_NWORDS       (0x7FFFFUL << SSTCP_PKT5_NWORDS_SHIFT)
#define SSTCP_PKT5_BYTEN_WN_SHIFT 22       /* byte-enables, last word  */
#define SSTCP_PKT5_BYTEN_W2_SHIFT 26       /* byte-enables, first word */
#define SSTCP_PKT5_SPACE_SHIFT  30
#define SSTCP_PKT5_LFB          (0x0UL << SSTCP_PKT5_SPACE_SHIFT)
#define SSTCP_PKT5_YUV          (0x1UL << SSTCP_PKT5_SPACE_SHIFT)
#define SSTCP_PKT5_3DLFB        (0x2UL << SSTCP_PKT5_SPACE_SHIFT)
#define SSTCP_PKT5_TEXPORT      (0x3UL << SSTCP_PKT5_SPACE_SHIFT)
#define SSTCP_PKT5_BASEADDR     0x1FFFFFFUL /* hdr2 dest byte addr mask */

#endif /* H3HW_H */
