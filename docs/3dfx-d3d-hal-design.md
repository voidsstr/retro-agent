# fxD3D — a clean-room Direct3D driver for Voodoo 3 / 4 / 5, backed by open Glide

Design document for a **clean-room Direct3D HAL** for the 3dfx VSA-100
(Voodoo4/5) and Avenger (Voodoo3) chips, implemented as a translation layer on
top of the **open-source Glide3** we already cross-compile
(`scripts/3dfx/build-glide.sh`). Built only from open sources — every reference
is either 3dfx's genuinely open Glide release, the Microsoft DDK, or public
register documentation.

Status: **active build.** This doc is the spec; code lives in
`scripts/3dfx/d3dhal/` (the HAL translation core + a Glide backend) and
`scripts/3dfx/gfxbench/` (the on-card mode/option test + benchmark harness).

---

## 1. Why this is tractable

Direct3D 6/7's Hardware Abstraction Layer and Glide3 are two vocabularies for
the **same fixed-function multitexture silicon**. Glide was 3dfx's own
abstraction of the FBI (frame-buffer interface) and TMU (texture-mapping unit);
the D3D HAL is Microsoft's. Because the hardware is fixed-function (no vertex or
pixel shaders — the Voodoo has none, and neither does DX6/7), the semantic gap
between "a D3D render-state vector" and "a sequence of Glide calls" is small.

The open Glide3 DLL exports **98 entry points**. The DX6/7 fixed-function state
model maps onto them almost 1:1 (see §5). So the HAL does not talk to registers
directly — it **re-expresses D3D DDI operations as Glide3 calls**, and Glide
already owns the genuinely hard hardware work we don't want to reimplement:

- chip init / mode set / memory sizing (`grSstWinOpen`, `minihwc`, `cinit`)
- the command FIFO and packet building
- VSA-100 **multi-chip SLI and rotated-grid FSAA** (incl. the Voodoo5 6000
  4-way paths and the "V5 6000 DAC workaround for 4x/8x FSAA")
- texture download + NCC/compression (`grTexDownloadMipMap`, `texus`)
- fog tables, alpha/chroma, depth buffering, dithering

That is the entire reason a clean-room D3D driver is realistic here and would be
a multi-year slog against bare metal: **90% of the driver already exists, open.**

## 2. Scope

**In scope (v1):** DirectX 6/7 fixed-function Direct3D — the DDI the Voodoo
hardware actually matches:

- `DrawPrimitives2` (DP2) token-stream execution model (DX6+).
- Fixed-function render states: shading, Z, alpha blend/test, fog, cull,
  dither, specular, texture stages (1–2 TMUs), texture filter/address/LOD.
- Texture management: create / lock / load into card memory, formats the TMU
  supports (ARGB1555, RGB565, ARGB4444, ARGB8888→dither, palettized, NCC).
- Multi-resolution / multi-depth output, FSAA, triple/double buffering.

**Out of scope (documented dead-ends, not oversights):**

- Vertex/pixel shaders (DX8+) — hardware has none.
- DX9 DDI — the runtime path the Voodoo never targeted; SFFT/3dfx stopped at
  DX6/7-class. Not attempted.
- The **2D GDI acceleration** beyond what the host display driver provides
  (§4) — fxD3D is the 3D DDI, not a full 2D miniport.

## 3. Where it plugs into Windows

Direct3D on the Voodoo era is layered on **DirectDraw**. The display driver
publishes a DirectDraw HAL (DDHAL); Direct3D is a set of additional callbacks
the runtime discovers through `GetDriverInfo`/`D3DHAL_*`. So a D3D HAL cannot
exist on its own — it lives **inside a display driver** that provides modeset +
DirectDraw surfaces.

```
        Direct3D game (DX6/7)
                |
        Direct3D runtime  (d3dim.dll / ddraw.dll)   ← Microsoft, unchanged
                |  DDI: caps + DP2 token stream
        ┌───────▼─────────────────────────────────────────┐
        │  fxD3D  (this project)                            │
        │  d3dhal_global : caps / device desc / GetDriverInfo
        │  d3dhal_ddi    : Context{Create,Destroy}, DP2 loop │
        │  d3dhal_state  : D3DRENDERSTATE_* → gr* mapping    │
        │  d3dhal_tex    : surface/texture → grTexDownload   │
        │  d3dhal_prim   : DP2 primitives → grDrawVertexArray│
        └───────┬─────────────────────────────────────────┘
                |  gr* calls
        Glide3  (open, we build it)  → command FIFO → 3dfx silicon
                ▲
        host display driver: modeset + DirectDraw HAL + 2D
        (9x: vmdisp9x-style minivdd ; NT/XP: RISCyVoodoo-style miniport)
```

The **host display driver** is the remaining hard dependency. Two clean routes,
both templated by existing open drivers, chosen per-OS:

- **Win9x:** a minivdd/display minidriver in the shape of
  [vmdisp9x](https://github.com/JHRobotics/vmdisp9x) (Open Watcom, modern host),
  with the 3dfx hardware init drawn from Glide's `minihwc`. It exposes the
  DDHAL; fxD3D registers as its D3D callbacks.
- **NT/2000/XP:** a kernel miniport + display DLL in the shape of
  [RISCyVoodoo](https://github.com/Eeveelution/RISCyVoodoo) (NT DDK `build`),
  same idea.

v1 milestone ordering (§7) deliberately front-loads the parts that are testable
**without** the finished host driver, so we get on-card validation early.

## 4. Module layout

```
scripts/3dfx/d3dhal/
  include/fxd3d.h        internal types shared by HAL + backend
  glidebackend.h/.c      thin, swappable wrapper over the gr* API (also used by
                         gfxbench) — one place that knows Glide
  d3dhal_global.c        caps: D3DDEVICEDESC / D3DHAL_GLOBALDRIVERDATA, formats
  d3dhal_state.c         the render-state → Glide translation table (§5)
  d3dhal_tex.c           texture create/lock/load → grTex*  (§6)
  d3dhal_prim.c          vertex layout + DP2 primitive draw → grDrawVertexArray
  d3dhal_ddi.c           ContextCreate/Destroy + the DrawPrimitives2 dispatcher
  Makefile               cross-compiles what compiles without the DDK; the DDI
                         glue behind -DHAVE_DDK for the real driver build
scripts/3dfx/gfxbench/
  gfxbench.c             on-card harness: cycle every mode + render option,
                         draw a diagnostic scene, benchmark, dump CSV
  Makefile               links our built glide3x import lib → gfxbench.exe
```

`glidebackend` is the seam: fxD3D and gfxbench both call it, so the driver's
hardware path is exercised and debugged by a plain user-mode EXE on the card
long before it has to work inside a Windows display driver.

## 5. Render-state translation (the heart of the HAL)

Each entry is a `D3DRENDERSTATETYPE` (or DP2 texture-stage state) mapped to a
Glide3 call. This table is implemented in `d3dhal_state.c` as a dispatch on the
DP2 `D3DHAL_DP2SETRENDERSTATE` / `D3DHAL_DP2TEXTURESTAGESTATE` tokens.

| D3D render state | Glide3 backend call |
|---|---|
| `D3DRS_SHADEMODE` (flat/gouraud) | vertex-layout RGB iteration; `grColorCombine` iterated vs constant |
| `D3DRS_ZENABLE` / `D3DRS_ZFUNC` / `D3DRS_ZWRITEENABLE` | `grDepthBufferMode` / `grDepthBufferFunction` / `grDepthMask` |
| `D3DRS_ALPHABLENDENABLE` + `D3DRS_SRCBLEND`/`D3DRS_DESTBLEND` | `grAlphaBlendFunction` (RGB + alpha factors) |
| `D3DRS_ALPHATESTENABLE`/`D3DRS_ALPHAREF`/`D3DRS_ALPHAFUNC` | `grAlphaTestFunction` + `grAlphaTestReferenceValue` |
| `D3DRS_FOGENABLE`/`D3DRS_FOGCOLOR`/`D3DRS_FOGTABLE*` | `grFogMode` + `grFogColorValue` + `grFogTable` (table-fog; build with `guFogGenerate*`) |
| `D3DRS_CULLMODE` | `grCullMode` (+ handedness via `grCoordinateSpace`/origin) |
| `D3DRS_DITHERENABLE` | `grDitherMode` |
| `D3DRS_SPECULARENABLE` | second color iterator / `grColorCombine` add stage |
| `D3DRS_TEXTUREFACTOR` | `grConstantColorValue` |
| `D3DRS_COLORKEYENABLE` (+ key) | `grChromakeyMode` + `grChromakeyValue` |
| `D3DRS_FILLMODE` (point/wire/solid) | `grDrawPoint`/`grDrawLine`/triangle path |
| **Texture stage** `D3DTSS_COLOROP/ARG*` | `grColorCombine` / `grTexCombine` (map MODULATE/ADD/SELECTARG/BLEND to combine functions) |
| `D3DTSS_ALPHAOP/ARG*` | `grAlphaCombine` / `grTexCombine` alpha |
| `D3DTSS_MINFILTER/MAGFILTER/MIPFILTER` | `grTexFilterMode` / `grTexMipMapMode` |
| `D3DTSS_ADDRESSU/V` (wrap/clamp) | `grTexClampMode` |
| `D3DTSS_TEXTURELODBIAS` | `grTexLodBiasValue` |
| viewport / clip | `grClipWindow` + `grViewport`-equivalent scaling in the T&L feeder |

**Vertex path.** DX6/7 hands the driver TL (transformed-and-lit) vertices in
DP2 buffers — screen-space XYZRHW + diffuse/specular + up to 2 sets of UVs.
That maps directly onto a Glide **vertex layout** (`grVertexLayout` /
`grGlideSetVertexLayout`) with `GR_PARAM_XY`, `GR_PARAM_Z`/`GR_PARAM_Q0`,
`GR_PARAM_RGB`, `GR_PARAM_A`, `GR_PARAM_ST0/ST1`, then `grDrawVertexArray`.
Because the vertices arrive already transformed, **no T&L is needed** — perfect,
since the Voodoo has no hardware T&L and the runtime already did it.

## 5a. 32-bit compatibility mode (render 16, present 32)

The Voodoo3 renders 3D **only in 16-bit** (565 + a 22-bit post-filter), but it
*scans out* 32-bit fine (640×480×32 etc. are in its DDraw mode list). The vintage
H5 HAL therefore advertises `dwDeviceRenderBitDepth = DDBD_16` only — so a D3D
game running on the normal 32-bit desktop (e.g. GoldSrc, which matches the
desktop depth) asks for a 32-bit render target, finds no device, and falls back
to software ("the specified video mode is not supported"). See
`retro-3dfx/FINDINGS.md` (2026-07-23).

Because fxD3D owns the whole surface/present path, it can close that gap without
forcing the user to a 16-bit desktop:

- **Advertise both** `DDBD_16 | DDBD_32` render depths (and `DDBD_16 | DDBD_24`
  Z) in the caps (`d3dhal_global`/`fxd_fill_caps`).
- When D3D creates a **32-bit** primary/render target, allocate it as the 32-bit
  scanout surface, but back the 3D device with a **16-bit render buffer** (what
  the FBI/Glide actually draws into).
- On **present** (`DdFlip` / windowed `DdBlt`), **expand 16→32** from the render
  buffer to the 32-bit primary — via the V3 2D blt (stretch/copy with format
  convert) or an LFB pass. Dither stays as the hardware does it; the 32-bit
  primary is a straight 565→888 expansion (no quality gain — pure compatibility).

Cost: one full-surface convert-blit per frame (cheap relative to the draw). Gate
it `FX_D3D_32BPP_COMPAT` (default **on**) so it can be A/B'd. This is the
clean-room advantage over the vintage HAL: we control caps + present, so a
32-bit desktop no longer blocks D3D. Fullscreen games that *do* switch to 16-bit
themselves skip the convert entirely (render == present == 16).

## 6. Textures & surfaces

- `D3dCreateSurfaceEx` / texture create → allocate TMU memory
  (`grTexMinAddress`/`grTexCalcMemRequired`), track a handle.
- Lock/unlock for upload → stage in system memory, convert D3D pixel format to
  the nearest TMU format, `grTexDownloadMipMap`(+`...Level`) on unlock/first-use.
- Format map: `D3DFMT_R5G6B5`→`GR_TEXFMT_RGB_565`, `A1R5G5B5`→`ARGB_1555`,
  `A4R4G4B4`→`ARGB_4444`, `A8R8G8B8`→dither to 4444/1555 or (Napalm) 32-bit
  framebuffer path, palettized→`GR_TEXFMT_P_8` + `grTexDownloadTable`, plus NCC.
- Mip chains → per-level download; LOD clamp via caps.

## 7. Milestones (front-load on-card validation)

1. **gfxbench on the card** *(no DDK needed — builds now)* — links our rebuilt
   `glide3x`, cycles every resolution/depth/refresh/FSAA/render option, draws a
   diagnostic scene, benchmarks. **Proves the rebuilt Glide + the backend seam
   on real Voodoo 3/5 hardware.** ← first, and independently useful.
2. **HAL translation core** *(compiles standalone)* — `d3dhal_state/tex/prim`
   + caps, driven by a host-side unit harness that feeds synthetic DP2 buffers
   and asserts the resulting Glide call sequence. No Windows needed to test the
   logic.
3. **Host display driver** — adopt vmdisp9x (9x) / RISCyVoodoo (NT) skeleton,
   wire 3dfx modeset from `minihwc`, publish the DDHAL.
4. **Register fxD3D as the D3D callbacks**, load in Windows, bring up a DP2
   triangle, then a textured/fogged/blended scene; iterate in **86Box** (Voodoo
   emulation) then on `.50`/`.143`.
5. **Conformance + speed** — walk DX6/7 test apps; regression via
   `scripts/benchmarks/` (swap driver, re-run, diff CSV).

### Milestone status (2026-07-23)

- **M1 — done.** `scripts/3dfx/gfxbench/` builds `gfxbench.exe` (imports
  `glide3x.dll`); `push_gfxbench.py` deploys+runs it over the agent.
- **M2 — done (host format).** `scripts/3dfx/d3dhal/` cross-compiles for Win32
  and passes the host DP2/state unit test. **Gap discovered:** the DP2 parser
  (`fxd_dp2_execute`) uses fxD3D's *simplified* `fxd2_hdr` (8-byte header) +
  `FXD2_*` opcodes, which do **not** match the real DX7 `D3DHAL_DP2COMMAND`
  (4-byte header) / `D3DDP2OP_*` values. Real-DP2 translation is a **required
  M3 work item** (the driver's DrawPrimitives2 must reshape the runtime's real
  DP2 stream into the fxd2 form, or the parser must be dualised).
- **M2.5 — on-card render core: ATTEMPTED, deferred.** Built
  `d3dhal/oncard_validate.exe` (real `fxd_dp2_execute` → real Glide backend on a
  Voodoo3, from a synthetic DP2 buffer). On .124 it hung in `grGlideInit()`
  (board left in a bad state after prior fullscreen churn) and bumped the
  display to the logon desktop; recovered with `setmode 1024 768 32`. On-card
  fullscreen validation is disruptive on .124 (no emulator) — the render core is
  already host-unit-tested + the Glide backend is gfxbench-validated on-card, so
  this is deferred in favour of building the real driver. If revisited: ensure a
  clean board (fresh boot / no prior fullscreen app) and add `grLfbReadRegion`
  pixel-readback instead of relying on a GDI screenshot of the Glide surface.
- **BUILD LOOP established (2026-07-23).** `fxd3ddd.dll` builds against the
  **local Wine DDK** — no on-box 3790 DDK needed. W2K DDK at
  `retro-3dfx/toolchain-3dfx/devtools/w2kddk/` (build.exe, winddi.h, ddrawint.h,
  win32k.lib) + DX7 DDK `devtools/dx7ddk/Inc/` (d3dhal.h, ddrawi.h, d3dtypes.h).
  INCLUDE add `dx7ddk\Inc`. `build.exe` **rejects the `..\` cross-tree SOURCES**
  → drive cl/link directly (harness `prefix/drive_c/clfxd3d.bat`), or restructure
  SOURCES to local files. Compiler flags per the H5 build log; link:
  `-dll -subsystem:native -entry:DrvEnableDriver -def:dispdrv.def -nodefaultlib
  <objs> win32k.lib libcntpr.lib int64.lib`.
- **M3 — DONE (2026-07-23): `fxd3ddd.dll` LINKS.** Compile blockers cleared
  (fxd3d.h HAVE_DDK types; d3dhal_state.c enum guard + D3DTSS_MIPMAPLODBIAS map;
  prim.c file-scope extern; enable.c DDCAPS_ZBLTS). Chassis written (clean-room,
  DDK framebuf pattern), a 10 KB native PE exporting `DrvEnableDriver`:
  - `driver/nt/chassis.c` — DRVFN[10] + `DrvEnableDriver`/`DrvDisableDriver`
    (iDriverVersion=NT4 0x20000); the 2D PDEV set `DrvEnablePDEV`/`CompletePDEV`/
    `DisablePDEV`/`EnableSurface`/`DisableSurface`/`AssertMode` + `DrvGetModes`
    (unaccelerated framebuffer: maps FB from the paired miniport, GDI draws in).
  - `driver/nt/enable.c` — DDraw enable pair + DD_CALLBACKS/DD_SURFACECALLBACKS
    (all `DDHAL_DRIVER_NOTHANDLED` stubs), `DdGetDriverInfo` answering
    `GUID_D3DCallbacks3` → `D3DHAL_CALLBACKS3.DrawPrimitives2 = d3d_DrawPrimitives2`;
    caps `dwDeviceRenderBitDepth = DDBD_16|DDBD_32` (§5a 32-bit compat),
    `dwDeviceZBufferBitDepth = DDBD_16|DDBD_24`.
  - `driver/nt/gbstub.c` — driver-side `gb_*` PLACEHOLDER (no-ops, TODO M4).
  - `driver/nt/crtshim.c` — malloc/calloc/free over EngAllocMem for -nodefaultlib.
  Host tests still PASS; `!HAVE_DDK` paths untouched. Build: the iterate loop
  `scratchpad/build_fxd3d.sh` (rsync → Wine W2K+DX7 DDK cl/link). ⚠️ **It LINKS but
  does nothing on hardware yet** — the DDraw/surface/modeset/present bodies + the
  Glide backend are stubs.
- **M4 — the functional bodies (next, the real work):**
  - **M4a real-DP2 parsing — DONE (2026-07-24).** `d3dhal/d3dhal_dp2real.c` +
    `include/fxd3d_dp2.h`: parses the real `D3DHAL_DP2COMMAND` stream (4-byte hdr,
    D3DDP2OP_* opcodes, FVF vertex fetch, strip/fan/indexed decompose) straight
    onto the fxD3D core — no intermediate buffer. Rigorous bounds checking
    (19 M-iter ASan/UBSan fuzz clean); `enable.c d3d_DrawPrimitives2` wired to the
    real `D3DHAL_DRAWPRIMITIVES2DATA` (cmd surface `lpGbl->fpVidMem`, vertex
    resolution via `D3DHALDP2_USERMEMVERTICES` vs vertex-buffer surface). Host
    test `test_dp2real.c` (real-format + malformed battery + POINTS-DoS-cap
    regression). Hardened post-review: render-state OOB skip (>=256), POINTS
    nested-count DoS cap (`FXDP2_MAX_EMIT`), MALFORMED/BADFVF→DDERR_INVALIDPARAMS
    (only UNKNOWN_OP→COMMAND_UNPARSED), offsetof layout pins, harness stale-obj
    clean. **Deferred to M4c (TODOs in enable.c):** (1) clamp
    dwCommandOffset+dwCommandLength to the command surface's byte size; (2)
    probe+capture the USERMEMVERTICES raw user pointer (EngProbeForRead-eq) — both
    need the real DDraw surface path + confirmed DD_SURFACE_GLOBAL size semantics.
  - **M4b kernel-Glide raw-register backend** — replace `gbstub.c` no-ops with a
    real `gb_*` that drives the command FIFO directly against the driver-mapped
    MMIO (drop `grSstWinOpen`; reuse Glide's packet/register core). **Biggest risk.**
    **Design COMPLETE (2026-07-24): [`3dfx-gbkernel-design.md`](3dfx-gbkernel-design.md)**
    — full register map, CMDFIFO PKT1/3/4/5 formats, init/carve-out/state/texture/
    clear/present paths lifted from the open GPL glide h3 tree (with file:line
    cites), file plan (~2 k LOC: gbkernel/gbkstate/gbktex/gbkdebug + 3 vendored
    headers), escape-driven bring-up ladder, kernel-safety risk list. Notes a
    **miniport gap**: the display DLL needs BAR0 via
    IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES + full-VRAM BAR1 from whatever
    miniport it pairs with (to be resolved at M4d).
  - **M4c-1 attach + bring-up ladder + FP bracket — DONE (2026-07-24).**
    `chassis.c DrvEnableSurface` wires the kernel backend to the card:
    BAR0 ← `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES`, VRAM ←
    `VIDEO_MEMORY_INFORMATION`, desktop geom ← the set mode →
    `gbkernel_attach(...,desktopStride,...)` (graceful 2D-only degrade on
    failure; `DrvDisableSurface` detaches + frees ranges). BAR1 non-cached is a
    documented miniport contract (gbk_mmio.h). New escape-driven ON-CARD
    BRING-UP LADDER — `gbkdebug.c` + `gbkdebug.h` + `gbkernel_dbg_*`, wired
    `INDEX_DrvEscape`, opcodes `0x3DF0..`: `FXDBG_PROBE`/`CLEAR`/`TRI`/`TEX`/
    `READBACK` — validates the backend one layer at a time on real HW WITHOUT
    D3D (the safety net for M4d). DDI FP bracket: `d3d_DrawPrimitives2` wraps
    `fxd_dp2_execute_real_cb` in one `EngSave/RestoreFloatingPointState` region;
    the TRI/TEX rungs bracket their float work. Builds `LINKEXIT=0`; host tests
    all-PASS. `desktopStride` threaded so `gb_swap` dst uses the desktop pitch.
  - **M4c-2 DDraw surface/present bodies — DONE (2026-07-25, code-complete;
    on-card validation pends M4d).** The DDraw DDI stubs in `enable.c` are now
    real:
    - **Placement (driver-managed, no DDraw heap):** `DdCreateSurface` places
      every vidmem surface on gbkernel's carve-out — primary→offset 0, screen
      16-bit Z→gbkernel's aux, the app's screen-sized 16bpp back/RT→gbkernel's
      back color buffer (D3D output IS the surface), everything else (textures
      ≤256² pow2, offscreen, the §5a 32bpp RT proxy) via a bump allocator over
      a region gbkernel now **reserves out of the top half of tram**
      (`gbkernel_get_meminfo`; `gb_tex_create` is ceilinged below it).
      `DdCanCreateSurface` gates formats (16bpp RGB anywhere, 32bpp non-texture
      only, Z16, pow2 ≤256 textures). `fpVidMem` = board byte offset (primary
      0) everywhere.
    - **Access:** `DdMapMemory` (IOCTL_VIDEO_SHARE/UNSHARE_VIDEO_MEMORY) gives
      each process its user mapping; `DdLock` drains the FIFO (`gb_finish`)
      then returns `fpProcess + fpVidMem` (+rect); `DdUnlock` no-op.
    - **§5a present:** `gbkernel_attach` takes the desktop **bpp**; `gb_swap`'s
      2D blit dst format is now `gbk_present_dstformat()` — 16bpp = plain 565
      copy, **32bpp = SSTG_PIXFMT_32BPP dst so the 2D engine expands 565→8888
      during the SRCCOPY blit** (the hw convert). `DdFlip` = `gb_swap` (blt-
      present; runtime fpVidMem rotation quirk documented in-code — real
      scanout flips are M4d). `DdBlt` detects the full-screen RT→primary
      present → `gb_swap`; otherwise CPU 1:1 SRCCOPY/COLORFILL/DEPTHFILL with
      clip-list support and the **CPU 565→8888 row convert** for 16→32 blits;
      all rects clamped to surfaces + the FB mapping. `DdSetExclusiveMode` /
      `DdFlipToGDISurface` answered via **GUID_NTCallbacks**.
    - **D3D discovery gap closed:** `DrvGetDirectDrawInfo` now fills `vmiData`
      (primary geometry/format/aligns) + real `ddCaps`, and publishes
      `lpD3DGlobalDriverData`/`lpD3DHALCallbacks` (was NULL — no D3D device
      ever enumerated) incl. the 565/1555/4444 texture-format list;
      `d3d_ContextCreate` sizes the context from the runtime's RT surface.
    - **Host-tested logic:** new pure module `gbk/gbk_surf.c` (pitch/size
      math, the surface bump allocator, `gbk_expand565` + row convert,
      `gbk_present_dstformat`) + `gbkernel-test/test_gbk_surf.c` (5/5 modules
      PASS); PDEV moved to shared `fxpdev.h`. Builds `LINKEXIT=0`.
    - **Remaining for M4d:** real scanout flip (vidDesktopStartAddr), confirm
      driver-managed placement + `FrameBufferLength==VRAM` on the stock
      miniport, texture-handle→TMU download (D3dCreateSurfaceEx), vblank poll.
    (`DrvAssertMode` stays the documented no-op: the modeset runs in
    `DrvEnableSurface` and GDI rebuilds the surface on foreground switches.)
  - **M4d miniport-paired bring-up on .124** — pair fxd3ddd.dll with the MS inbox
    Voodoo3 miniport; deploy via deploy-3dfx-driver (rollback net); first desktop,
    then a DP2 triangle, then a DX6/7 game.
- **M4/M5 — pending** the M3 driver: bring-up on .124 (no 86Box available → use
  the deploy-3dfx-driver rollback net: in-box driver stays in the store, remote
  Driver Roll Back via devmgmt), first DP2 triangle, then textured/blended scene,
  then a real DX6/7 game; regression via `scripts/benchmarks/`.

### Bring-up architecture decisions (2026-07-23)

- **Miniport pairing — DECIDED (2026-07-24, task #33).** Inventory on .124: the
  ONLY Voodoo3 miniports are `3dfxvsm.sys` (the stock miniport the MS inbox
  `3dfxvs2k.inf` pairs with `3dfxvs.dll`) and `3dfxv3m.sys` (our WFP-renamed copy,
  currently the active `3dfxvs` service). **There is no separate Microsoft-authored
  Voodoo3 miniport — the "inbox" driver is 3dfx's own code MS redistributed.** So:
  - **Primary bring-up plan:** pair our clean-room `fxd3ddd.dll` with the stock
    `3dfxvsm.sys` miniport (which the inbox `3dfxvs2k.inf` already installs → it's
    the standard, on-box pairing target + the rollback net). gbkernel gets its
    mappings via the **standard** DDK contract: BAR0 registers ←
    `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES` (a conformant miniport declares its
    BARs as public access ranges in HwFindAdapter for its display driver), BAR1
    framebuffer ← `IOCTL_VIDEO_MAP_VIDEO_MEMORY` (chassis.c already does this),
    VRAM size ← the miniport memory info / `QUERY_AVAIL_MODES`. **chassis.c change
    needed (M4c/M4d):** add the `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES` query and
    feed bar0va+bar1va+vram+desktopEnd to `gbkernel_attach`.
  - **Fallback (only if the stock miniport doesn't mark BAR0 public — confirm on
    hardware at M4d):** write a minimal clean-room `fxd3dmp.sys` — mode-set +
    public-BAR declaration liftable from the open GPL `h3cinit.c` (h3InitPlls
    :667 / h3InitSgram :434 / h3InitVga :710) over the DDK videoprt sample.
  - Provenance: pairing the clean-room DLL with the vintage miniport is a
    pragmatic bring-up choice; the D3D HAL + backend stay clean-room. The
    all-clean-room end state uses `fxd3dmp.sys`. Do NOT read the vintage H5
    miniport source — the standard IOCTL contract is the public DDK, no H5 needed.
- **Kernel-Glide = a raw-register `gb_*` backend, not `grSstWinOpen`.** The
  current `glidebackend.c` uses user-mode Glide (`grSstWinOpen`+HWND+DirectDraw
  glue `dxdrvr.c`, MessageBox, file logging) — none of which is legal in a kernel
  display driver. But the user-mode-isms live in Glide's *Windows/DDraw glue*,
  not its FIFO/packet/register core. The shipped driver needs a **second gb_*
  backend** that drives the command FIFO **directly** against the driver-mapped
  MMIO (the "future raw-register backend" `glidebackend.h` already anticipates):
  reuse Glide's packet-building + register knowledge (open source), drop
  `grSstWinOpen` (the driver owns the surface/mode via the miniport IOCTLs). The
  user-mode `glidebackend.c` stays for gfxbench/86Box bring-up. This is the
  biggest remaining engineering risk after the chassis links.

## 8. Testing

- **gfxbench** (§ milestone 1) is the primary hardware bring-up + regression
  tool; it's a Glide app, so it validates the backend independent of the DDI.
- **86Box** emulates VSA-100-class hardware well enough to load the display
  driver and smoke-test the DDI path on the Linux host before real silicon.
- **Real cards:** Voodoo3/Voodoo5 5500 on `.50` (Win98) and the Voodoo5 6000 on
  `.143` once seated — driven over the agent (`UPLOAD`/`LAUNCH`, screenshot loop).
- The existing **`scripts/benchmarks/`** suite becomes the D3D regression rig
  once a game runs on the driver.

## 9. Legal / provenance

Clean-room. Inputs: the open **Glide** release (3dfx Glide GPL), the **Microsoft
DDK** D3D DDI headers/docs, public **VSA-100/Avenger register** references, and
the open **vmdisp9x / RISCyVoodoo / triatomic** drivers. Everything here is built
only from those open/public sources — buildable, committable, and shippable to
the fleet.

## 10. References

- Open Glide: [sezero/glide](https://github.com/sezero/glide) · our build:
  `scripts/3dfx/build-glide.sh`
- Host-driver templates: [vmdisp9x](https://github.com/JHRobotics/vmdisp9x) ·
  [RISCyVoodoo](https://github.com/Eeveelution/RISCyVoodoo) ·
  [triatomic](https://github.com/AmrikSadhra/triatomic) (public V3 reg docs)
- Emulator: [86Box](https://86box.net) (Voodoo emulation for CI smoke tests)
