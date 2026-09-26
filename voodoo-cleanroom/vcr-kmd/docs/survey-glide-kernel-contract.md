# What our h5 Glide needs from the XP kernel driver (the contract)

Surveyed 2026-09-25 from `voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/`
(M = `minihwc/minihwc.c`, H = `minihwc/hwcext.h`, W = `minihwc/win_mode.c`,
D = `minihwc/dos_mode.c`, GS = `glide3/src/gsst.c`, GP = `glide3/src/gpci.c`,
C = `cinit/h3cinit.c`). Win32 build defines `HWC_EXT_INIT HWC_ACCESS_DDRAW
HWC_MINIVDD_HACK WINXP_ALT_TAB_FIX FX_GLIDE_NAPALM`; NOT
`ENABLE_V3_W2K_GLIDE_CHANGES` (no procID fields). Links only hwcio, gdebug,
minihwc, win_mode — cinit/pcilib/dos_mode/gpio are DOS/Linux only.

## Escapes

Codes 0x3df3, 0xfd3, 0x13df3 probed in that order with GETDEVICECONFIG; first
nonzero `ExtEscape` return wins. Also QUERYESCMODE 0x8001 (tolerated).
**Answer all three HWC codes identically.**

Request `hwcExtRequest_t` = 64 bytes: +0 contextID (garbage on fullscreen
path), +4 which, +8 union (56 bytes). Result `hwcExtResult_t` = 44 bytes: +0
resStatus (FxI32), +4 union (40). Success = ExtEscape > 0 AND resStatus == 1.

| which | request (+8..) | response (+4..) | |
|---|---|---|---|
| 0x02 GETDEVICECONFIG | dc@8, devNo@12 | devNum@4, vendorID@8 (**must be 0x121a**), deviceID@12 (6..15 = Napalm), fbRam@16 **bytes per chip** (8 MB forces H3), chipRev@20, pciStride@24, hwStride@28, tileMark@32, isMaster@36, numChips@40 (0..4) | fatal |
| 0x03 GETLINEARADDR | devNum@8, pHandle@12 = PID | numBaseAddrs@4 (ignored), base[0]@8 = user VA of memBase0 (**full 32 MB**), base[1]@12 = user VA of LFB (raw, 256 MB on the vintage driver) | fatal |
| 0x19 GET_SLAVE_REGS | DeviceId@8 = chip 1..n-1 | Regs[0..3]@4..16: [0] slave IO regs, [1] slave CMD/AGP, [2] unused, [3] slave 3D | fatal if numChips > 1 |
| 0x08 HWCSETEXCLUSIVE | devNum@8 | resStatus 1 | fatal |
| 0x09 HWCRLSEXCLUSIVE | devNum@8 | resStatus 1 (else the desktop is never restored) | close |
| 0x1B SLI_AA_REQUEST | dwChips@8 sliEn@12 aaEn@16 aaSampleHigh@20 (0/1/2) sliAaAnalog@24 sli_nlines@28 CfgSwapAlgorithm@32 TotalMemory@36 TileMark@40 TileCmpMark@44 aaSecColorBegin@48 aaSecDepthBegin@52 aaSecDepthEnd@56 Bpp@60 | logged only; **disable request leaves analog/swap/MemInfo as stack garbage** | needed multi-chip |
| 0x18 PCI_OP | DeviceId@8 (function 0-3) Operation@12 (0 rd/1 wr) Offset@16 Value@20 | Value@4 | tolerated |
| 0x17 CONTEXT_DWORD_NT | procId@8 cs@12 ds@16 | dwordOffset@4 = **user pointer** to a context-lost dword | optional |
| 0x16 UNMAP_MEMORY | procHandle@8 | logged | close |
| 0x0F VIDTIMING | user ptr@8 | ignored | optional |
| 0x0E GETAGPINFO | - | lAddr@4 pAddr@8 size@12 (only with FX_GLIDE_BUMP) | optional |

Never sent: GETDRIVERVERSION, HWCEXT2 (0x100+), I2C, DOWNLOAD_GAMMA,
RESTORE_DESKTOP, ALLOCFIFO, QUERYCONTEXT. Windowed-surface only: ALLOCCONTEXT,
RELEASECONTEXT, FIFOINFO, LINEAR_MAP_OFFSET, EXECUTEFIFO. **No driver-version
check anywhere.**

**Mapping validation (retro3dfx addition, M:1941-1990):** every base Glide uses
must satisfy VirtualQuery `MEM_COMMIT`, `MEM_MAPPED`, **AllocationBase == va**:
base0 >= 8 MB, base1 >= 1 MB, each slave Regs[0..3] >= 4 KB (incl. unused [2]).
Each VA must be the start of its own view — sub-offsets of one view are refused.
Glide dereferences base0 up to +0x1FFFFFF (IO +0, CMDAGP +0x80000, 2D
+0x100000, 3D +0x200000, tex +0x600000, 3D LFB +0x1000000). Napalm cmd FIFO at
rawLfb + fifoStart.

## Sequence

grGlideInit: hwcInit (EnumDisplayMonitors, GETDEVICECONFIG probe, QUERYESCMODE,
CreateDC, GETDEVICECONFIG(devNo)), checkResolutions (DDraw EnumDisplayModes),
hwcMapBoard (GETLINEARADDR, GET_SLAVE_REGS x3), first MMIO = read dramInit1.

grSstWinOpen: **DirectDraw SetCooperativeLevel(EXCLUSIVE|FULLSCREEN) +
SetDisplayMode(x, y, bpp, refresh)** (retry at default refresh; failure fatal)
-> SETEXCLUSIVE (fatal) -> master MMIO -> VIDTIMING -> SLI_AA_REQUEST (or
PCI_OP) -> CONTEXT_DWORD_NT -> gamma via MMIO -> hwcInitFifo (every chip idle
within ~2 s) -> sliCtrl through the FIFO.

Close: disable sliCtrl -> idle, cmdFifo0 off -> SLI disable request (or PCI_OP
clears) -> RLSEXCLUSIVE -> DDraw RestoreDisplayMode -> UNMAP_MEMORY. If the
context-lost dword is nonzero the teardown is SKIPPED — **the kernel must clean
up SLI and exclusive mode itself.**

bpp is 16/32, **but 8 when the Glide resolution equals the desktop's registry
resolution** (W:230-234) — **expose 8 bpp at every Glide resolution.**

## Who does what on Win32

Kernel/driver: CRTC, pixel PLL, refresh, DRAM/PCI init, VIDEO_PROCESSOR_EN,
**all SLI/AA config space and slave setup** (cfgInitEnable, cfgPciDecode,
cfgSliAAMisc, snoop, slave mode sets), the **V5 6000 external clock** (HiNT
bridge 0x3388:0x0021 config 0xC4 GPIO — DOS-only in Glide).

Glide itself, on the master via MMIO after SETEXCLUSIVE: overlay part of
vidProcCfg (cursor off; desktop off when no AA), vidOverlayDudxOffsetSrcWidth,
vidPixelBufThold, vidDesktopOverlayStride, overlay coords/dudx/dvdy, miscInit0
Y origin, lfbMemoryConfig, dramInit1 triple-buffer bit, vidMaxRGBDelta,
cmdFifo0 (at FB + 96 KB — Glide owns all local memory; re-init the desktop, 2D
FIFO and cursor on RLSEXCLUSIVE). After SLI enable it rewrites only master
vidScreenSize, lfbMemoryConfig, vidMaxRGBDelta and expects snooping to
broadcast. Per-chip sliCtrl through the FIFO with chip masks.

## Reusable open code (3DFX GLIDE Source Code General Public License)

- `dos_mode.c` `mapSlavePhysical` (D:227-323), `initSlave` (D:326-390),
  `buildVideoModeData`/`setVideoModeSlave` (D:432-589), `hwcSetSLIAAMode`
  enable (D:591-1482) / disable (D:1483-1512) — the complete SLI/AA sequence;
  its port I/O can be retargeted to MMIO (VGA regs aliased at IO regs
  0xB0-0xDF). `lin_mode.c` is a near copy.
- `h3cinit.c` `h3InitSetVideoMode` (C:822-1021, tables in `modetabl.h`, no PLL
  calc), `h3InitResetAll`, `h3InitVga`, `h3InitSgram`, `h3InitGetMemSize`.
  **`h4pll.h`/`h4oempll.h` are "UNPUBLISHED PROPRIETARY" — do not use.**
  `gpio.c` has no license header — write our own from the HiNT GPIO behaviour.
- Config offsets (`h3regs.h:461-511`): cfgInitEnable_FabID 0x40, cfgPciDecode
  0x48, cfgVideoCtrl0/1/2 0x80/0x84/0x88, cfgSliLfbCtrl 0x8C,
  cfgAADepthBufferAperture 0x90, cfgAALfbCtrl 0x94, cfgSliAAMisc 0xAC.
