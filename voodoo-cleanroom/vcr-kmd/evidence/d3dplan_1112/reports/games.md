# Direct3D / DirectDraw requirement table - every title deployed to `.124` (V5 6000, vcr-kmd)

Report for the D3D campaign planning run of 2026-11-12. READ-ONLY investigation: nothing was
edited, built or run, and no fleet box was contacted. Every claim carries the file (and line
where the file has lines) it was read from; binaries are cited by the string `strings` finds in
them. Anything that comes from general DirectX / 3dfx / engine knowledge instead of a file read
here is marked **(inferred)**.

Path shorthands used below:

| short | path |
|---|---|
| `REPO` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench` |
| `KMD` | `REPO/voodoo-cleanroom/vcr-kmd` |
| `LIB` | `/mnt/retro-share/Files/Games-Library` (staged library, read-only mount) |
| `PLAN` | `REPO/docs/v56k-benchmark-plan.md` |
| `FIND` | `/home/voidsstr/development/retro-3dfx/FINDINGS.md` |
| `VH5` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` (3dfx's own H5 HAL, guidance only) |
| `G3H5` | `REPO/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5` (our h5 Glide fork) |
| `SWP` | `KMD/evidence/d3dtune_1014` (the 2026-10-14 / 11-08 D3D sweeps) |
| `FBS` | `REPO/scripts/benchmarks/results/v56k_lan_192.168.1.124/day/fbshots` (2026-09-28 frames) |
| `CAPS` | `KMD/evidence/d3dplan_1112/caps_124.txt` (live D3D8 caps from `.124`, 2026-11-12) |

---

## 1. The short version

1. **Fifteen titles on `.124` are routed to our D3D HAL**: UT2003, UT2004 (D3D8), Halo,
   Thief II (D3D9), Max Payne, Hidden & Dangerous Deluxe, Red Faction, BF1942 (D3D8), Shogo,
   System Shock 2, Thief Gold, Rainbow Six (DX6-era IM), Jedi Knight, Mysteries of the Sith,
   Aliens vs Predator (DX5-era execute buffers). Two of them have never reached a device on
   `.124`: BF1942's client is stopped by SafeDisc 2.80, and AvP died on game data until the
   2026-11-12 library fix. Seven more Glide/OpenGL titles carry a D3D renderer that `.124` does not
   route to (section 3c). Five titles are DirectDraw-only 2D (RA2, Yuri, Tiberian Sun, StarCraft,
   Warcraft II), plus DOSBox and software-renderer titles that use DirectDraw for output.
2. **Measured, visible in-game defect waiting to be seen: MODULATE2X.** UT2003's own texture
   stages, logged by the HAL on 2026-11-08, use `D3DTOP_MODULATE2X` in stage 0 (vertex-lit
   meshes) and in stage 1 (the lightmap) (`SWP/sweep_1108/UT2003_combos_unaligned.txt`, combines
   1, 3, 5, 6, 8, 10, 12). The HAL draws it as plain MODULATE (`KMD/display/vcrdd_d3d.c:714`,
   `:862`), so the lit world in both Unreal Engine 2 titles is half as bright as designed. Only
   menus have been looked at since, so nobody has seen it yet. The VSA-100 has the 2x/4x output
   shift (`G3H5/incsrc/h3defs.h` `SST_CM_CC_OUTSHIFT_2X`, `SST_CM_TC_OUTSHIFT_2X`), and 3dfx's
   HAL uses it (`VH5/D6MT.C:649-652`).
3. **The two Diag switches are hard requirements, not tuning.** Seven of the fifteen titles need
   `D3D32` and/or `D3DBigTex`, and both are default OFF in the code
   (`KMD/README.md:110-111`). Without them: H&D Deluxe fails "Unable to initialize graphics"
   (`KMD/README.md:788`), UT2004 dies at `CreateTexture` (`KMD/README.md:480-482`,
   `KMD/evidence/silicon/d3d32_step16/d3d32_titles_sweep.json` "Critical Error"), Halo's menu
   comes out white (`FIND:435-442`), and Thief II drops to its DX6 path at 640x480
   (`LIB/Thief2/Play Thief 2.bat:14-21`). A fresh vcr-kmd install runs none of these.
4. **In-game evidence is thin and pre-dates the 2026-11-08 fixes.** Only Jedi Knight, Mysteries of
   the Sith and Thief II were ever seen in-game on this HAL (2026-09-28: `PLAN:628`, `PLAN:673-674`,
   `FBS/jkdf2_d3d3.png`, `FBS/mots_game.png`, `FBS/thief2_d3d9_armed_4.png`). That was before the
   half-pixel and alignment fixes. Every other D3D title has a menu or intro frame at most, and the
   LAN-readiness matrix marks `.124` `blk` for all of them (`REPO/docs/lan-multiplayer-status.md:42-75, 82-83`).
   The compat DB rows for `.124` are mostly 2026-08-31 measurements on the **GeForce2 GTS** that
   was in the box then (`compat.py status --box .124 --json`: `measured_at 2026-08-31`, details
   naming "the GeForce2 GTS"), so they say nothing about our HAL.
5. **Advertised but not implemented**, so an application that trusts the caps gets something
   wrong instead of a fallback:
   - Lines are advertised (`vcrdd_d3d.c:2063,2072`) and dropped (`:1690-1715`).
   - Texture colour key is advertised as `D3DPTEXTURECAPS_TRANSPARENCY` (`:2035`), and
     `D3DRENDERSTATE_COLORKEYENABLE` is never read.
   - `ValidateTextureStageState` answers "1 pass, OK" to everything (`:1905-1910`), including the
     MODULATE2X sets it then draws wrong.
   - The D3D8 runtime reports X8R8G8B8 + D16 as a matching pair (`CAPS` `zmatch`), but
     `ContextCreate` refuses that pair (`KMD/include/vcr_rtfmt.h` `vcr_rt_format`).
6. **Stability: Max Payne froze the box once (2026-10-04, cause not established, `PLAN:553-556`).**
   It is the only D3D title with a freeze on record, and it is why no D3D title goes into an
   unattended sweep.
7. **Performance: D3D renders on one of the four VSA-100s.** The HAL never programs SLI (no
   `sliCtrl`/`chipMask` writes in `vcrdd_d3d.c`/`vcrdd_3d.c`, and `Reset3D` writes `sliCtrl = 0`,
   `KMD/README.md:112`). 3dfx's HAL promotes every 3D surface to SLI (`VH5/DDSURF.C:452`
   `Compute_SLIAA_Config`, `VH5/DDFXNT.C:2406` `Promote_DeviceToSLIAA`). Other costs:
   - every register goes through the PCI FIFO, per vertex (`vcrdd_3d.c:183-224`);
   - every `Lock` drains the engine (`KMD/display/vcrdd_ddraw.c:658`);
   - the 16 bpp flip rate on `.124` (42-47 flips/s at 85 Hz) is unexplained (`KMD/README.md:669-677`).

---

## 2. What our HAL gives a game today (the yardstick for the "risk" column)

Live D3D8 caps from `.124` (`CAPS`, one JSON line), decoded, against the code that produces them:

| area | what the runtime sees | code | VSA-100 / 3dfx HAL can do |
|---|---|---|---|
| render targets | 16 bpp; 32 bpp only with `Diag\D3D32` | `vcrdd_d3d.c:2079`, `vcr_rtfmt.h` | 16/32 (`VH5/D3INIT.C` `thirtyTwoBppRendering`) |
| Z | D16; D24X8/D24S8 with D3D32 (stencil never written) | `vcrdd_d3d.c:2176-2215`, `vcr_rtfmt.h` (STENCIL IS NOT SUPPORTED) | stencil 8 ops at 32 bpp (`VH5/D3INIT.C:3241-3258`) |
| Z/target pairing | runtime says X8R8G8B8+D16 = match; ContextCreate refuses 32/16 | `CAPS` `zmatch`; `vcr_rt_format` | chip has no 32-colour/16-Z mode (`vcr_rtfmt.h` header) |
| texture formats | 565, 1555, 4444; +8888 (BigTex bit 2); +DXT1-5 (bit 1); no P8/L8/A8/A8L8/X8R8G8B8 | `vcrdd_d3d.c:2083-2101` | A8, I8, AI44, AI88, P8, AP88, ARGB8888, DXT1-5, FXT1 (`G3H5/glide3/src/g3ext.h:158-168` + Glide 3 base formats (inferred)) |
| max texture | 256; 2048 with BigTex bit 0 | `vcrdd_d3d.c:2158` | 2048 (`VH5/D3INIT.C:3190-3196`) |
| stages | 2 stages, 2 textures | `vcrdd_d3d.c:2169-2170` | 2 (`VH5/D3INIT.C:3291`) |
| texture ops | DISABLE, SELECTARG1/2, MODULATE, ADD, BLENDDIFFUSEALPHA, BLENDTEXTUREALPHA (`0x184f`) | `vcrdd_d3d.c:2165-2168` | + MODULATE2X/4X, SUBTRACT, ADDSIGNED(2X), ADDSMOOTH, BLENDFACTOR/CURRENT/TEXTUREALPHAPM, MODULATEALPHA_ADDCOLOR family (`VH5/D3INIT.C:3264-3288`) |
| what is drawn for an unsupported op | MODULATE2X/4X and the rest = MODULATE (stage 0); = TMU multiply (stage 1); arg modifiers COMPLEMENT/ALPHAREPLICATE masked off | `vcrdd_d3d.c:671, 714, 862` | combineMode OUTSHIFT, INVERT (`VH5/D6MT.C:649-652`) |
| blend | Src: ZERO ONE SRCALPHA INVSRCALPHA DESTCOLOR INVDESTCOLOR SRCALPHASAT BOTHSRC/BOTHINV (`0x1f33`); Dest: ZERO ONE SRCCOLOR INVSRCCOLOR SRCALPHA INVSRCALPHA (`0x3f`); alpha channel blend always ONE/ZERO | `vcrdd_d3d.c:630-663, 998, 2025-2031` | + src SRCCOLOR/INVSRCCOLOR, DESTALPHA/INVDESTALPHA both sides on Napalm (`VH5/D3INIT.C:3131-3156`; `G3H5/glide3/src/g3ext.h:155-156` `GR_BLEND_SAME_COLOR_EXT`) |
| depth | Z only; `ZENABLE = D3DZB_USEW` turns depth OFF; no Z bias | `vcrdd_d3d.c:986` | W-buffer + Z bias (`VH5/D3RSTATE.C:1017-1022, 1108-1116`; `vcr_3dregs.h:108,115` `FZ_WBUFFER`/`FZ_ZBIAS`) |
| fog | table fog on W, vertex fog via specular alpha | `vcrdd_d3d.c:1001-1018`; `vcrdd_3d.c:202-208` | + FOGGOURAUD/specular (`VH5/D3INIT.C:2798-2812`) |
| shading | gouraud only; SHADEMODE never read; specular RGB dropped | `vcrdd_d3d.c:921-1032` (no SHADEMODE); `vcrdd_3d.c:187` (diffuse only) | specular gouraud advertised by 3dfx (`VH5/D3INIT.C:2798-2801`) |
| filtering | point/bilinear, nearest-mip only (no MIPFLINEAR); no LOD bias; aniso 1 | `vcrdd_d3d.c:2036-2040, 2160`; `vcrdd_3d.c:116,124` (`tDetail` 0) | trilinear advertised (`VH5/D3INIT.C:2837-2849`); `SST_LODBIAS` field (`G3H5/incsrc/h3defs.h:719-722`) |
| addressing | WRAP, CLAMP, INDEPENDENTUV; MIRROR not offered | `vcrdd_d3d.c:792-795, 2044-2045` | MIRROR (`VH5/D3INIT.C:2874`; `h3defs.h:731-732` `SST_TMIRRORS/T`) |
| primitives | triangles (all 9 DP2 forms) and TRIANGLEFAN_IMM; lines and points parsed and skipped | `vcrdd_d3d.c:1612-1715` | lines/points (`VH5/D6LINE.C`, `VH5/D6POINT.C`) |
| vertices | `D3DFVF_XYZRHW` only; 2 tex coords per set (q ignored); 16 bits of Z per pixel | `vcrdd_d3d.c:1258`; `vcrdd_3d.c:211-222` | no T&L on the chip (VertexShaderVersion 0, `CAPS`) |
| palette | DP2 `SETPALETTE`/`UPDATEPALETTE` parsed and dropped | `vcrdd_d3d.c:1557, 1716-1723` | `ALPHAPALETTE` (`VH5/D3INIT.C:2819`) |
| validate | `ValidateTextureStageState` = 1 pass, OK, always | `vcrdd_d3d.c:1905-1910` | - |
| SLI | none: D3D renders on chip 0 | (no SLI code) | vendor promotes 3D surfaces to SLI (`VH5/DDSURF.C:452`) |
| DirectDraw | BLT, BLTCOLORFILL, READSCANLINE, FLIPNOVSYNC; no colour-key caps, no stretch, no overlay; 8 bpp palette through `DrvSetPalette` | `vcrdd_ddraw.c:101-111, 430, 532`; `KMD/display/vcrdd.c:620` | overlay/VPE (`VH5/DDOVL32.C`, `VH5/DDVPE32.C`) |

`.124` runs `D3D32=1` and `D3DBigTex=7` (task context; `PLAN:570, 619-620`). Every row below
assumes that unless it says otherwise.

---

## 3. The requirement table

"Staged res/bpp" is what the library launcher or config asks for **on `.124`**. Every
FLEETRES-driven D3D title measured **1280x960** in the 2026-11-08 sweep
(`SWP/sweep_1108_fixed/20261108_020841/sweep.json`), and FLEETRES hands Glide titles 85 Hz there
(`KMD/evidence/gametune_1001/README.md:295-320`). So FR_W x FR_H on `.124` is 1280x960@85. That
value is read off the sweep, not off the box. **Status** = the latest evidence on vcr-kmd (V5 6000).

### 3a. Titles that render through our Direct3D HAL on `.124`

| title | DX level / API | renderer on .124 | staged res/bpp | status on our HAL | features it needs | risk on our HAL |
|---|---|---|---|---|---|---|
| **Unreal Tournament 2003** | D3D8 (`System/D3DDrv.dll` imports `d3d8.dll`) | `D3DDrv.D3DRenderDevice` (`LIB/UT2003/System/UT2003.ini:21`) | FR (1280x960), 32 bpp (`Use16bit=False`, `:259`) | menu text fixed 11-08 (alignment), menu PASS at 1280x960 (`SWP/sweep_1108_fixed`); never seen in game; `.124` `blk` | DXT1/3/5 incl. 512xN DXT5 fonts, A8R8G8B8 to 1024x512, **MODULATE2X stage 0 and 1** (measured), TFACTOR, 2 stages plus multipass (detail, lightmap), DXT1 lightmaps, trilinear, LOD bias -0.5, cube maps, w-fog, Z bias and projected tex coords for decals/projectors (inferred), software VP | **HIGH**: lit world and actors half-bright (MODULATE2X); needs BigTex=7 + D3D32; LOD bias/trilinear/cube silently lost; vsync off staged (`:262`); one-chip fill at 1280x960x32 |
| **Unreal Tournament 2004** | D3D8 (`System/D3DDrv.dll` imports `d3d8.dll`; D3D9Drv present, unused) | `D3DDrv.D3DRenderDevice` (`LIB/UT2004/System/UT2004.ini:21`) | FR, 32 bpp (`:329`) | menu PASS 1280x960/1024 (11-08); without BigTex: "CreateTexture failed" (`KMD/README.md:480-482`); never seen in game | same engine as UT2003 (`UT2004.ini:315-335`) | **HIGH**, same as UT2003 (MODULATE2X is the same D3DDrv code path, inferred from the UT2003 measurement) |
| **Halo: Combat Evolved 1.10** | D3D9 (`halo.exe` strings `d3d9.dll`, `Direct3DCreate9`; no static import) | Halo's FF/0.0 shader path, which one not recorded (switches `-useff`/`-use00` exist in `halo.exe`) | `-vidmode 1280,960,85` (`LIB/Halo/Play Halo.bat:13`); bpp chosen by the game (32 needs D3D32, inferred) | textured main menu (`PLAN:627`, `SWP/sweep_1108_fixed` PASS); **gameplay never seen**; `-novideo` because Bink runs ~10x slow (`FIND:446-450`) | DXT1-5 **incl. DXT2/DXT4**, A8R8G8B8, X8R8G8B8, **A8/Y8/AY8/A8Y8, P8-bump** (`halo.exe` format strings), render targets (switched off for `0x121a:0x0009`, `LIB/Halo/config.txt:115-118`), cube maps, decal depth bias (`config.txt:326-328` per-card `DecalZBiasValue`), stencil (`CCW_STENCIL*`), specular, fog, Bink video textures | **HIGH**: missing 8-bit/luminance formats can give white surfaces as DXT2/4 once did (`FIND:439-441`); Bink/Lock path slow; decals without bias; SSE2 floor unproven past the menu (`FIND:443-445`) |
| **Max Payne 1.05** | D3D8 (`e2driver/e2_d3d8_driver_mfc.dll`; exe imports only DINPUT) | D3D8 HAL, software VP | FR, 32 bpp (`LIB/MaxPayne/install.reg:21`; launcher W/H `Play Max Payne.bat:56`) | main menu 1280x960 (09-29, `PLAN:599`, `KMD/evidence/lan_20260929_sweep/maxpayne_dt_menu_1280x960.png`); **froze the box 10-04** (`PLAN:553-556`); attended only | DXT1-5, 32-bit, render targets (`DRV_setRenderTarget`), cube maps, texop ADD/BLENDTEXTUREALPHA/MODULATE/SELECT (the caps its driver dumps), BUMPENVMAP optional, software vertex shaders | **HIGH (stability)**: freeze cause unknown; `vcrctl fbshot` cannot see it (`KMD/README.md:156`) |
| **Hidden & Dangerous Deluxe** | D3D8 (`Bin/igraph2.dll` loads `d3d8.dll`) | Illusion LS3D i3d2 on D3D8 | FR, **32 bpp forced** (`LIB/HiddenAndDangerous/Play Hidden and Dangerous Deluxe.bat:22`) | profile screen over its 3D scene, 1280x960 (`SWP/sweep_1108_fixed` PASS); needs D3D32 ("Unable to initialize graphics" without, `KMD/README.md:788`); in game only on the GeForce2 (08-30) | lightmaps (uncompressed: "cannot build lightmap with compressed textures"), bump maps (falls back: "cannot find bumpmap texture format"), reflection maps, fog, DXT, P8/A8P8 enumerated (`i3d2.dll`/`igraph2.dll` strings) | MEDIUM: in game unchecked; stage-1 ops beyond MODULATE/ADD map to multiply (inferred) |
| **Red Faction 1.00** | D3D8 (`rf.exe` strings `d3d8.dll`, `Direct3DCreate8`, `gr_direct3d.cpp`) | D3D8 HAL | launcher writes 1280x960x32 to **HKLM** (`Play Red Faction.bat:56-58`); measured **640x480x16** (sweep 11-08 stats; `PLAN:602`) | menu renders since 11-08 (`SWP/sweep_1108_fixed`); 09-29 black frame never explained (`PLAN:602`); never seen in game | hard requirements met: MODULATE + SRCALPHA/INVSRCALPHA (`rf.exe` "missing the following features"); detail textures, trilinear, fog, ARGB1555/4444, GeoMod dynamic textures (inferred), `-voodoo`/`-voodoo3` switches | MEDIUM: resolution and vsync config never land (inferred HKCU, see 4.5); raising it to 32 bpp may hit the D16@32 refusal; trilinear lost |
| **Rainbow Six (GOG)** | DX5/6 IM through DirectDraw (`RainbowSix.exe` imports DDRAW; `CreateZBuffer() : AddAttachedSurface()`) | D3D (`SoftwareOnly=0`, `LIB/RainbowSix/install.reg:87`) | 640x480x16 (its own pick, `PLAN:163-165`) | intro + main menu after vcrdd 95678704 (`PLAN:163-174`, `SWP/sweep_1108_fixed/RainbowSix_intro_then_menu_z16in32.png`); never seen in game | Z from the 32 bpp desktop's list (now 16/16 in the D24's rows), fog (readme "Turn off fog"), palettized textures if offered (`loadtex` palette strings), colour keys (`Surface::SetTransparentValue`) | MEDIUM: colour-keyed textures if it uses them (inferred); in game unchecked |
| **Shogo: MAD** | DX6 DrawPrimitive (`d3d.ren` imports DDRAW; `DrawPrim: %s`) | LithTech 1.0 `d3d.ren` (`LIB/Shogo/autoexec.cfg:23`) | FR x **16** (`autoexec.cfg:21` `screendepth 16`; launcher `:31`) | menu 1280x960 PASS (11-08); compat (GeForce era): "client menu renders intermittently" | **8-bit palettized textures preferred** ("Couldn't find suitable 8 bit texture format"), 1-pass (2 TMU) or 2-pass lightmaps, env maps, table fog, a SRCCOLOR-as-source blend check, palette alpha (`d3d.ren` strings) | MEDIUM-LOW: no P8 (falls back to 16-bit); SRCCOLOR source not advertised, though the chip can do it |
| **System Shock 2** | DX6 (Dark engine `lgd3d`; `SHOCK2.ICD` is SafeDisc-v1 encrypted) | Dark DX6 | 640x480 (no `game_screen_size` staged, `LIB/SystemShock2/cam.cfg`), 16 bpp (inferred) | menu (09-29 under DAEMON Tools, `PLAN:599`; 11-08 sweep); never seen in game | 16-bit textures, alpha, 2-stage or 2-pass lightmaps, fog (inferred); a texture-memory-report check (Thief's string, same engine) | MEDIUM (unknown): nothing in game ever checked |
| **Thief Gold** | DX6 (`THIEF.EXE` strings `lgd3d\d6Render.cpp`, `d6Frame.cpp`; ddraw loaded dynamically) | Dark DX6 | 640x480 (no res staged; `Play Thief Gold.bat` just starts `thief.exe`) | intro/menu (11-08, force-closed); never seen in game | as SS2; "Direct3d device driver does not accurately report texture memory usage" check (`THIEF.EXE`) | MEDIUM (unknown); our `dwVidMemFree` is a constant (`vcrdd_ddraw.c:109-110`) |
| **Thief II (NewDark 1.18)** | D3D9 (`Thief2.exe` strings `d3d9.dll`, `d3dx9_43.dll`) with a DX6 fallback | NewDark `use_d3d_display` (`LIB/Thief2/cam_ext.cfg:11`) when D3D32 + BigTex bits 0+2 are armed, else DX6 (`Play Thief 2.bat:14-21`) | FR x 32 (`Play Thief 2.bat:5`; `cam_ext.cfg:41` `force_32bit`) | **in mission 1280x960x32 on 09-28** (`PLAN:628`, `FBS/thief2_d3d9_armed_*.png`); menu 11-08 | 32-bit textures (`:130`), big textures, trilinear (`:152`), 16x aniso (`:149`), **8x MSAA** (`:211`), distortion FX render targets (`:255`), alpha-to-coverage (`:259`), particles staged as disks, not points (`:179,182`) | LOW-MEDIUM: runs; MSAA contradicts the user's "no AA" (falls back, inferred); trilinear/aniso lost |
| **Jedi Knight: Dark Forces II** | DX5 execute buffers (`JK.EXE` imports DDRAW; `D3DERR_EXECUTE_*`) | D3D (menus are 8 bpp DirectDraw) | per-box Display menu; played 1280x960x16 (`PLAN:673`) | **played 09-28 after the D3DOP_EXIT fix** (`vcrdd_d3d.c:1773`; `FBS/jkdf2_d3d*.png`), pre-11-08 fixes | 16-bit textures, alpha blend, gouraud colour, colour-keyed textures and automap lines (inferred) | LOW-MEDIUM: textures look point-sampled in `FBS/jkdf2_d3d3.png` (MotS is filtered, so probably JK's own option, inferred); re-verify after 11-08 |
| **Mysteries of the Sith** | DX5 execute buffers (`JKM.EXE` `D3DERR_EXECUTE_*`) | D3D | 1280x960, 16-bit (`PLAN:674`; `README-FLEET` "16-bit") | **PASS in D3D 1280x960: 7,014 DP2, 342k tris, 0 unparsed** (`PLAN:674`, `FBS/mots_game.png`) | as JK | LOW: re-verify after 11-08 |
| **Aliens vs Predator Gold** | DX5/6 IM via DirectDraw 6 (`avp.exe` imports DDRAW; execute-buffer error table, `AwSetD3DDevice`) | Rebellion `d3d_render.cpp` | 640x480x16@85 (flight recorder, `PLAN:206-207`) | **never reached a D3D device on `.124`** (game data, `PLAN:222-232`); library fixed 11-12 (`LIB/AliensVsPredator/README-FLEET.txt:1-12`); 7 boxes reach the menu, `.124` not among them (`REPO/.claude/evidence-readiness-2026-11-11/fix-avp/fleet/AliensVsPredator.json`) | palettized or 16-bit RGB texture formats (`AwSetPixelFormat`, "chosen %u-bit palettized texture format"), alpha transparency, D3DOP_EXIT handling, MMX (`requires.json`) | MEDIUM: first real D3D run is still to come; colour key (inferred) |
| **Battlefield 1942 1.61b** | D3D8 (`BF1942.exe` imports `d3d8.dll`) | Refractor 2 PCDX8 | FR x 32 (`Play Battlefield 1942.bat:56,59`) | **blocked before D3D**: SafeDisc 2.80 in `Mods\bf1942\Mod.dll` ("Cannot locate the CD-ROM", `SWP/sweep_1108` + compat) | cube maps (`makeNormalizedHalfVectorCubeMap`), stencil (`SStencilEnable`), DXT1-5, software vertex shaders, "32bit depth buffer" option; warns "only officially supports cards that supports HTL" | n/a until the DRM is solved; then a T&L warning dialog, cube maps (hardware cannot), stencil, D16@32 |

### 3b. DirectDraw-only and DirectDraw-output titles

| title | API | on .124 | staged res/bpp | status | needs | risk |
|---|---|---|---|---|---|---|
| Red Alert 2 / Yuri's Revenge | DirectDraw (`game.exe`, `gamemd.exe` import DDRAW) | 2D software + DD Lock/Blt/Flip | menu 800x600, in game FR (`Launch Red Alert 2.bat` -> `RA2.INI` ScreenWidth/Height), 16 bpp | renders 1280x960 (09-29 sweep, `lan_20260929_sweep/sweep_part1.log`; Yuri 800x600 menu) | 16 bpp primary + back buffer, Lock, Blt, Flip | LOW |
| Tiberian Sun | DirectDraw (`GAME.EXE` imports DDRAW; "Trying an 8 bit primary buffer format") | same | FR (`Play Tiberian Sun.bat` -> `SUN.INI`), 16 bpp, `VideoBackBuffer=yes` (`SUN.INI`) | renders 1280x960 (09-29) | VRAM back buffer, VRAM-to-VRAM blits (our 2D engine), `ForceBltToPrimary` | LOW |
| StarCraft / Brood War | DirectDraw through `storm.dll` (dynamic `DirectDrawCreate`) | 8 bpp palettized | 640x480x8 | PASS 640x480 (09-29) | 8 bpp primary Lock, palette changes (`DrvSetPalette`) | LOW; GDI cannot photograph it (compat note) |
| Warcraft II BNE | DirectDraw through `Storm.dll` | 8 bpp | 640x480x8 | PASS (09-29) | as StarCraft | LOW |
| Descent II (Win95 engine) | DirectDraw (`DESCENTW.EXE` imports DDRAW) | 8-bit software | game's own | registration card fixed 10-03; PASS on `.124` (`gametune_1001/README.md:27`, `descent2_regcard/`) | 8 bpp DD | LOW |
| DOSBox `output=ddraw`: Descent 1, Descent 2 (DOSBox shortcuts) | SDL 1.2 DirectX5 backend | `dosboxD1.conf`/`dosboxD2.conf`: `output=ddraw fullresolution=original` | 640x480 or 640x400 | **black 640x400** = DOSBox's surface fallback after one DirectDraw call failed (`KMD/README.md:592-610`); 09-29 sweep frame mean 0.0 | DD surface with explicit pitch, Lock, stretched Blt to the primary | MEDIUM: an unidentified DirectDraw failure (`ddlab sdlddraw` replays it; `DdHeapFloor` is the one hypothesis) |
| DOSBox `output=overlay`: Carmageddon, Daggerfall, Redneck Rampage, Shadow Warrior | SDL YUV overlay | no `DDCAPS_OVERLAY` in our caps (`vcrdd_ddraw.c:101-108`) | per conf | PASS (09-29) | a YUV overlay, or SDL's software YUV path (inferred) | LOW (CPU cost only, inferred) |
| DOSBox `output=surface`: Master of Orion II, Warcraft 1 | GDI (inferred) | - | - | PASS | - | none |
| Software renderers: WinQuake, Hexen II, SiN software | DirectDraw / DIB (`sin.exe` imports DDRAW) | 320x200 (Quake, Hexen II), 1280x960 (SiN) (09-29) | - | PASS | low-res DD modes | LOW |

### 3c. D3D renderers that `.124` does not use (fallbacks)

| title | D3D path in the tree | what `.124` uses instead (evidence) |
|---|---|---|
| Unreal Gold, UT 436, Deus Ex | UE1 `D3DDrv.dll` (DX6/7); staged `GameRenderDevice=D3DDrv` (`DEUSEX.INI:18`, `Unreal.ini:21`) | GlideDrv 1024x768x16@85, because `GlideRender=1` makes FLEETRES write it (`gametune_1001/README.md:297-320`; `provisioning/fleetres/fleetres.c:798-812`) |
| Turok 2 | `Video_D3D.dll` (imports DDRAW; DX6) | `Video_3DFx.dll` -> Glide (`gametune_1001/README.md:82`) |
| Carmageddon 2 | BRender `d3d.bdd`/`hardware_d3d.bdd` | Glide (`FR_GLIDE=1` moves nGlide aside, `Play Carmageddon 2.bat`) |
| Descent 3 | `main.exe` D3D renderer (imports DDRAW) | OpenGL 1280x960 (`PLAN:506-509`) |
| Half-Life / CS 1.6 / mods | GoldSrc `hw.dll` (imports DDRAW) D3D mode | OpenGL (`gametune_1001/README.md:105-114`) |
| Serious Sam TFE/TSE | Serious Engine D3D8 API (inferred) | OpenGL, 4-chip SLI (compat) |
| Tribes 2 | `GameData/OpenGL2D3D.dll` (imports DDRAW) | OpenGL 1280x960x32 (`PLAN:506`) |

UE1 `D3DDrv` was the 32-bit route under AmigaMerlin's HAL (`PLAN:91`). It is a fallback here and
does not need work now.

### 3d. Library titles not on `.124`

| title | D3D path | why not on `.124` |
|---|---|---|
| Wing Commander Prophecy / Secret Ops | `PROPHECY/GL_00005.DLL`, `GL_00006.DLL` import DDRAW (D3D builds beside the Glide `GL_00002.DLL`) | `max_os win9x` (`LIB/WCProphecy/requires.json`); compat `absent` |
| Star Wars Rogue Squadron 3D | `ROGUE/Rogue Squadron.EXE` imports DDRAW (D3D, DX6, inferred) | `max_os win9x`; staged for the Voodoo 2 box |
| Die by the Sword | `rld3d.dll` imports DDRAW (DX5 D3D) beside `rl3dfx.dll` (Glide) | `max_os win9x` |

These matter only if one is re-staged for XP. If one is, its D3D path would be a DX5/6 one like
JK's: execute buffers or DrawPrimitive, colour-keyed textures, 16-bit textures (inferred).

---

## 4. Per-title notes (evidence, fallbacks, what to watch in game)

### 4.1 UT2003 and UT2004 (Unreal Engine 2, D3D8)

- **API / device.** Both `System/D3DDrv.dll` import `d3d8.dll`, and the exes import `d3d8.dll` +
  `DDRAW.dll`. The staged ini picks D3DDrv (`UT2003.ini:21`, `UT2004.ini:21`). The ICD's own load
  log shows neither title loading OpenGL (`gametune_1001/README.md:113-114`).
- **Staged D3DDrv settings** (`UT2003.ini:244-270`, `UT2004.ini:314-340`):
  - `Use16bit=False` (32 bpp target and 32-bit textures)
  - `UseCompressedLightmaps=True` (DXT1)
  - `DetailTextures=True`, `UseTrilinear=True`, `UseCubemaps=True`
  - `DefaultTexMipBias=-0.5`
  - `UseHardwareTL=True` (falls back to software VP; the chip has no T&L)
  - `UseStencil=False`
  - **`UseVSync=False`** (`UT2003.ini:262`, `UT2004.ini:332`)
- **What D3DDrv probes** (UTF-16 strings in `UT2004/System/D3DDrv.dll`): "D3D Detected: 3dfx
  video card", "Supports LOD biasing", "Supports trilinear", "Supports Z biasing", "using
  cubemaps / not using cubemaps", "Using w- pixel fog / z- pixel fog / vertex fog / range fog",
  "WARNING: no support for DXT1/3/5", "ConvertDXT", "DecompressTextures", and the multipass
  messages "No stages left for lightmap / detail ...". The fallback design is therefore real:
  2-stage cards get multipass, DXT-less cards get decompression, cube-less cards lose
  environment maps.
- **Measured on our HAL.** `SWP/sweep_1108/UT2003_combos_unaligned.txt` is the `log_combo` dump
  (event 513 what 21-23, `vcrdd_d3d.c:882-919`). The format is "s0 COLOROP ARG1 ARG2 a ALPHAOP ARG1
  ARG2 s1 COLOROP a ALPHAOP".
  - Combine 1/5/6: `s0 2 2 1 ... s1 5 a 2` = stage 0 SELECTARG1(TEXTURE), **stage 1 MODULATE2X**.
    The TMU textureMode there carries `TM_TC_MULT` (`0x24000` inside `tm 800242cf`), i.e. a plain
    multiply.
  - Combine 3/10/12: `s0 5 2 0 a 4 2 0` = **stage 0 MODULATE2X(TEXTURE, DIFFUSE)**.
  - Combine 7: `s0 2 3 1` = SELECTARG1(TFACTOR).
  - Textures: A8R8G8B8 (`fmt 20` = 32 bits), DXT1 (`31545844`), DXT5 (`35545844`), up to 512x512
    in that log.
  - UT2003 calls MODULATE2X even though `TextureOpCaps` (`0x184f`) does not list it. Our
    `ValidateTextureStageState` says such sets are fine (`vcrdd_d3d.c:1905-1910`), so the engine has
    no reason to take its fallback. Whether UE2 consults ValidateDevice for this at all is inferred.
- **Expected in-game result today (inferred from the code):** BSP lightmaps and vertex-lit meshes
  at half intensity, i.e. a very dark game. Menus do not use 2X, which is why nobody saw it.
- **Diag dependence.** Without `D3DBigTex` UT2004's device dies at `CreateTexture`
  (`KMD/README.md:480-482`; `d3d32_titles_sweep.json` "Critical Error"). Its textures exceed 256
  and are DXT. UT2003 is the same engine with the same content shape (inferred).
- **Performance.** 1280x960x32 on one chip, software VP, and every triangle's registers through
  the PCI FIFO. UT2004 has a built-in benchmark/demo path for measuring this (inferred).

### 4.2 Halo: Combat Evolved (D3D9)

- **Device and config.**
  - D3D9 is loaded dynamically (`halo.exe` strings `d3d9.dll`, `Direct3DCreate9`, `DirectDrawCreateEx`).
  - Shader-path switches in the binary: `-useff -use00 -use11 -use14 -use20 -use2a`. The info line
    format is "... Shader=%s" (`halo.exe` strings). Which path `.124` takes is not recorded.
  - The staged `config.txt` gives `Vendor 0x121a / 0x0009 "Voodoo 4 and 5"` the flag
    `DisableRenderTargets` (`LIB/Halo/config.txt:115-118`). That flag avoids Halo's FATAL dialog
    105, which is render-target creation failing (`FIND:435-442`).
- **What has to be offered.**
  - `D3DBigTex=7` and `D3D32` (armed "for Halo" by operator decision, `PLAN:619-620`).
  - DXT2/DXT4 as well as DXT1/3/5. The menu was white until they were listed (`FIND:439-441`).
  - The binary also names bitmap formats `a8`, `y8`, `ay8`, `a8y8`, `p8`, `p8-bump`, `x8r8g8b8`,
    `a1r5g5b5`, `a4r4g4b4`, `r5g6b5`, `a8r8g8b8`, `dxt1/3/5` (`halo.exe` strings). We offer none of
    A8, L8/Y8, A8L8, P8 or X8R8G8B8 (`vcrdd_d3d.c:2083-2101`).
  - Risk: Halo's own handling of a missing DXT2/4 was to draw white rather than convert. Missing
    8-bit/luminance formats may do the same to HUD, effects or detail maps in gameplay (inferred
    from that precedent).
  - The VSA-100 samples A8, I8, AI44, AI88 and P8 natively (Glide 3 base texture formats, inferred;
    the Napalm additions are in `G3H5/glide3/src/g3ext.h:158-168`).
- **Other needs** (strings / config):
  - per-card decal depth bias: `DecalZBiasValue`, `DecalSlopeZBiasValue` (`config.txt:326-328`, for
    Radeons), and `DEPTHBIAS` in the binary;
  - `CCW_STENCILFAIL/FUNC/PASS/ZFAIL` (two-sided stencil);
  - cube maps (`cube map`, `CubeTextureFilterCaps`);
  - specular (`DisableSpecular` flag, `c_specular_brightness`);
  - fog (`environment_fog`, `c_fog_color_correction_*`);
  - BUMPENVMAP family (FF bump effects);
  - `MinMaxBlendOpIsBroken` (blend ops);
  - `UseAnisotropicFilter`.
  We advertise none of these, so they need the right fallback, and gameplay has never been looked at.
- **Video.** Bink intros ran about 10x slow on vcr-kmd. The launcher passes `-novideo` while
  `vcrmp` runs (`LIB/Halo/Play Halo.bat:4-6,13`; `FIND:446-450`, "texture Lock path is the first
  suspect"). Our `Dd_Lock` drains the whole 2D/3D engine on every lock (`vcrdd_ddraw.c:647-661`), and
  every unlock marks the texture for a TMU flush (`vcrdd_ddraw.c:676-680`, `vcrdd_d3d.c:1287-1320`).
  Bink updating one texture per frame serialises CPU and chip; whether it also reads VRAM back is
  inferred.
- **Gate.** `requires.json` declares SSE2 and the gate refuses `.124`; an operator override deploys
  it. The menu ran without SSE2, but "Gameplay is NOT yet seen - do not relax the floor"
  (`FIND:443-445`).
- **Hard-requirement fallbacks Halo itself has:** `-useff`, `-use00`, `SafeMode`, `ForceShader`,
  `UseFixedFunction`, `DisableSpecular` and `DisableAlphaRenderTargets` flags in `config.txt`
  (`config.txt` header comments). They are useful as staged per-card workarounds for `0x121a:0x0009`
  while HAL features land.

### 4.3 Max Payne (D3D8)

- **Device.** `e2driver/e2_d3d8_driver_mfc.dll` is the D3D8 renderer, and the exe imports no D3D at
  all. Its strings show the caps it checks:
  - `D3DTEXOPCAPS_ADD/BLENDTEXTUREALPHA/BUMPENVMAP/BUMPENVMAPLUMINANCE/DISABLE/MODULATE/SELECTARG1/SELECTARG2`;
  - `cube maps`, `DRV_setRenderTarget`, DXT1-5;
  - `CreateVertexShader (software)`, "D3D HAL HW T&L", "D3D Software Rasterizer", "D3D_OK stages:%d".
- **Config.** `install.reg:21` sets 32 bpp. The launcher sets W/H and passes `-nodialog -skipstartup`
  (`Play Max Payne.bat:56,67`).
- **Status.**
  - Main menu at 1280x960 on our HAL (09-29).
  - 10-04: "Max Payne stopped responding during that session (the user saw it frozen, Num Lock
    alive). A forced close restored the desktop. The cause is not established, so no D3D title goes
    in an unattended sweep" (`PLAN:553-556`).
  - The default `vcrctl fbshot` parks in win32k under its exclusive mode, and `--probe` works
    (`KMD/README.md:156`).
  - On the GeForce2 it played the tutorial level (`LIB/MaxPayne/README-FLEET.txt:115-121`).
- **Watch in game:** bullet-time and render-target effects, cube maps (it has a fallback by caps,
  inferred), and the freeze. A supervised run with the flight recorder (event ring) at
  `LogLevel 2` is the only safe way to localise the hang.

### 4.4 Hidden & Dangerous Deluxe (D3D8, LS3D)

- `Bin/igraph2.dll` loads `d3d8.dll` and dumps D3D8 caps. `i3d2.dll` holds the renderer: "cannot
  build lightmap with compressed textures", "cannot find bumpmap texture format", "A220 -
  Reflection map", "A230 - Bump map", `c_light_pfog_*`. It enumerates D3DFMT P8/A8P8/D16/D24S8/
  D24X8/D32/DXT1-5 (strings).
- It forces 32 bpp in the registry (`Play Hidden and Dangerous Deluxe.bat:22`), and without D3D32 it
  failed "Unable to initialize graphics" (`KMD/README.md:788`).
- The 11-08 frame shows the profile panel drawn over a 3D scene. Two other boxes cannot draw that
  panel (`REPO/docs/lan-multiplayer-status.md:99-100`).
- In game it was only seen on the GeForce2 (`LIB/HiddenAndDangerous/README-FLEET.txt:95-97`).

### 4.5 Red Faction (D3D8)

- **API.** `rf.exe` strings: `d3d8.dll`, `Direct3DCreate8`, "Unable to initialize DirectX 8.0 --
  Device creation failed!".
- **Hard requirements** (the "missing the following features" list): `Texture blending mode =
  Modulate`, `Source blending mode = SRCALPHA or BOTHSRCALPHA`, `Destination blending mode =
  INVSRCALPHA or BOTHINVSRCALPHA`, and 8 MB of video memory. We meet all of them.
- **Optional features:** "Detail textures enabled/disabled", "Trilinear filtering
  enabled/disabled", "Fogging", `AlphaTextureFormat`/`ARGB1555TextureFormat`/`NonAlphaTextureFormat`,
  and `-voodoo`/`-voodoo3` switches (strings; the switch table is in
  `LIB/RedFaction/README-FLEET.txt:56-58`).
- **Resolution.**
  - The launcher writes `Resolution Width/Height/Bit Depth` under **HKLM\SOFTWARE\Volition\Red
    Faction** (`Play Red Faction.bat:56-58`).
  - The game's per-user state lives under **HKCU\SOFTWARE\Volition\Red Faction**: `UpdateRate`,
    `Vsync`=0 in `install.reg:37-43`, and the card detection writes "Video Card 0 ..." there
    (`README-FLEET.txt:78-82`).
  - Both 11-08 sweeps measured 640x480, and the 09-29 scan-out was 640x480x16@85 (`PLAN:602`).
  - Conclusion (inferred): rf.exe reads its resolution from HKCU, so the FLEETRES values never land,
    and `Vsync=0` contradicts the user's vsync-on choice.
- **The 09-29 black frame** ("the game draws nothing visible through our D3D HAL", `PLAN:602`) went
  away by 11-08 with no recorded cause. It may have been fixed by the half-pixel, alignment or 16/16
  work, or it may be intermittent.
- **Risks when 32 bpp is turned on for it:** the X8R8G8B8 + D16 pairing (5.1). It also streams
  GeoMod changes into textures, which is the Lock path (inferred).

### 4.6 Rainbow Six (DX5/6 via DirectDraw)

- `RainbowSix.exe` imports DDRAW. It has the classic D3D error table and `CreateDirect3D()`,
  `CreateZBuffer() : AddAttachedSurface()`, `CheckForPalettized()`, "Create palette failed while
  loading surface (loadtex)", `Surface::SetTransparentValue`, `colorkey` (strings). `SoftwareOnly=0`
  and `FullScreen=1` are staged (`install.reg:83-87`). A software renderer is the fallback
  (`DevicePicker.exe`, `requires.json`).
- 11-08 fix: Rainbow Six took the deepest Z from the 32 bpp desktop's list (D24X8) and then switched
  to 16 bpp. `vcr_rt_format` now draws that pair as 16/16 in the Z's rows (`PLAN:163-174`).
- The 02:08 re-sweep still showed "RAINBOW SIX ERROR" (`SWP/sweep_1108_fixed/20261108_020841/sweep.json`).
  The intro+menu frame was taken at 02:40 on vcrdd 95678704 (`RainbowSix_intro_then_menu_z16in32.png`,
  commit `be58b7d`).
- Watch in game: colour-keyed textures or overlays (5.2), fog, the line overlays of its planning
  map in 3D (inferred).

### 4.7 Shogo (LithTech 1.0, DX6)

- `d3d.ren` strings:
  - "Couldn't find suitable 8 bit texture format", "Selected 8 bit texture format",
    `MasterPaletteMode` (`autoexec.cfg:25`);
  - "Using 1 pass lightmapping!" / "Using 2 pass lightmapping." / "Force1Pass doesn't work on this
    card";
  - "Src blend SRCCOLOR: %s, Dest blend SRCALPHA: %s", "Z-test: %s, Table fog: %s, Palette alpha: %s";
  - `EnvMapEnable`, `FogEnable`, `MipmapBias`.
- Detail config: `DetailHi.cfg` (`LightMap 1`, `EnvMapEnable 1`).
- Fallbacks exist for each (16-bit textures, 2-pass lightmaps).
- The VSA-100 can do P8 and the source-SRCCOLOR blend (`GR_BLEND_SAME_COLOR_EXT`, `g3ext.h:155`;
  3dfx's HAL advertises it on Napalm, `VH5/D3INIT.C:3131-3146`).

### 4.8 System Shock 2 and Thief Gold (Dark engine, DX6)

- Thief Gold's `lgd3d` is the DX6 build (`x:\prj\tech\libsrc\lgd3d\d6Render.cpp`,
  `d6Prim_s.cpp`, `d6Frame.cpp`; "Using D3D device: %s"; `d3d_driver_index`). SS2 is the same
  engine, but its game is inside the SafeDisc-encrypted `SHOCK2.ICD`.
- Neither tree stages a resolution (`LIB/SystemShock2/cam.cfg`, `LIB/ThiefGold/CAM.CFG`), so both
  run the engine default 640x480 (sweeps). Raising them is a staged `game_screen_size` once in-game
  rendering is proven.
- Thief Gold carries "Direct3d device driver does not accurately report texture memory usage".
  Our `dwVidMemTotal/Free` are set once (`vcrdd_ddraw.c:109-110`). If the runtime reports free
  texture memory from that, the Dark engine may warn or under-cache (inferred).
- Neither has been seen in game on any 3dfx card here. Both ignore WM_CLOSE (force-closed in every
  sweep).

### 4.9 Thief II (NewDark, D3D9 with a DX6 fallback)

- **Display path.** NewDark's D3D9 display (`use_d3d_display`, `cam_ext.cfg:11`) needs D3D32 and
  `D3DBigTex` bits 0+2. The launcher checks both in the registry (`Play Thief 2.bat:14-21`, regex
  `0x[57df]`) and otherwise moves `cam_ext.cfg` aside for the DX6 path. That path only ever reached
  640x480 (`FIND:466-470`).
- **Staged `cam_ext.cfg` values to reconsider for `.124`:**
  - `multisampletype 8` (`:211`): the user chose no anti-aliasing; our HAL offers none, so this
    relies on NewDark retrying without MSAA (inferred).
  - `tex_filter_mode 16` (`:149`) and `tex_filter_trilinear` (`:152`): neither aniso nor trilinear is
    advertised.
  - `d3d_disp_enable_distortionfx` (`:255`): render targets.
  - `d3d_disp_enable_atoc 1` (`:259`): alpha-to-coverage, an NVIDIA path.
  - `vsync_mode` is left at the default 3 = on (`:26-34`), which matches the user's choice.
- **Status.** Rendered a mission at 1280x960x32 on 09-28 (`FBS/thief2_d3d9_armed_4.png`: lit scene,
  alpha-tested fence and vines, HUD). That was before the half-pixel and alignment fixes.

### 4.10 Jedi Knight and Mysteries of the Sith (DX5 execute buffers)

- **Before the EXIT fix:** JK crashed in D3DIM. The DP2 stream of an execute-buffer title ends in
  `D3DOP_EXIT` (11), and answering COMMAND_UNPARSED to it sent D3DIM down a path that crashed
  (`vcrdd_d3d.c:1773-1782`).
- **After the fix:** JK played at 1280x960x16, and MotS ran 7,014 DP2 / 342k triangles / 0 unparsed
  (`PLAN:673-674`).
- **Texture filtering.** JK's frame is point-sampled (`FBS/jkdf2_d3d3.png`), and MotS's is bilinear
  (`FBS/mots_game.png`). Same engine, so most likely JK's own filtering option (inferred). If not,
  it is the legacy TEXTUREMAG/MIN mapping to `D3DTSS_MAGFILTER/MINFILTER`, which our HAL reads
  (`vcrdd_d3d.c:788-791`).
- **Menus** are 8 bpp DirectDraw (`LIB/JediKnightDF2/README-FLEET.txt:46-48`).
- **Inferred needs:** colour-keyed textures and automap lines.

### 4.11 Aliens vs Predator (DX5/6 via DirectDraw 6)

- **API and textures.** `avp.exe` imports DDRAW. It carries the execute-buffer error table and the
  Rebellion texture loader ("chosen %u-bit palettized texture format", "chosen %u-bit %u%u%u%u
  texture format", `AwSetD3DDevice() OK`, "alpha channel info is on a palettized format. This is
  not yet supported"). It has no software renderer (`requires.json`: Direct3D only).
- **Every earlier `.124` run** died on game data before a D3D device existed (`PLAN:199-232`).
- **The library is fixed** (`README-FLEET.txt:1-12`). The 11-12 seven-box proof did not include
  `.124` (`fix-avp/fleet/AliensVsPredator.json` boxes 110, 123, 133, 143, 171, 184, 240), so AvP is
  the next execute-buffer title to meet our HAL. It also needs the D3DOP_EXIT handling (inferred).

### 4.12 Battlefield 1942 (D3D8)

- **Blocked by DRM before D3D.** The client is stopped by SafeDisc 2.80 (`Mods\bf1942\Mod.dll`)
  before Direct3D starts. The dedicated server runs (11-08).
- **What the renderer needs** (`BF1942.exe` strings, `ENGINE\RendPCDX8`):
  - cube maps (`makeNormalizedHalfVectorCubeMap() Failed to create cubemap`);
  - stencil render states (`SStencilEnable/Func/PassOp`);
  - DXT1-5;
  - vertex processing chosen from `DBMHardware/Mixed/Software/PureDevice`.
- **Card warning.** On a card without T&L it warns "You might experinence very low performance and
  even crashes, Please note that Battlefield only officially supports cards that supports HTL"
  (the string ends with "does not support Hardware Transform and Lighting (HTL)"). That is probably
  a modal at every launch (inferred).
- **Depth.** It also offers a "32bit depth buffer" choice, which bears on 5.1.

---

## 5. Cross-title requirements and HAL gaps

### 5.1 X8R8G8B8 + D16: the runtime says yes, the HAL says no

The D3D8 runtime on `.124` reports `zmatch X8R8G8B8/D16 = 1` (`CAPS`). `ContextCreate` refuses a
32 bpp target with a 16-bit Z (`vcr_rt_format`, `vcrdd_d3d.c:1190-1197` -> DDERR_INVALIDPIXELFORMAT).
The VSA-100 has no 32-colour/16-Z mode (`vcr_rtfmt.h` header).

- A D3D8/9 title that picks D16 at 32 bpp gets a failed CreateDevice. Its message would be, for
  example, Red Faction's "Device creation failed!".
- None of the titles running today hits it. Titles that **could**:
  - Red Faction, once its 32 bpp setting actually lands (4.5);
  - BF1942 (offers a depth-buffer choice);
  - any D3D8 title on a fresh install with only D3D32 armed (inferred).
- A driver-side remedy (inferred design): back a D16 surface created while `rt32` is set with
  32-bit storage, as the reverse of the Rainbow Six 16/16 trick. Alternatively, make the runtime see
  the pair as non-matching.

### 5.2 Advertised but not done

| gap | where | titles exposed | hardware |
|---|---|---|---|
| lines and points dropped | caps `vcrdd_d3d.c:2063,2072`; walk `:1690-1715` | JK/MotS automap, R6 planning overlays, Halo, UE2 (all inferred); Thief II already avoids points (`cam_ext.cfg:179,182`) | yes: lines as thin triangles through the setup unit, or vendor `D6LINE.C` |
| texture colour key | `D3DPTEXTURECAPS_TRANSPARENCY` at `vcrdd_d3d.c:2035`; no `COLORKEYENABLE` handling | DX5/6 titles: JK, MotS (DDERR colour-key strings), R6 (`SetTransparentValue`), AvP (inferred) | yes: chromaKey register, as `VH5/D6DP2.C:6813-6860` (it tests after the filter, a known 3dfx limit) |
| validate | `vcrdd_d3d.c:1905-1910` | UT2003/2004 (MODULATE2X used though not in caps), Max Payne ("D3D_OK stages:%d") | n/a |

### 5.3 Not advertised; titles fall back, but the hardware could do it

| feature | titles that ask (evidence) | VSA-100 / 3dfx reference |
|---|---|---|
| MODULATE2X/4X | UT2003 (measured), UT2004 (same code); H&D, Halo FF, Max Payne (inferred) | `SST_CM_*_OUTSHIFT_2X/4X`; `VH5/D6MT.C:649-652` |
| other stage ops and argument modifiers | Halo, UE2, Max Payne (inferred) | combineMode INVERT/`X_MINUS_HALF` (`h3defs.h`); `VH5/D6MT.C:654-681` |
| Z bias | UE2 ("Supports Z biasing", `SetZBias`), Halo (`DecalZBiasValue`) | `SST_ENZBIAS` + `zaColor` (`VH5/D3RSTATE.C:1017-1022`) |
| W-buffer | none confirmed. Note `ZENABLE = USEW` disables depth (`vcrdd_d3d.c:986`) | `SST_WBUFFER` (`VH5/D3RSTATE.C:1114-1116`) |
| trilinear / LOD bias | UE2 (`UseTrilinear`, `DefaultTexMipBias`), RF, Thief II | `SST_TRILINEAR`, `SST_LOD_ODD/TSPLIT`, `SST_LODBIAS` (`h3defs.h:339, 719-722`) |
| MIRROR | Halo / UE2 (inferred) | `SST_TMIRRORS/T` (`h3defs.h:731-732`) |
| specular RGB | Halo, UE2 (inferred) | 3dfx advertises it (`VH5/D3INIT.C:2798-2801`) |
| dest alpha, separate alpha blend | NewDark, Halo (inferred) | Napalm 32 bpp (`VH5/D3INIT.C:3131-3156`) |
| P8, A8, L8, A8L8, X8R8G8B8 textures | Halo (format names), Shogo / AvP / R6 / H&D (palettized) | Glide base formats + `g3ext.h:158-168` |
| stencil | BF1942, Halo | Napalm 32 bpp (`VH5/D3INIT.C:3243-3255`) |
| projected texture coordinates | UE2 projectors (inferred) | per-TMU 1/W (`sOOW0/1`) (inferred) |
| cube maps | UE2, Halo, Max Payne, BF1942 | **no** (3dfx never advertised them: `VH5/D3INIT.C:3037-3038, 3469-3470`) |
| hardware T&L / shaders | every D3D8/9 title runs software VP; BF1942 warns | **no** (VertexShaderVersion 0, `CAPS`) |

### 5.4 Vsync (user choice: on)

- Our DirectDraw HAL advertises `DDCAPS2_FLIPNOVSYNC` and honours `DDFLIP_NOVSYNC`
  (`vcrdd_ddraw.c:102-105`), so a title that asks for immediate presents tears.
- Staged settings that ask for it:
  - UT2003/UT2004 `UseVSync=False` (`UT2003.ini:262`, `UT2004.ini:332`);
  - Red Faction `Vsync=0` (`install.reg:43`, HKCU).
- Thief II and Turok 2 stage vsync on (`cam_ext.cfg:26-34` default 3; `Data/config.ned:5`).
- These are library edits, per title or per 3dfx box. A driver-side "force vsync" switch would be
  the alternative (inferred design).

### 5.5 Performance items that every D3D title shares

1. **One chip.** No SLI for D3D. 3dfx's HAL promotes 3D surfaces to SLI (`VH5/DDSURF.C:452`,
   `VH5/DDFXNT.C:2406`). Fill-bound titles at 1280x960x32 (UT2003/4, Halo, Max Payne, H&D,
   Thief II) use a quarter of the board.
2. **PCI FIFO, per vertex.** `vertex()` writes 9-15 registers per vertex, behind `VcrDdRoom`
   (`vcrdd_3d.c:183-224`). The roadmap names the CMDFIFO move (`KMD/README.md:796`); 3dfx uses
   `VH5/CFIFO.C`.
3. **Lock drains the engine** on every surface lock (`vcrdd_ddraw.c:658`). This hits per-frame
   texture updates: Halo Bink, RF GeoMod, UE2 dynamic textures (inferred).
4. **16 bpp flip rate.** `.124` measured 46.9 / 42.1 flips/s at 16 bpp against 87.4 / 86.3 at 32 bpp
   (85 Hz). This is confounded and unexplained (`KMD/README.md:669-677`). The 16-bit D3D titles
   (Shogo, JK, MotS, R6, RF as staged, SS2, Thief Gold, AvP) may be capped at about half refresh.
5. **Software T&L** on the Athlon XP 2400+ (SSE, no SSE2). This is a hardware limit.

---

## 6. What this means for the plan (game-side view)

1. **Measure the requirements in game before changing the HAL.** The `log_combo` instrument
   already exists (event 513 what 21-23, first 64 per context, `vcrdd_d3d.c:882-919`). A supervised
   in-game run of each of the 15 titles at `LogLevel 2` turns the "(inferred)" stage-op entries
   above into measurements. A one-shot "unsupported state seen" log line per context, default off
   behind a Diag switch, would do the same for the other inferred gaps:
   - Z bias, W-buffer and flat shading;
   - colour key with a keyed texture bound;
   - stencil and specular;
   - D3DTA modifiers;
   - mirror/border addressing, LOD bias and projected texture coordinates;
   - line and point counts.
2. **MODULATE2X/4X first.** It is the only measured in-game defect, it affects two titles, and the
   hardware path and the vendor recipe are known. It needs a d3dprobe check: a stage-0 and a
   stage-1 MODULATE2X cell, comparing the locked back buffer with the computed values. That also
   makes `ValidateTextureStageState` honest for what is still not drawn.
3. **Decide the default for D3D32 and D3DBigTex.** Both have run armed on `.124` since 2026-09-28
   with d3dprobe 40/40, 42/42 and the 100/0 texture matrix (`PLAN:142-161, 619-620`). Seven titles
   need them. Rainbow Six, their one known victim, is fixed. The project rule says a default flips
   only after silicon proof, and that proof largely exists; the decision is the user's.
4. **Stability: Max Payne under the flight recorder, supervised.** This gates unattended D3D sweeps
   for everything else.
5. **Then the advertised-but-missing items (5.2), the Z/target pairing (5.1), and the
   hardware-capable features by how many titles ask (5.3).** Cube maps and T&L stay out: the
   hardware cannot.
6. **Library and config fixes, no driver change:**
   - UT2003/2004 `UseVSync=True` on the 3dfx box;
   - Red Faction resolution, depth and vsync into HKCU (after checking which hive rf.exe reads);
   - Thief II `multisampletype 0` for `.124`;
   - staged resolutions for SS2, Thief Gold, AvP and R6 once in-game rendering is proven.
7. **Performance last:** D3D SLI (non-AA, cfg-5 shape), CMDFIFO, scoped Lock waits, and the 16 bpp
   flip question. Measure with titles that have timedemos: Red Faction `-timedemo`/`-frameratetest`
   (`README-FLEET.txt:56-58`), Halo `-timedemo` (`halo.exe` switch table), and UE2's benchmark
   (inferred).

---

## 7. Evidence gaps (what nobody has looked at)

- **In game since the 11-08 fixes:** nothing. Before them: JK, MotS and Thief II only (09-28).
- **In game ever on a 3dfx card:** UT2003, UT2004, Halo, Max Payne, H&D, Red Faction, Rainbow Six,
  Shogo, SS2, Thief Gold, AvP, BF1942 have never been seen there.
- **Which Halo shader path** (`Shader=%s` in its info line) runs on `.124`.
- **Which registry hive** Red Faction reads its resolution from.
- **Why Red Faction's 09-29 frame was black,** and why it is not now.
- **Whether DOSBox `output=ddraw` still falls back on the current HAL,** and which DirectDraw call
  failed (`ddlab sdlddraw`).
- **The Max Payne freeze:** no ring capture exists.
- **The compat DB for `.124`** describes the GeForce2 GTS era for most titles. It needs re-recording
  as the in-game runs happen (`compat.py record`).
