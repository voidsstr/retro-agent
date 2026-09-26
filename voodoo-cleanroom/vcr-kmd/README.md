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
| `tools/deploy_box.py` | install / rollback / status on a real box: preflight (activation, kernel dumps, a complete rollback package), DRVUPDATE + read-back, safe-reboot, evidence |
| `tools/mode_sweep.py` | every mode: switch, current-mode read-back, GDI draw/read-back, no WARN/ERROR in the recorder |
| `tools/qemu/run-vcrkmd-vm.sh` | the VM test bed: build VM disk through a throwaway overlay, std-vga, debugcon captured, agent on 127.0.0.1:19910 |
| `tools/vcrlog.py`, `tools/vcrdump.py` | decode the recorder live / from a crash dump |
| `tools/check_imports.py` | the binaries will load on XP SP3 |

Host tests: `tests/native/test_vcr_kmd_{log,fmt,modes,abi}.c`,
`tests/python/test_vcr_kmd_tools.py` (all in `tests/run_all.sh`).

## Status (2026-09-25)

**Proven in the VM (QEMU std-vga, XP SP3):** installs through `DRVUPDATE`, PnP
starts the miniport, XP boots onto `vcrdd` — desktop, mode set with read-back,
**110/110 of our modes** (8/16/32 bpp, 320x200 to 1920x1440) switch and pass
the GDI read-back test with a clean recorder; the flight recorder, phases and
debugcon mirror all work. Glide's route to fullscreen (DirectDraw exclusive +
`SetDisplayMode`, no DirectDraw HAL needed) works at every Glide mode.
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

**Not yet on silicon:** the Voodoo mode set itself.

## Findings (measured)

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

1. **V5 desktop** — install on `.124` (rollback: `DRVUPDATE` the AmigaMerlin
   INF, or `Diag\Disable`), mode sweep, compare `vcrctl snapshot` against the
   golden capture.
2. **Glide single chip** — Quake II through our ICD + our Glide on our kernel
   driver: the whole stack ours.
3. **Four-chip SLI** — the SLI/AA setup Glide asks for (port of the Glide GPL
   `dos_mode.c` sequence), the V5 6000's external clock via the HiNT bridge.
4. **DDC/EDID** — monitor child + mode filtering (and EDID for GAMERES).
5. **2D acceleration + hardware cursor + tiled desktop.**
6. **DirectDraw HAL**, then D3D.
