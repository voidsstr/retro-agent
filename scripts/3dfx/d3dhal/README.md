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

## Status (per the design doc milestones — current 2026-08)

- [x] **M1** on-card Glide validation harness (`../gfxbench`, builds to a real
      `.exe` that imports `glide3x.dll`)
- [x] **M2** HAL translation core: state/tex/prim/DDI — compiles for Win32 and
      **passes the host unit test**
- [x] **M3** host display driver — `driver/nt/chassis.c` + `enable.c`: native PE
      `fxd3ddd.dll` that exports `DrvEnableDriver`, publishes the DDraw/D3D
      callbacks, and **links** against the W2K/DX7 DDK
- [x] **M4a** real `D3DHAL_DP2COMMAND` parsing (`d3dhal_dp2real.c`, fuzz-clean)
- [x] **M4b** kernel-mode Glide raw-register FIFO backend (`driver/nt/gbkernel*`;
      design in `../../../docs/3dfx-gbkernel-design.md`)
- [x] **M4c** attach/bring-up ladder + DDraw surface/present bodies + 16→32
      convert (`enable.c`, `gbk/gbk_surf.c`) — code-complete + host-tested
- [ ] **M4d** miniport-paired **on-card bring-up on real Voodoo3** (`.124`):
      first desktop → a DP2 triangle → a DX6/7 game. **Never yet run on silicon —
      this is the one remaining step.**
- [ ] **M5** conformance + speed via `scripts/benchmarks/` (pends M4d)

## What "done" is, honestly

At M5 this yields roughly what SFFT already gives as a binary — DX6/7-class D3D
on the Voodoo — but **as source we own, tune, and ship to the fleet**. The value
is ownership + instrumentation (per-game fixes, FSAA behavior, benchmark
hooks), not new capability over SFFT. Everything up to and including M4c is done
and provably correct off-hardware; **M4d — actually loading it on the card — is
the remaining lift**, and it needs supervised on-box iteration (deploy +
`fxdbg` escape ladder + rollback net), not more host work.
