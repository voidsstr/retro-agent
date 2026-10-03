# Every OpenGL/Glide game on `.124`, on our drivers: stability + tuning (2026-10-01)

User request: run each OpenGL/Glide game on `.124` with our drivers, make sure
it is stable, and tune it for the most the V5 6000 and a 2 GHz Athlon XP (twice
a period CPU) can give.

## Summary - where every title ended up (2026-10-02)

The whole box: ICD **0.1.82** (two colour buffers; a third measured no gain
under vsync); **vsync on everywhere** (system `FX_GLIDE_SWAPINTERVAL=1`, now in
Explorer's environment too); 4-chip SLI, no AA, stock clock (the user's
choices).

| title | before | after | how |
|---|---|---|---|
| Quake II (4 shortcuts), SiN (2), SoF | 8-bit paletted textures, bilinear | full-colour textures, trilinear | `v56k_tune.py` (per-box configs) |
| Quake III retail, Jedi Academy SP/MP, SoF II SP/MP, RtCW SP/MP | RtCW `r_picmip 2` + vertex light, SoF II `r_picmip 3` + "New Video card detected" | full-quality preset, renderer string pinned | `v56k_tune.py` - re-run after every ICD deploy |
| Tribes 2 | 800x600x16, vertex light | 1280x960x32, lightmaps | per-box prefs (`v56k_tune.py`) |
| Descent 3 | Direct3D | OpenGL 1280x960 (16-bit - the engine's limit), user's max detail kept, `-framecap 85`, the user's controls on every box | library launcher + pilot (`descent3_*`) |
| UT 436 | OpenGLDrv 1280x960x16, 57 fps, no volumetric light | GlideDrv 1024x768x16 @ 85 Hz, 63 fps | library launcher (`ue1_glide_device`) |
| Deus Ex | D3DDrv 1280x960 | GlideDrv 1024x768x16 @ 85 Hz | library launcher |
| Unreal Gold | GlideDrv 1024x768 @ 60 Hz | GlideDrv 1024x768 @ 85 Hz | library launcher (`ue1_glide_viewport`) |
| Hexen II | 1024x768 (a cap from another box's driver) | 1280x960x32 @ 85 Hz | library launcher (`h2_uncap`) |
| Serious Sam TFE/TSE | 1280x960; TFE exited ~20 s in (WS_CHILD canvas), refused its demos; TSE's refresh line a no-op | TFE runs (exclusive canvas, its Voodoo5 profile), demos play, TSE gets `gap_iRefreshRate`; both on Normal (Quality measured: -5/-6%) | library launchers + TFE GLSettings (`serioussam_preset/`) |
| GoldSrc, GLQuake | 1280x960 | unchanged | already at the box's mode |
| Carmageddon 2, Turok 2 | 640x480, 1024x768 | unchanged | the engines' own limits |
| Descent II (Win95 engine) | registration-card dialog at every launch | no card: the launcher sets the card's own `Times Bypassed` to 3 | library launcher (spec `Descent2.json` prelaunch; `descent2_regcard/`) |

Quit UE1 games from their own menus: an outside window close trips UE1's
RenDev assertion on Glide (below).

**Re-sweep after the tuning** (`sweep_tuned/summary.md`, 20 shortcuts,
2026-10-02 23:39 - 00:11): **0 FAIL**. Every game was alive at 60 s and the
board was healthy after every title. 7 PASS; the 13 CHECKs are all in the close
path, which the sweep sees now that it records dialogs raised by its own
WM_CLOSE:
- Quake II / SiN: "SwapBuffers() failed!", known.
- UE1 on Glide: RenDev, as above.
- Hexen II: its normal quit prompt.
- Descent 3 and SoF II: ignore WM_CLOSE, and the focus sat on the agent's
  console, so no close keys were sent.

**Quake II's close dialog is not the ICD's** (tried and withdrawn 2026-10-03
00:20). ICD build 0.1.83 answered TRUE to a swap with no context and logged the
first one. With it deployed, a WM_CLOSE to Quake II still raised
"GLimp_EndFrame() - SwapBuffers() failed!", and the ICD never saw a
context-less swap. The failing `SwapBuffers` stops before the driver, most
likely on the window and DC that the WM_CLOSE destroyed. `.124` is back on
0.1.82 (`retroicd_0182.bak`, md5 0183eb85), and nothing was committed to the
fork. Quit Quake II from its console or menu.

**Still open:**
- ~~Serious Sam stays on its "Normal" preset until a measured "Quality" run.~~
  **Measured 2026-10-03** (`serioussam_preset/`): Quality costs 5-6% (TSE
  36.2 -> 33.9, TFE 78.7 -> 74.8 fps); both stay on Normal. On the way: TFE
  had stopped running at all on `.124` (a WS_CHILD canvas Glide cannot take
  exclusively), its demos were refused (staged file dates) and TSE's refresh
  line never applied. All three fixed in the library.
- ~~Descent II's Win95 engine registration card.~~ **Fixed 2026-10-03**
  (`descent2_regcard/`): `DESCENTW.EXE` starts the disc's `REGCARD.EXE` at
  every launch, which skips its form once `[Registration Counters] Times
  Bypassed` in `%windir%\EREGREG.INI` is 3 or more. The generated mount
  launcher now sets it. Proven on `.124` after a purge + GAMESYNC: PASS.
- ~~A game started by `lan_sweep` does not get the keyboard focus.~~ **It
  does** (2026-10-03, `sweep_focus_probe/`): Descent 3, Quake II and Unreal
  Gold all held the keyboard at 30 and 60 s. The sweep's own WM_CLOSE destroyed
  their windows, the focus fell back to the agent's console, and only then were
  the close keys tried. `lan_sweep` now sends a proven console quit FIRST,
  while the game has the focus (UE1 `exit`, Quake II `quit`): Quake II, Unreal
  Gold and UT 436 PASS, closed cleanly, no dialog, no force
  (`sweep_clean_quit/`). Each sample records the foreground. Descent 3,
  Deus Ex and SoF II have no automatable clean quit and are still forced.

## Routing as found (`audit.json`, registry, PE imports)

- System ICD: `OpenGLDrivers\3dfx\DLL` = `retroicd.dll` = **our ICD 0.1.80**
  (md5 1f2096fc) - every opengl32-linked game (GoldSrc, UE1/UE2 OpenGLDrv,
  ioquake3, Serious Sam, Tribes 2, the Rebirth ports, GLQuake from the root).
- `system32\glide3x.dll` = **our h5 Glide** (07c96fd9).
- `system32\glide2x.dll` (AmigaMerlin, 94 KB) is a **Glide2->Glide3 wrapper**
  ("GlideXP": imports only KERNEL32/USER32, loads `glide3x.dll` by name) - so
  the Glide 2 titles (UE1 GlideDrv, Turok 2, Carmageddon 2, Descent 3) run on
  our h5 Glide too.
- Off our path: 3dfx MiniGL beside GLQuake (`VOODOO\`, the "3dfx Voodoo"
  shortcuts) and Quake II (`3dfxgl.dll`, unused while `gl_driver` is
  opengl32); RtCW's `gl\openglv5.dll`; a game-local `glide3x.dll` the bench's
  all-ours lane left beside Quake II and Quake III (an older pinned build that
  shadowed system32 in normal play - the bench now removes what it staged).

## Baseline sweep (`sweep_baseline/`, `lan_sweep.py`, 19:22-20:42)

49 shortcuts: **38 PASS, 11 CHECK, 0 FAIL; the board brought Glide up after
every title.** Table: `sweep_baseline/summary.md`. Most CHECKs are forced
closes - the keyboard focus sat on the agent watchdog's console
(`agentwd.cmd`), so the sweep withheld its close keys - not instability. The
real ones: Descent II Win95 (registration-card dialog), SoF II ("New Video
card detected", shown until a clean exit saves the new renderer string), Blue
Shift (SecuROM, known), Rainbow Six (Direct3D, out of scope).

Below the box's 1280x960: Descent 3 640x480, Tribes 2 800x600, UT2003 640x480,
Unreal Gold 1024x768, Hexen II 1024x768 (a fleet-wide cap), Carmageddon 2
640x480. (A Glide game does not change the GDI mode, so the "GDI mode seen"
column is only evidence for OpenGL titles.)

## Routing cleanup (2026-10-02)

Deleted the bench's game-local `glide3x.dll` (2df2f969) and `retrogl.dll`
(0.1.76) beside Quake II and Quake III; retail Quake III's `q3config.cfg`
`r_glDriver` back from `retrogl` to `opengl32` (the system ICD). Our ICD's
own load log (`C:\retrogl.log`) during the sweep: loaded by hl.exe x8,
quake2.exe, GLQUAKE.EXE, glh2.exe, ioquake3, quake3.exe, WolfSP/WolfMP,
jasp/jamp, SoF/SoF2/sof2mp, sin.exe, SeriousSam.exe, Tribes2.exe,
d1x/d2x-rebirth and UnrealTournament.exe (OpenGLDrv). UT2003/UT2004 ran
Direct3D (our HAL), not the ICD.

## Vsync (user: "On - tear-free"), `vsync_q3/`

The ICD puts `FX_GLIDE_SWAPINTERVAL=0` into the process when the environment
does not set it, and Glide applies that value over the game's own swap interval,
so **every OpenGL game ran without vsync**. One system variable decides it for
every Glide/OpenGL game on the box: `HKLM\...\Session
Manager\Environment\FX_GLIDE_SWAPINTERVAL = 1` (set 2026-10-02). Set with
reg, it told nobody; `scripts/fleet/win9x/envbcast` sent Explorer the
`WM_SETTINGCHANGE("Environment")` at 22:27, and `envof` then read
`FX_GLIDE_SWAPINTERVAL=1` out of the running `explorer.exe`'s own environment
block, so games started from the desktop get vsync without a log-on. (A check
from before the broadcast failed - the Run dialog never took the focus - so it
is not proven that Explorer lacked it before.) The agent itself still lacks it,
as does anything the agent starts. The bench's launchers set 0 themselves,
so timing runs are unaffected.

**CORRECTED 2026-10-02 09:45.** The first table (`vsync_q3_cpu_starved/`, 09:32) was
measured beside a Carmageddon 2 process that the sweep's forced close had NOT
ended. It had spun one thread at 100% CPU for 14 hours (below). On a quiet CPU,
ICD 0.1.82, Quake III retail demo four, cfg 5:

| | vsync off | vsync on, 2 buffers | vsync on, 3 buffers |
|---|---|---|---|
| 1280x960x32 (`vsync_q3/`) | **76.8**, 76.8 | 55.4 | 55.3 |
| 1024x768x32 (`vsync_q3_1024/`) | **104.3**, 104.3 | 74.6 | - |

Tear-free costs Quake III ~28% at either resolution: a frame that misses an 85 Hz
refresh waits for the next one. A third buffer does not help on this hardware,
so ICD 0.1.82 keeps two (`CHANGELOG.md`). Titles far above 85 fps (Quake II,
GoldSrc, GLQuake) lose nothing visible.

## A sweep leftover burned the CPU for 14 hours (found 2026-10-02)

`CARMA2_HW.EXE` from the sweep's first title (19:22) was still running at 09:50
the next day. One thread was Ready the whole time, using ~77% kernel + ~22%
user CPU (WMI: ~10.4 h of kernel time), and CPU load was 100%. It held nothing
in our driver (`vcrctl info`: `exclusive_pid` 0, no SLI session), and the
agent's `PROCKILL` ended it at once. The sweep's `taskkill /f` had not, and the
sweep never checked. Every title after it, and the 09:32 measurements, ran
beside it. The sweep results still stand as pass/fail; their timing does not.
`lan_sweep.py` now verifies a forced close (agent `PROCKILL` for a survivor)
and fails and stops on one (`tests/python/test_lan_sweep_force.py`).
Carmageddon 2 had never had the keyboard focus (the agent watchdog's console
had it), which is the likely start of its spin.

## Per-box tuning applied (`scripts/benchmarks/v56k_tune.py`, 2026-10-02 09:42)

48 values in 14 unstaged per-box config files; a second run changes 0.
- Quake II (baseq2/xatrix/rogue/ctf), SiN (base/2015): full-colour textures
  (`gl_ext_palettedtexture 0`), trilinear.
- SoF: trilinear.
- Quake III retail, Jedi Academy SP/MP, SoF II SP/MP, RtCW SP/MP: the
  full-quality preset (`r_picmip 0`, 32-bit textures and colour, lightmaps,
  uncompressed, trilinear, `r_subdivisions 4`).
- `r_lastValidRenderer` set to the deployed ICD's string. An id Tech 3 game that
  sees a "new card" resets to its low preset, and our renderer string carries
  the build number. That is how RtCW had dropped to `r_picmip 2` with vertex
  lighting and SoF II to `r_picmip 3` behind a "New Video card detected" box.
  Re-run the script after every ICD update (or move the build tag out of
  GL_RENDERER - proposed).
- ioquake3 (Quake III / Team Arena shortcuts) was already at full quality.


## Tribes 2 (2026-10-02 10:51)

`Classic\prefs\ClientPrefs.cs` (per-box): 800x600x16 -> **1280x960x32**, lightmaps
instead of vertex lighting, vsync allowed, `profiledRenderer`/`defaultsRenderer`
pinned to the deployed ICD (Torque re-profiles on a new renderer string the way
id Tech 3 does). **`safeModeOn` must stay 1.** With 0, Tribes 2 made its GL context
at 640x480, switched the desktop to 1280x960 under it, our kernel ended the Glide
session at the mode set, and the screen stayed black. With 1 it destroys and
re-opens the context, and the ICD log shows the board opened at 640x480, closed,
then opened at 1280x960. **User: "looks right".**

## Descent 3 (2026-10-02 21:00-21:55)

**Renderer and mode.** OpenGL through our ICD (`PreferredRenderer` 2) instead
of the staged Direct3D. The menus always run at 640x480. A level runs at the
launcher's `-Width/-Height` (1280x960 here), and the ICD log shows the board
re-opened at 1280x960 for every level. **User: "level runs fine".** The game's
OpenGL renderer is 16-bit only: the original code sets `dmBitsPerPel = 16` with
the `bit_depth` line commented out (`legacy/renderer/opengl.cpp`, released
source), and the ICD log shows `colDepth=16` with `RS_bitdepth` 32. So
`RS_bitdepth` stays 16, and the V5's 16-bit postfilter is what the user sees.

**The user's controls are on every box.** Descent 3 keeps the whole control
mapping in the pilot file. The user set up WASD + F and the mouse on `.124`, so
that `sdf.plt` (`descent3_controls/sdf.plt`, md5 6b8115ec) replaced the
library's April copy, and `install.reg` gained `"Default_pilot"="sdf.plt"`.
That value only pre-selects the pilot in the PILOTS menu (`pilot.cpp`
`PilotSelect`); the Play and Join launchers pass `-pilot SDF`, which skips the
menu. Deploy generation bumped at 21:24.
`retro-autodeploy` synced `.123`, `.124`, `.197` and `.243` (all `failed_files`
0), and the pilot read back md5 6b8115ec plus `Default_pilot` on `.123`,
`.124` and `.197` (`.243` is gated for D3). Proved on `.124` first by deleting
both and re-syncing: both came back from the library. A box that is off
receives the pilot through `retro-autodeploy` when it next answers.

**`PredefDetailSetting` is out of `install.reg`.** At start-up main.exe reads
the per-option detail values and then applies this preset (0-3) over all of
them; 4 = custom keeps them (`init.cpp` `LoadGameSettings`). The staged 1 reset
each box's own detail at every sync. The 21:26 sync did that to `.124`, whose
user had maxed the custom sliders (pixel error 0, render distance 200 - above
the very-high preset), and an earlier `v56k_tune.py` row had written 3 over it
too. Without the value, main.exe uses medium, which is what the staged 1 gave a
fresh box. `.124` is back on 4, and a later sync kept it (verified).

**Per box, in the library launcher** (`stage-fleetres.py` `d3_renderer`, marker
`D3_RENDERER`): `PreferredRenderer` 2 where the 3dfx card drives the screen
(FR_UE1DEV = Glide). GAMESYNC re-merges install.reg's 3 at every sync, so the
launcher sets the value at every launch. `-framecap %FR_HZ%`: main.exe caps
itself at 60 fps (16 ms), which under vsync at 75-100 Hz presents frames at
uneven one- and two-refresh steps. 60 Hz boxes keep the default. Checked on the
boxes by running the launcher with `echo` in place of `start`: `.124`
`-framecap 85` with the renderer set to 2; `.123` (no 3dfx) `-framecap 100`
with the renderer left at 3.

**The game's own benchmark** (`descent3_timetest/`,
`scripts/benchmarks/d3_timetest.py`): retail 1.4 `-timetest Secret2.dem` plays
the shipped demo and writes `fps.txt`. Average and per-second fps; every run
exited by itself.

| box | path | run | avg | median | max |
|---|---|---|---|---|---|
| `.124` | OpenGL, our ICD + h5 Glide, 1280x960x16 @ 85 Hz, user's maxed detail | vsync off, stock cap | 58.8 | 63 | 65 |
| `.124` | " | **vsync on, stock cap** (the user's play until now) | 60.2 | 64 | 65 |
| `.124` | " | **vsync on, `-framecap 85`** (now staged) | **63.9** | 64 | 78 |
| `.124` | " | vsync off, `-framecap 200` (the ceiling) | 79.0 | 78 | 120 |
| `.123` | Direct3D, 1280x960 | stock cap | 60.1 | 64 | 65 |
| `.123` | " | `-framecap 100` (now staged) | 60.5 | 64 | 73 |

The stock cap held `.124` at 62-65 fps, and the card can average 79. With
vsync on, a frame that misses an 85 Hz refresh waits for the next one, so the
raised cap is worth +6% rather than the full 79. `.123` gains little, with no
regression; its GDI captures (`*_123_*/*.png`) show the demo playing. On `.124`
GDI cannot capture a Glide frame: it reads the desktop surface in the board's
tiled layout, so those captures were deleted rather than kept as evidence. The
agent's own environment lacks the system `FX_GLIDE_SWAPINTERVAL=1` that a
desktop launch inherits, so the runner sets it explicitly for the vsync rows.

**The pilot also sizes the 3D view** (`descent3_pilot_window/`). At a level
start `SetScreenMode(SM_GAME)` takes the game window from the pilot and clamps
it to the display. The user's pilot saved .124's 1280x960, so on any larger
display the view would be a box. A/B on `.123` (XP, Direct3D, CRT) at
1600x1200, joined to the dev host's `descent3-server` with the Join launcher's
own arguments (`-pilot SDF -directip +connect`). The game's `-timetest` demo
cannot show this: it filled 1600x1200 with either pilot.

| pilot | md5 | window in the file | non-black area at 1600x1200 |
|---|---|---|---|
| the user's, as saved on `.124` | 6b8115ec | 1280x960 | **160,120-1440,1080** (a centred box) |
| the same, window raised to 4096x4096 | dd51fb83 | 4096x4096 | **0,0-1600,1200** (full, cockpit + HUD) |

The 4096x4096 patch was the `lcd1080` lane's
(`provisioning/patches/descent3/apply.py`, proven 2026-09-29 on `.240` on the
April pilot, version 0x2A). It had never been published. The tool is rebased
onto the user's pilot (version 0x2B, which appends two rearview bytes after the
window) and published with its own backup-then-put:
`_patches/Descent3/originals-2026-10-02/sdf.plt` = the user's original
(6b8115ec), `Descent3/sdf.plt` = the patched file (dd51fb83, exactly the file
run in the B row). Every byte of the user's controls is unchanged; only
0x1F..0x26 differ.

**On the boxes (deploy generation 2026-10-02 22:20, `retro-autodeploy`, all
`failed_files` 0).** `.123`, `.124` and `.197` read back `sdf.plt`, both
launchers and `install.reg` md5-equal to the library. On `.123` the pilot had
been deleted first, so the library had to deliver it. `.124` keeps
`PredefDetailSetting` 4 through the sync. The command lines each launcher
builds on its own box (`start` swapped for `echo`):

| box | Play | renderer after the launcher |
|---|---|---|
| `.124` (V5 6000, CRT 85 Hz) | `-Width 1280 -Height 960 -pilot SDF -framecap 85` | 2 (OpenGL) |
| `.123` (CRT 100 Hz) | `-Width 1280 -Height 960 -pilot SDF -framecap 100` | 3 (Direct3D) |
| `.197` (Win7, 1080p 60 Hz) | `-Width 1920 -Height 1080 -pilot SDF` | 3 (Direct3D) |

`.243` synced too (Descent 3 is gated there). Boxes that are off pick the
generation up from `retro-autodeploy` when they next answer. Fleetbook recipe
`descent3-stage-a-users-controls-fleet-wide` (#110).

## Unreal Engine 1: Unreal Gold, UT 436 and Deus Ex on Glide (2026-10-02 22:30-23:10)

`.124` carries `GlideRender` = 1, which tells FLEETRES to render UE1 through
GlideDrv. Only Unreal Gold's launcher used it. **UT 436 (`ut436_renderer/`,
UTbench, 4-chip, no AA, vsync off):**

| device | mode | fps |
|---|---|---|
| **GlideDrv** (Glide 2 -> our h5 Glide) | 1024x768x16 | **63.4** |
| OpenGLDrv (our ICD 0.1.82) | 1024x768x16 / x32 | 57.0 / 57.0 |
| OpenGLDrv | 1280x960x16 / x32 | 57.0 / 56.0 |

(`ut436_renderer/diag/` is the Dr. Watson record the bench found on the box
and kept: `hl.exe`, 2026-10-01 19:36:50 - the baseline sweep's Blue Shift
CHECK, SecuROM - not a UT crash.) OpenGL is CPU-bound, and the staged
OpenGLDrv section runs without volumetric lighting. So Glide is faster and complete. `stage-fleetres.py`
`ue1_glide_device` now gives UT 436 (Play and Join) and Deus Ex (Play; the
dedicated host renders nothing) Unreal Gold's treatment where the 3dfx card
drives the screen: GlideDrv for all three device keys, the Glide 2 4:3 ladder
(1024x768 inside .124's 1280x960), and the refresh. Every other box keeps its
staged device.

**The refresh.** GlideDrv's `RefreshRate` takes 60/70/72/75/80/85/90/100/120
Hz (the strings in GlideDrv.dll); the staged 60Hz flickered on the CRT and
capped the game at 60 under vsync. `ue1_glide_viewport` now writes the largest
of those not above `FR_HZ`, which is 85 on `.124`. The board's own reading
during each game (`vcrctl info`, `ue1_glide_sweep/vcrctl_info_samples.txt`,
`ut436_glide_85hz/`): **1024x768x16@85, 4-chip SLI** for Deus Ex, Unreal Gold and
UT 436, each started through its real desktop shortcut. Deus Ex's log:
`grSstOpen Res=8 Ref=7` (1024x768, 85 Hz). UT 436 at 85 Hz: 63.6 fps.

**Quitting.** Each game's own exit is clean on Glide. Unreal Gold's console
`exit` closed it at once, UT 436's bench Exit closed it, and Deus Ex (training
map, F12 bound to `exit` for the test only, then restored) closed in 3 s. All
three left the board back on the desktop. **An outside WM_CLOSE is not clean.**
UE1 then destroys the render device and a WM_KILLFOCUS hits
`UWindowsViewport::EndFullscreen`: "Critical Error - Assertion failed: RenDev
[WinViewport.cpp line 2187]" (Deus Ex, `vcrctl fbshot` + GDI capture). So
quit from the game's menu, not by closing its window. The baseline sweep closed
Deus Ex and UT 436 that way without trouble on D3D/OpenGL. `lan_sweep` never saw
the dialog, because it looked for dialogs only before its WM_CLOSE. It now
records `dialogs_on_close` (`tests/python/test_lan_sweep_force.py`).

**Tooling found on the way.** `v56k_bench.py` stamped stock-lane rows with
AmigaMerlin's `3dfxOGL.dll` while `OpenGLDrivers\3dfx\DLL` named our
`retroicd.dll`. It now reads the registration from the box (`registered_icd`,
`icd_source` per title). The rows' own `gl_renderer` (`[voodoo-cleanroom
0.1.82]`) had been right all along.

## Hexen II at 1280x960 (2026-10-02 23:15, `hexen2_1280x960/`)

The fleet-wide `-cap 1024 768` came from `.240`'s ATI driver, which refused
every larger mode. On `.124` glh2 opened 1280x960x32@85 through our ICD in
4-chip SLI, rendered, and quit cleanly from its menu. `stage-fleetres.py`
`h2_uncap` lifts the cap only where the 3dfx card drives the screen. `.124`'s
Play, Host and Join launchers now build 1280x960; `.123` still builds 1024x768.
