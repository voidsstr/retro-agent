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
- No 2D engine, no command FIFO, no interrupts in this version — the usual
  ways a display driver wedges a machine are simply not in it yet.
- The desktop starts 1 MB into video memory: Glide keeps its command FIFO at
  96 KB, so a GDI write during a game can hit a texture, never the FIFO.

## Tools (reusable tests, run through the agent)

| tool | proves |
|---|---|
| `vcrctl.exe` (on the box) | `info`, `log`, `mark`, `snapshot`, `reg`, `crtc`, `pci`, `bootok` on our driver; `modes`, `setmode`, `gdi`, `hwc`, `hwcregs`, `golden`, `restore` on ANY driver |
| `tools/golden_capture.py` | register dumps from the vendor driver per mode (through its own HWCEXT mapping; with `--probe`, the VGA register file and PCI config of every chip) |
| `tools/golden_compare.py` | our mode math (the driver's own `vcr_modes.c`, host-built) against a capture, register by register |
| `tools/golden_timings.py` | timing-table rows decoded from a capture's CRTC - modes the monitor is known to accept |
| `probe/vcrprobe.sys` | a read-mostly NT driver loaded on the fly (`sc start vcrprobe`): VGA file, PCI config (incl. raw 0xCF8 cycles), physical memory read |
| `tools/deploy_box.py` | install / rollback / status on a real box: preflight (activation, kernel dumps, a complete rollback package), DRVUPDATE + read-back, safe-reboot, evidence; `--port 19910 --reboot-cmd ...` for the VM test bed |
| `tools/mode_sweep.py` | every mode: switch, current-mode read-back, GDI draw/read-back, no WARN/ERROR in the recorder |
| `tools/qemu/run-vcrkmd-vm.sh` | the VM test bed: build VM disk through a throwaway overlay, std-vga, debugcon captured, agent on 127.0.0.1:19910 |
| `tools/vcrlog.py`, `tools/vcrdump.py` | decode the recorder live / from a crash dump |
| `tools/vcrphases.py [--prev]` | the FLUSHED phase history - this boot's, or (`--prev`) the boot before, which DriverEntry keeps as `Prev*`: after a wedge and a power cycle, the step the box never got past |
| `tools/check_imports.py` | the binaries will load on XP SP3 |
| `tools/sli_golden.py` | the LIVE multi-chip state under N-chip SLI: every chip's PCI config (raw cycles), IO registers and 3D sliCtrl, the bridge's clock GPIO - Quake II looping on the all-ours lane, on whichever kernel driver is installed |
| `tools/sli_compare.py` | two such captures diffed register by register (volatile registers and the unreadable VGA alias excluded) |
| `tools/sli_golden_sweep.py` | a capture per SLI/AA config, one clean boot each (the vendor's rule), optionally diffed against a reference label |
| `tools/sli_shot.py` | Quake II photographs itself through Glide's SLI LFB read - every chip's bands, with the mean luma of each chip's rows |
| `out/glidelab.exe` (`tools/glidelab.c`, `make glidelab GLIDE_SDK=...`) | our Glide test program, no game in the way: `fill` (Mpixel/s, flat or blended), `bands` (every scanline an exact RGB565 value read back through the LFB, bad lines per owning chip), `cycle` (open/close N times: SLI set up and torn down), `abandon` (exit without closing, as a killed game does) |
| `tools/glidelab_run.py`, `tools/glidelab_sweep.py` | run one glidelab mode on a box / sweep fill + bands over SLI/AA configs (one boot each, or `--no-reboot` for ours), JSON lines in `evidence/glidelab/` |
| `tools/cursor_golden.py` | the hardware cursor's registers and 1 KB pattern, read back (a screenshot cannot show a hardware cursor), compared against the vendor's |

Host tests: `tests/native/test_vcr_kmd_{log,fmt,modes,abi,sli,ics307}.c`,
`tests/python/test_vcr_kmd_tools.py`, `tests/python/test_vcr_kmd_sli_glue.py`
(all in `tests/run_all.sh`).

## Status (2026-09-26)

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
ours offers what THIS monitor accepts. `Diag\Ddc`=0 / `Diag\EdidFilter`=0
switch it off.

## Findings (measured)

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
  list is filtered by the monitor. Ours is not yet (no DDC) — see roadmap.

## Roadmap

1. ~~**V5 desktop**~~ — done 2026-09-26: 123/123 vendor modes.
2. ~~**Glide single chip**~~ — done 2026-09-26: the whole stack ours.
3. ~~**Four-chip SLI**~~ — done 2026-09-26 (above). Next there: the AA
   configs (1, 3, 4, 6, 7, 8) against vendor goldens (`sli_golden_sweep.py`).
4. ~~**DDC/EDID**~~ — done 2026-09-26 (above). Benchmarks against the vendor
   still need the refresh pinned: our list is the monitor's, the vendor's is
   its own table.
5. **2D acceleration + hardware cursor + tiled desktop.**
6. **DirectDraw HAL**, then D3D.
