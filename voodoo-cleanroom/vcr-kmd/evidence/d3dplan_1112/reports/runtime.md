# How the XP SP3 Direct3D runtimes see vcr-kmd's DX7-level HAL, and what it means for the games

Runtime investigation, 2026-11-12. This was a **read-only** pass with no box access. The parallel on-box session's
evidence from the same day (`ONBOX/`) is cited wherever it settles a question.
Claims taken from general DirectX knowledge and not from a file are marked **(inferred)**.

## Path legend

| tag | path |
|---|---|
| `KMD/` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/vcr-kmd` |
| `RA/` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench` |
| `ONBOX/` | `KMD/evidence/d3dplan_1112/onbox` (written today by the session that owns `.124`) |
| `CAPS` | `KMD/evidence/d3dplan_1112/caps_124.txt:1` (d3dprobe caps, D3D32 + D3DBigTex=7 armed) |
| `VH5/` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` (3dfx's NT driver, used as guidance) |
| `R3/` | `/home/voidsstr/development/retro-3dfx` (FINDINGS.md, V56K-SLI-FINDINGS.md, DRIVER-STACK-ASSESSMENT.md) |
| `DDK8/` | `~/retro3dfx-toolchain/devtools/dx8ddk/inc` (the same headers the build copies into `KMD/out/ddi`; `ddrawint.h` md5 5e59ec0e in all three copies) |
| `W2KDDK/` | `~/retro3dfx-toolchain/devtools/w2kddk`: `src/video/displays/3dlabs/driver` is Microsoft's NT Permedia2 D3D sample; `help/d_grafx.chm` holds the W2K DDK graphics docs, extracted with 7z and searched as text |
| `XPRT/` | the XP SP3 runtime DLLs, extracted with 7z from `/mnt/retro-share/Files/OS/XPSP3-FLEET/I386/{D3D8,D3D9,D3DIM,D3DIM700,DDRAW}.DL_` (details below) |
| `LIB/` | `/mnt/retro-share/Files/Games-Library` |

The `XPRT/` files are d3d8.dll 5.3.2600.5512 (md5 f099b129…), d3d9.dll 5.3.2600.5512 (0607cbc6…), d3dim700.dll 5.3.2600.5512, d3dim.dll 5.1.2600.0 and ddraw.dll 5.3.2600.5512.
Addresses are virtual addresses in those files, read with `i686-w64-mingw32-objdump -d`. **.124's own system32 copies were not read.** Check their md5 before relying on an address.

---

## 0. Summary

1. **D3D8 and D3D9 treat vcrdd as a DX7 (DP2EX, non-T&L) driver.** It never sets `DDHALINFO_GETDRIVERINFO2`, and in every ring dump today the runtimes never sent `GUID_GetDriverInfo2`.
   - The runtimes synthesize D3DCAPS8/9, the format list and the depth ops themselves.
   - That path works. D3D8 probes pass 40/40 (`ONBOX/session_log.md:12, 22`), and UT2003, Halo and NewDark all create HAL devices.
   - So the vintage lane's 2026-08-12 conclusion, "D3D8 has no hardware because the driver is DX=7", is wrong in general. 3dfx's shipped NT driver was DX7 too.
2. **The runtime decides render-target textures and depth pairing for a DX7 driver from a table built into d3d8.dll and d3d9.dll.** The table is keyed by PCI id, and we inherit 3dfx's entry `121a:0009 = 0x4580`.
   - We get R5G6B5 and A8R8G8B8 render-target textures, a lockable D16, and no "Z with arbitrary colour depth".
   - X8R8G8B8 render-target textures can never be granted through the DX7 DDI. The registry can only switch flags **off** (`ForceDriverFlagsOff`).
3. **X8R8G8B8 + D16: the runtime says "match", our ContextCreate refuses.** This was confirmed on silicon today.
   - CreateDevice returns `8876086c`.
   - The runtime switched the CRT twice, 156 ms apart, while cleaning up.
   - 3dfx fixed the same case by allocating a 16-bit Z with 32-bit storage. Recommended fix: the same, behind a Diag switch.
4. **The DDPIXELFORMAT→D3DFORMAT converter inside d3d8/d3d9 limits what a DX8/9 game can ever get from a DX7 driver.**
   - Formats we could still add to our DX7 list: X8R8G8B8, X1R5G5B5, X4R4G4B4, L8, A8L8, A4L4, R3G3B2, A8R3G3B2, P8 and A8P8. The VSA-100 TMU samples all of them.
   - **A8 is impossible**: the converter has no `DDPF_ALPHA` case. Bump formats are impossible because the hardware has no bump mapping.
5. **DX5/DX6 execute-buffer and SetRenderState titles send legacy render states straight to our DP2.**
   - The states are TEXTUREMAPBLEND, TEXTUREMAG/MIN, TEXTUREADDRESS(U/V), WRAPU/V, SHADEMODE and COLORKEYENABLE.
   - The HAL handles only TEXTUREHANDLE. JK, MotS and Rainbow Six (DX5) and AvP, Thief Gold, SS2 and Shogo (DX6) therefore draw point-sampled, with the wrong texture/alpha blend and no colour key.
6. **On XP every mip level arrives in its own CreateSurface** (`dwSCnt` = 1), because we do not answer `GUID_NTPrivateDriverCaps`. Our allocator therefore never sees a whole chain, so the TMU's packed-chain assumption holds only if the heap happens to place the levels back to back.
   - Today's UT2003, UT2004 and Halo in-game frames fit this: blocky noise on minified or distant surfaces, clean close up (`ONBOX/session_log.md:17, 19, 21`). That is unproven.
7. **The runtime passes texture ops the caps do not list.**
   - UT2003 uses MODULATE2X on stages 0 and 1 and BLENDCURRENTALPHA on stage 1, measured in the combine log. MODULATE2X is drawn as MODULATE, and both games' worlds are "very dark".
   - `ValidateTextureStageState` always answers "1 pass", so no title's fallback ever triggers.
8. **The runtime emulates several things itself, so the driver needs no work for them.**
   - It emulates state blocks for D3D8/9 (DDI type ≤ 5 → emulate, read in d3d8/d3d9).
   - It falls back to software vertex processing, which UT2003's log confirms.
   - It translates the D3D8 MIPFILTER enumeration; the silicon mip tests pass.
   - The DX7 runtime is the exception: DX7 drivers are expected to record and replay state blocks, and our walker skips them.
9. **A DX8-level DDI would buy little for these games** at a high cost.
   - It would add A8, X8R8G8B8 render-target textures, Caps2 and PresentationIntervals.
   - It would cost driver state blocks, the DX8 DP2 tokens, VB/IB handles, and moving every D3D8/9 title onto new code at once.
   - Recommendation: stay DX7 and fix items 3-7 there.
10. **Desktop versus fullscreen.**
    - Caps and the Z list are read at the desktop mode and re-read at every mode change. Ours are mode-independent and should stay so.
    - Without `Diag\D3D32` (the default), no D3D8/9 device exists at X8R8G8B8, and windowed D3D at `.124`'s 32 bpp desktop is impossible.
    - A leftover Glide `cmdFifo0` silently stops every D3D triangle (on-box finding).

---

## 1. Which runtime each title talks to

The runtime is identified from the import table, plus `strings` for libraries that are loaded at run time and a byte scan for the `IID_IDirect3D*` GUIDs.
The GUID scan shows what is *linked*. For JK, MotS and R6 the highest IID present is IDirect3D2.

| title on `.124` | binary (`LIB/`) | D3D interface | XP runtime DLL | notes |
|---|---|---|---|---|
| Jedi Knight DF2 | `JediKnightDF2/JK.EXE` (imports ddraw.dll) | IDirect3D2 + execute buffers (no IID_IDirect3D3) | d3dim.dll (inferred) | execute-buffer streams reach DP2; the walker had to accept `D3DOP_EXIT` (`KMD/display/vcrdd_d3d.c:41-42, 1773-1782`) |
| Mysteries of the Sith | `JediKnightMotS/JKM.EXE` (strings `DDRAW.dll`, `DirectDrawCreate`) | same as JK | d3dim.dll (inferred) | 7,014 DP2, 342k triangles, 0 unparsed (`RA/docs/v56k-benchmark-plan.md:674`) |
| Rainbow Six | `RainbowSix/RainbowSix.exe` (imports ddraw.dll) | IDirect3D2 + ExecuteBuffer IID | d3dim.dll (inferred) | colour-key code in the exe: strings "colorkey", "Surface::SetTransparentValue" |
| Aliens vs Predator | `AliensVsPredator/avp.exe` (imports ddraw.dll) | IDirect3D3 and ExecuteBuffer IIDs linked | d3dim.dll (inferred) | "DirectDraw 6 object" in its log (`RA/docs/v56k-benchmark-plan.md:221-224`) |
| Thief Gold / System Shock 2 | `ThiefGold/THIEF.EXE` (strings `ddraw.dll`, `DirectDrawCreate`); SS2's `SHOCK2.ICD` is SafeDisc-encrypted, same Dark engine | IDirect3D3 | d3dim.dll (inferred) | |
| Shogo | `Shogo/d3d.ren` (imports ddraw.dll) | IDirect3D3 | d3dim.dll (inferred) | wants paletted textures: "Palette alpha" in `d3d.ren` strings |
| Thief II, DX6 fallback display | `Thief2/Thief2.exe` | IDirect3D3 | d3dim.dll (inferred) | used only without D3D32+BigTex (`LIB/Thief2/Play Thief 2.bat`) |
| Half-Life (D3D option), Descent 3, Turok 2 | `HalfLife1/hw.dll`, `Descent3/main.exe`, `Turok2/Video_D3D.dll` | IDirect3D3 | d3dim.dll (inferred) | alternate renderers; `.124` runs them on OpenGL/Glide |
| Unreal Gold, UT 436, Deus Ex | `*/System/D3DDrv.dll` (strings `DirectDrawCreateEx`) | IDirect3D7 | d3dim700.dll (inferred) | alternate renderer (Glide on `.124`) |
| Tribes 2 | `Tribes2/GameData/OpenGL2D3D.dll` (imports ddraw.dll, `DirectDrawCreateEx`) | IDirect3D7 | d3dim700.dll (inferred) | GL-to-D3D7 wrapper |
| UT2003, UT2004 | `UT200[34]/System/D3DDrv.dll` (imports d3d8.dll) | D3D8 | d3d8.dll | UT2003 log: software T&L, R5G6B5 + D16 (`ONBOX/games/UT2003_flyby/UT2003.log:102-107`) |
| Battlefield 1942 | `BF1942/BF1942.exe` (imports d3d8.dll) | D3D8 | d3d8.dll | single player blocked by SafeDisc |
| Max Payne | `MaxPayne/e2driver/e2_d3d8_driver_mfc.dll` (strings `Direct3DCreate8`) | D3D8 | d3d8.dll | |
| Red Faction | `RedFaction/RF.exe` (strings `d3d8.dll`, `Direct3DCreate8`) | D3D8 | d3d8.dll | |
| Hidden & Dangerous Deluxe | `HiddenAndDangerous/Bin/igraph2.dll` (strings `Direct3DCreate8`) | D3D8 | d3d8.dll | forces 32 bpp (`KMD/README.md:788-789`) |
| Serious Sam TSE (D3D option) | `SeriousSamSecondEncounter/Bin/Engine.dll` (strings `D3D8.DLL`) | D3D8 | d3d8.dll | runs OpenGL on `.124` |
| Halo 1.10 | `Halo/halo.exe` (strings `Direct3DCreate9`, the D3DX9 effect compiler linked in, `DirectDrawCreateEx`) | D3D9 | d3d9.dll | needs `DisableRenderTargets` (`R3/FINDINGS.md:435-439`) |
| Thief II NewDark | `Thief2/Thief2.exe` (`Direct3DCreate9`; `D3DX9_43.dll` alongside) | D3D9 | d3d9.dll | needs D3D32 + BigTex bits 0 and 2 (`R3/FINDINGS.md:467-470`) |
| UT 469e (D3D9Drv), Far Cry | `UnrealTournament/System/D3D9Drv.dll`, `FarCry/Bin32/XRenderD3D9.dll` | D3D9 | d3d9.dll | gated off `.124` (469e: no SSE2; Far Cry: needs sm1.x) |

Which DLL serves which interface is (inferred) from DirectX history, but the files support it:
- `XPRT/ddraw.dll` names both `D3DIM.DLL` and `D3DIM700.DLL` (strings).
- `d3dim.dll` embeds `d:\xpclient\multimedia\directx\dxg\d3d\dx6\d3dim\...` source paths.
- `d3dim700.dll` embeds `...\dxg\d3d\dx7\...`.
- The assumed split: IDirect3D/2/3 go to the DX6 runtime, IDirect3D7 to the DX7 runtime.

---

## 2. What the runtimes ask, and what vcrdd answers

**Registration.**
- `DrvGetDirectDrawInfo` sets only `DDHALINFO_GETDRIVERINFOSET` (`KMD/display/vcrdd_ddraw.c:112-113`). It never sets `DDHALINFO_GETDRIVERINFO2` (0x8, `DDK8/ddrawint.h:906-907`).
- `VcrDdD3dHalInfo` publishes the DX7 global data, i.e. the D3DDEVICEDESC_V1 and texture list, plus ContextCreate/Destroy/DestroyAll (`KMD/display/vcrdd_d3d.c:2049-2113`).
- `VcrDdD3dDriverInfo` answers five GUIDs (`vcrdd_d3d.c:2129-2234`): D3DCallbacks3, D3DExtendedCaps, ZPixelFormats, Miscellaneous2Callbacks and ParseUnknownCommandCallback.
- `DdGetDriverInfo` adds NTCallbacks. Everything else gets `DDERR_CURRENTLYNOTAVAIL` (`vcrdd_ddraw.c:683-706`, default at `:688`).
- This meets the DX7 checklist in the W2K DDK ("Direct3D Driver Initialization": D3DDEVCAPS_DRAWPRIMITIVES2EX, Misc2 callbacks, D3DCallbacks3, ExtendedCaps, ZPixelFormats, ParseUnknownCommand).

**What XP actually asked, today, in one DirectDraw enable** (`ONBOX/ring_after_probe_ii_decoded.txt`, seq 24824-24836):

| GUID | meaning | our answer |
|---|---|---|
| ddf41230 | D3DCallbacks3 | DD_OK |
| 2e04ffa0 | D3DParseUnknownCommandCallback | DD_OK |
| ffaa7540 | KernelCaps | 88760028 (not available) |
| efd60cc2 | ColorControlCallbacks | not available |
| efd60cc0 | MiscellaneousCallbacks | not available |
| 406b2f00 | Miscellaneous2Callbacks | DD_OK |
| 6fe9ecde | NTCallbacks | DD_OK |
| 880baf30 | DDMoreCaps | not available |
| **fad16a23** | **NTPrivateDriverCaps** | **not available** → per-level CreateSurface (§7.1) |
| 80863800 | KernelCallbacks | not available |
| b1122b40 | MotionCompCallbacks | not available |
| **42e02f16** | **GetHeapAlignment** (expected size 0x8c = `DD_GETHEAPALIGNMENTDATA`, `DDK8/dmemmgr.h:67-89`) | **not available** (§7.2) |
| 3b8a0466 | DDMoreSurfaceCaps | not available |

Across the morning's ring dumps (`ONBOX/ring_start_decoded.txt` and the three `ring_after_probe_*_decoded.txt`), D3DExtendedCaps (7de41f80) was queried 29 times and ZPixelFormats (93869880) 25 times. Every other GUID was queried 8 times.
The runtime re-reads caps at every DirectDraw (re)enable, i.e. every mode change (§11).
**`GUID_DDStereoMode` = `GUID_GetDriverInfo2` (f828169c, `DDK8/ddrawint.h:49`, `DDK8/d3dnthal.h:1185-1336`) appears in no ring.**
The D3D8/9 runtimes issue GetDriverInfo2 only to a HAL that sets `DDHALINFO_GETDRIVERINFO2`. 3dfx sets it only when built with DX≥8 (`VH5/DDINIT.C:239-241`). (inferred: the condition; measured: the query never came.)

**So D3D8 and D3D9 build D3DCAPS8/9, the format list and the Z operations themselves from the DX7 data** (§3-5).
That path is proven:
- d3dprobe (D3D8) 40/40 at 1024x768x32 after today's FIFO reset, and at 640x480x16 (`ONBOX/session_log.md:12, 22`)
- UT2003 creates a device (`ONBOX/games/UT2003_flyby/UT2003.log:100-108`)
- Halo and Thief II NewDark run D3D9 (`R3/FINDINGS.md:435-445, 467-470`)

**Correction to the vintage lane.** `R3/V56K-SLI-FINDINGS.md:591-682` (§16) concluded "D3D8 has no hardware because the driver is built DX=7".
- A DX7 DDI does not cause that: ours is DX7-only and gives D3D8/9 a working HAL.
- §16 itself had found the vendor build reporting no video memory (`TotalVRAM=0`, commits `5ebed34` and `8919ac4` in `R3`).
- 3dfx's NT DX8 path "was evidently never compiled" (`R3/V56K-SLI-FINDINGS.md:719-726`), and the project's DX=8 build "has never run" (`R3/DRIVER-STACK-ASSESSMENT.md:204-205`).
- 3dfx's shipped W2K/XP driver was itself a DX7-DDI driver: `VH5/SOURCES` defaults `DX = 7`.

---

## 3. D3DCAPS8/9 as the runtime derives them from our DX7 caps

The measured values come from `CAPS` (D3D8 `GetDeviceCaps(HAL)` on `.124`). Fields d3dprobe does not print are marked "code".

| D3DCAPS8 field | measured | from (our DX7 data) | what the runtime did | consequence for games |
|---|---|---|---|---|
| DevCaps | 0008ae50 = EXECUTESYSTEMMEMORY, TLVERTEXSYSTEMMEMORY, TEXTUREVIDEOMEMORY, DRAWPRIMTLVERTEX, CANRENDERAFTERFLIP, DRAWPRIMITIVES2, DRAWPRIMITIVES2EX, HWRASTERIZATION | `vcrdd_d3d.c:2066-2070` | dropped FLOATTLVERTEX (no D3D8 bit) | no HWTRANSFORMANDLIGHT: HW/MIXED vertex processing refused (inferred); UE2 falls back to software T&L (`UT2003.log:106-107`) |
| PrimitiveMiscCaps | 00000072 = MASKZ, CULLNONE/CW/CCW | `:2017-2018` | none | no COLORWRITEENABLE (the chip has per-channel write enables, `KMD/include/vcr_3dregs.h:203`) and no BLENDOP |
| RasterCaps | 00100191 = DITHER, ZTEST, FOGVERTEX, FOGTABLE, WFOG | `:2019-2020` | dropped SUBPIXEL | no ZBIAS, WBUFFER, MIPMAPLODBIAS, ANISOTROPY; 3dfx advertised ZBIAS and WBUFFER (`VH5/D3INIT.C:2922, 2925`) |
| ShadeCaps | 00004008 = COLORGOURAUDRGB, ALPHAGOURAUDBLEND | `:2032-2033` | dropped the flat-shading caps | no SPECULARGOURAUDRGB or FOGGOURAUD (3dfx: `VH5/D3INIT.C:3004-3011`) |
| TextureCaps | 00004007 = PERSPECTIVE, POW2, ALPHA, **MIPMAP** | `:2034-2035` + ddCaps `DDSCAPS_MIPMAP` `:2110-2111` | **added MIPMAP** (no DX7 bit; inferred to come from DDSCAPS_MIPMAP); TRANSPARENCY has no D3D8 meaning | no NONPOW2CONDITIONAL, CUBEMAP, PROJECTED, ALPHAPALETTE (3dfx: ALPHAPALETTE `VH5/D3INIT.C:3022`) |
| TextureFilterCaps | 03010317 = MIN/MAG POINT+LINEAR, MIPFPOINT, plus raw DX7 legacy bits 0x17 | `:2036-2040` | passed through | no MIPFLINEAR (trilinear); 3dfx advertised it (`VH5/D3INIT.C:3057`) |
| TextureAddressCaps | 00000015 = WRAP, CLAMP, INDEPENDENTUV | `:2044-2045` | none | no MIRROR (3dfx: `VH5/D3INIT.C:3084`), no BORDER |
| TextureOpCaps | 0000184f = DISABLE, SELECTARG1/2, MODULATE, ADD, BLENDDIFFUSEALPHA, BLENDTEXTUREALPHA | `:2165-2168` | none | **apps use unlisted ops anyway (§8.1)**; 3dfx listed MODULATE2X/4X etc. on Napalm (`VH5/D3INIT.C:3262-3290`) |
| SrcBlendCaps / DestBlendCaps | 00001f33 / 0000003f | `:2025-2031` | none | no DESTALPHA/INVDESTALPHA, although `blend_src`/`blend_dst` map them (`vcrdd_d3d.c:636-637, 659-660`); 3dfx listed them on Napalm (`VH5/D3INIT.C:3134-3161`) |
| ZCmpCaps / AlphaCmpCaps | ff / ff | `:2021-2024` | none | OK |
| StencilCaps | code: 0 | `D3DNTHAL_D3DEXTENDEDCAPS` memset, never set (`:2151-2171`) | none | consistent with "stencil not supported", but D24S8 is listed (§5.3); 3dfx: KEEP…DECR on Napalm (`VH5/D3INIT.C:3243-3258`) |
| MaxTextureWidth/Height, AspectRatio, Anisotropy | 2048 (with BigTex bit 0), 8, 1 | `:2156-2160` | none | MaxTextureRepeat 0 (unset) |
| Guard band | code: 0 | computed but unused (`:2152, 2161-2162`) | none | the runtime clips every primitive that crosses the viewport (CPU work, inferred); 3dfx: −200…2048/1536 when enabled (`VH5/D3INIT.C:3223-3233`) |
| FVFCaps | 2 | `:2164` | none | 2 texture-coordinate sets |
| MaxTextureBlendStages / MaxSimultaneousTextures | 2 / 2 | `:2169-2170` | none | matches the two TMUs |
| MaxPrimitiveCount, MaxVertexIndex | 65535 | none | runtime-supplied | |
| MaxStreams, MaxActiveLights | 16, 8 (`UT2003.log:32-35`) | none | runtime-supplied (software pipeline) | **not 0**: D3D8 does not expose this HAL as "legacy" through MaxStreams |
| Vertex/PixelShaderVersion | 0.0 / 0.0 (`CAPS` fffe0000; `UT2003.log:37-39`) | none | runtime | software vertex shaders exist only on SWVP devices (inferred) |

D3D9 uses the same legacy machinery (§4; `XPRT/d3d9.dll` has an identical converter and op builder).

---

## 4. Formats: what a DX8/9 game can and cannot get from a DX7 driver

### 4.1 The runtime's converter (read from the binaries)

`XPRT/d3d8.dll` 0x6d9b6850-0x6d9b6d48 converts each DX7 `DDPIXELFORMAT` to a `D3DFORMAT`. `XPRT/d3d9.dll` 0x4fe059b0 onward has the same cases. The compare constants show:

| `dwFlags` | recognised masks → D3DFORMAT |
|---|---|
| `DDPF_FOURCC` (0x4) | the FOURCC itself (DXT1-5) |
| `DDPF_RGB` (0x40) | 8-bit e0/1c/03 → R3G3B2; 16-bit f800/7e0/1f → R5G6B5, 7c00/3e0/1f → X1R5G5B5, f00/f0/f → X4R4G4B4; 24-bit → R8G8B8; 32-bit ff0000/ff00/ff → X8R8G8B8 |
| `DDPF_RGB` + `DDPF_ALPHAPIXELS` (0x41) | 32-bit → A8R8G8B8; 16-bit ff00+e0/1c/3 → A8R3G3B2, f000+… → A4R4G4B4, 8000+… → A1R5G5B5 |
| `DDPF_PALETTEINDEXED8` + `DDPF_RGB` (0x60/0x61) | 8-bit → P8; 16-bit with alpha ff00 → A8P8 |
| `DDPF_ZBUFFER` (0x400) | 16-bit ffff → **D16_LOCKABLE**; 32-bit ffffffff → D32, 00ffffff → X8D24, ffffff00 → D24X8 |
| `DDPF_ZBUFFER` + `DDPF_STENCILBUFFER` (0x4400) | D24S8, S8D24, D24X4S4, X4S4D24, D15S1, S1D15 |
| `DDPF_LUMINANCE` (0x20000) | 8-bit → L8 |
| `DDPF_LUMINANCE` + `DDPF_ALPHAPIXELS` (0x20001) | 8-bit → A4L4, 16-bit → A8L8 |
| `DDPF_BUMPDUDV` (+`BUMPLUMINANCE`) | V8U8, L6V5U5, X8L8V8U8 |

**There is no `DDPF_ALPHA` (0x2) case. A8 can never reach a D3D8/D3D9 application from a DX7-DDI driver.**
3dfx's own DX7 list carries A8 as `DDPF_ALPHA` (`VH5/D3INIT.C:2144-2148`), which D3D8/9 cannot see either. Only the DX8 path (`DDPF_D3DFORMAT`, `VH5/D3INIT.C:2387-2397`) exposes A8.
The DDI-only Z names (S8D24, X8D24) end up as the API's D24S8/D24X8 (measured: `CAPS` z24x8=1, z24s8=1). Our masks are Z 0x00ffffff and stencil 0xff000000 (`vcrdd_d3d.c:2197-2211`).

### 4.2 The runtime's op builder for legacy drivers

`XPRT/d3d8.dll` 0x6d9af134-0x6d9af464 rewrites each converted entry as a `DDPF_D3DFORMAT` (0x200000, `DDK8/ddrawint.h:102`) entry with `D3DFORMAT_OP_*` flags (`DDK8/ddrawint.h:108-130`):
- **Texture formats:** `OP_TEXTURE`. The CUBE op is added only if the DX7 TextureCaps carry CUBEMAP (0x800). Render-target ops come only from the driver-flag table in §4.3.
- **Z formats** (0x6d9af3ff): `OP_ZSTENCIL` (0x40). `OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH` (0x80) is added only if driver flag 0x10 is set (0x6d9af427-0x6d9af444).
- **16-bit Z:** D16_LOCKABLE is renamed to plain D16 *unless* driver flag 0x80 is set (0x6d9af446-0x6d9af45c).

### 4.3 The runtime's built-in driver table: render-target textures are decided by PCI id

- **Table:** `XPRT/d3d8.dll` VA 0x6d9a1700 (file offset 0xb00). It holds 171 entries of 16 bytes: {vendor<<16 | device, minimum driver version hi, lo, flags}.
- **Lookup:** 0x6d9b0140-0x6d9b0234.
- **Registry override:** only an "off" mask, `HKLM\Software\Microsoft\Direct3D\ForceDriverFlagsOff` (0x6d9b36d6-0x6d9b373f). The "on" mask is forced to 0 at 0x6d9b3707.
- **3dfx entries:**
  - `121a:0003` → 0x190 (file offset 0xc20)
  - `121a:0005` → 0x190 (file offset 0xc30)
  - **`121a:0009` → 0x4580** (file offset 0xc40)
- `XPRT/d3d9.dll` carries the same three entries (file offsets 0x13f0, 0x1400, 0x1410) and the same per-format code (0x4fdfbe19-0x4fdfbe4b).

The bits are read at 0x6d9b5872-0x6d9b5994 and used at 0x6d9af18a-0x6d9af319 and 0x6d9af427-0x6d9af45c:

| flag | effect | 121a:0009 |
|---|---|---|
| 0x100 | R5G6B5 is a render-target texture format | **yes** |
| 0x200 | X8R8G8B8 render-target texture | no |
| 0x400 | A8R8G8B8 render-target texture | **yes** (only while D3DBigTex bit 2 lists A8R8G8B8, `vcrdd_d3d.c:2087-2091`) |
| 0x800 / 0x1000 / 0x2000 | A1R5G5B5 / A4R4G4B4 / X1R5G5B5 render-target texture | no |
| 0x4000 | grant `OFFSCREEN_RENDERTARGET` (instead of `SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET`) | **yes** |
| 0x10 | Z usable with any colour depth | no (Banshee/V3: yes) |
| 0x80 | keep D16 as **D16_LOCKABLE** | **yes** |

vcrdd reports the VSA-100's PCI id (`UT2003.log:50-51`: dwVendorId 4634, dwDeviceId 9), so **the runtime applies 3dfx's entry to our open driver.**

### 4.4 What a DX8/9 game can get today, and what it would take

"TMU" refers to the TMU format field in `VH5/H3DEFS.H:263-277`.

| format / usage | offered now? | exposable through the DX7 DDI? | VSA-100 | who wants it |
|---|---|---|---|---|
| R5G6B5, A1R5G5B5, A4R4G4B4 | yes (`vcrdd_d3d.c:2081-2084`) | — | TMU 10/11/12 | all |
| A8R8G8B8 | with D3DBigTex bit 2 | — | TMU 15 | UT2003/2004, Halo, NewDark |
| DXT1-5 (incl. DXT2/4) | with D3DBigTex bit 1 (`:2092-2098`) | — | compressed formats | UT2003/2004 fonts and world, Halo (DXT2/4 measured: `R3/FINDINGS.md:440-441`) |
| **X8R8G8B8 texture** | **no** | yes: a 32-bit `DDPF_RGB` entry, sampled as ARGB8888 with alpha forced to 1 | TMU 15 | Halo bitmap format `x8r8g8b8` (halo.exe strings); usage inferred |
| X1R5G5B5, X4R4G4B4 | no | yes (16-bit `DDPF_RGB`; sample as 1555/4444 with alpha forced to 1) | TMU 11/12 | unknown |
| R3G3B2, A8R3G3B2 | no | yes | TMU 0 / 8 | unknown |
| L8, A8L8, A4L4 | no | yes (`DDPF_LUMINANCE` entries) | TMU 3 (I8) / 13 (AI88) / 4 (AI44) | Halo `y8`, `a8y8`, `ay8` (inferred mapping) |
| P8, A8P8 | **no** (`CAPS` texP8=0) | yes, plus DP2 `SETPALETTE`/`UPDATEPALETTE` handling (now skipped by size, `vcrdd_d3d.c:1557, 1716-1724`) and a palette download | TMU 5/6/14 | Shogo ("Palette alpha"), Halo `p8-bump` (inferred) |
| **A8** | no | **never** (no converter case, §4.1) | TMU 2 | Halo `a8` (inferred) |
| V8U8 and other bump formats | no | never (no bump hardware) | none | none on the list |
| D16 (D16_LOCKABLE) | yes | — | 16-bit aux | all |
| D24X8, D24S8 | with D3D32 | — | 24+8 aux at 32 bpp | 32 bpp titles |
| D15S1, D24X4S4, D32, X4S4D24 | no | never (no 1- or 4-bit stencil, no 32-bit Z) | none | none |
| R5G6B5 / A8R8G8B8 render-target texture | granted by the table (§4.3) | — | yes (`target_of` accepts any video-memory 16/32 bpp surface, `vcrdd_d3d.c:1047-1088`) | Halo (§4.5) |
| **X8R8G8B8 render-target texture** | **no** | **never** (no 0x200; no "on" override) | yes | possibly Halo (unmeasured) |
| `D3DUSAGE_DYNAMIC` textures | unmeasured | `D3DCAPS2_DYNAMICTEXTURES` is a DX8-driver cap (inferred) | — | Halo Bink path, UE2 (inferred) |

### 4.5 Halo's render-target failure: what is known

- Halo stopped with its FATAL dialog 105 ("render-target creation failing") until `DisableRenderTargets` was added for `0x121a:0x0009` (`R3/FINDINGS.md:435-439`; `LIB/Halo/config.txt:115-118`).
- In today's Halo timedemo "a glow sprite around a blue crystal [is] drawn as a visible translucent SQUARE" (`ONBOX/session_log.md:21`). That fits an alpha-only or luminance texture (`a8`/`ay8`/`y8` in halo.exe's format names) that the D3D9 runtime cannot create on this driver (§4.1, §4.4). Inferred; Halo's texture-creation failures are not logged anywhere.
- From §4.3, the runtime *does* grant R5G6B5 and A8R8G8B8 render-target textures on this PCI id. The failure is therefore one of these:
  - an **X8R8G8B8** render-target texture, which is never granted;
  - a **non-power-of-2** render-target size (TextureCaps has POW2 without NONPOW2CONDITIONAL; Halo's `config.txt` lists `LinearTextureAddressing` "rendering to non-square textures");
  - an alpha format at a moment when A8R8G8B8 was not listed.
- Which one is unmeasured. `DisableRenderTargets` costs Halo every render-to-texture effect (inferred).

---

## 5. Depth/stencil pairing

### 5.1 X8R8G8B8 + D16: CONFIRMED broken today

- **Runtime:** `CheckDepthStencilMatch(X8R8G8B8, X8R8G8B8, D16)` = yes (`CAPS` zmatch). D16 carries only `OP_ZSTENCIL` (§4.2), so the runtime's own matching rule allows a 16-bit Z with a 32-bit target. That rule itself is (inferred); its outcome is measured.
- **Silicon, 10:01:28 today:**
  - `d3dprobe render --full 640x480x32 --zfmt d16` → `CreateDevice 8876086c`, D3DERR_INVALIDCALL (`ONBOX/d3dprobe_render_640x480x32_d16.txt`; `ONBOX/session_log.md:6`).
  - Ring: `ContextCreate refused: 32 bpp target with a 16-bit Z on a VSA-100` (`ONBOX/ring_after_probe_i_decoded.txt:85`).
- **Code path:**
  - The Z surface is created without objection. `Dd_CanCreateSurface` refuses non-16-bit Z only while rt32 is off (`vcrdd_ddraw.c:618-628`), and `Dd_CreateSurface` leaves Z allocation to DirectDraw (`:633-645`).
  - ContextCreate then calls `target_of` → `vcr_rt_format(rt32=1, 32, 16)` → `VCR_RT_REFUSED` (`KMD/include/vcr_rtfmt.h:84-85`) → `DDERR_INVALIDPIXELFORMAT` (`vcrdd_d3d.c:1189-1196`).
- **Side effect:** the runtime had already set 640x480x32 at 83009.862 s and restored 1280x1024x32 at 83010.002 s (`ring_after_probe_i_decoded.txt:36, 91-97`). That is two CRT mode switches 140-156 ms apart, made by the runtime's own cleanup (§11).

**Which CreateDevice paths fail:**
- (a) Any fullscreen D3D8/9 device at X8R8G8B8/A8R8G8B8 with an auto depth-stencil of D16.
- (b) Any **windowed** D3D8/9 device on `.124`'s 32 bpp desktop with D16. The windowed back buffer must be the desktop format, which is how d3dprobe builds it (`KMD/tools/d3dprobe.c:1423`).
- (c) A D16 surface bound later with `SetRenderTarget` to a 32 bpp target. SETRENDERTARGET refuses and stops drawing, but CreateDevice already succeeded, so this one fails silently (`vcrdd_d3d.c:1733-1749`).
- (d) DX6/DX7 apps that attach a 16-bit Z to a 32 bpp back buffer.

No deployed title is *measured* picking D16 at 32 bpp:
- UE2 picks 16 bpp + D16 on this card (`UT2003.log:102-103`).
- H&D, Halo, NewDark, Max Payne and Red Faction create devices.
- The hazard covers every D3D8/9 title whose Z choice is D16 at 32 bpp.

**What 3dfx did.**
- At CreateSurface, a 16-bit Z requested while the primary is 32 bpp is allocated at 32 bits a pixel and its format is rewritten to 32/0x00ffffff (`VH5/DDSURF.C:592-626`).
- At attach, a 16/32 mismatch either re-allocates the Z as 32-bit or uses it as 16-bit (`VH5/DDSURF.C:2237-2345`).
- ContextCreate dropped its mismatch error on 2000-04-06 (`VH5/D3CONTXT.C:34-38`, revision 12).
- Its DX8 list then honestly publishes D16 with `ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH` (`VH5/D3INIT.C:2553`).

**Recommended fix** (new Diag switch, default off):
1. In `Dd_CreateSurface`, give a 16-bit `DDSCAPS_ZBUFFER` 32-bit-a-pixel storage when `pd->rt32` is set and `pd->bpp == 32`. For fullscreen, the runtime sets the mode before it creates the Z (`KMD/tools/d3dprobe.c:1471-1474` comment; ring order above).
2. Mark the surface, e.g. a `dwReserved1` flag beside `SF_MAGIC` (`vcrdd_d3d.c:204-212`).
3. Make `surf_zbits` (`vcrdd_d3d.c:185-194`) return 32 for marked surfaces. The existing 32/32 rule then draws it; a 16 bpp target over it already draws 16/16-in-32 (`vcr_rtfmt.h:70-83`).
4. Keep D16 in the list. The runtime grants the pair either way.

**Tests:**
- Native: a helper that decides the Z storage, asserting 16→32 with rt32 at a 32 bpp PDEV (old behaviour: refused at ContextCreate) and 16→16 otherwise.
- Python: the switch gates it.
- On box:
  - `d3dprobe render --full --bpp 32 --zfmt d16` → 40/40
  - windowed `--zfmt d16` on the 32 bpp desktop
  - no 513/12 in the ring.

### 5.2 R5G6B5 + D24X8/D24S8: correct as is

- **D3D8:** the runtime refuses the pair itself: `CAPS` zmatch R5G6B5/D24X8 match=0, and CreateDevice returns `8876086a` before the driver is asked (`RA/docs/v56k-benchmark-plan.md:172-174`; `R3/FINDINGS.md:50-51`).
- **DX5/6/7:** these apps never ask the runtime. Rainbow Six reaches the driver and draws 16/16 in the 32-bit Z's rows (be58b7d; `vcr_rtfmt.h:70-83`).
- No action. Keep the tests that pin both answers.

### 5.3 D24S8: listed, but its stencil is never honoured

- **Listed:** D24S8 is in the Z list (`vcrdd_d3d.c:2204-2212`) and reaches applications as D3DFMT_D24S8 (`CAPS` z24s8=1, match with X8R8G8B8 = 1).
- **Caps:** D3D8 StencilCaps is 0 (code, §3).
- **HAL:**
  - It writes stencilMode = stencilOp = 0 at every 32 bpp setup and drops `D3DCLEAR_STENCIL` (`vcr_rtfmt.h:33-52`).
  - `compute_regs` reads no stencil state (`vcrdd_d3d.c:921-1029`).
- **Effect:**
  - Apps that check StencilCaps see no stencil.
  - Apps that only check the format get a silent no-op stencil test, which is wrong only for masking uses such as mirrors and portals.
- **Hardware:** the VSA-100 does 8-bit stencil at 32 bpp (`KMD/include/vcr_3dregs.h:37-38`; 3dfx caps `VH5/D3INIT.C:3243-3258`).
- **Titles:**
  - UT2003/2004 are staged with `UseStencil=False` (`LIB/UT2004/System/UT2004.ini`, `[D3DDrv.D3DRenderDevice]`).
  - The rest are unknown. Count `D3DRENDERSTATE_STENCILENABLE` per context before investing.

---

## 6. DX5/DX6-era paths: execute buffers and legacy render states

### 6.1 Execute buffers reach DP2 as they are

- The DX7 DDI flags it: `D3DNTHALDP2_EXECUTEBUFFER`, "the command buffer and vertex buffer are a system memory execute buffer resulting from the use of the Execute buffer API" (`DDK8/d3dnthal.h:863-865`).
- JK's streams arrive ending in `D3DOP_EXIT` (`vcrdd_d3d.c:1773-1782`).
- MotS ran 7,014 DP2 with 0 unparsed (`RA/docs/v56k-benchmark-plan.md:674`). So the runtime strips PROCESSVERTICES and the matrix/light ops and passes the rest as written:
  - POINT=1, LINE=2 and TRIANGLE=3; TRIANGLE uses the `D3DTRIANGLE` record, which equals `INDEXEDTRIANGLELIST` (`vcrdd_d3d.c:1643-1648`);
  - **STATERENDER=8 = RENDERSTATE** (`:1585-1590`), carrying the application's raw `D3DRENDERSTATE_*` values.

### 6.2 The legacy states arrive raw, and a NT DP2 driver must map them

- **Microsoft's NT DDK sample** mirrors TEXTUREHANDLE, TEXTUREMAPBLEND (all eight modes), BORDERCOLOR, MIPMAPLODBIAS, ANISOTROPY, TEXTUREADDRESS(U/V), TEXTUREMAG and TEXTUREMIN into TSS stage 0. It does so for *every* render state it receives (`W2KDDK/src/video/displays/3dlabs/driver/d3dstate.c:583-740`, called unconditionally at `:1836`).
- **3dfx** removed TEXTUREMAPBLEND for DX7 on Win9x only. The revision note reads "still used for dx6- and still used for WINNT" (`VH5/D6MT.C:150-154`), and the removal guard ends `&& !defined(WINNT)` (`VH5/D3RSTATE.C:2952`). It maps all eight blend modes to TSS0, using a private "legacy MODULATE" marker (`VH5/D3RSTATE.C:2860-2945`).
- **The W2K DDK docs** (D3DTEXTUREBLEND page) describe the MODULATE case as needing a private alpha op "assigned … when the driver receives a D3DRENDERSTATE_TEXTUREMAPBLEND of D3DTBLEND_MODULATE".
- **Conflict:** the same docs call TEXTUREADDRESS/MAG/MIN/MIPMAPLODBIAS/ANISOTROPY/BORDERCOLOR "obsolete … only used on Windows 95/98 when supporting legacy run-times".
- **What is settled:**
  - Execute-buffer titles (JK, MotS, R6) send them, per §6.1.
  - Whether d3dim.dll also passes them for `IDirect3DDevice2/3::SetRenderState` callers is (inferred) yes, by the sample and by 3dfx. Count them on the box (§13) before trusting either.

### 6.3 What our HAL does with them

- `set_rs` stores every state below 256 and treats only `TEXTUREHANDLE` specially (`vcrdd_d3d.c:1393-1406`).
- `compute_regs` reads only these states: CULLMODE, DITHERENABLE, ZENABLE/ZFUNC/ZWRITEENABLE, ALPHATEST*, ALPHABLEND/SRCBLEND/DESTBLEND, the FOG* group and TEXTUREFACTOR (`:921-1029`). It also reads TSS0/1 ops and arguments, filters, ADDRESSU/V, MIPFILTER and TEXCOORDINDEX (`:676-874, 1338-1356`).
- Consequences:
  - **TEXTUREMAPBLEND is ignored.** TSS stay at the defaults: MODULATE, with alpha taken from the texture (`ctx_defaults :1138-1143`). The effects:
    - MODULATEALPHA loses vertex-alpha translucency.
    - DECAL/COPY come out modulated by the diffuse colour.
    - DECALALPHA and ADD are wrong.
    - Legacy MODULATE on a texture without alpha takes alpha = 1 instead of the diffuse alpha (the DDK's LEGACY_ALPHAOVERRIDE case).
  - **TEXTUREMAG/TEXTUREMIN are ignored.** Everything is point-sampled whatever the game's bilinear/mipmap setting: TSS default POINT (`:1145-1146`), MIPFILTER 0.
  - **TEXTUREADDRESS/U/V and WRAPU/V are ignored**, so everything wraps (texture seams).
  - **SHADEMODE FLAT is ignored**; everything is Gouraud.

### 6.4 Colour key (COLORKEYENABLE)

- **DDK:** "Color-key is enabled with the AND of the render state enable and the DDRAWISURF_HASCKEYSRCBLT bit in the surface … It is up to the driver to correctly handle this behavior" (d_grafx.chm, D3DRENDERSTATE_COLORKEYENABLE).
- **We advertise** `D3DPTEXTURECAPS_TRANSPARENCY` (`vcrdd_d3d.c:2035`) but never set `FZ_CHROMAKEY`/`chromaKey` (`KMD/include/vcr_3dregs.h:32, 107`). Key-coloured texels draw opaque.
- **3dfx:**
  - It implements colour keys with the FBI chroma key: the key is converted to xRGB888 per texture format and fbzMode `SST_ENCHROMAKEY` is set (`VH5/D6DP2.C:6815-6957`).
  - It notes the chip keys *after* the bilinear filter (`:6820-6822`), so the result is approximate.
  - It once dropped the cap because "Napalm doesn't support texture chroma key" and restored it for WHQL (`VH5/D3INIT.C:99-100, 143-145`).
  - Hardware: partial.
- **Known user:** Rainbow Six (colour-key strings in the exe).

### 6.5 `D3DTSS_ADDRESS` (TSS 12)

- Stored but never applied: `tex_view` and `tex_view_ext` read only ADDRESSU/V (`vcrdd_d3d.c:792-795, 830-833`).
- Both the Microsoft sample (`d3dstate.c:62-66`) and 3dfx (`VH5/D6DP2.C:1490-1494`) copy it into U and V.
- Users are unknown. Count it.

### 6.6 Who is affected

- **DX5, execute buffers:** JK, MotS, Rainbow Six.
- **DX6:** AvP, Thief Gold, System Shock 2, Shogo, Thief II's DX6 fallback.
- **Alternate renderers:** Half-Life D3D, Descent 3, Turok 2.
- **Not affected** (inferred: the states were removed from the DX7 and later APIs):
  - DX7 titles: UE1 D3DDrv, Tribes 2
  - every D3D8/9 title

---

## 7. Runtime surface-creation behaviour that bites the HAL

### 7.1 Each mip level is a separate CreateSurface, and a chain is not one block

- **DDK:** "dwSCnt … is usually 1 on Windows 2000. However, if you support … GUID_NTPrivateDriverCaps, and the driver sets DDHAL_PRIVATECAP_ATOMICSURFACECREATION, the member will contain the actual number of surfaces" (d_grafx.chm DD_CREATESURFACEDATA; `DDK8/ddrawint.h:1022-1035`).
- **XP asked** NTPrivateDriverCaps and was refused (§2).
- **Measured in this repo:** "seven calls for a 64x64 D3D8 texture, DDSD_MIPMAPCOUNT 7 on each" (`RA/tests/python/test_vcr_kmd_d3d.py:106-114`; commit e74510a).
- **Our allocators** size and place a block from the levels in *that one call*: `VcrDdD3dCreateMipChain` (`vcrdd_d3d.c:550-583`) and `mipchain_ext` (`:423-460`). Each call's single surface is marked `MIP_TOP` (`:457, 575`).
- **The TMU** then reads level k at base + chain offset(k), over the attach-list length (`tex_view_ext :814-824`). That address is correct only if DirectDraw's heap returned the separately allocated levels back to back.
- **Fresh heap:** it does. `d3dprobe mip` and `mip2048` pass on silicon at 1024x768x32 (`ONBOX/session_log.md:13`).
- **In game:**
  - UT2004 DM-Antalus and the UT2003 flyby show blocky multicoloured noise exactly on minified surfaces (distant terrain, rock silhouettes), while the same materials close up are clean (`ONBOX/session_log.md:17, 19`; `ONBOX/games/UT2004_botmatch/UT2004_botmatch_090s*.png`; `ONBOX/games/UT2003_flyby/UT2003_flyby_090s.png`).
  - Halo's timedemo shows the same "blocky multi-coloured noise patches on some rock faces" (`ONBOX/session_log.md:21`), so the symptom is not UE2-specific.
  - This fits lower levels being read from memory that holds something else.
  - **Unproven.** The DXT small-level layout (`vcr_tex_chain_usable`) and texture-cache flushing are the competing candidates.
- **3dfx's approach:**
  - It reserves the whole chain on the top level's call from `lpSurfMore->dwMipMapCount`, then fills later levels' addresses from it (`VH5/D3TXTR.C:1635-1636, 1750-1785`; the comment calls per-level calls "a completely bogus operation" seen on Win9x).
  - It asks for atomic creation where its own memory manager is in use (`VH5/DDFXNT.C:1224-1248`).
- **Fix options** (Diag switch, default off):
  - (a) Answer `GUID_NTPrivateDriverCaps` with `DDHAL_PRIVATECAP_ATOMICSURFACECREATION`. The existing loops then see the whole chain. This also changes flip-chain creation for every application.
  - (b) Contained to textures: reserve the chain on the top call (dwMipMapCount) and satisfy the per-level calls from the reservation.
- **Test:**
  - Native: unit-test the reservation bookkeeping.
  - On box: log event 20 at INFO with `dwSCnt` and each level's `fpVidMem`.
  - New d3dprobe test: fragment the heap (create and free textures of mixed sizes), then build a 565 chain and a DXT1 chain and check levels 1-4.

### 7.2 GetHeapAlignment is asked for and declined

- **Query:** XP queries `GUID_GetHeapAlignment` (42e02f16, 0x8c bytes) at every enable (§2). We decline it.
- **Result:**
  - DirectDraw places surfaces on 4-byte boundaries (`vcrdd_d3d.c:381-388`: UT2003 font pages at 0x91d6b4).
  - `mipchain_ext` places every texture itself, but only on the D3DBigTex path (`:532-533`). On the default path, single-level textures and render-target textures (`:389-391`) keep DirectDraw's placement.
  - 3D targets and Z buffers get no 16-byte pitch alignment, so a windowed client whose width is not a multiple of 8 px at 16 bpp fails CreateDevice (`vcr_rtfmt.h:105-123`).
- **Fix:** answer it. Set `Texture`, `ZBuffer`, `FlipTarget` and `Offscreen` to `dwStartAlignment=16`, `dwPitchAlignment=16` (`DDK8/dmemmgr.h:41-89`). That XP honours the answer for the heap it queried is (inferred); the existing placement log (event 513 what 21-23) verifies it.

### 7.3 Managed-texture uploads serialise the chip

- D3D8 on this HAL fills managed textures by Lock + CPU writes, not TEXBLT (`R3/FINDINGS.md:1214-1216`).
- Every `Dd_Lock` drains the engine with `VcrDd2dSync` (`vcrdd_ddraw.c:647-660`).
- Every texture `Unlock` forces a TMU flush before the next use (`:676-681` → `vcrdd_d3d.c:1287-1320`).
- Per-frame uploads (movie textures, dynamic lightmaps) therefore stall the pipeline.
- Halo's Bink intros run about 10x slow (`R3/FINDINGS.md:446-450`), which is consistent with this (inferred, unmeasured).
- Lighter fix: drain only when the locked surface is one the engine may still be reading (a per-surface busy flag).

---

## 8. What the runtime does not validate

### 8.1 Texture ops the caps do not list arrive anyway

- UT2003 sent COLOROP **MODULATE2X (5)** on stage 0 (TEXTURE × DIFFUSE) and on stage 1, and **BLENDCURRENTALPHA (16)** on stage 1 (`KMD/evidence/d3dtune_1014/sweep_1108/UT2003_combos_unaligned.txt:2, 8, 60`). TextureOpCaps 0x184f lists neither.
- The HAL draws MODULATE2X/4X as MODULATE (`color_path :714-716`; `stage1_combine :862`), so lightmapped and lit surfaces come out at half brightness.
- Both games' worlds are "very dark" in today's frames (`ONBOX/session_log.md:17, 19`).
- Napalm does 2x/4x: 3dfx lists MODULATE2X/4X on Napalm (`VH5/D3INIT.C:3262-3290`), done through the combine extension's output shift (inferred route: `combineMode`, `KMD/include/vcr_3dregs.h:46`).

### 8.2 `ValidateTextureStageState` always passes

- It answers 1 pass, DD_OK, whatever the set-up (`vcrdd_d3d.c:1905-1910`).
- The D3D7/8/9 `ValidateDevice` calls reach this callback (inferred). Titles that probe a multi-stage set-up and fall back on failure therefore never fall back, and then degrade silently.
- 3dfx validated op by op and argument by argument (`VH5/D6MT.C:3175-3320`; revision notes `:60-127`).
- Make it answer exactly what `compute_regs` implements, together with §8.1.

### 8.3 Lines and points

- `dpcLineCaps` is a copy of the triangle caps (`vcrdd_d3d.c:2071-2072`), so the runtime believes lines draw with texture, blend and Z.
- D3D8 LineCaps derive from these (inferred; not dumped).
- The walker skips every line and point opcode (`:1690-1715`).
- Fix: draw them as triangle quads, or at least count them per context so the titles that use them are known.

### 8.4 Smaller state gaps the runtime will happily send

- **ZENABLE:** depth is enabled only for `D3DZB_TRUE` (`vcrdd_d3d.c:986`), so `D3DZB_USEW` (2) turns depth **off**. The hardware has a W-buffer (`FZ_WBUFFER`, `vcr_3dregs.h:108`; 3dfx: `VH5/D3INIT.C:2925`).
- **ZBIAS** (47) is ignored, although `FZ_ZBIAS` exists (`vcr_3dregs.h:115`). Decals z-fight (inferred).
- **SPECULARENABLE** is ignored. ShadeCaps carries no SPECULARGOURAUDRGB, so caps-checking apps skip specular anyway. Halo has a `DisableSpecular` option (`LIB/Halo/config.txt:29`).

---

## 9. What the runtime does for a DX7 driver (no driver work needed)

- **State blocks.**
  - d3d8.dll device init sets "emulate state blocks" when the device's DDI type is ≤ 5, and reads `HKLM\Software\Microsoft\Direct3D\EmulateStateBlocks` only above that (`XPRT/d3d8.dll` 0x6d9e8c05-0x6d9e8c32). d3d9.dll does the same (0x4fe6fc25-0x4fe6fc5a).
  - The same functions test 5/7/9/11 as the T&L DDI types (0x4fe6fc05-0x4fe6fc17), which puts a non-T&L DX7 driver at ≤ 5 (inferred mapping).
  - **Result: D3D8/9 never send `D3DDP2OP_STATESET` to vcrdd.**
  - **The DX7 runtime is different.** DX7 drivers are expected to record and replay state blocks: d_grafx.chm "Accelerated State Management"; the Microsoft sample's `D3D_STATEBLOCKS 1` (`W2KDDK/.../3dlabs/driver/d3dhw.h:59-62`); 3dfx `VH5/D7DP2.C:1368-1480`.
  - Our walker skips STATESET records by size (`vcrdd_d3d.c:1562`) and applies recorded states at once. DX7-interface titles that use state blocks (UE1 D3DDrv? Tribes 2's wrapper?) would mis-render. Count them.
- **Vertex processing.** HW/MIXED vertex processing is refused without HWTRANSFORMANDLIGHT (inferred). UE2 says "will use software vertex processing" (`UT2003.log:106-107`), and every D3D8/9 title in the sweeps made a device (`RA/docs/v56k-benchmark-plan.md:99-181`). **No deployed title insists on hardware vertex processing.**
- **MIPFILTER enumeration.** D3D8's `D3DTEXF_POINT` (1) reaches us as DX7's `D3DTFP_POINT` (2). Proof: the HAL tests `> D3DTFP_NONE` (`vcrdd_d3d.c:779, 814`), d3dprobe sets `D3DTEXF_POINT` (`d3dprobe.c:851, 1025`), and the mip tests pass on silicon.
- **Point sprites.** d3d8/d3d9 have an `EmulatePointSprites` knob (d3d8 0x6d9ca4e3). Size-1 points still arrive as DP2 POINTS and are skipped (§8.3) (inferred).

---

## 10. Would a DX8-level DDI gain anything?

What changes: we would set `DDHALINFO_GETDRIVERINFO2` and answer GETD3DCAPS8, GETFORMATCOUNT/GETFORMAT and DXVERSION (`DDK8/d3dnthal.h:1185-1440`; 3dfx `VH5/DDFXNT.C:1068-1158`, `VH5/D3INIT.C:2253-2570, 3358-3700`). The runtime then skips the legacy builder and the PCI table (`XPRT/d3d8.dll` 0x6d9b5872: `cmpl $0x8` gate) and takes our list.

**Concrete gains for these games:**
1. **A8 textures**, which are impossible otherwise (§4.1). Halo `a8` (inferred).
2. **Per-format render-target ops:** X8R8G8B8 render-target textures, and our own choice of `ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH`. Halo's render targets, *if* §4.5 resolves to X8R8G8B8.
3. Caps the DX7 path cannot state:
   - Caps2: DYNAMICTEXTURES, FULLSCREENGAMMA, CANRENDERWINDOWED
   - PresentationIntervals (INTERVAL_TWO etc.) and MaxPointSize
   - a D3DCAPS8 that no table overrides
4. **DXVERSION per process** ("for DX7 applications and DX8 applications", `VH5/DDFXNT.C:1087-1101`). Legacy-state handling could be scoped to old runtimes.

**Not a gain:**
- HAL availability (already works).
- Speed: vertex processing stays on the CPU; only the transport changes.
- Everything in §5-§8, all of which can be done in the DX7 DDI.

**Costs:**
- **State blocks.** Above DDI type 5 the runtime stops emulating them (§9). We would implement STATESET record/execute/capture (3dfx: `VH5/D7DP2.C:1368-1700`), or set `EmulateStateBlocks=1` on the box.
- **DX8 DP2 tokens.** With MaxStreams ≥ 1 the driver receives SETSTREAMSOURCE(UM), SETINDICES, DRAWPRIMITIVE(2), DRAWINDEXEDPRIMITIVE(2), CLIPPEDTRIANGLEFAN, SETVERTEXSHADER (FVF), BUFFERBLT, VOLUMEBLT, ADDDIRTYRECT/BOX and MULTIPLYTRANSFORM (`DDK8/d3dnthal.h:553-576`), with vertex and index buffers as handle surfaces (3dfx `VH5/D8DP2.C:96-300`).
  - Whether MaxStreams=0 keeps the DX7 token stream is unverified (inferred).
  - 3dfx sets 1, commented "Indicates a DX 8 driver" (`VH5/D3INIT.C:3633-3635`).
- **New D3D9 refusal paths.** d3d9 converts vertex declarations for a "dx8 driver" and refuses some (strings in `XPRT/d3d9.dll`).
- **No proven reference.** 3dfx's NT DX8 path never ran (`R3/DRIVER-STACK-ASSESSMENT.md:204-205`).
- **Blast radius.** Every D3D8/9 title moves to new code at once: UT2003/2004, BF1942, Max Payne, Red Faction, H&D, Halo, NewDark.

**Recommendation:** stay on the DX7 DDI.
- Do §5-§8 there.
- Revisit a DX8 DDI only if measurement shows Halo needs X8R8G8B8 render targets or A8 textures.
- If so, make it a separate default-off switch:
  - turn `EmulateStateBlocks=1` on first;
  - try the MaxStreams=0 hybrid with `d3dprobe caps` before any game.

---

## 11. Desktop 32 bpp versus the fullscreen mode

- **Caps are read at the desktop mode and re-read at every mode change** (GetDriverInfo counts, §2).
  - D3D8 answers CheckDeviceFormat and CheckDepthStencilMatch for a fullscreen format without switching modes; `d3dprobe caps` relies on exactly that (`d3dprobe.c:1336-1395`).
  - vcrdd keeps every answer mode-independent: render depths are DDBD_16, plus DDBD_32 with D3D32, whatever the desktop (`vcrdd_d3d.c:2073-2079`); the Z list is always D16, plus D24X8/D24S8 (`vcr_rtfmt.h:179-198`).
  - The 4a9793b variant took D16 away at the 32 bpp desktop and broke the proven 16 bpp device. **Any new format or Z entry must not depend on `pd->bpp`.**
- **DX6 titles enumerate Z at the desktop depth, then switch.** Rainbow Six took D24 and switched to 16 bpp (`R3/FINDINGS.md:45-51`). Keep 16/16-in-32.
- **Without `Diag\D3D32` (the default):**
  - D3D8/9 `CheckDeviceType(HAL, X8R8G8B8)` fails. `CAPS` hal_fullscreen X8R8G8B8 = 1 only because it is armed (`KMD/README.md:459-478`).
  - Windowed D3D8/9 on `.124`'s 32 bpp desktop cannot create a device (`RA/docs/v56k-benchmark-plan.md:1120-1121`).
  - H&D Deluxe fails ("Unable to initialize graphics", `KMD/README.md:788-789`).
  - NewDark's D3D9 display needs it (`R3/FINDINGS.md:467-470`).
  - A D3D7 `EnumDevices` at the 32 bpp desktop shows no 32-bit render depth (inferred).
  - D3D32 should become the default once the 32 bpp path is clean. It passed 40/40 after the FIFO reset today (`ONBOX/session_log.md:12`).
- **A failed CreateDevice after the runtime has set the mode costs two mode switches about 150 ms apart** (§5.1).
  - Every refusal at ContextCreate or SETRENDERTARGET is a possible flapping source on the CRT; the project rule is ≥ 5 s between switches.
  - Fix §5.1 so it stops happening. The refusals are already logged (513/12, 15, 17).
- **A killed Glide/OpenGL client's `cmdFifo0`, left enabled, silently stops every D3D triangle** while clears still work (`ONBOX/session_log.md:7-12`; `ONBOX/regs/regs_after_windowed_1009.txt`). That is the on-box session's finding. Reset the CMDFIFO state at D3D context creation.
- **UE2 picks 16 bpp + D16 on this card** although `Use16bit=False` (`UT2003.log:100-103`; `ONBOX/games/ut2003/UT2003.ini_before:259`), right after "D3D Detected: 3dfx video card" (`UT2003.log:54`). UE2 keys on vendor id 0x121a (inferred), so expect UT2003/2004 to stay 16 bpp whatever D3D32 says.

---

## 12. Plan: runtime-facing fixes in priority order

Every item lands default-off behind its own Diag switch, one change per clean boot, with a host test that asserts both the old and the new value plus a d3dprobe check.

| # | fix | why (evidence) | host test | on-box check | titles |
|---|---|---|---|---|---|
| P0 | clear a stale `cmdFifo0` and 3D state at D3D context creation (owned by the on-box investigation) | `ONBOX/session_log.md:7-12` | — | render 40/40 straight after a killed Glide session | every D3D title after any Glide/OpenGL use |
| P1 | MODULATE2X/4X (and BLENDCURRENTALPHA, ALPHAREPLICATE) through the combine output shift; `ValidateTextureStageState` honest | §8.1, §8.2 | native combine table (2X shift set; the old MODULATE value pinned as the bug) | new d3dprobe `mod2x`/`mod4x`; `ValidateDevice` expected to fail for an unsupported op | UT2003, UT2004 (dark world) |
| P2 | a mip chain as one block despite per-level CreateSurface (dwMipMapCount reservation, or ATOMICSURFACECREATION) | §7.1 | native reservation bookkeeping | fragmented-heap chain test (565 + DXT1); log `dwSCnt` and level addresses | all mipmapped titles; UT2003/2004 distance noise |
| P3 | D16 at a 32 bpp target as 32-bit aux storage | §5.1 | native Z-storage rule (old: refused) | `--bpp 32 --zfmt d16` full and windowed, 40/40, no 513/12 | any D3D8/9 at 32 bpp with D16 |
| P4 | legacy states → TSS0: TEXTUREMAPBLEND (with the legacy MODULATE alpha), MAG/MIN (+mip), ADDRESS/U/V, WRAP0/U/V, SHADEMODE flat; `D3DTSS_ADDRESS`; COLORKEYENABLE via the FBI chroma key | §6 | native mapping table for all 8 TBLEND modes and the filter values (expected TSS0 per mode) | a DX5/DX6 probe (d3dprobe is D3D8 only: build an IDirect3DDevice2/3 + execute-buffer probe); in game JK, MotS, R6 | JK, MotS, R6, AvP, Thief Gold, SS2, Shogo |
| P5 | answer `GUID_GetHeapAlignment` (16/16) | §7.2 | python: GUID answered, sizes 0x8c | placement log: DirectDraw-placed textures/RT 16-aligned; odd-width windowed device | default-path textures, render-target textures, windowed apps |
| P6 | more DX7 texture formats: X8R8G8B8, X1R5G5B5, X4R4G4B4, L8, A8L8, A4L4, R3G3B2, A8R3G3B2; later P8/A8P8 with palettes | §4.4 | native: format→TMU code and alpha-forced-to-1 combine for X formats | `d3dprobe caps` formats + one `*_decode1` per format | Halo, Shogo; UE2 adapts |
| P7 | caps ↔ hardware: ZBIAS, WBUFFER (and `D3DZB_USEW`), MIPMAPLODBIAS, MIRROR, trilinear, specular, DESTALPHA blend, stencil at 32 bpp, lines/points drawn | §3, §5.3, §8.3-8.4 | one native test per mapping | one d3dprobe test per feature | UE2 (bias/trilinear/decals), the rest by count (§13) |
| P8 | Lock without a full engine drain when the surface is not in flight | §7.3 | python: the drain is conditional | Halo Bink time with `-novideo` removed | Halo, streaming titles |
| P9 | throughput: draw DP2 strips/fans with the setup unit's strip/fan mode instead of independent triangles; count FIFO room instead of reading `status` for every vertex | UT2003 flyby **9.41 fps at 640x480x16** (`ONBOX/games/UT2003_flyby/avgfps-2026-11-12-10-28-15.log`; 59,589 DP2, 37,033,284 triangles: ring "context 1 gone"; stats: 116.7 ms "Hardware DrawPrimitive" of 123 ms a frame, `ONBOX/session_log.md:19`); `tri()` per triangle (`vcrdd_d3d.c:1612-1677`), 3 vertices × up to 15 MMIO writes (`vcrdd_3d.c:183-238`), `VcrDdRoom` reads status per vertex (`vcrdd_2d.c:92-108`) | native strip/fan emission | d3dprobe perf `--novsync`, UT2003 flyby | every D3D title |
| P10 | DX8 DDI: defer (§10) | | | | |
| — | D3D32 on by default after P0 and P3 | §11 | python default flip | full matrix at 16/32 bpp | 32-bit titles, windowed D3D |
| — | library: UT2003/2004 staged `UseVSync=False`, `DesiredRefreshRate=60` (`LIB/UT2004/System/UT2004.ini`, `[D3DDrv.D3DRenderDevice]`) conflict with "vsync on" and the CRT's 85-120 Hz | user choice | — | — | UT2003, UT2004 |

---

## 13. Measurements to add before and after the fixes

**Extend `d3dprobe caps`** (`KMD/tools/d3dprobe.c:1323-1397`). Note that D3D8 fails `LockRect` on a `D3DPOOL_DEFAULT` texture (`ONBOX/session_log.md:14`), so probe expectations must allow for it.
- **Caps fields to print:** StencilCaps, LineCaps, Caps2, PresentationIntervals, MaxStreams, MaxPointSize, Pixel/VertexShaderVersion, GuardBand*, MaxTextureRepeat, MaxTextureAspectRatio, VertexProcessingCaps.
- **`CheckDeviceFormat`:**
  - texture formats X8R8G8B8, X1R5G5B5, X4R4G4B4, L8, A8, A8L8, A4L4, R3G3B2, A8R3G3B2, A8P8;
  - Z formats D16_LOCKABLE, D15S1, D24X4S4, D32;
  - `D3DUSAGE_RENDERTARGET` on R5G6B5, A8R8G8B8 and X8R8G8B8 textures (expected yes, yes, no per §4.3);
  - `D3DUSAGE_DYNAMIC`.
- **`CheckDeviceType`:** windowed X8R8G8B8.
- **Calls that should succeed or fail:**
  - CreateTexture as a render target at power-of-2 and non-power-of-2 sizes;
  - `ValidateDevice` for MODULATE2X.

**HAL counters per context**, logged at "context gone" next to the existing totals (`vcrdd_d3d.c:1210-1221`):
- line and point primitives skipped
- STATESET operations
- a bitmask of legacy render states 1-49 ever set to a non-default value
- COLORKEYENABLE with a keyed texture bound
- STENCILENABLE, ZBIAS, SPECULARENABLE
- unsupported TSS ops by value
- `ValidateTextureStageState` calls

Run JK, MotS, R6, AvP, Thief Gold, SS2 and Shogo once with these counters, in attended sessions where a title is risky. That settles §6.2's open half and the P4/P7 scope.

---

## 14. Claims that rest on inference, to verify

- Which runtime DLL serves each interface: d3dim.dll for IDirect3D/2/3, d3dim700.dll for IDirect3D7 (§1).
- D3D8/9 send GetDriverInfo2 only when `DDHALINFO_GETDRIVERINFO2` is set; the observed fact is only that it never came (§2).
- The MIPMAP texture cap comes from `DDSCAPS_MIPMAP` (§3).
- HW/MIXED vertex processing is refused without HWTRANSFORMANDLIGHT; only the fallback is measured (§3, §9).
- The rule inside `CheckDepthStencilMatch` that lets D16 pair with X8R8G8B8. The outcome is measured (§5.1).
- d3dim.dll passes legacy states for SetRenderState callers, not only for execute buffers (§6.2).
- XP honours a `GetHeapAlignment` answer (§7.2).
- The DDI-type enumeration behind "≤ 5 → emulate state blocks" (§9).
- MaxStreams=0 under a DX8 DDI keeps the DX7 token stream (§10).
- Halo's use of A8, L8, A8L8, X8R8G8B8 and P8 textures and the format of its render targets (§4.4, §4.5).
- The cause of the UT2003/2004 distance noise: mip-chain contiguity versus the DXT small-level layout versus cache flushes (§7.1).
- Halo's Bink slowdown comes from the Lock path (§7.3).
- UE2's 16 bpp choice is its 3dfx code path (§11).
