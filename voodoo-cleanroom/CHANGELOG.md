# retro3dfx driver changelog

Versioning: `VERSION` (MAJOR.MINOR) + auto-incrementing `.buildnum` → `0.1.N`,
injected into `GL_RENDERER` (`... [retro3dfx 0.1.N]`) so logs and benchmarks
self-document. One functional change per version. Every benchmark row in the
specpicks DB (`retro_benchmark_runs`) carries a `driver_stack` JSON naming the
exact composition of all three layers, and `driver_version` = the ICD version.

## 0.1.52 — robustness, shadowing, and five refuted optimisation theories (2026-08-29)

**`-static-libgcc` on the ICD link.** Any use of a libgcc helper — a 64-bit
divide is enough — silently adds an import on `libgcc_s_dw2-1.dll`, which is
not present on the retro boxes. The ICD then fails `LoadLibrary` and the game
reports only `could not load "retrogl"`, with nothing pointing at a missing
DLL. Hit while adding profiling counters; worth having permanently.

**`-march` and `-mtune` are separable** (`TUNE ?= $(CPU)`). They were welded to
one variable, so the ICD could not be scheduled for the CPU that runs it
without also raising the instruction-set floor and faulting on .124's Pentium
III. Now `-march=pentium3 -mtune=pentium4`. **Measured: exactly neutral**
(57.2 either way) — kept for the capability, not for a gain.

**`grTexCombine` is shadowed.** It was the one texture-state call the 0.1.5
Glide shadow missed, and on a 2-TMU part it is issued twice per bind. No
regression (57.2), no measurable gain — the profiler then explained why.

**`FX_PROFILE=1`** adds a per-frame counter/cycle dump to `C:\retrogl.log`.

### The 22 ms multitexture cost: five theories, all refuted

Enabling single-pass multitexture cuts per-pixel fill **65%** but adds ~22 ms
of fixed per-frame CPU. Each of these was implemented or switched and measured
on hardware, and none of them is the cause:

| theory | test | result |
|---|---|---|
| texture thrashing in a 4MB bank | `gl_picmip 0/1/2` (16× less texture RAM) | 34.0 → 34.1 fps. No. |
| `glClientActiveTextureARB` flush | skipped it | 32.0 → 32.9. No. |
| per-vertex texcoord submission | dropped the 2nd texcoord | 31.9 → 32.3. No. |
| redundant `grTexCombine` | shadowed it | profiler shows 0 issued/frame. No. |
| Mesa x86 vertex codegen | forced on and off | 30.2 vs 30.5. No. |

The profiler settles what it is *not*: **2 texture-setup calls per frame**, not
thousands, and ~0 cycles in setup. So the cost is not per-surface state at all.
It is flat against resolution (35.6 / 33.9 / 30.2 fps at 512×384 / 640×480 /
800×600), so it is per-frame or per-vertex CPU, still unattributed.

Worth continuing: our per-*pass* fill already beats the MiniGL
(2.55e-5 vs ~3.26e-5 ms/px), so single-pass is the only thing between us and
overtaking it — the model says ~127 fps if the 22 ms goes.

## 0.1.44 — stop advertising an extension we do not accelerate (2026-08-29)

`GL_EXT_point_parameters` is now **withdrawn by default** (`FX_POINT_PARAMS=1`
restores it). We advertised it; Mesa implements distance-attenuated points by
expanding each one into geometry, so an application that takes the extension
gets a *slower* path than its own fallback. 3dfx's MiniGL never advertised it —
Quake II logs `...GL_EXT_point_parameters not found` against the MiniGL and
`...using GL_EXT_point_parameters` against us, then draws its particles the
expensive way.

Measured on .171 (Q2 demo1, 640x480, vsync off, 4 runs per arm, same binary,
zero variance in both):

| | fps |
|---|---|
| advertised (old default) | 51.0 |
| **withdrawn (new default)** | **57.2** |

**+12.2%**, and it needs no env var to get it. Standing against the stock
MiniGL's 90.7: we move from 56% to **63%**.

## 0.1.42 — wglGetProcAddress consulted Mesa before us (2026-08-29)

`wglGetProcAddress` called `_glapi_get_proc_address()` **before** searching our
own `wgl_ext[]` table. Mesa's glapi does not fail on an unknown `gl*` name — it
**synthesizes a dispatch stub** — so it answered for `glSelectTextureSGIS` with
a stub wired to nothing, and the real implementation below it was never
reached. Quake II called that stub the instant multitexture engaged and the
demo1 timedemo stopped completing at all (>180s). Our table is now searched
first; everything we do not implement still falls through to glapi unchanged.

This is a correctness fix independent of SGIS: **any** entry point we add to
`wgl_ext[]` was previously unreachable if its name began with `gl`.

With the shim actually reachable, single-pass multitexture now runs to
completion — and is **slower**, which is a real result rather than a hang:

| config | fps |
|---|---|
| SGIS off (two-pass, shipped default) | **51.0** |
| SGIS on (single-pass) | **30.9** |
| stock 3dfx MiniGL | 90.7 (re-verified, was 91.1) |

So the missing extension was never the whole story. The suspected cause is
texture-bank placement on the split-TMU Voodoo 2: with multitexture engaged the
base-texture pool is confined to one 4MB bank, and Q2's working set then
thrashes. That is the next thing to fix; SGIS stays behind
`FX_SGIS_MULTITEXTURE` until it is a win.

## 0.1.41 — Voodoo 2 (cvg) lane: the stack runs on a Voodoo 2 (2026-08-29)

First execution of the clean-room stack on **Voodoo 2** silicon (.171, Pentium 4
2.8GHz, XP SP3, 12MB card = 4MB FB + 2 TMUs x 4MB). `GL_RENDERER` reports
`Mesa Glide v0.62 Voodoo2 [voodoo-cleanroom 0.1.41]` — MesaFX detects the chip
correctly via `GR_SSTTYPE_Voodoo2`.

- **New `cvg` build lanes** (`FX_GLIDE_HW=cvg`) for glide3x and glide2x →
  `out/glide3x_cvg.dll`, `out/glide2x_cvg.dll`, both dual-ABI. CVG is 3dfx's
  codename for the Voodoo 2; the vintage `retro-3dfx` tree cannot drive this
  card at all (its INFs cover DEV_0003/0005/0009/000B — no DEV_0002).
- **Fixed: the cvg relink emitted no DLL.** `dual_abi_relink` globbed only the
  chip directory, but glide3x/cvg links the SHARED `swlibs/newpci/pcilib`
  objects — `fxnt.c`, the NT layer that opens `\\.\GpdDev` (fxgpio.sys) and
  `\\.\MAPMEM` (fxptl.sys). The relink failed, gcc deleted its output, and the
  next `cp` aborted the script under `set -euo pipefail`. Both relink helpers
  now take an extra object dir.
- **No display driver needed.** A Voodoo 2 is a 3D-only passthrough card
  (Class=MEDIA), so the Intel 865G keeps 2D and `vcr-disp` is out of scope —
  this is the first box where the whole 3D stack can be ours without one.
- **`-mtune=pentium4`** for the cvg lane (`-march=pentium3` retained so one
  artifact still runs on .124's Pentium III).
- **GL_SGIS_multitexture: implemented but OPT-IN** (`FX_SGIS_MULTITEXTURE=1`),
  see `patches/mesafx-sgis-multitexture.patch`. Quake II predates
  ARB_multitexture and probes only the SGIS name, so the stock MiniGL gets
  single-pass lightmapping and we do not. Advertising SGIS does flip Q2 over
  (`...using GL_SGIS_multitexture` in qconsole.log) but the timedemo then never
  finishes (>180s vs 13.5s). Not root-caused, so it stays off by default.

**Measured** (Quake II demo1, 640x480, vsync OFF, 689 frames, zero variance):

| renderer | fps |
|---|---|
| stock 3dfx MiniGL (`3dfxgl`) | **91.1** |
| our MesaFX ICD (`retrogl`) | **51.0** |
| Intel 865G onboard (control) | 58.8 |

The MiniGL implements only what Quake needs; part of that gap is structural.
Closing the rest is the open work — the SGIS path above is the main lead.

## Stack composition legend

A "driver version" here is OUR OpenGL ICD (`retro3dfx-gl`, MesaFX 6.2 fork).
Until 0.1.6 every benchmark ran on a **hybrid** stack:

| Layer | 0.1.1–0.1.6 benchmarks | target (top-to-bottom ours) |
|---|---|---|
| XP display driver (kernel) | **AmigaMerlin 2.9** (retail) | our `3dfxvsm.sys`+`3dfxvs.dll` (deployed build from the private retro-3dfx repo) |
| glide3x.dll (3D HAL) | **AmigaMerlin retail** (`_grFoo@N` underscore ABI) | retro3dfx-glide (our sezero/glide fork) |
| OpenGL ICD (`retrogl.dll`) | **ours** — retro3dfx-gl vN | ours |

The hybrid required linking our ICD against the *retail* glide import lib
(`build-mesafx-retail.sh`); the all-ours stack uses the default
`build-stack.sh` link (our glide exports both `grFoo` and `grFoo@N`).

## Versions

### 0.1.1 — baseline (versioning introduced)
- **Change:** none functional; version stamp added to `GL_RENDERER`, buffer
  widened 64→96B for the marker.
- **Why:** benchmarks must self-document the driver build.
- **Results (Q3 1.32 `timedemo four`, 16bpp, P3-845/Voodoo3/XP, hybrid stack):**
  640x480 **53.7**, 800x600 50.4, 1024x768 **38.7** fps.
  With `FX_GLIDE_SWAPINTERVAL=0` env: 57.6 / — / 51.0.

### 0.1.2 — modern compiler codegen (fork `1bfd219`)
- **Change:** build flags `-march=$(CPU) -mfpmath=sse -DNDEBUG` (was
  `-mtune` only → gcc13 emitted pentiumpro **x87** for every C hot loop);
  `FX_PACK_UBYTE` branchless SSE color pack in `fxvbtmp.h` (7 sites) replacing
  `UNCLAMPED_FLOAT_TO_UBYTE` (store-forwarding stall + 2 branches/component);
  host-tool `gen_matypes` rule filters target-only flags (64-bit host gcc
  errors on `-march=pentium3`).
- **Why:** the audit found fxvb.o had **zero** SSE instructions; P3 has SSE1.
  After: 2729 SSE scalar ops in fxvb.o.
- **Result:** 640x480 54.2 (**+0.9%** vs 53.7). Honest read: most of the
  640x480 frame is Q3 engine + Glide, not our C loops.

### 0.1.3 — batched triangle submission (fork `3a4e790`)
- **Change:** `fx_render_vb_triangles` → one
  `grDrawVertexArrayContiguous(GR_TRIANGLES, ...)`; indexed path
  `fx_render_triangles_elts_batched` submits 768-vert pointer-array chunks via
  `grDrawVertexArray` instead of one `grDrawTriangle` DLL call **per triangle**.
- **Why:** cut DLL-boundary overhead (~1 call/tri on Q3 world geometry).
- **Result:** tuned-env 640x480 58.1 (**+0.9%** vs 57.6), 1024x768 51.2 (flat).
  Lesson: retail glide3x loops per-triangle internally, so only the call
  boundary was saved — smaller win than estimated.

### 0.1.4 — swap-default env injection (fork `424bd32`) — **INERT**
- **Change:** set `FX_GLIDE_SWAPINTERVAL=0` etc. via `_putenv` +
  `SetEnvironmentVariableA` before `grGlideInit` when the user hasn't.
- **Why:** make the measured +7%/+32% swap tuning a driver default.
- **Result:** no effect. Retail glide3x is static-CRT and snapshots the
  environment at **DLL load** — before any ICD code can run. Kept (harmless;
  correct for late env readers) but superseded by the machine-env deploy step.

### 0.1.5 — Glide state shadow cache (fork `204555d`)
- **Change:** TU-local shadow of `grTexClampMode/FilterMode/MipMapMode/
  TexSource/grAlphaCombine/grColorCombine` in `fxsetup.c`; identical calls
  skipped. Reset on `grSstWinOpen` and `grGlideSetState` (MakeCurrent).
- **Why:** every `glBindTexture` re-issued the full 8-10-call register set;
  Q3 rebinds per surface.
- **Result:** 640x480 54.9 no-env (**+0.7%** vs 54.2). Quality screenshot
  (in-engine glReadPixels, q3dm1): pristine — no regressions from 0.1.2-0.1.5.

### 0.1.6 — swap-interval env-read fix (fork `06497b2`)
- **Change:** read `FX_GLIDE_SWAPINTERVAL` from the **process env** with our
  CRT (`getenv`) instead of Glide's `grGetRegistryOrEnvironmentStringExt`;
  default 0 when unset.
- **Why:** bisect proved `FX_GLIDE_SWAPINTERVAL=0` alone = 51.3 @1024 (+32%);
  the other tuned vars were inert. Via Glide's reader, a system-wide
  `FX_GLIDE_SWAPINTERVAL=1` (planted by the 3dfx tools install) reached us.
- **Result:** still 38.7 @1024 — because the retail **glide3x itself** also
  reads the env var from its own load-time snapshot and **ignores the
  `grBufferSwap(interval)` argument**. No ICD-side code can override it.
  Machine-wide env (`HKLM\...\Session Manager\Environment`) is the working
  mitigation for the hybrid stack; on OUR glide3x the default is ours in code.

## Swap-interval saga (summary of findings)

1. Launcher env `FX_GLIDE_SWAPINTERVAL=0` → 51.3 fps @1024 (works; process
   env at creation is in every snapshot).
2. Runtime env injection from ICD (0.1.4) → inert (glide snapshots at DLL load).
3. Registry `Services\{3dfxvs,banshee,3Dfx}\Device0` → inert for this build.
4. ICD-side interval fix (0.1.6) → inert (glide ignores the swap argument).
5. Root cause of the "mystery vsync": the environment actually contains
   `FX_GLIDE_SWAPINTERVAL=1` system-wide (3dfx tools artifact) — glide honors
   it. Overwritten to 0 in `Session Manager\Environment` on .124.

## MILESTONE 2026-07-17 — all-retro3dfx stack live, beats AmigaMerlin

Our XP kernel display driver (deployed build from the private retro-3dfx repo)
replaced AmigaMerlin on .124 via SetupAPI (`deploy-3dfx-driver` skill). Desktop
2D at 1024x768x32@75 correct. Our glide3x build (underscore ABI) binds the
existing retail-linked MesaFX 0.1.6 without a rebuild. Q3 renders pristine
(in-engine screenshot parity with the hybrid baseline — mean pixel diff
4.1/255 = animation noise).

## Cumulative scoreboard (Q3 timedemo four 16bpp)

| Config | 640x480 | 1024x768 |
|---|---|---|
| HYBRID 0.1.1, env untouched (`SWAPINTERVAL=1` system-wide) | 53.7 | 38.7 |
| HYBRID 0.1.6, env untouched | 54.2 (+0.9%) | 38.7 |
| HYBRID 0.1.6 + `FX_GLIDE_SWAPINTERVAL=0` | ~58 (+8%) | ~51 (+32%) |
| **ALL-RETRO3DFX 0.1.6, no env tuning** | **58.8 (+9.5%)** | **51.3 (+32.6%)** |
| Era references (P3-850/933 + V3 3000, 3dfx ICD) | 75-91 | **44.3 — we beat this** |

The all-ours stack needs no tuning: our glide3x's swap defaults are sane in
code. At fillrate-bound 1024x768 we exceed the era's official 3dfx ICD
reference by ~16%.

Remaining CPU-side gap at 640x480 (~ -30%) is the target of the queued deep
work: SSE intrinsics vertex emit, SSE 4-wide cliptest (rcpps + Newton-Raphson
replacing serial x87 fdiv), end-to-end ubyte colors.

## Optimization campaign 2026-07-17 (0.1.7–0.1.11) — the "queued deep work", answered

The queued CPU-side work above was attempted as one fork branch per idea, each
A/B'd on `.124` (P3-845 Voodoo3) against the 58.8/51.3 baseline across the
resolution × quality grid, every run in specpicks. **Result: the vertex path is
already near-optimal — none of the fps optimizations merged.** This is a real,
useful finding, not a failure: the stack that already beat AmigaMerlin and the
era 3dfx ICD has little vertex headroom left on this card.

| Ver / branch | Change | 640x480 | Verdict |
|---|---|---|---|
| 0.1.7 `opt/lto` | `-O3 -funroll-loops` (was `-O2`) | 58.7 | **INERT** — the hot path is already SSE; `-O` can't remove the algorithmic cost. Not merged. |
| 0.1.8/0.1.9 `opt/sse-cliptest` | SSE 4-wide cliptest + `rcpps`+Newton perspective divide (transpose-load in 0.1.9) | 38.7 | **REGRESSION** — Josh Vanderhoof's hand-tuned x86-asm cliptest beats C intrinsics at the CPU-bound res. Renders correctly (4.5/255). Confirms the cliptest *is* a real hot-path lever — just already optimal. Dropped. |
| 0.1.10 `opt/sse-emit` | SSE `movaps` viewport emit (vs 3 scalar MACs) | 58.7 | **INERT** — emit is a negligible fraction of the frame. Renders identically (0.60/255). Not merged. |
| **0.1.11 `opt/lod-bias`** | **QUALITY: default `-0.5` texture LOD bias** | (quality) | **MERGE candidate** — sharpens textures on the V3 bilinear+nearest-mip path (classic 3dfx trick); `FX_LOD_BIAS` env-tunable. |

**Conclusion for the MesaFX/V3 lane:** the transform is SSE (`sse.c`), the
cliptest is tuned asm, the emit is small, and the V3 is single-TMU (no
multitexture single-pass lever, unlike the Voodoo5 lane). The remaining wins on
this card are **quality** (LOD bias) and higher-res/quality coverage, not vertex
fps. The 640x480 gap to the era P3-850/933 references is CPU-clock (845 MHz) +
engine, not driver inefficiency.

## Q2 support + game-integration findings 2026-07-18 (0.1.12–0.1.19)

**Quake II now runs on our MesaFX ICD** (was stock `3dfxgl` only). Root cause of
the prior green-screen / "driver stopped working" crash: `retrogl.dll` binds
`glide3x.dll`, and the Q2 dir shipped none, so the loader resolved an incompatible
build and `grSstWinOpen` faulted. Fix = stage the known-good retail `glide3x.dll`
(344064 B, AmigaMerlin) next to `quake2.exe`. Also added a window message-pump
before `grSstWinOpen` in `fxwgl.c` (harmless; a freshly-shown ref_gl window's
activation messages were queued). Full diagnosis: `retro3dfx/DEBUGGING-NOTES.md`.
- Q2 @640×480×16: **93.6 fps** on our ICD vs **75.7** stock 3dfxgl (+23%),
  and stable. Res sweep to Voodoo3 max: 640=93, 800=69, 960=51, 1024=47,
  1152=38, 1280=32, 1600×1200=20.8 fps.
- New env knobs (default OFF): `FX_NO_PALETTED_TEXTURE`, `FX_NO_MULTITEXTURE`
  (hide those extensions to force an engine onto the RGBA / single-texture path).

**Counter-Strike 1.6 (GoldSrc): not supported on our ICD** — architecture
mismatch (GoldSrc GDI-fullscreen-mode + render-into-desktop vs our Glide-exclusive
board grab). hl.exe exits after GL init. Runs on the stock `gldrv\3dfxgl.dll`
MiniGL. Not an extension issue (ruled out paletted + multitexture). Details in
DEBUGGING-NOTES.md.

**Q3 unchanged**: 57.9 fps @640 (tied with the 0.1.11 best); high-res sweep added
to 1600×1200 (22.9 fps). The V3 vertex/transform path remains near-optimal for fps.

## Refresh + cursor session 2026-08-03 (0.1.34–0.1.35)

### 0.1.34 — fullscreen refresh: monitor-max instead of hardcoded 60Hz
- **Change:** `fxapi.c fxMesaCreateBestContext()` no longer hardcodes
  `GR_REFRESH_60Hz`. New `fxBestRefresh(w,h)`: env override
  (`FX_GLIDE_REFRESH_RATE` / `SSTV2_REFRESH_RATE` / `MESA_FX_REFRESH`, Hz), else
  the monitor's max refresh for that WxH from `EnumDisplaySettings`
  (EDID-filtered — can't exceed monitor caps), snapped DOWN to the nearest
  `GR_REFRESH_*` Glide has a timing for. If `grSstWinOpen` still rejects the
  rate, one retry at 60Hz (a bad rate degrades, never fails the context).
  `FX_GLIDE_REFRESH_RATE=60` restores the old behaviour.
- **Why:** Glide programs the video timing itself in fullscreen — GoldSrc's
  `-freq`, GDI mode sets, and XP's refresh dialogs are all bypassed, so every
  GL game ran at 60Hz on a 100Hz-capable monitor (.124 CS 1.6 verified 60→100Hz
  via retrogl.log: `grSstWinOpen ref=6`, open OK).
- **Test:** `tests/native/test_fx_best_refresh.c` (snap table mirror).

### 0.1.35 — fullscreen software cursor overlay (+ FX_DUMP_FRONT debug dump)
- **Change:** `fxapi.c fxDrawCursorOverlay()` — when `GetCursorInfo` says the
  cursor is showing, stamp a classic 11×19 arrow (black outline / white fill,
  transparent elsewhere) into the back buffer via `grLfbLock` right before
  `grBufferSwap`. Desktop→Glide coordinate scaling, full edge clipping,
  565/1555/8888 paths. `FX_CURSOR=0` disables. Gameplay hides the OS cursor, so
  the overlay costs nothing in-game. Also `fxDumpFrontBuffer()`:
  `FX_DUMP_FRONT=<path>` dumps the front buffer raw every 64th swap (GDI
  screenshots can't see Glide scanout; this is the remote verification path).
- **Why:** fullscreen Glide scanout never composites the GDI/hardware cursor
  plane — CS 1.6's GL menu pointer was invisible (D3D mode showed it). Verified
  on .124: front-buffer dump shows the arrow at the clicked position in the CS
  menu.
- **Test:** `tests/native/test_fx_cursor_overlay.c` (bitmap + stamp/clip mirror).

## glide2x bring-up session 2026-08-04 (Unreal Gold 3dfx renderer)

### glide2x: XP bring-up fixes + dual-ABI exports (fork 79ee51e)
- **Problem:** selecting the 3dfx renderer in Unreal Gold (GOG) hard-wedged
  .124 — the GOG install ships **nGlide** as game-local `glide2x.dll`, whose
  failing grSstOpen attempts froze the chip (physical power cycle needed).
  Our own glide2x had never worked either: no `_grFoo@N` (MSVC) exports, and
  a GPF inside `grGlideInit`.
- **Fixes:**
  1. `build-stack.sh`: dual-ABI relink for glide2x (same as glide3x's) — the
     Glide2-era games are MSVC-linked and import `_grFoo@N`.
  2. `glide2x/h3/minihwc/minihwc.c` (fork 79ee51e): port of the verified
     glide3x XP fixes — GETLINEARADDR prime before ALLOCCONTEXT, zero-base /
     failed-escape guards in hwcMapBoard, plus clearing
     `linearInfo.initialized` on failure (Unreal's error callback doesn't
     exit; hwcInitRegisters' only defense is that flag).
- **Deployed:** game-local `Unreal Gold\System\glide2x.dll` (nGlide kept as
  `.nglide` backup), `system32\glide2x.dll` (2003-era copy kept as
  `.old2003`).
- **Verified on .124:** standalone Glide2 exerciser full pass (init → query →
  WinOpen → 60 swaps → close, desktop restored); Unreal Gold fullscreen Glide
  640x480x16 **@100Hz**, stable.
- **Operational rule (hard-won): NEVER `taskkill /f` a fullscreen Glide2
  game** — killing mid-FIFO-packet wedges the chip beyond the display
  driver's bounded waits (bus-level hang, physical power cycle). Exit via the
  game's own quit path.
- **Test:** `tests/native/test_glide2x_mapboard_guards.c`.
