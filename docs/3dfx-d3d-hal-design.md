# fxD3D — a clean-room Direct3D driver for Voodoo 3 / 4 / 5, backed by open Glide

Design document for a **clean-room Direct3D HAL** for the 3dfx VSA-100
(Voodoo4/5) and Avenger (Voodoo3) chips, implemented as a translation layer on
top of the **open-source Glide3** we already cross-compile
(`scripts/3dfx/build-glide.sh`). ***REMOVED*** is used or consulted — every
reference is either 3dfx's genuinely open Glide release, the Microsoft DDK, or
public register documentation.

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
the open **vmdisp9x / RISCyVoodoo / triatomic** drivers. ***REMOVED*** is **not**
used, opened, or referenced. Everything here is buildable, committable, and
shippable to the fleet — which ***REMOVED***.

## 10. References

- Open Glide: [sezero/glide](https://github.com/sezero/glide) · our build:
  `scripts/3dfx/build-glide.sh`, landscape: `docs/3dfx-drivers.md`
- Host-driver templates: [vmdisp9x](https://github.com/JHRobotics/vmdisp9x) ·
  [RISCyVoodoo](https://github.com/Eeveelution/RISCyVoodoo) ·
  [triatomic](https://github.com/AmrikSadhra/triatomic) (public V3 reg docs)
- Emulator: [86Box](https://86box.net) (Voodoo emulation for CI smoke tests)
