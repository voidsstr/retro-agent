# 3dfx — Glide rebuild, benchmark harness, and the fxD3D clean-room driver

Everything for optimizing/extending 3dfx Voodoo drivers for the fleet (the
Voodoo5 5500, the incoming Voodoo5 6000, Voodoo3), built with the same mingw
toolchain as `retro_agent.exe`. **Clean-room throughout** — built only from the
open Glide source release, public DDK/DDI, and public register documentation.

D3D driver design: [`docs/3dfx-d3d-hal-design.md`](../../docs/3dfx-d3d-hal-design.md).
This is layer [1] of the clean-room stack — see the whole-stack overview in
[`voodoo-cleanroom/README.md`](../../voodoo-cleanroom/README.md).

**Status (2026-08):** the D3D driver `fxd3ddd.dll` is **code-complete through
M4c-2 and links** (real DP2 parsing, a kernel-mode Glide FIFO backend, DDraw
surface/present bodies, host-tested logic all green). The one remaining step is
**M4d** — pairing it with the stock miniport and bringing it up on real Voodoo3
hardware (`.124`), never yet done. Until then `.124`'s layer [1] is the vintage
H5 driver; our ICD + Glide (layers [2]/[3]) are already the clean-room ones.

> **Two Glide builds, don't confuse them:** the `build-glide.sh` here produces the
> fxD3D-local Glide inputs (`out/glide3x_h3_voodoo3.dll` etc.) for gfxbench/backend
> validation. The Glide that actually **ships in the deployed stack** is built by
> [`voodoo-cleanroom/build-stack.sh`](../../voodoo-cleanroom/build-stack.sh)
> (the 787 KB h3 `glide3x.dll`) — that's the canonical one.

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
platform DDK — see `driver/README.md`. The DDK toolchain is deployable to any
fleet box over the agent so the box builds the driver itself:
```bash
python3 ../../provisioning/ddk/provision_ddk.py <box-ip>   # deploy the DDK (one-time)
python3 ../../provisioning/ddk/build_driver.py  <box-ip>   # build fxd3ddd.dll on the box
```
See [`provisioning/ddk/README.md`](../../provisioning/ddk/README.md).

## The pieces, and how they fit

| Layer | Dir | Status |
|---|---|---|
| Glide3/2 DLLs (the native API + hardware knowledge) | `build-glide.sh` | **built** (h3+h5+glide2) |
| Glide backend seam (`gb_*`) | `d3dhal/glidebackend.*` | **built, compiles vs real SDK** |
| On-card validator + benchmark | `gfxbench/` | **built** → `gfxbench.exe` imports `glide3x.dll` |
| fxD3D D3D→Glide core (DP2 dispatch, state/tex/prim) | `d3dhal/` | **built + host unit test PASS** |
| DDI glue (Windows callback → fxD3D) | `driver/ddi_glue.*` | **built + host glue test PASS** |
| Host display driver + kernel-Glide backend (loads/registers fxD3D) | `driver/nt` | **built** — `fxd3ddd.dll` links (native PE, ~45 KB), M4a→M4c-2 code-complete + host-tested; **M4d on-card bring-up is the only remainder** |

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
