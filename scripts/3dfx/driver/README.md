# fxD3D host display driver (M3)

The "chassis" that makes Windows load fxD3D: a display driver that provides
2D + modeset + DirectDraw, and — the novel part — advertises fxD3D's Direct3D
callbacks so the runtime routes `DrawPrimitives2` into our translation core.

See [`docs/3dfx-d3d-hal-design.md`](../../../docs/3dfx-d3d-hal-design.md) for the
architecture. This tree is clean-room (public DDI + open templates only).

## Files

```
ddi_glue.h/.c    portable bridge: the driver's DrawPrimitives2 callback reduces
                 to fxdglue_draw_primitives2() -> fxd_dp2_execute(). Compiles
                 and is unit-tested WITHOUT the DDK.
fxd3d.inf        registration: binds PCI VEN_121A DEV 0005 (V3) / 0009 (V4/5)
                 under the Display class, copies driver files, writes the
                 Display\0000 entries (applied via PnP).
nt/              Windows 2000/XP host: GDI display driver DLL
  enable.c         DrvGetDirectDrawInfo + the D3D DDI callbacks (ContextCreate,
                   DrawPrimitives2) -> the glue.  [needs the NT DDK]
  SOURCES          NT DDK `build` file -> fxd3ddd.dll
  dispdrv.def      exports DrvEnableDriver
win9x/           Windows 98/ME host: DDHAL D3D-callback registration
  d3dcb.c          GetDriverInfo(GUID_D3DCallbacks3) -> DrawPrimitives2 -> glue.
                   Rides on a vmdisp9x-shaped 2D/DDraw minivdd.  [needs the 98 DDK]
test/test_glue.c host test: context + DrawPrimitives2 path against the stub
                 backend (no DDK, no card).
```

## Build / test (here, DDK-independent)

```bash
make        # cross-compile the glue for Win32 + build & RUN the host glue test
```

The glue test drives the exact path the driver's `DrawPrimitives2` takes —
create context, bind texture, run a DP2 buffer — and asserts the render. This
proves the bridge is correct with no DDK and no hardware.

## Build the FULL loadable driver (needs the platform DDK)

The `nt/` and `win9x/` sources use real DDK types (`winddi.h`, `ddrawint.h`,
`d3dhal.h`) and only compile in a DDK environment. Two ways:

1. **DDK VM / toolchain** — open the NT DDK `build` environment and run `build`
   in `nt/` (uses `SOURCES`) → `fxd3ddd.dll`. For 9x, the Win98 DDK + the
   vmdisp9x host build.
2. **On a provisioned fleet box** — the era DDK is staged onto an XP machine and
   driven over the agent (`UPLOAD` sources, `EXEC build`, `DOWNLOAD` the binary)
   — i.e. the fleet builds its own driver. **This is fully automated:**
   ```bash
   python3 ../../../provisioning/ddk/provision_ddk.py <box-ip>   # one-time: deploy the DDK
   python3 ../../../provisioning/ddk/build_driver.py  <box-ip>   # build fxd3ddd.dll on the box
   ```
   See [`provisioning/ddk/README.md`](../../../provisioning/ddk/README.md).

## Backend on the driver side

`glidebackend` in bring-up (gfxbench, 86Box) is the **Glide3 DLL** running over
whatever 3dfx display driver is installed. In the **shipped** driver, Glide's
hardware-init (`minihwc`/`cinit`, open source) is compiled *into* the driver so
there's no dependency on a separate 3dfx driver being present. The `fxd_*` /
`gb_*` seam is identical either way — only where the hardware access is linked
from changes.

## Registration recap (how Windows loads it)

1. `fxd3d.inf` applied via PnP ("have disk" / Update Driver) — matches the PCI
   ID, copies files, writes the Display-class registry key.
2. Boot: (XP) the miniport `.sys` loads, win32k loads `fxd3ddd.dll`; (9x) the
   minivdd + display minidriver load.
3. A DX6/7 game starts D3D → the runtime calls the driver's `GetDriverInfo`/DDI
   → gets fxD3D's callbacks → every frame flows through `DrawPrimitives2` →
   `fxd_dp2_execute` → Glide → the card.

## Bring-up order

1. `make` here (glue verified) ✓
2. Build the host driver in the DDK; load in **86Box** (Voodoo emulation) — first
   DP2 triangle, then a textured/fogged scene.
3. Real cards over the agent: `.50` (Voodoo5) / a Voodoo3 box — `LAUNCH` a D3D
   test app, screenshot-verify, then a real game.
4. Regression speed via `../../benchmarks/`.
