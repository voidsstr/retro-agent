# Direct3D on the Voodoo 5 6000 (`.124`, vcr-kmd): everything already known

History report for the D3D planning run of 2026-11-12. Read-only: nothing was
built, edited or run on a fleet box for it. It collects what the docs, the git
history, the sweep evidence and the tests already say, and corrects the brief
where the record disagrees with it.

**Path shorthands used below**

| short | path |
|---|---|
| `WT` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench` |
| `KMD` | `WT/voodoo-cleanroom/vcr-kmd` |
| `PLAN` | `WT/docs/v56k-benchmark-plan.md` |
| `FIND` | `/home/voidsstr/development/retro-3dfx/FINDINGS.md` |
| `VH5` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` (vendor source, guidance only) |
| `RES` | `WT/scripts/benchmarks/results/v56k_lan_192.168.1.124` |
| `ONBOX` | `KMD/evidence/d3dplan_1112/onbox` - written **today, during this run, by the agent that owns `.124`**. It is uncommitted, in-progress evidence. It is labelled wherever it is used. |
| `HOSTEV` | `~/.retro-fleet/evidence/v56k-gametune_1001-fbshot_probe-20261004` (full-size PNGs on the dev host) |
| `LIB` | `/mnt/retro-share/Files/Games-Library` |

"(inferred)" marks a statement that comes from general DirectX or 3dfx
knowledge rather than from a file I read.

---

## 0. Summary

1. **What the HAL is.** It is a DX7-level NT DDI rasterizer: no T&L, pre-transformed
   vertices only (`KMD/display/vcrdd_d3d.c:1-30`). It drives **one** VSA-100,
   chip 0. Nothing in `vcrdd_d3d.c`/`vcrdd_3d.c` touches SLI. Every vertex is a
   register write through the **PCI FIFO**, with a status poll before each one
   (`KMD/display/vcrdd_3d.c:183-239`, `KMD/display/vcrdd_2d.c:92-108`). There is
   no CMDFIFO.
2. **What a fresh install gets.** 16 bpp targets, D16 Z, and 16-bit textures up
   to 256x256. Two switches add the rest: `Diag\D3D32` (32 bpp targets) and
   `Diag\D3DBigTex` (2048 textures, DXT1-5, A8R8G8B8). Both are default OFF
   (`KMD/README.md:110-111`), and both have been ARMED on `.124` since
   2026-09-28 by operator decision (`PLAN:619-620`, `PLAN:219-220`).
3. **What is proven on silicon.**
   - The d3dprobe default matrix: 40/40 fullscreen and 42/42 windowed, at
     16 bpp (09-26, 11-08) and 32 bpp (09-28).
   - The big-texture/DXT/decode matrices (09-28, 11-08).
   - Every swept title reaches its menu or intro.
   - **In-game** frames exist only for Jedi Knight DF2, Mysteries of the Sith
     and Thief II (D3D9 and DX6). All were taken on 09-28, before the 11-08
     fixes, and none was compared against a reference.
4. **New today (concurrent on-box session, `ONBOX/session_log.md`):**
   - **The X8R8G8B8 + D16 lead is real.** The runtime answers "match", our
     ContextCreate refuses the pair, and CreateDevice returns `8876086c`.
   - **At 32 bpp no triangle draws.** Clears pass; 9/31 fullscreen 1024x768x32,
     7/35 windowed. The HAL counted 36 triangles sent.
   - This came after a 4-chip Glide session that ended without
     HWCRLSEXCLUSIVE (a display reset), with `Reset3D` off. That is the exact
     hazard the README calls unproven.
5. **The HAL's known functional gaps:**
   - MODULATE2X/4X are drawn as MODULATE. UT2003 uses MODULATE2X in 7 of its
     26 logged set-ups.
   - D3DTA_ALPHAREPLICATE and D3DTA_COMPLEMENT are dropped.
   - Alpha always blends ONE/ZERO.
   - Lines and points are skipped, while line caps are advertised.
   - Flat shading, colour-keyed textures and specular are not implemented.
     Flat shading and colour key are still advertised.
   - Not offered at all: trilinear, LOD bias, Z bias, stencil, mirror/border,
     DESTALPHA and render-to-texture.
   - `ValidateTextureStageState` says "1 pass, OK" to everything.
6. **Max Payne, 2026-10-04.**
   - The game froze; the box did not. Num Lock was alive, and Windows key →
     Task Manager worked.
   - Normal-priority work crawled. The default fbshot was blocked on the
     display lock.
   - No flight-recorder ring was taken.
   - My own observation: the frozen frame lacks the menu text that the 09-29
     menu frame shows.
   - The cause is not established. Because of it, `.124` is "blk" for every
     Direct3D title in the 11-11 whole-fleet readiness rounds.
7. **Performance is unmeasured for games.** No D3D game has a frame rate on
   our HAL. The only numbers are synthetic: d3dprobe perf, 151 fps at
   640x480x16 with vsync off (≈30k triangles/s), and 84-85 fps with vsync.
   The only D3D game numbers on `.124` are AmigaMerlin's: UT99 D3DDrv at
   69 fps. The vendor driver ran D3D in 4-chip SLI by default; ours uses one chip.
8. **Tests.** No host test covers the state→register mapping (`color_path`,
   `blend_src/dst`, `cmp`, `stage1_combine`, `fvf_layout`, culling). d3dprobe
   has no check for any of:
   - alpha test, the blend-factor matrix, culling;
   - indexed or fan primitives;
   - wrap/clamp, bilinear;
   - TFACTOR, the BLEND*ALPHA ops;
   - render-to-texture, lost device.
9. **Default-off switches.** None of `DdHeapFloor`, `FlipDeadline` and
   `Reset3D` has ever run on silicon. `D3D32` and `D3DBigTex` are proven and in
   daily use. Three things block making them default: today's 32 bpp failure,
   the D16 refusal, and the 16 bpp stencil premise.

---

## 1. Corrections to the brief

| the brief says | the record says | evidence |
|---|---|---|
| "ONLY MENUS have been captured: no in-game 3D scene has been checked on this HAL" | In-game frames on vcr-kmd exist: JK DF2's first level at 1280x960x16, a MotS in-game frame, and Thief II in mission through NewDark's D3D9 display at 1280x960x32, plus Thief II's DX6 display at 640x480. All are from **09-28, before the 11-08 half-pixel and alignment fixes**, and none was compared with a reference renderer. So "checked for correctness" is still never true. | `RES/day/fbshots/jkdf2_d3d2.png`, `jkdf2_d3d3.png` (12:18-12:20), `mots_game.png` (12:24), `thief2_d3d9_armed_4.png` (23:12), `thief2_c2.png` (13:05); `git show c6a3e93` ("renders the first level (vcrctl fbshot)"); `FIND:467-472`; `PLAN:628`, `PLAN:673-674` |
| "Max Payne froze the box once" | **The game froze, not the box.** Num Lock was alive, and the user reached the desktop with the Windows key and opened Task Manager. `taskkill /f` at high priority ended the game in ~3 s, and the desktop came back at 1280x1024x32@85. | `KMD/evidence/gametune_1001/fbshot_probe/README.md:49-67`; `FIND:144-148` |
| D3DBigTex is "OFF by default ... not yet on silicon" (the README row) | Verified on `.124` on 09-28: tex512..2048, mip2048, texhigh, dxt1/3/5 and tex8888 pass (`git show 4ef226e`). The 11-08 big-texture/DXT matrix was 100/0 (`git show 481acfd`). It has run armed for six weeks since. The README row, the README Status/Roadmap and `tests/README.md:239` still say "NOT yet on silicon", and the roadmap still says UT2004 dies at 256x256. | `KMD/README.md:111`, `KMD/README.md:479-501`, `KMD/README.md:794-796` |
| X8R8G8B8 + D16: "an UNVERIFIED lead" | **Confirmed today.** `d3dprobe render --full 640x480x32 --zfmt d16` gives `CreateDevice 8876086c`, and the ring shows 513/12 "ContextCreate refused: 32 bpp target with a 16-bit Z on a VSA-100". The failed CreateDevice made the D3D8 runtime switch modes twice, 156 ms apart. | `ONBOX/session_log.md` (10:01:28 entry); `ONBOX/d3dprobe_render_640x480x32_d16.txt`; `ONBOX/ring_after_probe_i_decoded.txt` (seq 24713) |
| "AvP's failure was game data, fixed in the library on 2026-11-11" | The library fix landed on **11-12**: 154 InstallShield-obfuscated files were decoded (`3f3d1bd`) and deployed fleet-wide (`9bcb565`). The proof run lists 7 boxes, `.110 .123 .133 .143 .171 .184 .240`. **`.124` is not among them**, so AvP has never reached a D3D device on vcr-kmd. | `WT/.claude/evidence-readiness-2026-11-11/fix-avp/fleet/AliensVsPredator.json`; `git show --stat 3f3d1bd 9bcb565` |
| The 11-08 "re-sweep on the fixed driver" | It ran at 02:08 on `13256e46`, the alignment build. The Rainbow Six fix (`95678704`, the build now on `.124`) came at 02:41, and in that sweep Rainbow Six still errors. **`95678704` has had no full sweep.** The R6 fix rests on one image. | `KMD/evidence/d3dtune_1014/sweep_1108_fixed/run.out` (R6 "RAINBOW SIX ERROR" at 02:25); `git log -1 --format=%ad be58b7d` (02:41:09); `PLAN:170-176` |
| compat DB: H&D, Thief, Thief II, UT2004 and Shogo "verified" on `.124` | Those rows were measured on **2026-08-31**, when `.124` held a GeForce2 GTS (the V5 was first reported on 09-01, per CLAUDE.md "Driver Stack Map"). Only the Max Payne and System Shock 2 rows (09-29) are vcr-kmd measurements. Every row is flagged `stale`. | `python3 WT/scripts/fleet/compat.py status --box .124 --json` (fields `measured_at`, `stale`) |
| The 16-byte texture placement fix | It is on the **D3DBigTex path only**. `mipchain_ext` runs only when `pd->tex_ext` is set. On the default path, single-level textures keep DirectDraw's 4-byte placement. | `KMD/display/vcrdd_d3d.c:367-391`, `KMD/display/vcrdd_d3d.c:532-539` |

---

## 2. Timeline of the HAL: what changed, what it fixed, how it was checked

| date / commit | change | found by → verified by |
|---|---|---|
| 09-26 `56e0bc1` | first light: the DX7 NT DDI HAL over the triangle setup unit | 86Box Voodoo3, d3dprobe 26/26 windowed and fullscreen (`KMD/README.md:612-618`). Traps: walk the attach list (XP re-targets with SETRENDERTARGET by handle; fullscreen was 18/26 without it); trust `ddpfSurface` without `DDRAWISURF_HASPIXELFORMAT`; the TMU base for textures under 256 (`FIND:1231-1249`) |
| 09-26 `e74510a` | a mip chain is one block from DirectDraw's heap; `DDSCAPS_MIPMAP` advertised | Unreal Gold D3DDrv "DDERR_NOMIPMAPHW" (`KMD/README.md:628-633`) |
| 09-26 `719c267` | table fog on W; vertex fog through a synthetic 1/W | d3dprobe fogtable/fogvertex 6/6 on 86Box (`WT/tests/README.md:240`) |
| 09-26 `8c190d3` | stage 1 on TMU1; TMU cache flush after CPU writes | d3dprobe tex2add grey→green (`WT/tests/python/test_vcr_kmd_d3d.py:145-168`, `FIND:1214-1221`) |
| 09-26 `d9ca3a3` | VSA-100 `renderMode` (pixel size and write enables) written with every target | otherwise nothing drew (`FIND:1224-1227`) |
| 09-26 `d716d6f` | `SST_PARMADJUST` on every fbzColorPath | **silicon**: falling gouraud channels stayed constant; d3dprobe 40/40 at 640x480 and 1024x768 (`FIND:1152-1168`, `PLAN:1141-1142`) |
| 09-26 `85a17eb` + `DDCAPS2_FLIPNOVSYNC` | perf `--novsync` honoured | 85 → 151 fps (`PLAN:1097`, `KMD/display/vcrdd_ddraw.c:102-105`) |
| 09-26 `5be6a59` → 09-27 `64829cb`, `f55e2f9`, `0ea0e87` | 32 bpp targets (24+8 Z). They were armed for every windowed client, then hardened: `Diag\D3D32` default OFF, stencilMode=stencilOp=0 at 32 bpp only, Z/pitch checks, `Diag\Reset3D` | offline, then 86Box (`PLAN:797-798`, `KMD/README.md:459-477`) |
| 09-27 flip work `d0e6af7`, `556e2f9`, `f146917` | flip completion rule, counters, `Diag\FlipDeadline` | 86Box only (`KMD/README.md:503-546`, `KMD/README.md:559-564`) |
| 09-28 step 16 (`RES/night/step16_d3d32.sh`) | **D3D32 on silicon, one boot** | caps; `--noz` clear; 640x480x32 40/40; 1024x768x32 40/40; windowed on the 32 bpp desktop 42/42; 16 bpp 40/40; perf 84.2 fps at 85 Hz; H&D Deluxe starts (`KMD/evidence/silicon/d3d32_step16/step16_run.log`) |
| 09-28 `c6a3e93` | a DX5 execute buffer's `D3DOP_EXIT` (11) ends the DP2 walk with DD_OK | JK DF2 had crashed in D3DIM's fallback; it then rendered its first level (`PLAN:673`, `git show c6a3e93`, `WT/tests/python/test_vcr_kmd_dp2_exit.py:1-28`) |
| 09-28 `7d2779b`, `2991448` | `Diag\D3DBigTex`: TBIG to 2048, DXT1/3/5, A8R8G8B8, 26-bit texBaseAddr | UT2004 died at CreateTexture with the 256 limit (`FIND:768-772`) |
| 09-28 `4ef226e` | DXT2/DXT4 listed as DXT3/DXT5 hardware formats | Halo's menu drew white; it now renders textured. The d3dprobe big-texture list passes (commit message) |
| 09-28 `be6cdf0` | GDI gamma (`DrvIcmSetDeviceGammaRamp`) | Jedi Academy's ramp lands (`KMD/README.md:117`) |
| 11-08 `76d36e7` | `xy_bias = 0.5` (half-pixel centre, as `VH5/D3RSTATE.C` pixelOffset) | d3dprobe `rgb565_decode1` went from 1024/1024 wrong to 0 (`PLAN:129-140`) |
| 11-08 `481acfd` | every texture placed 16-byte aligned (D3DBigTex path); `log_combo` (513/21-23) | UT2003's DXT5 font pages sat at `...d6b4`; the menu now reads cleanly (`PLAN:142-160`, `KMD/evidence/d3dtune_1014/sweep_1108/UT2003_menu_{before,after}_align_fix.png`) |
| 11-08 `be58b7d` | `vcr_rt_format(rt32,16,32)` = 16: a 16 bpp target over a 32-bit Z draws 16/16 | Rainbow Six's DX6 Z pick; intro and menu (`PLAN:163-174`, `KMD/evidence/d3dtune_1014/sweep_1108_fixed/RainbowSix_intro_then_menu_z16in32.png`) |

The vcr-kmd build on `.124` was `vcrmp.sys fda1b8c9` with `vcrdd.dll b63ea6a5`
from the end of September until 11-08 (`PLAN:260-262`, `PLAN:139-140`). Since
11-08 it is `vcrdd.dll 95678704` (`PLAN:170`).

---

## 3. Open and known Direct3D problems: current state

### 3.1 New today (concurrent on-box session, uncommitted)

- **32 bpp: no triangle draws.**
  - Results: `d3dprobe render --full 1024x768x32` (D24X8) passes 9 and fails 31;
    windowed on the 32 bpp desktop it passes 7 and fails 35. Clears pass; every
    flat, gouraud, tex, blend and ztest check reads the clear colour
    (`ONBOX/d3dprobe_render_1024x768x32.txt`,
    `ONBOX/d3dprobe_render_windowed_desktop32.txt`, `ONBOX/session_log.md`).
  - The ring shows a valid context (`ContextCreate 1: target 300000 ... 32 bpp 3D, z 600000`)
    and `context 1 gone: 28 DrawPrimitives2, 36 triangles, 14 clears, 0 unparsed`
    (`ONBOX/ring_after_probe_ii_decoded.txt`, seq 24853 and 24952). The HAL
    wrote 36 triangles and none appeared.
  - Same configuration before: 40/40 on 09-28
    (`KMD/evidence/silicon/d3d32_step16/`). Windowed 32 bpp was 42/42 on 11-08
    (`PLAN:170-172`).
  - **The leading suspect, from the ring:** earlier in this boot, pid 0x764
    took HWCSETEXCLUSIVE and enabled 4-chip SLI at 75244 s. The session ended at
    75345.6 s with `SLI/AA off: display reset` (DrvAssertMode → RESET_DEVICE),
    and **no HWCRLSEXCLUSIVE was logged** (`ONBOX/ring_start_decoded.txt`, seq
    23973, 24424, 24470, 24518). `Reset3D` is off ("Glide 3D reset off",
    seq 24674).
  - The README names exactly this case as unproven: "a KILLED Glide client
    still needs a cold boot before a 32 bpp D3D test" (`KMD/README.md:112`,
    `PLAN:884-887`). Reset3D would not have run here even if armed: it runs
    only at the owner's own release, never at DrvAssertMode
    (`WT/tests/python/test_vcr_kmd_d3d.py:550-556`). The stale-owner warning
    also cannot fire across a PDEV swap (`FIND:93-97`).
  - Not proven. Separating it needs a cold boot with no Glide session, then the
    same run.
- **X8R8G8B8 + D16 refused** (see §1). The vendor HAL publishes D16 with
  `D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH` (`VH5/D3INIT.C:2550-2556`).
  It says outright that "Napalm cannot handle a different depth zbuffer and
  primary", and so allocates a 16-bit Z request as 32-bit at a 32 bpp primary
  (`VH5/DDSURF.C:592-626`). It also reallocates or substitutes at attach time
  (`VH5/DDSURF.C:2243-2315`). Our HAL refuses the pair instead
  (`KMD/include/vcr_rtfmt.h:79-87`). Its own comment expected the runtime to
  say "no match" (`KMD/include/vcr_rtfmt.h:186-191`,
  `KMD/display/vcrdd_d3d.c:2181-2185`, `KMD/tools/d3dprobe.c:1343-1346`), but
  the runtime answers yes (`KMD/evidence/d3dplan_1112/caps_124.txt`, zmatch).
  It already answered yes on 09-28 (`KMD/evidence/silicon/d3d32_step16/d3d32_caps_.txt`).
  **Any D3D8/9 title that picks D16 for a 32 bpp back buffer fails
  CreateDevice.** Which staged titles do that is unknown.

### 3.2 State → hardware gaps in the HAL

| gap | where | consequence | hardware |
|---|---|---|---|
| MODULATE2X/4X drawn as MODULATE, on stage 0 and stage 1 | `KMD/display/vcrdd_d3d.c:714-716`, `:861-862` | UT2003 used op 5 (MODULATE2X) in 7 of 26 logged set-ups and op 16 (BLENDCURRENTALPHA) once (`KMD/evidence/d3dtune_1014/sweep_1108/UT2003_combos_unaligned.txt`, "ops:" lines). Surfaces come out half as bright (inferred), and op 16 is wrong. | the VSA-100 combine extension has an output `shift` (`WT/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/glide3/src/distate.c:887-896`); D3D mapping inferred |
| `D3DTA_ALPHAREPLICATE` / `D3DTA_COMPLEMENT` dropped | `ARG_KIND` = `a & D3DTA_SELECTMASK` (`KMD/display/vcrdd_d3d.c:671`, `:678-681`) | documented as a gap (`PLAN:127-128`, `FIND:43-44`). d3dprobe was changed to avoid it (`KMD/tools/d3dprobe.c:735-737`) | supported (inferred) |
| separate alpha blend factors ignored: alpha is always ONE/ZERO | `KMD/display/vcrdd_d3d.c:995-999` | at 32 bpp, destination alpha receives source alpha unblended | separate `AM_ASRC`/`AM_ADST` fields exist (same lines) |
| lines and points parsed and skipped | `KMD/display/vcrdd_d3d.c:1690-1715` | nothing draws for `LINELIST(_IMM)`, `LINESTRIP`, the indexed line ops or `POINTS` | inferred: drawable as thin triangles |
| `D3DRENDERSTATE_SHADEMODE` never read (0 references in `vcrdd_d3d.c`) | — | flat-shaded geometry is gouraud-shaded | vendor implements `shadeMode` (`VH5/D3RSTATE.C:491`, `:569`) |
| colour-keyed textures (`COLORKEYENABLE`) not implemented (0 references) | — | DX5/6 key texels draw opaque (inferred) | 3dfx: "Napalm doesn't support texture chroma key", then re-enabled the cap "for WHQL" (`VH5/D3INIT.C:143-145`, `:99-100`); vendor has `colorKeyEnable` (`VH5/D3RSTATE.C:520`, `:613`) |
| specular not drawn; the specular colour is read only for vertex fog | `KMD/display/vcrdd_3d.c:202-209` | no specular highlights | vendor advertises SPECULARGOURAUDRGB (`VH5/D3INIT.C:2798-2801`) and handles render state 29 (`VH5/D3RSTATE.C:596`) |
| no trilinear (MIPFILTER LINEAR → nearest level), no `D3DTSS_MIPMAPLODBIAS`, no `MAXMIPLEVEL`, anisotropy 1 | `KMD/display/vcrdd_d3d.c:777-791`, `:2036-2040`, `:2160` | mip banding and no LOD control | vendor advertises MIPFLINEAR (`VH5/D3INIT.C:2847`) |
| no ZBIAS | not in `prim_caps` (`KMD/display/vcrdd_d3d.c:2019-2020`) | decals z-fight (inferred) | vendor biases through zaColor (`VH5/D3RSTATE.C:1017-1022`, `:1268-1274`) |
| no stencil; D24S8 listed on D24X8's storage, stencil never written | `KMD/include/vcr_rtfmt.h:33-52` | stencil shadows/effects off or wrong (inferred) | vendor advertises 8 stencil ops on Napalm (`VH5/D3INIT.C:3241-3259`) |
| address modes WRAP and CLAMP only (no MIRROR or BORDER); `D3DRENDERSTATE_WRAP0..7` never read | `KMD/display/vcrdd_d3d.c:792-795`, `:2044-2045` | MIRROR is not offered | vendor offers MIRROR (`VH5/D3INIT.C:2874`) |
| DESTALPHA/INVDESTALPHA not advertised (DestBlendCaps `0x3f`), though `blend_src/dst` map them | `KMD/display/vcrdd_d3d.c:2025-2031`, `:636-637`, `:659-660` | no destination alpha at 32 bpp | vendor advertises INVDESTALPHA (`VH5/D3INIT.C:3155`) |
| `ValidateTextureStageState` always answers 1 pass, DD_OK | `KMD/display/vcrdd_d3d.c:1905-1910` | an application that validates (D3D7/8 `ValidateDevice`) is told every combination works; then MODULATE2X, op 16 and the rest degrade silently | — |
| P8 textures not offered; `SETPALETTE`/`UPDATEPALETTE` skipped | `KMD/display/vcrdd_d3d.c:1716-1724`, `:1557`; `caps_124.txt` `texP8: 0` | titles fall back to 16-bit formats (inferred) | vendor lists P8/ALPHAPALETTE (`VH5/D3INIT.C:2819`) |
| `D3DDP2OP_STATESET`, `ZRANGE`, `VIEWPORTINFO`, `WINFO` skipped by size | `KMD/display/vcrdd_d3d.c:1552-1567` | state blocks and the W range are ignored; whether the runtime relies on them for a DX7 HAL is unknown (inferred risk) | — |

### 3.3 Surfaces and memory

- **Default path, single-level textures.** These are not driver-placed:
  `VcrDdD3dCreateMipChain` handles MIPMAP chains only
  (`KMD/display/vcrdd_d3d.c:532-539`). The 11-08 alignment bug can therefore
  still hit a default-path (switch-off) single-level 16 bpp texture that
  DirectDraw places at `...4`. The test pins only `mipchain_ext`
  (`WT/tests/python/test_vcr_kmd_d3d.py:746-757`).
- **Default path, texBaseAddr width.** The default path writes the Voodoo3's
  24-bit texBaseAddr, so a texture above 16 MB is sampled 16 MB lower.
  `.124`'s heap is 27 MB (`KMD/README.md:494-497`). This is latent on every
  VSA-100 without `D3DBigTex`.
- **Render-target textures.** They keep DirectDraw's placement
  (`KMD/display/vcrdd_d3d.c:387-391`). Render-to-texture has never been tested.
  Halo runs with `DisableRenderTargets` for `0x121a:0x0009`
  (`LIB/Halo/config.txt:27`, `:115-118`). Without it Halo's FATAL dialog 105 is
  "render-target creation failing" (`FIND:435-442`).
- **The heap's offset-0 block.** The block at offset 0 is lost: the heap
  manager answers 0 for "no memory" (`KMD/README.md:118`). Since 481acfd
  **every** D3DBigTex texture goes through the single-pass
  `HeapVidMemAllocAligned`, and a 0 return there is reported as
  `DDERR_OUTOFVIDEOMEMORY` (`KMD/display/vcrdd_d3d.c:441-446`). `DdHeapFloor`
  would cure that, but it has never run on silicon (§6).

### 3.4 Performance and transport

- **One chip.** D3D renders on chip 0 only (no SLI reference in
  `vcrdd_d3d.c`/`vcrdd_3d.c`). The vendor's 6000 defaults to D3D
  `SSTH3_SLI_AA_CONFIGURATION="5"` ("Fastest", 4-chip SLI) and reaches SLI from
  its D3D path at 3D-surface creation (`PLAN:1528-1530`, `PLAN:1534-1550`;
  `Compute_SLIAA_Config` at `VH5/DDFXNT.C:3571`, called from `VH5/DDSURF.C:452`).
- **The transport.** Every vertex is up to 15 PCI-FIFO writes preceded by a
  `VcrDdRoom` status poll (`KMD/display/vcrdd_3d.c:188`, `:225-239`). That is
  about 50 MMIO writes per triangle (my count from the code). The roadmap still
  lists "The CMDFIFO instead of PCI-FIFO writes" (`KMD/README.md:796`).
- **The only throughput number.** d3dprobe perf at 640x480x16 with `--novsync`,
  300 frames of 200 textured triangles plus a clear: 151.2 / 151.1 / 150.9 fps,
  about 30.2k triangles/s
  (`KMD/evidence/silicon/vcrkmd-v5-6.jsonl`, `vcrkmd-v5-7.jsonl`,
  `vcrkmd-256-1.jsonl`; `KMD/tools/silicon_battery.py:256-258`). That workload is far too small to
  stand for a game.
- **Texture streaming.** It is a CPU write plus a TMU flush per dirty texture
  per draw (`KMD/display/vcrdd_d3d.c:1287-1335`). Halo's Bink intros play ~10x
  slow on vcr-kmd, "texture Lock path is the first suspect"; the launcher
  passes `-novideo` where vcrmp runs (`FIND:446-450`,
  `LIB/Halo/Play Halo.bat:4-6`).
- **16 bpp DirectDraw flips at half rate on `.124`.** ddlab measured 46.9/42.1
  flips/s at 85 Hz. It is unresolved and confounded, and is not the completion
  rule (`KMD/README.md:669-682`, `FIND:1004-1014`). d3dprobe perf at 16 bpp
  with vsync held 85.0 fps (`KMD/evidence/silicon/vcrkmd-v5-4.jsonl`,
  `-5.jsonl`; these runs predate FLIPNOVSYNC).

### 3.5 Per title (`.124`, vcr-kmd)

| title | state | evidence |
|---|---|---|
| Max Payne 1.05 | renders its menu at 1280x960 (09-29); froze once (10-04, §4); attended runs only; `blk` in every readiness round | `PLAN:599`, `KMD/evidence/lan_20260929_sweep/maxpayne_dt_menu_1280x960.png`, `WT/docs/lan-multiplayer-status.md:70`, `:82-83` |
| Rainbow Six | fixed 11-08 (16-over-32 Z, only with rt32); one image; never re-swept on `95678704`; compat `untested` | `PLAN:163-174`; `KMD/evidence/d3dtune_1014/sweep_1108_fixed/run.out` |
| UT2003 | menu text fixed (alignment). It still runs MODULATE2X/op 16 combos as MODULATE. In-game never seen. Compat `untested`. | `PLAN:142-160`; §3.2 |
| UT2004 | intro and menu render (needs D3DBigTex); in-game never seen | `PLAN:107`, `KMD/evidence/d3dtune_1014/sweep_1108_fixed/20261108_020841/Unreal_Tournament_2004_60s.png` |
| Halo 1.10 | textured menu (DXT2/4 alias); `DisableRenderTargets`; `-novideo`; "Gameplay needs a person"; `blk` in readiness | `PLAN:627`, `PLAN:675`, `FIND:435-450` |
| Thief II (NewDark) | the D3D9 display needs D3D32 plus D3DBigTex bits 0+2 (the launcher tests them); in mission on 09-28; ignores WM_CLOSE | `LIB/Thief2/Play Thief 2.bat:10-26`, `RES/day/fbshots/thief2_d3d9_armed_4.png`, `FIND:467-472` |
| Hidden & Dangerous Deluxe | needs 32 bpp (the launcher forces `Display bitdepth` 32); the menu renders over a 3D scene; compat "stops at profile creation" | `LIB/HiddenAndDangerous/Play Hidden and Dangerous Deluxe.bat:22`, `FIND:730-736`, sweep `Hidden_and_Dangerous_Deluxe_60s.png` |
| Jedi Knight DF2 / MotS | D3DOP_EXIT fix; in-game frames from 09-28; menus/crawls 11-08 | `git show c6a3e93`, `PLAN:673-674` |
| Red Faction | black frame 09-29 (the sweep says the game was **not focused**); menu 11-08 (focused); the cause of the black frame was never established | `KMD/evidence/lan_20260929_sweep/sweep_part2.json` (Red Faction `keys_skipped`), `KMD/evidence/lan_20260929_sweep/redfaction_fbshot.png` (extrema 0/0), `PLAN:108` |
| Shogo | intro logo at 30 s; the 60 s frames are near-black (11-08 mean 2.0, 09-29 mean 1.6); the menu itself is not clearly evidenced; compat (GeForce2 era) "client menu renders intermittently" | `sweep_1108_fixed/run.out`, `KMD/evidence/lan_20260929_sweep/sweep_part2.log:60-61` |
| System Shock 2, Thief Gold | menus at 640x480; WM_CLOSE ignored (force-closed) | `sweep_1108_fixed/run.out` |
| AvP | the library is fixed fleet-wide (11-12); never run on `.124` since | §1 |
| BF1942 | SP: SafeDisc 2.80 (not ours); the LAN host runs dedicated; join needs a partner | `PLAN:109-111` |

### 3.6 DirectDraw items the D3D titles depend on

- **The DirectDraw HAL caps** are `DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_READSCANLINE`
  and `DDCAPS2_FLIPNOVSYNC`. There is no stretch blit and no overlay
  (`KMD/display/vcrdd_ddraw.c:96-105`).
- **Flips** are latched at retrace, and `DDERR_WASSTILLDRAWING` is returned
  until then (`KMD/README.md:663-668`).
- **The DirectDraw-only 2D titles** render their menus: RA2/YR at 1024x768,
  TS, StarCraft and WC2 (fbshot only; the 8 bpp DirectDraw surface is black to
  GDI) (compat rows; `FIND:692-695`).
- **Descent's DOSBox `output=ddraw` fallback to 640x400** is unexplained.
  `ddlab sdlddraw` was built to name the failing step and was never run on
  silicon (`KMD/README.md:591-610`; no `sdlddraw` file under `KMD/evidence`).

### 3.7 Tooling and documentation drift

- **`vcrctl fbshot` (default path)** never finishes under a D3D exclusive app.
  Every escape runs under win32k's display lock. `--probe` works but crawled
  under Max Payne (`KMD/evidence/gametune_1001/fbshot_probe/README.md:1-31`,
  `KMD/README.md:156`). The rule is "never run vcrctl/ExtEscape tools while a
  D3D game runs".
- **`lan_sweep.py` frames are the agent's GDI SCREENSHOT**
  (`WT/scripts/benchmarks/lan_sweep.py:8-21`). For a flip-chain game it may
  return either buffer (inferred). Verdicts are "alive, no error window,
  board healthy".
- **The kill switches `Diag\D3D`** (default 1; 0 = no D3D HAL) **and
  `Diag\TexPortFlush`** (default 1) exist (`KMD/miniport/vcrmp.c:234-235`,
  `KMD/include/vcr_ioctl.h:110-111`). Neither is in the README Diag table.
- **Stale documentation:**
  - the README "Status (2026-09-27)" header (`KMD/README.md:277`);
  - the D3DBigTex row and its Status paragraph (§1);
  - roadmap item 6 on UT2004 (`KMD/README.md:794-796`);
  - `WT/tests/README.md:239`;
  - the `test_vcr_kmd_bigtex.py` docstring, all of which still say D3DBigTex is
    "not on silicon";
  - the event table for 513, which omits what 4/7/8/10/11/13/14/21-23 that the
    code emits (`KMD/include/vcr_events.h:75` against `KMD/display/vcrdd_d3d.c:905-918`, `:1731`, `:1854`).

---

## 4. The Max Payne freeze of 2026-10-04

### 4.1 What was running

- **Driver:** vcr-kmd `vcrmp.sys fda1b8c9`, `vcrdd.dll b63ea6a5`. This is
  before the half-pixel, alignment and 16-over-32 fixes (`PLAN:260-262`,
  `PLAN:139-140`). `Diag\D3D32`=1 and `Diag\D3DBigTex`=7 were armed
  (`PLAN:570`, `PLAN:219-220`).
- **The game:** Max Payne 1.05 at its main menu, Direct3D at 1280x960x32.
  The probe JSON reads `w 1280 h 960 fmt 3`, desktop layer, scan-out start
  `004b0000`, stride `1400`
  (`KMD/evidence/gametune_1001/fbshot_probe/fbab_maxpayne.json`). It runs under
  SafeDisc 2.51, satisfied by DAEMON Tools 3.47 (`PLAN:599`; compat row
  `MaxPayne`).
- **The tools:**
  - the new `vcrctl.exe` (`185cdacd`): `fbshot` on its default (escape) path,
    then `fbshot --probe` through `vcrprobe.sys` (`babc0d26`, started with
    `sc start`);
  - EXECW through the agent; `wmic`; `taskkill`, both WM_CLOSE and `/f`
    (`KMD/evidence/gametune_1001/fbshot_probe/README.md:15-25`, `:49-67`, `:84-89`).
- **Before it:** on 09-29 the same title on the same build sat at its menu
  under a default `fbshot` that "never finishes" and was killed cleanly.
  WM_CLOSE still exited the game (`FIND:332-333`, `PLAN:599`).

### 4.2 What the evidence shows

| step | result | evidence |
|---|---|---|
| default `fbshot` (ExtEscape → DrvEscape under the display lock) | printed nothing in 25 s; EXECW killed the process tree | `fbab_maxpayne.json` `esc` |
| `fbshot --probe`, normal priority | completed in ~120 s (1.4 s on the desktop); the BMP was written at box time 23:56:04 as EXECW gave up | `fbab_maxpayne.json` `probe`; `README.md:45-47` |
| `fbshot --probe` under `start /high` | ~38 s | `README.md:39`, `FIND:143` |
| `wmic` | no answer within 40 s | `README.md:53` |
| `taskkill` (WM_CLOSE) | did not return within 60 s; two `taskkill` processes left waiting | `README.md:54-55` |
| the two probe frames, minutes apart | pixel-identical (I re-checked: `ImageChops.difference` bbox None) | `HOSTEV/fbab_maxpayne_probe.png`, `HOSTEV/fbab_maxpayne_probehigh.png` |
| the GDI SCREENSHOT after them | differs only in a 285x184 box around the cursor (bbox 640,480-925,664) | `HOSTEV/maxpayne_gdi_screenshot.png` |
| the user at the box | "max payne appear locked but num lock is working"; Windows key → desktop → Task Manager | `README.md:60-64` |
| `taskkill /f` at high priority | ended the game in ~3 s; DISPLAYCFG 1280x1024x32@85; no vcrctl left running | `README.md:65-67` |
| flight-recorder ring | **not captured** (no ring file in the evidence directory or the commit) | `KMD/evidence/gametune_1001/fbshot_probe/` listing |

### 4.3 A detail the write-ups do not mention

The 09-29 capture of the same menu shows the menu items (Resume Game, Tutorial,
New Game, Load, Save, Options, Credits, Quit) and the status line
(`KMD/evidence/lan_20260929_sweep/maxpayne_dt_menu_1280x960.png`). **The frozen
10-04 frame has the background art and the cursor, but no menu text and no
status line** (`HOSTEV/fbab_maxpayne_probe.png`; the contact sheet
`KMD/evidence/gametune_1001/fbshot_probe/fbshot_probe_contact.jpg`, bottom-right).
So the last frame the game presented was not a complete menu frame. Either it
stopped presenting before or while drawing its text, or the menu items had not
yet faded in (inferred; I do not know Max Payne's menu animation). Whichever it
was, the image is consistent with the game being stopped **before the
capture**. It does not fit well with a freeze *caused* by the second capture.

### 4.4 Hypotheses and how each would show

| # | hypothesis | for | against | signature to look for |
|---|---|---|---|---|
| H1 | the default fbshot's queued wait for the display lock, behind a CPU-bound game: a priority inversion or lock convoy (the README's leading suspect) | the escape blocked 25 s; on NT the escape takes the display lock (`fbshot_probe/README.md:10-13`, `:70-74`) | on 09-29 the same blocked escape did not freeze the game (`README.md:73-74`); §4.3 suggests the game had already stopped | a freeze **only** in runs that issue the escape |
| H2 | CPU starvation by the game: it holds the CPU above normal priority | everything at normal priority crawled; `start /high` was 3x faster; `/f` at high priority was instant (`README.md:51-58`) | this explains slowness, not a frozen picture | the game's priority class and CPU share; whether the game itself is still running frames |
| H3 | the probe's ~1.2 M uncached video-memory reads while the game draws | listed in `README.md:75` | the frame was already static | a freeze only in runs that use `--probe` |
| H4 | a HAL stall: `VcrDdRoom`/`VcrDd2dSync` exhaust `SPIN_CAP` (2,000,000 status reads, "~a second") and `give_up()` clears `g2d_ok`. From then on `w.drawable` is false and every DP2 draws nothing until the next PDEV. | `KMD/display/vcrdd_2d.c:71`, `:83-108`, `:118-133`; `KMD/display/vcrdd_d3d.c:1859-1860`. That gives a frozen picture with the game alive, and taskkill + mode set brings the desktop back (a new PDEV). | no ring was read | ring event **512 a=9** ("2D engine FIFO never drained / never went idle", ERROR); 513 a=2 counters with triangles > 0 but nothing on screen |
| H5 | a game-side hang or a SafeDisc/DAEMON stall, or a deadlock between the game's Lock/Flip loop and our Dd_Flip/Lock (WASSTILLDRAWING polling through `IOCTL_VCR_VBLANK`, inferred) | `README.md:76`; Flip answers WASSTILLDRAWING until retrace (`KMD/README.md:663-668`) | — | 511 a=12/13 flip counters at session end: flips stop and no long vblank read means a game hang; a huge "longest wait" or "longest read" means a flip/IOCTL stall |
| H6 | a correctness bug in the old build (`b63ea6a5`, before 11-08) | three D3D bugs were fixed after it | none of them is known to hang | run on `95678704` |

### 4.5 The experiment that separates them

Attended (the person at the box watches and reports animation, mouse and
highlight), on `95678704` with the switches as they are now. One variable per
run:

0. **Setup** (one boot, read at boot). Set `Diag\LogLevel`=2 and a larger
   `LogEntries` (`KMD/miniport/vcrmp_log.c:279-282`). At the default
   LogLevel 3 the per-DP2 DEBUG events (513 what 8, 14) wrap the 1024-entry
   ring in seconds of play (inferred from the event rate; today's two probe
   runs used ~400 entries, `ONBOX/ring_start_decoded.txt` against
   `ring_after_probe_ii_decoded.txt`). Start from a **cold boot with no Glide
   session** (§3.1).
1. **E0, no capture at all.** Launch through `LIB/MaxPayne` "Play Max
   Payne.bat". At the menu take only agent `PING`/`PROCLIST`/`WINLIST` for 10
   minutes: no EXEC, no SCREENSHOT, no vcrctl. Record MaxPayne.exe's priority
   with one `wmic process ... get Priority,ThreadCount` *before* any problem.
   Quit with WM_CLOSE (taskkill without `/f`), **then** `vcrctl log`.
   - A freeze here means the tools are acquitted: H1 and H3 are out.
   - The ring then decides between H4 (512 a=9), H5 (511/12-13: flips stopped,
     or a long wait/read) and a game hang.
2. **E1, `--probe` only**, at `start /high`, every 2 minutes for 10 minutes.
   This tests H3.
3. **E2, the escape** (the default fbshot) once, 25 s timeout, then the same
   at `start /high`. This tests H1, and separates a priority inversion
   (completes at high priority) from a deadlock (never completes). It knowingly
   breaks the "no vcrctl under a D3D game" rule, so do it last and only
   attended.
4. **Repeat E0 three times** before calling it clean. A freeze seen once in
   about four sessions (09-29, 10-04) needs repetition.

Result so far: `.124` is `blk` for Halo, UT2003, UT2004, Shogo, MotS, Rainbow
Six, Red Faction, both Thiefs, SS2, Max Payne, AvP and H&D in the 11-11
whole-fleet readiness rounds. The stated reason is the Max Payne freeze
(`WT/docs/lan-multiplayer-status.md:40-75`, `:82-83`).

---

## 5. What is verified, and how

### 5.1 d3dprobe: every test (`KMD/tools/d3dprobe.c`)

Method: one HAL device, `D3DCREATE_SOFTWARE_VERTEXPROCESSING`, a lockable back
buffer (`:1452`, `:1469`). After EndScene the **top-left 256x256** of the back
buffer is locked and compared, before Present (`:292-323`, `:362-367`). The
default list is `clear,flat,gouraud,gouraudb,tex,modulate,blend,ztest,bigtex,present,fogtable,fogvertex,tex2mod,tex2add`
(`:1247`). That is 42 checks windowed and 40 fullscreen, because `present` is
windowed only.

**Default tests**

| test | what it checks | lines |
|---|---|---|
| `clear` | colour Clear at two points, then a second clear | `:918-925` |
| `flat` | an untextured quad inside, outside and at the edges (rasterization rule) | `:926-934` |
| `gouraud` / `gouraudb` | interpolated corners; `gouraudb` rotates them so a FALLING channel is tested (PARMADJUST) | `:935-954` |
| `tex` / `modulate` / `bigtex` | a 64 or 256 checker, MODULATE with white or grey diffuse, point sampling, four cells incl. the last | `:955-981` |
| `blend` | SRCALPHA/INVSRCALPHA 50 % red over blue | `:1121-1131` |
| `ztest` | LESSEQUAL with Z write: far hidden, near drawn | `:1132-1149` |
| `present` | windowed: the clipped blit to the primary, read through GDI | `:1033-1057` |
| `fogtable` / `fogvertex` | linear table fog on W at rhw 1, 1/3, 1/8; vertex fog in specular alpha | `:1097-1120` |
| `tex2mod` / `tex2add` | two stages: stage 1 MODULATE or ADD on TMU0 over TMU1 | `:1058-1096` |

**Explicit tests**

| test | what it checks | lines |
|---|---|---|
| `mip` | 64x64 chain, point mip, levels 0-2 by texel:pixel ratio ("the check to run on silicon"; no run is on record) | `:982-1032`, `:47-50` |
| `tex512`/`tex1024`/`tex2048` | 8x8-cell R5G6B5 at that size; last cell green | `:1150-1155`, `:590-611` |
| `tex8888`, `tex8888_256/_512/_512x128` | A8R8G8B8 cells | `:1156`, `:1196-1201` |
| `dxt1`/`dxt3`/`dxt5`, `dxt1_512`, `dxt5_512`/`_512x256`/`_512x128`/`_256x128` | solid-block DXT cells; logs LockRect's pitch | `:1158-1171`, `:1202`, `:482-521` |
| `*_def` (`dxt5_512x128_def`, `dxt5_128_def`, `tex8888_512x128_def`) | the same in a DEFAULT-pool texture | `:1172-1182` |
| `*_upd` / `*_copy` | a SYSTEMMEM texture copied into a DEFAULT one by UpdateTexture or CopyRects | `:1183-1195`, `:526-562` |
| `dxt1/3/5_decode`, `rgb565_decode` | every texel of varied blocks against a reference decoder; the alpha pass goes through the blender, not ALPHAREPLICATE | `:1230-1235`, `:690-800` |
| `*_decode1` | the same, one texel a pixel (found the half-pixel bug) | `:1204-1210` |
| `dxt5_big_a/b/c` | 1:1 windows of a 512x256, 512x512 or 1024x512 DXT5 | `:1211-1229` |
| `mip2048` | 2048 chain, levels 0-4 around TBIG's 256 base | `:807-859` |
| `texhigh`, `texhigh_dxt5` | ~20 MB of fill textures first, so the probe texture lands above 16 MB | `:867-912` |

**Other modes**

- `caps`: D3DCAPS8, formats, `zmatch`, `hal_fullscreen`; no device, no switch
  (`:1323-1402`).
- `perf`: 200 linear-filtered textured triangles and a clear per frame
  (`:1497-1532`).
- Flags: `--noz`, `--zfmt`, `--novsync`, `--pace` (`:1266-1290`).
- The runner: `KMD/tools/d3dprobe_run.py`. It uploads the probe, gates the mode
  against the EDID, paces switches and cleans up after a run that did not end
  civilly. An explicitly named test that is skipped counts as a failure
  (`:1-34`).

### 5.2 Runs on record

| date | where / build | run | result | evidence |
|---|---|---|---|---|
| 09-26 | 86Box Voodoo3 | render, windowed and full | 26/26, then 38/38 | `KMD/README.md:617`, `FIND:1212` |
| 09-26 | `.124`, 16 bpp | render full 640x480 and 1024x768 | 40/40 | `PLAN:1141-1142`; `KMD/evidence/silicon/vcrkmd-v5-*` |
| 09-26 | `.124`, 640x480x16 | perf `--novsync` | 151 fps (85.0 in v5-4/v5-5, before FLIPNOVSYNC) | `KMD/evidence/silicon/vcrkmd-v5-6.jsonl`, `vcrkmd-v5-7.jsonl`, `vcrkmd-256-1.jsonl` |
| 09-27 | 86Box, integration build `5e4ca36` | render; refusals | 42/0 windowed, 40/0 full (640x480x16, 800x600x16); a 32 bpp device and 16 bpp + D24S8 refused by the D3D8 runtime from the caps | `KMD/README.md:557-568`, `KMD/evidence/86box_v3/int_20260927/04_d3d/` |
| 09-28 | `.124`, D3D32 armed | caps, `--noz` clear, 640/1024x32 full, windowed 32, 640x16, perf | 3/3, 40/40, 40/40, 42/42, 40/40, 84.2 fps | `KMD/evidence/silicon/d3d32_step16/` |
| 09-28 | `.124`, D3D32 + BigTex=7 | tex512..2048, mip2048, texhigh, dxt1/3/5, tex8888 | pass (commit text only; raw output not committed) | `git show 4ef226e` |
| 11-08 | `ef30672a` | decode tests, `*_upd/_copy`, render | rgb565_decode1 1024/1024 → 0, render 42/42 (raw output not committed) | `git show 76d36e7`; `PLAN:121-140` |
| 11-08 | `13256e46` | render + big/DXT including `dxt5_big_*` | 42/42 and 100/0 | `git show 481acfd` |
| 11-08 | `95678704` | render windowed and full 640x480x16 | 42/42, 40/40 | `PLAN:170-172` |
| 11-12 | `.124` now (concurrent, uncommitted) | 640x480x32 with D16; 1024x768x32; windowed 32 | CreateDevice refused; 9/31; 7/35 | `ONBOX/` |

### 5.3 Title sweeps on record

`lan_sweep.py`: GDI frames at 30 and 60 s, WM_CLOSE, `glideprobe --noopen`.

| sweep | what it covered | evidence |
|---|---|---|
| 09-28 07:05 | 7 titles with D3D32 armed: H&D, JK and MotS PASS; Thief II, R6 and UT2004 errored ("Critical Error" for UT2004, before BigTex) | `KMD/evidence/silicon/d3d32_step16/step16_run.log` |
| 09-29 parts 1 and 2 | the full desktop sweep, 53 titles in part 2 | `KMD/evidence/lan_20260929_sweep/sweep_part{1,2}.log`, `sweep_part2.json` |
| 10-14 baseline | AvP only | `KMD/evidence/d3dtune_1014/sweep_baseline/` |
| 11-08 00:48 and 02:08 | 15 and 13 shortcuts | `KMD/evidence/d3dtune_1014/sweep_1108*/` |

In none of them was the agent lost or the board dead. Every verdict is
menu- or intro-level.

### 5.4 Host tests that pin D3D behaviour (all in `WT/tests/run_all.sh`)

**Python**

- `WT/tests/python/test_vcr_kmd_d3d.py`, 25 source/compile tests:
  - the attach-list walk (`:40`), HASPIXELFORMAT (`:49`), the target re-read
    per call (`:57`);
  - every DX7 opcode parsed or handed to the runtime (`:66`), and the FPU
    bracket (`:85`);
  - the setup-unit protocol (`:94`), handle cleanup (`:101`), the mip-chain
    block (`:106`), fog (`:128`), the texture flush (`:145`);
  - float vertex colour (`:171`), PARMADJUST (`:189`);
  - `vcr_rt_format` compiled and asserted, including `(1,16,32)=16` and
    `(1,32,16)=0` (`:207-253`);
  - the 32 bpp stencil writes (`:314`), the Z list (`:348`), D3D32 default off
    (`:373`), target validation (`:392`), offset 0 (`:434`);
  - the Glide release reset (`:473`), register offsets against the GPL header
    (`:582`);
  - the d3dprobe `--noz` / `--zfmt` / runner (`:628`, `:656`, `:692`);
  - half-pixel (`:735`) and 16-byte placement (`:746`).
- `test_vcr_kmd_bigtex.py` (9): the switch read at boot (`:45`), VSA-100 only
  (`:59`), off = the proven path (`:70`), the chain view (`:110`), write-back
  (`:127`), the TMU nopCMD before compressed state (`:142`), TEXBLT bounds
  (`:151`), register codes (`:161`), probe/README (`:179`).
- `test_vcr_kmd_dp2_exit.py` (2) and `test_vcr_kmd_dxt24.py` (3).
- `test_vcr_kmd_integration.py`: every Diag default pinned (`:60-106`); moving
  one requires a `MOVED_DIAG` entry with evidence (`:86-91`). It also pins the
  fbshot probe.
- `test_vcr_kmd_ddraw.py`: flip, scanline, heap floor, `zsurf`, `sdlddraw`.
- `test_newdark_display.py`: the Thief II launcher.

**Native (true-source)**

- `WT/tests/native/test_vcr_kmd_d3dseq.c`: target sequences write for write,
  the Z fit, the reset, its idle and CMDFIFO gates (TESTs at `:93-430`).
- `test_vcr_kmd_texlod.c`: bases against 3dfx's GPL tables, 357 cases (`:23-342`).
- `test_vcr_kmd_fog.c`: the 1/W index, `exp`, tables, register packing (`:15-60`).
- `test_vcr_kmd_ddheap.c` (`:33-110`) and `test_vcr_kmd_flip.c` (`:457-1257`).

---

## 6. Test-coverage gaps

### 6.1 HAL behaviour with neither a d3dprobe check nor a host test

| behaviour | code |
|---|---|
| alpha test (`ALPHATESTENABLE/ALPHAFUNC/ALPHAREF` → `AM_ATEST`) | `KMD/display/vcrdd_d3d.c:992-994` |
| every blend factor except SRCALPHA/INVSRCALPHA and SRCALPHA/ZERO: ONE/ONE, DESTCOLOR/ZERO, SRCCOLOR, BOTH*, SRCALPHASAT, and the ONE fallback for unsupported ones | `:630-663` |
| Z functions other than LESSEQUAL; ZWRITEENABLE off; ZENABLE `D3DZB_USEW` (which turns depth **off**: only `D3DZB_TRUE` enables it) | `:986-990` |
| culling CW/CCW (`SM_CULL`, `SM_CULL_NEGATIVE`); d3dprobe always sets CULLNONE (`KMD/tools/d3dprobe.c:1493`) | `:1023-1026` |
| the odd-triangle winding swap for strips and indexed strips (untested without culling) | `:1621-1633`, `:1659-1677` |
| primitive paths: TRIANGLELIST, TRIANGLEFAN, INDEXEDTRIANGLELIST(2), INDEXEDTRIANGLESTRIP/FAN, TRIANGLEFAN_IMM (d3dprobe draws only through DrawPrimitiveUP) | `:1612-1689` |
| address modes CLAMP/WRAP beyond [0,1]; INDEPENDENTUV | `:792-795` |
| bilinear MIN/MAG (only perf uses it, unchecked); the `mip` 64 chain on silicon | `:788-791` |
| TFACTOR (`c1`), BLENDTEXTUREALPHA, BLENDDIFFUSEALPHA, SELECTARG2, DISABLE, stage-0 ADD, alpha ops other than SELECTARG1 | `:686-741`, `:1020` |
| the stage-1 approximation "diffuse applied after" (tex2mod uses white diffuse) | `:845-848` |
| EXP/EXP2 table fog on silicon (the arithmetic is native-tested only); fog with no W | `:1007-1016` |
| dithered 16 bpp output (d3dprobe turns dither off, `KMD/tools/d3dprobe.c:1495`) | `:979-980` |
| sub-rectangle clears (every probe clear is full-target) | `:1408-1421` |
| texture-filled TEXBLT on silicon, beyond `*_upd` | `:1499-1549` |
| render-to-texture (SETRENDERTARGET to a texture, then sampling it) | `:1725-1751` |
| a mode change or alt-tab under a live D3D context (lost device) | — |
| X8R8G8B8 + D16 (now run once, today, by hand) | `KMD/include/vcr_rtfmt.h:84-85` |
| `ValidateTextureStageState` / `ValidateDevice` answers | `:1905-1910` |
| lines and points (would fail today) | `:1690-1715` |
| `log_combo` output (event 513/21-23 content) | `:882-919` |

### 6.2 Limits of d3dprobe itself

- **It reads only the top-left 256x256** of the back buffer (`:292-323`). A
  fault in the rest of a 1280x960 target cannot be seen.
- **The textures land where the test's allocation pattern puts them.** The
  11-08 alignment bug passed every solid-cell test because "its textures
  happened to land aligned" (`FIND:30-33`). Only `log_combo` placement logs
  from a real game showed it.
- **Solid-colour cells hide decode and placement bugs** (`FIND:40-43`). Only
  the `*_decode` family compares every texel.
- **`perf`'s workload is tiny.** It neither represents a game nor measures fill
  rate.
- **It uses D3D8 only.** DX5/6/7 interfaces (execute buffers, DrawPrimitive,
  DX6 texture handles, colour key) are exercised only by games. JK's
  `D3DOP_EXIT` crash was found that way.

### 6.3 Source-text tests versus behaviour tests

Most of `test_vcr_kmd_d3d.py` asserts that particular lines exist. **No test
executes the state → register translation** (`color_path`, `blend_src/dst`,
`cmp`, `stage1_combine`, `fvf_layout`, `compute_regs`). These sit in the DDI
file, which needs Microsoft's DDK headers, and those are not committed
(`WT/tests/python/test_vcr_kmd_ddraw.py:91-97`). `vcr_rtfmt.h`, `vcr_3dseq.h`
and `vcr_texlod.h` show the existing pattern for host-testable logic:
Win32-free headers.

---

## 7. The default-off Diag switches: what remains before each could be default-on

| switch | read | on silicon? | what remains |
|---|---|---|---|
| **`D3D32`** (32 bpp targets, D24X8/D24S8) | boot (`KMD/README.md:110`, `:124-125`) | **yes**: 09-28 step 16, 40/40 and 42/42; armed and used daily since (Halo, Thief II D3D9, H&D, UT2004 at 1280x960x32) | (1) **Today's 32 bpp "no triangles" after an unreleased Glide session** (§3.1) must be explained. If it is leftover Glide state, a reset that covers a killed or display-reset Glide end is needed before default-on: Reset3D does not cover that end. (2) X8R8G8B8 + D16: the runtime promises it and we refuse it. A default-on D3D32 exposes every 32 bpp D3D8/9 client to it; the vendor's answer is a 32-bit aux buffer (`VH5/DDSURF.C:592-626`). (3) The README's blockers (`KMD/README.md:789-796`): R6 is now fixed (be58b7d) and Thief II works. Still open: Reset3D after Glide is unproven, and the box is a LAN-party machine. (4) The 16 bpp stale-stencil premise is unproven (`KMD/include/vcr_rtfmt.h:39-44`, `PLAN:888-892`). (5) Moving the default needs a `MOVED_DIAG` entry (`WT/tests/python/test_vcr_kmd_integration.py:86-91`) and the `test_d3d32_is_a_default_off_switch` change (`WT/tests/python/test_vcr_kmd_d3d.py:373-389`). |
| **`D3DBigTex`** (bit 0 to 2048, bit 1 DXT1-5, bit 2 A8R8G8B8, 26-bit texBaseAddr) | boot (`KMD/README.md:111`) | **yes**: 09-28 (`4ef226e`), 11-08 (`481acfd`); armed since 09-28 | Raw probe outputs were never committed. The README and tests README still say "not on silicon". The README's open questions are the DXT LockRect pitch and the texture-port write-back (`KMD/README.md:498-501`); the decode tests cover the first in practice, and the write-back is excluded by design (`WT/tests/python/test_vcr_kmd_bigtex.py:127-140`). **Arguments for defaulting at least part of it:** the default path's 24-bit texBaseAddr bug above 16 MB (`KMD/README.md:494-497`), and the 16-byte placement, which exists only on this path (§3.3). It shares a risk with DdHeapFloor: every texture goes through the single-pass allocator. |
| **`DdHeapFloor`** (heap starts one page up) | every mode change (`KMD/README.md:118`) | **no**: host and 86Box only; no `ddlab vidmem` "on" run and no `sdlddraw` run on `.124` | the A/B in the README: `ddlab vidmem` should report 121 with the lowest block at +4 KB; then `sdlddraw` for Descent. Now more relevant to D3D because of `mipchain_ext` (`KMD/display/vcrdd_d3d.c:441-446`). |
| **`FlipDeadline`** (achieved-refresh deadline) | every mode set (`KMD/README.md:114`, `:513-525`) | **no**: 86Box only, and the bed's ~3 ms per-frame overhead hides the effect (`KMD/README.md:559-564`) | first ddlab `vblank_hz` against `refresh_mhz`/1000 (well under 1 %), then the `--work-us 12353` A/B (expect 0.889x → 0.95x of the refresh, `PLAN:870-874`); "stays OFF until counters from real titles justify it" (`KMD/README.md:523-525`). No title's 511/12-13 counters are on record. |
| **`Reset3D`** (Glide 3D state reset at the owner's release) | boot (`KMD/README.md:112`) | **no**: no 604 a=3 event in any committed evidence; step 16 skipped it | the `PLAN:884-887` run (604 a=3 c=1 after a clean Glide session). **Even proven, it does not cover a killed Glide client or a display-reset end** (`WT/tests/python/test_vcr_kmd_d3d.py:550-556`), and that is today's case (§3.1). |
| `Diag\D3D`, `Diag\TexPortFlush` (kill switches, default ON) | boot | — | document them in the README table |

---

## 8. Evidence gaps

1. **No D3D game frame rate on vcr-kmd.** The only D3D game numbers on `.124`
   are AmigaMerlin's UT99 436 D3DDrv: 69.11 fps at 1024x768x16, 69.5 at
   800x600x16, 68.1 at 800x600x32, with exits at 1280x960 and above
   (`WT/scripts/benchmarks/results/v56k_titles_192.168.1.124/results.csv`,
   `PLAN:91`). `v56k_bench.py` already has a `ut:d3d` lane (`PLAN:65`).
2. **No checked in-game scene since the 11-08 fixes.** The 09-28 in-game frames
   were never compared with a reference (AmigaMerlin's D3D or the D3D8 RefRast).
   Gameplay has never been seen for Halo, UT2003/UT2004, H&D, Red Faction,
   Shogo, SS2, Thief Gold, R6, Max Payne, AvP or BF1942.
3. **No soak.** The longest D3D runs on record are about one minute (JK DF2)
   and the 30/60 s sweep samples. The GL and Glide titles got 20-30 minute MP
   soaks (`PLAN:695-700`).
4. **No ring for the Max Payne freeze**, and no ring per title from the sweeps.
5. **Raw d3dprobe outputs** for the 09-28 BigTex runs and the 11-08 decode and
   big-DXT runs are only described in commit messages.
6. **The compat DB's D3D rows for `.124` are GeForce2-era** (2026-08-31). The
   11-08 results were never recorded there (MaxPayne and SS2 rows are 09-29;
   RainbowSix and UT2003 still say `untested`).
7. **AvP on `.124` after the library fix:** never run.
8. **No `log_combo` capture for any title except UT2003.** The facility exists
   (INFO 513/21-23, first 64 set-ups per context) and would show which
   unsupported ops each title uses.
9. **No measurement of D3D CPU cost** (status polls per vertex, FPU
   save/restore per DP2) on the 2 GHz Athlon XP.
10. **No 4-chip D3D at all**, so nothing compares with the vendor's default SLI
    D3D.
11. **The 16 bpp flip half rate** has no per-frame counters from silicon
    (`KMD/README.md:799`).
12. **Today's 32 bpp failure** has no cold-boot control yet (in progress in the
    concurrent session).

---

## 9. What the history implies for the plan, in order (from the record only)

1. **Settle today's 32 bpp regression first.** Cold boot, no Glide, the same
   1024x768x32 run. If it passes, the fix is a reset that covers a killed or
   display-reset Glide end: the class `Reset3D` and `SliOffAaCtrl` address,
   wired at a point that sees that end. Every 32 bpp title depends on it.
2. **Run the Max Payne E0-E2 experiment** (§4.5). It unblocks `.124` from
   every D3D readiness round.
3. **Match the runtime on Z.** Accept X8R8G8B8 + D16 the vendor's way (a
   32-bit aux buffer), or stop the runtime from promising it.
4. **Close the caps-versus-behaviour lies:** MODULATE2X/4X and op 16 (UT2003
   uses them), ALPHAREPLICATE, separate alpha blend, lines/points, flat shade,
   colour key, `ValidateTextureStageState`. Pair each with a d3dprobe check and
   a host test of the mapping, extracted Win32-free.
5. **Take the first D3D frame-rate numbers** (UT99 D3DDrv first, against
   AmigaMerlin's 69 fps), then decide on transport (CMDFIFO) and SLI.
6. **Re-sweep `95678704`.** Record the results in the compat DB, commit the
   probe outputs, and fix the README/test drift.

---

## Appendix: evidence index

| what | where |
|---|---|
| caps today | `KMD/evidence/d3dplan_1112/caps_124.txt` |
| caps 09-28 (identical except the texture size and formats) | `KMD/evidence/silicon/d3d32_step16/d3d32_caps_.txt` |
| step 16 (D3D32 on silicon) | `KMD/evidence/silicon/d3d32_step16/`, `RES/night/step16_d3d32.sh` |
| 09-28 in-game frames | `RES/day/fbshots/{jkdf2_d3d2,jkdf2_d3d3,mots_game,thief2_d3d9_armed_4,thief2_c2}.png` |
| 09-29 sweep and fbshots | `KMD/evidence/lan_20260929_sweep/` |
| Max Payne / fbshot probe | `KMD/evidence/gametune_1001/fbshot_probe/`, `HOSTEV/` |
| 11-08 sweeps, UT2003 combos and fix images | `KMD/evidence/d3dtune_1014/` |
| AvP library fix | `WT/.claude/evidence-readiness-2026-11-11/fix-avp/` |
| readiness rounds (`.124` blk) | `WT/docs/lan-multiplayer-status.md:40-89` |
| today's on-box session (concurrent) | `ONBOX/session_log.md`, `ONBOX/d3dprobe_*.txt`, `ONBOX/ring_*_decoded.txt` |
| vendor Z/stencil/SLI references | `VH5/D3INIT.C:2550-2556`, `:3155-3171`, `:3241-3259`; `VH5/DDSURF.C:592-626`, `:2243-2315`; `VH5/D3RSTATE.C:491-624`, `:1017-1022`; `PLAN:1528-1553` |
