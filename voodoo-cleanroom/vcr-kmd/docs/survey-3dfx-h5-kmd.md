# What 3dfx's H5 W2K/XP kernel driver does — a behavioural map (guidance only)

Surveyed 2026-09-25 from `retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/`
(`MP/` = `Miniport/H5/`, `DD/` = `Displays/H5/`). **No code is copied from it
into `vcr-kmd`**; this file records *what* that driver does and in what order,
so our own driver can be written from open sources (Glide GPL `h3cinit`, X.org
tdfx, Linux tdfxfb) with the hardware sequence known. `[retro3dfx]` marks code
this project added to that tree later — it is not 3dfx's.

## 1. Find / initialize

- `DriverEntry` `MP/H3.C:130-267`: FindAdapter, Initialize, StartIO, ResetHw,
  GetChildDescriptor, Get/SetPowerState. `AdapterInterfaceType = PCIBus`.
- Identify (`MP/PCI.C:167-367`): `VideoPortGetAccessRanges` for 3 ranges, vid
  0x121A; accepts device 0x04-0x0F (V3 <= 5, Napalm >= 6); **function 0 only**
  (slaves never load the driver); reads full config via `HalGetBusData`
  (videoprt only gives the 64-byte header); AGP cap id 2 -> 66 MHz; if decode is
  off the card is a secondary and decode is enabled.
- BARs (`MP/H3.C:1544-1788`): BAR0 MMIO mapped in pieces — IO regs +0x000000,
  CMD/AGP +0x080000, 2D +0x100000, 3D +0x200000, YUV +0xC00000; BAR1 LFB mapped
  as 2 x per-chip memory (upper half = tiled aperture), write-combined; BAR2 I/O,
  256 bytes. Secondary: `vgaInit0 |= LEGACY_DECODE`.
- Memory size (`MP/H3.C:1982-2136`): chips from `dramInit0 & SGRAM_NUM_CHIPSETS`,
  part size `SGRAM_TYPE_H5`, SDRAM from `dramInit1 & MCTL_TYPE_SDRAM`. Per chip.
- FindAdapter order: ConfigurePCI -> DetectNumUnits -> registry -> map ranges ->
  read BIOS OEM table (`pciInit0, miscInit0/1, dramInit0/1, agpInit0, pllCtrl1,
  sgramMode`) -> POST if secondary -> slave config space -> map slaves -> i2c ->
  DMT mode table from registry -> validate modes.
- POST (secondary device, `MP/H3.C:2138-2246`): `pciInit0`; `vgaInit0 =
  EXTENSIONS|WAKEUP_SELECT|LEGACY_DECODE`; `dramInit1`, `dramInit0` (straps kept);
  `miscInit0`; `dramData = sgramMode`, `dramCommand = 0x10D` (0x10E SGRAM);
  `miscInit1`, `agpInit`, `pllCtrl1`; `lfbMemoryConfig |= RAW`;
  `vidPixelBufThold = 0x10410`; VGA wake: IO+0xC3<-1, IO+0xC2<-0x76, IO+0xCE<-0x0506.
- HwInitialize (`MP/H3.C:2248-2459`): VGA-safe reset (`vidProcCfg=0`,
  `vgaInit1=0`, `vgaInit0` bit12 clear, `dacMode` bits0-4 clear, CRTC 0x1A/0x1B=0);
  optional `pllCtrl1` override; `pciInit0` LOWTHRESH; `vidPixelBufThold`;
  `miscInit1 |= CLUTINVERT`; POST each slave.

## 2. Multi-chip (V5 5500 / 6000)

- Slaves are **PCI functions 1-3 of the master's bus/device** (`MP/SLIAA.C:814-960`),
  read with raw 0xCF8/0xCFC config cycles.
- Slave config (`SLIAA.C:1033-1321`): master `cfgPciDecode` (cfg 72) sizes; each
  slave: `cfgInitEnable` (cfg 64) bit10 UPDATE_MEMBASE_LSBS, **driver writes the
  slave BARs itself** — BAR0 = master BAR0 + 32 MB x fn, BAR1 = master BAR1 + 2 x
  mem (same for all slaves), IO BAR = master's — slave `cfgPciDecode` incl. snoop
  sizes, Command |= MEM, clear bit10. (Those addresses were never reserved by PnP.)
- Shared I/O BAR is time-multiplexed by toggling Command.IO per chip.
- EnableSLIAA (`SLIAA.C:3481-3592`): per slave full mode set, copy
  `lfbMemoryConfig, vidDesktopStartAddr, vidProcCfg, vidOverlayDudxOffsetSrcWidth,
  vidDesktopOverlayStride` from master, then SETUP_SLI_AA: `pciInit0` waits,
  `tmuGbeInit` AA clock delay, `cfgInitEnable` swap algorithm/master, snoop
  (slaves: snoop membase from master BAR0/BAR1 >> 22, SNOOP_SLAVE,
  FBIINIT_WR_EN, SWAP_QUICK if > 2 chips), `cfgSliLfbCtrl` (cfg 140) + 3D
  `sliCtrl` (+0x20C) render/compare/scan masks, AA: `cfgAaLfbCtrl` (148),
  `cfgAaZbuffAperture` (144); `cfgSliAaMisc` (172) vsync offset on slaves;
  `cfgVideoCtrl0/1/2` (128/132/136) topology muxes; slaves `CFG_VIDPLL_SEL` +
  `miscInit1 |= POWERDOWN_DAC`. Disable path clears all of it, tristates slave
  syncs.
- The V5 6000's **external clock** (HiNT GPIO) is only in the Win9x driver
  (`H5/Win9x/DX/MINIVDD/GPIO.C:352`); the W2K code never programs it
  (`[retro3dfx]` added it).

## 3. Mode set

- Timings come from registry DMT strings (`MP/h3registry.c`), static
  `h3modetab.c` only as fallback. CRTC blob: [0-13] std CRTC, [14]/[15] H/V ext
  (CRTC 0x1A/0x1B), [16] MiscOut, [17] SR1, [18..19] `pllCtrl0`, [20] `dacMode`.
- PLL: `f = 14.31818 * (N+2) / ((M+2) * 2^K)` MHz. Napalm 2x mode (2 px/clock) when
  pixel clock > 262 MHz at width >= 1280, or htotal > 261 chars.
- H3SetMode (`MP/h3modeset.c:748-859`): unlock `vgaInit1` bits 21-28;
  desktop surface: `vidProcCfg` desktop pixfmt (8 PAL, 16 RGB565, 24, 32 RGB32),
  CLUT select for > 8 bpp, `vidDesktopStartAddr = 0`, desktop stride in
  `vidDesktopOverlayStride`; `miscInit0 = 0`, `lfbMemoryConfig = 0x3FFF`, overlay
  off; MiscOut (0x3C2) <- blob[16]|1; CRTC unlock (0x11), CRTC 0-7, 9-0x12,
  0x15, 0x16, 0x1A, 0x1B; `pllCtrl0`; `dacMode`; SR1/SR0; attribute ctlr;
  `vidScreenSize = H<<12 | W` (H<<13 doublescan); overlay coords; CLUT;
  `vgaInit0 |= bit12`; final `vidProcCfg`: VIDEO_PROCESSOR_EN | CURSOR_MICROSOFT
  | DESKTOP_EN, 2X from dacMode bit0, HALF for height < 400 at <= 16 bpp,
  **Napalm sets bits 28|29** (hot-environment erratum); sync `dramInit1` bit0
  with `vidProcCfg` bit0.
- After mode set the display driver puts the cmdFIFO at FB offset 0 and moves
  `vidDesktopStartAddr` past cmdFIFO + cursor (`DD/ENABLE.C:1232-1328`).
  Slaves' video processors stay off at desktop.

## 4. IOCTLs

Standard: MAP/UNMAP_VIDEO_MEMORY, QUERY/FREE_PUBLIC_ACCESS_RANGES,
QUERY_AVAIL_MODES, QUERY_NUM_AVAIL_MODES, QUERY_CURRENT_MODE, SET_CURRENT_MODE,
SET_COLOR_REGISTERS, RESET_DEVICE, SHARE/UNSHARE_VIDEO_MEMORY, power, child state.

Private, `CTL_CODE(0x23, fn, METHOD_BUFFERED, FILE_ANY_ACCESS)`:
0xfd3 QUERY_GLIDE_ACCESS_RANGES (map BARs into the calling process: [0] BAR0,
[1] BAR1 WC, [2] IO, then per chip 6 entries IOregs/CMDFIFO/2D/3D/FB/IO),
0xfd4 FREE_GLIDE_ACCESS_RANGES, 0xfd5/6 QUERY/SET_REGISTRY_VALUE, 0xfd7
WRITE_LOG_FILE, 0xfd8 IDENTITY_INFO, 0xfdc GET_BIOS_VERSION, 0xfd2/0xfd1
SLI_AA_ENABLE/DISABLE (14-dword SLI_AA_REQUEST), 0xfcf SLI_AA_INFO, 0xfce
PCI_OP {op, func, offset, value}, 0xfca GET_CURRENT_PROCESS_ID, 0xffd/0xffe
noncached alloc + map to process (Glide context dword).

## 5. HWCEXT (DrvEscape 0x3DF3 / 0xFD3; 0x3DF4 = cpu type)

Request `{contextID, which, optData}`, result `{resStatus, optData}`; processes
identified by PID from the miniport. Ops: 0 GETDRIVERVERSION (0xDEAD/0xCAFE), 1
ALLOCCONTEXT (protocolRev 1), 2 GETDEVICECONFIG (vid, did, fbRam per chip, rev,
strides, tileMark, isMaster, numChips), 3 GETLINEARADDR (IOCTL 0xfd3 on the app
thread -> user mappings; returns 3 bases), 4 ALLOCFIFO (obsolete), 5 EXECUTEFIFO,
6 QUERYCONTEXT (0), 7 RELEASECONTEXT (unmap on last ref), 8 HWCSETEXCLUSIVE, 9
HWCRLSEXCLUSIVE (re-set the desktop mode), 0xE GETAGPINFO (-1 by default), 0xF
VIDTIMING (-1), 0x10 FIFOINFO (FB), 0x11 LINEAR_MAP_OFFSET, 0x12
DOWNLOAD_GAMMA, 0x15/0x16/0x17 context dword share/unmap/NT, 0x18 PCI_OP (needs
exclusive), 0x19 GET_SLAVE_REGS (4 VAs per chip), 0x1B SLI_AA_REQUEST (needs
exclusive). Trap: a GLIDESTATE created before GETLINEARADDR returns stale bases
and a zero slave table.

## 6. Logging in that tree

3dfx: `VideoDebugPrint`/`EngDebugPrint` (checked builds only) and a 64 KB
buffered file log through IOCTL 0xfd7 -> `ZwWriteFile C:\3dfxvs.log`.
`[retro3dfx]`: always-on `V5DLog`, and the RLog ring in 1000-byte `REG_SZ`
chunks `RLog00..31` + `RLogSeq` under the Device0 key.

## 7. GDI surface

`DrvEnableSurface`: hardware enable (public ranges, MAP_VIDEO_MEMORY, mode set,
cmdFIFO, cursor), then **`EngCreateDeviceSurface` + `EngModifySurface(...,
MS_NOTSYSTEMMEMORY, pjScreen, lDelta)`** with HOOK_SYNCHRONIZE — a GDI-managed
surface over the WC LFB, `DrvSynchronize` waits for 2D idle. `DrvAssertMode(FALSE)`
disables the cmdFIFO and RESET_DEVICEs. Build: miniport links videoprt,
ntoskrnl, hal; display DLL has no .def (DDK GDI_DRIVER supplies the entry).
INF binds `PCI\VEN_121A&DEV_0009` subsys 0001-0005 (1 = 6000 AGP).
