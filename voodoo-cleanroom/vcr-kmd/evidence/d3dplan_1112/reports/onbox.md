# On-box Direct3D evidence: `.124`, Voodoo 5 6000 on vcr-kmd (2026-11-12)

Paths below are relative to `voodoo-cleanroom/vcr-kmd/evidence/d3dplan_1112/`. The run index (every
command, result and file) is [`onbox/README.md`](../onbox/README.md). Notes taken during the run are in
[`onbox/session_log.md`](../onbox/session_log.md).

## Summary

1. **A Glide client that closed on its not-idle path breaks Direct3D silently.** A 4-chip OpenGL/Glide
   session ran about two hours before this run (pid 1892, 75242-75345 s). It closed through h5 Glide's
   "board not idle" path, which skips the command-FIFO disable, so Glide's **command FIFO 0 stayed
   switched on** (`cmdFifo0.baseSize` 0x1fe, window 0x18000-0x117000, read pointer stuck at 0x2f624).
   In that state:
   - every D3D device was created and every clear worked, but **no triangle reached the target**:
     d3dprobe gave 9/40 fullscreen at 1024x768x32 and 7/42 windowed;
   - the windowed device's texture landed inside the stale FIFO window (0xc0000). Within a second the
     chip read `SST_CMD0_BUSY` (status 0xa1f). The desktop PDEV then logged **"2D engine never went
     idle - acceleration off for this PDEV"**;
   - after that, **the D3D HAL was not offered at all**: `GetDeviceCaps(HAL)` and `CreateDevice` both
     returned `8876086a`.

   One paced mode switch (`vcrctl modeseq`) brought the box back. At that switch the miniport found
   the chip busy and ran its own "busy at mode set" engine reset, which writes `cmdFifo0.baseSize = 0`.
   With **no driver or setting changed**, the same tests then passed: 40/40 (1024x768x32), 42/42
   (windowed) and 40/40 (640x480x16).

   The vcr-kmd README already says that a *killed* Glide client needs a cold boot before a 32 bpp D3D
   test. This run adds four things:
   - the same state follows a close that was not a kill (its views and DirectDraw object were
     released);
   - the mechanism is that `cmdFifo0` is left on;
   - a recovery exists that needs no reboot;
   - the state is invisible until something makes the FIFO busy.
2. **The X8R8G8B8 + D16 lead is confirmed.** The runtime reports the pair as matching, yet
   `CreateDevice` fails with `D3DERR_INVALIDCALL` (8876086c). The ring logs `513/12 ContextCreate
   refused: 32 bpp target with a 16-bit Z`. On that failure the D3D8 runtime itself made **two CRT mode
   switches 156 ms apart**. No caller pacing can prevent those two switches.
3. **On a clean engine the HAL's own checks pass.** All d3dprobe render, texture and decode checks
   pass at 1024x768x32. That covers mipmaps to 2048, A8R8G8B8, single-level DXT1/3/5 up to 1024x512,
   and texel-exact decode at 1:1, so the half-pixel fix holds on silicon. The only 3 failures are the
   `*_def` tests, which the D3D8 runtime refuses (a DEFAULT-pool texture cannot be locked).
4. **In-game D3D scenes are captured for the first time, and they are not correct.** In UT2004
   (twice), the UT2003 flyby and Halo's timedemo, the sky, weapons, HUD, text, trees and smoke render
   right, but:
   - (a) blocky multi-coloured "confetti" noise covers the terrain and many rock meshes;
   - (b) lightmapped surfaces in both Unreal titles are very dark;
   - (c) large solid-white areas appear where sky or horizon should be (UE2);
   - (d) a Halo glow sprite is drawn as a visible translucent square;
   - (e) UT2003's grass is blue;
   - (f) a grey checkerboard patch appears in UT2003.

   d3dprobe passes all its texture tests while this noise appears in game. So the noise comes from a
   path the probe does not exercise.
5. **D3D performance is very low.**
   - The UT2003 flyby averaged **9.4 fps** (minimum 2.1) at 640x480x16. Frames took 123 ms, 116.7 ms of
     it inside DrawPrimitive, at about 29k triangles and 34k software-processed vertices a frame. That
     works out to about 240k triangles/s end to end.
   - UT2004 botmatch at 1280x960x16 with 8 bots: **4 current / 6 average FPS**.
   - Halo's timedemo had not finished after 485 s.
   - d3dprobe's perf test (200 triangles a frame) holds the 85 Hz refresh at both depths, so it is too
     light to show the problem.
6. **Stability was good.** There was no hang, crash, Dr. Watson entry or error dialog across 13 d3dprobe
   runs and 4 game sessions. Every program returned the desktop to 1280x1024x32@85.

## The box during the run

- **Timing.** Agent 1.100.1, 23 h uptime, `boot_count` 69, desktop 1280x1024x32@85. Box time was
  09:59-10:46 (47 min).
- **Starting state.** The screensaver was in front; `UIKEY SHIFT` dismissed it (`ScreenSaverIsSecure=0`).
  Also running: the `agentwd.cmd` watchdog, `daemon.exe` and `NvMixerTray.exe`.
- **Driver.** `vcrdd.dll` 95678704, which is identical to `out/vcrdd.dll`. Diag D3D32=1 and
  D3DBigTex=7 are armed, and the ring confirms them with 513/16 and 513/18.
- **Not done.** No reboot, and no registry, driver, Diag or file change outside `C:\vcr\`.
  Max Payne was not launched.

## 1. A stale Glide command FIFO blocks D3D triangles (`onbox-01`, `onbox-02`)

Evidence chain, all in `onbox/`:

| step | what | evidence |
|---|---|---|
| a | pid 1892 runs an OpenGL session through our Glide at 1280x960x32 with 4-chip SLI (75242-75345 s). Its close: `SLI/AA off: display reset` (75345.579 s, from `IOCTL_VIDEO_RESET_DEVICE`, `miniport/vcrmp.c:693`) comes first; then `DirectDraw exclusive 0`, its 14 views released (75346.471 s), a `hwcext 16` escape from pid 1892 itself (so the process was alive and running its close, not killed) and `DestroyDDLocal`. No `SLI/AA off: Glide asked` appears, so Glide never sent its own SLI/AA disable. That is h5 Glide's `hwcRestoreVideo` not-idle path (`build/retro3dfx-glide/glide3x/h5/minihwc/minihwc.c` 5980-6019), which skips the whole register restore, including `/* disable the CMD fifo */ cmdFifo0.baseSize = 0`. | `ring_start_decoded.txt` lines 234-987 |
| b | 10:02: fullscreen 1024x768x32 + D24X8. The device and the context were created (`ContextCreate 1: target 300000 ... z 600000`), and the context logged `28 DrawPrimitives2, 36 triangles, 14 clears, 0 unparsed`. Every clear read back right; every triangle check read the clear colour. | `d3dprobe_render_1024x768x32.txt`, `ring_after_probe_ii_decoded.txt` |
| c | 10:03: windowed on the desktop. Target 0x40000, Z 0x80000 and texture 0xc0000 all sit inside the FIFO window. 1.3 s after the texture: `ERR DD_2D 2D engine never went idle (status 00000a1f) - acceleration off for this PDEV`. From then on even the clears read black. | `d3dprobe_render_windowed_desktop32.txt`, `ring_after_probe_windowed_decoded.txt` |
| d | `vcrctl reg` (read only, chip 0). Status 0xa1f = SST_BUSY + SST_CMD0_BUSY, with FBI, TMU and 2D idle and the PCI FIFO empty. `cmdFifo0` baseAddr 0x18, baseSize **0x1fe (enable bit 8 set)**, readPtr 0x2f624 on two reads, aMin 0x2f630, depth 0, holeCount 0x22177. The 3D state is the HAL's own: fbzMode 0x201, clip 256x256, colBuffer 0x40000/0x400, auxBuffer 0x80000/0x400. | `regs/regs_after_windowed_1009.txt` |
| e | 10:11: a fullscreen device is not possible at all: `GetDeviceCaps(HAL) 8876086a`, `CreateDevice 8876086a`, and no mode switch was made. **Any D3D game launched now would have failed the same way.** | `d3dprobe_render_1024x768x32_after_reset.txt` |
| f | 10:12: `vcrctl modeseq 6000 1024 768 32 85` logged `chip 0 not idle (0xa5f)`, then `engine reset (busy at mode set): idle again (0x5f)`. The GDI test had 0 mismatches. Afterwards cmdFifo0.baseSize was 0 and status 0x1f. | `vcrctl_modeseq_recover_1012.txt`, `ring_after_modeseq_decoded.txt`, `regs/regs_after_modeseq_1012.txt` |
| g | The same command as (b): **40/40**. Windowed, same as (c): **42/42** (10:43). 640x480x16: **40/40**. | `d3dprobe_render_1024x768x32_clean.txt`, `..._windowed_desktop32_clean.txt`, `..._640x480x16.txt` |

**What is established.** These facts were measured directly:
- with cmdFifo0 on, the HAL's triangles do not appear while its fastfill clears do;
- host writes into the FIFO window make the FIFO busy;
- a busy chip makes the 2D path give up, and with it the whole D3D HAL;
- the miniport's existing engine reset (`VcrHwResetEngine`, which stops the FIFO fetch first) cures it.

**What is inferred, not measured.** That the FIFO was left on *by pid 1892's not-idle close*. It was
the only Glide client in the ring, its SLI went off through the kernel's display reset rather than by
Glide's own request, and the source shows that this path skips the FIFO disable. Glide's own trace
(`RETRO_GLIDE_MAPLOG`) for that session was not read. It is also not measured why an enabled but idle
FIFO stops PIO triangles while fastfill still works. One candidate is that the fetcher is stuck inside a
packet that owns the setup unit (readPtr is 3 dwords short of aMin). The 16 bpp path was not tested
while the FIFO was on.

**Why nothing caught it.** The miniport resets the engine only when the chip reads *busy* at a mode
set. An enabled but idle FIFO passes that check, and so does the 2D idle test: the desktop did 10
million 2D operations after pid 1892 without a give-up. `Diag\Reset3D` would not help either: it is off
by default, and it refuses whenever SST_CMDFIFOEN is set (README, Reset3D row).

**Fix direction for the plan (not done here).**
- The kernel already turns SLI/AA off at the display reset that ends a not-idle Glide close
  (`VcrSliOff(x, "display reset")`). It should also stop the command-FIFO fetch there, the first step
  of `VcrHwResetEngine`, when no Glide client holds the board.
- At D3D `ContextCreate`, read `cmdFifo0.baseSize`. If it is enabled and no live process holds the
  board for Glide, run the engine reset and log it.
- Never place a DirectDraw or D3D surface inside an enabled command FIFO window.
- A 2D give-up should try the engine reset before it removes acceleration, and with it the D3D HAL, for
  the life of the PDEV.
- A regression test can encode these as `vcr_3dseq.h`-style pure-logic checks: "FIFO on + no owner →
  reset" and "FIFO on + owner → no reset".

## 2. d3dprobe results (clean engine unless noted)

| run | config | result |
|---|---|---|
| render | fullscreen 640x480x32 + **D16** (`--zfmt d16`) | **CreateDevice 8876086c**; ring 513/12 refused (32 bpp target + 16-bit Z); two mode switches 156 ms apart (`onbox-03`) |
| render | fullscreen 1024x768x32 + D24X8, **stale FIFO** | 9 pass / 31 fail: clears only (`onbox-01`) |
| render | windowed 256x256 on the 32 bpp desktop + D24X8, **stale FIFO** | 7 / 35, then the 2D give-up (`onbox-02`) |
| render | fullscreen 1024x768x32 + D24X8 | **40 / 0** |
| textures A | 1024x768x32: mip, mip2048, tex512/1024/2048, tex8888 (64/256/512/512x128), dxt1/3/5 128, dxt1_512, dxt5_512/512x256/512x128/256x128 | **83 / 0**, 0 skipped; mip selects levels 0-2, mip2048 levels 0-4 (2048 to 128) |
| textures B | 1024x768x32: dxt1/3/5_decode, dxt1/5_decode1, rgb565_decode, **rgb565_decode1**, dxt5_big_a/b/c (3 windows each), texhigh, texhigh_dxt5, `_upd`/`_copy`, `*_def` | **37 / 3**; every decode **0 of 1024 texels wrong, RGB and alpha**; the 3 `*_def` fail with `LockRect 8876086c` (runtime refusal, `onbox-10`) |
| render | fullscreen 640x480x16 + D16 | **40 / 0** |
| render | windowed, 32 bpp desktop | **42 / 0** |
| perf | fullscreen 640x480x16, 300 frames, vsync | **85.1 fps**, 17029 tris/s (refresh-bound; the runtime chose 85 Hz) |
| perf | fullscreen 1024x768x32, 300 frames, vsync | **84.4 fps**, 16879 tris/s; ring `flips 300: retrace 284, deadline 15` |

The monitor gate expected the "highest refresh the driver lists" (120 Hz at 640x480, 100 Hz at
1024x768). In every run the runtime chose 85 Hz.

## 3. The X8R8G8B8 + D16 lead (`onbox-03`)

- The runtime's answer is in `caps_124.txt`: zmatch X8R8G8B8 / D16 has format 1 and match 1, and
  hal_fullscreen X8R8G8B8 is 1.
- The device fails anyway: `CreateDevice(HAL, fullscreen, back buffer 640x480 fmt 22, z fmt 80)`
  returns `8876086c`. Ring entry 24713: `dd WRN DD_D3D 0000000c 00000020 00000010 ... ContextCreate
  refused: 32 bpp target with a 16-bit Z on a VSA-100 (16/16 or 32/32 only)`.
- **Mode switches.** The runtime set mode 144 (640x480x32@85) at 83009.846 s, then put the desktop
  (mode 159) back at 83010.002 s, before d3dprobe's exit hold could act. A D3D8 or D3D9 game that takes
  the advertised pair will get INVALIDCALL and give the 1998 CRT two re-syncs 156 ms apart. The probe
  was not repeated.
- **Fix choices for the plan.** Either stop D16 from matching a 32 bpp target, or make that pair draw.
  The second could use a hidden 32-bit aux buffer: the 16-over-32 trick of 11-08, the other way round.
  `vcr_rtfmt.h` notes the constraint: D16 must stay listed at a 32 bpp desktop for 16 bpp fullscreen
  devices.

## 4. In-game scenes

All frames come from the agent's GDI `SCREENSHOT 0`. All four games ran fullscreen on D3DDrv or
`halo.exe` with their staged settings.

### UT2004: DM-Antalus botmatch, spectating (`games/UT2004_botmatch/`, `games/UT2004_statfps/`)

UT2004.log reports `SetRes: 1280x960`, back buffer format 23 (16-bit) and depth 80 (D16), although
`UT2004.ini` has `Use16bit=False`.

| frame | what it shows |
|---|---|
| `UT2004_botmatch_025s.png`, `UT2004_statfps_040s.png` | the splash over the desktop (loading); right |
| `UT2004_botmatch_045s.png` | the new 1280x960 mode before the first frame: stale desktop VRAM read at 16 bpp (doubled icons, green/purple). The visible primary is not cleared at the mode set (`onbox-13`), the same thing AvP showed on 10-14 |
| `UT2004_botmatch_065s.png` | an early in-level frame: flat sky colour, a horizontally striped band and a white area at the bottom. Broken |
| `UT2004_botmatch_090s.png` (+ `_zoom_rock`, `_zoom_terrain`) | **in game**: orange sky gradient right; the near rock face right but dark; the terrain and the central rock mesh covered in **blocky multi-coloured noise** (4x4-block-like confetti) with some patches correctly textured; everything very dark |
| `UT2004_statfps_080s.png` | **in game**, a bot's view with `stat fps` showing **4 cur / 6 avg FPS**. Right: the link-gun model, yellow-green grass, the whole HUD, chat text. Wrong: the floor beyond the grass is block noise; the sky seen through the opening is **solid white**; dark |
| `UT2004_statfps_100s.png` | orange sky band at the top, then **a large solid-white region** over the horizon; distant terrain with noise; near ground (moss) right |

The first run did not end by itself: `-benchmark -seconds=60` was still running at 242 s. UT2004
ignored WM_CLOSE and was forced by pid, so its log is 0 bytes. The second run ended cleanly through the
console `exit`.

### UT2003: the staged flyby benchmark (`games/UT2003_flyby/`)

- Ran at **640x480x16@120**: `Best-match display mode: 640x480x16@120`, back buffer 23, depth 80. The
  ini asks for 1280x960 (`onbox-14`).
- The log also says `software transform and lighting`, `Device doesn't support
  D3DVTXPCAPS_MATERIALSOURCE7` and `Using w- pixel fog`.
- **`Results\avgfps-2026-11-12-10-28-15.log`: `2.067308 / 9.411991 / 1.#INF00 fps`, Score 9.419.**
- The stats CSV (`stats-2026-11-12-10-28-15.csv.gz`; 1998 frames, 245.7 s, 8.1 fps measured from frame times): 122.99 ms mean frame,
  **116.73 ms in `Hardware DrawPrimitive`**, 29,362 primitives and 33,846 software-processed vertices
  a frame. Present takes 0.76 ms; locks, state and texture changes are negligible.

| frame | what it shows |
|---|---|
| `UT2003_flyby_030s.png` | broken: terrain **solid white**, a cyan triangle, a striped floor; only the sky band is right |
| `UT2003_flyby_060s.png` | rock formations covered in **block noise**; deco grass **blue** (UT2004's grass is right); dark; sky right |
| `UT2003_flyby_090s.png` | canyon: near rock walls right, distant towers right, noise on mesh edges and on the floor, blue grass |
| `UT2003_flyby_120s.png` | pine trees right (alpha-tested), sky right; a **white wedge** in the sky; a **grey checkerboard** patch; rocks in noise; blue grass |

### Halo: `halo.exe -novideo -vidmode 1280,960,85 -timedemo` (`games/Halo_timedemo/`)

These are the staged launcher's own switches for this box, from `FLEETRES.EXE -cmd` →
`games/halo/fleetres_cmd.txt`. The per-box key `reg import` was not done, because it is a registry
change.

| frame | what it shows |
|---|---|
| `Halo_timedemo_030s.png` | the ring and the gas giant from space: textured and **largely right**. The ring's outer band is speckled like the starfield; this may be correct art |
| `Halo_timedemo_060s.png` | canyon cutscene (letterboxed): rocks and bridge plausible; the glow around a blue crystal is drawn as a **visible translucent teal square** (`onbox-07`) |
| `Halo_timedemo_090s.png`, `_120s.png` | canyon: **block-noise patches** on some rock faces; a blocky stair-stepped distant structure; subtitle text ("Flawless Cowboy") crisp |
| `Halo_timedemo_180s.png` | escape pod with its smoke trail over trees, ring arc and planet: **largely right**; a distant ruin has blocky holes |

The timedemo did not finish within 485 s: `dir /s` found no `timedemo.txt` anywhere on C:. WM_CLOSE
ended it cleanly. The ring summary for the session reads `32670 DrawPrimitives2, 26,070,025
triangles, 764 clears`.

### What the in-game defects point at (hypotheses for the static readers; none verified here)

- **Block noise** (`onbox-04`) appears on lightmapped and terrain surfaces. Unlit or alpha-tested things
  are right: weapons, grass in UT2004, trees, HUD and text. `UseCompressedLightmaps=True` in both
  Unreal titles makes the lightmaps DXT1, and they are drawn in the **second** texture stage.
  d3dprobe never puts a DXT texture in stage 1, never builds a multi-level DXT chain (sub-4x4 levels),
  and never draws a terrain-style alpha-layer pass. Those are the first d3dprobe tests to add.
- **Darkness** (`onbox-05`). TextureOpCaps 0x184f does not advertise `MODULATE2X`, and the HAL draws
  MODULATE2X as MODULATE (known gap). UE2's lightmaps expect 2x, so the world comes out about half
  as bright. VSA-100's extended texture combine has the shift for this: Glide's
  `grTexColorCombineExt` takes a shift, 0/1/2 = x1/x2/x4.
- **White areas** (`onbox-06`) cover the sky and horizon in UE2, which logs `Using w- pixel fog`.
  d3dprobe's fogtable test covers only w 1 to 8 with a blue fog colour. Candidates: W table fog over
  large world-unit ranges, fog colour, TFACTOR, or a blend mode the HAL maps to white.
- **Glow square** (`onbox-07`). A glow texture normally carries its shape in alpha. If the HAL ignores
  `D3DTA_ALPHAREPLICATE` (known gap: color_path masks it), or ignores the stage's alpha source, the
  square shows.
- **Blue grass** (`onbox-08`, UT2003 only) looks like a red/blue swap in one texture format or a
  vertex-colour path.

## 5. Performance (`onbox-09`)

| title / test | setting | result |
|---|---|---|
| UT2003 flyby (`-benchmark`) | 640x480x16@120, 1998 frames | **avg 9.41 fps, min 2.07** (Score 9.419); 123 ms/frame, 116.7 ms in DrawPrimitive |
| UT2004 botmatch, 8 bots, spectator | 1280x960x16 | **4 cur / 6 avg FPS** (`stat fps`) |
| UT2004 botmatch, run 1 (ring) | 1280x960x16 | 15.2 M triangles and 1069 clears in about 180 s of play (about 6 fps if it clears once a frame) |
| Halo timedemo | 1280x960 | not finished in 485 s; 26.1 M triangles and 764 clears in the session |
| d3dprobe perf (200 tris/frame) | 640x480x16 / 1024x768x32 | 85.1 / 84.4 fps = the 85 Hz vsync |

Almost all of the frame time is inside DrawPrimitive. That is the runtime's software vertex processing
plus the HAL's DP2 walk and register writes, at roughly 4 µs per triangle. These numbers do not separate
the two. The likely cost, not measured: per-triangle PIO writes through the PCI FIFO with status-register
reads for room. d3dprobe needs a heavy perf scene (thousands of triangles a frame, `--novsync`) to measure
the HAL alone.

## 6. Flight recorder

- **Refusals and errors this session**: the 513/12 refusal (24713), the 2D give-up ERR (25058), and the
  idle-wait plus engine-reset WRN pair (25145/25146). The only earlier WRN is `hwcext 17` from pid
  1892's session (24435).
- **Contexts.** Every D3D context logged `0 unparsed`. Totals: UT2004 run 1 had 26145 DP2 and 15.2 M
  triangles; UT2003 had 59589 DP2, 37.0 M triangles and 3985 clears; Halo had 32670 DP2 and 26.1 M
  triangles; UT2004 run 2 had 3166 DP2 and 1.28 M triangles.
- **`ring_end.txt`** holds the last 1024 entries, from the final windowed probe. They contain no refusal
  or error.
- **Ring size.** At LogLevel 3 the 1024-entry ring wraps within seconds of a game starting. UT2004 alone
  logged about 67,000 events, mostly dbg `CreateSurfaceEx`. So a game's D3D record survives only in a
  fetch made right after it ends (`onbox-11`).

## 7. Tooling problems met (`onbox-11`, `onbox-12`)

- **d3dprobe.**
  - `d3dprobe_run.py` has no `--pace`; `d3dprobe_paced.py` adds `--pace 5000`.
  - The `*_def` tests always fail under D3D8, because a DEFAULT-pool texture cannot be locked. Mark
    them expected-fail or drop them.
  - perf (200 triangles a frame) is too light to measure the HAL.
- **Monitor gate.** It assumes the "highest listed" refresh (100/120 Hz); the runtime chose 85 Hz every
  time.
- **The D3D8 runtime's own revert after a failed CreateDevice cannot be paced** (two switches 156 ms
  apart). Avoid probing refused pairs fullscreen.
- **UT2004** `-benchmark -seconds=60` never ends at these frame rates, and UT2004 ignores WM_CLOSE. Use
  `stat fps` through `-exec=` and the console `exit` instead, which the second run did cleanly.
- **Halo** `-timedemo` did not finish in 8 minutes at this speed.
- **Screensaver.** The 10-minute screensaver was running at the start. Any unattended sweep should
  dismiss it first, or a fullscreen device can lose focus.
- **Downloads.** `DOWNLOAD` answers with status 1 (binary OK). The first version of `agentcall.py` read
  that as an error; fixed.

## 8. What was not done

- **Red Faction.** Its binary has `-level`, `-timedemo` and `-frameratetest` (README-FLEET lines 56-58),
  but it needs its disc mounted. D: currently holds the SoF image, and a Daemon Tools mount is
  registry state, which this run was not allowed to change.
- **No usable switch found** in a quick strings look:
  - Shogo: no direct-world switch in `Client.exe`;
  - Thief II (NewDark): only `fm_*` and `mission` config names;
  - Hidden & Dangerous Deluxe: nothing.

  None of the three was run.
- **No in-game attempt** for Jedi Knight / MotS, System Shock 2, Thief Gold, Rainbow Six, BF1942 or AvP.
  **Max Payne** was not launched (rule a).
- **No fps number for Halo** (timedemo unfinished). There is no UT2004 benchmark result file either; the
  fps comes from `stat fps`.
- **Untested in the stale-FIFO state:** the 16 bpp path, and whether a fresh boot (no Glide session)
  ever shows the in-game noise or white areas. All in-game captures were made after the engine reset,
  with cmdFifo0 off; it read off again after the games (`regs/regs_after_games_1039.txt`).
- **No `--novsync` perf** and no Glide session were run.

## Item index (matches the structured answer)

`onbox-01` stale Glide CMDFIFO (from Glide's not-idle close) blocks D3D triangles · `onbox-02` FIFO window collision → 2D give-up → no
D3D HAL · `onbox-03` X8R8G8B8+D16 matched but refused (plus an unpaced double switch) · `onbox-04` in-game
block noise · `onbox-05` dark lightmaps · `onbox-06` white sky/horizon (UE2) · `onbox-07` Halo glow
square · `onbox-08` UT2003 blue grass + checkerboard · `onbox-09` performance · `onbox-10` d3dprobe
clean-engine results · `onbox-11` tooling · `onbox-12` stability record · `onbox-13` stale VRAM at mode
set · `onbox-14` Unreal titles pick 16 bpp (UT2003 at 640x480) · `onbox-15` not reached.
