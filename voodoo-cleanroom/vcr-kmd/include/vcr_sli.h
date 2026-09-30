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
    /* memory per chip, bytes (vcrmp_hw.c voodoo_fb_bytes): memBase1 decodes
     * twice this. 0 = the 32 MB/chip board dos_mode.c assumes (64 MB). */
    vcr_u32 fb_bytes;
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
 *                               bytes (Glide: h3Mem MB, colBuffStart0[0] twice);
 *                               logged, unused as in dos_mode.c - except by the
 *                               vendor AA recipe (vcr_sli_set_ex), which places
 *                               the AA base and depth aperture with the first two
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
#define VCR_SLI_EINVAL     (-1)     /* bad request / accessor table / unsupported combination */
#define VCR_SLI_ENODEV     (-2)     /* a slave did not answer as a VSA-100 */
#define VCR_SLI_ENOTIMPL   (-3)     /* vcr_sli_6k_clock(): not built in, switched off, or failed */
#define VCR_SLI_EDENIED    (-4)     /* refused by policy: an AA request while Diag\SliAA = 0
                                       (the kernel's AA kill switch, vcrmp_multi.c) */
#define VCR_SLI_W_TIMEOUT   0x1     /* a bounded wait expired; the write it guarded was skipped */
#define VCR_SLI_W_NOCLOCK   0x2     /* 4-chip board: the V5 6000 external clock was NOT programmed */
#define VCR_SLI_W_NOMUX     0x4     /* no cfgVideoCtrl branch for this chip/SLI/AA combination.
                                       UNREACHABLE since 2026-09-27: vcr_sli_set refuses such a
                                       combination before its first write (VCR_SLI_R_COMBO).
                                       Kept so a future branch-table edit cannot slip past. */
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
    VCR_SLI_STEP(VCR_SLI_S_SET_BEGIN,      400, "reg=sliEn|aaEn<<1|analog<<2|sampleHigh<<4|vendorRecipe<<8 val=nlines") \
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
    VCR_SLI_STEP(VCR_SLI_S_SET_MEMINFO,    421, "request memory info (the vendor AA recipe uses it): reg=totalMem val=tileMark") \
    VCR_SLI_STEP(VCR_SLI_S_AA_STATE,       422, "config read-back into Diag\\SliAAState, config cycles only: chip (reg 0) before its reads, reg 1 = done") \
    VCR_SLI_STEP(VCR_SLI_S_AAONLY_SLICTRL, 423, "3D sliCtrl = 0 on an AA-only request (vendor AA recipe)") \
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
#define VCR_SLI_R_COMBO       9     /* no video-mux branch for {chips, sli, aa, sampleHigh, analog}
                                       (val = VCR_SLI_TUPLE) - e.g. cfg 1 on a 4-chip board, which
                                       Glide sends as {4,0,1,0,1} and which wedged .124 twice */
#define VCR_SLI_R_AA_OFF     10     /* AA requested while Diag\SliAA = 0 (val = VCR_SLI_TUPLE) */
#define VCR_SLI_R_MEMINFO    11     /* vendor AA recipe: the request's tileMark / totalMemory cannot
                                       place the AA base or the depth aperture (val = tileMark >> 12) */

/* flags of vcr_sli_set_ex() - the kernel's per-request policy, not Glide's */
#define VCR_SLI_F_VENDOR_AA  0x1u   /* Diag\SliAAVendorRecipe = 1: the vendor-style AA recipe (below) */
/* Supervised arms for the cfg 3 ghost on the V5 6000 (2026-09-28), both
 * default OFF and both confined to the cfg 3 shape {4 chips, SLI, AA,
 * 2-sample, analog} - every other shape writes exactly what it did before.
 *   AAFIFO_GATE (Diag\SliAAFifoGate = 1): each chip's AA-FIFO equation
 *     (cfgVideoCtrl2, Databook 11.5: a feeder drives aa_vld/aa_clk/aa_data,
 *     a summer takes its other-mux TRUE branch, when (line & rmask) == cmask)
 *     becomes its fetch band - a feeder drives only the lines it fetched, a
 *     summer sums only on its own band (chip 2's sum moves to the TRUE mux).
 *     The vendor code drives the bus on every line in this shape alone; 3dfx's
 *     "Video SLI AA Configs" sheet gates it to the band.
 *   FEEDER_LEAD_C1 / _C3 (Diag\SliAAFeederLead bit 0 / bit 1): that feeder's
 *     vga_vsync_offset 47 px (chars 5, "run slave 8 clocks ahead") becomes
 *     39 px (chars 4) - the value every slave runs in the clean cfg 5 on the
 *     same board. Only chars 4 is reachable: 31 px (chars 3) hard-froze the
 *     board (retro-3dfx FINDINGS.md). */
#define VCR_SLI_F_AAFIFO_GATE     0x2u
#define VCR_SLI_F_FEEDER_LEAD_C1  0x4u
#define VCR_SLI_F_FEEDER_LEAD_C3  0x8u
#define VCR_SLI_F_CFG3_ARMS  (VCR_SLI_F_AAFIFO_GATE | VCR_SLI_F_FEEDER_LEAD_C1 | VCR_SLI_F_FEEDER_LEAD_C3)

/* The request's shape, one nibble per field, so a hex dump reads left to
 * right as {chips, sli, aa, sampleHigh, analog}: cfg 1 as Glide sends it on
 * the V5 6000 is 0x40101. Logs and persisted phases only - never decide on
 * it (a nibble saturates at 0xf; vcr_sli_combo_ok takes the raw fields). */
#define VCR_SLI_NIB(v)  ((vcr_u32)(v) > 0xfu ? 0xfu : (vcr_u32)(v))
#define VCR_SLI_TUPLE(n, sli, aa, high, analog) \
    ((VCR_SLI_NIB(n) << 16) | (VCR_SLI_NIB(sli) << 12) | (VCR_SLI_NIB(aa) << 8) | \
     (VCR_SLI_NIB(high) << 4) | VCR_SLI_NIB(analog))

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
/* The AA secondary buffer base is a BYTE ADDRESS, written as it is into
 * bits 4-25 (the low nibble is not part of it): Glide's own single-chip AA
 * path ORs the address in unshifted and reads it back the same way (minihwc.c,
 * aaMark = cfgAALfbCtrl & ~0xfc00000f). dos_mode.c:871 shifts it left by 4;
 * with a base of 0 - what Glide sends for every 1-sample-per-chip tuple, cfg
 * 3/7 - the two agree, and with any real base the shift spills into
 * CPU_WRITE_EN, READ_EN and the read format (0x01a00000 << 4 sets READ_EN).
 * Masked, a base can never reach bit 26. vcr_sli_aalfb_base().
 * The byte-address form is part of the VENDOR recipe only (VCR_SLI_F_VENDOR_AA,
 * vcr_sli_set_ex): the default recipe is the dos_mode.c control arm of an AA
 * A/B and keeps D:871's shift, spill and all (VCR_AALFB_SECONDARY_BASE_SHIFT) -
 * it is what 097b1f7 wrote, and it is reachable only with Diag\SliAA = 1. */
#define VCR_AALFB_SECONDARY_BASE_SHIFT  4           /* D:871, the default recipe */
#define VCR_AALFB_SECONDARY_BASE_MASK   0x03fffff0u
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

/* hwcSetSLIAAMode: enable when r->ChipInfo.dwsliEn || dwaaEn, else disable.
 * An enable whose combination has no video-mux branch is refused before the
 * first write (VCR_SLI_R_COMBO -> VCR_SLI_EINVAL).
 * vcr_sli_set() is vcr_sli_set_ex(io, r, 0): the dos_mode.c-derived sequence,
 * the one that ran cfg 0/2/5 on .124. */
int vcr_sli_set(const vcr_sli_io *io, const vcr_sli_aa_req *r);

/* The same, with the kernel's policy flags (VCR_SLI_F_*).
 *
 * VCR_SLI_F_VENDOR_AA - the vendor-style AA recipe: the kernel's DEFAULT for
 * every AA request since 2026-09-30 (Diag\SliAAVendorRecipe absent/1; 0 = the
 * dos_mode.c control arm, whose base of 0 overwrote Glide's command FIFO -
 * vcrmp_multi.c sli_recipe()). It changes AA requests and
 * nothing else - an SLI-only request or a disable writes exactly what it
 * writes without it. Written from the register semantics (the vendor's W2K
 * miniport was read for guidance, not copied):
 *   - a tuple that stores ONE sample per chip (vcr_sli_samples_per_chip() ==
 *     1: {2,0,1,0,x}, cfg 3 {4,1,1,0,1}, cfg 7 {4,0,1,1,1}): cfgAALfbCtrl
 *     base = the request's tileMark (the primary buffers - there is no
 *     secondary buffer to point at, and Glide sends base 0), AA LFB READ_EN
 *     (the inter-chip read handshake a paired read needs) and RD_DIVIDE_BY_4;
 *   - 4 chips, no SLI, 4- or 8-sample (cfg 7/8): the depth aperture covers
 *     the whole tiled range [tileMark, totalMemory), so an LFB read of any
 *     tiled buffer returns the master's (aliased) data instead of asking four
 *     chips for a merge the hardware cannot do;
 *   - AA without SLI: 3D sliCtrl = 0 on every chip (VCR_SLI_S_AAONLY_SLICTRL),
 *     where dos_mode.c leaves whatever the chips held;
 *   - every AA base is written as the byte address it is (bits 4-25,
 *     vcr_sli_aalfb_base), not D:871's << 4 - which changes cfgAALfbCtrl of
 *     every shape that stores two samples per chip and so has a real
 *     secondary base (cfg 8, 2-way SLI + 4-sample, the 1- and 2-chip shapes).
 * A tuple that needs tileMark / totalMemory and gets values that cannot place
 * a base or an aperture (0, not 4 KB aligned, tileMark >= totalMemory, over
 * 64 MB) is refused before the first write (VCR_SLI_R_MEMINFO) - and, in the
 * kernel, before a live session is torn down (vcr_sli_policy_ex).
 * Every request is refused (VCR_SLI_R_NODEV, val 0) unless chip 0 is a
 * VSA-100: on a Banshee / Voodoo 3 0x80-0xAC are not the SLI/AA registers. */
int vcr_sli_set_ex(const vcr_sli_io *io, const vcr_sli_aa_req *r, vcr_u32 flags);

/* cfgAALfbCtrl's base field for a byte address: addr & bits 4-25, unshifted */
vcr_u32 vcr_sli_aalfb_base(vcr_u32 addr);

/* Samples each chip stores for an AA shape the sequence accepts: 1 or 2.
 * 0 = not an AA request, or a shape vcr_sli_combo_ok refuses. The samples
 * (2 << sampleHigh) spread over the chips of ONE SLI unit - the units
 * cfgSliLfbCtrl splits the bands into: 2 on two chips, and on four chips 2
 * when AA pairs them (analog 2-sample, or 4-sample), else 4. */
vcr_u32 vcr_sli_samples_per_chip(vcr_u32 n, vcr_u32 sli, vcr_u32 aa, vcr_u32 high, vcr_u32 analog);

/* ---- what an AA request left in config space: Diag\SliAAState ---------------
 * After an AA enable the kernel reads back every chip's SLI/AA config
 * registers - by CONFIG CYCLES ONLY, nothing through a BAR - and writes this
 * record, flushed, as REG_BINARY Diag\SliAAState. So a supervised AA run that
 * wedges later still says exactly what the kernel had programmed: compare it
 * with the expected tables in tests/native/test_vcr_kmd_sli.c; tools/
 * vcrphases.py decodes it. Written only for an AA request that was programmed
 * (vcr_sli_aa_state_wanted); a refusal or an SLI-only request leaves the last
 * record as it was.
 *   pciInit0 has no config-space alias (0x4c, cfgStatus, aliases the STATUS
 *   register), so it is the value the sequence WROTE, captured from its own
 *   log callback - flagged per chip by VCR_SLI_ST_PCI0(c). */
#define VCR_SLI_STATE_MAGIC     0x31414153u     /* "SAA1" in memory order */
#define VCR_SLI_STATE_NCFG      9               /* 0x40 0x48 0x80 0x84 0x88 0x8c 0x90 0x94 0xac */
#define VCR_SLI_STATE_BYTES     224
#define VCR_SLI_ST_PCI0(c)      (0x100u << (c)) /* flags: chip c's pciInit0 was recorded */
typedef struct vcr_sli_aa_state {
    vcr_u32 magic, size;        /* VCR_SLI_STATE_MAGIC, VCR_SLI_STATE_BYTES */
    vcr_u32 boot, ms;           /* Diag\BootCount and the driver's clock at the read-back (kernel) */
    vcr_u32 tuple;              /* VCR_SLI_TUPLE of the request */
    vcr_u32 flags;              /* VCR_SLI_F_* it ran with | VCR_SLI_ST_PCI0(c) */
    vcr_i32 result;             /* vcr_sli_set_ex() */
    vcr_u32 nchips;             /* chips read back */
    vcr_u32 nlines, bpp;        /* the request, as sent */
    vcr_u32 tile, total, col, dbeg, dend;
    vcr_u32 reserved;
    struct {
        vcr_u32 cfg[VCR_SLI_STATE_NCFG];    /* in the order of VCR_SLI_STATE_NCFG's comment */
        vcr_u32 pciinit0;                   /* as written (see above) */
    } chip[VCR_SLI_MAX_CHIPS];
} vcr_sli_aa_state;
VCR_STATIC_ASSERT(sli_aa_state_size, sizeof(vcr_sli_aa_state) == VCR_SLI_STATE_BYTES);

/* 1 when an AA request was programmed (dwaaEn, result >= 0): the record's condition */
int vcr_sli_aa_state_wanted(const vcr_sli_aa_req *r, int result);

/* Fill `out` (always zeroed first) from io->cfg_rd ONLY - no BAR access, no
 * write - for the request's chips, logging VCR_SLI_S_AA_STATE before each
 * chip's reads (so a config read that hangs is named by the flight recorder).
 * pciinit0[c] is taken when bit c of pciinit0_mask is set. Returns the chips
 * read back; 0, touching nothing, unless vcr_sli_aa_state_wanted(). The
 * caller fills boot and ms. */
int vcr_sli_aa_readback(const vcr_sli_io *io, const vcr_sli_aa_req *r, int result,
                        vcr_u32 flags, const vcr_u32 pciinit0[VCR_SLI_MAX_CHIPS],
                        vcr_u32 pciinit0_mask, vcr_sli_aa_state *out);

/* ---- the AA safety net: pure decisions, no hardware ---------------------------
 *
 * vcr_sli_combo_ok: 1 when an ENABLE of this shape has a cfgVideoCtrl branch
 * in the ported sequence (video_mux, D:928-1428) AND passes the sanity checks
 * a vendor miniport applies to its input: a single chip does 2-sample AA only,
 * two chips never combine SLI with 4-sample AA, 8-sample AA is 4 chips, no
 * SLI, analog. sli/aa/analog are booleans (non-zero = on); high is the raw
 * dwaaSampleHigh and means nothing without aa. A disable (no sli, no aa) is
 * not a combination: 0. tests/native/test_vcr_kmd_sli.c pins this predicate
 * to video_mux itself for every shape.
 * Inline, so a user-mode tool applies the kernel's own rule without linking
 * the sequence: `vcrctl sliaa` refuses such a shape before it sends anything,
 * which is what protects a VENDOR kernel (AmigaMerlin's froze on cfg 1's
 * {4,0,1,0,1}) that has no such refusal of its own. */
static __inline int vcr_sli_combo_ok(vcr_u32 n, vcr_u32 sli, vcr_u32 aa, vcr_u32 high,
                                     vcr_u32 analog)
{
    sli = sli ? 1 : 0;
    aa = aa ? 1 : 0;
    analog = analog ? 1 : 0;
    if (!aa)
        high = 0;               /* the sample count means nothing without AA */
    if ((!sli && !aa) || high > 2)
        return 0;               /* a disable is not a combination; 16 samples do not exist */
    switch (n) {
    case 1:
        /* one chip: 2-sample AA only (it has no partner for more), never SLI */
        return !sli && high == 0;
    case 2:
        /* 2-way SLI alone or with 2-sample AA, digital or analog - never with
         * 4-sample AA (that takes both chips); AA alone: 2 or 4 samples. Not
         * 8: video_mux's 2-chip 4-sample branches test `high` as a boolean,
         * so an 8-sample request would be programmed as 4-sample. */
        return sli ? high == 0 : high <= 1;
    case 4:
        if (sli)                /* 4-way SLI, or two 2-way units with 2/4-sample AA */
            return high <= 1;
        /* AA across all four chips without SLI: 4 or 8 samples, analog only.
         * {4,0,1,0,x} - Glide's cfg 1 laid out for four chips - has no mux. */
        return analog && high >= 1;
    }
    return 0;
}

/* The request normalised the way vcr_sli_set reads it, packed (VCR_SLI_TUPLE). */
vcr_u32 vcr_sli_req_tuple(const vcr_sli_aa_req *r);

/* What the kernel decides BEFORE it touches anything - before it even tears
 * down a live session to make room (vcrmp_multi.c VcrSliRequest):
 *   0                  go ahead (every disable; SLI-only; AA when allowed and valid)
 *   VCR_SLI_R_AA_OFF   the request enables AA and aa_allowed (Diag\SliAA) is 0
 *   VCR_SLI_R_COMBO    an enable with no video-mux branch
 *   VCR_SLI_R_MEMINFO  (vcr_sli_policy_ex with VCR_SLI_F_VENDOR_AA only) an AA
 *                      shape the vendor recipe places by tileMark/totalMemory,
 *                      with values that cannot place it - the same test
 *                      vcr_sli_set_ex makes, asked before the teardown
 * vcr_sli_policy(r, a) is vcr_sli_policy_ex(r, a, 0). */
int vcr_sli_policy(const vcr_sli_aa_req *r, vcr_u32 aa_allowed);
int vcr_sli_policy_ex(const vcr_sli_aa_req *r, vcr_u32 aa_allowed, vcr_u32 flags);

/* ---- what a flushed phase keeps of a step --------------------------------------
 * The kernel's log callback persists some steps to Diag\PhaseLog (flushed, it
 * survives the power cycle a wedge needs): VcrPhase(VCR_EV_SLI_STEP, a = step,
 * b = vcr_sli_phase_b(step, chip, reg, val)).
 *   vcr_sli_step_persists: the milestones (x00 steps, MAP_DONE, PCIINIT0,
 *     CLOCK_6K, SLICTRL, NOMUX, SET_DONE, AA_STATE, AAONLY_SLICTRL, OFF_DONE,
 *     9xx); with persist_all
 *     (Diag\SliPersistAll = 1, supervised runs) EVERY step, so the last
 *     phase names the exact write a wedge stopped at - at ~15-30 ms a flush,
 *     a 4-chip enable then takes seconds, and the 64-slot history keeps the
 *     last 64 steps.
 *   vcr_sli_phase_b:
 *     most steps       chip << 24 | (reg & 0xffffff)           (the value is dropped)
 *     value steps      chip << 24 | VCR_SLI_PB_VALUE | (val & 0x7fffff)
 *                      SET_DONE / OFF_DONE (the VCR_SLI_W_* mask), CLOCK_6K (the
 *                      hook's result, 23-bit signed), NOMUX (its flags)
 *     REFUSED          reason << 24 | VCR_SLI_PB_VALUE | (val & 0x7fffff)
 *   Bit 23 marks the value form: a record written before 2026-09-27 kept only
 *   chip << 24 | reg for those steps (reg 0 there), so tools/vcrphases.py can
 *   tell "warn 0" from "not recorded". */
#define VCR_SLI_PB_VALUE    0x00800000u
int     vcr_sli_step_persists(vcr_u32 step, vcr_u32 persist_all);
vcr_u32 vcr_sli_phase_b(vcr_u32 step, vcr_u32 chip, vcr_u32 reg, vcr_u32 val);

/* ---- HWCEXT PCI_OP / VCR_ESC_PCI writes to a chip: what the miniport lets through
 * The SLI/AA config registers - cfgInitEnable (0x40), cfgPciDecode (0x48) and
 * 0x80-0xAF (cfgVideoCtrl0/1/2, cfgSliLfbCtrl, cfgAADepthBufferAperture,
 * cfgAALfbCtrl, ..., cfgSliAAMisc) - are vcr_sli_set's. But Glide's NT build
 * writes some of them through PCI_OP on paths that WORK and must keep working
 * byte for byte (they ran cfg 0/2/5 on .124 under 097b1f7):
 *   - hwcRestoreVideo, every Napalm close that was not an SLI / multi-chip AA
 *     session (cfg 0): 0 into cfgSliLfbCtrl, cfgAADepthBufferAperture,
 *     cfgAALfbCtrl, cfgVideoCtrl0 and cfgVideoCtrl2 (twice) of EVERY chip
 *     Glide counts - 24 writes on the 6000;
 *   - hwcSLIReadEnable/Disable (only with FX_GLIDE_A0_READ_ABORT, in n-way
 *     SLI): cfgSliLfbCtrl and cfgAALfbCtrl with READ_EN toggled.
 * So a write is judged by what it would TURN ON, not by where it lands
 * (vcr_sli_poke_policy, the first reason that applies):
 *   VCR_POKE_R_BOUNDS  past the 256-byte header, or not naturally aligned
 *                      (reads too: vcr_cfg_access_ok). A slave is written by
 *                      raw 0xCF8 cycles, which keep only offset bits 2-7 - so
 *                      0x140 WAS cfgVideoCtrl0 and 0x104 the command register.
 *   VCR_POKE_R_HEADER  the standard header (< 0x40) without Diag\AllowPoke
 *                      (as ever)
 *   VCR_POKE_R_SNOOP   cfgInitEnable / cfgPciDecode (snoop, swap, decode)
 *                      without Diag\AllowPoke - no Glide NT path writes them
 *   VCR_POKE_R_AA_OFF  an AA / video-merge value (vcr_sli_poke_enables_aa)
 *                      while Diag\SliAA = 0 - EVEN WITH AllowPoke: the kill
 *                      switch covers Glide's single-chip AA path, which never
 *                      sends an SLI_AA_REQUEST (cfgAALfbCtrl CPU|DISPATCH|
 *                      READ_EN|base, the depth aperture, cfgVideoCtrl0 =
 *                      EN|LOCALMUX|DIV2)
 *   VCR_POKE_R_SLAVE   such a value into a SLAVE the kernel has no live
 *                      session on, without AllowPoke: single-chip AA programs
 *                      the master; upstream Glide writes every chip it
 *                      counts, which on the 6000 un-tristates three slaves'
 *                      syncs behind the kernel's back
 * Everything else goes through as it did before the safety net: zeros (the
 * close reset), READ_EN toggles, cfgSliLfbCtrl, cfgSliAAMisc, 0x98-0xA8.
 * vcr_sli_cfg_owned: 1 when [off, off+size) overlaps an SLI/AA register (the
 *   poke memo's key; the 2026-09-27 guard refused ALL of them - see the test).
 * vcr_sli_poke_first: 1 the first time this (chip, dword, value) is refused
 *   since the memo was zeroed - the caller persists only those as phases, so
 *   a repeated refusal cannot flush the 64-slot phase history away; every
 *   refusal still goes to the flight recorder. */
#define VCR_SLI_OWNED_SLOTS 14      /* 0x40, 0x48, 0x80..0xac */
typedef struct vcr_sli_poke_memo {
    vcr_u32 val[VCR_SLI_MAX_CHIPS][VCR_SLI_OWNED_SLOTS];
    vcr_u32 seen[VCR_SLI_MAX_CHIPS];            /* bit per slot */
} vcr_sli_poke_memo;
int vcr_sli_cfg_owned(vcr_u32 off, vcr_u32 size);
int vcr_sli_poke_first(vcr_sli_poke_memo *m, vcr_u32 chip, vcr_u32 off, vcr_u32 size,
                       vcr_u32 val);

#define VCR_POKE_F_ALLOW    0x1u    /* Diag\AllowPoke = 1 */
#define VCR_POKE_F_AA       0x2u    /* Diag\SliAA = 1 */
#define VCR_POKE_R_BOUNDS   1
#define VCR_POKE_R_HEADER   2
#define VCR_POKE_R_SNOOP    3
#define VCR_POKE_R_AA_OFF   4
#define VCR_POKE_R_SLAVE    5
/* 1 when a config access of `size` (1, 2, 4) at `off` stays inside the
 * 256-byte header and is naturally aligned */
int vcr_cfg_access_ok(vcr_u32 off, vcr_u32 size);
/* 1 when the bytes written would set an AA / video-merge bit: cfgVideoCtrl0
 * anything but the two sync-tristate bits, cfgVideoCtrl1/2 or the depth
 * aperture non-zero, cfgAALfbCtrl anything but READ_EN */
int vcr_sli_poke_enables_aa(vcr_u32 off, vcr_u32 size, vcr_u32 val);
/* 0 = write it; else VCR_POKE_R_*. flags VCR_POKE_F_*; live_chips = the
 * kernel's live SLI/AA session (x->sli_chips, 0 = none) */
/* An IDLE slave - one outside a live kernel SLI session (chip >= live_chips) -
 * must keep its DAC syncs tristated: its video processor and DAC are off, and
 * a slave driving HSYNC/VSYNC fights the master's, so the monitor loses sync
 * (the screen goes dark while the PC runs on - .124, 2026-09-27, after Glide's
 * close wrote 0 to every chip's cfgVideoCtrl0). The value a PCI_OP write to a
 * slave's cfgVideoCtrl0 lands as: `val` with both tristate bits forced on while
 * the slave is idle; `val` unchanged for the master, a live slave or any other
 * register. Byte and word writes that cover bits 24-25 are adjusted too. */
vcr_u32 vcr_sli_poke_adjust(vcr_u32 chip, vcr_u32 off, vcr_u32 size, vcr_u32 val,
                            vcr_u32 live_chips);

int vcr_sli_poke_policy(vcr_u32 chip, vcr_u32 off, vcr_u32 size, vcr_u32 val, vcr_u32 flags,
                        vcr_u32 live_chips);

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
