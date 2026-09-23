# vcr-disp — our cooperative display-driver skeleton

> The whole stack is documented on one page: [`../README.md`](../README.md).
> This file summarises the state of this directory; the detail is in
> [§7.1 there](../README.md#71-vcr-disp--our-cooperative-display-driver-skeleton).

**Goal:** a display driver of our own whose main job is to answer the HWCEXT
escapes our Glide sends (`ExtEscape` codes `0x3df3` / `0xfd3` / `0x13df3`), so
our unmodified Glide can drive the card while Windows keeps the desktop through
us. Modelled on the open Device3Dfx (Linux), RISCyVoodoo (NT) and vmdisp9x —
read for structure, not copied.

**State (audited 2026-09-23): a skeleton that cannot compile, and would not
work if it did.** No build or load of it has ever been recorded. The fuller
clean-room display driver is fxD3D in [`../../scripts/3dfx/`](../../scripts/3dfx/).

## Files

| File | Contents |
|---|---|
| `vcr_hwcext.h` | The escape contract: codes, opcodes, device IDs `0003`/`0005`/`0009`. (Its include guard still says `RETRO3DFX_HWCEXT_H`.) |
| `disp_escape.c` | `r3dfx_escape_dispatch()` — answers GETDRIVERVERSION, GETDEVICECONFIG, GETLINEARADDR, ALLOCCONTEXT, exclusive/restore-desktop and context queries; fails everything else. `DrvEscape` wrapper under `HAVE_DDK` |
| `disp_hw.c` | Under `HAVE_DDK` only: reads BAR0/BAR1, maps BAR0 with `MmMapIoSpace`, maps both into the caller through `\Device\PhysicalMemory` + `ZwMapViewOfSection`. Hard-codes Voodoo 3 values (16 MB, 4 KB strides) |
| `SOURCES` | DDK build file — lists `disp_enable.c` and `disp_modeset.c`, **which do not exist** |
| `disp.def` | Exports `DrvEnableDriver`, **which is not written** |
| `vcr-disp.inf` | Binds `DEV_0005`/`DEV_0009` on NT 5.1; copies a `retro3dfx-mp.sys` miniport that does not exist; no `InstalledDisplayDrivers`, no services section |

## What must change before it can work

1. **Match Glide's HWCEXT layout.** Glide sends `{contextID, which, optData}`
   and reads `{resStatus, optData}` (`retro3dfx-glide/glide3x/h3/minihwc/hwcext.h`).
   This code reads `contextID` as the opcode and writes results with no
   `resStatus`.
2. Answer `LINEAR_MAP_OFFSET` and `FIFOINFO`, which Glide sends.
3. Return three base addresses (registers, frame buffer, I/O), as the H5 driver does.
4. Write the GDI chassis (`DrvEnableDriver`, PDEV, surfaces, mode set) — or
   reuse fxD3D's `scripts/3dfx/driver/nt/chassis.c`, which already has one.
5. Move the kernel mapping into a real miniport: a GDI display DLL may import
   only `win32k.sys`.
6. A working INF, and host tests for the escape dispatcher.
