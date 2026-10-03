# Serious Sam on the V5 6000: presets measured, and three library defects found (2026-10-03, `.124`)

Stack: vcr-kmd + our h5 Glide (`07c96fd9`) + ICD 0.1.83, 4-chip SLI, no AA
(cfg 5), 1280x960x32. The bench forces vsync off (`FX_GLIDE_SWAPINTERVAL=0`,
`gap_iSwapInterval=0`); played from the desktop, Glide holds vsync at 85 Hz.
Each number is the engine's own demo profiler ("Originally recorded: N
frames in S seconds => F FPS average") over the first auto-demo.

## Presets

| Title | profile TFE/TSE picks | Normal | Quality | Quality costs |
|---|---|---|---|---|
| The Second Encounter (v1.05), 166 s demo | 3Dfx Voodoo5 | **36.2** (low sust. 26.1) | 33.9 (24.8) | 6.4% |
| The First Encounter (v1.00), 103 s demo | 3Dfx Voodoo5 (Mesa Glide ICD entry, below) | **78.7** (56.3) | 74.8 (54.1) | 5.0% |
| The First Encounter, before that entry | Voodoo Graphics (generic) | 86.1 (65.9) | 81.6 (63.5) | 5.2% |

TSE scaling, Normal: 640x480 **52.2**, 1024x768 **41.8**, 1280x960 **36.2**.
Four times the pixels cost 30%, so TSE is mostly CPU-bound: our ICD
transforms every vertex on the CPU, and the VSA-100 has no T&L.

**Decision: both stay on Normal.** Quality costs 5-6% for trilinear filtering,
finer model LOD and denser particles. The user asked for performance first.
TFE's Voodoo5 profile, its own design for this card, is kept rather than the
Voodoo 1 profile it fell into by accident.

## Defects found on the way (all fixed in the library, all boxes)

1. **TFE exited ~20 s into every launch on `.124`.** Our ICD's
   `grSstWinOpenExt` failed with "GLIDE non-fatal ERROR" and no message. An
   instrumented debug Glide (`tfe_glide_debug2.log`) showed the cause:
   `SetCooperativeLevel(EXCLUSIVE|FULLSCREEN)` returned `0x80070057`
   (DDERR_INVALIDPARAMS). The window was a child (style `0x56000000`,
   WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN), and DirectDraw
   exclusive mode needs a top-level window.
   - Serious Engine's canvas is a WS_POPUP when `ogl_bExclusive` is 1 and a
     WS_CHILD when it is 0 (`ViewPort.cpp` `OpenCanvas`).
   - TFE's own auto-adjust set 0. Its `GLSettings.lst` matches a 3dfx card by
     a `"3Dfx*"` vendor; our ICD says "Brian Paul". TFE therefore took
     `Default.ini`, which includes `Initial.ini`, which sets
     `ogl_bExclusive = 0`. TSE's `Initial.ini` sets 1.
   - It passed on 10-01 and then saved the 0. The ICD (0.1.80, 0.1.82, 0.1.83
     all fail), Glide and kernel were ruled out by A/B (`tfe_ab_icd0182/`,
     `tfe_icd0180/`).
   - **Fix:** where the 3dfx card drives the screen, the launchers write
     `ogl_bExclusive=1` into `Game_startup.ini` (read before the canvas
     exists). TFE's `GLSettings.lst` gains "Brian Paul" + Voodoo entries ahead
     of its `"3Dfx*"` block, each naming a script that includes the card's own
     TFE profile and sets `ogl_bExclusive = 1`. The engine's auto-adjust then
     agrees, so an in-game mode change stays exclusive. Proven after a sync:
     PASS, `ogl_bExclusive=1` and the Voodoo5 match saved (`tfe_settled/`).
2. **TFE refused its own demos.** "Cannot play demo because file
   'Demos\auto-demo0001.dem' is older than file 'Levels\11_AlleyOfSphinxes.wld'".
   The staged tree carries the staging copy's times, and the demos were
   copied 12-20 s before the levels. The 5 `.dem` files were re-dated past the
   newest level on the share (`smbclient utimes`, bytes unchanged, md5
   verified). GAMESYNC carries the time to every box.
3. **TSE's refresh setting never applied, on any box.** TFE's `Engine.dll`
   declares `gfx_iRefreshRate`; TSE's declares `gap_iRefreshRate`. TSE logged
   "Identifier 'gfx_iRefreshRate' is not declared" at every start and took
   the driver's default rate. Its launchers now also write
   `gap_iRefreshRate=%FR_SE1HZ%`. Verified: TSE saved `gap_iRefreshRate=85`.

## The bench class was fixed before it could measure anything

`v56k_bench.SeriousSam` had never run on the box. It needed four corrections:
- `+script`, not `+exec`.
- No `StartDemoPlay`. The attract loop plays the demo, and the engine prints
  the profile when a profiled demo finishes.
- The shell's real names: `sam_bFullScreen` and `gap_iSwapInterval`.
- Every variable the bench sets is persistent, so the class keeps and restores
  the player's `PersistentSymbols.ini` and reads it back.

It also needed `up_re` ("Started playing demo"). Without it the first runs were
killed as hangs while the demo played silently (`first_attempt_false_hang/`).

Debug-only side note: the debug Glide's sanity assert fires in `distate.c:1907`
(`combineExtsInUse` mixing standard and extended combine). That check is not
compiled into the release Glide, and nothing broke there.
