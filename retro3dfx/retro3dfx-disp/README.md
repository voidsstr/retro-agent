# retro3dfx-disp — the cooperative Voodoo display driver (the bottom of our stack)

The layer that lets **our unmodified Glide** drive a real Voodoo on Windows XP.
It's a display driver we own that (a) does the minimum to be the display adapter
and (b) **answers the HWCEXT escape protocol** `retro3dfx-glide` sends — the exact
probe the retail 2001 `3dfxvs` driver ignores (verified on `.124`, see
`docs/3dfx-glide-hardware-init.md`). "Cooperative" = we're the display driver, so
no fight over the card (unlike a direct PCI grab).

## Why this and not a PCI mapper
Our Glide's NT path asks the *display driver* `"are you a 3dfx device?"` via
`ExtEscape(0x3df3, HWCEXT_GETDEVICECONFIG)` and then `HWCEXT_GETLINEARADDR` for the
card's mapped registers/framebuffer. Own the display driver → answer those → Glide
works, and Windows still manages the desktop through us (no takeover corruption).

## Files
```
retro3dfx_hwcext.h  the escape ABI (opcodes + structs) - shared contract, mirrors
                    retro3dfx-glide's minihwc/hwcext.h. Single source of truth.
disp_escape.c       THE escape server: DrvEscape -> r3dfx_escape_dispatch(),
                    answers GETDEVICECONFIG / GETLINEARADDR / ALLOCCONTEXT /
                    exclusive. Host-tested (the dispatch logic is DDK-independent).
disp_hw.c           device bring-up: find the Voodoo, map its PCI BARs into the
                    caller (the r3dfx_hw_map_linear the escape server calls).
                    Modeled on the open Device3Dfx logic. [needs DDK]
disp_modeset.c      CRTC / video mode-set for fullscreen. [needs DDK]
disp_enable.c       DrvEnableDriver + the GDI driver table (2D/DDraw chassis,
                    from the vmdisp9x / RISCyVoodoo skeleton). [needs DDK]
SOURCES, disp.def   DDK build + exports
retro3dfx.inf       registration (binds PCI VEN_121A DEV 0005/0009 under Display)
```

## Status
- **Escape server (`disp_escape.c`) — written + host-tested + cross-compiles.**
  It correctly answers `GETDEVICECONFIG` (reports Voodoo3/5) and `GETLINEARADDR`
  (returns the mapped BARs) — the two probes that fail today.
- **`disp_hw.c` / `disp_modeset.c` / `disp_enable.c` — to write against the DDK**
  (BAR mapping, CRTC, the GDI chassis). These are the parts that need real
  hardware iteration; the Device3Dfx model + RISCyVoodoo/vmdisp9x skeletons give
  the templates.

## Build
Needs the platform DDK. Build via the fleet DDK toolchain
(`provisioning/ddk/build_driver.py` pattern) or an NT DDK `build` env → produces
`retro3dfx-disp.dll` + the miniport. Install via `retro3dfx.inf` (PnP), then run
our unmodified `retro3dfx-glide` `glide3x.dll` on top → the Voodoo lights up.

## Bring-up order
1. Escape server ✓ (done, tested).
2. `disp_hw.c` BAR mapping (from Device3Dfx) → `GETLINEARADDR` returns real addrs.
3. `disp_modeset.c` fullscreen CRTC.
4. `disp_enable.c` minimal 2D so Windows loads us as the adapter.
5. Load driver, run our Glide → gfxbench → UT → Q3 (via retro3dfx-gl).
