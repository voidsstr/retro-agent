# `vcrctl fbshot --probe`: a frame capture that asks the display driver nothing (2026-10-04, `.124`)

## Why

`fbshot` asked the display driver for everything:
- `VCR_ESC_INFO`;
- the HWCEXT mapping of the registers and memBase1;
- the SLI/AA config reads.

Each of those is an `ExtEscape`, and win32k runs `DrvEscape` under its display
lock. Under Max Payne's Direct3D menu on 2026-09-29 the capture "never
finished". Its thread waits inside the kernel, so no user-mode timeout ends it
and the agent's EXEC waits it out.

`--probe` takes everything from `vcrprobe.sys` (`probe/vcrprobe.c`, unchanged,
md5 `babc0d26` - the copy already on the box) and calls no GDI at all, not even
`GetDC`:
- **the board**: the first 3dfx display chip the HAL lists. On `.124` that is
  `3:0.0`, device 0009, with BAR0 `d0000000` and BAR1 `c0000000` - the same as
  `vcrctl info`.
- **memory per chip**: `HardwareInformation.MemorySize` in vcr-kmd's own video
  key, found through `DEVICEMAP\VIDEO`. On `.124` it is 64 MB, so `limit` is
  `08000000`, the same as the default path. The probe path is therefore
  vcr-kmd only.
- **registers and memBase1**: `IOCTL_VCRPROBE_MEM`, dword reads, 4 KB a call.

The SLI/AA gate is not weakened. A VSA-100 counts as multi-chip, so the
master's `cfgSliLfbCtrl`/`cfgAALfbCtrl` are always read through the HAL, and
re-read during the read, as on the default path. An 8 bpp frame comes out
greyscale, because reading the CLUT writes `dacAddr`.

## Results

| scene | default path | `--probe` | same? |
|---|---|---|---|
| desktop, Starfield screensaver running (`fbab_desktop.json`) | 1.2 s | 1.4 s | 335 px differ: the stars moved between the two reads |
| desktop, still (`fbab_desktop2.json`) | 1.1 s | 1.4 s | **0 of 1,310,720 px differ** |
| Quake II attract demo, 4-chip SLI overlay, tile aperture (`fbab_quake2.json`) | 3.4 s, ok | 3.2 s, ok | same registers, method `aperture`, `sli_shift` 2, cfgSliLfbCtrl `1e0f0030` (READ_EN), gate ok. Both are game frames; the demo was running, so the pixels differ |
| Max Payne 1.05 Direct3D menu, 1280x960x32 (`fbab_maxpayne.json`) | **printed nothing in 25 s** (EXECW timeout) | completed in ~120 s at normal priority; ~38 s at high priority (`start /high`) | the probe's frame is the menu |

`fbshot_probe_contact.jpg` shows four of the frames. The full-size PNGs are on
the dev host, under
`~/.retro-fleet/evidence/v56k-gametune_1001-fbshot_probe-20261004/`.

The Max Payne row has the timeout output only. The first probe run finished its
read and wrote its BMP (box time 23:56:04) at the moment EXECW gave up on it.
The high-priority run's JSON went to the console `start` opened for it.

## Max Payne stopped responding during this session - the cause is NOT established

- Everything at normal priority crawled while the game ran:
  - the probe read took ~120 s, against 1.4 s on the desktop;
  - `wmic` did not answer within 40 s;
  - `taskkill` (WM_CLOSE) did not return within 60 s, and two `taskkill`
    processes were left waiting.

  At high priority the probe read took 38 s, and `taskkill /f` returned at
  once. Something held the CPU above normal priority.
- The two probe frames, taken minutes apart, are **pixel-identical**. The GDI
  `SCREENSHOT` taken after them differs only around the mouse cursor.
- **The user, at the box:** "max payne appear locked but num lock is working".
  Windows was alive and the game was not. They pressed the Windows key to get
  back to the desktop and opened Task Manager. That came after the game was
  already stuck.
- `taskkill /f` at high priority ended the game in about 3 s.
  `DISPLAYCFG get` then read 1280x1024x32@85, the desktop was back and no
  `vcrctl` was left running.

**Candidate causes.** None is proven, and the game may have frozen on its own:
1. **The default path's wait on the display lock** - the leading suspect. A
   normal-priority thread queued for the lock against a CPU-bound game is a
   priority inversion: when the game releases the lock, the starved thread
   owns it next. On 09-29 that same wait was the only capture tried, and WM_CLOSE
   still closed the game.
2. **The probe's ~1.2 million uncached reads of busy video memory.**
3. **Max Payne itself** (SafeDisc 2.51 under DAEMON Tools, on our D3D HAL).

Until a run that captures nothing separates these, do not point either path at
a Direct3D title in an unattended sweep. The agent's GDI `SCREENSHOT` already
photographs Max Payne correctly (`maxpayne_gdi_screenshot.png` on the host).

## The box afterwards

- `C:\vcr\vcrctl.exe` is the new build, md5 `185cdacd`. The previous build is
  kept as `vcrctl_0930.exe` (`465080c0`).
- `vcrprobe` is registered as a demand-start kernel service and is STOPPED.
  Start it with `sc start vcrprobe` before `fbshot --probe`, and stop it with
  `sc stop vcrprobe` afterwards.
- All test BMPs and `fbab.bat` are deleted.

Tests: `tests/native/test_vcr_kmd_fbshot.c` covers:
- reads cover any run exactly;
- the board match, and a VSA-100 never skipping the gate;
- the registry helpers.

`tests/python/test_vcr_kmd_integration.py` checks two more things:
- the probe branch calls no escape and no GDI;
- `main()` takes no DC for it.
