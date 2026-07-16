# retro3dfx — an open, top-to-bottom Voodoo driver stack we own and optimize

The goal: run **Unreal Tournament** (Glide) and **Quake 3** (OpenGL) on real
Voodoo 3 / Voodoo 5 hardware, with **every layer from the PCI register writes up
to OpenGL built from source we control** — so we can optimize the whole stack past
what 3dfx shipped. Nothing proprietary is in the render path.

## Ownership map
🟢 = **ours** (build / fork / optimize) · ⚪ = not ours (hardware, games, OS, toolchain)

```
   ⚪ Quake 3 (OpenGL)          ⚪ Unreal Tournament (Glide)
        │                            │
   🟢 retro3dfx-gl  ────────────────┐│   MesaFX fork → opengl32.dll.
      (our MesaFX fork)             ││   The full OpenGL geometry pipeline
        │  gr* calls                ││   (transform/light/clip) — biggest
        ▼                           ▼▼   optimization surface (no HW T&L)
   🟢 retro3dfx-glide  →  glide3x.dll / glide2x.dll
      (our Glide fork)              triangle setup, FIFO, texture, SIMD paths
        │  register / FIFO writes
        ▼
   🟢 retro3dfx-disp  →  the display driver that serves the HWCEXT escape
      (our cooperative driver)      so our Glide finds+maps the card
        │
   ⚪ Voodoo 3 / Voodoo 5 hardware
```

## What builds today (from our forks, on this Linux host)
`./build-stack.sh` produces into `out/`:
| 🟢 Artifact | From | Status |
|---|---|---|
| `glide3x.dll` (Voodoo4/5) + `glide3x_h3.dll` (Voodoo3) + `glide2x.dll` | **retro3dfx-glide** | ✅ builds |
| `opengl32.dll` (MesaFX OpenGL over our Glide — for Q3) | **retro3dfx-gl** | ✅ builds, imports our `glide3x`, exports OpenGL |

The **retro3dfx-disp** display driver (`retro3dfx-disp/`) needs the platform DDK;
its **escape server is written + host-tested**, the hardware/chassis parts are
scaffolded for the fleet DDK build.

## Our forks (real GitHub forks under `voidsstr`)
| 🟢 Fork | ⚪ Upstream | Ships as |
|---|---|---|
| [voidsstr/retro3dfx-glide](https://github.com/voidsstr/retro3dfx-glide) | sezero/glide | `glide3x.dll` / `glide2x.dll` |
| [voidsstr/retro3dfx-gl](https://github.com/voidsstr/retro3dfx-gl) | sezero/MesaFX-6.2 | `opengl32.dll` |
| retro3dfx-disp | (ours; models: Device3Dfx, RISCyVoodoo, vmdisp9x) | display driver |

See `FORKS.md` for exact fork points. ⚪ Not ours: the Voodoo silicon, the games,
Windows/DirectX/GDI, and the Microsoft DDK (we use it to build, don't own it).

## Build
```bash
./build-stack.sh              # glide + mesa from our forks -> out/
./build-stack.sh --debug      # verbose GDBG glide for tracing
# retro3dfx-disp: build via the fleet DDK (provisioning/ddk/), see its README
```

## Bring-up path (each step benchmarkable on the fleet)
1. **retro3dfx-disp escape server** ✓ (written+tested) → **disp_hw BAR mapping** →
   our Glide detects+maps the Voodoo.
2. **retro3dfx-glide** ✓ → **UT runs** on Glide.
3. **retro3dfx-gl** ✓ → **Q3 runs** on OpenGL→our Glide.
4. **Optimize** — the loop: edit a fork → `build-stack.sh` → `scripts/benchmarks/
   benchmark_runner.py` → real FPS delta on the Voodoo3/5.

## Where the optimization lives (all in 🟢 our code)
- **retro3dfx-gl** geometry (Q3, all OpenGL) — the software T&L the card can't do.
- **retro3dfx-glide** — triangle setup / vertex / FIFO / texture; the half-tuned
  SSE/SSE2 paths; TMU texture caching; VSA-100 FSAA/SLI (`gaa.c`).
- **Compiler** — gcc 13 `-mtune=<exact CPU>` vs era MSVC6; SIMD dispatch.
- **retro3dfx-disp** — BAR mapping, mode-set (minor, but ours).
