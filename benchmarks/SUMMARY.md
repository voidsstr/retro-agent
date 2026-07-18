# retro3dfx Benchmark Summary — Voodoo3 (.124 / ADMIN, x86 ~845MHz)

All results from the `driver-bench` harness, recorded in the specpicks DB
(`retro_benchmark_runs`, `machine_id=1`). fps = average of the demo timedemo runs
at 16-bit color (the Voodoo3 renders 16bpp only). Newest driver highlighted.

## Current driver: retro3dfx MesaFX ICD **0.1.19** (`retrogl.dll`)

Release highlights vs 0.1.11: **Quake II now runs on our ICD** (was stock 3dfxgl
only) — see the fxwgl message-pump + glide3x-in-dir fixes and
`FX_NO_PALETTED_TEXTURE`/`FX_NO_MULTITEXTURE` env knobs. Q3 unchanged (57.9 fps),
Q2 93.6 fps @640×480 (+23% vs stock 3dfxgl 75.7).

### Quake III Arena (`timedemo four`, quality=default)
| Resolution | fps (0.1.16) | Notes |
|-----------|-------------|-------|
| 640×480   | 58.0 | tied with best (0.1.6/0.1.11); CPU/transform-bound |
| 1152×864  | 42.6 | |
| 1280×1024 | 33.1 | |
| 1600×1200 | 22.9 | Voodoo3 max 3D res; fillrate-bound |

### Quake II (`timedemo demo1`, our ICD via `gl_driver retrogl`)
| Resolution | fps (0.1.16) | Notes |
|-----------|-------------|-------|
| 640×480   | 93.1 | vs stock `3dfxgl` 75.7 — **+23% and stable** |
| 800×600   | 69.2 | |
| 960×720   | 51.0 | |
| 1024×768  | 47.1 | |
| 1152×864  | 38.3 | |
| 1280×960  | 31.6 | |
| 1600×1200 | 20.8 | Voodoo3 max |

Quality screenshot (base1, 640×480): `benchmarks/q2_retrogl_0.1.16_base1_640x480_quality.png`
— textures render correctly with our LOD-bias sharpening (0.1.11).

## Driver version history on Q3 @640×480 (regression tracking)
| Ver | fps | What changed |
|-----|-----|-------------|
| 0.1.1 | 55.6 | initial retail-link ICD |
| 0.1.3 | 56.1 | |
| 0.1.6 | 58.3 | `FX_GLIDE_SWAPINTERVAL` vsync-off fix (big win at high res) |
| 0.1.7 | 55.4 | SSE viewport/texcoord emit (inert → not merged) |
| 0.1.8 | 38.6 | ⚠ SSE cliptest AoS→SoA gather — **severe regression, dropped** |
| 0.1.9 | 38.8 | cliptest transpose fix attempt — still regressed, **dropped** |
| 0.1.10 | 55.8 | reverted to asm cliptest |
| 0.1.11 | 58.8 | **LOD-bias −0.5 texture sharpening (quality win), merged** |
| 0.1.16 | 58.0 | Q2 context fix (fxwgl msg pump + glide3x-in-dir); no Q3 change |

**Established:** the V3 MesaFX vertex/transform path is near-optimal for fps at
640 (3 rigorous A/B branch tests: SSE cliptest, SSE emit, LTO/PGO all inert or
regressed). The real levers are (a) **quality** (LOD bias, merged) and
(b) **fillrate at high res** (hardware-bound, not driver-bound). Optimization
effort now targets texture/blend state selection and per-game correctness.

## Games status on our ICD
- **Quake III**: ✅ working, benchmarked, quality-verified.
- **Quake II**: ✅ **now working on our ICD** (was stock 3dfxgl only) — see
  `retro3dfx/DEBUGGING-NOTES.md` for the glide3x-in-dir root cause.
- **Counter-Strike 1.6**: ❌ **not supported on our ICD** — GoldSrc's fullscreen
  model (GDI `ChangeDisplaySettings` + render into the desktop framebuffer)
  conflicts with our ICD's Glide-exclusive board grab, so hl.exe exits after GL
  init. Runs on the stock `gldrv\3dfxgl.dll` MiniGL instead. Full analysis in
  `retro3dfx/DEBUGGING-NOTES.md` (2026-07-18). Q2/Q3 are the ICD benchmark games.
