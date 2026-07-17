# fxD3D — clean-room Direct3D HAL for Voodoo 3/4/5

A Direct3D 6/7 fixed-function HAL implemented as a translation layer over the
open-source Glide3 we build in `../build-glide.sh`. Design +rationale:
[`docs/3dfx-d3d-hal-design.md`](../../../docs/3dfx-d3d-hal-design.md).

**Clean-room.** Built only from the open Glide release, the Microsoft DDK's
public D3D DDI, and public VSA-100/Avenger register docs. Everything here is
buildable, committable, and shippable.

## Layout

```
glidebackend.h/.c   the seam that knows Glide — fixed-function state expressed
                    in gr* calls. Shared with ../gfxbench (the on-card harness).
include/fxd3d.h     internal HAL types + minimal public-DDI mirrors (so the
                    core compiles/tests without the DDK)
d3dhal_state.c      D3DRENDERSTATE_* / D3DTSS_* -> Glide  (the core mapping)
d3dhal_tex.c        D3D pixel formats -> TMU formats + upload
d3dhal_prim.c       DP2 TL-vertices -> grDrawTriangle/Line/Point
d3dhal_ddi.c        DrawPrimitives2 command-buffer dispatcher + caps
test/               host unit test (stub backend) — runs on the build machine
```

## Build / test

```bash
make            # cross-compile the HAL core for Win32 + build & RUN the host test
make winobj     # just the Win32 driver-core objects
make test       # just the host unit test (native gcc, no hardware)
```

`make test` feeds a synthetic DP2 buffer (render states + a texture stage + a
textured triangle) through `fxd_dp2_execute` against a recording stub backend
and asserts the resulting Glide call sequence — validating the DDI dispatch and
the D3D→Glide state mapping with **no Glide DLL and no card**.

## Status (per the design doc milestones)

- [x] **M1** on-card Glide validation harness (`../gfxbench`, builds to a real
      `.exe` that imports `glide3x.dll`)
- [x] **M2** HAL translation core: state/tex/prim/DDI — compiles for Win32 and
      **passes the host unit test**
- [ ] **M3** host display driver (adopt vmdisp9x on 9x / RISCyVoodoo on NT and
      wire 3dfx modeset from Glide `minihwc`, publish the DDHAL)
- [ ] **M4** register fxD3D as the D3D callbacks; bring up in 86Box then on
      real Voodoo 3/5
- [ ] **M5** conformance + speed via `scripts/benchmarks/`

## What "done" is, honestly

At M5 this yields roughly what SFFT already gives as a binary — DX6/7-class D3D
on the Voodoo — but **as source we own, tune, and ship to the fleet**. The value
is ownership + instrumentation (per-game fixes, FSAA behavior, benchmark
hooks), not new capability over SFFT. M3 (the loadable display-driver host) is
the remaining heavy lift; M1–M2 (here) are the parts that are provably correct
without it.
