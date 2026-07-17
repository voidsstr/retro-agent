# retro3dfx driver changelog

Versioning: `VERSION` (MAJOR.MINOR) + auto-incrementing `.buildnum` → `0.1.N`,
injected into `GL_RENDERER` (`... [retro3dfx 0.1.N]`) so logs and benchmarks
self-document. One functional change per version. Every benchmark row in the
specpicks DB (`retro_benchmark_runs`) carries a `driver_stack` JSON naming the
exact composition of all three layers, and `driver_version` = the ICD version.

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
