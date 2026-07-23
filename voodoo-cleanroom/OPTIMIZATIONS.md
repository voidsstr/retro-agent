# Clean-room Voodoo3 stack — optimization log (complete)

Single place documenting **every** performance/quality optimization attempted on
the clean-room 3dfx stack (`retro3dfx-glide` + MesaFX `retrogl` ICD) on the
fleet's **.124 Voodoo3 box** (Intel Pentium III Coppermine, 845 MHz, SSE, **no
SSE2**; Voodoo3 AGP, XP SP3) — what shipped, what was tested-and-rejected, the
testing method, and the current state.

Companion docs:
- `CHANGELOG.md` — per-version (0.1.N) narrative of each change.
- `OPTIMIZATION-RESEARCH.md` — the 2026-07-23 deep-research + profiling write-up.
- `DEBUGGING-NOTES.md` — the driver bring-up (crash) trail.

## Method (how every optimization was validated)

- **One change per version**, each its own fork branch/commit; version stamped
  into `GL_RENDERER` (`... [retro3dfx 0.1.N]`) so logs/benchmarks self-document.
- **A/B on real hardware** (.124), Q3 `timedemo four`, resolution × quality grid,
  every run recorded in the specpicks DB (`retro_benchmark_runs`) with a
  `driver_stack` JSON naming all three layers.
- **Keep only if it improves fps AND is stable** — inert or regressing changes
  are *not* merged (documented here as honest negatives, not hidden).
- **Quality gate**: in-engine `glReadPixels` screenshot vs baseline (mean pixel
  diff; animation noise ≈ 4/255) so a "faster" change can't silently corrupt.
- Benchmark hygiene (2026-07-23): the `driver-bench` skill now quiesces
  background CPU (AI engine opt-in per agent v1.17.0, plus `rotate_wall`,
  Windows Update, tray apps) — an un-quiesced single-core P3 reads several fps
  low and confounds A/Bs.

## Baseline & the "beat retail" bar

The clean-room glide is at **parity with retail 3dfx glide** (46.0 vs 46.3 fps,
demo four, 640×480, same ICD; or ~67 fps sound-off + quiesced). To *beat* retail
we must improve the shared bottleneck, not just match it.

## MERGED optimizations (shipped wins)

| Ver | Change | Result | Layer |
|---|---|---|---|
| **0.1.2** | **Modern P3/SSE codegen** — `-march=pentium3 -mfpmath=sse` (gcc-13 was emitting x87 for every C hot loop) + branchless-SSE `FX_PACK_UBYTE` color pack (`fxvbtmp.h`, kills a store-forward stall + 2 branches/component) | +0.9% @640 (fxvb.o: 0 → 2729 SSE ops) | ICD |
| **0.1.3** | **Batched triangle submission** — `grDrawVertexArray(GR_TRIANGLES, chunk)` instead of one `grDrawTriangle` DLL call per triangle | +0.9% @640 | ICD |
| **0.1.5** | **Glide state shadow cache** — skip redundant `grTexClamp/Filter/MipMap/AlphaCombine/ColorCombine` re-issues per `glBindTexture` (Q3 rebinds per surface) | +0.7% @640, pristine quality | ICD |
| **0.1.6** | **Swap-interval default** — read `FX_GLIDE_SWAPINTERVAL` from process env with our CRT, default 0 (no vsync wait) | headline **+32% @1024** vs a system-wide `SWAPINTERVAL=1` artifact | ICD |
| **0.1.11** | **Default −0.5 texture LOD bias** (`FX_LOD_BIAS`) — sharpens textures on the V3 bilinear+nearest-mip path (classic 3dfx trick) | quality (no fps cost) | ICD |
| **0.1.12–0.1.19** | **Quake II on our ICD** — root-caused the green-screen crash (Q2 dir shipped no `glide3x.dll` → loader bound an incompatible build → `grSstWinOpen` fault); stage the known-good glide next to `quake2.exe`; `fxwgl.c` window message-pump; `FX_NO_PALETTED_TEXTURE`/`FX_NO_MULTITEXTURE` knobs | Q2 **93.6 fps** vs 75.7 stock `3dfxgl` (+23%) | ICD |
| **0.1.30** | Quality/compat: default gamma 1.3 (`FX_GAMMA`) + forced `GR_DITHER_4x4` (both free, brighter/smoother 16-bit) + exposed V3 ARGB1555 alpha PFDs so alpha-requesting games can create a context | quality/compat | ICD |
| (2026-07-23) | **Clean-room glide P3/SSE build flags** — `build-stack.sh` GLIDEOPT `-march=pentium3 -mfpmath=sse` (stock default was original-Pentium/x87) | perf-neutral but correct target CPU (`gsst.o`: 100% x87 → 41 SSE) | glide |

Bring-up fixes (not perf, but enabled the all-open stack — see DEBUGGING-NOTES):
TLS accessor (`getThreadValueFast` → `TlsGetValue`), lost-context NULL fallback,
GETLINEARADDR-before-ALLOCCONTEXT base map. These made OUR glide render at all.

## TESTED-AND-REJECTED (honest negatives — not merged)

| Ver / when | Change | Verdict | Why |
|---|---|---|---|
| 0.1.4 | Runtime env injection (`_putenv` swap vars) | **INERT** | retail glide snapshots env at DLL load, before ICD code runs |
| 0.1.7 | `-O3 -funroll-loops` (LTO) | **INERT** | hot path already SSE; `-O` can't remove algorithmic cost |
| 0.1.8/0.1.9 | **SSE 4-wide cliptest** + `rcpps`+Newton perspective divide | **REGRESSION** | Vanderhoof's hand x86-asm cliptest beats intrinsics at the CPU-bound res (AoS transpose overhead on P3 > divide savings) |
| 0.1.10 | SSE `movaps` viewport emit | **INERT** | emit is a negligible fraction of the frame |
| 2026-07-23 | **Cull-aware GR_TRIANGLES de-batch** (glide `_grDrawTrianglesCull`) | **INERT** | 67.0/67.5 vs 67.5/67.2 — per-triangle fixed overhead is negligible vs per-vertex work |
| 2026-07-23 | **Intel MMX/`movq` wide-texture download** (glide) | **INERT** | correct (textures render clean) but texture upload is **AGP/PCI bus-bandwidth-bound**; halving CPU store count changes nothing |
| 2026-07-23 | **SSE cliptest re-test** (C-`divss` vs x87 asm) | **REGRESSION** | slower + crashed (wedged the board); the perspective divide is only ~1% of the frame — even a perfect `rcpps` version can't move the needle |
| — | CS 1.6 (GoldSrc) on our ICD | **not supported** | GDI-fullscreen render-into-desktop architecture vs our Glide-exclusive board grab (not an extension issue) |

## The definitive conclusion

**You cannot beat retail here by optimizing driver CPU code, because the driver
CPU code is not the bottleneck.** Profiling (2026-07-23, one Q3 knob varied at a
time) showed: at ≤640 the box is CPU-bound in an *already-SSE, near-optimal*
per-vertex path; texture upload is **bus-bandwidth**-bound; fill only matters at
1024+; vsync is already relaxed; and a big chunk of the frame is Q3's own
game-logic + sound mixing on a single core. Every driver-instruction lever tried
was inert or regressing because none of them is the actual limiter — which is
exactly *why* the clean-room stack matches retail: retail hits the same
system-level ceilings. Genuine further gains need faster hardware (bus/CPU) or a
different workload, not driver micro-optimization.

Where the clean-room stack already **wins**: the all-open stack (0.1.6) beat the
AmigaMerlin retail hybrid and the era's official 3dfx-ICD reference by ~16% at
fillrate-bound 1024×768, with no per-machine env tuning (sane swap defaults in
code); Q2 is +23% vs stock `3dfxgl`.

## Current state (2026-07-23)

- **Deployed on .124**: clean-room glide (0.1.31 lineage) + MesaFX `retrogl`
  ICD. Q3 fullscreen timedemo **66–67 fps @640×480** (sound off, quiesced),
  46/44/39 fps @640/800/1024 (sound on), zero crashes. Q2 88–94 fps. Renders
  correctly (HUD, textures, lighting, in-world text, player models).
- All 2026-07-23 experimental changes **reverted** — the deployed driver is the
  clean baseline. Board healthy after a reboot (a crashed SSE-cliptest test had
  wedged the fullscreen Glide surface — the documented Voodoo3 hazard; reboot
  clears it).
- Fleet AI engine (`retro-infer`) is **opt-in** (agent v1.17.0) so it no longer
  steals cycles from benchmarks/games by default.

## Open / future levers (not yet done)

- **Triple buffer / deeper swap queue** for fill-bound 1024 (`grSetNumPendingBuffers`
  2→3 + 3rd color buffer) — likely marginal since vsync is already off.
- `movntq`/streaming FIFO packer + write-combining verification of the LFB BAR
  (miniport `VideoPortMapMemory` cache attribute) — only helps if the transfer
  is not already bus-saturated.
- **Multi-board / SLI** (Voodoo3 or Voodoo5) — analyzed 2026-07-23, see below.

## Multi-card (multiple Voodoo3 / Voodoo5 boards) — analysis

**Question:** can the drivers use multiple V3/V5 cards on one machine, and would
it speed up rendering? **Answer: no single-game performance benefit.**

Code facts (grounded in the clean-room glide source):
- Glide already **detects up to 4 boards** (`minihwc.h HWC_MAX_BOARDS=4`,
  `fxglide.h MAX_NUM_SST=4`); `grGet(GR_NUM_BOARDS)` (`diget.c:425`) reports the
  count. So multiple cards are *seen*.
- But a rendering context binds **exactly one** board: `grSstSelect` sets
  `current_sst` and the thread GC to `GCs[current_sst]` (`disst.c:186`). There is
  **no cross-board frame combining** in Glide3 — one game renders on one board.
- The SLI code in the **h3 (Voodoo3)** tree is dormant **Voodoo1/2 legacy** —
  `sliDetect` is only consulted for the `VoodooConfig` union (`gpci.c:792`,
  `GR_SSTTYPE_VOODOO/Voodoo2`), never for the Voodoo3 `SST96Config`. The Voodoo3
  is a single-chip, single-board part with **no SLI bridge**: 3dfx moved SLI
  on-board starting with Voodoo4/5, so there is no hardware to link two V3 cards.
- The **h5 (Voodoo5)** tree *does* have active multi-chip SLI (`_grChipMask`,
  `gc->chipmask`, `realNumChips` 2/4 — `gsfc.c:520`, `diget.c:912`). But that is
  the **on-board** VSA-100 SLI (2 chips on a 5500, 4 on a 6000) — it is how a
  single V5 card already achieves its speed, handled by the driver. Two separate
  V5 *cards* is not a 3dfx-supported configuration and would not combine.

Why it wouldn't optimize performance even if built from scratch (SFR/AFR across
two independent boards): (1) .124 is **CPU-bound** on its single P3 at ≤640 —
one CPU feeds both boards, so a second GPU can't lift a CPU-limited frame; (2)
compositing (read board-1's framebuffer over PCI, blit to board-0's display)
is bus-bandwidth-limited and would likely cost more than it saves; (3) only
fill-bound 1024+ could theoretically gain, and the composite + CPU ceiling
negate it. The only 3dfx **multi-card** SLI ever shipped is the **Voodoo2**
(two cards + SLI ribbon, hardware scanline interleave) — not V3/V5.

**What multiple boards *could* usefully enable** (not single-game speed):
multi-monitor (one display per board), or running independent games/contexts on
separate boards. Neither is a rendering-performance optimization.
