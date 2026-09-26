/*
 * vcr_3dregs.h - the 3D register block of the Banshee / Voodoo3 / VSA-100
 * (BAR0 + 0x200000), as the display driver's Direct3D HAL uses it.
 *
 * Offsets: offsetof() over the open 3dfx Glide release's SstRegs (h3regs.h),
 * computed, not transcribed. Bit fields: its h3defs.h (SST_*), renamed. Both
 * are the GPL release this stack is built on (voodoo-cleanroom/FORKS.md).
 *
 * Chip field: address bits [13:10] of a 3D register pick the unit - 0 writes
 * the FBI and every TMU at once, 0x800 TMU0 alone. Texture registers are
 * written to TMU0 (TMU1, where there is one, keeps passing its input through).
 */
#ifndef VCR_3DREGS_H
#define VCR_3DREGS_H

#define V3D_BASE                0x200000u
#define V3D_TMU0                0x000800u   /* chip field: TMU0 only */

#define V3D_STATUS              0x000
#define V3D_FBZCOLORPATH        0x104
#define V3D_FOGMODE             0x108
#define V3D_ALPHAMODE           0x10c
#define V3D_FBZMODE             0x110
#define V3D_LFBMODE             0x114
#define V3D_CLIPLEFTRIGHT       0x118
#define V3D_CLIPBOTTOMTOP       0x11c
#define V3D_NOPCMD              0x120
#define V3D_FASTFILLCMD         0x124
#define V3D_FOGCOLOR            0x12c
#define V3D_ZACOLOR             0x130
#define V3D_CHROMAKEY           0x134
#define V3D_C0                  0x144
#define V3D_C1                  0x148
#define V3D_FOGTABLE            0x160
#define V3D_COLBUFFERADDR       0x1ec
#define V3D_COLBUFFERSTRIDE     0x1f0
#define V3D_AUXBUFFERADDR       0x1f4
#define V3D_AUXBUFFERSTRIDE     0x1f8
#define V3D_SSETUPMODE          0x260
#define V3D_SVX                 0x264
#define V3D_SVY                 0x268
#define V3D_SARGB               0x26c
#define V3D_SVZ                 0x280
#define V3D_SOOWFBI             0x284
#define V3D_SOOW0               0x288
#define V3D_SSOW0               0x28c
#define V3D_STOW0               0x290
#define V3D_SDRAWTRICMD         0x2a0
#define V3D_SBEGINTRICMD        0x2a4
#define V3D_TEXTUREMODE         0x300
#define V3D_TLOD                0x304
#define V3D_TDETAIL             0x308
#define V3D_TEXBASEADDR         0x30c

/* fbzColorPath */
#define CP_RGBSEL_ITER          (0u << 0)
#define CP_RGBSEL_TMU           (1u << 0)
#define CP_RGBSEL_C1            (2u << 0)
#define CP_ASEL_ITER            (0u << 2)
#define CP_ASEL_TMU             (1u << 2)
#define CP_ASEL_C1              (2u << 2)
#define CP_LOCAL_C0             (1u << 4)       /* 0: iterated */
#define CP_ALOCAL_C0            (1u << 5)       /* 0: iterated */
#define CP_CC_ZERO_OTHER        (1u << 8)
#define CP_CC_SUB_CLOCAL        (1u << 9)
#define CP_CC_M(v)              ((unsigned)(v) << 10)
#define   CC_M_ONE              0u              /* with REVERSE: zero */
#define   CC_M_CLOCAL           1u
#define   CC_M_AOTHER           2u
#define   CC_M_ALOCAL           3u
#define   CC_M_ATMU             4u
#define   CC_M_RGBTMU           5u
#define CP_CC_REVERSE           (1u << 13)      /* factor = m, not 1 - m */
#define CP_CC_ADD_CLOCAL        (1u << 14)
#define CP_CC_ADD_ALOCAL        (1u << 15)
#define CP_CC_INVERT            (1u << 16)
#define CP_CCA_ZERO_OTHER       (1u << 17)
#define CP_CCA_SUB_CLOCAL       (1u << 18)
#define CP_CCA_M(v)             ((unsigned)(v) << 19)
#define CP_CCA_REVERSE          (1u << 22)
#define CP_CCA_ADD_CLOCAL       (1u << 23)
#define CP_CCA_ADD_ALOCAL       (1u << 24)
#define CP_CCA_INVERT           (1u << 25)
#define CP_PARMADJUST           (1u << 26)
#define CP_TEXTURE              (1u << 27)

/* fbzMode */
#define FZ_RECTCLIP             (1u << 0)
#define FZ_CHROMAKEY            (1u << 1)
#define FZ_WBUFFER              (1u << 3)
#define FZ_DEPTH                (1u << 4)
#define FZ_ZFUNC(f)             ((unsigned)(f) << 5)    /* LT=1 EQ=2 GT=4 */
#define FZ_DITHER               (1u << 8)
#define FZ_RGBWRITE             (1u << 9)
#define FZ_ZAWRITE              (1u << 10)
#define FZ_DITHER2X2            (1u << 11)
#define FZ_ZBIAS                (1u << 16)
#define FZ_YORIGIN              (1u << 17)
#define FZ_ALPHABUFFER          (1u << 18)

/* alphaMode */
#define AM_ATEST                (1u << 0)
#define AM_AFUNC(f)             ((unsigned)(f) << 1)
#define AM_BLEND                (1u << 4)
#define AM_RGBSRC(f)            ((unsigned)(f) << 8)
#define AM_RGBDST(f)            ((unsigned)(f) << 12)
#define AM_ASRC(f)              ((unsigned)(f) << 16)
#define AM_ADST(f)              ((unsigned)(f) << 20)
#define AM_AREF(r)              ((unsigned)(r) << 24)
#define   BF_ZERO               0u
#define   BF_SRCALPHA           1u
#define   BF_COLOR              2u      /* source factor: DEST colour; dest factor: SRC colour */
#define   BF_DSTALPHA           3u
#define   BF_ONE                4u
#define   BF_INVSRCALPHA        5u
#define   BF_INVCOLOR           6u
#define   BF_INVDSTALPHA        7u
#define   BF_SATURATE           0xfu    /* source factor only */

/* fogMode */
#define FM_ENABLE               (1u << 0)

/* textureMode (TMU) */
#define TM_PERSPECTIVE          (1u << 0)
#define TM_MINFILTER            (1u << 1)       /* bilinear */
#define TM_MAGFILTER            (1u << 2)
#define TM_CLAMPW               (1u << 3)
#define TM_CLAMPS               (1u << 6)
#define TM_CLAMPT               (1u << 7)
#define TM_FORMAT(f)            ((unsigned)(f) << 8)
#define   TF_RGB565             10u
#define   TF_ARGB1555           11u
#define   TF_ARGB4444           12u
#define   TF_ARGB8888           0x12u   /* VSA-100 only: textureMode[11:8] + tLOD ext bit */
/* the TMU's own combine = pass its texture through: other zeroed, local added */
#define TM_TC_REPLACE           ((1u << 12) | (1u << 18))
#define TM_TCA_REPLACE          ((1u << 21) | (1u << 27))

/* tLOD */
#define TL_LODMIN(l)            ((unsigned)(l) << 2)        /* 4.2 fixed, lod 0 = 256 texels */
#define TL_LODMAX(l)            ((unsigned)(l) << 8)
#define TL_S_IS_WIDER           (1u << 20)
#define TL_ASPECT(a)            ((unsigned)(a) << 21)       /* log2 of the aspect ratio */

/* sSetupMode */
#define SM_RGB                  (1u << 0)
#define SM_A                    (1u << 1)
#define SM_Z                    (1u << 2)
#define SM_WFBI                 (1u << 3)
#define SM_W0                   (1u << 4)
#define SM_ST0                  (1u << 5)
#define SM_FAN                  (1u << 16)
#define SM_CULL                 (1u << 17)
#define SM_CULL_NEGATIVE        (1u << 18)
#define SM_NO_PINGPONG          (1u << 19)

/* colBufferStride / auxBufferStride: bit 15 clear = linear, stride in bytes */
#define BS_LINEAR_STRIDE(b)     ((unsigned)(b) & 0x3fffu)

#endif /* VCR_3DREGS_H */
