# FLEETRES.EXE — per-box resolution for the staged library

**Problem.** One staged tree deploys to eight machines with eight different
monitors. A resolution written into a staged config is therefore wrong
somewhere *by construction*. Measured panels are at the bottom of this file:
four boxes are on 1920x1080 16:9 LCDs, four are on 4:3/5:4 CRTs.

**Answer.** Keep the fix inside the staged game, but make it RUNTIME. Every
title's `Play <Game>.bat` runs a 54 KB helper staged beside it, which reads
*this* box's panel and writes the game's own config before the game starts.

## Build
```
i686-w64-mingw32-gcc -O2 -s -o FLEETRES.EXE fleetres.c -ladvapi32 -luser32 -lm
```
59,392 bytes. sha256 2266d6737dc76909344eb908a6d8eb444d5181ee23b71df013e8fd5d42598493
Runs on XP SP3 and Windows 7 (both verified on the fleet). Pure Win32 — no CRT
redist, no SSE, so it is safe on the pre-SSE2 boxes (.124/.133/.143).

## Modes
| invocation | what it does |
|---|---|
| `FLEETRES.EXE -cmd` | prints `set FR_*=...` lines; `CALL` the output |
| `FLEETRES.EXE -info` | human-readable panel report |
| `FLEETRES.EXE -ini <file> <section> <key> <value>` | WritePrivateProfileString — for UE1/UE2 `.ini`, DOSBox `.conf`, `SUN.INI`, `RA2.INI` |
| `FLEETRES.EXE -setline <file> <key> <line...>` | replaces the line whose first token is `<key>` (append if absent) — for the configs that are not INI-shaped: Dark engine `CAM.CFG`, LithTech `autoexec.cfg`, Quake-family `.cfg`. A backtick in the replacement becomes a double quote, because cmd.exe eats real ones and LithTech's format needs them. |
| `FLEETRES.EXE -kv <file> <key> <value>` | replaces/appends `key=value`, splitting at the `=`. For a config that is bare key=value with **no `[section]` header**, which neither of the two above can address — DXX-Rebirth's `DESCENT.CFG`. A line that merely *contains* an `=` (a comment) is never matched. |
| `FLEETRES.EXE -reg <HKLM\|HKCU> <subkey> <value> <dword\|sz> <data>` | writes one registry value. For the engines that keep the mode **only** in the registry — Max Payne (`HKCU\Software\Remedy Entertainment\Max Payne\Video Settings`), Red Faction (`HKLM\SOFTWARE\Volition\Red Faction`). Used rather than `reg.exe`, which does not exist on Win9x. |

## Variables from `-cmd`
| var | meaning |
|---|---|
| `FR_W` / `FR_H` | **the resolution to use** for an engine that can do widescreen |
| `FR_W43` / `FR_H43` | the resolution to use for an engine that is 4:3-only |
| `FR_Q2MODE` | id Tech 2 `gl_mode` index matching `FR_W43`/`FR_H43` |
| `FR_Q3MODE` | id Tech 3 `r_mode` index — **a different table.** id Tech 2's mode 8 is 1280x960 (4:3); id Tech 3's is 1280x1024 (5:4). Handing `FR_Q2MODE` to a Quake III-family engine asks a 16:9 panel for a squashed picture. `FR_Q3MODE` skips index 8 and index 11 (856x480). |
| `FR_FOV` | horizontal FOV that preserves the 4:3 vertical FOV (90 at 4:3, 106 at 16:9) |
| `FR_ASPECT` `FR_WIDE` | `16:9`/`4:3`/`5:4`; `FR_WIDE=1` on a widescreen panel |
| `FR_PANEL` | `LCD` or `CRT` |
| `FR_DOSFULLRES` | `desktop` on an LCD, `original` on a CRT — for DOSBox `[sdl] fullresolution` |
| `FR_NATIVE_W/H` `FR_DESK_W/H` `FR_LIVE_W/H` | panel native; persisted desktop; live desktop |
| `FR_MON` | monitor name from EDID |
| `FR_HZ` | refresh of the **persisted** desktop mode, for an engine whose mode switch takes one (Halo's `-vidmode w,h,hz`). A hardcoded 60 is a staged constant like any other and is wrong on every CRT box — .143 runs 100 Hz, .124 75 Hz. |
| `FR_GLIDE` | `1` when the box has REAL 3dfx silicon (`VEN_121A` anywhere in the PCI enum). A Voodoo 2 is `Class=MEDIA` and never appears as a display adapter, so nothing else on the box can see it. |
| `FR_GLIDEDEV` | the matching PCI instance, for the log |
| `FR_UE1DEV` | the UE1 render device to write: `GlideDrv.GlideRenderDevice` when the box has *and opts into* Glide, else `D3DDrv.D3DRenderDevice` |

## Why the render device is here too
Same problem, same shape: one staged tree, eight boxes, and a constant that is
wrong somewhere by construction. Two titles (`UnrealGold/System/`,
`Carmageddon2/`) ship a game-local nGlide `glide2x.dll` (1,310,720 B). Game-local
wins at load time, so on the only two boxes that still have Glide silicon that
wrapper **shadows the real system32 `glide2x.dll`** and the game gets neither the
card nor a working wrapper — `grSstOpen` fails and UE1 drops to the software
rasterizer at 100% CPU. That cost a whole session on `.171`, where it presented
as "UnrealGold crashes" and was never a crash.

The wrapper is **not deleted** — six boxes have no other Glide path. The
launcher moves it aside when `FR_GLIDE=1` and moves it **back** when it is 0.

`FR_GLIDE` (silicon present) and `FR_UE1DEV` (render through it) are deliberately
two different questions: `.143` has a Voodoo5 5500 fitted but its monitor is on a
GeForce 6800, so rendering through Glide would draw to a port nobody is looking
at. Rendering on the 3dfx card is therefore an explicit per-box opt-in,
`HKLM\Software\RetroAgent\GlideRender` (REG_DWORD 1) — set on `.171` only,
whose Voodoo 2 is the box's only real 3D.

## Do NOT paste a launcher block. Call the staged one.

> **This section used to print a block to copy into every `Play <Game>.bat`,
> and that was a mistake that reached a box.** Halo was staged in 2026-08-30
> with the pre-`FLEETRES.BAT` version of it hand-pasted in, so the title
> shipped `FLEETRES.EXE` with no `FLEETRES.BAT`, its `%FR_*%` all fell through
> to the fallbacks, and `validate-staged-library.py` failed the whole library.
> A block that is copied is a block that goes stale in 27 places at once.

Each title carries **`FLEETRES.BAT`**, staged beside `FLEETRES.EXE`, and its
launchers do exactly this:

```bat
call "%~dp0FLEETRES.BAT"
rem  ... or, for an engine with a ceiling:
call "%~dp0FLEETRES.BAT" -cap 1280 960
```

Both files, and the engine-specific lines after the call, are written by
`scripts/fleet/stage-fleetres.py`. **Add the title there and re-run it** — do
not hand-edit a staged launcher, and do not copy another title's block. The
tool is idempotent, every substitution must match, and an unmatched one aborts
the run rather than shipping a launcher that silently did not get its
arguments.

```
python3 scripts/fleet/stage-fleetres.py            # apply
python3 scripts/fleet/stage-fleetres.py --check    # exit 1 if not applied
```

## Per-box override, for hardware that cannot drive its monitor
`HKLM\Software\RetroAgent\ResCapW` / `ResCapH` (REG_DWORD) cap the answer on
one machine without touching the staged tree. Intended for **.171**, whose 3D
is a Voodoo 2 (hard 800x600 ceiling) hiding behind an Intel 865G.

## How it decides
1. Current mode: `EnumDisplaySettings(ENUM_CURRENT_SETTINGS)`.
   **Not `wmic`** — measured 2026-08-29, XP's
   `Win32_VideoController.CurrentHorizontalResolution` reported 640x480 on
   .123 while the box was really at 1024x768. It is not a usable source.
2. Persisted mode: `EnumDisplaySettings(ENUM_REGISTRY_SETTINGS)`. The target is
   derived from **this**, never from the live mode — a game that exits without
   restoring leaves the desktop at 640x480, and .123 and .240 were both sitting
   at 640x480 during this survey. A launcher trusting the live mode would then
   pin every later game to 640x480 permanently.
3. Panel: `EnumDisplayDevices` for the monitor's PnP id, then the EDID blob at
   `HKLM\SYSTEM\CurrentControlSet\Enum\DISPLAY\<pnp>\<inst>\Device Parameters\EDID`
   (the instance carrying a `Control` subkey is the live one). Preferred
   detailed timing = native mode; physical size = tube aspect.
4. LCD vs CRT: digital-input bit **OR** (EDID vertical-refresh max <= 76 Hz AND
   preferred timing <= 61 Hz). Correct on all eight fleet panels — every CRT
   here quotes 85-180 Hz, every LCD quotes <= 76 and 60.
5. Target: **LCD -> the panel's native mode** (anything else is resampled by
   the panel's scaler and looks soft, and a 4:3 mode is additionally stretched
   or pillarboxed). **CRT -> the largest mode matching the TUBE's aspect that
   does not exceed the persisted desktop mode.** A CRT has no pixel grid, so
   sharpness is not the issue — geometry is: 1280x1024 (5:4) on a 4:3 tube
   squashes everything vertically, which is what .133 and .171 were doing.

## Measured fleet panels (2026-08-29)
| box | monitor | panel | native | aspect | desktop (persisted) | running native? | FLEETRES target | 4:3 target |
|---|---|---|---|---|---|---|---|---|
| .123 | DELL P2312H | **LCD** | 1920x1080 | 16:9 | 1920x1080 | yes | **1920x1080** | 1280x960 |
| .124 | Sony CPD-G200 | CRT | 1024x768@85 | 4:3 | 1024x768 | n/a | 1024x768 | 1024x768 |
| .133 | ViewSonic G790 | CRT | (pref 1280x1024@85) | 4:3 tube | 1280x1024 | **5:4 on a 4:3 tube** | **1280x960** | 1280x960 |
| .143 | (no EDID; Default_Monitor) | CRT | unknown | — | 1024x768 | n/a | 1024x768 | 1024x768 |
| .145 | DELL E2414H | **LCD** | 1920x1080 | 16:9 | 1920x1080 | yes | **1920x1080** | 1280x960 |
| .171 | Gateway VX1120 | CRT | 1920x1440@75 | 4:3 | 1280x1024 | **5:4 on a 4:3 tube** | **1152x864** | 1152x864 |
| .240 | DELL E2313H | **LCD** | 1920x1080 | 16:9 | 1920x1080 | yes | **1920x1080** | 1280x960 |
| .246 | HP 2511 (digital) | **LCD** | 1920x1080 | 16:9 | 1920x1080 | yes | **1920x1080** | 1280x960 |

Every desktop is already correct. **The problem is entirely in the game
configs**, which are pinned at 1024x768 (or 640x480 for Tiberian Sun) across
the whole library.

.143's display driver answers `ENUM_CURRENT_SETTINGS` but returns FALSE from
the indexed mode enumeration, and its active monitor node is `Default_Monitor`
with no EDID — so on that one box FLEETRES has only the persisted desktop mode
to go on. It answers 1024x768, which is right, but it is an inference, not a
measurement. Plugging the monitor into a port that gives DDC would fix it.

## Which engines can actually take which mode
Per-title, with the measurement behind each answer: **`PER-TITLE-STATUS.md`**.
Two rules that came out of building it and are easy to get wrong again:

* **A game's own mode menu is not evidence of its ceiling.** Tiberian Sun lists
  640x400/640x480/800x600 and renders 1920x1080, because the CnCNet patch reads
  `SUN.INI` directly.
* **`r_mode -1` is not universal in the id Tech 3 family.** `quake3.exe`,
  `jasp.exe` and `jamp.exe` take it; `sof2mp.exe` silently renders 640x480. All
  four contain the string `r_customwidth`, so the symbol table proves nothing.
