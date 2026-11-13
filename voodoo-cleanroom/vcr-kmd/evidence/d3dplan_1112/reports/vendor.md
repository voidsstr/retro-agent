# 3dfx's own Direct3D HAL for the VSA-100: what it supports, how it does it, and what vcr-kmd should take from it

*Workflow step `d3dplan_1112`, vendor reference, 2026-11-12. This was a read-only source study. Nothing was
built or edited, and no fleet box was contacted.*

The question was what 3dfx's own H5/Napalm (Voodoo4/5) Direct3D HAL supports, and how it does it at register
level, so that our HAL (`vcr-kmd`) can match it. The vendor source is guidance only: every recommendation
below is to write our own code.

## How to read this report

All paths are absolute through these keys:

| key | path |
|---|---|
| `VH5/` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5/`: 3dfx's W2K/XP display driver with its D3D HAL. This is the code 3dfx shipped for NT. |
| `W9X/` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/Win9x/DX/D3D/`: the Win9x D3D HAL. It holds later revisions of the same files. |
| `W2KBUILD/` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/BUILD/` |
| `DOCS/` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/DOCS/` |
| `KMD/` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/vcr-kmd/`: our driver |
| `EV/` | `KMD/evidence/d3dplan_1112/` |
| `GL5/` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/glide3/src/`: our h5 Glide fork |

How evidence is marked:

- **`path:line`** means I read it in this session.
- **(inferred)** marks general DirectX or 3dfx knowledge, or my own reasoning. It does not come from a file.
- **SPEC** is `DOCS/avenger_spec.doc`. Despite the file name, it is the **Napalm** register specification; its
  text says "Napalm" throughout. A Word binary has no line numbers, so SPEC is cited by a quoted sentence. Every
  quote can be found with `strings -n 5 avenger_spec.doc | grep -n '<words>'`.

The vendor files are upper-case on disk, and every search was case-insensitive. The hex cap values for the
vendor were computed from the bit lists at the cited lines. "Ours as D3D8 reports" is `EV/caps_124.txt`, which
d3dprobe captured on `.124` today.

---

## 0. Summary: what matters for the plan

1. **3dfx never shipped a DirectX 8 HAL on NT.** The W2K driver is a DX7 DDI (`VH5/SOURCES:76-78`, `:81-83`).
   `GetDriverInfo2`, the D3D8 format list and `D3DCAPS8` are compiled only for DX >= 8
   (`VH5/DDFXNT.C:1067-1164`, `VH5/D3INIT.C:2254-2583`, `:3357-3652`). retro3dfx made the DX8 build compile on
   2026-08-13 (commit `c34d62f`); 3dfx itself never did. Our HAL is in the same position, so the vendor's DX7
   caps are the right yardstick (vendor-01).
2. **3dfx never refuses a depth buffer whose size differs from the render target. It resizes the Z instead.**
   - A 16-bit Z on a 32 bpp primary is allocated as a 32-bit (24-bit depth) Z (`VH5/DDSURF.C:592-627`).
   - At attach time the Z is relabelled or reallocated to match the target (`VH5/DDSURF.C:2211-2350`).
   - A depth fill on a promoted Z replicates the 16-bit value (`VH5/DDBLT32.C:938-958`).
   - `ContextCreate` has no depth check at all (`VH5/D3CONTXT.C:33-34`, `:466-498`).

   Our `ContextCreate` refuses that pair (`KMD/display/vcrdd_d3d.c:1190-1196`). That refusal is today's
   `X8R8G8B8 + D16 -> CreateDevice 8876086c` on `.124` (vendor-02).
3. **Every texture-stage operation 3dfx offers on Napalm runs through `combineMode` with bit 31
   (`USE_COMBINE_MODE`) set**, in the FBI and in both TMUs (`VH5/D6MT.C:4412-4414`, `:4891-4893`). Our HAL writes
   `combineMode = 0` (`KMD/include/vcr_3dseq.h:119-122`, `:193`). That single register supplies all of these:
   - MODULATE2X and MODULATE4X (`OUTSHIFT`);
   - ADD, ADDSIGNED, ADDSIGNED2X, SUBTRACT and ADDSMOOTH (the `INVERT_LOCAL` and `INVERT_OTHER` fields);
   - `D3DTA_COMPLEMENT` and `D3DTA_ALPHAREPLICATE`;
   - the TFACTOR constant inside the TMUs.

   3dfx advertises 20 texture ops and we advertise 7. This register accounts for all 13 we lack (vendor-03, -04,
   -05).
4. **Trilinear filtering is single-pass on one texture.** Both TMUs read the same mip chain: TMU1 takes the odd
   LODs, TMU0 the even ones, and TMU0 blends them by LOD fraction (`VH5/D6MT.C:5453-5526`,
   `VH5/D3TXTR.C:2189-2193`). With two textures, trilinear is refused (`D3DERR_TOOMANYOPERATIONS`,
   `VH5/D6MT.C:3288-3302`) unless LOD dithering is switched on (vendor-07).
5. **The VSA-100 has hardware features our HAL leaves unused.** 3dfx drives each of them:
   - stencil (32 bpp only);
   - W-buffer;
   - Z bias through `zaColor`;
   - MIRROR addressing (tLOD bits 28/29);
   - the `SAMECOLOR` blend factors (true SRCCOLOR-as-source and DESTCOLOR-as-destination);
   - the guard band;
   - FASTFILL clears of depth and stencil;
   - two-pixels-per-clock (2ppc) rendering at 16 bpp.

   See vendor-13, -14, -15, -09, -20, -24, -26 and -06.
6. **3dfx does some features in software or in several passes.** Specular on textured triangles is a second,
   additive pass (`VH5/D6TRI2I.C:1283-1400`). Lines and points are drawn as quads through the setup unit
   (`VH5/D6LINE.C:629-700`, `VH5/D6POINT.C:254-365`). Wireframe and point fill are drawn as edges and points
   (`VH5/D6STRIPI.C:287-560`). Our HAL currently drops lines and points (`KMD/display/vcrdd_d3d.c:1690-1715`)
   (vendor-18, -22).
7. **3dfx never offered these** (vendor-10, -36, -21):
   - hardware T&L: the software T&L HAL is not even built on NT;
   - anisotropy;
   - cube maps, bump mapping and DOT3;
   - border addressing;
   - projected textures.

   The hardware *can* do BLENDOP subtract/reverse and per-channel colour write masks at 32 bpp, but a DX7 DDI
   cannot express either.
8. **Do not copy these vendor behaviours** (vendor-31 to -33, -19):
   - the NT DP2 `TEXTURETRANSFORMFLAGS` case overwrites `ADDRESSU`/`ADDRESSV` (`VH5/D6DP2.C:1508-1511`); Win9x
     fixed it;
   - destination-alpha blend factors are replaced by ONE/ZERO on every draw (`VH5/D6DP2.C:6240-6269`);
   - the `Z_ACCESS_OPT` clear-skipping hack;
   - the `ZTEST` and `MIPMAPLODBIAS` caps are not advertised;
   - stage 0's `MAXMIPLEVEL` is used for both TMUs;
   - lines are drawn from y-1 to y.
9. **Performance: 3dfx runs D3D in SLI** (vendor-28, -29, -06, -27).
   - A fullscreen 3D application's flip chain moves onto the overlay and into **4-way SLI by default on a
     4-chip board** (`VH5/DDFXNT.C:2399-2415`, `:3604-3606`).
   - Commands go through a frame-buffer command FIFO with packet-3 vertices.
   - 2ppc is used for single-textured 16 bpp drawing.
   - At most 6 swaps are queued, because more hangs the chip.

   Ours is one chip, writing PCI registers one by one.
10. **3dfx's per-application workarounds** that a retro game might rely on are listed in section 12 (vendor-34).
    For Napalm, the ones that matter are these:
    - Unreal's palette fix;
    - Homeworld's buffer setup;
    - F-22 Lightning 3's mip clamp;
    - the Centipede fog fix;
    - Hidden & Dangerous's clip initialisation;
    - the Armor Command alpha mask.

    The UT 3-stage remap is Voodoo3-only.

---

## 1. What 3dfx built

### 1.1 Provenance: vendor code versus retro3dfx edits

The tree is `retro-3dfx`, imported in commit `67a6aff` (2026-07-17, "Initial import: 3dfx H5 source archive").
retro3dfx edits are marked in the files. The ones that touch D3D are:

| edit | where | effect |
|---|---|---|
| P8 textures no longer exported | `VH5/D3INIT.C:523-530` (`#define DISABLE_PAL8_ON_NAPALM 1`) | Its comment: "The Napalm P8 palette pipeline never delivers the palette to the TMU -- P8 textures sample WHITE". 3dfx had the switch but left it off. |
| TEXBLT of FourCC (DXT) surfaces | `VH5/D6DP2.C:780`, `:3827-3832`; `VH5/DDBLT32.C:2506-2647` (`Blt32_TexBltCopyFourCC`) | "bugcheck 8E on the first managed DXTn texture blt (UT2004)" (`VH5/D6DP2.C:3830-3831`) |
| bounded swap spin | `VH5/DDFLIP.C:369-370` | was unbounded: "the D3D hard-freeze vector" |
| `d8dp2.c` added, DX=8 builds | `VH5/SOURCES:250`; commit `c34d62f` | The DX8 path was "written for Win9x and never compiled for NT" (commit message). |
| logging (`LF=1`, `ENABLE_LOG_FILE`) | `VH5/SOURCES:132`, `:136`; e.g. `VH5/D6MT.C:4922-4941` | diagnostics only |

Everything else cited here is 3dfx's code.

### 1.2 DDI level

- `DX = 7` and `DXDDKVERSION = 7` are the defaults (`VH5/SOURCES:76-78`, `:81-83`).
- `DdGetDriverInfo` answers these GUIDs (`VH5/DDFXNT.C:927-1222`):
  - Misc2 callbacks (`:951-953`);
  - D3D extended caps (`:1022`);
  - D3DCallbacks3, i.e. ValidateTextureStageState and DrawPrimitives2, with Clear2 NULL (`:1033-1066`);
  - Z pixel formats (`:1165-1222`).
- `GetDriverInfo2`, and with it the D3D8 caps and format operations, exist only under
  `DIRECTDRAW_VERSION >= 0x0800` (`VH5/DDFXNT.C:1067-1164`).
- `BuildDX8TextureFormats` and `BuildD3DCaps8` exist only under `(DIRECT3D_VERSION >= 0x0800) && (DX >= 8)`
  (`VH5/D3INIT.C:2254`/`2583` and `:3357`/`3652`).
- Consequence: on XP, the D3D8 runtime builds `D3DCAPS8` from the DX7 caps for 3dfx's driver exactly as it does
  for ours (inferred).
- The DX8 sources still show what 3dfx *meant* to report (sections 2 and 3), and they are cited as such.

### 1.3 Build switches that change D3D behaviour

- **`VH5/SOURCES`:**
  - `-DUSE_D3D_CODE -DNUMTEXTUREUNITS=2` (`:75`);
  - `SA=1` gives `-DSLI_AA` (`:87-91`);
  - `SW_TNL=0` means no `TnL_HAL` (`:92-97`);
  - `AFIFO=0` means no `AGP_CMDFIFO` (`:154-159`).
- **`W2KBUILD/STBPERF.INC`:** `K6_2` (`:5`), `NEWASMTRI` (`:8`) and `NEW_CCU 1` (`:18`, `:49`).
  `VH5/D3GLOBAL.H:716-720` includes it on NT.
- **`VH5/DDFXNT.H:68-76`:** the W2K defines `Z_ACCESS_OPT`, `TEXTURE_REPEAT_NOT_SCALED`,
  `COLUMNBANDCONTROL_DEFAULT`, `MAXPENDINGBUFFERS`, `LODDITHER`, `LODBIASPERCHIP` and `SLI_ABOVE_1280`.
- **The compiled D3D files** (`VH5/SOURCES:185-250`):
  - d3init, d3global, d3contxt, d3rstate, d3txtr, d3fog;
  - d6line, d6fvf, d6mt, d6dp2, d6fan, d6tri2, d6strip, d6point;
  - d3stubs, tcutils, fxt;
  - d7d3d, d7dp2, d8dp2;
  - ddsurf, ddblt32, ddflip, ddmemmgr.

  `LINES.C` and `STRIPS.C` are GDI line drawing, not D3D.

### 1.4 What is not built: T&L

`TLCLIP.C`, `LIGHT.C`, `SOALIGHT.C`, `PROCPRIM.C`, `SOAPRIM.C`, `TNLDP2.C`, `TNLINIT.C` and `XFORM.C` are a
CPU software T&L HAL (an SSE/3DNow "structure of arrays" pipeline). It is not compiled on NT (`SW_TNL=0`,
`VH5/SOURCES:92-97`).

- On Win9x it is optional (`W9X/MAKEFILE:447`, `:843`) and defaults off. Without the `TRANSFORM_AND_LIGHTING`
  environment variable it is `TnL_DISABLED` (`W9X/D3INIT.C:1000-1006`).
- Even on Win9x it sets `HWTRANSFORMANDLIGHT` only for SSE or extended-3DNow CPUs, and never on NT
  (`VH5/D3INIT.C:2642-2656`).
- **The VSA-100 has no geometry engine.** Its absence is a hardware limit, not a driver gap (vendor-36).

---

## 2. Caps: 3dfx against ours, field by field

The vendor column is the DX7 HAL caps as built for Napalm (`VH5/D3INIT.C` `BuildD3DCaps` `:2585-3176`,
`BuildD3DExtendedCaps` `:3178-3355`).

| field | 3dfx W2K, DX7 | ours as built, DX7 | ours as D3D8 reports on `.124` | difference |
|---|---|---|---|---|
| DevCaps | `0x0000ae11` (`VH5/D3INIT.C:2618-2638`) | `0x0008ae51` (`KMD/display/vcrdd_d3d.c:2066-2070`) | `0x0008ae50` | We add TLVERTEXSYSTEMMEMORY and HWRASTERIZATION. Harmless. |
| MiscCaps | `0x72`: MASKZ and the CULL bits (`:2899-2907`) | `0x72` (`:2017-2018`) | `0x72` | same |
| RasterCaps | `0x001449a1`: DITHER, SUBPIXEL, FOGVERTEX, FOGTABLE, ZBIAS, WBUFFER, WFOG (`:2911-2928`), plus ANTIALIASSORTINDEPENDENT on Napalm (`:3162-3163`). **No ZTEST** (`:2917` commented out). | `0x001001b1`: DITHER, ZTEST, SUBPIXEL, FOGVERTEX, FOGTABLE, WFOG (`:2019-2020`) | `0x00100191` | We lack ZBIAS and WBUFFER. 3dfx lacks ZTEST, which is a vendor bug. |
| Z / alpha compare | `0xff` / `0xff` | `0xff` / `0xff` | `0xff` / `0xff` | same |
| SrcBlendCaps | `0x1fff`: base `0x1b33` (`:2945-2959`) plus SRCCOLOR, INVSRCCOLOR, DESTALPHA, INVDESTALPHA and SRCALPHASAT on Napalm (`:3142-3146`) | `0x1f33` (`:2025-2028`) | `0x1f33` | SRCCOLOR/INVSRCCOLOR: the hardware does these (`SAMECOLOR`). DESTALPHA/INVDESTALPHA: 3dfx advertises them and then substitutes ONE/ZERO (vendor-19). |
| DestBlendCaps | `0x1fff`: base `0x183f` (`:2963-2977`) plus DESTALPHA, INVDESTALPHA, DESTCOLOR, INVDESTCOLOR and SRCALPHASAT (`:3154-3158`) | `0x3f` (`:2029-2031`) | `0x3f` | DESTCOLOR and INVDESTCOLOR are real (`SAMECOLOR`). |
| ShadeCaps | `0x000c53ca`: flat and gouraud RGB, all four SPECULAR bits, alpha flat and gouraud blend, FOGFLAT, FOGGOURAUD (`:2994-3016`) | `0x500a` (`:2032-2033`) | `0x4008` | SPECULARGOURAUDRGB and FOGGOURAUD (vendor-39) |
| TextureCaps | `0xcf`: PERSPECTIVE, POW2, ALPHA, TRANSPARENCY, ALPHAPALETTE, TEXREPEATNOTSCALEDBYSIZE (`:3020-3042`). BORDER, SQUAREONLY, PROJECTED and CUBEMAP are commented out (`:3033-3039`). | `0xf` (`:2034-2035`) | `0x4007` | ALPHAPALETTE only matters with P8 textures. |
| TextureFilterCaps | `0x0303033f`, which includes MIPLINEAR, LINEARMIPLINEAR and MIPFLINEAR (`:3046-3061`) | `0x03010317` (`:2036-2040`) | `0x03010317` | trilinear (vendor-07) |
| TextureBlendCaps (legacy) | `0xcf`: DECAL, MODULATE, DECALALPHA, MODULATEALPHA, COPY, ADD (`:3065-3077`). DECALMASK and MODULATEMASK were removed (PRS 8369, revision log). | `0xcb` (`:2041-2043`) | not in D3D8 | DECALALPHA (vendor-37) |
| TextureAddressCaps | `0x17`: WRAP, MIRROR, INDEPENDENTUV, CLAMP (`:3081-3088`) | `0x15` (`:2044-2045`) | `0x15` | MIRROR (vendor-09) |
| StencilCaps | `0xff`, all eight ops, on Napalm (`:3243-3255`) | `0` | none | stencil (vendor-13) |
| TextureOpCaps | `0x001effff`: every op except PREMODULATE, BUMPENVMAP, BUMPENVMAPLUMINANCE and DOTPRODUCT3 (`:3262-3289`) | `0x184f`: 7 ops (`:2165-2168`) | `0x184f` | 13 ops (vendor-03) |
| blend stages / simultaneous textures | 2 / 2 on Napalm (`:3291`, `:3322`). It is 3 / 2 on Voodoo3, for the UT remap (`:3318`). | 2 / 2 (`:2169-2170`) | 2 / 2 | same |
| FVFCaps | `0x00080002`: 2 coordinate sets plus DONOTSTRIPELEMENTS (`:3326-3329`) | `0x2` (`:2164`) | `0x2` | DONOTSTRIPELEMENTS |
| max texture size | 2048 on Napalm (`:3190-3196`) | 2048 with `Diag\D3DBigTex` bit 0, otherwise 256 (`:2158`) | 2048 | same when armed |
| MaxTextureRepeat | 256 (`:3206-3211`) | unset (0) | n/a | minor |
| aspect ratio / anisotropy | 8 / 1 (`:3215`, `:3219`) | 8 / 1 (`:2159-2160`) | 8 / 1 | same |
| guard band | -200, -200, 2048, 1536 when `GUARDBAND_CLIPPING` is set, the default (`:3223-3236`) | 0 (declared but unused, `:2152`, `:2161-2162`) | n/a | guard band (vendor-24) |
| dvMaxVertexW | never assigned in the DX7 path (`:3178-3355`); 0 in the DX8 path (`:3575`) | 65536 (`:2152`, `:2163`) | n/a | ours is more useful |
| render depths | DDBD_16, plus DDBD_32 when `32BPP_RENDERING` is on (default 1) (`:3100`, `:3165-3171`) | DDBD_16, plus DDBD_32 with `Diag\D3D32` (`:2079`) | 16 and 32 | same |
| Z depths and formats | DDBD_16, plus DDBD_24 at 32 bpp (`:3170`). Z16, Z24 and Z24S8 pixel formats (`VH5/DDFXNT.C:1165-1222`). | DDBD_16 and DDBD_32; D16, D24X8, D24S8 (`:2055-2057`, `KMD/include/vcr_rtfmt.h:188-194`) | z16, z24x8, z24s8 | The lists are equal. The pairing rule is not (section 3). |

**Caps that 3dfx sets but does not deliver, or omits although it delivers them** (vendor-33):

- **`D3DPRASTERCAPS_ZTEST` is commented out** (`VH5/D3INIT.C:2917`) although every Z compare function is
  implemented (`VH5/D3RSTATE.C:974-1058`). Ours sets it, correctly.
- **`D3DPRASTERCAPS_MIPMAPLODBIAS` is never set**, although `D3DTSS_MIPMAPLODBIAS` is implemented
  (`VH5/D6MT.C:5636-5642`, `:5550-5580`).
- **DESTALPHA, INVDESTALPHA and SRCALPHASAT are advertised** (`VH5/D3INIT.C:3138-3158`), **but replaced on
  every Napalm draw** (`VH5/D6DP2.C:6240-6269`; see vendor-19).
- **`D3DPRASTERCAPS_ANTIALIASSORTINDEPENDENT`** (`VH5/D3INIT.C:3160-3163`) is full-scene AA through the SLI/AA
  buffers (section 10.3). `D3DRENDERSTATE_EDGEANTIALIAS` is a no-op (`VH5/D3RSTATE.C:842`). The user's choice
  is "no AA", so we should not advertise it (inferred) (vendor-41).
- **DX8 path only** (not in 3dfx's NT build):
  - `Caps2` is never assigned: `D3DCAPS2_CANRENDERWINDOWED | 0;` is a dangling expression statement after a
    commented-out assignment (`VH5/D3INIT.C:3384-3390`);
  - `COLORWRITEENABLE` and `BLENDOP` are commented out (`:3426-3430`);
  - `LineCaps` is 0 (`:3544-3550`), `MaxPointSize` is 1 (`:3623`) and the shader versions are 0.0
    (`:3640-3646`).

---

## 3. Render targets and depth buffers: the `D16 + X8R8G8B8` failure

### 3.1 The hardware rule

The aux (depth) buffer follows the colour depth:

- "When running in 32BPP rendering mode, the 16-bit incoming depth value is converted to a 24-bit depth value
  ... (MSB bit replication)" (SPEC, lfbMode section). The aux buffer is 24-bit depth plus 8-bit stencil at
  32 bpp, and 16-bit at 15/16 bpp.
- 3dfx's own comment says it plainly: "Napalm cannot handle a different depth zbuffer and primary!"
  (`VH5/DDSURF.C:592-596`).

### 3.2 What 3dfx does: it never refuses, it resizes

- **At Z creation** (`DdCreateSurface`, `VH5/DDSURF.C:540-628`):
  - A 24-bit Z request becomes 32 bits: "When 24bpp zbuffer is requested, 32bpp zbuffer must be allocated"
    (`:559-569`).
  - On Napalm, a 16-bit Z requested while the primary is 32 bpp is **allocated and labelled as 32-bit, mask
    `0x00FFFFFF`** (`:619-627`).
  - On Win9x only, a 24/32-bit Z on a 16 bpp primary is forced down to 16 at 640 pixels wide and up
    (`:600-618`, `#ifndef WINNT`).
- **At attach** (`DdAddAttachedSurface`, `VH5/DDSURF.C:2211-2350`). The comment reads "Very dangerous code to
  munge the zbuffer surface depth" (`:2211-2214`). The rule table is at `:2242-2247`:
  - **16 bpp target, 24 or 32-bit Z:** relabel the Z as 16-bit, mask `0xFFFF`, and set `FX_ORIGINALLY_Z32`
    (`:2249-2262`). It keeps its bigger allocation.
  - **32 bpp target, 16 or 24-bit Z:**
    - in a 32 bpp desktop, the Z was already over-allocated at creation;
    - in a 16 bpp desktop, a new 32 bpp Z is allocated and swapped in for the old one (`:2270-2338`);
    - then the Z is labelled 32-bit, mask `0x00FFFFFF`, with `FX_ORIGINALLY_Z16` (`:2341-2347`);
    - `DDERR_CANNOTATTACHSURFACE` is returned only if that allocation fails (`:2284-2290`, `:2313-2320`).
  - The flags are defined at `VH5/DRIVER.H:781-782`.
- **Depth fills on a resized Z** (`VH5/DDBLT32.C:938-958`).
  - On a Z promoted from 16 bits, `DdBlt` puts the 16-bit fill value into the high word as well,
    `dwFillColor = (dwFillColor << 16) | dwFillColor` (`:943-948`). Its comment: "we should probably convert a
    565 value to an 888 value but to pass the Blt-Exotic 3D Depth Fill test, we need to replicate".
  - On a Z relabelled down from 32 bits, it does a 32 bpp fill (`:950-957`).
  - Within the 24-bit mask the trick is right only for `0x0000` and `0xFFFF`. `0xFFFF` becomes `0xFFFFFF`, but a
    mid value such as `0x8000` lands near `0x008000` (inferred arithmetic).
  - SPEC's LFB rule, MSB replication, is the correct conversion: "the 16-bit incoming depth value is converted to
    a 24-bit depth value by taking the 8 most-significant-bits ... and placing them to the right".
- **Context creation never checks depths.** The D3CONTXT revision 12 log says it "removed the error code for Z /
  render-target depth mismatch" (`VH5/D3CONTXT.C:33-34`). `ddiContextCreate` fails only on a missing handle
  (`:466-498`). `SETRENDERTARGET` has no depth check either (`VH5/D7DP2.C:69-142`).
- **Per draw.** The Z scale and the pixel depth come from the *target* (`SETSURFACEPIXELDEPTH`,
  `VH5/D3GLOBAL.H:831-859`): `zScale` is 65536 at 16 bpp and 16777216 at 32 bpp. Z bias scaling follows the
  primary depth (`VH5/D3RSTATE.C:1285-1288`).
- **DX8 intent.** D16 carries `D3DFORMAT_OP_ZSTENCIL | D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH`
  (`VH5/D3INIT.C:2550-2557`), and S8D24 carries plain `ZSTENCIL` (`:2559-2566`). 3dfx meant D16 to pair with any
  colour depth.

### 3.3 Ours, and what to change

- **The rule.** `vcr_rt_format` refuses a 32 bpp target unless its Z is 32-bit
  (`KMD/include/vcr_rtfmt.h:79-87`). `ContextCreate` turns that refusal into `DDERR_INVALIDPIXELFORMAT`
  (`KMD/display/vcrdd_d3d.c:1190-1196`).
- **What the runtime says.** The D3D8 runtime on `.124` answers `zmatch X8R8G8B8/D16 = 1` (`EV/caps_124.txt`).
- **What happened.** The on-box probe got `CreateDevice 8876086c` with the ring message "ContextCreate refused:
  32 bpp target with a 16-bit Z on a VSA-100" (`EV/onbox/d3dprobe_render_640x480x32_d16.txt`).
- **The reverse case already works.** We draw a 16 bpp target with a 32-bit Z as 16/16, using the first half of
  each Z row (`KMD/include/vcr_rtfmt.h:70-78`).

**The vendor pattern applied to us (inferred design):**

- **(a)** When `rt32` is armed, back every D16 surface in video memory with 32 bits per pixel. Report its pitch
  as the 32-bit pitch and its depth as 16 to the runtime, or relabel it as 3dfx does.
- **(b)** Pair such a Z with a 32 bpp target as 24-bit depth.
- **(c)** Convert Clear2 and DDBLT depth fills from 16 to 24 bits by MSB replication, `(d << 8) | (d >> 8)`, the
  SPEC LFB rule. Do not copy 3dfx's high-word copy, which is right only for 0 and `0xFFFF`.
- **(d)** Keep `ContextCreate` refusing only pairs that are really impossible. A Z that is too small or has a
  bad pitch is still refused.

**Lock is the risk.** An application that locks a promoted D16 and reads 16-bit words sees 24-bit values. 3dfx
accepted that risk: the `FX_ORIGINALLY_*` flags are read only by the depth fill (`VH5/DDBLT32.C:943`, `:953`),
and `DdLock` (`VH5/DDSURF.C:1523`) never looks at them. Put the change behind a Diag switch and test it with
d3dprobe's existing `640x480x32_d16` case (vendor-02).

---

## 4. The texture-stage pipeline: `combineMode`

### 4.1 Hardware

- **Register layout.** `combineMode` exists in the FBI and in each TMU (`VH5/H3DEFS.H:379-530`). Bit 31
  `USE_COMBINE_MODE` switches the colour/alpha combine units from the legacy `fbzColorPath`/`textureMode` fields
  to `combineMode`'s fields. Bit 30 `DISABLE_CHROMA_SUBSTITUTION` makes `chromaRange` hold TMU constant colours.
  Bit 29 is 2ppc.
- **What the fields add.** On the legacy path a combine unit does `(other - local) * f + local`-style blends.
  `combineMode` adds:
  - **invert stages** on the "other" and "local" inputs. `INVERT_OTHER` is X, 0-X, 1-X or X-0.5
    (`VH5/H3DEFS.H:492-497`). `INVERT_LOCAL` is **0-X (the default)**, X, 1-X or X-0.5 (`:498-503`).
  - an **output shift** of 1x, 2x or 4x (`:505-509` for the TMU colour unit; the FBI's is at bits 14-15 and
    26-27 per the field list).
  - **extra input selects**: iterated RGB/A and chroma-range RGB/A inside the TMUs (`:462-491`).
- **SPEC** on bit 30: "When combineMode[30]=1, then the chromaRange register is used to store constant color
  values in the texture units for texture blending." The chromaRange layout also notes "31:24 Constant Alpha
  (used by texture units only)".
- **Stage mapping.** "Remember Stage 0 is actually TMU1 ... and Stage 1 is actually TMU0"
  (`VH5/D6MT.C:4943-4947`). Texels flow TMU1 -> TMU0 -> FBI. Ours is the same (`KMD/display/vcrdd_d3d.c:919-960`).

### 4.2 One active stage: the FBI does the operation

- **The TMU.** It `REPLACE`s, passing the texel through and keeping the trilinear bits
  (`VH5/D6MT.C:4505-4507`).
- **The FBI.** Its colour combine unit applies the stage's colour and alpha op through the `CCU_miniStageOp`
  tables (`VH5/D6MT.C:4507-4639`; tables `:598-744`). Settings:
  - `fbzColorPath |= SST_ENTEXTUREMAP` "to get TCU non-textured (ie iterated only) data into FBI"
    (`:4451-4452`);
  - iterated alpha is always sent (`:4502-4504`);
  - TFACTOR is the FBI's `c1`/`c0` (`:598-744`).
- **Our equivalent.** This is our current single-stage model (`color_path`, `KMD/display/vcrdd_d3d.c:676-742`),
  except that ours stays on the legacy CCU. So the FBI side of the same change is also `combineMode` bit 31
  with the CCU's invert and shift fields.

### 4.3 Two active stages

1. 2ppc is turned off: "Disable 2 pixel-per-clock mode for multitexturing" (`VH5/D6MT.C:4888-4895`).
2. Stage 0 is programmed into TMU1 and stage 1 into TMU0 (`:4943-4947`), using the `miniStageOp` TMU tables
   (`:748-849`, `:853-1039`).
3. A stage's "Factor" argument is the TMU's chroma-range constant (`CR_RGB`/`CR_A`) or chroma key.
4. The TMUs can select iterated RGB and alpha (`VH5/H3DEFS.H:468-469`, `:488-489`), so diffuse works as a
   stage-0 argument on Napalm.
5. The Unreal Tournament 3-stage remap (PRS 12099) is in the **non-Napalm** branch
   (`VH5/D6MT.C:5326-5396`; the `else` at `:5326` pairs with `if (IS_NAPALM)` at `:5003`).

### 4.4 Operation recipes (stage 0 in TMU1)

From the `mini_twoStage_s0_colorOp` table (`VH5/D6MT.C:853-900`). Column 2 is the `textureMode` TC bits, and
column 4 is the TMU `combineMode` bits. Arguments arrive through the OTHER, LOCAL or MSELECT_7 multiplexers
(`:493-508`).

| D3D op | textureMode TC | TMU combineMode | arguments | line |
|---|---|---|---|---|
| DISABLE | none | `OTHERSELECT_IRGB` (passes iterated colour) | none | `:856-857` |
| SELECTARG1 / SELECTARG2 | none | none | the arg through OTHER | `:858-861` |
| MODULATE | `REVERSE_BLEND` | none | arg1 through OTHER, arg2 through MSELECT_7 | `:862-863` |
| MODULATE2X / 4X | `REVERSE_BLEND` | `TC_OUTSHIFT_2X` / `_4X` | same | `:864-867` |
| ADD | `SUB_CLOCAL` | `INVERT_LOCAL_X` (local enters as +X) | arg1 OTHER, arg2 LOCAL | `:868-869` |
| ADDSIGNED / 2X | `SUB_CLOCAL` | `INVERT_LOCAL_X_MINUS_HALF` (+ `OUTSHIFT_2X`) | same | `:870-873` |
| SUBTRACT | `SUB_CLOCAL` | default `INVERT_LOCAL` = 0-X | same | `:874-875` |
| ADDSMOOTH | `ZERO_OTHER + SUB_CLOCAL + ADD_CLOCAL + REVERSE_BLEND` | `INVERT_LOCAL_ONE_MINUS_X` | arg1 LOCAL, arg2 MSELECT_7 | `:876-877` |
| BLENDDIFFUSEALPHA | `SUB_CLOCAL + ADD_CLOCAL + MSELECT + REVERSE_BLEND` | `MSELECT_7_IA` (iterated alpha) | arg1 OTHER, arg2 LOCAL | `:878-879` |
| BLENDTEXTUREALPHA | as above, with `MALOCAL` | none | same | `:880-881` |
| BLENDFACTORALPHA | as above | `MSELECT_7_CR_A` (TFACTOR alpha) | same | `:882-883` |
| BLENDTEXTUREALPHAPM | `ADD_CLOCAL + REVERSE_BLEND` | `INVERT_OTHER_ONE_MINUS_X + OTHERSELECT_LOCAL_TA` | arg1 LOCAL, arg2 MSELECT_7 | `:884-885` |
| BLENDCURRENTALPHA | as BLENDDIFFUSEALPHA | `MSELECT_7_IA` | same | `:886-887` |
| MODULATEALPHA_ADDCOLOR and the other three MODULATE*_ADD* ops | `ADD_CLOCAL + REVERSE_BLEND` | `INVERT_OTHER_ONE_MINUS_X` for the INV forms | mixed multiplexers | `:889-896` |
| PREMODULATE, BUMPENVMAP, BUMPENVMAPLUMINANCE, DOTPRODUCT3 | **FALSE: not supported** | | | `:888`, `:897-899` |

The stage-1 colour table (TMU0) is at `:902-949`, the alpha-op tables at `:951-1039`, and the FBI (CCU) tables
used for one stage at `:598-744`.

### 4.5 Argument modifiers

- **`D3DTA_COMPLEMENT`.** The table entry carries XOR masks (`ARG1_COMP_XOR_CM` and the others,
  `VH5/D6MT.C:504-512`). Complementing an argument XORs the invert field into `combineMode`, for example
  `COMP_OTH = INVERT_OTHER_ONE_MINUS_X` (`:512`). It is applied at `:4536-4543` (alpha) and `:4576-4583`
  (colour). Validation refuses a complement the op's entry does not allow (`:3527-3537`).
- **`D3DTA_ALPHAREPLICATE`.** "The nature of the register allows us to add to the field to select alpha arg
  instead of color arg". The code adds `OTHERSELECT_IA`, `LOCALSELECT_IA` or `MSELECT_7_IA` to the selected
  field, or switches `fbzColorPath` MSELECT from `MRGBTMU` to `MATMU` (`VH5/D6MT.C:4596-4639`; the TMU forms are
  at `:5093-5160` and `:5253-5320`). Each select field lists the RGB source and then its alpha
  (`VH5/H3DEFS.H:464-471`), so adding 1 picks the alpha.

### 4.6 TFACTOR

When `D3DRENDERSTATE_TEXTUREFACTOR` changes, Napalm writes the factor to four registers
(`VH5/D6DP2.C:7014-7028`):

- both TMUs' `chromaKey` and `chromaRange` (chip field 6);
- the FBI's `c0` and `c1`.

Because `combineMode` bit 30 is always set (`VH5/D6MT.C:4412-4414`), the TMUs read it as a constant colour, with
its alpha in `chromaRange[31:24]`. It is also re-sent after a FastFill (`VH5/D7DP2.C:744-745`).

The TMU chroma registers are therefore **reserved for TFACTOR**. The texture colour key uses only the FBI's
`chromaKey`: "Don't write the TMU chromakey (its used for texturefactor)" (`VH5/D6DP2.C:6943-6951`).

Ours writes TFACTOR to `c1` only (`KMD/display/vcrdd_d3d.c:1019-1020`). That works for an FBI-side stage, but
not for stage 1 (TMU0) or for `BLENDFACTORALPHA` inside a TMU (vendor-05).

### 4.7 What validation refuses

`ddiValidateTextureStageState` starts at `VH5/D6MT.C:3186`. It refuses:

- **more than 2 stages** on Napalm (`:3321-3328`);
- **multitexture with trilinear** (any stage with `MIPFILTER=LINEAR`), with `D3DERR_TOOMANYOPERATIONS`, unless
  `LOD_DITHER` is on (`:3265-3305`; `LOD_DITHER` defaults to 0, `VH5/D3INIT.C:1014-1024`);
- **`D3DTA_SPECULAR` as an argument**: "Napalm doesn't support specular as an argument" (`:3184`, `:3538-3546`);
- **unsupported ops or complements** (`:3520-3537`).

Ours always answers OK (`KMD/display/vcrdd_d3d.c:1905-1910`, per `EV/reports/games.md` 5.2). UT2003 and UT2004
use MODULATE2X without checking (`EV/reports/games.md` 5.3) (vendor-38).

### 4.8 Legacy `TEXTUREMAPBLEND` (DX5-era titles)

3dfx maps each legacy blend mode onto stage 0's texture stage states when the state changes
(`VH5/D3RSTATE.C:2851-2936`):

| legacy mode | stage-0 colour op | stage-0 alpha op | line |
|---|---|---|---|
| DECALALPHA | BLENDTEXTUREALPHA (texture, diffuse) | SELECTARG2 (diffuse) | `:2862-2870` |
| MODULATEALPHA | MODULATE (texture, diffuse) | MODULATE (texture, diffuse) | `:2872-2881` |
| DECAL, COPY | SELECTARG1 (texture) | SELECTARG1 (texture) | `:2883-2891` |
| MODULATE | MODULATE (texture, diffuse) | SELECTARG1 (texture), plus the internal marker `0x7ffffffe` | `:2893-2902` |
| DECALMASK, MODULATEMASK | as DECAL / MODULATEALPHA, plus `SST_ENALPHAMASK` | same | `:2904-2922` |
| ADD | ADD (texture, diffuse) | SELECTARG2 (diffuse) | `:2924-2933` |

- `setupTexturing` reads the MODULATE marker for the legacy alpha rule (`VH5/D6MT.C:4815-4868`).
- The DECALMASK and MODULATEMASK caps were withdrawn (PRS 8369), and Armor Command needed `ENALPHAMASK`
  stripped (`VH5/D6MT.C:4416-4423`).
- **Ours.** We advertise DECAL, MODULATE, MODULATEALPHA, COPY and ADD (`KMD/display/vcrdd_d3d.c:2041-2043`) but
  never act on `D3DRENDERSTATE_TEXTUREMAPBLEND`. `set_rs` only stores it (`:1393-1406`), and `compute_regs`
  builds stage 0 from the texture stage states alone (`:676-742`). Only the legacy `TEXTUREHANDLE` is honoured
  (`:1400-1401`).
- **Consequence.** A DX5-interface title that sets MODULATEALPHA, DECAL or COPY gets the stage-0 defaults
  instead: MODULATE, with texture alpha. This assumes the runtime passes the legacy state through rather than
  translating it, which 3dfx's own handler suggests (inferred). Jedi Knight and Mysteries of the Sith are DX5
  execute-buffer titles (`EV/reports/games.md` 4.10). A d3dprobe log of the render states such a title sends
  would settle it (vendor-37).

---

## 5. Texture sampling and memory

### 5.1 Filtering, mip modes, trilinear

- **Magnification and minification filters.** Any filter other than `D3DTFG_POINT`/`D3DTFN_POINT` sets
  bilinear (`SST_TMAGFILTER`/`SST_TMINFILTER`); POINT stays point-sampled (`VH5/D6MT.C:5613-5619`).
- **`MIPFILTER=NONE`.** Uses the no-mipmap base and LOD (`:5621-5629`). With `AUTOMIPMAP=1` the driver ignores
  this; that key defaults to 0 (`VH5/D3INIT.C:1916`).
- **"Home".** A texture lives once in video memory and is marked **`HOME_BOTH`**: both TMUs address the same
  copy (`VH5/D3TXTR.C:2189-2193`).
- **Single-stage trilinear** (`MIPFILTER=LINEAR`, home BOTH):
  - TMU0 `textureMode` is `TRILINEAR + TC_BLEND_LODFRAC + TCA_BLEND_LODFRAC + REVERSE_BLEND`
    (`VH5/D6MT.C:5474-5475`);
  - TMU1 is `TRILINEAR + REPLACE` (`:5502`);
  - TMU1's `tLOD` carries `LOD_ODD` (`:5525`).

  So TMU1 samples the odd LODs of the same chain, and TMU0 blends its even-LOD sample with them by the LOD
  fraction. **It costs both TMUs and turns 2ppc off** (`:5671-5687`).
- **Point mip.** Gets a +0.5 LOD bias (`LB_HALF`, "end users expect the hardware to round to the closest LOD",
  `:5507-5524`).
- **TMU0-only home with linear mip.** Uses `SST_TLODDITHER`, a dithered LOD and not true trilinear, with a
  +0.25 bias (`LB_DITH`, `:5472`, `:5518`).
- **Multitexture with linear mip.** Refused by validation unless `LOD_DITHER` is on (4.7). When it is allowed,
  each stage gets `TLODDITHER` (`:5931-5933`, `:6097-6099`).
- **SLI only:** a per-chip LOD-bias offset approximates the blend across chips (5.2).
- **Registry overrides.** `FORCE_BILINEAR` and `FORCE_TRILINEAR` flags (`VH5/D3RSTATE.C:2233-2260`;
  `VH5/FXGLOBAL.H:122-123`).
- **Ours.** We advertise point mips only (`MIPFPOINT`, `KMD/display/vcrdd_d3d.c:2036-2040`). Single-stage
  trilinear is the cheap win, and it needs TMU1 programmed even for one texture (vendor-07).

### 5.2 LOD bias and MAXMIPLEVEL

- **`D3DTSS_MIPMAPLODBIAS`.** Converted to the tLOD 4.2 `LODBIAS` field (bits 12-17) by `CalcBias42`
  (`VH5/D6MT.C:5550-5580`, `:5636-5642`). If the application set none, two stages get +0.5 by default
  (`2 << SST_LODBIAS_SHIFT`, `:5927`, `:6093`).
- **Registry `LOD_BIAS`.** Clamped to -32..31 (`VH5/D3INIT.C:971-992`) and added in (`VH5/D6MT.C:5412-5445`).
- **Per-chip LOD bias (SLI with multitexture and trilinear).** Each chip in SLI gets an offset: 0/0.5 for 2
  chips, or 0/0.25/0.5/0.75 for 4 chips, "per Gary Tarolli" (`VH5/D6MT.C:4300-4348`). It is written per chip
  through `chipMask` (`VH5/D6DP2.C:6479-6500`).
- **`D3DTSS_MAXMIPLEVEL`.** Clamps `LODMIN` up by `maxMipLevel << 2` (`VH5/D6DP2.C:6676-6706`). **Stage 0's
  value is used for both TMUs** (`:6729-6756`; the comment at `:6663-6666` admits it). F-22 Lightning 3
  (PRS 12046) drove the clamp (`:6666-6667`).
- **Ours.** Neither LOD bias nor MAXMIPLEVEL is handled (no reference to either in
  `KMD/display/vcrdd_d3d.c`) (vendor-08).

### 5.3 Address modes

- **CLAMP:** `textureMode` `SST_TCLAMPS`/`SST_TCLAMPT` (bits 6/7).
- **MIRROR:** `tLOD` `SST_TMIRRORS`/`SST_TMIRRORT` (bits 28/29) (`VH5/D6MT.C:5692-5702`, `:5936-5946`,
  `:6102-6111`; `VH5/H3DEFS.H:660-679`).
- **WRAP:** the default.
- **BORDER:** not supported (`VH5/D3INIT.C:3033`, absent from `:3081-3088`). Our caps lack MIRROR
  (`KMD/display/vcrdd_d3d.c:2044-2045`) (vendor-09).

### 5.4 Anisotropy

`dwMaxAnisotropy = 1` (`VH5/D3INIT.C:3219`). `D3DRENDERSTATE_ANISOTROPY` is a dummy handler
(`VH5/D3RSTATE.C:558-789`). It is a hardware limit (inferred: no anisotropic field exists in `textureMode` or
`tLOD`, `VH5/H3DEFS.H:237-334`, `:660-679`) (vendor-10).

### 5.5 Formats and palettes

- **The DX7 texture list** (`VH5/D3INIT.C:2020-2252`):
  - 565, 1555, 4444, 332;
  - P8;
  - 8332, L8, A8, A4L4, A8L8;
  - on Napalm, 8888, DXT1-5 and FXT1.

  A8P8 is permanently off (`|| TRUE`, `:2172`; PRS 13607 in the log). P8 is now off as well, by retro3dfx
  (`:523-530`).
- **Format to hardware mapping** (`VH5/D3TXTR.C:1918-2107`):
  - L8A8 -> AI88, A4L4 -> AI44, L8 -> I8;
  - A8 -> A8 (alpha with a zero R mask);
  - 8888, 4444, 1555, 565, 332, 8332 map directly;
  - an unknown format becomes 8888 at 32 bpp, otherwise 565.
- **Palettes.** `TXTRDOWNLOADPALETTE` writes `nccTable0[4..11]` (`VH5/D3TXTR.C:4158-4323`), and the palette is
  bound in `setDX6state` (`VH5/D6DP2.C:6783-6795`). Unreal's palette corruption is PRS 14371
  (`VH5/D6MT.C:39-47`, `:4949-4988`). retro3dfx found that P8 sampled white through this driver (above). Glide's
  own Napalm palette path is in `GL5/gtexdl.c:390-633`. That points to a driver bug, not silicon, but this is
  inferred and unverified.
- **Ours** (`EV/caps_124.txt`): 565, 1555, 4444, 8888 and DXT1/3/5, with P8 = 0. The palettized and luminance
  formats are the gap. That matters for Shogo, AvP, Rainbow Six and H&D (palettized) and for Halo
  (A8/L8 names), per `EV/reports/games.md` 5.3 (vendor-11).
- **DXT and FXT1** are decoded on the CPU for some paths. There are stretch and shrink helpers
  (`VH5/D3TXTR.C:4371-4913`) and FXT1/DXT encoders (`VH5/FXT.C`, `VH5/TCUTILS.C`).

### 5.6 Size, aspect, alignment

- **Limits.**
  - Textures are 1..2048 on Napalm (`VH5/D3INIT.C:3190-3196`).
  - Large textures need `tLOD` `TBIG` (bit 30) (`VH5/D3TXTR.C:1681-1723`, `:1393-1402`).
  - DXT dimensions must be multiples of 4 (`:1667-1680`).
  - Non-power-of-two sizes are refused (`:1725-1737`).
  - **Aspect ratios above 8:1 are stored as 8:1** (`:1790-1882`).
- **Base address math.** `texBaseAddr` is 16-byte granular (`VH5/H3DEFS.H:1047-1056`). Base =
  `start - sum_of_lod_sizes`, rounded up `(base+0xF)&~0xF` (`VH5/D3TXTR.C:3734-3766`); DXT1 has its own variant
  (`:3774-3843`). The revision log records "Took out 16 byte alignment PRS 8811" and its earlier enforcement
  (`:117-120`, `:149-151`).
- **Placement.** Textures go linear-heap first; render targets and Z go tiled first (5.8 and 10.6).
- **Texel centre.** With `PIXELCENTER=1` (the default, `VH5/D3INIT.C:1911-1914`), each texture gets
  `textureCenter[log2 size]` added to S and T (`VH5/D3TXTR.C:804-808`, `:2209-2218`).
- **Ours.** We allow 2048 only with `Diag\D3DBigTex` (`KMD/display/vcrdd_d3d.c:2156-2158`) (vendor-12).

### 5.7 Downloads and the texture cache

- **Download path.** Textures go through the command FIFO with PK5 (texture port) packets
  (`VH5/D3TXTR.C:3264-3727`, Napalm at `:3697`). AGP texturing was removed (`VH5/D3TXTR.C:63-64`).
- **Cache flush.** On every texture state send, the TMU cache is flushed: the base address is written inverted,
  then the real one is written: "Flush the texture cache by writing any other base addr"
  (`VH5/D6DP2.C:6709-6714`, `:6762-6767`).
- **Ours.** We flush only when a bound texture was written (`VcrDdD3dTexWritten`,
  `KMD/display/vcrdd_d3d.c:603-614`; `VcrDd3dTexFlush`, `KMD/display/vcrdd_3d.c:242-253`), by pointing the TMUs
  away and back with a nopCMD in between. That is the same mechanism with a narrower trigger (vendor-30).
- **Untextured draws.** 3dfx writes `tLOD = 0x820`, and on Napalm also both TMUs' `textureMode`, because "Napalm
  sometimes uses the TCUs for getting iterated data to the CCU" (`VH5/D6DP2.C:6965-6983`).

### 5.8 Render-to-texture and 32-bit targets

- `DdCanCreateSurface` accepts `TEXTURE | 3DDEVICE` surfaces. Format 565 works everywhere, and 8888 and 1555 work
  on Napalm (`VH5/DDSURF.C:111-206`; precedence quirk at `:174-182`).
- In the DX8 list, X8R8G8B8 carries DISPLAYMODE, 3DACCELERATION and offscreen render target. A8R8G8B8 carries
  `SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET` (`VH5/D3INIT.C:2256-2583`).
- Heaps (`VH5/DDMEMMGR.C`):
  - the Z goes in `TILED_HEAP1` when fullscreen (`:159-198`);
  - the flip chain goes in `TILED_HEAP2` (`:233-238`);
  - other 3D devices go tiled first (`:240-271`);
  - textures go linear first (`:286-300`);
  - alignment is the tile size, or 16 bytes when linear (`:350-361`).
- Not checked against ours in this pass (vendor-40).

---

## 6. Depth, stencil, W-buffer, Z bias

### 6.1 Z buffer

- **Compare functions.** All eight work through `fbzMode` `ZFUNC` bits 5-7 (`VH5/D3RSTATE.C:974-1058`;
  `VH5/H3DEFS.H:694-718`).
- **Enable and write.** `ZENABLE=TRUE` sets `ENDEPTHBUFFER`. Z writes (`fbzMode` bit 10) follow
  `ZWRITEENABLE` only while depth is enabled (`VH5/D3RSTATE.C:1067-1193`, the DX7 branch at `:1156-1189`).
- **Win9x revision 109** stopped clearing `zEnable`/`zWriteEnable` when no Z is attached, so a Z attached later
  takes effect ("Fixes zBuffer problem with the DX8 water sample", `W9X/D6DP2.C:32-35`) (vendor-35).
- **Defaults.** `initNewRC` sets `fbzMode = RGBWRMASK | ENRECTCLIP | ENZBIAS`, plus `DITHER2x2`
  (`VH5/D3RSTATE.C:3758-3761`). So **Z bias is always enabled** and `zaColor` decides it.

### 6.2 W-buffer

- **The state.** `ZENABLE = D3DZB_USEW` sets `SST_WBUFFER | SST_ENDEPTHBUFFER` (`VH5/D3RSTATE.C:1067-1193`).
- **Vertex setup.** `D3DDP2OP_WINFO` supplies the near and far W; the driver computes `aW`/`bW` so that the W
  sent to the chip falls within `[WMIN=1, WMAX=65520]` (`VH5/D6DP2.C:1621-1764`;
  `VH5/D3GLOBAL.H:1230-1232`). Vertex setup sends the remapped W (`VH5/D6TRI2I.C:866-921`).
- **SPEC:** "When bit(3)=1 enabling w-buffering, the inverse of the normalized w iterator is used ... a
  floating-point w-buffering scheme utilizing a 4-bit exponent and a 12-bit mantissa". The SPEC entry for
  fbzMode bit 21 reads "Depth float select (0=iterated W is used for floating point depth buffering, 1=iterated
  Z ...)".
- **Caps.** 3dfx advertises `WBUFFER` and `WFOG` (`VH5/D3INIT.C:2924-2927`).
- **Ours.** `ZENABLE = USEW` currently **disables depth** (`KMD/display/vcrdd_d3d.c:986` tests only `D3DZB_TRUE`).
  An application that asks for W-buffering loses all depth testing; that is a correctness bug, independent of
  advertising (vendor-14).

### 6.3 Z bias

- **The handler.** "Direct3D defines zbias as a value in the range 1..16". The value is shifted left by 1 at
  16 bpp and by 9 for the 24-bit Z, negated for LESS and LESSEQUAL, masked to the Z width, and put in `zaColor`
  (`VH5/D3RSTATE.C:1274-1309`). It is sent as `zaColor` in `setDX6state` (`VH5/D6DP2.C:7039-7048`). `ENZBIAS`
  is always on (6.1).
- **`zaColor` is shared.** FASTFILL takes its depth value from `zaColor`, so the vendor's FastFill restores
  `zaColor` after a clear (`VH5/D7DP2.C:627-984`). Our clears are `fastfillCMD` (`KMD/display/vcrdd_3d.c:8-9`),
  so Z bias needs the same restore (inferred).
- **Ours.** Not implemented, and no ZBIAS cap (vendor-15). UE2 and Halo use it (`EV/reports/games.md` 5.3).

### 6.4 Stencil

- **SPEC:** "the stenciling functions are only capable of being used when 32 BPP rendering is enabled
  (renderMode[1:0]=0x2)". Below 32 bpp, `setDX6state` removes `STENCIL_ENABLE` (`VH5/D6DP2.C:6271-6275`).
- **Registers.** `stencilMode` holds the function, reference, mask and write mask; `stencilOp` holds the three
  ops (`VH5/H3DEFS.H:143-186`). The handlers are at `VH5/D3RSTATE.C:1344-1820`.
- **Hardware quirk.** "When SST_STENCIL_ENABLE is cleared, hardware assumes stencil test passes ALWAYS, and
  stencil operation is REPLACE. We must also clear the write mask" (`VH5/H3DEFS.H:157-160`). So 3dfx writes
  `stencilMode = SST_STENCIL_MODE_DISABLE` (0, `VH5/H3DEFS.H:162`) and `stencilOp = 0` (KEEP) whenever
  stencil is disabled, on every NT state send (`VH5/D6DP2.C:6107-6130`). PRS 12972, the
  "stencil HW workaround", is in the D6DP2 revision log (`VH5/D6DP2.C:158-160`).
- **Clears.** FASTFILL clears stencil through `stencilMode`'s CLEAR bit and reference value
  (`VH5/D7DP2.C:627-984`). SPEC: "FASTFILL will optionally clear ... the stencil and depth buffers with the depth
  value taken from zaColor[23:0] and the stencil value taken from stencilMode[7:...]".
- **Ours.** We list D24S8 (`KMD/include/vcr_rtfmt.h:188-194`) but have no stencil caps and no stencil state
  handling (vendor-13). BF1942 and Halo use stencil (`EV/reports/games.md` 5.3).

---

## 7. Fog

### 7.1 Table fog

- **The table.** 64 entries indexed by W. `indexToW` is at `VH5/D3FOG.C:192-200`.
- **Generators.** Exp, exp2 and linear are at `:291-421`, with the W scale at `:402-405`.
- **Packing.** `fogTable` is packed as `(e1<<24)|(d1<<16)|(e0<<8)|d0` with deltas in .2 format (`:430-462`).
- **When it is loaded.** On a fog state change (`VH5/D6DP2.C:6987-7007`).
- **fogMode.** Table fog uses `ENFOGGING | FOG_DITHER` (`VH5/D3FOG.C:202-239`).
- **Centipede workaround.** A linear pixel-fog fix for Centipede is at `VH5/D3FOG.C:178` and `:376-394`.

### 7.2 Vertex fog

- **Where the factor comes from.** The fog factor is the vertex's specular alpha (`VH5/D6TRI2I.C:866-921`).
- **Routing** (`FnZTable`, `VH5/D3FOG.C:202-239`):
  - with no Z, or with a Z-buffer, the setup sends `Wfbi`, and fogMode is
    `ENFOGGING | FOG_Z | FOG_ALPHA | FOG_DITHER` (rows 3 and 4, `:215-222`). The fog value travels in the
    W(fbi) iterator (`STATE_REQUIRES_W_FBI | STATE_REQUIRES_VERTEXFOG`).
  - with a **W-buffer**, fogMode is `ENFOGGING | FOG_Z | FOG_DITHER` with `Z` and `Wfbi` both set up (row 5,
    `:223-226`). The fog value moves to the Z iterator, which is free when W does the depth.
  - table fog in every depth mode is `ENFOGGING | FOG_DITHER` on `Wfbi` (rows 6-8, `:227-238`).
- **Ours.** We carry vertex fog by loading `1/W(fbi)` so that it lands on a ramp fog table (`VCR_FOG_RAMP`,
  `KMD/display/vcrdd_3d.c:203-209`, `KMD/display/vcrdd_d3d.c:1000-1018`). It is proven on silicon, but it
  occupies the W iterator. **Once a W-buffer exists, vertex fog has to move off W.** The vendor's choice there,
  fog in Z, is the natural one (inferred) (vendor-16).

### 7.3 Range fog and pixel fog

- `RANGEFOGENABLE` is a dummy, and `FOGRANGE` is not advertised (`VH5/D3RSTATE.C:558-789`; `:2911-2928`).
- Fog on specular draws is handled in 8.2.

### 7.4 An undocumented bit combination (evidence gap)

- **The conflict.** SPEC: "If both bit(3) and bit(4) are set, then bit(4) takes precedence". Read literally,
  3dfx's Z-buffered vertex fog (`FOG_Z | FOG_ALPHA`) would fog by the depth value.
- **What the shipped driver implies.** 3dfx's own table puts the fog value in W(fbi) for exactly those rows
  (`VH5/D3FOG.C:215-222`). SPEC adds that "the W triangle parameter is clamped to [0,0xff] inclusive for use in
  the Alpha Combine Unit and the fog unit" (fbzColorPath bit 28). The likely reading (inferred) is that
  fogMode bits [4:3] = 3 selects the iterated W as fog alpha on Napalm.
- **Reference point.** Our own h3 Glide sets `ENFOGGING` alone for the fog coordinate and `ENFOGGING | FOG_Z` for
  iterated Z (`build/retro3dfx-glide/glide3x/h3/glide3/src/gglide.c:2012-2052`).
- **Rule.** Do not adopt the vendor combination without a d3dprobe test on silicon (vendor-17).

---

## 8. Shading, specular, blending, alpha test

### 8.1 Shading

`SHADEMODE` flat or gouraud is handled at `VH5/D3RSTATE.C:3543`. Flat shading uses the first vertex's colour
(inferred from D3D rules). `PARMADJUST` is set on every Napalm path (`VH5/D6MT.C:4409`, `:4450`, `:4886`).

### 8.2 Specular

- **Untextured triangles.** The specular colour is added into the diffuse **per vertex** with an 8-bit clamp
  (`CLAMP888`) (`VH5/D6TRI2I.C:772-800`).
- **Textured triangles.** If any vertex's specular RGB is non-zero, the triangle is **drawn a second time**
  (`VH5/D6TRI2I.C:1283-1400`):
  - **Blend.** Additive: `alphaMode` = ONE/ONE when blending is off, otherwise the app's source factor with
    destination ONE (`:1315-1326`).
  - **Depth.** `ZFUNC` EQUAL when Z writes are on (`:1328-1338`).
  - **Colour.** The FBI combine passes iterated RGB (the specular colour) through `OTHERSELECT_IRGB`
    (`:1340-1354`).
  - **Fog.** Fog switches to `FOGADD` so that `FOG(T1+T2)` holds (`:1305-1313`).
  - **Switch.** `IGX_SPECULAR_FIX` is set in `VH5/D6GLOBAL.H:102-110`.
- **Win9x "SPECULARFIX".** It made untextured specular two-pass as well, because per-vertex clamping is wrong
  across an edge ("the hardware will add diffuse & spec at each pixel and clamp", `W9X/D6LINE.C:30-36`).
- **Ours.** Specular RGB is ignored; only its alpha is read, for fog (`KMD/display/vcrdd_3d.c:203-209`). Halo and
  UE2 are the likely users (`EV/reports/games.md` 5.3, inferred there) (vendor-18).

### 8.3 Blend factors

**`alphaMode` factor codes** (`VH5/H3DEFS.H:918-949`): 0 ZERO, 1 SRCALPHA, 2 COLOR, 3 DSTALPHA, 4 ONE,
5 1-SRCALPHA, 6 1-COLOR, 7 1-DSTALPHA, 8 SAMECOLOR, 9 1-SAMECOLOR, and 0xF, which is SATURATE as a source and
COLORBEFOREFOG as a destination.

SPEC: "When the value 0x8 is selected as the destination alpha blending factor (A_SAMECOLOR function), the
destination pixel color is used ... When the value 0x8 is selected as the source alpha blending factor, the
source pixel color is used". The SPEC change log reads: "Added new alpha blending functions A_SAMECOLOR and
AOM_SAMECOLOR to allow src*src and dst*dst blending calculations".

- **SAMECOLOR (vendor-20).**
  - 3dfx maps SRCCOLOR as a source to `SST_A_SAMECOLOR` and INVSRCCOLOR to `AOM_SAMECOLOR`
    (`VH5/D3RSTATE.C:3084-3107`).
  - It maps DESTCOLOR as a destination to `SAMECOLOR` and INVDESTCOLOR to `AOM_SAMECOLOR`
    (`VH5/D3RSTATE.C:3250-3272`).
  - Ours maps both to ONE: "SRCCOLOR the chip cannot" and "DESTCOLOR the chip cannot"
    (`KMD/display/vcrdd_d3d.c:630-662`). On a VSA-100 that comment is wrong; it holds only on Voodoo3.
- **Destination alpha (vendor-19).**
  - 3dfx maps DESTALPHA and INVDESTALPHA to `DSTALPHA`/`AOM_DSTALPHA`, and SRCALPHASAT to `SATURATE`
    (`VH5/D3RSTATE.C:3119-3151`, `:3232-3245`, `:3275-3280`).
  - Then `setDX6state` rewrites the **RGB** factors on every Napalm draw, at any depth: DSTALPHA becomes ONE, and
    1-DSTALPHA, SATURATE and COLORBEFOREFOG become ZERO (`VH5/D6DP2.C:6240-6269`; "PRS 8373 fix -
    conditionally modify the source and/or destination alpha blend mode", `W9X/D6DP2.C:314-315`).
  - Below 32 bpp, the **alpha** factors that need destination alpha are clamped as well (`VH5/D6DP2.C:6271-6313`).
  - The substituted values are exactly D3D's answer for a target **without** an alpha channel, where destination
    alpha reads as 1 (inferred). A 32 bpp primary is X8R8G8B8, so 3dfx in effect assumed no target ever has
    alpha.
  - The silicon supports RGB `ADST_ALPHA` and `ASATURATE` (SPEC factor table), and at 32 bpp it supports the
    alpha-channel factors ZERO, SRCA, DSTA, ONE, 1-SRCA and 1-DSTA (SPEC "32 BPP alpha channel alpha blending
    modes").
  - **Correct design (inferred).** Use the hardware's destination alpha only when the target format has alpha
    (A8R8G8B8, or A1R5G5B5 at 15 bpp); otherwise substitute as 3dfx does.
  - **Ours.** We pass DSTALPHA to the hardware (`KMD/display/vcrdd_d3d.c:636`, `:659`) and advertise
    SRCALPHASAT (`:2025-2028`). On an X8R8G8B8 target that reads whatever alpha the last write left. Our
    alpha-channel factors are fixed at ONE/ZERO (`:997-998`).
- **BLENDOP.** `fogMode` bits 8-11 are `RGB_BLEND_REVERSE`, `RGB_BLEND_SUB`, `A_BLEND_REVERSE` and `A_BLEND_SUB`;
  the header warns "NOTE: these bits control alphablending!!!" (`VH5/H3DEFS.H:835-839`). So the subtract and
  reverse-subtract blend ops exist in silicon. MIN and MAX are not listed (inferred: absent). DX7 has no BLENDOP
  state, and 3dfx's DX8 caps leave it commented out (`VH5/D3INIT.C:3426-3430`) (vendor-21).
- **Colour write mask.** renderMode R/G/B/A write masks are "Only used when 32BPP rendering is enabled" (SPEC
  renderMode bits 17-20). `fbzMode` bit 9 is an all-or-nothing RGB mask at every depth. D3D8's
  `COLORWRITEENABLE` cannot reach a DX7 DDI (inferred) (vendor-21).

### 8.4 Alpha test, dither, stipple, AA

- **Alpha test.** `alphaMode` function plus reference (`VH5/D3RSTATE.C:3312-3429`). Ours is equivalent
  (`KMD/display/vcrdd_d3d.c:992-994`).
- **Dither.**
  - `ENDITHER` is set and cleared with `DITHERENABLE` (`VH5/D3RSTATE.C:3488-3534`).
  - The 2x2 pattern is the default (`:3758-3761`).
  - The dither rotation registers sit in `fogMode` bits 12-17 (`VH5/H3DEFS.H:841-845`), with the
    `DITHER_ROTATION` registry key defaulting to 1 (`VH5/D3INIT.C:1026`).
  - FastFill turns dither rotation off (`VH5/D7DP2.C:667-669`).
- **Stippled alpha.** Dummy handlers for `STIPPLEDALPHA` and `STIPPLEENABLE` (`VH5/D3RSTATE.C:558-789`). No
  `ALPHAFLATSTIPPLED` cap.
- **Anti-aliasing.** See section 2. Off by user choice (vendor-41).

---

## 9. Primitives

### 9.1 Triangles, strips, fans

Triangles are emitted as **packet-3 vertex runs**, `CMDFIFO_BUILD_PK3(cmd, noVert, setupFlag, packed)`
(`VH5/FIFOMGR.H:91-93`). `sSetupMode` chooses which parameters follow (`VH5/H3DEFS.H:987-998`):

- RGB, A, Z, Wfbi, W0, ST0, W1, ST1;
- FAN, EN_CULLING, CULL_NEGATIVE, DIS_PINGPONG.

Strips and fans continue with `CMD_CONT` (`VH5/D6TRI2I.C:1121-1124`, `:1242`; `VH5/D6STRIPI.C:1158-1161`).
Vertex emission is at `VH5/D6TRI2I.C:1180-1236`. The cull sign test is in software (`:730-735`). A vertex with no
diffuse colour gets white (`:760-858`).

Ours writes `sBeginTriCMD`/`sDrawTriCMD` per vertex through PCI registers (`KMD/display/vcrdd_3d.c:8-9`,
`:183-236`) (vendor-29).

### 9.2 Lines

- **Method.** `dp2Line` draws a line as a **1-pixel-wide quad** through the triangle setup unit, with specular
  `CLAMP888` and fog (`VH5/D6LINE.C:382+`).
- **NT geometry.** An X-major line spans y-1 to y (`VH5/D6LINE.C:629-700`). Win9x revision 9: "Fix for WHQL tests
  when TNL is enabled. This changes the line width to start at -.5 and goto +.5 rather than go from -1.0 to 0"
  (`W9X/D6LINE.C:27-29`).
- **MDK.** PRS#726 is at `VH5/D6LINE.C:75-81`.
- **Ours.** Line lists and strips are dropped: "not drawn yet" (`KMD/display/vcrdd_d3d.c:1690-1715`) (vendor-22).

### 9.3 Points and point sprites

`dp2Point` (`VH5/D6POINT.C:127`) draws each point as a **1x1 quad**, ±0.5 around the centre (`:254-365`). There is
no point size: `MaxPointSize = 1` even in 3dfx's DX8 caps (`VH5/D3INIT.C:3623`), so no point sprites.

### 9.4 Wireframe and point fill

`FILLMODE` (`VH5/D3RSTATE.C:3567`) makes strips and fans draw their edges through the line code, or their
vertices through the point code (`VH5/D6STRIPI.C:287-560`, `VH5/D6FANI.C:239-509`; wireframe dispatch in DP2 at
`VH5/D6DP2.C:1925`).

### 9.5 Culling, guard band, pixel centre

- **Culling.** `CULLMODE` maps to `sSetupMode` cull bits (`VH5/D3RSTATE.C:3440-3478`). Ours matches
  (`KMD/display/vcrdd_d3d.c:1023-1026`).
- **Guard band (vendor-24).**
  - renderMode bit 21 `ENGUARDBAND` with the `clip1` rectangle, set up from the viewport
    (`VH5/D6DP2.C:1562-1619`; default in `initNewRC`, `VH5/D3RSTATE.C:3807-3812`).
  - SPEC: "the left and right edges of the clipping rectangle defined in clipLeftRight1 must be aligned on even
    pixel boundries".
  - 3dfx advertises -200..2048 by -200..1536 (`VH5/D3INIT.C:3223-3229`).
  - This saves the runtime from clipping triangles that cross the screen edge (inferred).
  - Hidden & Dangerous needed clip initialisation (PRS 12728, `VH5/D3CONTXT.C:445-449`).
- **Pixel centre (vendor-25).** `PIXELCENTER=1` adds 0.5 to every x and y: "0.5 to every vertex x,y" in the
  `D3RSTATE` log (`VH5/D3RSTATE.C:379`; `pixelOffset` at `:3832-3835`; registry default at
  `VH5/D3INIT.C:1911-1914`). This confirms our `xy_bias = 0.5` (`KMD/display/vcrdd_3d.c:190-191`, `:304`).

---

## 10. Clears, flips, SLI, transport: performance

### 10.1 Clears

DP2 `CLEAR` and Clear go through **FASTFILL** (`VH5/D6DP2.C:4158-4194`; `VH5/D7DP2.C:627-984`):

- colour from `color1`;
- depth from `zaColor`, which is depth × mask;
- stencil through `stencilMode` CLEAR plus the reference value.

Dither rotation is turned off during the fill (`:667-669`). Afterwards TFACTOR is re-uploaded (`:744-745`) and the
clip registers and `zaColor` are restored. `Z_CLEAR_OPTIMIZATION`, default 1 (`VH5/DDINIT.C:886-895`), feeds the
`Z_ACCESS_OPT` hack (10.7). Ours uses `fastfillCMD` (`KMD/display/vcrdd_3d.c:8-9`) (vendor-26).

### 10.2 Swaps and vsync

- **Queue limit.** `MAXPENDINGBUFFERS_TOAVOIDHANG = 6` (`VH5/D3GLOBAL.H:749`).
- **With vsync.** At most `WaitOnVsync+1` swaps are queued, capped at 6.
- **Without vsync.** 6, commented "limit number of swap buffers to avoid hardware hang"
  (`VH5/DDFLIP.C:330-367`).
- **Registry.** `MAX_PENDING_BUFFERS` can override it, capped at 6 (`VH5/D3INIT.C:994-1012`).
- **Vsync.** `STBPERF_USE_FLIPNOVSYNC` (`W2KBUILD/STBPERF.INC`) lets `DDFLIP_NOVSYNC` through. The user's choice
  is vsync on (vendor-27).

### 10.3 SLI and the overlay for D3D

- **Overlay.** Creating a 3D flip chain calls `Enter_3DApplication` (`VH5/DDSURF.C:1112-1135`). That calls
  `Promote_DeviceToSLIAA` and `Promote_PrimaryToOverlay` (`VH5/DDFXNT.C:2399-2415`). The reason: "Desktop
  flipping can't be pipelined, so the video overlay is borrowed to display the primary flipping chain for 3D,
  exclusive-mode, full-screen applications". The overlay is also "particularly on napalm where sli requires
  overlay promotion" (`VH5/DDFXNT.C:2528-2529`).
- **SLI defaults.** `Compute_SLIAA_Config` (`VH5/DDFXNT.C:3572-3901`) chooses
  `SLI_AA_CONFIGURATION`: "We default to maximum performance if registry value is missing" (`:3601-3612`):
  - **4-way SLI with AA off on a 4-chip board** (value 5, `VH5/DDGLOBAL.H:275`);
  - 2-way SLI on 2 chips;
  - nothing on 1 chip.
- **AA.** Requested with `DDSCAPS2_HINTANTIALIASING` (`VH5/DDSURF.C:441-445`; `VH5/DDFXNT.C:3659-3660`) or the
  `ENABLE_AA` registry key (`DOCS/Napalm registry keys.doc`). Off for us by user choice.
- **Ours.** D3D runs on one chip (`EV/reports/games.md` 5.5), although our kernel driver already runs Glide in
  4-chip SLI. The vendor shows that D3D on a 4-chip VSA-100 was meant to be SLI by default. That is the largest
  performance item for fill-bound titles. It needs the per-chip LOD-bias handling of 5.2 for multitexture with
  trilinear (vendor-28).

### 10.4 Command transport

- `CMDFIFO` is defined, and FIFO0 sits in the frame buffer. AGP command FIFOs are compiled out (`AFIFO=0`,
  `VH5/SOURCES:154-159`).
- State goes in PK1 and PK4 packets, with chip fields: `CMDFIFO_BUILD_PK1CHIP` and `CMDFIFO_BUILD_PK4`
  (`VH5/FIFOMGR.H:91-101`).
- Textures go in PK5 packets (5.7). Vertices go in PK3 packets (9.1).
- Ours writes every register over PCI behind `VcrDdRoom` (`KMD/display/vcrdd_3d.c:183-224`). The roadmap names
  the move to a CMDFIFO (`EV/reports/games.md` 5.5) (vendor-29).

### 10.5 Two pixels per clock

- **When.** 2ppc is enabled for single-stage and untextured drawing when `TwoPpc` (the registry `2PPC` key,
  default on, `VH5/D3INIT.C:1046-1060`) and the mode-table flag allow it: "only when we are in a 15/16 bpp mode in
  a 'non-high' refresh/resolution screen mode and rendering single textured triangles"
  (`VH5/D6MT.C:4411-4414`).
- **When not.** It is turned off for two stages (`:4888-4895`) and for trilinear (`:5679-5686`).
- **Leaving 2ppc** (`VH5/D6DP2.C:6405-6431`):
  - 3dfx sends **12 nopCMDs to chip field 0x6** (both TMUs, not the FBI);
  - it then writes **`combineMode` first**, before any other 3D register (`:6443-6452`).
  - This is the SPEC's hardware-bug rule: "when switching from 2 pixel-per-clock rendering to single
    pixel-per-clock rendering, the TMU units must be idled ... chipField=0x6".
- **Band height.** renderMode bits 22-24 from `2PPC_LOG2_BAND_HEIGHT`, default 2 (`VH5/D3INIT.C:1062-1082`).
- **SPEC:** 2ppc "should only be enabled for 15/16 bpp rendering modes, and should not be enabled for 32bpp
  rendering modes"; with 2ppc on, "writes to either texture unit will be received by both".
- **Ours.** Our sequence knows Glide's 12-NOP rule (`KMD/include/vcr_3dseq.h:109-115`) but never enables 2ppc
  (`combineMode` is 0) (vendor-06).

### 10.6 Heaps

See 5.8. The depth buffer and the flip chain go in the tiled heaps (`VH5/DDMEMMGR.C:159-238`).

### 10.7 `Z_ACCESS_OPT`: do not copy

- **What it is.**
  - It is defined for W2K (`VH5/DDFXNT.H:68-76`).
  - It counts flips without a Z clear (`VH5/DDFLIP.C:183-220`), is reset by Z bias and depth fills
    (`VH5/D3RSTATE.C:1311-1328`; `VH5/DDBLT32.C:305-321`; `VH5/D7DP2.C:290-306`), and is used in `DdLock`
    (`VH5/DDSURF.C:1554-1573`) and `HandleSLIAA` (`VH5/D6DP2.C:5402-5457`).
- **Why it is risky.** It guesses at the application's clear pattern. A guess like that can break any title that
  clears its depth differently (inferred) (vendor-32).

---

## 11. Registry knobs (3dfx's D3D tuning surface)

The key names are prefixed `SSTH3_` (`VH5/REGKEYS.H:117-369`) and read in `D3DHALCreateDriver`
(`VH5/D3INIT.C:797-2018`). `DOCS/Napalm registry keys.doc` documents several.

| key | default | effect | line |
|---|---|---|---|
| `ANTIALIAS` | 0 | AA request | `VH5/D3INIT.C:924` |
| `CBC` | 0 | column band control | `:957-968` |
| `LOD_BIAS` | 0 (clamped -32..31, 4.2 format) | global LOD bias | `:971-992` |
| `MAX_PENDING_BUFFERS` | none (6 cap) | swap queue | `:994-1012` |
| `LOD_DITHER` | 0 | allows multitexture with trilinear (LOD dither) | `:1014-1024` |
| `DITHER_ROTATION` | 1 | dither rotation | `:1026` |
| `32BPP_RENDERING` | 1 | DDBD_32 render and 24-bit Z | `:1036-1044` |
| `2PPC` / `2PPC_LOG2_BAND_HEIGHT` | on / 2 | 2ppc | `:1046-1082` |
| `GUARDBAND_CLIPPING` | 1 | guard band | `:1084-1092` |
| `3DSMAX2.5` | off | swaps the 1555 format first | `:1829-1870` |
| `PIXELCENTER` | 1 | +0.5 xy offset and texel centre | `:1911-1914` |
| `AUTOMIPMAP` | 0 | mips even with `MIPFILTER` NONE | `:1916` |
| `SLI_AA_CONFIGURATION` | 5 / 2 / 0 by chip count | SLI/AA mode | `VH5/DDFXNT.C:3599-3612` |
| `Z_CLEAR_OPTIMIZATION` | 1 | `Z_ACCESS_OPT` | `VH5/DDINIT.C:886-895` |

`ALPHADITHERMODE`, `SWAPINTERVAL`, `TRANSFORM_AND_LIGHTING` and `CANBLTSYSMEM` are declared in
`VH5/REGKEYS.H:117-369`.

---

## 12. Per-application workarounds a retro game might depend on

These are taken from the revision logs and in-code comments. "Napalm?" says whether the workaround is in the path
a VSA-100 takes.

| application | 3dfx PRS / reference | what the driver does | where | Napalm? |
|---|---|---|---|---|
| Unreal (palette corruption) | PRS 14371 | handles a palette swap between stages in two-stage setup | `VH5/D6MT.C:39-47`, `:4949-4988` | yes |
| Unreal Tournament | PRS 12099 | remaps a 2-stage setup that uses diffuse at stage 0 to 3 stages, if the app never called ValidateDevice | `VH5/D6MT.C:72-78`, `:5326-5366`, `:487-490` | **no**, Voodoo3 branch |
| UT demo | n/a | texture handling | `VH5/D3TXTR.C:917-921` | yes |
| Quake III with the MS wrapper | n/a | an access violation fixed in textures | `VH5/D3TXTR.C:46-49` | yes |
| 3DMark 2000 | PRS 13414; 0x184 FVF | stage setup; an FVF case in DP2 | `VH5/D6MT.C:59-67`; `VH5/D6DP2.C:259` | yes |
| Armor Command | PRS 13521 | strips `ENALPHAMASK` when MODULATEMASK is used with no texture | `VH5/D6MT.C:68-69`, `:4416-4423` | yes |
| Homeworld | n/a | buffer-state setup | `VH5/D6DP2.C:61-62`, `:5268-5274` | yes |
| F-22 Lightning 3 | PRS 12046 | MAXMIPLEVEL clamp for single-LOD textures | `VH5/D6DP2.C:131-132`, `:6666-6667` | yes |
| Centipede | n/a | linear pixel-fog fix | `VH5/D3FOG.C:178`, `:376-394` | yes |
| Hidden & Dangerous | PRS 12728 | clip-register initialisation | `VH5/D3CONTXT.C:445-449` | yes |
| SpeedBusters | PRS 4335 | clears `chromaRange` at context init | `VH5/D3CONTXT.C:547-555` | yes |
| Redline Racer | n/a | texture handling | `VH5/D3TXTR.C:1749-1785` | yes |
| G-Police | n/a | texture handling | `VH5/D3TXTR.C:405-406` | yes |
| Heavy Gear II | n/a | render state | `VH5/D3RSTATE.C:179` | yes |
| Rockem3D | n/a | render state | `VH5/D3RSTATE.C:364` | yes |
| Forsaken | n/a | order of the 565 and 1555 texture formats | `VH5/D3INIT.C:384` | yes |
| Hyperblade | n/a | caps/format | `VH5/D3INIT.C:411` | yes |
| Shadows of the Empire | n/a | Z fix (merge note) | `VH5/D3INIT.C:44` | yes |
| Flanker 2.0, Roll Cage 2, TTF | n/a | DP2 fixes | `VH5/D6DP2.C:59-60`, `:66-71`, `:117-118` | yes |
| MDK | PRS#726 | lines | `VH5/D6LINE.C:75-81` | yes |
| Lego Island, Revolt, Shadow Volumes SDK | n/a | named in the D3INIT/D3RSTATE/D6DP2 revision logs | the logs (`VH5/D3INIT.C:17-494`, `VH5/D3RSTATE.C:17-446`, `VH5/D6DP2.C:40-300`) | yes |
| BEND3DIM.EXE ("LegacyApps") | n/a | per-exe flag table | `VH5/D3CONTXT.C:231-311` | **no**, non-NT and unused |

**Which of these the fleet has.**

- **Unreal Gold and UT 436 are staged.** Their D3D renderers are fallbacks on `.124`, which runs them on Glide
  (`EV/reports/games.md` 3c). The Unreal palette fix matters only once P8 is exported.
- **Hidden & Dangerous Deluxe is staged.** It is a D3D8 title on a later engine (`EV/reports/games.md` 4.4).
  3dfx's clip fix, PRS 12728, was for the original 1999 game and may not apply (inferred) (vendor-34).

---

## 13. Vendor bugs and hacks not to copy

1. **`D3DTSS_TEXTURETRANSFORMFLAGS` overwrites `ADDRESSU` and `ADDRESSV`** in the NT DP2 parsers
   (`VH5/D6DP2.C:1508-1511`; `VH5/D7DP2.C:1620-1623`, `:1750-1753`). Win9x revision 100 fixed it: "for the
   D3DTSS_TEXTURETRANSFORMFLAGS case we were loading the value into D3DTSS_ADDRESSU and D3DTSS_ADDRESSV, which
   was wrong" (`W9X/D6DP2.C:58-62`, `:1431-1437`). A DX7 title that sets transform flags then loses its wrap and
   clamp modes on 3dfx's NT driver (vendor-31).
2. **Destination-alpha substitution on every target** (8.3). Copy it only for targets without alpha (vendor-19).
3. **`Z_ACCESS_OPT`** (10.7) (vendor-32).
4. **Caps omissions**: ZTEST, MIPMAPLODBIAS, the DX8 `Caps2` dangling expression, and MaxVertexW 0 (section 2)
   (vendor-33).
5. **Stage 0's MAXMIPLEVEL used for both TMUs** (`VH5/D6DP2.C:6663-6666`, `:6729-6744`) (vendor-08).
6. **Lines from y-1 to y** instead of ±0.5 (9.2; Win9x fixed it) (vendor-22).
7. **Z-bias scale keyed to the primary's depth**, not the Z's (`VH5/D3RSTATE.C:1285-1288`). Key ours to the
   bound Z (inferred) (vendor-15).
8. **The WHQL DCT "Sub Tex1 from Tex2" pointer swap** is Voodoo3-only (`!IS_NAPALM`, `VH5/D6MT.C:5874-5882`,
   `:6040-6048`). It is irrelevant on Napalm and should not be ported.

---

## 14. The Win9x tree: fixes the NT tree lacks

| fix | where (Win9x) | NT state |
|---|---|---|
| TEXTURETRANSFORMFLAGS no longer touches ADDRESSU/V (revision 100) | `W9X/D6DP2.C:58-62`, `:1431-1437` | bug present (`VH5/D6DP2.C:1508-1511`) |
| zEnable/zWriteEnable kept when no Z is attached; set up when a Z arrives (revision 109) | `W9X/D6DP2.C:32-35` | older behaviour |
| lines span -0.5..+0.5 (revision 9) | `W9X/D6LINE.C:27-29` | y-1..y |
| untextured specular also two-pass, "SPECULARFIX" (revision 8) | `W9X/D6LINE.C:30-36` | per-vertex clamp |
| LOD-dither chip mask for subsamples (revision 108) | `W9X/D6DP2.C:37-38` | not checked |
| DX8 path (D8DP2, GetDriverInfo2, caps8) | `W9X/` | ported by retro3dfx `c34d62f`, "NOT YET DEPLOYED" |
| T&L HAL (optional, default off) | `W9X/D3INIT.C:997-1007`; `W9X/MAKEFILE:447` | not built |

The texgen fixes in the D6MT revision log are listed at `W9X/D6MT.C:25-60`, and later revision-log entries at
`W9X/D3INIT.C:27-140` (vendor-35).

---

## 15. What this means for vcr-kmd, in order (inferred design)

Every item below is new behaviour, so each goes in default-off behind its own `Diag` switch with one change per
clean boot. Each needs a d3dprobe check plus a host test before it can become default-on.

1. **Z promotion for `rt32`** (section 3). This unblocks D16 at 32 bpp, which the runtime already promises
   (vendor-02).
2. **W-buffer, so that `ZENABLE = USEW` stops disabling depth** (6.2). Map it to `WBUFFER` with an `aW`/`bW` remap
   from WINFO, and move vertex fog to the Z iterator while W-buffering (7.2). This is a correctness fix even
   before the cap is advertised (vendor-14).
3. **`combineMode` bit 31 in the FBI and both TMUs, with the op table of 4.4.** Add MODULATE2X/4X, ADDSIGNED(2X),
   SUBTRACT, ADDSMOOTH, BLENDFACTORALPHA, BLENDCURRENTALPHA, BLENDTEXTUREALPHAPM, the four MODULATE*_ADD* ops,
   COMPLEMENT and ALPHAREPLICATE. Load TFACTOR into TMU `chromaRange`/`chromaKey` (chip field 6) with bit 30, as
   in 4.6. Answer ValidateDevice honestly (4.7) (vendor-03, -04, -05, -38).
4. **SAMECOLOR blend factors** (8.3). This is a two-line change in `blend_src`/`blend_dst`, plus the caps
   (vendor-20).
5. **Destination alpha done properly**: the hardware factor when the target has alpha, ONE/ZERO when it has none
   (vendor-19).
6. **Z bias** through `zaColor`, with the restore after FASTFILL (6.3) (vendor-15).
7. **Lines and points as quads** through the existing triangle path (9.2, 9.3) (vendor-22).
8. **Texture colour key through the FBI `chromaKey`** (vendor-23).
   - 3dfx enables `SST_ENCHROMAKEY` when `COLORKEYENABLE` is set and the texture has a source colour key
     (`VH5/D6DP2.C:6823-6963`). For paletted textures the key goes through the palette (`VH5/D6DP2.C:216-221`).
   - **Limitation:** the test runs after the bilinear filter. 3dfx's comment reads "SST-1 chroma key tests after
     the bi-linear filter and this is not correct. D3D performs the chroma test before the filter"
     (`VH5/D6DP2.C:6818-6822`).
   - **Default on.** A new 3dfx context starts with `colorKeyEnable = TRUE` (`VH5/D3RSTATE.C:3709`). That is the
     DX5 behaviour, where a texture's colour key applies without any render state (inferred). DX5-era titles
     therefore get keying without asking.
   - Our caps claim TRANSPARENCY without implementing it (`EV/reports/games.md` 5.2).
9. **MIRROR, LOD bias, MAXMIPLEVEL (per stage), single-stage trilinear, and SPECULAR (second pass)** (vendor-09,
   -08, -07, -18).
10. **Stencil at 32 bpp**, with the disabled state written as `stencilMode` 0 and `stencilOp` 0 (vendor-13).
11. **Performance**:
    - guard band (vendor-24);
    - 2ppc at 16 bpp, single texture, with the 12-NOP exit (vendor-06);
    - CMDFIFO with packet-3 vertices (vendor-29);
    - **SLI for D3D** via overlay promotion (vendor-28).
12. **Palettized and luminance formats** (P8, A8, L8, A8L8, A4L4). P8 needs the palette path proven on silicon
    first, because retro3dfx saw it sample white (vendor-11).

Explicitly **out of scope**, because the hardware cannot do them (vendor-10, -36):

- hardware T&L and shaders;
- anisotropy;
- cube and volume maps;
- bump mapping and DOT3;
- border addressing;
- point sprites.

---

## 16. Evidence gaps

1. **`FOG_Z | FOG_ALPHA` together** (7.4): SPEC and the shipped driver disagree. It needs a silicon test.
2. **Why PRS 8373 substitutes destination alpha** (8.3): the code gives no reason. My reading, that it is the
   no-alpha-target rule, is inferred.
3. **P8 white sampling.** It is unknown whether the cause is a driver bug or silicon (5.5). Glide's palette path
   (`GL5/gtexdl.c:390-633`) suggests a driver bug.
4. **Z bias with the W-buffer.** SPEC does not say whether `zaColor` bias applies to W depth.
5. **Our render-to-texture and heap placement** were not compared against 5.8 in this pass.
6. **Per-chip LOD bias, SLI band height and AA** need real SLI D3D on silicon. None of it has run with our HAL.

---

## Appendix: decoding the hex caps

**Vendor caps (DX7, from the bit lists cited in section 2):**

| field | value | bits |
|---|---|---|
| DevCaps | `0xae11` | FLOATTLVERTEX 0x1, EXECUTESYSTEMMEMORY 0x10, TEXTUREVIDEOMEMORY 0x200, DRAWPRIMTLVERTEX 0x400, CANRENDERAFTERFLIP 0x800, DRAWPRIMITIVES2 0x2000, DRAWPRIMITIVES2EX 0x8000 |
| RasterCaps | `0x1449a1` | DITHER 0x1, SUBPIXEL 0x20, FOGVERTEX 0x80, FOGTABLE 0x100, ANTIALIASSORTINDEPENDENT 0x800, ZBIAS 0x4000, WBUFFER 0x40000, WFOG 0x100000 |
| ShadeCaps | `0xc53ca` | COLORFLATRGB 0x2, COLORGOURAUDRGB 0x8, SPECULARFLATMONO 0x40, SPECULARFLATRGB 0x80, SPECULARGOURAUDMONO 0x100, SPECULARGOURAUDRGB 0x200, ALPHAFLATBLEND 0x1000, ALPHAGOURAUDBLEND 0x4000, FOGFLAT 0x40000, FOGGOURAUD 0x80000 |
| TextureOpCaps | `0x1effff` | ops 0x1..0x8000, plus MODULATEALPHA_ADDCOLOR 0x20000, MODULATECOLOR_ADDALPHA 0x40000, MODULATEINVALPHA_ADDCOLOR 0x80000, MODULATEINVCOLOR_ADDALPHA 0x100000 |

**Ours (`EV/caps_124.txt`):**

| field | value | bits |
|---|---|---|
| TextureOpCaps | `0x184f` | DISABLE, SELECTARG1, SELECTARG2, MODULATE, ADD 0x40, BLENDDIFFUSEALPHA 0x800, BLENDTEXTUREALPHA 0x1000 |
| RasterCaps | `0x100191` | DITHER, ZTEST, FOGVERTEX, FOGTABLE, WFOG. The D3D8 runtime drops SUBPIXEL, which has no D3D8 equivalent (inferred). |
