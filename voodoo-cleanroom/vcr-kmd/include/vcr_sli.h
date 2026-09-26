/*
 * vcr_sli.h - VSA-100 multi-chip (SLI / AA) bring-up: the interface of the
 * port of 3dfx's Glide GPL dos_mode.c sequence (miniport/vcrmp_sli.c).
 *
 * The sequence talks to the hardware ONLY through the accessor table below,
 * so the exact same code runs in the kernel (over VcrPciRead/VcrRd/the I/O
 * BAR) and in the host test (tests/native/test_vcr_kmd_sli.c, over a mock of
 * four chips). No OS headers, no floating point.
 *
 * Order of use (the order Glide's minihwc uses):
 *   1. vcr_sli_map_slaves()   once, before the slaves are touched - gives
 *                             chips 1..n-1 their BARs (mapSlavePhysical).
 *                             The caller then maps each slave's new BAR0.
 *   2. vcr_sli_set(enable)    per SLI_AA_REQUEST with sliEn||aaEn. Runs
 *                             initSlave on every slave itself (as Glide does),
 *                             copies the master's video mode to the slaves,
 *                             and programs the SLI/AA config space.
 *   3. vcr_sli_set(disable)   per SLI_AA_REQUEST with neither - AND on
 *                             teardown after a Glide client died, because
 *                             Glide skips its own teardown when its context
 *                             was lost (docs/survey-glide-kernel-contract.md).
 * vcr_sli_init_slaves() is exported on its own for bring-up and tests.
 *
 * Licence: the sequence is a port of 3DFX GLIDE Source Code General Public
 * License code (glide3x/h5/minihwc/dos_mode.c, glide3/src/gsst.c), the
 * licence our Glide fork carries.
 */
#ifndef VCR_SLI_H
#define VCR_SLI_H

#include "vcr_types.h"
#include "vcr_hwcext.h"     /* vcr_sli_aa_req: the SLI_AA_REQUEST payload */

#define VCR_SLI_MAX_CHIPS   4

/*
 * The accessor table. Every callback is required except log and stall_us.
 *
 *   cfg_rd/cfg_wr  PCI config dword of chip 0..3 (chip 0 = the master =
 *                  function 0; slaves need raw 0xCF8 cycles on the V5 6000).
 *   io_rd/io_wr    a dword of that chip's memBase0: the IO registers at
 *                  0x000-0x0ff, the 3D registers at 0x200000+ (sliCtrl).
 *                  For chip >= 1 this needs the slave's BAR0 as programmed by
 *                  vcr_sli_map_slaves().
 *   vga_rd/vga_wr  a legacy VGA register 0x3b0-0x3df of `chip`. ALL chips
 *                  share ONE I/O BAR (mapSlavePhysical programs every slave's
 *                  ioBase = the master's); which chip answers is decided by
 *                  the Command-register I/O-decode bit that this sequence
 *                  toggles itself, exactly as dos_mode.c does. So the kernel
 *                  must reach these through the I/O BAR ALIAS (ioBase + port -
 *                  0x300), never the legacy 0x3xx ports: a slave is set to
 *                  legacy decode OFF by initSlave, and the master's legacy
 *                  decode is off with its I/O decode. `chip` says which chip
 *                  the sequence believes it is talking to (the mock checks it).
 *   stall_us       busy-wait; used between polls of a bounded wait.
 *   log            called BEFORE every register write (so after a hang the
 *                  flight recorder's last entry is the write that hung), and
 *                  for a few read-backs and decisions. step = VCR_SLI_S_*,
 *                  reg = config offset / IO-register offset / VGA port (for an
 *                  indexed VGA pair: VCR_SLI_VGA_IDX(port, index)), val = the
 *                  value.
 */
#define VCR_SLI_VGA_INDEXED      0x01000000u
#define VCR_SLI_VGA_IDX(port, index) \
    (VCR_SLI_VGA_INDEXED | ((vcr_u32)(index) << 16) | (vcr_u32)(port))
typedef struct vcr_sli_io {
    void *ctx;
    vcr_u32 (*cfg_rd)(void *ctx, vcr_u32 chip, vcr_u32 off);          /* PCI config dword of chip 0..3 */
    void    (*cfg_wr)(void *ctx, vcr_u32 chip, vcr_u32 off, vcr_u32 v);
    vcr_u32 (*io_rd)(void *ctx, vcr_u32 chip, vcr_u32 off);           /* memBase0 dword (IO regs, 3D regs at 0x200000+) */
    void    (*io_wr)(void *ctx, vcr_u32 chip, vcr_u32 off, vcr_u32 v);
    vcr_u8  (*vga_rd)(void *ctx, vcr_u32 chip, vcr_u32 port);          /* legacy VGA port 0x3b0-0x3df, per chip */
    void    (*vga_wr)(void *ctx, vcr_u32 chip, vcr_u32 port, vcr_u8 v);
    void    (*stall_us)(void *ctx, vcr_u32 us);
    void    (*log)(void *ctx, vcr_u32 step, vcr_u32 chip, vcr_u32 reg, vcr_u32 val, const char *what);
} vcr_sli_io;

/*
 * The request is Glide's own SLI_AA_REQUEST payload, vcr_sli_aa_req
 * (vcr_hwcext.h), with these semantics (minihwc.c fills it):
 *   ChipInfo.dwChips            chips to use: 1, 2 or 4
 *   ChipInfo.dwsliEn            SLI on (Glide: h3nwaySli > 1)
 *   ChipInfo.dwaaEn             AA on (Glide: h3pixelSample > 1)
 *   ChipInfo.dwaaSampleHigh     0 = 2-sample, 1 = 4-sample, 2 = 8-sample AA
 *   ChipInfo.dwsliAaAnalog      1 = analog SLI/AA (video merged in the DACs)
 *   ChipInfo.dwsli_nlines       SLI band height in LINES (1 << Glide's log2), 2..128
 *   ChipInfo.dwCfgSwapAlgorithm Glide always sends 1; dos_mode.c hard-codes
 *                               the swap-algorithm bit - we set it iff != 0
 *   MemInfo.dwTotalMemory, dwTileMark, dwTileCmpMark
 *                               bytes; logged, unused (as in dos_mode.c)
 *   MemInfo.dwaaSecondaryColorBufBegin / DepthBufBegin / DepthBufEnd
 *                               bytes (Glide: colBuffStart1[0], lfbBuffAddr0[n])
 *   MemInfo.dwBpp               15, 16 or 32 (needed only when AA is on)
 * A DISABLE request (sliEn = aaEn = 0) uses dwChips only - Glide leaves the
 * other fields as stack garbage.
 */

/* ---- results --------------------------------------------------------------
 * < 0: refused, and NOTHING was written (all checks run before the first write)
 *   0: done
 * > 0: done, but with the VCR_SLI_W_* conditions set - make them visible */
#define VCR_SLI_OK           0
#define VCR_SLI_EINVAL     (-1)     /* bad request / accessor table */
#define VCR_SLI_ENODEV     (-2)     /* a slave did not answer as a VSA-100 */
#define VCR_SLI_ENOTIMPL   (-3)     /* vcr_sli_6k_clock(): not built in, switched off, or failed */
#define VCR_SLI_W_TIMEOUT   0x1     /* a bounded wait expired; the write it guarded was skipped */
#define VCR_SLI_W_NOCLOCK   0x2     /* 4-chip board: the V5 6000 external clock was NOT programmed */
#define VCR_SLI_W_NOMUX     0x4     /* no cfgVideoCtrl branch for this chip/SLI/AA combination
                                       (dos_mode.c leaves those registers as they were) */
#define VCR_SLI_W_READBACK  0x8     /* a slave BAR did not read back as written */

/* Bound of every poll loop: polls x VCR_SLI_POLL_US. The original's
 * CHECKFORROOM (h3cinitdd.h:69) spins forever. */
#define VCR_SLI_ROOM_POLLS  10000
#define VCR_SLI_POLL_US     1

/* ---- step codes (the `step` argument of io->log) ---------------------------
 * The kernel logs each as VCR_EV_SLI_STEP (a=step b=chip c=reg d=value).
 * X-macro so a tool can parse names out of this file. NEVER renumber. */
#define VCR_SLI_STEP_TABLE \
    /* 1xx mapSlavePhysical (dos_mode.c:227-323) */ \
    VCR_SLI_STEP(VCR_SLI_S_MAP_BEGIN,      100, "reg=master cfgPciDecode val=nchips") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_MASTER_DEC, 101, "master cfgPciDecode narrowed from power-up") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_INITEN,     102, "slave cfgInitEnable: init/FIFO/BAR writes on") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_DECODE,     103, "slave cfgPciDecode (+ snoop decode sizes)") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_BAR0,       104, "slave memBase0") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_BAR1,       105, "slave memBase1 (shared by all slaves)") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_IOBAR,      106, "slave ioBase (= master's; I/O decode off)") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_COMMAND,    107, "slave command: memory on, I/O off") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_DONE,       108, "read-back: reg=BAR0 val=BAR1") \
    VCR_SLI_STEP(VCR_SLI_S_MAP_LOCK,       109, "slave cfgInitEnable: BAR writes off again (vendor state)") \
    /* 2xx initSlave (dos_mode.c:326-390, h3cinit.c h3InitResetAll/h3InitVga) */ \
    VCR_SLI_STEP(VCR_SLI_S_INIT_BEGIN,     200, "") \
    VCR_SLI_STEP(VCR_SLI_S_INIT_COPY,      201, "master IO register copied to the slave") \
    VCR_SLI_STEP(VCR_SLI_S_INIT_MISC0,     202, "miscInit0 with raw-LFB byte swizzle (bit 30) off") \
    VCR_SLI_STEP(VCR_SLI_S_INIT_DRAM,      203, "slave SDRAM mode register (dramData/dramCommand)") \
    VCR_SLI_STEP(VCR_SLI_S_IODEC_OFF,      204, "command I/O decode off (reg=0x04 val=command)") \
    VCR_SLI_STEP(VCR_SLI_S_IODEC_ON,       205, "command I/O decode on (reg=0x04 val=command)") \
    VCR_SLI_STEP(VCR_SLI_S_INIT_RESET,     206, "h3InitResetAll: miscInit1/miscInit0 reset pulse") \
    VCR_SLI_STEP(VCR_SLI_S_INIT_VGA,       207, "h3InitVga(legacy decode off)") \
    VCR_SLI_STEP(VCR_SLI_S_INIT_READBACK,  208, "read-back: reg=offset val=value") \
    /* 3xx slave video mode (dos_mode.c:432-589, 628-682) */ \
    VCR_SLI_STEP(VCR_SLI_S_MODE_INDEX,     300, "master VGA index write while capturing its mode") \
    VCR_SLI_STEP(VCR_SLI_S_MODE_CAPTURED,  301, "captured: reg=modeData slot val=value (no write)") \
    VCR_SLI_STEP(VCR_SLI_S_MODE_VGA,       302, "slave VGA write (reg=port, or 0x01000000|index<<16|port)") \
    VCR_SLI_STEP(VCR_SLI_S_MODE_REG,       303, "slave pllCtrl0/dacMode/vidProcCfg/vgaInit0") \
    VCR_SLI_STEP(VCR_SLI_S_MODE_COPY,      304, "master video-processor register copied to the slave") \
    /* 4xx SLI/AA enable, per chip (dos_mode.c:684-1482; sliCtrl gsst.c) */ \
    VCR_SLI_STEP(VCR_SLI_S_SET_BEGIN,      400, "reg=sliEn|aaEn<<1|analog<<2|sampleHigh<<4 val=nlines") \
    VCR_SLI_STEP(VCR_SLI_S_PCIINIT0,       401, "pciInit0: read/write wait states, no retry interval") \
    VCR_SLI_STEP(VCR_SLI_S_TMUGBEINIT,     402, "tmuGbeInit: AA clock delay 2, inverted") \
    VCR_SLI_STEP(VCR_SLI_S_SWAP,           403, "cfgInitEnable: swap algorithm / swap master") \
    VCR_SLI_STEP(VCR_SLI_S_SNOOP,          404, "cfgInitEnable: address/memBase snooping") \
    VCR_SLI_STEP(VCR_SLI_S_SNOOP_DECODE,   405, "cfgPciDecode: memBase1 snoop address") \
    VCR_SLI_STEP(VCR_SLI_S_SLILFBCTRL,     406, "cfgSliLfbCtrl") \
    VCR_SLI_STEP(VCR_SLI_S_AALFBCTRL,      407, "cfgAALfbCtrl") \
    VCR_SLI_STEP(VCR_SLI_S_AADEPTH,        408, "cfgAADepthBufferAperture") \
    VCR_SLI_STEP(VCR_SLI_S_VSYNC_OFFSET,   409, "cfgSliAAMisc vga_vsync_offset") \
    VCR_SLI_STEP(VCR_SLI_S_VIDEOCTRL0,     410, "cfgVideoCtrl0") \
    VCR_SLI_STEP(VCR_SLI_S_VIDEOCTRL1,     411, "cfgVideoCtrl1") \
    VCR_SLI_STEP(VCR_SLI_S_VIDEOCTRL2,     412, "cfgVideoCtrl2") \
    VCR_SLI_STEP(VCR_SLI_S_SLV_WAIT,       413, "cfgSliAAMisc: last chip waits for AA LFB read data") \
    VCR_SLI_STEP(VCR_SLI_S_AA_READ_OFF,    414, "cfgAALfbCtrl: AA LFB reads off on chips 2/3") \
    VCR_SLI_STEP(VCR_SLI_S_VIDPLL_SEL,     415, "cfgVideoCtrl0: video PLL locks to sync_clk") \
    VCR_SLI_STEP(VCR_SLI_S_DAC_OFF,        416, "slave miscInit1: RAMDAC powered down") \
    VCR_SLI_STEP(VCR_SLI_S_SLICTRL,        417, "3D sliCtrl (gsst.c _grEnableSliCtrl)") \
    VCR_SLI_STEP(VCR_SLI_S_CLOCK_6K,       418, "V5 6000 external clock hook: val=result") \
    VCR_SLI_STEP(VCR_SLI_S_NOMUX,          419, "no cfgVideoCtrl branch for this combination") \
    VCR_SLI_STEP(VCR_SLI_S_SET_DONE,       420, "val=result") \
    VCR_SLI_STEP(VCR_SLI_S_SET_MEMINFO,    421, "request memory info, unused as in dos_mode.c: reg=totalMem val=tileMark") \
    /* 5xx SLI/AA disable (dos_mode.c:1483-1512; sliCtrl gsst.c _grDisableSliCtrl) */ \
    VCR_SLI_STEP(VCR_SLI_S_OFF_BEGIN,      500, "val=nchips") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_SLICTRL,    501, "3D sliCtrl = 0") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_INITEN,     502, "cfgInitEnable: snooping and swap control off") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_CFG,        503, "SLI/AA config register = 0 (reg=offset)") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_VIDEOCTRL0, 504, "cfgVideoCtrl0 (slaves: H/V sync tristated)") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_DAC,        505, "slave dacMode: DPMS both syncs") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_VIDPROC,    506, "slave vidProcCfg: video processor off") \
    VCR_SLI_STEP(VCR_SLI_S_OFF_DONE,       507, "val=result") \
    /* 9xx trouble */ \
    VCR_SLI_STEP(VCR_SLI_S_TIMEOUT,        900, "bounded wait expired: reg=register polled val=last value") \
    VCR_SLI_STEP(VCR_SLI_S_REFUSED,        901, "request refused before any write: reg=reason val=value") \

#define VCR_SLI_STEP(name, code, desc) name = code,
enum vcr_sli_step { VCR_SLI_STEP_TABLE VCR_SLI_S__END };
#undef VCR_SLI_STEP

/* reasons for VCR_SLI_S_REFUSED (reg) */
#define VCR_SLI_R_ACCESSORS   1     /* a required callback is NULL */
#define VCR_SLI_R_CHIPS       2     /* dwChips not 1, 2 or 4 (val = dwChips) */
#define VCR_SLI_R_NLINES      3     /* SLI band height not 2..128, power of 2 */
#define VCR_SLI_R_BPP         4     /* AA with a bpp other than 15/16/32 */
#define VCR_SLI_R_SAMPLE      5     /* dwaaSampleHigh > 2 */
#define VCR_SLI_R_SLI_1CHIP   6     /* SLI requested on a single chip */
#define VCR_SLI_R_NODEV       7     /* chip (val) is not a VSA-100 */
#define VCR_SLI_R_BARS        8     /* master BARs unusable (val = BAR0) */

/* ---- register map the sequence needs beyond vcr_regs.h ---------------------
 * From glide3x/h5/incsrc/h3defs.h / h3regs.h (Glide GPL). None of these names
 * exists in vcr_regs.h (2026-09-26); if vcr_regs.h grows one, delete it here. */
#define VCR_SLI_PCI_ID              0x00
#define VCR_SLI_PCI_COMMAND         0x04    /* status_command */
#define VCR_SLI_PCI_BAR0            0x10    /* memBaseAddr0 */
#define VCR_SLI_PCI_BAR1            0x14    /* memBaseAddr1 */
#define VCR_SLI_PCI_IOBAR           0x18    /* ioBaseAddr */
#define VCR_SLI_CMD_IO              0x1u
#define VCR_SLI_CMD_MEM             0x2u

/* cfgInitEnable - h3defs.h: "the spec has these bits shifted left by 8", and
 * dos_mode.c works on (value >> 8). Kept in that form, so the port reads like
 * the original. */
#define VCR_IE_HW_INIT_WRITES       (1u << 0)
#define VCR_IE_PCI_FIFO_WRITES      (1u << 1)
#define VCR_IE_BASE_ADDR_WRITES     (1u << 2)
#define VCR_IE_ADDRESS_SNOOP        (1u << 3)
#define VCR_IE_MEMBASE0_SNOOP_EN    (1u << 4)
#define VCR_IE_MEMBASE1_SNOOP_EN    (1u << 5)
#define VCR_IE_ADDRESS_SNOOP_SLAVE  (1u << 6)
#define VCR_IE_MEMBASE0_SNOOP_SHIFT 7
#define VCR_IE_MEMBASE0_SNOOP       (0x3ffu << 7)
#define VCR_IE_SWAPBUFFER_ALGORITHM (1u << 17)
#define VCR_IE_SWAP_MASTER          (1u << 18)
#define VCR_IE_QUICK_SAMPLING       (1u << 19)
#define VCR_IE_MULTIFUNCTION        (1u << 20)  /* not used here; see report */
#define VCR_IE_INIT_REGISTER_SNOOP  (1u << 22)

/* cfgPciDecode (the membase0/1 / io decode sizes are in vcr_regs.h) */
#define VCR_PCIDEC_IO_256           (0u << 8)
#define VCR_PCIDEC_SNOOP_MB0_SHIFT  10
#define VCR_PCIDEC_SNOOP_MB0_MASK   (0xfu << 10)
#define VCR_PCIDEC_SNOOP_MB1_SHIFT  14
#define VCR_PCIDEC_SNOOP_MB1_MASK   (0xfu << 14)
#define VCR_PCIDEC_MB1_SNOOP_SHIFT  18
#define VCR_PCIDEC_MB1_SNOOP        (0x3ffu << 18)

/* cfgSliLfbCtrl */
#define VCR_SLILFB_RENDERMASK_SHIFT     0
#define VCR_SLILFB_COMPAREMASK_SHIFT    8
#define VCR_SLILFB_SCANMASK_SHIFT       16
#define VCR_SLILFB_NUMCHIPS_LOG2_SHIFT  24
#define VCR_SLILFB_CPU_WRITE_EN         (1u << 26)
#define VCR_SLILFB_DISPATCH_WRITE_EN    (1u << 27)
#define VCR_SLILFB_READ_EN              (1u << 28)

/* cfgAADepthBufferAperture / cfgAALfbCtrl */
#define VCR_AADEPTH_BEGIN_SHIFT         0
#define VCR_AADEPTH_END_SHIFT           16
#define VCR_AALFB_SECONDARY_BASE_SHIFT  4
#define VCR_AALFB_CPU_WRITE_EN          (1u << 26)
#define VCR_AALFB_DISPATCH_WRITE_EN     (1u << 27)
#define VCR_AALFB_READ_EN               (1u << 28)
#define VCR_AALFB_FMT_16BPP             (0u << 29)
#define VCR_AALFB_FMT_15BPP             (1u << 29)
#define VCR_AALFB_FMT_32BPP             (2u << 29)
#define VCR_AALFB_RD_DIVIDE_BY_4        (1u << 31)

/* cfgSliAAMisc */
#define VCR_SLIAA_VSYNC_OFFSET          0x1ffu
#define VCR_SLIAA_VSYNC_PIXELS_SHIFT    0
#define VCR_SLIAA_VSYNC_CHARS_SHIFT     3
#define VCR_SLIAA_VSYNC_HXTRA_SHIFT     6
#define VCR_SLIAA_LFB_RD_SLV_WAIT       (1u << 12)

/* cfgVideoCtrl0 */
#define VCR_VC0_ENHANCED_VIDEO_EN       (1u << 0)
#define VCR_VC0_ENHANCED_VIDEO_SLV      (1u << 1)
#define VCR_VC0_LOCALMUX_DESKTOP_PLUS_OVERLAY (1u << 3)
#define VCR_VC0_OTHERMUX_TRUE_SHIFT     4
#define VCR_VC0_OTHERMUX_FALSE_SHIFT    6
#define   VCR_VC0_MUX_PIPE              0u
#define   VCR_VC0_MUX_PIPE_PLUS_AAFIFO  1u
#define   VCR_VC0_MUX_AAFIFO            2u
#define VCR_VC0_SLI_AAFIFO_COMPARE_INV  (1u << 10)
#define VCR_VC0_VIDPLL_SEL              (1u << 11)
#define VCR_VC0_DIVIDE_BY_1             (0u << 12)
#define VCR_VC0_DIVIDE_BY_2             (1u << 12)
#define VCR_VC0_DIVIDE_BY_4             (2u << 12)
#define VCR_VC0_DIVIDE_BY_8             (3u << 12)
#define VCR_VC0_DAC_VSYNC_TRISTATE      (1u << 24)
#define VCR_VC0_DAC_HSYNC_TRISTATE      (1u << 25)
/* cfgVideoCtrl1 */
#define VCR_VC1_RENDER_FETCH_SHIFT      0
#define VCR_VC1_COMPARE_FETCH_SHIFT     8
#define VCR_VC1_RENDER_CRT_SHIFT        16
#define VCR_VC1_COMPARE_CRT_SHIFT       24
/* cfgVideoCtrl2 */
#define VCR_VC2_RENDER_AAFIFO_SHIFT     0
#define VCR_VC2_COMPARE_AAFIFO_SHIFT    8

/* IO registers / bits (h3defs.h) */
#define VCR_PI0_READ_WS                 (1u << 8)
#define VCR_PI0_WRITE_WS                (1u << 9)
#define VCR_PI0_RETRY_INTERVAL          (0x1fu << 13)
#define VCR_PI0_FORCE_FB_HIGH           (1u << 26)
#define VCR_TMU_AA_CLK_INVERT           (1u << 20)
#define VCR_TMU_AA_CLK_DELAY_SHIFT      21
#define VCR_TMU_AA_CLK_DELAY            (0xfu << 21)
#define VCR_MI0_MEMORY_TIMING_RESET     (1u << 6)
#define VCR_MI0_VGA_TIMING_RESET        (1u << 7)
#define VCR_MI0_RAWLFB_BYTE_SWIZZLE     (1u << 30)
#define VCR_MI1_STRAPS                  (0x1fu << 24)   /* PCI fast/BIOS size/66 MHz/AGP/device type */
#define VCR_SLI_SLAVE_PIXBUFTHOLD       0x00010410u     /* the vendor's slaves, every mode */
#define VCR_STATUS_ROOM_MASK            0x3fu    /* CHECKFORROOM's mask */
#define VCR_STATUS_PCIFIFO_FREE         0x1fu

/* 3D sliCtrl (h3defs.h "SST sliCtrl bits") */
#define VCR_SLICTRL_RENDER_SHIFT        0
#define VCR_SLICTRL_COMPARE_SHIFT       8
#define VCR_SLICTRL_SCAN_SHIFT          16
#define VCR_SLICTRL_LOG2_CHIPS_SHIFT    24
#define VCR_SLICTRL_ENABLE              (1u << 26)

/* ---- entry points ------------------------------------------------------------ */

/* mapSlavePhysical for chips 1..nchips-1. Narrows the master's decode first if
 * it is still in its power-up configuration. Fills slave_bar0/1[c] for every
 * c < nchips (index 0 = the master's own BARs). */
int vcr_sli_map_slaves(const vcr_sli_io *io, vcr_u32 nchips,
                       vcr_u32 slave_bar0[4], vcr_u32 slave_bar1[4]);

/* initSlave for chips 1..nchips-1 (vcr_sli_set's enable path does this too). */
int vcr_sli_init_slaves(const vcr_sli_io *io, vcr_u32 nchips);

/* hwcSetSLIAAMode: enable when r->ChipInfo.dwsliEn || dwaaEn, else disable. */
int vcr_sli_set(const vcr_sli_io *io, const vcr_sli_aa_req *r);

/* The pure values, for callers and tests. */
vcr_u32 vcr_sli_slictrl(vcr_u32 nchips, vcr_u32 nlines, vcr_u32 aa_en,
                        vcr_u32 aa_sample_high, vcr_u32 chip);   /* 0 = no SLI */

/*
 * THE V5 6000 EXTERNAL CLOCK HOOK - NOT IMPLEMENTED HERE, ON PURPOSE.
 * A 4-chip board needs its SLI clock programmed through GPIO in its HiNT HB1
 * PCI bridge (3388:0021, config register 0xC4) before the slaves are started
 * (dos_mode.c:617-621 calls gpio_6k_clock()). Glide's gpio.c carries no
 * licence header, so it is NOT ported; this hook is where our own
 * implementation goes. vcrmp_sli.c provides a stub returning VCR_SLI_ENOTIMPL
 * unless VCR_SLI_HAVE_6K_CLOCK is defined, in which case the real one is
 * linked from elsewhere. It reaches the bridge through io->ctx.
 */
int vcr_sli_6k_clock(const vcr_sli_io *io);

#endif /* VCR_SLI_H */
