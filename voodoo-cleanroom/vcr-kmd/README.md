# vcr-kmd — our own XP kernel-mode driver pair for 3dfx cards

The last piece of the stack that was not ours. With it, everything between a
game and the Voodoo is source we build: our MesaFX ICD, our h5 Glide, and now
the **miniport** (`vcrmp.sys`) and **GDI display driver** (`vcrdd.dll`) that
replace AmigaMerlin's `3dfxvs` on Windows XP. First target: the Voodoo 5 6000
on `.124`; the same code drives Banshee / Voodoo 3 / Voodoo 4 / 5.

It is built so that **when it fails, it says where** — and so that a failure
at boot costs a reboot, not a trip to the box.

## Layout

| path | what |
|---|---|
| `miniport/` | `vcrmp.sys`: PCI discovery, mode set, CLUT, user mappings for Glide, the flight recorder, the boot-safety counter |
| `display/` | `vcrdd.dll`: GDI primary surface over the LFB, `DrvEscape` (HWCEXT for Glide, `OPENGL_GETINFO` for XP's opengl32, the `VCR_ESC_*` harness escapes) |
| `common/` | code shared by kernel, DLL and host tests: log ring, formatter, mode math, mini CRT |
| `include/` | the contracts: registers, IOCTLs/escapes, HWCEXT ABI, event codes, log format |
| `inf/vcrkmd.inf` | install (DRVUPDATE) |
| `tools/` | the harness — see below |
| `golden/` | register captures from the vendor driver, the reference our math is checked against |
| `docs/` | surveys of the vendor driver, our Glide's contract, toolchain research |

Two hardware backends behind one interface: **VOODOO** (the real thing) and
**BOCHS** (QEMU std-vga, 1234:1111), which exists only to prove the whole
chassis — install, PnP, mode set, GDI, escapes, the recorder — in a VM before
any of it runs on silicon.

## Build

```bash
make -C voodoo-cleanroom/vcr-kmd            # out/vcrmp.sys out/vcrdd.dll out/vcrctl.exe
make -C voodoo-cleanroom/vcr-kmd imports    # every import checked against XP SP3
```

mingw-w64 i686, no DDK. Flags and the traps behind them are in
[`docs/research-xp-driver-toolchain.md`](docs/research-xp-driver-toolchain.md);
the one that matters most: **mingw's `libntoskrnl.a` offers ~700 functions XP
does not export (`memcmp` among them)** — such a driver links and then fails to
load at boot. `tools/check_imports.py` checks every import of both binaries
against the export tables of the real XP SP3 kernel files
(`tools/xp_exports/`), and the display DLL may import from `win32k.sys` only.

## The debug harness — "where did it fail"

| layer | survives | read it with |
|---|---|---|
| **flight recorder**: 128-byte entries in a non-paged ring (miniport + display DLL, one timeline) | nothing — unless the box bugchecks: it is in the kernel dump | `vcrctl log` live; `tools/vcrdump.py MEMORY.DMP` after a crash (scans for the magic, no WinDbg) |
| **phases**: boot and mode-set milestones, written to `HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag` and **flushed** | a hard hang and a power cycle | the agent's `REGREAD` (`LastPhase`, `PhaseLog`) |
| **debug port mirror** (`Diag\DebugPort` = 0xE9 QEMU debugcon, 0x3F8 COM1) | everything, in real time | the host file (`debugcon.log`) |

Event codes are one table, `include/vcr_events.h`; `tools/vcrlog.py` parses
that file, so names cannot drift.

## Safety — a display driver that fails at boot takes the box off the network

- `Diag\BootAttempts` is incremented and **flushed before the hardware is
  touched**, and cleared by a timer once the driver has run 60 s. Past
  `MaxBootAttempts` (3) the miniport declines the card and XP boots on its VGA
  driver — reachable, with the recorder saying why.
- `Diag\Disable = 1` declines the card outright (settable from Safe Mode, the
  agent, or remote registry).
- `HwResetHw` restores the VGA state the BIOS left, so a bugcheck screen and a
  reboot see a sane card.
- Every poll loop is bounded and logs what it saw; every mode-set register is
  read back and logged.
- The engines are driven, but never waited on without a bound: the 2D engine
  (GDI copies and fills, DirectDraw blits) and the Direct3D HAL's setup unit
  write straight to the chip's PCI FIFO (`display/vcrdd_2d.c`,
  `vcrdd_3d.c`), and every FIFO-room and idle wait is capped (`SPIN_CAP`,
  about a second of status reads); one that runs out logs the status it saw
  (512 a=9) and turns acceleration OFF for that PDEV - the software paths
  take over. A mode set that finds the 3D engine busy resets it
  (`vcrctl reset-engine` on demand).
- No command FIFO of our own and no interrupts: the CMDFIFO is Glide's alone
  (the Glide 3D reset, `Diag\Reset3D`, writes nothing to a chip whose
  `cmdFifo0` is on), and the miniport registers no interrupt handler. Glide's
  SLI/AA request is programmed by the kernel (`miniport/vcrmp_sli.c`), behind
  the refusals under "Diag switches".
- The desktop sits at the TOP of video memory, per mode (`vcr_desktop_offset`,
  the vendor's layout), with the hardware cursor one page under it: Glide's
  command FIFO (96 KB .. ~1116 KB on the VSA-100) never shares memory with
  it, so a GDI repaint during a game cannot write into the live command
  stream. (The first layout started the desktop 1 MB in, inside that FIFO,
  and hung the engine - Findings.)

## Diag switches — the controls for supervised runs

`HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag`, DWORD. Every switch here
is **off when absent**, and with all of them off the register traffic is what
ran on silicon on 2026-09-26: cfg 0/2/5, 2-chip SLI and cfg 3/7/8 write the
same bytes as 097b1f7, a 16 bpp D3D target is the proven sequence write for
write, and the flip deadline is the nominal rule to the tick
(`test_vcr_kmd_sli.c`, `test_vcr_kmd_d3dseq.c`, `test_vcr_kmd_flip.c` pin all
three). They exist for supervised runs on `.124`: arm one, run, disarm.

| value | read | what 1 does (default 0) |
|---|---|---|
| `SliAA` | at every SLI_AA_REQUEST | **allows AA.** At 0 an AA request is refused before any write, before a live SLI session is torn down (`VCR_SLI_EDENIED` -4, reason 10 AA_OFF, a persisted phase), and so is an AA value sent through HWCEXT `PCI_OP` (Glide's single-chip AA path), even with `AllowPoke` = 1. SLI-only requests and every disable are unaffected |
| `SliPersistAll` | at every request | every SLI step becomes a flushed phase, so after a wedge the last phase names the write. Slow: ~440 flushes of 15-30 ms for a 4-chip enable; only the last 64 steps are kept. **Only for hunting a wedge:** an AA enable then takes ~9.6 s instead of ~0.2 s, and for all of it the monitor shows the desktop through a half-programmed four-chip video path - the "garbled start-up screens" of 2026-09-30, a grid of copies of the game's splash (`evidence/glidelab/aa_garble_0930/`). Deployed as 0 |
| `SliAAVendorRecipe` | at every AA request | **absent = 1 = the vendor recipe, the DEFAULT since 2026-09-30**; 0 = the dos_mode.c arm, kept only for A/B. Why: the dos_mode.c arm leaves `cfgAALfbCtrl` READ_EN off on every chip (2x: 0x4C000000) and every in-game AA session froze the box hard at a random moment; the vendor recipe sets READ_EN on the master pair, as both 3dfx miniports do, and 2x, 4x and 8x ran clean (`evidence/glidelab/aa_vendor_0930/`; 8x needed READ_EN on chips 0/1 for its 2-samples-per-chip shape too - 0xDE3DC000 / 0xCE3DC000). The vendor recipe for that request: the cfgAALfbCtrl secondary base as a byte address masked to bits 4-25 (the default keeps `dos_mode.c:871`'s `<< 4`, spill included, as the control arm); base = tileMark + AA READ_EN + RD_DIVIDE_BY_4 for the one-sample-per-chip shapes (`{2,0,1,0,x}`, cfg 3, cfg 7); the whole tiled depth aperture for 4 chips without SLI at 4/8 samples (cfg 7, 8); 3D sliCtrl = 0 for AA without SLI. A tileMark / totalMemory it cannot place is refused before any write (reason 11 MEMINFO). One recipe per clean boot |
| `SliAAReadback` | at every AA request | after an AA enable that succeeds, reads every chip's 0x40, 0x48, 0x80-0x94 and 0xAC back by config cycles only into `Diag\SliAAState` (REG_BINARY, flushed; `vcrphases.py` decodes it, its header names the boot that wrote it). It costs ~100-200 ms between SET_DONE and Glide's next MMIO, which is why it is off. pciInit0 in the record is the value written: a config cycle cannot read it (0x4C is the status register) |
| `SliAAFifoGate` | at every AA request | **cfg 3 ghost arm (2026-09-28), off by default.** In the cfg 3 shape only ({4 chips, SLI, AA, 2-sample, analog}), every chip's AA-FIFO equation (cfgVideoCtrl2) becomes its fetch band - 0x20/0 on chips 0/1, 0x20/0x20 on chips 2/3 at 32-line bands - and chip 2's sum moves to the TRUE mux (cfgVideoCtrl0 0x1843 -> 0x1813). The vendor code drives the AA bus on every line in this shape alone; 3dfx's `Video SLI AA Configs.xls` gates it to the band. SET_BEGIN phase bit 9; `Diag\SliAAState.flags` 0x2 |
| `SliAAFeederLead` | at every AA request | **cfg 3 ghost arm, off by default.** Bit 0 = chip 1, bit 1 = chip 3: that feeder's cfgSliAAMisc vga_vsync_offset 47 px (chars 5) -> 39 px (chars 4), the value every slave runs in the clean cfg 5. Chars 3 (31 px, froze the board) is unreachable by construction. SET_BEGIN phase bits 10-11; `SliAAState.flags` 0x4/0x8 |
| `SliAALive` | written by the driver | **a record, not a switch (2026-09-30).** 1 from the moment an enable that includes AA succeeds (flushed before Glide's next MMIO), 0 when the session ends (`VcrSliOff`: Glide's disable, re-enable, mode set, display reset). A boot that finds it 1 follows a boot that died with AA live, and runs the AA AUTO-DISARM (`include/vcr_aaguard.h`, `VcrSliAABootGuard`, after `VcrMultiInit`): `SliAA` = 0, Glide's `SSTH3_SLI_AA_CONFIGURATION` in `Services\3dfxvs\Device0\glide` back to plain SLI (5 on 4 chips, 2 on 2, 0 on 1) only if it held an AA value (key opened, never created), `SliAAAutoOff` = the boot that died, event 705 a=3. Verified on `.124`: a simulated dead-in-AA boot came back at SliAA 0 / cfg 5 at 1.4 s; a real 2x AA session logged 705 a=1 then a=2 and left 0 |
| `SliAAAutoOff` | written by the driver | the boot count of the last boot that died with AA live (see `SliAALive`); read it to know why AA is off |
| `SliOffAaCtrl` | at every SLI request (cached; the off path reads no registry) | **prepared 2026-10-07, default OFF, not yet run on silicon.** At 1 the SLI/AA disable also writes the 3D `aaCtrl` = 0 on every chip, after every `sliCtrl` = 0 and before snooping goes off (step 508, `VCR_SLI_F_OFF_AACTRL`). A clean Glide close writes 0 itself. A killed or crashed AA client never closes, and a non-AA open never writes `aaCtrl` (Glide writes it only when `grPixelSample > 1`), so without this every chip carries AA_ENABLE and the jitter offsets into the next session; `Reset3D` clears chip 0 only. The supervised test is in `docs/v56k-benchmark-plan.md` (resume point 2026-10-07) |
| `AllowPoke` | at boot (FindAdapter) | config writes through `PCI_OP` / `VCR_ESC_PCI` that the kernel otherwise refuses: below 0x40 (as before), and now also cfgInitEnable (0x40), cfgPciDecode (0x48), and an AA value into a slave outside the live kernel session. A write is judged by what it would turn ON: zero writes, cfgSliLfbCtrl, READ_EN-only toggles, cfgSliAAMisc and 0x98-0xA8 always pass (Glide's cfg 0 close makes 24 zero writes to the SLI/AA registers); an offset past 0xFF or misaligned never does (a slave's raw 0xCF8 cycle keeps only bits 2-7, so 0x140 reached its cfgVideoCtrl0). A refusal is event 703 SLI_POKE_REFUSED, a = reason<<24 \| chip<<16 \| offset (1 BOUNDS, 2 HEADER, 3 SNOOP, 4 AA_OFF, 5 SLAVE) |
| `D3D32` | at boot | offers 32 bpp Direct3D targets on a VSA-100: DDBD_32, and the Z list D16 plus D24X8/D24S8 (see 32 bpp below). `vcr_info.flags` 0x10; event 513 what 16 when the display driver arms it |
| `D3DBigTex` | at boot | **the VSA-100 texture path (2026-09-28, NOT yet on silicon), bit by bit:** 1 = textures up to 2048x2048 (tLOD TBIG, LODs from 2048, texBaseAddr still at the 256-level with the bigger levels before it - h5 Glide `grTexSource`/`_grMipMapOffset`), 2 = DXT1/DXT3/DXT5 FOURCC textures (textureMode bit 31 + format 1/2/3; DirectDraw allocates the blocks we size; a DXT1 level narrower than 8 is never sampled - the TMU reads DXT1 in 8x4 units), 4 = A8R8G8B8 (format 15). Any bit also gives texBaseAddr its 26 bits (bits 24:4 + address bit 25 in bit 1): the Voodoo3 field the HAL writes otherwise samples a texture above 16 MB 16 MB lower, and `.124`'s heap is 27 MB. Off, every Banshee/Voodoo3, and 16 bpp textures to 256 are the proven path call for call; armed, the flush's texture-port write-back still fires only for a 16 bpp texture to 256 BELOW 16 MB (where the default path's range test always had it), and a mipmap chain is packed in the surface's own format (integration review 2026-09-28). `vcr_info.flags` 0x200/0x400/0x800; event 513 what 18 when armed (19 a compressed texture sized, 20 a chain placed). 7 = all three. Verify with `d3dprobe render --tests tex512,tex1024,tex2048,mip2048,tex8888,dxt1,dxt3,dxt5,texhigh` (below) |
| `Reset3D` | at boot | clears what a Glide session left on chip 0 - chipMask = ALL, sliCtrl = 0, 12 nopCMD, combineMode, aaCtrl, stencilMode, stencilOp = 0 - at the exclusive OWNER's own HWCRLSEXCLUSIVE after a good RESTORE_MODE (VSA-100, D3D on), and only if chip 0 reads idle three times and cmdFifo0 has SST_CMDFIFOEN clear. Never at DrvAssertMode, so a KILLED Glide client still needs a cold boot before a 32 bpp D3D test. Event 604 a=3, c: 0 not run, 1 reset, 2 not idle, 3 command FIFO on, 4 gave up. Flags 0x20 |
| `ClutRead` | at boot | **default OFF (absent = 0; 1 = on), 2026-09-28 - unproven on silicon, so off like every other new kernel behaviour of this lane; with it off `vcrctl` falls back to the AllowPoke path (refused unless `AllowPoke` = 1), so an 8 bpp `fbshot` comes out greyscale as before.** `IOCTL_VCR_REG` kind `VCR_REG_CLUT` (6): a READ of one CLUT entry (0-511) with no `AllowPoke` - dacAddr set, dacData read, dacAddr put back, all in StartIO so no kernel CLUT write can interleave, every loop bounded (`include/vcr_clutread.h`). Reachable only by a tool asking (`vcrctl fbshot` / `clut`); `vcr_info.flags` 0x2000 (positive: an older miniport never sets it). A failed read is event 305 with d = 1. Unproven on silicon when written |
| `FlipDeadline` | at every mode set | the achieved-refresh flip deadline (Flip completion, below). Voodoo backend only |
| `Accel2DText` | at every mode change (IOCTL_VCR_INFO per PDEV) | DrvTextOut on the 2D engine: one 1 bpp mask per clip rectangle, sent as a host-to-screen blit (`include/vcr_text.h`). 0 bad on the 86Box bed at 8/16/32 bpp, but SLOWER there than the software path (~115k vs ~139k glyphs/s, in-box driver ~275k), so off until measured on silicon. `vcr_info.flags` 0x40 (positive); `VCR_ESC_2D_STATS` gives the counters; evidence `evidence/86box_v3/2d_*` |
| `Accel2DPattern` | at every mode change | **default ON (absent = 1; 0 = off) since 2026-09-28.** 8x8 1 bpp brushes (hatches, the 50 % grey of drag rectangles) realized by DrvRealizeBrush and filled by the engine's mono-pattern rectangle fill - PATCOPY/PATINVERT opaque, PATCOPY transparent (`include/vcr_line.h`). 0 bad on `.124` at 16/32 bpp and on the 86Box bed at 8/16/32; on `.124` at 1280x1024x32 hatch fills 64x64 14.1k -> 92.6k/s, grey drag frames 2.0k -> 47.7k/s (gdilab plbench). `vcr_info.flags` 0x80; evidence `evidence/silicon/patline`, `evidence/86box_v3/patline` |
| `GdiGamma` | at every mode change | **default ON (absent = 1; 0 = refuse).** GDI's `SetDeviceGammaRamp` reaches `DrvIcmSetDeviceGammaRamp` (`GCAPS2_CHANGEGAMMARAMP` at 16/24/32 bpp) and loads colour-table bank 0, which the desktop and a Glide game's overlay both read (`include/vcr_gamma.h`). Until 2026-09-28 win32k refused every ramp: Jedi Academy logged "SetDeviceGammaRamp failed." and the id Tech 3 family ran on software gamma with overbright forced to 0 - the dark picture. Verified on `.124`: `tools/gammaprobe.c` (identity, gamma 1.3 and a doubled overbright ramp accepted, each recorded as `gamma ramp loaded`), and Jedi Academy's own ramps land with no failure line. A mode set still reloads the identity table. `vcr_info.flags` 0x4000 (NO_GDIGAMMA) |
| `DdHeapFloor` | at every mode change | the DirectDraw heap starts one page up instead of at video-memory offset 0 (`include/vcr_ddheap.h`). Offset 0 is what `HeapVidMemAllocAligned` answers for "no memory", so the first block of every heap is handed out at 0, read as a failure and lost until the next mode set - `ddlab vidmem` on `.124` made 120 of the 121 512 KB surfaces that fit, the one at 0 missing - and the D3D HAL's single-pass mipmap-chain allocator can get `DDERR_OUTOFVIDEOMEMORY` for it. The runtime's own second pass hides it from applications. Default OFF (unproven on silicon); with it on, `ddlab vidmem` on the same desktop should report 121, the lowest at +4 KB. `vcr_info.flags` 0x1000 |
| `Accel2DLine` | at every mode change | **default ON since 2026-09-28.** Solid cosmetic COPYPEN horizontal/vertical lines (DrvLineTo, and DrvStrokePath paths made only of them) as engine rectangle fills of exactly GDI's pixels; slanted/styled/XOR stay GDI's. 0 bad on `.124` and the bed; on `.124` 200 px h-lines 287k -> 520k/s, v-lines 81k -> 100k/s, 200x150 outlines 38k -> 61k/s. `vcr_info.flags` 0x100 |
| `CoreClock` | at every `IOCTL_VCR_CLOCK` SET/RESTORE | **a KILL switch, default ON (absent = 1; 0 = refuse), 2026-09-29.** It only permits an explicit request - the driver never writes the clock unasked, and nothing is persisted, so every boot is the VBIOS's clock. 0 answers `VCR_CLOCK_R_DISABLED`; a GET still reads every chip. See "The graphics clock, live" below |

**Arm:** `REGWRITE HKLM SYSTEM\CurrentControlSet\Services\vcrmp\Diag SliAA
REG_DWORD 1`, then `REGREAD` it back (the agent answers OK to a malformed
REGWRITE). `AllowPoke`, `D3D32`, `D3DBigTex`, `ClutRead` and `Reset3D` then need a reboot through
`scripts/fleet/safe-reboot.py`; `FlipDeadline` a mode set (a DirectDraw
application's own switch is one); the `Sli*` switches act on the next request,
so they can be armed for one run without a reboot. **Disarm** as soon as the run
ends: write 0, or `EXEC reg delete "HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag"
/v SliAA /f` and read it back absent. The agent's `REGDELETE` deletes KEYS,
never a value - never point it at `...\vcrmp\Diag`, which holds the phase
history.

**"At every mode change" means a NEW display surface (measured 2026-09-28,
QEMU bed):** a same-mode `ChangeDisplaySettingsEx(CDS_RESET)` is only
`DrvAssertMode(FALSE/TRUE)` on the same PDEV - no `DrvEnableSurface`, so the
`Accel2D*` switches are not re-read. Pass through another listed mode and
back (two paced switches), as the 3dfx Control Panel does
(`scripts/3dfx/3dfxctl`, which writes only `SliAA` and the three `Accel2D*`
switches, never a key; evidence in `scripts/3dfx/3dfxctl/evidence/`).

**Default changes with no switch (2026-09-27):** an SLI/AA shape with no
video-mux branch is refused before the first write (`VCR_SLI_EINVAL`, reason 9
COMBO); only a VSA-100 is given an SLI_AA_REQUEST; a `PCI_OP` offset past 0xFF
or misaligned is refused, reads included; a HWCRLSEXCLUSIVE from a process that
is not the owner, while there is one, is refused before RESTORE_MODE (604 a=4 -
any process can send that escape; the owner's own release and a release with
no owner are unchanged); `exclusive_pid` survives a failed RESTORE_MODE (604
a=2); a D3D target whose pitch is not a 16-byte multiple fails CreateDevice
(`DDERR_INVALIDPIXELFORMAT`) instead of getting a device that never drew;
GetScanLine never returns an unset line.

## Tools (reusable tests, run through the agent)

| tool | proves |
|---|---|
| `vcrctl.exe` (on the box) | `info` (also the display driver's exclusive owner, `exclusive_pid`), `log`, `mark`, `snapshot`, `reg`, `crtc`, `pci`, `bootok` on our driver; `modes`, `setmode`, `gdi`, `hwc`, `hwcregs`, `golden`, `restore`, `sliaa` on ANY driver; **`fbshot [path.bmp]`** (2026-09-28) writes the frame the video processor is SCANNING OUT, because a GDI screenshot of a fullscreen Glide/GL/D3D game on our driver photographs the old desktop memory instead. The desktop layer (verified: matches GDI), or - a fullscreen Glide game, desktop off - the OVERLAY: vidCurrOverlayStartAddr, tiled. **Above lfbMemoryConfig's tile begin page memBase1 is a linear APERTURE over the tiled memory, not raw memory**: Glide puts the begin page at its first colour buffer, lines 8 KB apart, and with cfgSliLfbCtrl READ_EN the chips answer their own SLI bands, so the whole 4-chip frame reads from the master (a buffer's aperture line is its chip-local line << log2(units), minihwc.c hwcBufferLfbAddr). Reading raw tile offsets through it was the first overlay build's column blocks. The start register is re-read each line (flips followed, counted); 8 bpp goes through the CLUT via the read-only kind (`ClutRead`), else the AllowPoke pokes; memBase1 is never read while multi-chip AA is live (the V5 6000 froze on an LFB read in cfg 3): on a multi-chip board the master's cfgSliLfbCtrl/cfgAALfbCtrl are read whatever the kernel's session count says, read AGAIN before line 0, at every flip and every 64 lines, and the read stops (`ok:false`, `stopped_at_line`) the moment an AA enable appears; raw reads under SLI (one chip's bands) are refused, and a frame with lines left black is `ok:false, partial:true` (integration review 2026-09-28). Decoding: `include/vcr_fbshot.h`, tests `tests/native/test_vcr_kmd_fbshot.c` (a model of the 4-chip aperture), `test_vcr_kmd_clutread.c`. **`fbshot [path] --probe` (2026-10-04)** reads the same frame through `vcrprobe.sys` alone (`sc start vcrprobe` first): no GDI, no escape, because every escape runs under win32k's display lock and a Direct3D game holding it parks the default path in the kernel (Max Payne: nothing in 25 s; `--probe` finished). The board comes from the HAL (the first 3dfx display chip, its BARs), memory a chip from vcr-kmd's own `HardwareInformation.MemorySize` (so vcr-kmd only), registers and memBase1 through `IOCTL_VCRPROBE_MEM`. A VSA-100 counts as multi-chip, so the SLI/AA gate always reads the master's registers. 8 bpp is greyscale (a CLUT read writes `dacAddr`). Verified on `.124`: the desktop pixel-identical to the default path, the Quake II 4-chip overlay the same plan. Max Payne stopped responding during that session, cause not established - no D3D title in an unattended sweep (`evidence/gametune_1001/fbshot_probe/`) |
| `vcrctl sliaa N SLI AA HIGH ANALOG [NLINES BPP TILEMARK COL DEPTHLO DEPTHHI] --i-am-at-the-box [--force-desktop-pll]` / `vcrctl sliaa off` | Glide's `HWCEXT_SLI_AA_REQUEST` as a kernel-only probe - no Glide open, no LFB. Sent as Glide sends it (GETDEVICECONFIG first, totalMemory in whole MB, tileMark = tileCmpMark), one paced switch in Glide's order: HWCSETEXCLUSIVE, then the request. Prints resStatus and, on our driver, `sli_result`/`sli_chips`/`clock_6k_hz`. Refused before anything is sent: any enable without `--i-am-at-the-box`, a shape with no video mux (the kernel's own `vcr_sli_combo_ok`), AA on a desktop not in 2x mode without `--force-desktop-pll`. An enable keeps exclusive, as Glide does, until `sliaa off` (Glide's disable, then HWCRLSEXCLUSIVE; it takes exclusive first, so it also clears a stale owner). With `Diag\SliAAVendorRecipe` on, pass the real tileMark or cfg 3/7/8 are refused (MEMINFO) |
| `vcrctl clock [get \| set <MHz> \| restore]` | the graphics clock through `VCR_ESC_CLOCK` (2026-09-29): every chip's pllCtrl1 word and MHz, as the VBIOS left it and now; `set` moves the MASTER in <= 5 MHz steps (read back), `restore` ends on the VBIOS's own word. Exit 2 with the driver's `why` on any refusal. The 3dfx Control Panel's Clock tab (`scripts/3dfx/3dfxctl`) is the same escape with a keep-or-revert |
| `tools/golden_capture.py` | register dumps from the vendor driver per mode (through its own HWCEXT mapping; with `--probe`, the VGA register file and PCI config of every chip) |
| `tools/golden_compare.py` | our mode math (the driver's own `vcr_modes.c`, host-built) against a capture, register by register |
| `tools/golden_timings.py` | timing-table rows decoded from a capture's CRTC - modes the monitor is known to accept |
| `probe/vcrprobe.sys` | a read-mostly NT driver loaded on the fly (`sc start vcrprobe`): VGA file, PCI config (incl. raw 0xCF8 cycles), physical memory read |
| `tools/deploy_box.py` | install / rollback / status on a real box: preflight (activation, kernel dumps, a complete rollback package), DRVUPDATE + read-back, safe-reboot, evidence; `--port 19910 --reboot-cmd ...` for the VM test bed |
| `tools/mode_sweep.py` | every mode: switch, current-mode read-back, GDI draw/read-back, no WARN/ERROR in the recorder |
| `tools/qemu/run-vcrkmd-vm.sh` | the VM test bed: build VM disk through a throwaway overlay, std-vga, debugcon captured, agent on 127.0.0.1:19910 |
| `tools/vcrlog.py`, `tools/vcrdump.py` | decode the recorder live / from a crash dump |
| `tools/vcrphases.py [--prev]` | the FLUSHED phase history - this boot's, or (`--prev`) the boot before, which DriverEntry keeps as `Prev*`: after a wedge and a power cycle, the step the box never got past. SET_DONE reads `chips N warn 0x.. (NAMES)`, a refusal `COMBO`/`AA_OFF shape {n,sli,aa,high,analog}` (a record from before 2026-09-27: "value not recorded"); `Diag\SliAAState` is decoded after the phases |
| `tools/check_imports.py` | the binaries will load on XP SP3 |
| `tools/sli_golden.py` | the LIVE multi-chip state under N-chip SLI: every chip's PCI config (raw cycles), IO registers and 3D sliCtrl, the bridge's clock GPIO - Quake II looping on the all-ours lane, on whichever kernel driver is installed |
| `tools/sli_compare.py` | two such captures diffed register by register (volatile registers and the unreadable VGA alias excluded) |
| `tools/sli_golden_sweep.py` | a capture per SLI/AA config, one clean boot each (the vendor's rule), optionally diffed against a reference label |
| `tools/sli_shot.py` | Quake II photographs itself through Glide's SLI LFB read - every chip's bands, with the mean luma of each chip's rows |
| `out/glidelab.exe` (`tools/glidelab.c`, `make glidelab GLIDE_SDK=...`) | our Glide test program, no game in the way: `fill` (Mpixel/s, flat or blended), `bands` (every scanline an exact RGB565 value read back through the LFB, bad lines per owning chip), `cycle` (open/close N times: SLI set up and torn down), `abandon` (exit without closing, as a killed game does), `edges` and `tbuffer` (2026-10-07, AA configs only: the left half drawn once at 50% grey on every sample, the right half one sample at a time through `grTBufferWriteMaskExt` - the halves match only when every sample reaches its own chip and buffer; for a person at the box, no LFB read); `--trace N` / `--trace-cfg` (our h5 Glide's step trace, below) |
| `tools/glidelab_run.py`, `tools/glidelab_sweep.py` | run one glidelab mode on a box / sweep fill + bands over SLI/AA configs (one boot each, or `--no-reboot` for ours), JSON lines in `evidence/glidelab/`; `glidelab_run.py --trace N` brings the trace home, `--collect [--restore-cfg 0\|2\|5]` reads what a wedged session left (below) |
| `tools/cursor_golden.py` | the hardware cursor's registers and 1 KB pattern, read back (a screenshot cannot show a hardware cursor), compared against the vendor's |
| `out/ddlab.exe` (`tools/ddlab.c`) + `tools/ddlab_run.py` | our DirectDraw test program: `caps` (HAL vs HEL, video memory), `flip` (a frame-numbered pattern written to the back buffer must read back from the FRONT after each flip; RESULT adds `first_frame_ms`, `max_frame_ms`, `min_frame_ms`, `slow_frames` (> 1.5 refreshes), `fast_frames` (< half a refresh), `flips_s_first_last`; `--work-us N` busy-works after every Flip - the D3D pattern), `blt` (copy, colour fill, an overlapping scroll and a SOURCE-COLOUR-KEYED copy between video-memory surfaces, read back), `zsurf --zbits 16|24|32` (one DirectDraw 7 Z surface - the request the HAL's CanCreateSurface judges; no mode switch), `sdlddraw [--src WxH] [--direct]` (DOSBox 0.74's `output=ddraw` through SDL 1.2.13's DirectX 5 backend, call for call - IDirectDraw2, SDL's refresh pick, the foreground wait, the lone primary, a `--src` blit surface with an explicit pitch and format, a per-frame Lock/write/Blt stretched onto the primary and read back; every step's HRESULT, and `dosbox`: "ddraw" or "surface-fallback:<step>"; `--direct` = the output=surface fallback) |
| `out/gdilab.exe` (`tools/gdilab.c`) | our GDI test program, self-checking against a per-pixel pattern: solid fills, BLACKNESS/WHITENESS, screen-to-screen copies (odd positions and sizes), overlapping scrolls in all four directions, a copy through a clip region with a hole, and the engine and the CPU interleaved on the same pixels |
| `out/d3dprobe.exe` (`tools/d3dprobe.c`) + `tools/d3dprobe_run.py` | our Direct3D 8 test program: `caps` (adapter, D3DCAPS8, formats, `zmatch` per target/depth pair, `hal_fullscreen` per format), `render` (clear, flat, gouraud, texture, modulate, blend, z-test, 256x256 texture - the back buffer LOCKED and compared with computed values, windowed or `--full`; `--zfmt d16|d24x8|d24s8` asks for that depth format, e.g. a device the HAL must refuse; `--noz` renders with no depth buffer; the `D3DBigTex` checks, explicit only: `tex512` `tex1024` `tex2048` (8x8 cells, the last one green), `mip2048` (levels 0-4 of a 2048 chain by texel:pixel ratio), `tex8888`, `dxt1` `dxt3` `dxt5` (128x128; logs LockRect's pitch), `texhigh` (~20 MB of textures first, so the probe lands above 16 MB) - each SKIPPED, not failed, where the HAL does not offer it; RESULT adds `skipped` and `max_texture`), `perf` |
| `tools/lab_run.py <lab> <host>` | runs any of the labs on a box or test bed and fails on any `bad*`/`fail` count |
| `make labs` | builds ddlab, d3dprobe, gdilab |
| `tools/86box/` | **the Voodoo3 test bed**: 86Box emulating a real Voodoo3 3000 - the driver's Voodoo paths, recoverable by script. [`tools/86box/README.md`](tools/86box/README.md) |

Host tests: `tests/native/test_vcr_kmd_*.c` (among them `sli`, `d3dseq`,
`flip`), `tests/python/test_vcr_kmd_*.py` (`sli_glue`, `d3d`, `ddraw`, `2d`,
`phases`, `tools`), `tests/python/test_glidelab_trace.py`, and the Glide side of
the AA guards, `tests/native/test_h5_sliaa_tuple.c` and
`tests/python/test_h5_sliaa_guards.py` (all in `tests/run_all.sh`).

**Tracing our h5 Glide (the fork's AA-TRACE).** `glidelab --trace N` sets
`FX_GLIDE_TRACE=N` - read from the process environment only; a registry value
does nothing - and the fork writes `<log>.trace`, each line flushed before the
hardware access it names. Level 1 is file writes only. Level 2 adds a bounded
grFinish after each FIFO step inside `grSstWinOpen`, so the last line names the
step the chips executed; it is not byte-identical to an untraced open, so a
wedge that vanishes at level 2 is timing-sensitive, and that is a result.
`--trace-cfg` adds the config-space dumps (`FX_GLIDE_TRACE_CFG=1`, 36 PCI_OP
escapes per dump on the V5 6000). An AA open also gets `FX_GLIDE_NO_SPLASH=1`
and `FX_GLIDE_NO_PLUGIN=1`: NO_SPLASH alone does not stop `3dfxspl3.dll`'s init
from drawing through Glide inside the open. glidelab works out the
configuration Glide will really open (`--cfg`, else env / HKCU / HKLM
`Services\{3dfxvs|banshee}\Device0\glide`) and refuses an AA open (rc 13, before
any window) on a Glide whose `GR_EXTENSION` lacks `RETRO3DFX_SLIAA_GUARD`.
`glidelab_run.py --trace N` deletes the old trace, downloads the new one beside
the step log (`evidence/glidelab/trace/`, or `--save-dir`) and fails the run on
a missing or header-less trace; its plan line gives the staged DLL's md5 and
markers, it refuses an AA `--cfg` on an unguarded DLL, and it puts the value
back afterwards. After a wedge and a power cycle, `glidelab_run.py HOST MODE
--collect` switches nothing: it downloads the step log and trace and reads
every Glide registry location, with a banner if an AA value is armed;
`--collect --restore-cfg 5` writes a safe value where one was found. A traced
`bands` run flushes every scanline (768 at 1024x768): raise `--timeout` above
180 s. **Registry trap:** `v56k_bench.apply_aa_config` writes
`SSTH3_SLI_AA_CONFIGURATION` under the display class key
(`Class\{4D36E968-...}\<inst>\Settings\Glide`), which our Glide never reads; it
reads `Services\3dfxvs\Device0\glide` when `Services\3dfxvs\Device0` exists,
else `Services\banshee\Device0\glide`. Only the in-process `--cfg` env reaches
it (unverified on the box; the same trap made every `sli_golden` capture run
Glide's default).

## The graphics clock, live (2026-09-29)

The user asked for 3dfx clock settings that apply in real time. Nothing in the
stack could move the clock before: Glide's `SSTH3_GRXCLOCK` path is compiled out
of the Windows build (`minihwc.c`, `HWC_ACCESS_DDRAW`) and the kernel never wrote
`pllCtrl1`. Now `IOCTL_VCR_CLOCK` / `VCR_ESC_CLOCK` (`include/vcr_ioctl.h`) read
and set it live; `miniport/vcrmp_clock.c` `VcrCoreClock` is the hardware half and
`include/vcr_clock.h` the rule, Win32-free (`tests/native/test_vcr_clock.c`).

- **What is clocked.** A VSA-100 has three PLLs: `pllCtrl0` the pixel clock,
  `pllCtrl1` the graphics core - and its SDRAM runs from it - and `pllCtrl2`.
  Measured on `.124`, chip 0 as the VBIOS left it: `pllCtrl1` 0xE721 = N 231
  M 8 K 1 = **166.8 MHz**, `pllCtrl0` 0x4005 = 157.5 MHz (1280x1024@85),
  `pllCtrl2` 0xBF01 (691 MHz by the same formula - not a memory clock). ONE clock
  moves core and memory together.
- **Only the MASTER is written.** At boot chips 1-3 read their **reset word
  0x0C01 (50.1 MHz) with reset DRAM timings** (dramInit0 0x58579d29 against the
  master's 0x607eadf9, tmuGbeInit 0xffb against 0xff0) - nothing initializes a
  slave until a game asks for SLI. Every SLI enable (`vcrmp_sli.c` `sli_enable` ->
  `init_slave`, as the vendor's `InitializeSlaveChipsInitRegs` and Glide's
  `initSlave` do) copies the master's `pllCtrl1` and DRAM timings into every
  slave, so the next game start carries a new clock to all four chips. The first
  build wrote all four, which would have jumped an uninitialized slave's PLL from
  50 to ~162 MHz for nothing.
- **Never under a running game.** The display driver turns a SET/RESTORE into a
  GET and answers `VCR_CLOCK_R_EXCLUSIVE` while a Glide program holds the board
  (`exclusive_pid`) - every 2D path already stands back then - so no chip ever
  renders at a clock the others do not have.
- **The rule.** K = 1 (VCO = 2f, the vendor table's 100-440 MHz VCO), M 1..10,
  N+2 <= 257, the lowest error; **120-219 MHz, refused outside, never clamped**
  (219 is the vendor's own "< 220"; the floor keeps well inside the refresh count
  0x18 the vendor ran from Banshee's 100 MHz to the Voodoo3 3500's 183 MHz).
- **How it moves.** In steps of at most 5 MHz. Each step waits (PASSIVE_LEVEL)
  until every chip reads idle three times, raises to DISPATCH_LEVEL for a last
  idle check and the write (`KfRaiseIrql`/`KfLowerIrql`, HAL - on a 1-CPU box no
  thread runs in between), gives the PLL 1 ms and reads the word back; anything
  unexpected stops the ramp there and says which `VCR_CLOCK_R_*`. RESTORE ends on
  the VBIOS's exact word, captured in `FindAdapter` after `VcrMultiInit` and
  never after our own first write (`core_changed`). Every step is recorder event
  704 `VCR_EV_CORE_CLOCK`.
- **Nothing persisted by the driver** - a reboot is always the VBIOS's clock. The
  3dfx Control Panel's "use this clock again after Windows restarts" re-applies
  one at logon, ONLY after a clean shutdown (Windows' `ShutdownTime` stamp must
  have moved since it was set; `scripts/3dfx/3dfxctl/DEPLOY.md`).
- **Verified on `.124` (2026-09-29):** 166.8 -> 150 MHz in 4 steps, 0 idle
  retries, master 0xF929 = 149.7 MHz read back, the desktop drawn normally at 150;
  RESTORE -> 0xE721 exactly; through the panel 175.0 MHz (0xDA1D) kept, and not
  kept -> put back by its countdown. Evidence `evidence/2026-09-29-clock-live/`,
  the panel's in `scripts/3dfx/3dfxctl/evidence/20260929/`. **The fill rate follows
  the clock exactly** (glidelab `fill`, 640x480, cfg 5 = 4-chip SLI, `evidence/
  2026-09-29-clock-live/fill.jsonl`): 1110.7 Mpix/s at stock, **997.1 at 149.7 MHz**,
  1110.7 again after RESTORE - 997.1 / 1110.7 = 0.8977 = 149.744 / 166.806. And after
  the SLI run at 149.7 all four chips read 0xF929: the slaves took the master's word
  at the SLI enable, as designed (before it they read 0xE721 from the previous
  game; after a reboot, 0x0C01).
- Tests: `tests/native/test_vcr_clock.c`, `tests/python/test_vcr_kmd_core_clock.py`.

## Status (2026-09-27)

**Proven in the VM (QEMU std-vga, XP SP3):** installs through `DRVUPDATE`, PnP
starts the miniport, XP boots onto `vcrdd`, and every mode we offer switches
and reads back as the current mode. *Correction (2026-09-26): the early VM
sweeps reported "110/110" and "200/200" GDI passes, but a `CDS_FULLSCREEN`
mode reverts when the process that set it exits, and those sweeps switched
in one process and ran the GDI test in the next - so GDI was only ever tested
at the desktop mode. `vcrctl modetest` now switches, draws and reads the
registers in ONE process.* The flight recorder, phases and debugcon mirror
work. Glide's route to fullscreen (DirectDraw exclusive + `SetDisplayMode`,
no DirectDraw HAL needed) works at every Glide mode.
**The safety net, proven by `tools/safety_test.py`:** `Diag\Disable` boots XP
on its VGA driver with `LastDecline` = 0x1xxxx; a boot counter left at its
limit makes the next boot decline the card (`LastDecline` 0x20004) and the box
comes back reachable; a forced bugcheck (0xE2) leaves a kernel dump from which
`vcrctl dump` reads the recorder on the box - including the marker written
before the crash and the miniport's own `HwResetHw` DURING the bugcheck.

**Proven on the V5 6000 (read-only):** `vcrctl` maps the card through
AmigaMerlin's HWCEXT exactly as our Glide checks it;
`golden/amigamerlin-3.1-r11_192.168.1.124.json` holds the vendor's IO
registers AND (via `vcrprobe.sys`) its full VGA register file for all 123
modes it offers on `.124`, plus PCI config of the four chips and the bridge.
**All 123 are byte-identical to what our driver computes** - every CRTC byte,
CR1A/CR1B, misc, PLL frequency, 2X, screen size (`golden_compare.py`; pinned
by `test_vcr_kmd_modes.c`, 13 modes byte for byte).

**ON THE V5 6000 (2026-09-26): our driver drives the desktop.** Installed with
`deploy_box.py` (AmigaMerlin rollback package kept on the box), two boots, both
past the stable mark. `mode_sweep.py --golden` on `.124`: **123/123 of the
modes the vendor offers pass** - the switch, GDI draw + read-back at 8/16/32
bpp, a recorder with no WARN/ERROR, and the registers READ BACK FROM THE CHIP
equal to the vendor's for that mode (`evidence/sweep_124_vs_vendor.json`).
The scanout runs (`vidCurrentLine` advances). First-boot finding: the open
drivers' PLL search picked N=174 M=0 K=3 for 157.5 MHz - a 1.26 GHz VCO where
the vendor runs 315 MHz - fixed by the vendor's selection rules (exact
`pllCtrl0` match in all 123 modes).

**THE WHOLE STACK IS OURS (2026-09-26): Quake II on our ICD + our h5 Glide +
our miniport + our display driver, 146.8 fps** at 640x480x16 on one VSA-100
(timedemo demo1, `v56k_bench.py --titles quake2:allours --configs 0`) - the
same cell measured 147.9 fps over AmigaMerlin's kernel driver (README §13.3):
0.7 %, run-to-run noise. Getting there took three hardware findings (below):
the master's power-up PCI decode, a desktop that overlapped Glide's command
FIFO, and a 3D engine left busy by a killed game (now reset automatically).

**Glide on one chip, our kernel vs AmigaMerlin's (Quake II single-pass, our ICD
+ Glide):** 640x480 147.9 / 147.9, 800x600 101.1 / 101.1, 1024x768 63.6 / 63.6
fps at 16 and 32 bpp - identical (`evidence/glide_q2_1chip_matrix_vcrkmd.csv`).
At 1600x1200 ours measured 22.9 against 25.4, both on one chip. *Correction
(2026-09-26): this was first put down to the vendor running four chips at
"config 0" - a capture artefact: `sli_golden.py` wrote the config only to the
registry, which our Glide does not read, so every capture ran Glide's default
(all chips in SLI). The refresh is the difference instead - see the 60 Hz rows
under FOUR CHIPS.*

**The vendor's 4-chip SLI state, captured live** (`golden/sli_*`, via
`tools/sli_golden.py`): slaves at BAR0 0xD2/D4/D6000000, BAR1 0xC4000000,
command 0x0002; cfgInitEnable master 0x06000B01 / slaves 0x4BA07B01;
cfgPciDecode slaves 0x0C011445; cfgVideoCtrl0 0x801 / 0x803; cfgVideoCtrl1 and
cfgSliLfbCtrl carry the chip index and a band size that follows the
resolution; cfgSliAAMisc 0x827 on slaves (the 39 px vsync offset); slaves'
DAC powered down; tmuGbeInit 0x00500FF0 everywhere. **The HiNT bridge GPIO
(0xC4) goes 0x00111101 -> 0x00222201: the vendor DOES program the V5 6000's
external clock for SLI.**

**FOUR CHIPS ON OUR KERNEL DRIVER (2026-09-26).** The miniport places the
three slaves exactly where the vendor does (BAR0 master + 32 MB x chip inside
the master's own 128 MB window, BAR1 master + 64 MB) at boot, maps their
registers, and tells Glide `numChips = 4`; Glide's `SLI_AA_REQUEST` runs our
port of the Glide GPL `dos_mode.c` sequence (`miniport/vcrmp_sli.c`) and
programs the V5 6000's external clock through the HiNT bridge GPIO
(`common/vcr_ics307.c`, `miniport/vcrmp_clock.c`). Every mode set turns SLI off
again - the teardown a Glide client that died skips.
- **Config space equals the vendor's under 4-chip SLI** - all four chips'
  cfgInitEnable, cfgPciDecode, cfgVideoCtrl0-2, cfgSliLfbCtrl, cfgAA*,
  cfgSliAAMisc, BARs, command, and the bridge's 0xC4 (0x00222201: the same
  clock word) - `sli_compare.py`, 640x480 and 1600x1200. Remaining IO-register
  differences are deliberate: pciInit0 bit 11 (the vendor turns the chips' I/O
  decode off; our VGA access needs the I/O BAR), the slaves' miscInit0 Y
  origin (dos_mode.c copies the master's; the vendor leaves 0), and the
  refresh (below).
- **Every chip draws its bands**: Quake II's own screenshot under SLI, read
  back through the SLI LFB path - intact, per-chip band luma 13.2 / 13.5 /
  13.8 / 13.9 (`evidence/sli_shots/`).
- **Quake II single-pass, 4 chips, our ICD + our Glide, our kernel vs
  AmigaMerlin's** (fps, 16-bit): 640x480 201.3 / 201.5, 800x600 191.4 / 193.4,
  and with the refresh pinned to 60 Hz 1024x768 174.5 / 176.8, 1280x960
  141.3 / 140.1, 1600x1200 98.2 / 98.6 (`evidence/glide_q2_4chip_cfg5_*`).
  Unpinned, ours read 4-5 % low at 1024 and above: our ICD asks for the
  highest refresh the driver lists, and ours lists every timing to 85 Hz
  (1600x1200 ran at 75 Hz, 195.8 MHz, where the vendor's EDID-filtered list
  stops at 70 and it ran 60) - the extra scanout bandwidth comes out of fill
  rate on every chip. DDC/EDID filtering is next for that reason as much as
  for the monitor's sake.

**THE MONITOR (2026-09-26).** At FindAdapter the miniport reads the EDID over
the chip's DDC pair (`vidSerialParallelPort` bits 18-22, through videoprt's
`VideoPortDDCMonitorHelper`) and builds the mode list inside the monitor's
declared ranges (`common/vcr_edid.c`); the EDID goes to XP as the monitor
child. On `.124`: the Sony CPD-G200 (H 30-96 kHz, V 48-120 Hz, 260 MHz), 210
-> 204 modes (1600x1200@85 at 106 kHz is gone), no New Hardware wizard, and
the agent's `GAMERES` sees the monitor again (native 1024x768@85). The vendor
list is a fixed 3dfx table (1600x1200 stops at 70 Hz whatever the monitor);
ours offers what THIS monitor accepts. The list is never unfiltered for want
of an EDID (monitor off at boot, a KVM, a bad checksum, `Diag\Ddc`=0, which
only skips the read). The limits in force come from, in order:

- **EDID** (`mon_src` 1) - the range descriptor read this boot;
- **SAME** (2) - an EDID without range limits gets the persisted range only if
  `Diag\MonId` says it is the same monitor; any other range-less EDID gets the
  default;
- **ENVELOPE** (3) - no EDID at all: `Diag\MonHminKhz`..`MonMaxPixclkKhz`,
  narrowed on every good EDID to the intersection of every monitor this box
  has seen, **intersected with the default**. The envelope only knows the
  tubes whose EDID was read, and a DDC-less tube or one behind a KVM is
  exactly the no-EDID case, never narrowed into it - so it may narrow the
  default (a 56 Hz panel floor), never widen it. `Diag\MonTrustEnvelope`=1
  (default 0) uses the bare envelope for an operator who knows which tube sits
  behind the KVM, and logs a WARN every boot it does;
- **DEFAULT** (4) - nothing usable known (or an empty envelope): H 30-48 kHz /
  V 50-75 Hz / 80 MHz, safe for any monitor - 640x480@60-75, 800x600@56-75 and
  1024x768@60 (48.4 kHz, in the +0.5 kHz slack), no 1024x768@70 and up, no
  1280x1024. (The first default, 70 kHz / 85 Hz / 135 MHz, claimed "any CRT of
  the era"; 14" and 15" CRTs of 1995-98 stop at 38-60 kHz.)

Each fallback is a WARN naming its source, and `vcrctl info` reports it as
`"mon_src":N` (0 = no filter: `Diag\EdidFilter`=0 or a virtual display) - the
host's mode gates trust only 1 and 2. `Diag\MonReset`=1 forgets the envelope
when a tube leaves the box for good (the driver sets it back to 0). Only
`Diag\EdidFilter`=0 lists every mode, and it logs a WARN saying so. A desktop
persisted in a mode the current limits no longer list (1280x1024 on a boot
with the monitor off) is not failed over to the VGA driver: the display DLL
takes the largest listed mode inside it (then any depth, then 640x480) and
logs the swap at WARN. And a bugcheck or shutdown in 4-chip SLI first gives
the master back its own video clock (cfgVideoCtrl0, raw config cycles legal
at any IRQL), so the HAL's text screen is not scanned from the SLI clock.

**DIRECTDRAW (2026-09-26, proven in the VM test bed; on the V5 6000 next).**
The display DLL carries a DirectDraw HAL (`display/vcrdd_ddraw.c`, built
against Microsoft's public DDK headers from a local copy - `Makefile`
`HAVE_DDI`, never committed): a heap in the video memory below the desktop,
flips by scan-out address (`IOCTL_VCR_DDFLIP`: `vidDesktopStartAddr` on the
Voodoo, the VBE Y offset in the VM), vertical blank and scan line
(`IOCTL_VCR_VBLANK`), process views (`SHARE_VIDEO_MEMORY`), and software blits
over video memory (copy / fill; anything else falls back to the HEL). The
primary became an opaque device surface whose GDI drawing is hooked and punted
to the DIB engine (`display/vcrdd_punt.c`) - the DDK samples' and VirtualBox's
shape, and where the 2D engine plugs in. VM results (`evidence/ddlab_vm/`):
HAL caps identical to XP's own Cirrus driver (0x04020040 / 0x250), flips at
8/16/32 bpp with every frame read back correctly from the front buffer, blits
correct at 8/16/32 bpp, and the GDI mode sweep 60/60 through the punt layer.
Before it: no HAL - surfaces in system memory and **the primary could not be
locked at all** (`DDERR_CANTLOCKSURFACE`), which alone breaks DirectDraw games
that draw on the screen. This is the chassis the fxD3D Direct3D HAL
(`scripts/3dfx/`) lives in next.

**THE AA SAFETY NET (2026-09-27; offline and on the 86Box bed, NOT yet on
silicon).** The three AA wedges of 2026-09-26 (`docs/v56k-benchmark-plan.md`)
are now refused on both sides before anything is written. Kernel: a shape
with no video-mux branch is refused (COMBO) - Glide's cfg 1 on the V5 6000,
`{4,0,1,0,1}`, used to cost ~430 writes reported as success; AA needs
`Diag\SliAA` = 1; `PCI_OP` writes are judged by what they would turn on; the
persisted SET_DONE keeps the warn mask, and refusals and NOMUX are phases.
Glide (fork `631221b` + `7736039` + `e767d89`, the SLIAA-GUARD `glide3x.dll`):
cfg 1 on more than 2 chips is refused; a request outside the kernel's shapes
fails the open before the mode set and again just before the escape; the
kernel's refusal is honoured and the display given back after a 3 s hold (the
`vcr_pace` floor); a READ lock in multi-chip AA is refused unless
`RETRO_GLIDE_AA_LFB_READ=1`; a busy multi-chip SLI/AA board gets no master
reset. **The kernel's refusal protects `.124` only together with that
`glide3x.dll`.** A Glide built from origin (`d161bd4`) ignores the escape's
FAIL and opens its multi-chip AA layout anyway, and the kernel cannot stop it:
the SLI request comes after HWCSETEXCLUSIVE, and the escapes after it are
unchecked. The fork commits are local (`glide-devel-sezero` ahead 4 of
origin): push them, rebuild with an explicit workdir (`bash
voodoo-cleanroom/build-stack.sh <repo>/voodoo-cleanroom/build` - without it
the script clones origin), and check the md5 of
`C:\Games\Quake2Complete\glide3x.dll` (what glidelab loads) before any deploy.
Otherwise confirm the AA configuration is 0/2/5 before any Glide app runs.

**32 bpp DIRECT3D ON THE VSA-100: code done, default OFF, untested on
silicon.** renderMode 32 bpp with a 24+8 aux buffer and Z scaled to 2^24-1
(2026-09-26), hardened 2026-09-27 (`4a9793b` + `46eef4c` + `967b4aa`) and put
behind `Diag\D3D32`, because `.124`'s desktop is 32 bpp and `5be6a59` had
armed it for every windowed client. A 16 bpp target is the proven sequence
write for write whatever the switch says (Voodoo3 6 writes, VSA-100 7). Only a
32 bpp target writes stencilMode = stencilOp = 0: with SST_STENCIL_ENABLE
clear the chip still does REPLACE under stencilMode's write mask, and
vcr-kmd had never written either. Stencil is not supported - D24S8 is listed
on D24X8's storage, and its stencil is never written or tested. The Z list is
D16 always, plus D24X8/D24S8 with the switch. A target and its Z must fit
(video memory, pitch, size), and video-memory offset 0 IS a surface: the
DirectDraw heap starts there, because the desktop sits at the top. Recorder:
513 what 12 target refused, 15 Z refused (b = 2 not in video memory, 3 pitch,
4 size), 17 target pitch refused. Unproven premise: that the chip ignores a
stale stencil enable at 16 bpp - plan step 16 tests it. On a Voodoo3 the D3D8
runtime refuses a 32 bpp device and a D24S8 one from the caps, before the
driver sees them; the driver's own CanCreateSurface guard (511/11) is reached
only the DirectDraw 7 way (`ddlab zsurf`).

**BIG, 32-BIT AND DXT TEXTURES ON THE VSA-100: code done, default OFF,
untested on silicon (2026-09-28, `Diag\D3DBigTex`).** UT2004's D3D8 device
opened on our HAL and died at `CreateTexture failed (D3DERR_INVALIDCALL)`: the
HAL said 256x256 and no FOURCC. Everything is from 3dfx's GPL h5 Glide: a
texture wider than 256 sets tLOD TBIG (bit 30) and counts its LODs from 2048,
and texBaseAddr still names the 256-level - the levels above it lie BEFORE the
base (`_grTexCalcBaseAddress` with `_grMipMapOffset`'s negative entries), so
base = the texture's address + the sizes of its levels wider than 256. S and T
still span 256 on the wide side (Glide's s_scale, MesaFX's `fxTexGetInfo`).
DXT data is the D3D block stream as is, but the TMU sizes DXT1 levels in 8x4
units (`_grMipMapOffsetCmp4Bit`): a DXT1 level narrower than 8 is not D3D's
layout (3dfx's own HAL pads it), so such a texture is refused and a chain is
sampled down to its last level 8 wide. A compressed texture's state is preceded
by a TMU nopCMD (Glide's workaround for a switch to compressed). The math is
host-tested against Glide's tables (`tests/native/test_vcr_kmd_texlod.c`),
including that the Banshee/Voodoo3 answers do not move. **Latent, switch
off:** the default path writes the Voodoo3's 24-bit texBaseAddr field; on a
64 MB VSA-100 a D3D texture placed above 16 MB (`.124`'s heap is 0 - 27 MB)
is sampled 16 MB lower. Any `D3DBigTex` bit writes all 26 bits; `d3dprobe
--tests texhigh` shows the difference on the card. Open: whether XP's D3D8
runtime reports a DXT LockRect pitch as the block row (d3dprobe logs it), and
whether the chip's texture-port write-back behaves for these textures (not
issued for them).

**FLIP COMPLETION (2026-09-27, `include/vcr_flip.h`).** A flip writes
`vidDesktopStartAddr`, and the chip latches it at the next vsync start.
`vcr_flip.h` (Win32-free; the display DLL's flip_done/Dd_Flip are its IOCTL
and QPC glue) calls a flip done once it has seen active display and then a
retrace (`status[6]` clear) after the write, or once a deadline has passed.
The retrace state is sampled AFTER the start-address write, so a write that
races into the retrace is reported a frame late, never early; a sample taken
before the write would be a frame early (the native test shows both). The
default deadline is the nominal frame + 1/8 (47376 ticks at 85 Hz, 67116 at
60 Hz, QPF 3579545), unchanged since the first silicon runs.
`Diag\FlipDeadline` = 1 makes the miniport send the mode's achieved refresh
(`vcr_modeset.refresh_mhz`: the PLL's clock over the timing table's totals)
in `vcr_dd_vblank.refresh_mhz` (the old `reserved` word; size and offset
unchanged), and the deadline becomes one achieved frame + 1/32. The sent rate
is trusted only within 1/64 of nominal, narrower than that margin, so the rule
stays never-early; every table timing computes within +1.12 %/-0.29 %. Nine 2X
modes scan a line 8 px shorter than the table (the halved total truncates to
whole characters), 0.31-0.39 % faster: the deadline is late there, never
early, and the host test pins that direction for every timing. In the model a
D3D-style application that works 1.05 refreshes per frame then flips at 0.95
of the refresh instead of 0.889. It stays OFF until counters from real titles
justify it; before setting it on a box, ddlab's `vblank_hz` at the target mode
must agree with `refresh_mhz`/1000 to well under 1 %. `vidCurrentLine` plays no
part in completion: 86Box reads 0x94 as 0x7ff, and on the VSA-100 it very
probably reads 0 through the blank.
- **Counters:** once per session - at the end of exclusive mode, at
  DrvAssertMode(FALSE) (before the device reset, so a session that changed the
  mode is not lost) or at DrvDisableDirectDraw - event 511 a=12 (b flips,
  c done by retrace, d done by deadline; the text adds superseded and pending)
  and a=13 (b longest vblank read, c longest wait, d the deadline, all in us).
  Nearly every flip by deadline means the rule is failing; a long single read,
  an IOCTL that blocked; a long wait with few deadline completions, a
  preempted thread. A session in the desktop's own mode (XP never calls
  SetExclusiveMode(0) at its release - seen on 86Box) is logged when its
  process's DirectDraw object goes away (DestroyDDLocal), once: the 511/12 text
  ends with where it was logged - "(exclusive end)", "(mode off)",
  "(DD disabled)" or "(DD local gone)" - and 511 a=14 (DEBUG) records each
  DestroyDDLocal with its pid. Before f146917 such sessions appeared merged at
  the next mode change ("flips 661" for a 60- and a 600-frame run).
- **GetScanLine** reports `vidCurrentLine & 0x7ff`. "In the blank" is
  `status[6]` alone, and `dwScanLine` is 0 on every path that is not DD_OK
  (ddlab read 2293576 of stack garbage). A line at or past the visible height
  is returned raw with DD_OK: 86Box's 0x7ff would read as the blank forever,
  and so would the lower half of a doublescan mode.

**The integration build on the 86Box Voodoo3 bed (2026-09-27;
`worktree-vk-int` 5e4ca36, `evidence/86box_v3/int_20260927/`).** Plan steps 5,
7 and 13 - all pass except the deadline A/B, which the bed cannot show:
- **install:** the guest's files = `out/`; DRIVER_ENTRY sizeof(VCR_EXT) 0x1388
  (was 0x1278); no new declined boot over three installs. `vcrctl info`
  reports `build 1` for both builds, so tell builds apart by file hash and
  struct size.
- **regression:** gdilab 0 bad; ddlab blt 0 bad, 2259 blts/s mean against 2298
  for `5be6a59` reinstalled in the same session (-1.7 %, inside the spread);
  ddlab flip 0 mismatch, scanline 0 (was 2293576); d3dprobe 42/0 windowed and
  40/0 fullscreen at 640x480x16 and 800x600x16.
- **flip:** no half rate at 16 or 32 bpp, on a 16 or a 32 bpp desktop, in
  either order: 58.7-64.0 flips/s at 60.35 Hz, 89-96 % done by retrace at 600
  frames. `FlipDeadline` = 1 moves the deadline 18749 -> 17086 us, but the
  emulator's ~3 ms of per-frame overhead hides the 0.889x/0.95x ratio: that
  A/B needs silicon. Open: completions outrun the refresh by 2-3 % at 600
  frames (`fast_frames` 22-29 under half a refresh), on `5be6a59` too.
- **D3D:** a 32 bpp device (windowed and fullscreen) and a 16 bpp device with
  D24S8 are refused by the D3D8 runtime, cleanly, with no mode set;
  `ddlab zsurf` 24- and 32-bit Z are refused by CanCreateSurface (511/11), the
  16-bit Z is created in video memory.
- **SLI/AA:** refused by the tool's gates, then EDENIED -4 (`SliAA` absent,
  reason 10) and EINVAL -1 (`SliAA` = 1: one chip, device 0005); zero SLI
  register writes in every run. `sliaa off` from a separate process takes a
  stale owner's exclusive and gives the desktop back, while a plain non-owner
  release is refused (604 a=4).

## Findings (measured)

- **Descent's "black" 640x400 is a DOSBox fallback, not a scan-out fault
  (2026-09-28, offline from the night's evidence).** `vcrctl fbshot` at
  640x400x32 read `start 03f06000, stride 0a00`: exactly the primary at the
  top of chip 0's 64 MB (0x4000000 - 640*400*4) at 640*4 bytes a line, and
  the frame there is DOSBox's blank 80x25 screen right after the autoexec's
  `cls` - 16 pixels, the cursor at row 0 (lines 13-14, 0xAAAAAA) - with GDI's
  capture identical (diff 0.0). DOSBox's `output=ddraw` with `aspect=true`
  and `fullresolution=original` never asks for 640x400: it sets 640x480 and
  stretches its 640x400 picture into it (`src/gui/sdlmain.cpp`
  GFX_SetupSurfaceScaled, scaley 1.2) - the 06:53 run did, its frame 320x200
  content with every column doubled and rows 2 or 3 times. Only GFX_SetSize's
  fallback ("Failed to create ddraw surface, back to normal surface.") sets
  width x height = 640x400, so in the 02:47 and 06:57 runs one DirectDraw
  call of SDL's DirectX 5 setup failed, and DOSBox then showed nothing past
  that `cls` for 70 s (the host's own DOSBox prints the DOS/4GW banner within
  2 s of it). Which call is not in the evidence (the recorder ring had
  wrapped); `ddlab sdlddraw` replays the sequence and names the step. The one
  way this HAL's heap differs from a stock driver's on that path - its first
  block at offset 0 - is `Diag\DdHeapFloor` (default off; A/B it with
  `ddlab vidmem` and `sdlddraw`).

- **Direct3D: our own HAL on the 86Box Voodoo3 (2026-09-26)** —
  `display/vcrdd_d3d.c` (DX7-level NT DDI: caps, contexts, CreateSurfaceEx
  handles, a bounds-checked DrawPrimitives2 walk of every DX7 opcode, D3D state
  → fbzColorPath/fbzMode/alphaMode/textureMode/tLOD) over `display/vcrdd_3d.c`
  (the triangle setup unit and fastfill, the one object built with the x87).
  **d3dprobe render 26/26 windowed and 26/26 fullscreen** - the matrix XP's
  in-box Voodoo3 driver passes on the same emulated card. The traps:
  - **XP does not move video memory between a flip chain's surfaces**; it
    re-targets rendering with DP2 SETRENDERTARGET by handle, and names a
    complex surface once, by its root, in CreateSurfaceEx - the driver walks
    the attach lists (a ring) to learn the back buffer's handle. Without the
    walk every other fullscreen frame went to the front buffer (18/26).
  - **a texture's DD_SURFACE_LOCAL has no DDRAWISURF_HASPIXELFORMAT** on NT,
    though ddpfSurface is filled; trust the format, not the flag.
  - **the TMU addresses a texture as if its LOD 0 (256 wide) came first**: a
    64x64's base is its address minus 160 KB (`common/vcr_texlod.c`).
  - **a mipmap chain is one block** the driver allocates from DirectDraw's own
    heap (`HeapVidMemAllocAligned` on the VIDEOMEMORY array it filled in
    DrvGetDirectDrawInfo): XP creates every level with its own CreateSurface,
    and the TMU walks the levels back to back. Without DDSCAPS_MIPMAP Unreal's
    D3DDrv stops at "Failed to preallocate initial textures, 4x4:
    DDERR_NOMIPMAPHW".
  - **Unreal Gold's D3DDrv renders on it** - the intro flyby, fullscreen:
    7,373 DrawPrimitives2, 1,096,654 triangles, 0 unparsed commands
    (`evidence/86box_v3/unreal_gold_d3d_vcrkmd_*`). Mip LEVEL SELECTION cannot
    be judged on 86Box: XP's in-box 3dfx driver samples level 0 at every size
    there too.
  - a windowed present is a clipped blit to the primary: done by the 2D
    engine per clip rectangle (d3dprobe `present` reads it back from the screen).

- **The 2D engine, on the 86Box Voodoo3 (2026-09-26).** `display/vcrdd_2d.c`
  drives it straight through the PCI FIFO (registers mapped for the display
  driver by `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES`): DirectDraw Blt (copy,
  colour fill, source colour key) and GDI screen-to-screen copies and solid
  fills, clip rectangle by clip rectangle. ddlab blt: 2349 blts/s, 153.9
  Mpix/s against the in-box driver's 77.6, 0 bad at 16 and 32 bpp; gdilab 0
  bad. Three things the labs caught on the way:
  - a RECTFILL's colour is the **source** operand (colorFore): PATCOPY (0xF0)
    fills from the pattern registers - ddlab bad_fill 4096/4096. Fills are
    SRCCOPY.
  - a source colour key picks the ROP from the **rop register** (byte 1 for a
    source match); it must say D (0xAA) or keyed pixels are copied anyway.
  - the sync before any CPU access waits for busy clear **and** the PCI FIFO
    back at its empty free count: an operation still queued has not started,
    and 86Box's status counts only started work - gdilab engine/CPU
    interleave 170 bad, then 0.
- **`status[6]` is CLEAR during vertical retrace** (Glide `grSstVRetraceOn`:
  `(status & SST_VRETRACE) == 0`). The miniport read it the other way, so
  `WaitForVerticalBlank` waited for the END of the blank and `GetScanLine`
  reported "in blank" during the visible frame - on silicon too. Found on the
  86Box bed.
- **A DirectDraw flip is latched at the next retrace**; until then Flip,
  GetFlipStatus and a Lock (or blit) of the buffer being taken off the screen
  answer `DDERR_WASSTILLDRAWING`. Before: 768 flips/s on a 60 Hz mode (no
  vsync at all); after: 62.3, the in-box driver 60.9. The completion rule and
  the integration build's flip counters on the bed are under Status (FLIP
  COMPLETION; the 86Box integration build).
- **`.124`'s 16 bpp flip half rate is NOT the completion rule** (flip
  analysis, 2026-09-27). flip_done's own deadline (1.125 nominal frames)
  guarantees ~77 flips/s for a 60-frame ddlab run at 85 Hz even if every
  retrace were missed; the 16 bpp runs measured 46.9 and 42.1, so 0.5-0.64 s
  went outside the rule. The same box, build and timing ran 87.4 / 86.3 at
  32 bpp, and the 16 bpp run is confounded (always the battery's first
  fullscreen flip run and its only depth change). A scanline rule was
  rejected: "in blank = vidCurrentLine >= vdisp" is always true on 86Box
  (0x7ff) and very probably never true on the VSA-100 (0 through the blank;
  32 golden readings, 4 zeros, none >= vdisp); a deadline from the scanline
  needs 33 bits of CRTC data in a 32-bit field and cannot tell line 0 from the
  blank. The rule's one unproven silicon assumption: `status[6]` does not rise
  before the start-address latch (if it covered the whole blank, done could
  come up to the front porch early - 1 line at 800x600@85, 10 at 640x480@60).

- **DirectDraw on XP is switched off, silently, by `DDCAPS_GDI`.** A HAL that
  claims it is probed at every PDEV (info twice, enable, ten GetDriverInfo
  queries) and disabled again, and applications get `DDCAPS_NOHARDWARE`. Found
  by bisection against VirtualBox's minimal HAL; XP's Cirrus driver reports
  BLT | READSCANLINE | BLTCOLORFILL. (`GCAPS_DIRECTDRAW` in the DEVINFO is
  needed too.)
- **The runtime passes Blt's raster op as `0x00CC0000`**, not GDI's SRCCOPY
  (`0x00CC0020`); compare the ROP byte. A HAL that claims `DDCAPS_BLT` and
  declines gets the application `E_NOTIMPL`, not a HEL fallback.
- `EngModifySurface(..., MS_NOTSYSTEMMEMORY, ...)` is refused unless the
  surface hooks something; a reported monitor child is polled with
  `IOCTL_VIDEO_GET_CHILD_STATE`.

- **METHOD_BUFFERED: an IOCTL's input and output are ONE buffer.** The first
  4-chip run zeroed the answer before reading Glide's request, so the enable
  arrived as dwChips = sliEn = aaEn = 0 - a disable - and Quake II ran on the
  master alone, successfully, with nothing reporting a failure. Copy the
  request out first (`test_vcr_kmd_sli_glue.py` holds every private IOCTL to it).
- **The slaves go INSIDE the master's windows.** PnP sized the master's BARs
  from the power-up decode (128 MB / 256 MB); once the decode is narrowed to
  32 MB / 64 MB the rest of those windows is where the slaves live - so they
  are routed by the bridge and videoprt maps them like any claimed range.
- **The vendor keeps each slave's own miscInit1 straps** (chips 2-3 bit 28
  set, chip 1 clear) and its slaves' vidPixelBufThold at 0x10410 whatever
  Glide writes on the master (0x20820 at 1600x1200) - dos_mode.c would copy
  the master's for both. pciInit0's PCI FIFO low threshold is 10 on the vendor
  (BIOS: 8); ours now matches.

- **The desktop must not sit in Glide's command FIFO.** On VSA-100 Glide
  puts its FIFO at 96 KB .. ~1116 KB (minihwc.c: a 96 KB pad plus
  MAXFIFOSIZE_16MB). Our desktop first started at 1 MB: the repaint after
  each game mode switch wrote into the live command stream and the engine
  hung (status 0xA5F). The desktop now sits at the TOP of memory, per mode -
  the vendor's layout (0x01B00000 at 1280x1024x32, its exact value).
- **The BIOS leaves the master in its power-up PCI decode** (membase0 128 MB,
  membase1 256 MB, cfgPciDecode 0x10); the vendor and Glide's GPL dos_mode.c
  narrow it to 32 MB / 64 MB / 256 B (0x45) before 3D. Our miniport does too.
- **A game killed mid-frame leaves the 3D engine busy** and every later Glide
  open fails. A mode set that finds the chip busy now resets the engine (the
  engine half of Glide cinit's h3InitResetAll: graphics core, FBI FIFO, 2D,
  command stream; video and memory timing untouched) - measured: 0xA5F ->
  0x5F, desktop intact. `vcrctl reset-engine` does it on demand.

- **The vendor programs sync start/end one unit EARLY** (CR04/05 and CR10/11
  one below textbook VGA) - tdfxfb's convention; X.org's is off by one here.
- **CR1A bit 5 is bit 6 of `(htotal - hdisp) + ((hdisp - 1) & 63)`** (the
  vendor's blank-end value), not of `htotal - 1` as both open drivers write it
  - they disagree with the vendor in 37 of 51 modes.
- **2X mode (VSA-100): above 262 MHz at width >= 1280, OR htotal > 261
  characters** (blank end is only 6 bits). The vendor's 1600x1200 is not DMT:
  DMT porches with the back porch cut to htotal 2088 = 261 chars (156.6 MHz at
  60 Hz), which keeps it in 1X.
- Misc is `0x0F | polarity` (no page bit), CR13 0x28, CR17 0x80; doublescan
  is offered and programmed identically at 32 bpp.
- **The V5 6000's slave chips are bus 3 dev 0 fn 1-3 and the HAL cannot see
  them**: function 0's header type has no multifunction bit, so
  `HalGetBusDataByOffset` reports fn 1-3 absent. Raw mechanism-#1 cycles find
  all four (slaves: memory decode on, I/O off, `cfgPciDecode` 0x00011445).
- The I/O BAR's alias of the VGA registers (0xC0B0-0xC0DF on `.124`) reads
  the same register file as the legacy ports.
- **The VSA-100's VGA registers are NOT readable through the MMIO alias** at
  IO-register offsets 0xB0-0xDF: CRTC reads return 0 and 0xCC returns the
  status byte. VGA state is reachable only through the I/O BAR (kernel).
- **`VideoPortGetBusData` cannot reach the slave chips**: for a PnP device it
  substitutes the adapter's own slot, and the V5's chips 1-3 are functions 1-3
  that PnP never enumerated (the header has no multifunction bit — `.124`
  shows exactly one `VEN_121A` instance). Use `HalGetBusDataByOffset`.
- The vendor desktop is **tiled** with a **hardware cursor**, placed at the top
  of video memory; ours is linear with a software cursor for now.
- `.124` offers 1600x1200 only up to 70 Hz through the vendor driver: its mode
  list is a fixed 3dfx table. Ours is filtered by the monitor's EDID range
  (DDC, 2026-09-26 - see THE MONITOR above), so it offers 1600x1200@75 on the
  Sony and refuses @85.

## Roadmap

1. ~~**V5 desktop**~~ — done 2026-09-26: 123/123 vendor modes.
2. ~~**Glide single chip**~~ — done 2026-09-26: the whole stack ours.
3. ~~**Four-chip SLI**~~ — done 2026-09-26 (above). **AA** (cfg 1, 3, 4, 6,
   7, 8): cfg 1/3/7 wedged `.124` on 2026-09-26; the safety net (kernel
   refusals + `Diag\SliAA`, the SLIAA-GUARD Glide, `vcrctl sliaa`, the Glide
   step trace, the flag-gated vendor recipe) is done offline and its refusals
   are proven on the 86Box bed. Next, and only with the user at the box: plan
   steps 15-20 in `docs/v56k-benchmark-plan.md` - the refusals on silicon, the
   kernel state alone (`vcrctl sliaa`), a clean-boot traced cfg 7, the cfg 3
   read-back, then vendor AA goldens (§4.1 of the plan).
4. ~~**DDC/EDID**~~ — done 2026-09-26 (above). Benchmarks against the vendor
   still need the refresh pinned: our list is the monitor's, the vendor's is
   its own table.
5. **2D acceleration** — copies and solid fills done on the 86Box Voodoo3
   (above); next: mono-expanding text (host-to-screen), patterns, lines.
   Hardware cursor (branch, untested on silicon); tiled desktop.
6. **DirectDraw HAL** — done in the VM and on the 86Box Voodoo3 (flip on
   vsync, engine blits). **Direct3D** — first light on the 86Box Voodoo3:
   d3dprobe 26/26 windowed and fullscreen. Next: games; mipmaps; fog;
   specular; lines/points; a second texture stage (Voodoo3 has two TMUs);
   32 bpp render targets on VSA-100 (code done 2026-09-26, hardened
   2026-09-27 and **off by default** behind `Diag\D3D32` - see Status; a
   Banshee/Voodoo3 refuses 32 bpp targets and 32-bit Z, `include/vcr_rtfmt.h`;
   86Box: no regression, `evidence/86box_v3/rt32/` and `int_20260927/`.
   **Verified on silicon 2026-09-28** (`.124`, V5 6000, plan step 16, one
   armed boot, `evidence/silicon/d3d32_step16/`): 513/16 armed; d3dprobe
   X8R8G8B8 + D24X8 fullscreen 640x480 40/40 and 1024x768 40/40, windowed
   on the 32 bpp desktop 42/42, 16 bpp fullscreen 40/40, `--noz` clear 3/3,
   perf 84.2 fps (the 85 Hz vsync); Hidden & Dangerous Deluxe, which forces
   32 bpp and failed "Unable to initialize graphics", starts. **Still OFF by
   default**: Rainbow Six raised its own error with it armed (confounded - a
   Thief II run had just stranded the desktop at 640x480), Thief II (NewDark,
   D3D9) still refuses, `Reset3D` after a Glide session is unproven, and the
   box is a LAN-party machine. Next: the R6 A/B on a clean desktop, then a
   default flip. UT2004's device opens and dies at CreateTexture: the HAL
   still advertises the Voodoo3's 256x256 limit and no FOURCC (the VSA-100
   does 2048 and DXTn/FXT1). The CMDFIFO instead of PCI-FIFO writes.
7. **Flip rate** - the completion rule is factored and pinned; the opt-in
   achieved-refresh deadline (`Diag\FlipDeadline`) needs its A/B on silicon,
   and `.124`'s 16 bpp half rate its per-frame counters (plan step 16).
