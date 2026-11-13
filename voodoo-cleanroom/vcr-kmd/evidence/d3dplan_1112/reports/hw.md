# VSA-100 (Napalm): what the hardware can and cannot do for Direct3D

`voodoo-cleanroom/vcr-kmd/evidence/d3dplan_1112/reports/hw.md`, 2026-11-12.
This was a read-only investigation. No fleet box was contacted, and nothing was built, edited or committed.

**Scope.** The report covers the VSA-100 3D engine as a Direct3D HAL would drive it on the Voodoo 5 6000 in `.124`. It is independent of any driver.
Each feature gets one of three verdicts:
- **NATIVE**: one register setting does it.
- **TRICK**: it works with multipass, an allocation change, host work or a precision loss.
- **IMPOSSIBLE**: no register can do it.

Every verdict names the register field that proves it.
Claims that come from general DirectX/3dfx knowledge, and not from a file read for this report, are marked **(inferred)**.

---

## 0. Sources, and how to re-check every citation

The most important source was not on the task's list. 3dfx's own VSA-100 hardware specification sits in the vendor tree under an old file name:

| tag | path | what it is |
|---|---|---|
| **SPEC** | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/DOCS/avenger_spec.doc` (md5 `8f49e07f61450f20ecefdb4158edcfe3`) | *"Napalm High Performance Graphics Engine, Revision 1.13, October 6, 1999"*, the full register specification of the chip: fbzMode, combineMode, renderMode, stencil, textureMode, tLOD, CMDFIFO packets, programming caveats |
| **D3DXLS** | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/DOCS/D3D.XLS` (md5 `f1cf4dfefc73ef4e9dae372e216bb3ea`) | *"3dfx Direct3D Features, Version 1.6, February 17, 2000"*: a feature matrix with a Napalm column (column 6) |
| **SWARCH** | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/DOCS/SW Architecture Spec.doc` (md5 `0f3cf67b149f5569530256c1ff7e2532`) | Napalm driver architecture spec with a DX7 capability table. It is from the planning stage, and the shipped HAL differs in places (LOD bias, specular). |
| **PSEXT** | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/DOCS/PS_Napalm_External v.1.06.pdf` (md5 `10c1178f6f618f1595193c988dcefaa2`) | board product spec (bus, memory per board type) |
| **INC** | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/INCLUDE/H3DEFS.H`, `H3REGS.H` | register bit fields and register struct. The register content is identical to our Glide fork's `glide3x/h5/incsrc/h3defs.h` and `h3regs.h`; a `diff` shows only licence text and the added 8x-AA jitter constants. |
| **G5** | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/glide3/src` | our h5 Glide 3 fork |
| **VH5** | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` | 3dfx's shipped Napalm W2K D3D/DDraw HAL |
| **KMD** | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/vcr-kmd` | our driver (vcrmp.sys + vcrdd.dll) |
| **CAPS** | `KMD/evidence/d3dplan_1112/caps_124.txt` | live D3D8 caps that the runtime reports on `.124` |

How the citations work:
- **Line numbers for SPEC, SWARCH and PSEXT** (`L1234`) are lines of the text produced by `soffice --headless --convert-to txt:Text <file>` (LibreOffice 26.2.6.3), or `pdftotext -layout` for PSEXT.
- **D3DXLS rows** are rows of `soffice --headless --convert-to csv`.
- **The section number is the real anchor.** The `.doc` itself has no line numbers.
- **SPEC chapter numbering.** From chapter 12 on, SPEC's body numbers chapters one higher than its own table of contents (TOC "12. Command Transport" is body "13."). This report uses the body numbers.
- **SPEC §3.3 is out of date.** It opens with *"This section is horribly out-of-date and inaccurate ... Please ignore this section"* (L539). Anything this report cites from §3.3 is either backed elsewhere or marked as such.

```
soffice -env:UserInstallation=file:///tmp/lo --headless --convert-to txt:Text --outdir /tmp/x "<...>/H5/DOCS/avenger_spec.doc"
soffice -env:UserInstallation=file:///tmp/lo --headless --convert-to csv     --outdir /tmp/x "<...>/H5/DOCS/D3D.XLS"
```

---

## 1. Answers at a glance

"Today" means our HAL on `.124` with `D3D32=1` and `D3DBigTex=7` (CAPS, `KMD/display/vcrdd_d3d.c`).

| # | D3D feature | VSA-100 verdict | proving field(s) | today |
|---|---|---|---|---|
| Q1 | Trilinear (MIPFLINEAR) | **NATIVE for one texture, single pass**, but it takes both TMUs and loses 2 ppc. **IMPOSSIBLE in one pass with two textures** (TRICK: LOD dither / 2 passes). | textureMode[30] `trilinear`, tc_mselect=5 `LOD_frac`, tLOD[18] `lod_odd`, [19] `lod_tsplit`, textureMode[4] `tloddither` | not offered (FilterCaps 03010317) |
| Q2 | MODULATE2X/4X, ADDSIGNED(2X), SUBTRACT, ADDSMOOTH, BLEND*ALPHA(PM), MODULATE*_ADD*, COMPLEMENT, ALPHAREPLICATE, TFACTOR in a texture stage | **NATIVE** in all three combine stages (TMU1, TMU0, FBI) | combineMode[31] `USE_COMBINE_MODE`, cc/tc_outshift [15:14] (×1/2/4), invert_other [10:9] / invert_local [12:11] (x, −x, 1−x, x−½) | 2X/4X are drawn as MODULATE, ALPHAREPLICATE is dropped, combineMode is 0 |
| Q3 | Destination alpha | **NATIVE at 32 bpp, alongside depth.** 15 bpp: 1-bit. 16 bpp: only as an aux alpha buffer *with depth off*. | renderMode[1:0]=2 (alpha planes automatic), alphaMode factors 3/7 | DESTALPHA not offered; alpha always ONE/ZERO |
| Q4 | Stencil | **NATIVE, 8-bit, all 8 D3D ops, every compare, 32 bpp only** | stencilMode (ref/mask/wmask/enable/func), stencilOp (fail/zfail/zpass) | not offered (stencil caps 0) |
| Q5 | 32 bpp colour + 16-bit Z / 16 bpp + 24-bit Z | **IMPOSSIBLE natively** (one pixel-size field sets both). **TRICK by allocation** in both directions; 3dfx's HAL did it. | renderMode[1:0] | 16/32-bit Z done; 32 bpp + D16 refused, while D3D8 reports a match (CAPS) |
| Q6 | Mirror / border | Mirror **NATIVE**. Border **IMPOSSIBLE** (clamp is to the edge). | tLOD[28] `tmirrors`, [29] `tmirrort` | MIRROR not offered |
| Q7 | P8 / A8P8 / alpha palette | P8 and A8P8 **NATIVE** per the register spec, but **never shown working on this silicon** (the vintage HAL saw P8 sample white). Alpha palette **TRICK** (6-bit ARGB 6666). | textureMode format 5 / 6 / 14; palette = nccTable0 I/Q writes with bit 31 | not offered |
| Q8 | Cube, volume, DOT3, EMBM (and PREMODULATE) | **IMPOSSIBLE** | only S/W, T/W, 1/W per TMU; per-channel combine; no coordinate path between TMUs | not offered (correct) |
| Q9 | Lines / points | **No primitive.** **TRICK**: thin quad / pixel quad from triangles, as Glide and 3dfx's HAL do. | only TRIANGLE / setup commands | line caps offered, primitives skipped |
| Q10 | Depth bias (ZBIAS) | **NATIVE**, constant bias (no slope scale) | fbzMode[16] `ENZBIAS` + zaColor[15:0] / [23:0] | not offered |
| Q11 | W-buffer | **NATIVE.** 16-bit 4.12 float, 24-bit at 32 bpp. Shares the FBI 1/W (Wb) with the fog table. | fbzMode[3] `WBUFFER`, [21] `DEPTH_FLOAT_SEL` | not offered |
| Q12 | Render to texture | **NATIVE** for a *linear* 565/1555/8888 texture with pitch = width × bytes and a 16-byte-aligned base. **Not under SLI** (TRICK: SLI off for that target). | colBufferAddr[25:4], colBufferStride[13:0]/[15], texBaseAddr[0] | render-target textures keep DirectDraw's pitch |
| Q13 | AGP texturing | **IMPOSSIBLE.** The texture base is a local 26-bit address; AGP is only for DMA into local memory, and the V5 6000 sits on PCI-66 behind a bridge. | texBaseAddr; packet 6 / agpMoveCMD | n/a |
| Q14 | Anisotropic | **IMPOSSIBLE** | textureMode[2:1] point/bilinear only | MaxAnisotropy 1 (correct) |
| Q15 | Fog: per-pixel vs vertex | Table fog **NATIVE per pixel**. Vertex fog **NATIVE**: the fog factor rides the W (or Z) iterator straight into the fog unit. | fogMode[0], fogMode[4:3] source mux, fbzColorPath[28] W clamp, fogTable | vertex fog is approximated through the W table |
| Q16 | Specular | **No second colour iterator.** **TRICK**: add on the host (untextured) or a second additive pass (textured), as 3dfx did. | one iterated ARGB per triangle | not offered |
| Q17 | COLORWRITEENABLE / dither | Per-channel mask **NATIVE at 32 bpp**, all-or-nothing at 15/16 bpp. Dither **NATIVE** (2x2/4x4, dither-subtract) and must be **off at 32 bpp**. | renderMode[20:17] + fbzMode[9]; fbzMode[8], [11], [19] | mask not offered; dither is set at 32 bpp too |
| + | Blend factors / ops | **NATIVE**: all 13 D3D factors, ADD / SUBTRACT / REVSUBTRACT, separate alpha factors (32 bpp). **MIN/MAX IMPOSSIBLE.** | alphaMode[23:8], factor 8 `SAMECOLOR`, fogMode[11:8] | SRCCOLOR-as-source and DESTCOLOR-as-dest become ONE |
| + | Pipes / 2 ppc / SLI | **NATIVE**: 2 TMUs, 2 ppc for single texturing, 4-chip SLI | combineMode[29], renderMode[24:22], sliCtrl | 1 ppc, chip 0 only |
| + | Transport | **NATIVE**: CMDFIFO, packet 3 strips/fans of up to 16 vertices | §13.1.6 | MMIO through the 32-entry PCI FIFO, independent triangles |

---

## 2. The machine D3D has to work with

### 2.1 One chip: two texture units, three combine stages, one colour iterator

- **Two full TMUs per chip.** SPEC §1 L439: *"Napalm also has 2 full-featured texturing units, which allow for advanced features like trilinear filtering, dual-texturing or bump mapping to be performed at the rate of a pixel per clock"*. Also §3.5 L631.
  - Glide sets `num_tmu = 2` for the Napalm device IDs (`G5/gpci.c:1176-1183`).
  - 3dfx's HAL is built with `-DNUMTEXTUREUNITS=2` (`VH5/SOURCES:75`).
- **A pixel passes three combine stages:** TMU1 → TMU0 → FBI colour/alpha combine units.
  - Each TMU's combine is *"three Texture Color Combine Units (RGB) and one Texture Alpha Combine Unit"*, one lane per channel (SPEC §8.76 L3905).
  - The FBI's units are in SPEC §8.20 (L2731-2800).
  - With combineMode[31]=1, all three use the extended controls (SPEC §8.21 L2801-2836; `INC/H3DEFS.H:394-527`).
  - 3dfx's Voodoo3 path offered `NUMTEXTUREUNITS + 1` = 3 blend stages (`VH5/D3INIT.C:3318`); on Napalm it offered 2 (`D3INIT.C:3291`). A third texture-less stage in the FBI unit is therefore possible (inferred).
- **One colour per vertex.**
  - startR/G/B/A are FBI registers (SPEC register table L2239-2243).
  - The setup unit takes one ARGB per vertex (SPEC §8.61-8.65 L3747-3774).
- **The packed and separated colour registers reach different units.**
  - The packed `sARGB` is written to *"FBI+TREX\*"*, i.e. the TMUs too (L2360).
  - The separated `sRed`..`sAlpha` go to *"FBI"* only (L2361).
  - The TMU combine inputs "iterated RGB/alpha" (`INC/H3DEFS.H:483,493`, `SST_CM_TC_OTHERSELECT_IRGB`, `..._LOCALSELECT_IRGB`) are therefore fed only by `sARGB`. 3dfx's HAL sends packed `sARGB` (`VH5/D6TRI2I.C:1134`) and uses diffuse inside TMU stages (`VH5/D6MT.C:748-857`, `SST_CM_TC_OTHERSELECT_IRGB`).
  - **Our HAL sends `sRed`..`sAlpha`** (`KMD/display/vcrdd_3d.c:196-199`). That is harmless today, because no TMU stage selects diffuse. It becomes a trap the moment one does. This is inferred from the register table's chip column.
- **Three 1/W values.**
  - `sWb` is the FBI's global 1/W (L3781 *"a global 1/W that is sent to both the FBI and all TMUs"*).
  - `sWtmu0` is the TMUs' 1/W (L3785).
  - `sWtmu1` is TMU1's own 1/W (L3799), with its own S/W and T/W (L3803-3810; table L2366-2371; `INC/H3DEFS.H:977-981` `SST_SETUP_Wfbi/W0/ST0/W1/ST1`).
  - The FBI W drives the W-buffer and the fog table; the TMU Ws drive perspective division. Q11, Q15 and §4.10 depend on this split.
- **Render formats are 565, 1555 and 8888**, selected by renderMode[1:0] (SPEC §8.26 L3291, L3304).
  - The pipeline is 32-bit ARGB internally.
  - It is dithered to 15/16 bpp and stored undithered at 32 bpp (§3.3 L582, out-of-date section; confirmed by §8.25 L3254).

### 2.2 Two pixels per clock (2 ppc)

- **Fields.** combineMode[29] (`INC/H3DEFS.H:472`; SPEC L2819) plus the band height in renderMode[24:22] (SPEC L3300; `H3DEFS.H:144-145`).
- **Rules** (SPEC §8.21 L2838-2840; §21.6 L6233-6239):
  - It applies only to single-textured triangles.
  - Both TMUs must hold identical state; writes to either TMU reach both.
  - Leaving 2 ppc needs at least 12 NOPs addressed to the TMUs only (chip field 0x6) before bit 29 is cleared. This is a hardware bug.
  - *"should only be enabled for 15/16 bpp rendering modes, and should not be enabled for 32bpp rendering modes (except possibly for some very low resolutions)"* (L6238).
- **It works on this silicon (inferred arithmetic).** Our h5 Glide fills **1124.6 Mpix/s on four chips** (`docs/v56k-benchmark-plan.md:1095`) at the stock **166.8 MHz** (`KMD/include/vcr_clock.h:9`). That is 281 Mpix/s per chip, above the 166.8 Mpix/s a 1 ppc chip can reach, so 2 ppc is active and pays about 1.7×.
- **3dfx's HAL** turns it on by default (`VH5/D3INIT.C:1046-1061`) and at every mode set (`VH5/DDFXNT.C:790-791`). It writes it into all three combineModes (`VH5/D6MT.C:4412-4414`) and drops it for trilinear (`D6MT.C:5679`).
- **Ours:** combineMode = 0, so every D3D pixel is 1 ppc (`KMD/include/vcr_3dseq.h:119-122, 193`).

### 2.3 Four chips: scanline interleave (SLI)

- **sliCtrl** (SPEC §8.29 L3405-3428; `INC/H3DEFS.H:557-566`):
  - A chip renders line y when `(y & renderMask) == compareMask`.
  - Each chip packs its lines: `y' = [(y >> M) & ~scanMask] + [y & scanMask]` (L3427).
  - chipMask (SPEC §8.31, L3445-3482) masks register writes per chip.
- **Memory.** Every chip has its own memory, and commands can go to all of them at once.
  - SWARCH L278: *"Commands and data can be transmitted to all chips simultaneously, or to individual chips using a hardware chip mask"*.
  - SWARCH L280: the linear memory of the slave chips *"is overlapped with linear memory on the master chip"*. Linear textures are therefore replicated, while tiled render buffers are split.
- **SLI caveats:**
  - FASTFILL runs at single-chip speed in SLI and corrupts tiled SGRAM surfaces, so clears should use the 2D blitter (§8.18 L2704-2708; §21.7 L6240-6247). The 6000 uses SDRAM (PSEXT L182), so only the speed bug applies.
  - Dithering breaks with a band height of 1 (L6244).
  - A Y-origin flip needs compareMask flipped as well (§21.7).
- **Our D3D renders on chip 0 alone.** sliCtrl is only ever written as 0, by the reset (`KMD/include/vcr_3dseq.h:189`). SLI is requested only by Glide's escape (`KMD/display/vcrdd_escape.c:221-226`).
- **Fill-rate ceiling (inferred estimate):**

  | configuration | single-textured | dual-textured |
  |---|---|---|
  | 4 chips × 2 ppc × 166.8 MHz | about 1.33 Gpix/s | about 0.67 Gpix/s |
  | today: 1 chip × 1 ppc | 0.167 Gpix/s | 0.167 Gpix/s |

  Fill-bound scenes have up to 8× headroom. Scenes bound by the CPU's software T&L gain less.

### 2.4 Memory, bus, command transport

- **Memory.** 32 MB per chip (`KMD/include/vcr_sli.h:76-77`). The chip can address 64 MB (SPEC §3.6 L644; the texture base is 26 bits, §8.79 L3966).
- **Bus.**
  - PSEXT L81-82: board PCB-4 is 4-way SLI with 128 MB SDRAM (L182: 2Mx32 SDRAM at 166 MHz).
  - PSEXT L171-173: *"The SLI configurations attach to the AGP bus using a universal slot and the PCI 66 protocol ... The 4-way SLI board uses a bridge chip"*.
  - So on the 6000 the chips are PCI-66 devices behind the bridge, and the AGP-only paths are out of reach: the AGP command FIFO and the AGP DMA (packet 6, agpMoveCMD) (inferred from PSEXT).
- **PCI FIFO.** It is 32 entries deep (`INC/H3DEFS.H:111` `SST_PCIFIFO_FREE 0x1F`; SPEC §3.3 L541).
- **CMDFIFO in frame-buffer memory:**
  - It must sit in the lower 16 MB (§21.8 L6252) and counts holes, so the CPU may write out of order (§13.1 L4782-4795).
  - There are two independent streams (§13.1.10 L4941).
  - **Packet 3** carries up to 16 vertices of an independent-triangle, strip or fan stream per header. Codes 000/001/010 (§13.1.6 L4826-4880) imply sBeginTriCMD/sDrawTriCMD.
  - **Packet 5** streams LFB or texture-port data in order with rendering (§13.1.8 L4903-4906).
  - 3dfx's HAL emits packet 3 (`VH5/D6DP2.C:2188`, `CMDFIFO_BUILD_PK3`).
- **Ours:**
  - MMIO through the PCI FIFO, with room polling (`KMD/display/vcrdd_3d.c:5-7, 24-27`; `vcrdd_2d.c:92`).
  - One independent triangle per sBeginTriCMD plus two sDrawTriCMD (`vcrdd_3d.c:224-237`).
  - 8 to 14 words per vertex, and no strip reuse.
- **Strips and fans in the setup unit** (sSetupMode[16] fan, one sDrawTriCMD per new vertex; SPEC §8.59-8.60 L3718-3746) send one vertex per triangle instead of three (inferred benefit).

---

## 3. The seventeen questions

### Q1 Trilinear filtering
**Verdict.** It depends on how many texture stages are in use:
- **One texture:** NATIVE in a single pass. It needs both TMUs, and the pixel drops to 1 per clock.
- **Two textures:** IMPOSSIBLE in one pass. LOD dither or a second pass are the substitutes.

**Fields:**
- textureMode[30] `trilinear` (`INC/H3DEFS.H:348`; SPEC §8.76 L3852).
- `tc_mselect` = 5 `LOD_frac` with `tc_reverse_blend` choosing f or 1−f (`H3DEFS.H:325`; SPEC L3839).
- tLOD[18] `lod_odd` and [19] `lod_tsplit`, *"odd or even levels only"* (`H3DEFS.H:682-683`; SPEC L3918-3919).
- tLOD[23] `lod_zerofrac`, *"useful for bilinear when even and odd levels are split across two TREXs"* (L3922).
- textureMode[4] `tloddither`, which *"adds an average of 3/8 (.375) to the LOD value and needs to compensated in the amount of lodbias"* (L3861).

**How it works.**
- One TMU fetches one LOD per pixel. TMU1 samples the odd (or even) levels and passes them on; TMU0 samples the other levels and blends the two by LOD fraction.
- Glide `_grTexCheckTriLinear`: *"One pass trilinear ... Make sure that the tmu's have the levels split across the two tmu's"*, with a two-pass fix-up otherwise (`G5/gtex.c:854-984`, 903, 911).
- 3dfx's D3D tables (`VH5/D6MT.C:5453-5526`):
  - mip LINEAR gives `T0 = TRILINEAR|BLEND_LODFRAC`, `T1 = TRILINEAR|REPLACE`, with `LOD_TSPLIT/LOD_ODD`.
  - A texture that lives in one TMU only gets `SST_TLODDITHER`.
  - *"We can't do trilinear and 2ppc so disable 2ppc"* (`D6MT.C:5679`).
- **With two stages,** 3dfx's ValidateTextureStageState fails with `D3DERR_TOOMANYOPERATIONS`, *"because V5 doesn't support it"* (`D6MT.C:3266-3302`). LOD dither is the stand-in when its registry key is set.

**One copy of the texture is enough.** texBaseAddr is a full 26-bit address in shared memory (SPEC §8.79 L3966), so both TMUs read the same texture. Glide's "single pass trilinear in UMA state" depends on this (`G5/gtex.c:1410-1416`; `GR_TEXTURE_UMA_EXT`, `gpci.c:1206-1210`).

**The per-chip LOD-bias trick needs AA.** Glide's `g3LodBiasPerChip` (`G5/gtex.c:3785`) blends only when each chip renders a separate *sample* (AA), so it does not apply with AA off (inferred from its table comment).

**The 3.3 text is wrong here.** SPEC §3.3 says *"trilinear texture filtering as a two-pass operation"* (L554). That is the stale section, and §3.5 L631 (*"2 TMUs ... single-cycle ... trilinear filtering"*) contradicts it.

**Today:** no MIPFLINEAR or LINEARMIPLINEAR (CAPS FilterCaps 03010317; `KMD/display/vcrdd_d3d.c:2036-2040`). 3dfx offered MIPFLINEAR (`VH5/D3INIT.C:3057`; D3DXLS rows 172, 177).

### Q2 Texture and colour combine: MODULATE2X/4X, ADDSIGNED, SUBTRACT
**Verdict.** NATIVE, in each of the three combine stages.

**The extended combine.** In the FBI it is fbzColorPath + combineMode; in a TMU it is textureMode + that TMU's combineMode, selected by the chip field (SPEC §8.21 L2801-2836; `INC/H3DEFS.H:394-527`). The result is:

    out = invert_output?( clamp( ((A' + B') x C' + D') << outshift ) )      (clamp: inferred)
    A' = invert_other(A)  : x, -x, 1-x, x-0.5   combineMode[10:9]  (alpha [22:21])
    B' = invert_local(B)  : -x, x, 1-x, x-0.5   combineMode[12:11] (alpha [24:23]); enabled by cc_sub_clocal
    C' = mselect / mselect_7, or 1-C           fbzColorPath[12:10] + combineMode[7:6] (FBI) or [8:6] (TMU); reverse_blend picks C or 1-C
    D' = add_clocal / add_alocal (both set = texture RGB in the FBI, iterated RGB in a TMU); invert_add_local [13]
    outshift = x1 / x2 / x4                    combineMode[15:14] (alpha [27:26])

**Inputs:**
- FBI A: iterated RGB, texture RGB, color1, LFB, iterated A, texture A, color1 A, zero.
- FBI B: iterated RGB, color0, texture RGB, iterated A, color0 A, texture A, zero.
- TMU inputs: other TMU RGB/A, local texture RGB/A, iterated RGB/A, and the TMU constant colour (`H3DEFS.H:476-527`; Glide `_grCCExtcombineMode` `G5/gglide.c:5035-5183`, `grTexColorCombineExt` `G5/gtex.c:1499-1975`).
- The constant colour is per TMU: TMU chromaKey/chromaRange become constant RGB/A when combineMode[30]=1 (SPEC L2825; §8.37 L3522; §8.38).

**D3D op mapping,** as shipped by 3dfx (`VH5/D6MT.C`, FBI table 649-684, TMU table 864-948):

| D3D op | how |
|---|---|
| MODULATE2X / 4X | MODULATE + outshift 1 / 2 (`SST_CM_CC_OUTSHIFT_2X/4X`, `H3DEFS.H:436-439`) |
| ADDSIGNED | A + (B − ½): `SUB_CLOCAL` + `INVERT_LOCAL_X_MINUS_HALF` (`H3DEFS.H:433`); 2X adds outshift |
| SUBTRACT | A − B (`SUB_CLOCAL`, invert_local = −x) |
| ADD | invert_local = +x |
| ADDSMOOTH, BLENDTEXTUREALPHAPM, MODULATEALPHA_ADDCOLOR, MODULATECOLOR_ADDALPHA, MODULATEINVALPHA_ADDCOLOR, MODULATEINVCOLOR_ADDALPHA, BLEND{DIFFUSE,TEXTURE,FACTOR,CURRENT}ALPHA | all TRUE in those tables |
| PREMODULATE, BUMPENVMAP(LUMINANCE), DOTPRODUCT3 | FALSE (`D6MT.C:673, 682, 684`) |

**Argument modifiers.**
- **ALPHAREPLICATE:** select an alpha source in A, B or C (`D6MT.C:534-545`). Note *"MuxM is not supported for arep for tmu"* (`D6MT.C:540`).
- **COMPLEMENT:** use the 1−x modes. Exceptions: SWARCH L753 lists complement-of-arg2 as unsupported for ADDSIGNED(2X), SUBTRACT and BLEND*ALPHA.
- D3DXLS row 201: *"Napalm: All color/alpha argument combinations are supported."*

**D3D8 3-argument ops (inferred).**
- LERP fits `(A − B) × C + B` when arg0 is one of the C sources.
- MULTIPLYADD fits only when arg0 is the D term.

**Required bits.** combineMode[31], and combineMode[30] set, which disables texture chroma substitution (§21.8 L6248: *"When the new texture and color combine unit functionality ... combineMode[30] must be set"*).

**Today** (CAPS TexOpCaps 0000184f; `KMD/display/vcrdd_d3d.c:2165-2168`):
- MODULATE2X/4X are not advertised (0x184f lacks 0x10 and 0x20). An application that sets them anyway gets MODULATE (`vcrdd_d3d.c:714`).
- ALPHAREPLICATE is masked off.
- combineMode is written 0, the legacy path (`vcr_3dseq.h:193`).

3dfx offered the full list above (`VH5/D3INIT.C:3262-3291`).

### Q3 Destination alpha
**Verdict by render depth:**
- **32 bpp:** NATIVE, together with depth and stencil.
- **15 bpp (A1R5G5B5):** NATIVE, 1 bit.
- **16 bpp (565):** only by turning the aux buffer into an alpha buffer, which forbids depth.

**Fields and spec text:**
- 32 bpp: *"When 15-bit or 32-bit rendering is selected (renderMode[1:0]={0x1,0x2}), the destination alpha planes are automatically enabled and fbzMode bit(18) must be cleared"* (SPEC §8.25 L3266; §21.4 L6224).
- At 32 bpp, alpha writes are controlled by renderMode[20] and depth writes by fbzMode[10] (L3258, L3312).
- 16 bpp: fbzMode[18] `ENALPHABUFFER` makes the aux buffer an alpha buffer, and *"depth buffering cannot be used"* (L3266; `INC/H3DEFS.H:728`).
- Blend factors `ADST_ALPHA` = 3 and `AOMDST_ALPHA` = 7 (SPEC §8.23.2 L2955-2970; `H3DEFS.H:920`).
- Alpha-channel factors at 32 bpp: zero, srcA, dstA, one, 1−srcA, 1−dstA (L2985-2995).
- Alpha-channel factors at 15/16 bpp: only ZERO and ONE, and only with an alpha buffer (L2976-2983).
- Glide restricts the alpha-channel factors by pixel size the same way (`G5/gglide.c:682-720`).
- D3DXLS rows 255-256 and 269-270: *"Destination surface must be 32bpp."*

**Without dest alpha** (16 bpp with Z on), map DESTALPHA→ONE and INVDESTALPHA→ZERO (inferred D3D convention).

**Today:**
- The blend caps do not offer DESTALPHA (CAPS SrcBlend 1f33, DestBlend 3f).
- The alpha channel is always written with ONE/ZERO (`KMD/display/vcrdd_d3d.c:998`), so a 32 bpp target's alpha holds the raw source alpha and is never blended.

### Q4 Stencil
**Verdict.** NATIVE: 8-bit, every D3D stencil op and compare function, **32 bpp only.**

**Fields:**
- stencilMode: [7:0] ref, [15:8] mask, [23:16] write mask, [24] enable, [27:25] function, all 8 compares (SPEC §8.27 L3357-3383; `INC/H3DEFS.H:158-185`).
- stencilOp: [3:0] fail, [7:4] zfail, [11:8] zpass. Values 0-7 are keep, zero, replace, incr-clamp, decr-clamp, invert, incr-wrap, decr-wrap (L3384-3404; `H3DEFS.H:187-201`; Glide `G5/g3ext.h:101-109`).
- These are exactly the eight `D3DSTENCILOP_*` values; 3dfx offered all of them (`VH5/D3INIT.C:3241-3253`).
- *"the stenciling functions are only capable of being used when 32 BPP rendering is enabled"* (L3358). The 32 bpp aux word is 24-bit Z plus 8-bit stencil (Glide `G5/gglide.c:2356` *"the depth buffer is 24bpp"*; D3DXLS row 82).

**Clears.** FASTFILL writes the stencil reference into every stencil byte when any stencilMode[23:16] bit is set. Per-bit masks are not honoured by FASTFILL (§8.18 L2703; §8.83.3 L4056). Masked clears go through the 2D engine with a byte mask: *"Windowed & SDRAM stencil clears have to be done with hot WAX since OpenGL wants to apply the 8bit stencil write mask"* (`G5/gglide.c:2270-2296`).

**Hazard (a hardware fact).** With `STENCIL_ENABLE` clear, the hardware "assumes stencil test passes ALWAYS, and stencil operation is REPLACE", so the write mask must be cleared too (`INC/H3DEFS.H:172-177`). Our 32 bpp target setup already does this (`KMD/include/vcr_rtfmt.h:33-44`).

**Today:** stencil caps 0. D24S8 is listed, but its stencil is never written or tested (`vcr_rtfmt.h:33-52`).

**Two-sided stencil (D3D9)** is impossible, because there is a single stencilOp (inferred).

### Q5 Colour and Z depth must match
**Verdict.** IMPOSSIBLE natively, in both directions. TRICK by allocation.

**Why both depths are tied:**
- renderMode[1:0] is the only pixel-size field (SPEC §8.26 L3291).
- startZ is 20.12 at 16 bpp and 28.4 at 32 bpp (SPEC register table L2242).
- zaColor depth is [15:0] at 15/16 bpp and [23:0] at 32 bpp (§8.36 L3513; §8.25 bit 20 L3270).
- Glide reports `GR_BITS_DEPTH = pixelSize × 8` (`G5/diget.c:437-442`).
- 3dfx: *"Napalm cannot handle a different depth zbuffer and rendering surface!"* (`VH5/DDSURF.C:2210-2213`).
- SWARCH L871 and L873: 16-bit Z *"render target must be 16bpp"*; 24-bit Z *"must be 32bpp"*.

**The trick 3dfx shipped:**

| case | what to do | 3dfx source | ours |
|---|---|---|---|
| 16 bpp target + 24/32-bit Z | use a 16-bit aux buffer in the 32-bit Z surface's rows | *"silently substitute 16bpp zbuffer"* (`DDSURF.C:2243-2262`) | done (`KMD/include/vcr_rtfmt.h:70-87`) |
| 32 bpp target + D16 | allocate the Z at 32 bits a pixel | at create time on a 32 bpp desktop (`DDSURF.C:617-628`), or reallocate at attach (`DDSURF.C:2264-2346`) | refused (`vcr_rtfmt.h:84-85`) |

- A lockable D16 then holds 24-bit Z in 32-bit words. 3dfx patched DepthFill values for that (`VH5/DDBLT32.C:939-957`); a Lock conversion would be needed (inferred).
- For D3D8, 3dfx listed **D16 with `D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH`** (`VH5/D3INIT.C:2553`). That promises D16 with a 32 bpp target, which this allocation trick delivers.

**The open lead is consistent with all of this.** The D3D8 runtime on `.124` reports `X8R8G8B8 + D16: match 1` (CAPS), but `vcr_rt_format` refuses that pair at ContextCreate. The hardware cannot fix it; the remedy is on the allocation side.

**Measured the same day by the on-box run** (another agent's evidence, read here, not produced here): `d3dprobe render --full 640x480x32 --zfmt d16` failed with **CreateDevice 0x8876086c** (D3DERR_INVALIDCALL), and the ring logged *"ContextCreate refused: 32 bpp target with a 16-bit Z"* (`KMD/evidence/d3dplan_1112/onbox/README.md:29`; `onbox/d3dprobe_render_640x480x32_d16.txt:8-9`). So a title that takes the pair the runtime reports gets a failed CreateDevice.

### Q6 Mirror and border addressing
**Verdict.** Mirror: NATIVE, per axis. Border colour: IMPOSSIBLE.

**Mirror.** tLOD[28] `tmirrors` and [29] `tmirrort` (`INC/H3DEFS.H:692-693`; SPEC §8.77 L3927). This is Glide's `GR_TEXTURECLAMP_MIRROR_EXT` (`G5/gtex.c:1106`). 3dfx offered MIRROR and set it per stage (`VH5/D3INIT.C:3084`; `D6MT.C:5696`).

**Clamp is clamp-to-edge.** *"the S iterator is clamped to [0, texture width)"* (L3865).

**There is no border-colour register.** The TMU registers are textureMode, tLOD, tDetail, texBaseAddr*, nccTable, chromaKey and chromaRange (SPEC table L2380-2390; `INC/H3REGS.H:445-456`).
- D3DXLS row 190 and SWARCH L979 both say Border Color: No.
- Emulation by padding the texture with a border ring is only approximate (inferred).
- MIRRORONCE has no single bit. Mirror and clamp together are undefined (inferred).

**Today:** AddressCaps 0x15 (WRAP, CLAMP, INDEPENDENTUV); MIRROR is not offered.

### Q7 P8 textures and alpha palettes
**Verdict** (register level; see the caution below):
- P8: NATIVE per the spec, unverified on this silicon.
- A8P8: NATIVE per the spec, unverified on this silicon.
- Alpha palette (D3DPTEXTURECAPS_ALPHAPALETTE): TRICK, 6 bits per channel.

**Formats** (SPEC §8.76 L3875, L3876, L3885; `INC/H3DEFS.H:283-284, 291`):

| textureMode format | expands to | D3D format |
|---|---|---|
| 5 `P8` | palette RGB, alpha 0xFF | P8 |
| 6 `P8_ARGB6666` | 6-bit ARGB, top bits replicated | alpha palette |
| 14 `AP88` | 8-bit alpha + palette RGB | A8P8 |

**Palette storage:**
- *"The palette is written through the NCC table 0 I and Q register space when the MSB of the register write data is set"*.
- Four texels are looked up at once, so bilinear filtering works on P8 (SPEC §8.82 L4037-4040).
- Entry format: `0x80000000 | ((i & 0xFE) << 23) | rgb`, written to nccTable0[4..11], up to 8 entries per register group (`G5/gtexdl.c:476-479, 494-497`). The same format is in 3dfx's D3D (`VH5/D3TXTR.C:4148-4290`).
- 3dfx noted *"palettized ARGB are 6bit values ... the hw replicates the 2 msb's"* (`VH5/D6DP2.C:6892`).

**One palette or two.** nccTable is a "TREX\*" register (per TMU, table L2390), so each TMU can hold its own palette (inferred). Glide and 3dfx broadcast one palette to both TMUs (`G5/gtexdl.c:773`; `VH5/D6DP2.C:6788`).

**Cost.** A palette change is 256 register writes, 32 packets of 8.

3dfx listed P8 and A8P8 for DX8 (`VH5/D3INIT.C:2528, 2539`); D3DXLS rows 121 and 132 say Yes.

**Caution: no file shows a palettized texture drawing correctly on this silicon.**
- The vintage lane found P8 textures sampling *white* under 3dfx's D3D HAL on Napalm (GoldSrc in D3D: a white world under a textured RGB sky). It turned on 3dfx's own kill switch, `DISABLE_PAL8_ON_NAPALM`, which compiles P8 and A8P8 out of both format lists (`VH5/D3INIT.C:524-531, 2101, 2522`).
- The same comment records 3dfx toggling it (rev 24 "disable palettized texture", rev 25 "reenable"). 3dfx's own DX7 A8P8 entry is permanently off (`#if ... || TRUE`, `VH5/D3INIT.C:2172`).
- Our OpenGL ICD runs Counter-Strike with `FX_NO_PALETTED_TEXTURE=1` (`/home/voidsstr/development/retro-3dfx/FINDINGS.md:12112`).
- The comment's explanation, *"The Napalm P8 palette pipeline never delivers the palette to the TMU"*, is a hypothesis. The cause (the palette write path, the chip field, or the HAL) is unknown.
- The register spec says the format exists; whether it works must be measured before any cap is offered (a P8 quad in d3dprobe, or Glide `grTexDownloadTable` with a `GR_TEXFMT_P_8` texture).

**Today:** neither format is offered (CAPS `texP8: 0`).

### Q8 Cube maps, volume textures, DOT3, EMBM
**Verdict.** IMPOSSIBLE, all four, and PREMODULATE too.

**Cube and volume.**
- Each TMU receives only S/W, T/W and 1/W (`INC/H3REGS.H:302-304`; SPEC table L2244-2246, L2367-2373).
- There is no R or Q coordinate, no face select and no depth slice.
- textureMode and tLOD describe a single 2D mip chain (SPEC §8.76-8.79).

**DOT3.** The combine units are per channel: (a ± b)·c + d per R, G, B lane (SPEC §8.76 L3905). There is no cross-channel sum.

**EMBM.**
- TMU1's output enters TMU0 only as a combine input, "other texture RGB/alpha" (`INC/H3DEFS.H:479-482`).
- There is no path from a TMU result into another TMU's address generation. The only TMU-to-TMU link carries the upstream TMU's output into the next TMU's combine (trexInit1 `tt_FIFO_sil`, `rg_ttcii_inh`: SPEC §8.80 L3990, L3993).

**3dfx's own word:**
- `CUBEMAP`, `VOLUMEMAP` and `DOTPRODUCT3` are commented out (`VH5/D3INIT.C:3284, 3470-3471`).
- `BumpEnvMap` and `DotProduct3` are FALSE (`D6MT.C:682, 684`).
- D3DXLS rows 218 and 223-225 (PREMODULATE, BUMPENVMAP, BUMPENVMAPLUMINANCE, DOTPRODUCT3: No) and row 306 (cube maps: No); SWARCH L1097 (Cubic Environment Mapping: No).

**What remains.** The "bump mapping" in SPEC L439 is emboss-style (subtract a shifted height with the two TMUs) (inferred). Applications must fall back on their own.

### Q9 Lines and points
**Verdict.** There is no line or point primitive in the 3D engine. TRICK: draw them as triangles.

**Commands available:** TRIANGLE, the setup commands (sBeginTriCMD/sDrawTriCMD), NOP, FASTFILL and SWAPBUFFER only (SPEC §8.16-8.19 at L2664-2722, §8.59-8.75; register table L2266, L2302, L2311-2313). SPEC §3.3 (stale, but consistent) says: *"spans and lines are rendered as special case triangles"* (L550).

**Glide:**
- Lines are *"the triangle pair as a strip of 4 vertices"*, offset ±0.5 in the minor axis (`G5/gdraw.c:755, 831`).
- Points are *"a little triangle, with the lower left corner at pixel center"* (`G5/gdraw.c:416, 497`).

**3dfx's D3D:**
- Lines are two triangles with a sub-pixel adjustment table tuned to the reference rasterizer (`VH5/D6LINE.C:188-262`).
- Points are a 1×1 quad at ±0.5 (`VH5/D6POINT.C:259-365`).
- MaxPointSize is 1 (`VH5/D3INIT.C:3623`). SWARCH L824: *"Points ... Using Triangles"*. There are no point sprites (L825, L1104).

**Not the 2D engine.** It has Bresenham lines (SPEC §7.3.6 L2022), but no Z, texture or alpha blend, so it is useless for D3D.

**Today:** line caps are advertised (`dpcLineCaps` is filled by the same `prim_caps`, `KMD/display/vcrdd_d3d.c:2072`), but every POINT and LINE opcode is skipped, *"not drawn yet"* (`vcrdd_d3d.c:1690-1715`). That is a correctness gap, not a hardware limit.

### Q10 Depth bias
**Verdict.** NATIVE, as a constant bias.

**Fields:**
- fbzMode[16] `ENZBIAS`: *"the calculated depth value (irrespective of Z or 1/W type of depth buffering selected) is added to the depth field of zaColor"* (SPEC §8.25 L3262; `INC/H3DEFS.H:726`).
- zaColor[15:0] at 15/16 bpp, [23:0] at 32 bpp (§8.36 L3513; `H3DEFS.H:938-941`).
- Glide sets ENZBIAS with every Z or W buffer (`G5/gglide.c:3752`), and shifts the level at 32 bpp W (`gglide.c:3605-3607`).

**3dfx's D3D mapping:**
- ZBIAS 0..16 becomes `state << 1` at 16 bpp and `state << 9` at 24 bpp.
- It is negated when ZFUNC is LESS or LESSEQUAL (`VH5/D3RSTATE.C:1279-1305`).
- ZBIAS was offered (`VH5/D3INIT.C:2922`).

**Limits.**
- There is no slope-scaled bias register (D3D9 `SLOPESCALEDEPTHBIAS`) (inferred).
- zaColor is shared with the FASTFILL clear depth and with fbzMode[20] compare-to-constant (§8.25 L3270). Glide saves and restores it around clears (`G5/gglide.c:1537, 1736`). Our Clear2 writes zaColor (`KMD/display/vcrdd_3d.c:160`), so a z-bias implementation must restore it after each clear.

**Today:** ZBIAS is not offered (CAPS RasterCaps 00100191).

### Q11 W-buffer and its relation to fog
**Verdict.** NATIVE.

**Fields:**
- fbzMode[3] `WBUFFER`: *"the inverse of the normalized w iterator is used for the depth-buffer comparison ... a floating-point w-buffering scheme utilizing a 4-bit exponent and a 12-bit mantissa"* (SPEC §8.25 L3252).
- fbzMode[21] `DEPTH_FLOAT_SEL` picks W or Z for the 4.12 float depth (L3272; `INC/H3DEFS.H:713, 733`).
- At 32 bpp the W-buffer is 24-bit: Glide's `NAPALM_WDEPTHVALUE_FARTHEST 0xFFFFFF` (`G5/diget.c:783-791`); SWARCH L873; D3DXLS row 83 *"24-bit W ... Render target must be 32bpp"*.

**The W iterator is shared.**
- Both the W-buffer and the fog table use the FBI's own 1/W, `sWb`/`sOowfbi`, setup bit 3 (SPEC §8.67 L3781; fog index L2867). Texture perspective uses the separate TMU Ws.
- **W-buffer plus table (W) fog** therefore combine naturally on the same rhw. This is 3dfx's case 8: *"Fog - HWTable, ZBuffer - Wbuffer"* (`VH5/D3FOG.C:235`).
- **Vertex fog plus W-buffer** must move the fog factor off W into Z (case 5, `D3FOG.C:223`; Q15).
- Glide's fog-coordinate path uses W for fog and falls back to floating-point Z for depth (`G5/gglide.c:3758-3762`).

3dfx offered WBUFFER and WFOG (`VH5/D3INIT.C:2925`).

**Today:** WBUFFER not offered (CAPS); `D3DZB_USEW` is not handled (`KMD/display/vcrdd_d3d.c:986`).

### Q12 Render to texture
**Verdict.** NATIVE within these limits:
- The texture is linear (or tiled, see below).
- Its format is a render format: 565, 1555 or 8888.
- Pitch = width × bytes, with a 16-byte-aligned base and pitch.
- Not while SLI is on.

**Fields:**
- colBufferAddr[25:4]: *"The address must be 16-byte aligned"* (SPEC §8.40 L3568).
- colBufferStride: [15] linear or tiled; [13:0] linear stride in bytes, *"Linear stride must be 16-byte aligned"*, at most 16383 bytes (L3577; `INC/H3DEFS.H:990-1004`).
- texBaseAddr[0] linear or tiled; [31:25] tile stride (SPEC §8.79 L3966-3973).
- A linear texture's rows are packed: `lfb_texel_addr = base + lod_offset + s + t*lod_width` (§20.2 L6113). The render target's pitch must therefore equal width × bytes.

**Glide's TEXTUREBUFFER extension** does exactly this:
- It requires the format to match the render mode (*"format incompatible with rendermode"*) and uses `stride = width * bytes` (`G5/gtex.c:3494-3591`, 3541).
- It points colBufferAddr at the texture (`G5/distate.c:2085-2092`).

**Tiled targets.** Tiled render-to-texture is possible (tiled textures exist), but the tiled mip layout is complex (§20.2) (inferred).

**Under SLI** a render target is split across the chips' memories by y′ packing (§8.29 L3427). A texture rendered with SLI on is incomplete on every chip. Render-target textures need SLI off for that target, with every chip rendering the whole texture (inferred).

**Coherency.** Texture caches must be flushed after rendering into a texture: write texBaseAddr inverted and then rewrite it, between pixel flushes (§20.3 L6183-6189). Our HAL already has that sequence (`KMD/display/vcrdd_3d.c` VcrDd3dTexFlush) (inferred applicability).

**Other formats.** 3dfx's DX8 list marks 4444, 332, 8332, L8, A8, A4L4 and A8L8 as `OFFSCREEN_RENDERTARGET` (`VH5/D3INIT.C:2311-2410`). The 3D engine cannot render those formats (§8.26), so those entries would need a render-then-2D-convert path. Treat them as unverified.

**Today:** render-target textures keep DirectDraw's placement and pitch (`KMD/display/vcrdd_d3d.c:383-388`). A render target is only samplable when that pitch happens to equal w × bytes.

### Q13 AGP texturing
**Verdict.** IMPOSSIBLE.

- **Textures must be in local memory.** The texture base is *"The 26-bit texture base address (for 64 MByte address accessibility)"*, a local memory offset with no AGP or non-local bit (SPEC §8.79 L3966; `INC/H3DEFS.H:1036`).
- **What AGP is used for:**
  - the command FIFO (`SST_CMDFIFOAGP`, `H3DEFS.H:1766`; §13.1);
  - DMA *into* local memory: agpMoveCMD (§14.6 L5010-5017) and CMDFIFO packet 6, *"primarily used to transfer data from system AGP memory into frame buffer local memory"* (L4921; destination type 3 = texture port, `H3DEFS.H:1749`).
- *"Packet type 6 is pretty much broken with AGP command fifos"* (§21.8 L6253).
- **On the V5 6000** the chips sit on PCI-66 behind a bridge (§2.4), so even the AGP DMA path is out of reach (inferred).
- **3dfx's own word:**
  - SWARCH L595: *"lack of support for AGP execute mode, which our competitors use ... to texture map from system memory"*.
  - SWARCH L937: AGP Texture Memory: No.
  - `D3DDEVCAPS_TEXTURENONLOCALVIDMEM` is commented out (`VH5/D3INIT.C:2629`).
- **D3DXLS row 147 says "Yes"** for Napalm. That is an API-level line: the runtime may *place* textures in AGP and the driver copies them. It contradicts the register-level evidence above.

**Consequence.** Every texture must fit in a chip's 32 MB beside its buffers, and under SLI every chip holds a full copy. Texture management (MANAGED pool eviction) is the runtime's job.

**Uploads** can go through the texture port in packet 5, in order with rendering, with no idle needed (§20.1 L6094) (inferred benefit).

### Q14 Anisotropic filtering
**Verdict.** IMPOSSIBLE.

- textureMode offers only point or bilinear minification and magnification (bits 1-2, SPEC §8.76 L3828-3829).
- tDetail adds separate RGB/alpha filter selects only (§8.78; `INC/H3DEFS.H:696-707`).
- Each TMU takes one bilinear footprint per pixel.
- 3dfx set MaxAnisotropy = 1 (`VH5/D3INIT.C:3219`); D3DXLS rows 156 and 175; SWARCH L946.
- The only substitute is a negative LOD bias (sharper, with aliasing) (inferred).

### Q15 Per-pixel versus vertex fog
**Verdict.** Table (pixel) fog: NATIVE, per pixel. Vertex fog: NATIVE, with exact per-pixel interpolation of the factor (mux details below).

**Table fog:**
- 64 entries, each an 8-bit factor plus a 6.2 delta, written as 32 registers of 2 entries (SPEC §8.46 L3644; `INC/H3REGS.H:386`).
- Indexed by *"the MSBs of a normalized floating point representation of (1/W)"* from the FBI W. *"8 lower order bits ... are used to blend between multiple entries"* (§8.22 L2867).
- That is a per-pixel lookup with interpolation.

**Fog source mux,** fogMode[4:3] (`H3DEFS.H:845-846`; SPEC L2848-2849):

| bits | source | evidence |
|---|---|---|
| bit 3 | iterated alpha (clashes with diffuse alpha) | L2885 |
| bit 4 | upper 8 bits of iterated Z | L2885 |
| both set | **iterated W, W[39:32]**, clamped to [0,0xff] only when fbzColorPath[28]=1 (with it clear, W wraps) | 3dfx's shipping D3D; SPEC: *"When fbzColorpath bit(28)=1, then the W triangle parameter is clamped to [0,0xff] inclusive for use in the Alpha Combine Unit and the fog unit"* (L2791) |

**3dfx's vertex fog** (`VH5/D3FOG.C:202-239`; `D6TRI2I.C:866-905`):

| depth mode | fogMode | where the fog value goes |
|---|---|---|
| Z-buffer | `ENFOGGING \| FOG_Z \| FOG_ALPHA` | `sOowfbi = 255 − specular.alpha` |
| W-buffer | `ENFOGGING \| FOG_Z` | `sVz = (255 − specular.alpha) << 8`, i.e. Z[27:20] at 16 bpp's 20.12 Z (inferred: a different shift at 32 bpp's 28.4) |

**Evidence conflict.** SPEC §8.22 L2885 says *"If both bit(3) and bit(4) are set, then bit(4) takes precedence"*. That sentence looks carried over from SST-1 and contradicts both 3dfx's own code and §8.20's W-to-fog-unit note. It needs one silicon check: d3dprobe `fogvertex` (it exists, `KMD/tools/d3dprobe.c:1097`).

**FOGGOURAUD** (interpolated vertex fog) is therefore native. 3dfx offered it (`VH5/D3INIT.C:3014`).

**Z-based table fog** (non-W projection): FOG_Z gives fog linear in Z, or a remapped table can be fed a Z-derived value in Wb (inferred).

**FOGRANGE** is computed per vertex by the runtime or application; it does not involve the rasterizer (inferred). 3dfx did not offer it (D3DXLS row 78).

**Multipass helpers:** fogMode FOGADD/FOGMULT (§8.22 L2841-2866), and the destination factor 0xF `A_COLORBEFOREFOG` (L2970-2972).

**Today:** vertex fog sends a *synthetic* 1/W through a ramp table (`KMD/display/vcrdd_3d.c:201-207`). Its comment says *"the one a fog unit that only knows W allows"* (`KMD/include/vcr_fog.h:13-14`). That premise is wrong: the fog unit also takes iterated W, Z and alpha directly. FOGGOURAUD is not offered (CAPS ShadeCaps 00004008).

### Q16 Specular add
**Verdict.** No second colour iterator. TRICK:
- **Untextured:** add specular to diffuse per vertex on the host.
- **Textured:** a second additive pass.

**One colour per triangle.** There is one ARGB iterator set per triangle (§2.1; SPEC table L2239-2243, L2360-2365). D3DXLS row 309 confirms "Specular into texture stage(s): Napalm No", *"Just a second iterated color into texture stages."*

**3dfx's shipped method** (`VH5/D6TRI2I.C`):
- Untextured: `CLAMP888(color, diffuse, specular)` per vertex (775).
- Textured: a second pass of the same triangle (1295-1400):
  - the specular colour is the iterated colour;
  - blend ONE/ONE (or keep the source factor when alpha blending);
  - ZFUNC EQUAL when Z writes are on;
  - FOGADD-only fog, so `fog(T1+T2)` splits correctly;
  - auto-stripping is disabled.
- D3DXLS row 199: *"Napalm: single texture scenario only, 2-pass."*
- 3dfx offered SPECULARGOURAUDRGB (`VH5/D3INIT.C:3004`).

**Limits of the two-pass method.** It is exact for opaque surfaces and approximate under alpha blending (inferred).

**Possible single pass (UNVERIFIED, inferred).** The TMUs' iterated colour comes from `sARGB`, which is chip-field addressable ("TREX\*", L2360), while `sRed`..`sAlpha` are FBI-only. If a TMU's colour iterator can be loaded with a *different* colour than the FBI's, one free TMU (single-texture scenes) could carry diffuse into `tex × diffuse` and the FBI could add specular. This needs a d3dprobe experiment before anyone relies on it.

**Colour interpolation.** Iterated colours are interpolated affine, in screen space; only S and T are divided by W (inferred from the register set). 3dfx's DX8 `COLORPERSPECTIVE` claim (`VH5/D3INIT.C:3436`) is doubtful.

**Today:** no specular shade caps (CAPS ShadeCaps 00004008).

### Q17 Colour write mask and dither control
**Colour write mask.**

| render depth | what can be masked | fields |
|---|---|---|
| 32 bpp | each of R, G, B, A | renderMode[17]/[18]/[19]/[20] + fbzMode[9] as master (SPEC §8.26 L3295-3298, L3310-3312; §21.4 L6224-6228; `INC/H3DEFS.H:139-142`); Glide `grColorMaskExt` (`G5/distate.c:2348-2366`) |
| 15/16 bpp | all RGB together only | fbzMode[9] (L3256); renderMode[20:17] are *"Only used when 32BPP rendering is enabled"* |

- At 15 bpp, RGB and the 1-bit alpha share the one mask (§21.5).
- D3DXLS row 294: PLANEMASK impossible, *"Can only mask individual color channels"*.
- 3dfx never offered `COLORWRITEENABLE` (`VH5/D3INIT.C:3426` commented out). Its only per-channel use was a DEBUG toggle (`VH5/D6DP2.C:6177-6205`).
- **Verdict:** NATIVE at 32 bpp. At 16 bpp only 0x0 and 0xF can be honoured.

**Dither.**
- Fields:
  - fbzMode[8] enable;
  - fbzMode[11]: 0 = 4x4, 1 = 2x2 ordered;
  - fbzMode[19] dither subtraction on the destination for blending (SPEC §8.25 L3210, L3213, L3268; `H3DEFS.H:720-731`);
  - dither rotation: renderMode[25] (L3301) with the rotated-matrix selects in fogMode[19:12] (L2857-2860; the [19:16] pair is for triangles repeated during AA).
- *"fbzMode bit(8) must be cleared when 32-bit color rendering is selected"* (L3254). Glide clears it at 32 bpp (`G5/gglide.c:3905-3908`) and forces 2x2 + dither-subtract (`gglide.c:3874-3904`).
- 3dfx's D3D sets DITHER2x2 always (`fbzMode |= SST_DITHER2x2`, `VH5/D3RSTATE.C:3500` and `:3761`, the latter in initNewRC).
- In SLI, dithering breaks with a band height of 1 (§21.7 L6244).
- **Verdict:** NATIVE.
- **Today:**
  - `D3DRS_DITHERENABLE` sets `FZ_DITHER` whatever the target depth (`KMD/display/vcrdd_d3d.c:979-980`). That violates L3254 on every 32 bpp target (the `D3D32` path).
  - It uses the 4x4 matrix and no dither-subtract.

---

## 4. Other capabilities the task names

### 4.1 Alpha blending factors and operations
**Factors.** The RGB factor table (SPEC §8.23.2 L2955-2972; `INC/H3DEFS.H:904-936`) covers all 13 D3D blend factors natively:

| D3D factor | as source | as destination |
|---|---|---|
| ZERO / ONE | 0 / 4 | 0 / 4 |
| SRCALPHA / INVSRCALPHA | 1 / 5 | 1 / 5 |
| DESTALPHA / INVDESTALPHA | 3 / 7 (needs dest alpha) | 3 / 7 (needs dest alpha) |
| SRCCOLOR / INVSRCCOLOR | **8 / 9** (`SAMECOLOR`) | 2 / 6 |
| DESTCOLOR / INVDESTCOLOR | 2 / 6 | **8 / 9** (`SAMECOLOR`) |
| SRCALPHASAT | 0xF (`MIN(As, 1−Ad)`) | n/a |
| BOTHSRCALPHA / BOTHINVSRCALPHA | a src/dst pair | a src/dst pair |

Destination 0xF is "colour before fog" (L2969-2970).

**Alpha-channel factors are separate** (alphaMode[23:16], L2907). At 32 bpp they are zero, srcA, dstA, one, 1−srcA, 1−dstA (L2985). Every D3D factor's alpha component maps to one of these (inferred mapping).

**Operations.**
- fogMode[8]/[9] for RGB and [10]/[11] for alpha give ADD, `S·a − D·b` (SUBTRACT) and `D·b − S·a` (REVSUBTRACT) (L2853-2856; `H3DEFS.H:851-854`; Glide `G5/gglide.c:3981-4020`).
- There is no MIN or MAX.
- 3dfx left `D3DPMISCCAPS_BLENDOP` off (`VH5/D3INIT.C:3430`), and added SRCCOLOR, INVSRCCOLOR, DESTALPHA, INVDESTALPHA and SRCALPHASAT for Napalm (`D3INIT.C:3134-3161`). D3DXLS rows 251-272 agree.

**Today:**
- `blend_src` maps SRCCOLOR to ONE (*"SRCCOLOR the chip cannot"*, `KMD/display/vcrdd_d3d.c:643`).
- `blend_dst` maps DESTCOLOR to ONE (*"DESTCOLOR the chip cannot"*, `:661`).
- Both comments are wrong for VSA-100: factor 8 does exactly this.
- Alpha factors are fixed at ONE/ZERO (`:998`).

**Alpha test.** NATIVE and already used: alphaMode[0], [3:1] with 8 functions, reference [31:24] (§8.23.1 L2910-2923).

**A cheap performance trick.** Glide forces alpha test `GT 0` when blending SRCALPHA/INVSRCALPHA with Z writes off, so fully transparent pixels skip their memory write (`G5/distate.c:2032-2066`) (inferred benefit).

### 4.2 Colour key and chroma range
**Verdict.** NATIVE.

- **FBI chromaKey** (fbzMode[1]): the compare *"is performed immediately after texture mapping lookup, but before the color combine unit and fog"* (SPEC §8.25 L3224; §8.37 L3519-3527). It therefore keys on the texel even when the texel is later modulated.
- **chromaRange** gives per-channel inclusive or exclusive ranges with union or intersection (§8.38; `INC/H3DEFS.H:944-951`).
- **TMU texture chroma key with substitution** (Glide *"substitude with black"*, `G5/gtex.c:3335`) keys in texture space, before filtering. It exists only while combineMode[30]=0, so it cannot coexist with the TMU constant colour or the new combine modes (SPEC L2825; §21.8).
- **3dfx's D3D** keys in the FBI only: *"Don't write the TMU chromakey (its used for texturefactor)"* (`VH5/D6DP2.C:6943`).

### 4.3 Stipple
**Verdict.** TRICK for D3D STIPPLEDALPHA; IMPOSSIBLE for a 32×32 STIPPLEPATTERN.

- fbzMode[2] with [12]: rotate mode (bit 31 of a 32-bit rotating register) or pattern mode (a 4×8 monochrome pattern indexed by x[2:0], y[1:0]) (SPEC §8.25 L3206, L3214 and the pattern pseudo-code; Glide `G5/gglide.c:3836-3870`).
- Rarely used by these games (inferred).

### 4.4 Texture formats
**Hardware formats.** SPEC §8.76 L3872-3896; `INC/H3DEFS.H:277-299`; Glide `G5/g3ext.h:158-169`.

| family | formats |
|---|---|
| 8-bit | RGB332, A8 (RGB = a), I8, AI44, P8, P8-6666, YIQ422 |
| 16-bit | ARGB8332, AYIQ8422, RGB565, ARGB1555, ARGB4444, AI88, AP88 |
| 32-bit | ARGB8888 |
| compressed (textureMode[31]) | FXT1, DXT1 (with 1-bit key), DXT2/3, DXT4/5 |

**D3D mapping:**
- **Native:** R5G6B5, A1R5G5B5, A4R4G4B4, A8R8G8B8, R3G3B2, A8R3G3B2, L8, A4L4, A8L8, A8, P8, A8P8, DXT1-5.
- **Native once alpha is forced to 1** (inferred): X1R5G5B5, X4R4G4B4, X8R8G8B8.
- **A8 needs care:** the hardware also returns RGB = a, where D3D expects RGB = 0 (inferred), so the combine must not use its colour.
- **Not native:**
  - R8G8B8: convert on upload.
  - Bump formats (V8U8, L6V5U5, Q8W8V8U8).
  - YUY2/UYVY as textures. SWARCH L937-940 says no; the 2D engine can convert them.
- Max texture size is 2048 (§4.5).

**Today** (CAPS): 565, 1555, 4444, 8888, DXT1, DXT3, DXT5. 3dfx's DX7 and DX8 lists added 332, 8332, L8, A8, A4L4, A8L8, P8, A8P8, DXT2, DXT4 and FXT1 (`VH5/D3INIT.C:2057-2238` for DX7, `:2294-2545` for DX8; P8 and A8P8 are compiled out in the vintage tree, see Q7).

### 4.5 Texture size, aspect, pow2, alignment, layout, coordinate range
- **Size.** 2048×2048 with tLOD[30] `tbig` (SPEC L3929; `G5/diget.c:534-544`).
- **Aspect.** Up to 8:1: tLOD[22:21] `lod_aspect` + [20] `s_is_wider` (L3921; `diget.c:545-550` returns 3).
- **Power of two only** (`diget.c:597-602`; D3DXLS row 150).
  - A non-pow2 texture can be padded with scaled coordinates (clamp, no mips only), or an aspect above 8:1 padded on the short side. Both are inferred and both are partial.
- **Base.** 16-byte units, 26 bits (`{texBaseAddr[1], texBaseAddr[24:4], 0000}`, §8.79 L3966; `SST_TEXTURE_ALIGN 0x10`, `G5/fxglide.h:3237`). The UT2003 noise of 2026-11-08 was exactly a 4-byte-aligned base.
- **Layout.** Linear or tiled (texBaseAddr[0]).
- **Multi-base mode.** tLOD[24] `tmultibaseaddr` with texBaseAddr1/2/38 places LOD 1, LOD 2 and LODs 3-8 separately (§8.79; §20 L6079-6091). This would let DirectDraw-allocated per-level chains be sampled without repacking (inferred).
- **Coordinate range.** S/W and T/W iterate in 14.18 fixed point (SPEC register table L2244-2245; the registers are `INC/H3REGS.H:302-303`), with the wide side scaled to 256 texels (`KMD/display/vcrdd_3d.c:273` VcrDd3dTexScale). That leaves about ±32 repeats before the integer part wraps (inferred).
  - 3dfx reported `dwMaxTextureRepeat` 256 with `TEXREPEATNOTSCALEDBYSIZE` (`VH5/D3INIT.C:2824-2826, 3204-3210`; `DDFXNT95.H:70`).
  - Re-basing each triangle's integer texture offset on the host is a standard Voodoo remedy for large tiled floors (inferred).

### 4.6 LOD bias, LOD clamp, filters
**Verdict.** NATIVE.

- tLOD[17:12] `lodbias`, 4.2 signed (−8 to +7.75) (SPEC L3917; Glide `grTexLodBiasValue`, `G5/gtex.c:2439-2490`).
- tLOD[5:0] and [11:6] `lodmin`/`lodmax` (D3DTSS_MAXMIPLEVEL).
- `tloddither` adds +0.375 on average (L3861).
- tDetail[21:17] gives separate RGB and alpha min/mag filters (§8.78 L3955).
- 3dfx converts D3D's float bias with `CalcBias42` (`VH5/D6MT.C:5550`). D3DXLS row 183: LOD bias Yes.
- **Today:** `D3DPRASTERCAPS_MIPMAPLODBIAS` is absent (CAPS).

### 4.7 Clears
**Verdict.** NATIVE, except under SLI.

- **FASTFILL** (SPEC §8.18 L2691-2715, §8.83.3 L4046-4058):
  - colour from color1, with per-plane masks at 32 bpp;
  - depth from zaColor[23:0] (32 bpp) or [15:0];
  - stencil from stencilMode[7:0] when any write-mask bit is set;
  - one 8-pixel 2D or block-write path.
- **Under SLI,** use the 2D blitter: FASTFILL runs at single-chip rate (L2706).

### 4.8 32 bpp and 15 bpp rendering
- **32 bpp:**
  - renderMode=2 (SPEC L3291);
  - the aux buffer holds 24-bit Z + 8-bit stencil;
  - Z iterates in 28.4 (L2242);
  - destination alpha comes for free (§21.4);
  - dithering must be off (L3254);
  - 2 ppc is not recommended (L6238).
- **15 bpp (ARGB1555):**
  - renderMode=1;
  - renderMode[16:15] chooses how the 1-bit alpha is written (0, 1 or the MSB of the blended alpha) (L3308);
  - the colour mask covers RGB and alpha together (§21.5 L6229-6232).
- **Today:** 565 and 8888 targets only (`KMD/include/vcr_rtfmt.h:63-87`). X1R5G5B5/A1R5G5B5 render targets are possible but not offered.

### 4.9 Guard band
**Verdict.** NATIVE.

- Rasterization covers [−4k, 4k] (SPEC §3.6 L655).
- renderMode[21] guard-band clipping uses clipLeftRight1/clipTopBottom1, with even-pixel left and right edges (§8.26 L3299; §8.45 L3624-3642; §21.8).
- 3dfx: GuardBand −200..2048 × −200..1536 (`VH5/D3INIT.C:3225-3228`), `renderMode |= SST_RM_ENGUARDBAND` (`VH5/D3RSTATE.C:3809-3810`).
- **Today:** guard band 0 (`KMD/display/vcrdd_d3d.c:2152-2161`). The runtime clips every primitive that crosses the viewport on the CPU (inferred cost).

### 4.10 Projected textures
**Verdict.** TRICK; plausibly native, but unverified (inferred).

- Each TMU divides by its own 1/W (sWtmu0, and sWtmu1 for TMU1 only, SPEC §8.68-8.73).
- `tclampw` exists *"when projecting textures to avoid projecting behind the source of the projection"* (L3859).
- Sending q × rhw as a TMU's W, and s × rhw, t × rhw as S and T, yields per-pixel s/q and t/q (inferred mapping).
- 3dfx did not offer it (`VH5/D3INIT.C:3469`; D3DXLS row 310).

### 4.11 Flat shading
**Verdict.** TRICK.

- There is no flat mode; send the provoking vertex's colour on all three vertices.
- 3dfx did exactly that, and noted *"HW cannot support FLAT_SHADE and Auto Strip/Fan"* (`VH5/D6TRI2I.C:767`). Strips lose their reuse under flat shading.

### 4.12 DirectDraw-side hardware that D3D titles lean on
- **Colour-keyed blits:** the 2D engine has source and destination colour-key *ranges* (8 bpp by index, 16/24/32 bpp per channel) (SPEC §7.2.11 L1781-1786).
- **Other 2D:** stretch blits (§7.3.2 L1945, §7.3.4 L2004), ternary ROPs, format conversion through srcFormat/dstFormat (§7.2.9 L1680), and YUV→RGB through the planar aperture (§19).
- **Texture memory can be a blit target,** for render-then-convert (§3.3).

---

## 5. Where the project's current beliefs contradict the hardware documents

1. **"SRCCOLOR / DESTCOLOR the chip cannot"** (`KMD/display/vcrdd_d3d.c:643, 661`) is wrong. alphaMode factor 8 `A_SAMECOLOR` gives the source colour as source factor and the destination colour as destination factor (SPEC L2966, L2972; `INC/H3DEFS.H:925-926`).
2. **"a fog unit that only knows W"** (`KMD/include/vcr_fog.h:13-14`) is wrong. fogMode[4:3] feeds iterated alpha, Z or W straight into the fog unit (SPEC L2885, L2791; 3dfx `VH5/D3FOG.C:202-239`).
3. **"Stencil ignored at 16 bpp: UNPROVEN"** (`KMD/include/vcr_rtfmt.h:41`). SPEC says stencil is *"only capable of being used when 32 BPP rendering is enabled"* (L3358). That supports the assumption without proving that a stale enable is ignored; keep the planned silicon check.
4. **Dithering at 32 bpp.** The HAL can set fbzMode[8] on 32 bpp targets (`vcrdd_d3d.c:979-980`); SPEC requires it clear (L3254).
5. **Single chip and 1 ppc.** Our D3D renders single-chip at 1 ppc, while the hardware offers 2 ppc and 4-chip SLI (§2.2, §2.3). This is the largest performance gap in the stack, and no document of ours mentions it for D3D.
6. **D16 at 32 bpp.** The runtime says CheckDepthStencilMatch(X8R8G8B8, D16) = yes, while ContextCreate refuses the pair (CAPS; `vcr_rtfmt.h:84-85`). The hardware cannot pair them, but 3dfx did so by allocation and listed D16 with `ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH` (`VH5/D3INIT.C:2553`).
7. **SPEC conflicts with itself in places,** and the newer text wins in each case: SPEC §3.3 (stale) on two-pass trilinear (L554), destination alpha without depth (L573) and a "palette offset register" (L567); and SPEC §8.22 on the fog bits[4:3] precedence (vs 3dfx's code).
8. **Palettized textures are not proven.** SPEC defines P8 and A8P8 (§8.76 L3875, L3885) and the palette write (§8.82 L4037-4039). Yet the vintage lane saw P8 sample white through 3dfx's HAL and compiled P8 and A8P8 out (`VH5/D3INIT.C:524-531`). Our OpenGL ICD also runs Counter-Strike with `FX_NO_PALETTED_TEXTURE=1` (`retro-3dfx/FINDINGS.md:12112`). Treat P8 as unverified until a probe draws it.

---

## 6. Silicon checks this report could not settle (all d3dprobe-sized)

| # | check | why | proposed probe |
|---|---|---|---|
| S1 | fogMode[4:3] = 3 uses iterated W | SPEC L2885 vs 3dfx D3FOG.C; decides the vertex-fog design | `fogvertex` with Wb = 255 − specular alpha, pixel-exact against D3D's linear fog |
| S2 | stale `STENCIL_ENABLE` at 16 bpp | the open premise in `vcr_rtfmt.h:41` and `vcr_3dseq.h:44-50` (remedy named there: Diag\Reset3D) | a 16 bpp draw after a 32 bpp session that left stencil on with a failing function |
| S3 | TMU colour iterator separately loadable via chip-field `sARGB` | a single-pass specular would need it | two different ARGB writes (FBI vs TMU0), TMU combine selects ITRGB |
| S4 | 2 ppc gain at 1024x768/1280x1024 × 16/32 bpp | §21.6 says bandwidth may eat it | `d3dprobe perf` with and without combineMode[29] (default-OFF Diag switch) |
| S5 | renderMode per-channel masks ignored at 16 bpp | SPEC says ignored, never measured | colour-mask test at 16 and 32 bpp |
| S6 | projected textures via TMU W | §4.10 | quad with q ≠ 1, compare with a CPU reference |
| S7 | SLI D3D: buffer layout, flips, LFB aperture, texture broadcast | §2.3 | the miniport already drives SLI for Glide (vcrmp_sli.c); a D3D fill test on 1/2/4 chips |

---

## 7. What this means for the plan (hardware view, by value)

1. **Performance:**
   - 2 ppc for single-textured 16 bpp draws (§2.2).
   - 4-chip SLI for D3D (§2.3): the largest gain, and the largest job.
   - CMDFIFO packet 3 and strips/fans (§2.4).
   - Guard band (§4.9).
2. **Correctness that games hit:**
   - lines and points as triangles (Q9);
   - extended combine for MODULATE2X/4X, ADDSIGNED, SUBTRACT, ALPHAREPLICATE (Q2);
   - SAMECOLOR blend factors and alpha-channel factors at 32 bpp (§4.1);
   - dither off at 32 bpp (Q17);
   - Z allocation for D16 at 32 bpp (Q5);
   - exact vertex fog (Q15);
   - LOD bias (§4.6);
   - z-bias (Q10).
3. **Capability that some titles probe for:**
   - trilinear, single stage (Q1);
   - mirror (Q6);
   - stencil at 32 bpp (Q4);
   - W-buffer (Q11);
   - dest alpha (Q3);
   - more texture formats including P8/A8P8 (Q7, §4.4);
   - colour write mask at 32 bpp (Q17);
   - two-pass specular (Q16).
4. **Hardware limits to report honestly in the caps:** cube, volume, DOT3, EMBM, PREMODULATE, anisotropy, border colour, MIN/MAX blend, AGP texturing, slope-scaled bias, and the 8:1 aspect and power-of-two limits.

Every hardware behaviour named here should land default-OFF behind a Diag switch, with a d3dprobe check and a native/python test asserting the register value (project rule).
