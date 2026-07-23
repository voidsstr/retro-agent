# Clean-room Voodoo3 stack — optimization research & plan (2026-07-23)

Goal: push the clean-room stack (retro3dfx-glide + MesaFX retrogl ICD) **past
retail 3dfx glide** on the .124 Voodoo3 (Pentium III Coppermine, 845 MHz, SSE,
**no SSE2**), using modern approaches the 1999-era code never had.

Baseline established: our clean-room glide is at **parity** with retail glide
(46.0 vs 46.3 fps, Q3 demo four, 640×480 fullscreen, same ICD). To *beat* retail
we must improve the shared bottleneck, not just match it.

## On-hardware profiling — where the frame time actually goes (Q3 timedemo)

Method: hold the driver fixed, vary one Q3 knob, read the fps delta. Sound ON is
realistic; several deltas were confounded by sound until controlled.

| Knob change | fps | Reading |
|---|---|---|
| 640×480 baseline | 46.2 | — |
| **320×240** (¼ the pixels) | 46.7 | **≈ no change → CPU-bound, not fill-bound at ≤640** |
| picmip 3 (texture removed) | 46.2 | no change → **texture is NOT a per-frame cost** |
| point-sample (no bilinear) | 46.3 | no change → **fill/filtering not the bottleneck at 640** |
| vertexlight (no lightmaps) | 50.5 | **+9% → per-vertex/2nd-TMU emit cost is real** |
| fastsky + nogun | 52.1 | **+13% → per-surface/per-vertex draw volume is the cost** |
| 1024×768 | 39.2 | fill starts to bite only above 640 |
| sound off (vs on) | +~13 fps @320 | Q3's mixer, single-core P3 — NOT a driver lever |

**Conclusion:** at the primary target (≤640) the box is **CPU-bound in the
per-vertex/per-triangle geometry path** (transform → clip → emit → FIFO-pack).
Texture upload, fill, filtering, and vsync are NOT the bottleneck there. Fill
matters only at 1024+. **Vsync: no real cap** — the ICD already sets
swapInterval=0; the ~60 low-res ceiling is CPU/engine-bound (Agent C confirmed).

## Layer research (3 parallel deep-dives)

### ICD (MesaFX) — per-vertex path
- Build genuinely assembles the SSE `.S` transforms (`X86=1`); SSE runtime-selected
  on Windows. MVP transform = SSE. Color pack already branchless-SSE (`fx_pack_ub`).
  Triangle submit already batched to `grDrawVertexArray`. `GrVertex` is 64 B,
  32-B aligned (streaming stores legal, currently unused).
- **The one remaining scalar-x87 per-vertex hotspot:** cliptest + perspective
  divide `x86/x86_cliptest.S:_mesa_x86_cliptest_points4` — `FLD1; FDIV_S` per
  vertex (~39-cyc serialized). *But an SSE-cliptest attempt regressed once before
  (campaign 0.1.8/0.1.9)* → highest theoretical win, highest risk, do last.
- Fresh, un-tried: SSE color-pack of all 4 channels at once (`fxvbtmp.h`);
  `movntps` + `prefetchnta` vertex emit exploiting the aligned 64-B GrVertex.

### Glide (h3) — per-triangle / FIFO path
- Build uses the **C** trisetup/pack path (USE_X86 unset → `-DGLIDE_USE_C_TRISETUP`;
  no `xdraw*.o`/`cpuid.o`). SSE-scalar `movss`, no `movntq`, no per-triangle divide
  (HW does area setup). Parity with retail = gcc-13+SSE already matches vintage asm.
- **★ De-batch bug (highest confidence):** MesaFX batches Q3 world into one
  `grDrawVertexArray(GR_TRIANGLES,N)`, but `distrip.c:448/549` re-explodes it into
  a per-triangle `grDrawTriangle()` loop → separate GR_BEGIN + indirect
  triSetupProc + FIFO room-check + bookkeeping *per triangle*. The batched packer
  `_grDrawTriangles_Default` (15 verts/packet, one room-check) was unreachable
  because it doesn't cull and MesaFX relies on glide cull. **Retail glide is the
  same source → has the identical de-batch → fixing it beats retail.**
- FIFO stores are scalar `movss` into a WC command surface → `movntq`/MMX
  streaming packer is a real P3 win the vintage asm never had.

### Swap / transfer (Agent C)
- No real vsync lock (swapInterval=0). Lever = deeper swap queue / triple buffer
  for fill-bound 1024 (`fxdd.c:1749 grSetNumPendingBuffers` 2→3).
- **Intel is denied even the 64-bit `movq` texture-download path** — it's
  AMD/3DNow-gated (`femms`); Intel falls to scalar dword C copy
  (`xtexdl_def.c`). An Intel MMX/SSE + `movntq` texture download helps level
  loads / texture-thrash games (NOT the Q3 timedemo — textures stay resident).
- Prereq for all non-temporal wins: the LFB/FIFO BAR must be mapped
  **write-combining** by the display driver (miniport `VideoPortMapMemory`).

## Ranked plan (each = own change, A/B vs baseline, keep only if faster+stable)

1. **Cull-aware batched GR_TRIANGLES packer** (glide) — kill the de-batch.
   `_grDrawTrianglesCull` pre-culls (bit-identical to `_grTriCull`) → packs
   survivors via `_grDrawTriangles_Default`. Gated `FX_BATCH_TRIS` (default on).
   **[IMPLEMENTED 2026-07-23 — A/B in progress]**
2. `movntq`/MMX streaming FIFO packer (glide) — non-temporal WC stores.
3. SSE color-pack (ICD, `fxvbtmp.h`) — 4 channels/`__m128`.
4. `movntps` + `prefetchnta` vertex emit (ICD).
5. SSE cliptest + `rcpps` perspective divide (ICD) — biggest but regressed before; last, careful.
6. Intel `movntq` texture download (glide) — helps loads/texture-thrash, not Q3 timedemo.
7. Triple buffer / deeper swap queue (glide) — helps 1024 fill-bound.

Key files: glide `distrip.c` (448/549), `gdraw.c` (`_grDrawTriangles_Default`,
`_grDrawTrianglesCull`), `gxdraw.c` (`_grTriCull` 198), `fxcmd.h`/`fxhal.h`
(store macros), `fifo.c`. ICD `fxvbtmp.h` (emit), `x86/x86_cliptest.S`,
`x86/sse.c`. Display: H5 miniport `H3.C` `VideoPortMapMemory` (WC attribute).
