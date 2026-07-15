# 3dfx — Glide rebuild, benchmark harness, and the fxD3D clean-room driver

Everything for optimizing/extending 3dfx Voodoo drivers for the fleet (the
Voodoo5 5500, the incoming Voodoo5 6000, Voodoo3), built with the same mingw
toolchain as `retro_agent.exe`. **Clean-room throughout** — open Glide release,
public DDK/DDI, public register docs. ***REMOVED*** is not used.

Landscape + rationale: [`docs/3dfx-drivers.md`](../../docs/3dfx-drivers.md).
D3D driver design: [`docs/3dfx-d3d-hal-design.md`](../../docs/3dfx-d3d-hal-design.md).

## What's here

```
build-glide.sh    cross-compile the open Glide DLLs:
                    out/glide3x_h5_voodoo5.dll (VSA-100, incl. 6000 SLI/FSAA)
                    out/glide3x_h3_voodoo3.dll (Voodoo3)
                    out/glide2x.dll            (Napalm Glide2)
glide-sdk/        vendored open Glide headers + h3/h5 import libs (build inputs)
gfxbench/         on-card mode/option test + benchmark  -> gfxbench.exe
d3dhal/           fxD3D: clean-room Direct3D 6/7 HAL, translated to Glide
driver/           fxD3D host display driver (INF + NT/9x hosts + DDI glue)
push_gfxbench.py  deploy gfxbench to a fleet Voodoo box, run it, pull the CSV
Makefile          build everything that is DDK-independent, run all host tests
```

## Build everything (DDK-independent parts)

```bash
make            # glide DLLs + gfxbench.exe + fxD3D core + driver glue + run tests
make test       # just the host unit tests (d3dhal + driver glue)
make glide      # just the Glide DLLs
make clean
```

The **full loadable display driver** (`driver/nt`, `driver/win9x`) needs the
platform DDK — see `driver/README.md`.

## The pieces, and how they fit

| Layer | Dir | Status |
|---|---|---|
| Glide3/2 DLLs (the native API + hardware knowledge) | `build-glide.sh` | **built** (h3+h5+glide2) |
| Glide backend seam (`gb_*`) | `d3dhal/glidebackend.*` | **built, compiles vs real SDK** |
| On-card validator + benchmark | `gfxbench/` | **built** → `gfxbench.exe` imports `glide3x.dll` |
| fxD3D D3D→Glide core (DP2 dispatch, state/tex/prim) | `d3dhal/` | **built + host unit test PASS** |
| DDI glue (Windows callback → fxD3D) | `driver/ddi_glue.*` | **built + host glue test PASS** |
| Host display driver + INF (loads/registers fxD3D) | `driver/nt`, `driver/win9x` | skeleton + INF; **DDK build pending** |

Full data flow on a card:
`D3D game → DirectX runtime → fxD3D (DrawPrimitives2 → fxd_dp2_execute) → Glide → 3dfx silicon`,
with the host display driver providing modeset/DirectDraw and registering fxD3D.

## Deploy to the fleet

```bash
# validate the rebuilt Glide + the backend seam on a real Voodoo box:
(cd gfxbench && make CARD=h5)          # or CARD=h3 for Voodoo3
python3 push_gfxbench.py <agent-ip> --frames 300        # headless sweep -> CSV
python3 push_gfxbench.py <agent-ip> --with-glide        # also push rebuilt glide3x
python3 push_gfxbench.py <agent-ip> --interactive       # GUI harness (screenshot loop)
```
