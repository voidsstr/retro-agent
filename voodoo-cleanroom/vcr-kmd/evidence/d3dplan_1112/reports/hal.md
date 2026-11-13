# vcr-kmd Direct3D HAL audit: every gap, and the plan to close them (2026-11-12)

Lane: **clean-room** (`vcr-kmd`, our own DX7-level NT D3D HAL in `vcrdd.dll`).
This was a read-only investigation. Nothing was edited, built or deployed, and no box was contacted.
The only file written is this report.

Path aliases used below:

| alias | path |
|---|---|
| `KMD` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/vcr-kmd` |
| `VH5` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` (3dfx's own W2K/XP V5 HAL; guidance only) |
| `W9X` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/Win9x` |
| `GLIDE` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5` (our h5 Glide fork, its `incsrc/h3defs.h` is the register bible) |
| `ONBOX` | `KMD/evidence/d3dplan_1112/onbox` (today's session by the agent that owns `.124` during this run. **Read, not produced, by this audit.**) |

"(inferred)" marks a statement that comes from general DirectX DDK / 3dfx knowledge, not from a file read here.
All `vcrdd_d3d.c` line numbers are `KMD/display/vcrdd_d3d.c` unless another file is named.

---

## 0. Headline: what matters, in order

1. **A Glide client that was KILLED leaves its command FIFO enabled. That silently erases every D3D triangle, and the HAL never notices.** The on-box agent measured this today on `.124` with the deployed driver.
   - Clears still work and every triangle vanishes: fullscreen 1024x768x32 scored 9 pass / 31 fail in the probe's RESULT (`ONBOX/d3dprobe_render_1024x768x32.txt`); the session log counts it as "13 ok / 29 FAIL".
   - A windowed run then tripped the 2D idle check (`status 0x00000a1f = SST_BUSY + SST_CMD0_BUSY`). Acceleration went OFF for the desktop PDEV, and the D3D HAL disappeared (`GetDeviceCaps` failed with `8876086a`) until a mode set.
   - After a mode-set engine reset the same probe was 40/40.
   - Evidence: `ONBOX/session_log.md:7-12`, `ONBOX/regs/regs_after_windowed_1009.txt` (`0x80024 = 0x1fe`: `cmdFifo0.baseSize` enable bit 8 set; readPtr stuck at `0x2f624`).
   - Nothing in our code turns off an *idle but enabled* CMDFIFO. The miniport resets the engine only when it is BUSY at a mode set (`KMD/miniport/vcrmp_hw.c:568-572`), and that reset is the only place the FIFO is stopped (`vcrmp_hw.c:190`). `Diag\Reset3D` *refuses* to run while the FIFO is on (`KMD/include/vcr_3dseq.h:176-183`).
   - The windowed device's surfaces (0x40000, 0x80000, a texture at 0xc0000) sat **inside** the stale FIFO window 0x18000-0x117000 (`ONBOX/session_log.md:9`). A FIFO that later fetches from memory D3D has overwritten would execute garbage. That makes it a credible freeze mechanism (inferred).
   - **This is fix #1, before any further game work.**
2. **The HAL programs only the legacy subset of the 3D state it depends on.**
   - It never writes:
     - `combineMode` on the FBI, TMU0 or TMU1, nor the 12-NOP exit from 2-pixels-per-clock;
     - `clipLeftRight1`/`clipBottomTop1` (`SST_ENRECTCLIP1`);
     - `chromaKey`/`chromaRange`, stipple, `aaCtrl` or `chipMask`.
   - Evidence: `KMD/display/vcrdd_3d.c:45-129` and `KMD/include/vcr_3dseq.h:56-78`.
   - 3dfx's own V5 HAL writes `combineMode` first, at every state flush (`VH5/D6DP2.C:6404-6452`). Glide's extended combine and stencil "outlive even a clean close" (`vcr_3dseq.h:80-86`).
   - The only cleaner is `Diag\Reset3D`. It is off by default, runs only at an owner's own release, and does not cover clip1, chroma or stipple (`vcr_3dseq.h:185-198`).
3. **A dead engine is silent to Direct3D.**
   - When a FIFO or idle wait gives up, `g2d_ok = 0` (`KMD/display/vcrdd_2d.c:83-132`).
   - From then on every DrawPrimitives2/Clear2 returns `DD_OK` and draws nothing (`vcrdd_d3d.c:1859, 1869, 1887, 1901`). `ContextCreate` never checks the engine (`vcrdd_d3d.c:1163-1199`).
   - The game keeps running behind a frozen picture. That matches Max Payne's 2026-10-04 symptom: "frozen with Num Lock alive", with two probe frames minutes apart pixel-identical (`/home/voidsstr/development/retro-3dfx/FINDINGS.md:144-148`).
4. **A 32 bpp target with a D16 Z is refused, although the runtime says the pair matches.** This is now confirmed on silicon.
   - `CheckDepthStencilMatch(X8R8G8B8, D16)` = yes (`KMD/evidence/d3dplan_1112/caps_124.txt`, `zmatch`).
   - `ContextCreate` refuses the pair (`KMD/include/vcr_rtfmt.h:84-85`), so `CreateDevice` returns `D3DERR_INVALIDCALL`.
   - The D3D8 runtime's cleanup then made **two mode switches 156 ms apart** on the 1998 CRT (`ONBOX/session_log.md:6`).
   - 3dfx allocated a requested 16-bit Z as 32-bit on a 32 bpp primary (`VH5/DDSURF.C:592-626`).
5. **The texture combine implements a small fraction of Direct3D.**
   - Argument modifiers (COMPLEMENT, ALPHAREPLICATE) are masked off (`vcrdd_d3d.c:671`).
   - TFACTOR as a second argument becomes diffuse (`699-703`).
   - The BLEND* ops assume the texture is arg1 (`708-713`).
   - MODULATE2X/4X and every other op become MODULATE (`714-716`, `862`).
   - Stage 1 is applied *before* stage 0's diffuse (`954, 977`).
   - `ValidateTextureStageState` answers "1 pass, OK" to everything (`1905-1910`), so games never fall back.
   - The VSA-100's extended combine (`combineMode`: OUTSHIFT 2x/4x, 1-x and x-0.5 inverts, alpha selects; `GLIDE/incsrc/h3defs.h:385-530`) does all of this, and 3dfx's HAL used it (`VH5/D6MT.C:512-693`).
   - The first in-game frame ever captured on this HAL (UT2004 botmatch, `ONBOX/games/UT2004_botmatch/UT2004_botmatch_090s.png`, luma 28) is very dark and has coloured block noise on several surfaces. The darkness is consistent with lost MODULATE2X lighting (inferred); the noise is unexplained (see hal-23).
6. **The hardware can do several things that 3dfx's own V5 HAL shipped and ours ignores or under-reports.** ZBIAS, the W-buffer, specular, texture colour key, trilinear filtering and LOD bias, MIRROR addressing, stencil at 32 bpp, lines and points, the luminance/alpha texture formats, and per-channel colour write masks (section 9).
7. **Performance is bounded by the transport, not the chip.**
   - Every register is a PCI PIO write.
   - Every vertex waits on a status read (`KMD/display/vcrdd_3d.c:188`, `vcrdd_2d.c:92-108`).
   - Strips and fans are sent as independent triangles (`vcrdd_d3d.c:1621-1689`).
   - Flip waits for the engine to go idle (`KMD/display/vcrdd_ddraw.c:334`).
   - All of this runs under win32k's display lock. That fits the "everything at normal priority crawled while Max Payne ran" note (`FINDINGS.md:138-143`, inferred link). And D3D draws on one chip of four.

---

## 1. What was read

- **The HAL, in full:**
  - `KMD/display/vcrdd_d3d.c` (2236 lines) and `KMD/display/vcrdd_3d.c` (307)
  - `KMD/display/vcrdd_3d.h` and `KMD/display/vcrdd.h`
  - `KMD/display/vcrdd_ddraw.c` (745) and `KMD/display/vcrdd_2d.c` (520)
  - the D3D-relevant parts of `KMD/display/vcrdd.c` (PDEV lifetime, `DrvAssertMode`) and `KMD/display/vcrdd_escape.c` (exclusive set/release)
- **Headers and common code it uses:** `KMD/include/vcr_3dregs.h`, `vcr_3dseq.h`, `vcr_rtfmt.h`, `vcr_texlod.h`, `vcr_fog.h`, `vcr_flip.h` (head); `KMD/common/vcr_texlod.c`, `vcr_fog.c`; `KMD/compat/d3dnthal.h` (a stub). The build's real DDK header is `KMD/out/ddi/d3dnthal.h` (opcode values `:513-576`, records `:578-760`).
- **The miniport:** `KMD/miniport/vcrmp_hw.c:170-205, 560-575` (engine reset) and `KMD/miniport/vcrmp_dd.c` (flip/vblank IOCTLs).
- **Vendor, for comparison:**
  - `VH5/D3INIT.C` (caps and texture formats)
  - `VH5/D3RSTATE.C` (state functions)
  - `VH5/D6DP2.C` (DP2 walker, state emission)
  - `VH5/D6MT.C` (multitexture)
  - `VH5/D3FOG.C`, `VH5/DDSURF.C` (Z allocation)
  - `VH5/D7DP2.C` (state blocks)
  - `VH5/CFIFO.C` (head)
  - `W9X/DX/D3D/D3TRI.C` (specular)
  - `GLIDE/incsrc/h3defs.h`, `GLIDE/glide3/src/gtex.c:1195-1225`, `distate.c:1940-1975, 2165-2195`
- **Evidence and docs:**
  - `KMD/README.md` (Diag table `:88-125`, Status `:425-575`, Findings `:575-660`, Roadmap `:758-800`)
  - `docs/v56k-benchmark-plan.md` (resume points 2026-11-08, 10-15, 09-28/29)
  - `/home/voidsstr/development/retro-3dfx/FINDINGS.md` (D3D and Max Payne entries)
  - `KMD/evidence/d3dplan_1112/caps_124.txt`
  - `ONBOX/*` (session log, register dumps, rings, probe outputs, UT2004 frames)
  - `KMD/tools/d3dprobe.c` (test list)
  - `tests/python/test_vcr_kmd_d3d.py` (what is already pinned)

---

## 2. Stability and robustness (the gate for everything else)

### R1. A killed Glide client's command FIFO stays on, and D3D draws into the void (hal-01)

This is the cause of today's failure.

- **What happened** (`ONBOX/session_log.md:7-12`):
  1. A 4-chip OpenGL/Glide session (pid 1892) ran about 2 hours earlier and left `cmdFifo0` enabled with a stuck read pointer (`ONBOX/regs/regs_after_windowed_1009.txt`: `0x80024 = 0x000001fe`, `0x8002c = 0x0002f624` on two reads).
  2. `Diag\Reset3D` was off (`ONBOX/ring_start_decoded.txt`, event 513/16 "Glide 3D reset off").
  3. The next fullscreen D3D device was created fine: its clears passed and every triangle disappeared.
  4. The next windowed device made the PCI-FIFO idle check time out on `SST_CMD0_BUSY` and gave up the 2D engine. The desktop then drew in software, and the HAL was withdrawn: `VcrDdD3dHalInfo` returns early when `!pd->g2d_ok` (`vcrdd_d3d.c:2058-2059`).
- **Why the code lets it happen:**
  - The display driver's target/state writers (`vcrdd_3d.c:45-129`) and the 2D engine (`vcrdd_2d.c`) never read `cmdFifo0.baseSize`.
  - The miniport stops the FIFO only from `VcrHwResetEngine`, and calls that only when the chip is **busy** at a mode set (`vcrmp_hw.c:564-572`).
  - `Diag\Reset3D` explicitly writes nothing while `SST_CMDFIFOEN` is set (`vcr_3dseq.h:141-148, 176-183`; `vcrdd_2d.c:400-428`), and runs only at an owner's own release (`vcrdd_escape.c:184-196`).
- **Risk beyond lost triangles (inferred):** D3D surfaces were placed inside the stale FIFO's window (`ONBOX/session_log.md:8-9`). A fetch resuming over them would execute texture or colour data as commands. This is a candidate for the Max Payne freeze (R6).
- **Fix:**
  1. *Miniport:* at every mode set with no Glide exclusive owner, write `cmdFifo0.baseSize = 0` (and `cmdFifo1`) whether or not the chip is busy, logged as a phase. The existing engine reset stays as it is for the busy case.
  2. *Display DLL:* at `ContextCreate` (and on the first DP2 of a context), if `exclusive_pid == 0` and `cmdFifo0.baseSize & SST_CMDFIFOEN`, refuse to draw. Log the stale-FIFO event once, and ask the miniport (new IOCTL, reusing `VcrHwResetEngine`) to stop it.
  - Both go behind one new switch (proposed `Diag\D3DSafe` bit 0), default off until proven.
- **Verify:**
  - Reproduce on purpose: start a Glide title, kill it, run `d3dprobe render --full --res 1024x768 --bpp 32`. It should fail without the switch and pass with it.
  - Native test: the decision function, asserting the old behaviour (no write when idle) and the new one (write when the FIFO is enabled and there is no owner).
  - Python test: the switch is default-off.

### R2. The HAL does not own all the 3D state it depends on (hal-02)

- **What each writer covers:**
  - `vcr_3d_target_seq` writes renderMode, stencilMode/stencilOp (32 bpp only), the colour/aux buffer addresses and strides, and clip rect 0 (`vcr_3dseq.h:56-78`).
  - `VcrDd3dState` writes fbzColorPath, fbzMode, alphaMode, fogMode, fogColor, c0, c1, sSetupMode, the fog table (cached), and the TMU0/TMU1 textureMode/tLOD/tDetail/texBaseAddr (`vcrdd_3d.c:74-129`).
- **Never written by the HAL:**
  - `combineMode` on FBI/TMU0/TMU1 (0x208), and the 2PPC exit NOPs
  - `clipLeftRight1`/`clipBottomTop1` (0x200/0x204; enable bit 31 `SST_ENRECTCLIP1`, `GLIDE/incsrc/h3defs.h:1008-1009`; Glide writes it with guard-band clipping on by default, `GLIDE/glide3/src/distate.c:2181-2187`, `gpci.c:1872`)
  - `chromaKey` (0x134) / `chromaRange` (with `SST_ENCHROMARANGE`/`SST_ENCHROMAKEY_TMU`, `h3defs.h:984-989`)
  - stipple
  - `aaCtrl`, `chipMask`, `sliCtrl`
  - `zaColor`, except inside a clear (`vcrdd_3d.c:160`)
- **Two conditions are needed for the HAL to be correct:**
  - Glide clears `SST_CM_USE_COMBINE_MODE` *and* every select field before using legacy combines, "since some of the combineMode bits still take effect even if SST_CM_USE_COMBINE_MODE is cleared" (`GLIDE/glide3/src/gtex.c:1205-1225`).
  - 3dfx's HAL writes all three `combineMode` registers "before all other 3D regs to clear/set 2ppc mode" at every flush, with the 12-NOP 2PPC exit (`VH5/D6DP2.C:6404-6452`).
- **Today's register dump does not convict this item.** `combineMode`, `aaCtrl`, `chipMask` and stencil all read 0 after the run (`ONBOX/regs/regs_after_windowed_1009.txt`). It is the same class of latent fault as R1, and today's reproduction found the FIFO, not this.
- **Fix:** a "D3D baseline" written at each DP2's first prepare. Put it behind `Diag\D3DSafe` bit 1, pinned write-for-write in `tests/native/test_vcr_kmd_d3dseq.c`. It writes, as 3dfx's HAL does:
  - `combineMode` FBI/TMU0/TMU1 = 0, preceded by 12 NOPs to TMU0|TMU1 when a 2PPC state is possible;
  - `clipLeftRight1 = clipBottomTop1 = 0` (enable clear);
  - `chromaRange` enable clear, stipple off, `aaCtrl = 0`.

### R3. An engine give-up is invisible to the game (hal-03)

- `give_up()` turns acceleration off for the PDEV (`vcrdd_2d.c:83-89`) after `SPIN_CAP` = 2,000,000 status reads (`:71`). It fires from `VcrDdRoom` (`:92-108`) or `VcrDd2dSync` (`:118-132`).
- After that, `VcrDdRoom` returns FALSE at once (`:99-100`). `prepare()` fails (`vcrdd_d3d.c:1361-1362`), `tri()` silently returns (`:1369-1370`), DP2 still returns `DD_OK` (`:1869`), and `D3d_ContextCreate` does not check `g2d_ok` (`:1163-1199`).
- Flips continue (`vcrdd_ddraw.c:315-361`), so the screen shows stale buffers forever.
- **Recovery** happens only at the next `DrvEnableSurface` (`vcrdd.c:540` calls `VcrDd2dInit`), i.e. the next mode set. Today that took a deliberate `vcrctl modeseq` (`ONBOX/session_log.md:10-11`).
- **Fix** (`Diag\D3DSafe` bit 2):
  - return a loss code (`DDERR_SURFACELOST`, inferred as the code games handle) from DP2, Clear2 and Flip once the engine is given up;
  - fail ContextCreate while `!g2d_ok`;
  - log the transition once;
  - optionally ask the miniport for `VcrHwResetEngine` and re-learn the FIFO count (`vcrdd_2d.c:482-491`) before the next context.
- Test: a Python source test that DP2 does not return `DD_OK` when the engine is off. On the 86Box bed, a forced give-up (inferred method: a test escape).

### R4. Contexts hold a PDEV pointer that a mode change frees (hal-04)

- **The stale pointers:**
  - `ContextCreate` stores `pd = l->lpGbl->dhpdev` (`vcrdd_d3d.c:1166, 1183`).
  - `DrvDisablePDEV` frees the PDEV (`vcrdd.c:436-445`).
  - `ctx_free` calls `VcrDd2dSync(c->pd)` (`vcrdd_d3d.c:1216-1217`).
  - Every DP2/Clear2 dereferences `c->pd` (`:1859, 1887`).
  - A mip chain's free uses the heap pointer saved at allocation (`:456, 574, 592`).
- If dxg delivers any call on an old context, or frees a chain after the old heap is gone, the result is a use-after-free in kernel mode. Whether XP's dxg tears contexts down before `DrvDisablePDEV` on a dynamic mode change is unknown (inferred risk; never tested, and the D3D sweeps quit through the game).
- **Fix:**
  - in `DrvDisablePDEV`/`DrvDisableDirectDraw`, mark every `g_ctx` entry whose `pd` is that PDEV dead, so `ctx_of` returns NULL and the call answers `D3DNTHAL_CONTEXT_BAD`;
  - re-derive `pd` from `c->ddlcl->lpGbl->dhpdev` per call.
- Test: an alt-tab and mode-change cycle under a running d3dprobe on the 86Box bed, plus a Python source test for the scrub.

### R5. Cached chip state that nothing invalidates (hal-05)

- The fog table cache (`pd->fog_loaded/fog_valid`, `vcrdd.h:78-79`, used at `vcrdd_3d.c:86-98`) is reset only by a fresh PDEV allocation.
- An exclusive Glide/OpenGL session that loads its own fog table and releases does not invalidate it (`vcrdd_escape.c:134-200`). Neither does `DrvAssertMode(FALSE)`'s device reset (`vcrdd.c:607-615`).
- A D3D game run on the same PDEV afterwards fogs through Glide's table.
- Fix: clear `fog_valid` in `HWCRLSEXCLUSIVE`, `HWCSETEXCLUSIVE` and `DrvAssertMode`. This is CPU-only, so it can be default-on with a Python test.

### R6. Max Payne's freeze: the candidates are now concrete (hal-30)

The cause is still not established (`FINDINGS.md:144-148, 332`). The code and today's evidence give four candidates:

- (a) a stale CMDFIFO with D3D surfaces inside its window (R1);
- (b) an engine give-up hidden from the game (R3), which fits "picture frozen, Num Lock alive";
- (c) display-lock starvation from PIO plus per-vertex polling inside DrawPrimitives2 (R7; `FINDINGS.md:127, 138-143`: escapes parked behind the game, `taskkill` not returning at normal priority);
- (d) the game or its SafeDisc 2.51.

Plan:

1. Apply R1 and R3 first.
2. Cold boot, and confirm `cmdFifo0.baseSize` bit 8 is clear.
3. Run Max Payne attended, with the ring captured afterwards. Event 2D "never went idle / FIFO never drained" decides (b). The `regs` dump decides (a).

### R7. Every register write is synchronous PIO under the display lock (hal-25; performance and responsiveness)

- `vertex()` calls `VcrDdRoom(9..15)`, a status read, before each vertex (`vcrdd_3d.c:188`). It writes 8 to 14 floats, including colour as four separate floats (`:197-200`).
- Strips and fans become independent triangles of 3 vertices each (`vcrdd_d3d.c:1621-1642, 1659-1689`).
- The DP2 walk runs entirely inside the DDI call (`:1821-1871`).
- `Dd_Flip` waits for the engine to go idle before the flip IOCTL (`vcrdd_ddraw.c:334`), so CPU and GPU never overlap across a frame boundary.
- 3dfx streamed packet-3 triangles through the CMDFIFO (`VH5/CFIFO.C`; `W9X/DX/D3D/D3TRI.C:824-828` builds PK3 fan packets).
- Today's `perf` run held vsync (85 fps) with a light load: 17k tris/s (`ONBOX/session_log.md:15`). There is no heavy-load measurement yet.

### R8. A full handle table, and lookups that degrade as it fills (hal-29)

- One 4096-entry open-addressing table serves all processes (`vcrdd_d3d.c:45, 84`). `name_surface` enters **every** attached surface, including every mip level (`:1921-1949`).
- An Unreal-Engine-2-scale game with thousands of mipmapped textures can fill it. "surface handle table full" is logged (`:109-111`) and the texture then draws untextured (refused at `:932-944`).
- Lookups for absent keys scan until an empty slot (`:118-130`), so at a high load factor every `compute_regs` costs up to two 4096-step scans. `handle_forget` rebuilds the whole table on every DestroySurface (`:134-146`).
- Fix:
  - do not name mip sub-levels (TEXBLT and sampling reach them through attach lists, `next_mip :617-626`);
  - grow the table;
  - log a high-water mark.

---

## 3. Caps against implementation

Live D3D8 caps on `.124`: `KMD/evidence/d3dplan_1112/caps_124.txt`. They are built at `vcrdd_d3d.c:2013-2046` (prim caps), `:2049-2113` (DX5 caps and formats) and `:2150-2173` (extended caps).

| cap (where reported) | reported | implemented by | verdict |
|---|---|---|---|
| DevCaps FLOATTLVERTEX, EXECUTESYSTEMMEMORY, TLVERTEXSYSTEMMEMORY, TEXTUREVIDEOMEMORY, DRAWPRIMTLVERTEX, HWRASTERIZATION (`:2066-2070`) | yes | XYZRHW only (`fvf_layout :1258-1259`); system-memory command/vertex buffers (`:1838-1847`) | OK |
| CANRENDERAFTERFLIP (`:2068`) | yes | the flip syncs the engine first (`vcrdd_ddraw.c:334`) | OK |
| DRAWPRIMITIVES2 / **DRAWPRIMITIVES2EX** (`:2069`) | yes | DP2EX makes the driver a DX7-DDI driver. 3dfx's DX7 HAL records and executes state blocks (`VH5/D6DP2.C:1372-1373, 1447-1448, 4196-4245`; `VH5/D7DP2.C:1366-1826`). Ours skips STATESET (`:1562`) | **over-promise if the runtime sends STATESET (inferred)**: hal-07 |
| MiscCaps CULLNONE/CW/CCW (`:2017`) | yes | `setupMode` (`:1023-1026`) | OK |
| MiscCaps MASKZ (`:2018`) | yes | `FZ_ZAWRITE` (`:988-989`) | OK |
| RasterCaps DITHER (`:2019`) | yes | `FZ_DITHER` (`:979-980`) | OK |
| RasterCaps ZTEST | yes | only for `D3DZB_TRUE` (`:986`) | OK, but USEW turns depth off (hal-12) |
| RasterCaps SUBPIXEL | yes | `CP_PARMADJUST` always (`:977`) plus the 0.5 centre (`vcrdd_3d.c:304`) | OK |
| RasterCaps FOGVERTEX | yes | identity ramp and a synthetic 1/W per vertex (`:1004-1016`, `vcrdd_3d.c:202-209`, `vcr_fog.h:10-14`). No fog at all if the FVF has no specular (`:1358-1360`) | approximation |
| RasterCaps FOGTABLE + WFOG | yes | table indexed by W only (`vcr_fog.c:36-62`) | partial: Z-based tables fog everything (hal-13) |
| ZCmpCaps / AlphaCmpCaps, all 8 (`:2021-2024`) | yes | `cmp()` (`:665-668`) | OK |
| SrcBlend ZERO, ONE, SRCALPHA, INVSRCALPHA, DESTCOLOR, INVDESTCOLOR (`:2025-2027`) | yes | `blend_src` (`:630-645`) | OK |
| SrcBlend SRCALPHASAT (`:2027`) | yes | `BF_SATURATE` (`:640`) | **unverified**: 3dfx did not advertise it (`VH5/D3INIT.C:2753`). A 16 bpp target has no destination alpha (inferred: reads as 1, so the factor is 0) |
| SrcBlend BOTHSRCALPHA / BOTHINVSRCALPHA (`:2028`) | yes | `:641-652` | OK |
| DestBlend ZERO, ONE, SRCCOLOR, INVSRCCOLOR, SRCALPHA, INVSRCALPHA (`:2029-2031`) | yes | `blend_dst` (`:647-663`) | OK; matches `VH5/D3INIT.C:2761-2773` |
| ShadeCaps COLORFLATRGB, ALPHAFLATBLEND (`:2032-2033`) | yes | SHADEMODE is never read | **over-promise**: flat drawn gouraud (hal-17) |
| ShadeCaps COLORGOURAUDRGB, ALPHAGOURAUDBLEND | yes | yes | OK |
| TextureCaps PERSPECTIVE, POW2, ALPHA (`:2034-2035`) | yes | `TM_PERSPECTIVE` (`:787`); non-power-of-2 refused (`vcr_texlod.c:18`) | OK |
| TextureCaps **TRANSPARENCY** (`:2035`, DX5-7 only; D3D8 drops it) | yes | COLORKEYENABLE never read; no chroma key written | **over-promise** (hal-15) |
| MIPMAP (`ddsCaps |= DDSCAPS_MIPMAP`, `:2110-2111`) | yes | chains packed as one block (`:367-466`, `:522-584`) | OK, nearest LOD only |
| TextureFilterCaps NEAREST, LINEAR, MIPNEAREST, LINEARMIPNEAREST, MIN/MAG POINT+LINEAR, MIPFPOINT (`:2036-2040`) | yes | `:779-791`, `:814-829` | OK; no +0.5 LOD rounding (hal-18) |
| TextureBlendCaps DECAL, MODULATE, MODULATEALPHA, COPY, ADD (`:2041-2043`) | yes | TEXTUREMAPBLEND never read (`set_rs :1393-1406`) | **over-promise** for DX5 titles (hal-08) |
| TextureAddressCaps WRAP, CLAMP, INDEPENDENTUV (`:2044-2045`) | yes | ADDRESSU/V each (`:792-795`, `:830-833`); `D3DTSS_ADDRESS` and the legacy TEXTUREADDRESS states ignored | partial (hal-08) |
| **LineCaps = TriCaps** (`:2072`), `D3DDD_LINECAPS` (`:2063`) | yes | lines and points parsed and skipped (`:1690-1715`) | **over-promise** (hal-16) |
| RenderBitDepth 16 (+32 with D3D32) (`:2079`) | yes | `vcr_rt_format` (`vcr_rtfmt.h:79-87`) | OK |
| Z list D16 (+D24X8, D24S8) (`:2055-2057, 2174-2214`) | yes | 32 bpp + D16 refused (`vcr_rtfmt.h:84-85`); D24S8 stencil never used (`vcr_rtfmt.h:33-52`) | **over-promise**: the pairing (hal-06) and stencil (hal-20) |
| Formats 565/1555/4444 (`:2081-2084`) | yes | `tex_view` (`:762-771`) | OK |
| A8R8G8B8 with D3DBigTex bit 2 (`:2087-2091`) | yes | `pf_tmfmt` (`:319-321`) | OK, single level proven (hal-23) |
| DXT1-5 with bit 1 (`:2092-2098`, `:508-520`) | yes | `dxt_class` (`:270-275`); DXT1 narrower than 8 refused (`vcr_texlod.c:109-110`) | OK for single levels; mip chains unproven (hal-23) |
| Ext Min/Max texture 1..256 (2048) (`:2155-2158`), aspect 8 (`:2159`), anisotropy 1 (`:2160`) | yes | `vcr_texlod*.c` | OK |
| Ext `dvMaxVertexW` 65536 (`:2152, 2163`) | yes | the fog table covers w up to 52428.8 (`vcr_fog.h:5-6`) | OK |
| Ext `dwFVFCaps` = 2 (`:2164`) | yes | two coordinate sets (`:1339-1350`) | OK; 3dfx also set DONOTSTRIPELEMENTS (`VH5/D3INIT.C:3328`) |
| Ext TextureOpCaps DISABLE, SELECTARG1/2, MODULATE, ADD, BLENDDIFFUSEALPHA, BLENDTEXTUREALPHA (`:2165-2168`) | yes | `color_path` (`:676-745`); BLEND* right only with TEXTURE as arg1 (`:708-713`); stage-1 BLEND* become MULT (`:862`) | **partial** (hal-09) |
| Ext stages 2 / simultaneous textures 2 (`:2169-2170`) | yes | stage 1 combined before diffuse (`:949-956, 977`) | **partial** (hal-09) |
| Ext guard band (`:2152, 2161-2162`, zero) | 0 | the runtime clips | under-report (perf): 3dfx used -200..2048 x -200..1536 (`VH5/D3INIT.C:3221-3236`) |
| Ext `dwMaxTextureRepeat` | 0 (never set) | none | 3dfx reported 32768 (`VH5/D3INIT.C:3208-3210`) |
| Ext `dwStencilCaps` | 0 | none | consistent, but D24S8 is listed |
| `ValidateTextureStageState` (`:1905-1910`) | always 1 pass, OK | none | **over-promise** (hal-10) |

---

## 4. DrawPrimitives2, opcode by opcode

Values are from `KMD/out/ddi/d3dnthal.h:513-576`. The walk is `vcrdd_d3d.c:1572-1819`. Every record is bounds-checked (`NEED`, `:1570`), every primitive is clamped to `dwVertexLength` (`:1369`), and every opcode advances the pointer. The walker cannot loop or overrun; a truncated record returns `D3DNTERR_COMMAND_UNPARSED` with the offset (`:1814-1818`).

| opcode | handling | lines | gap |
|---|---|---|---|
| POINTS (1) | parsed, **skipped** | `:1695-1698` | points vanish (hal-16) |
| INDEXEDLINELIST (2) | parsed, **skipped** | `:1704-1707` | hal-16 |
| INDEXEDTRIANGLELIST (3) | drawn; wFlags (edge, flat) ignored | `:1643-1648` | flat lost (hal-17) |
| RENDERSTATE (8) | `set_rs`, which also updates `lpdwRStates` | `:1585-1590, 1393-1406` | see section 5 |
| LINELIST (15), LINESTRIP (16) | parsed, **skipped** | `:1699-1703` | hal-16 |
| INDEXEDLINESTRIP (17) | parsed (start + n+1 indices), **skipped** | `:1712-1715` | record size matches `VH5/D6DP2.C:3521-3535` |
| TRIANGLELIST (18) | drawn | `:1612-1620` | none |
| TRIANGLESTRIP (19) | drawn as independent triangles, winding kept | `:1621-1633` | 3x vertex traffic (R7) |
| INDEXEDTRIANGLESTRIP (20), INDEXEDTRIANGLEFAN (22) | drawn (start + n+2 indices) | `:1659-1677` | matches `VH5/D6DP2.C:2406-2425` |
| TRIANGLEFAN (21) | drawn as independent triangles | `:1634-1642` | R7 |
| TRIANGLEFAN_IMM (23) | drawn from inline vertices; edge flags ignored | `:1678-1689` | none |
| LINELIST_IMM (24) | parsed, **skipped** | `:1690-1694` | hal-16 |
| TEXTURESTAGESTATE (25) | stages 0/1 stored; stage 2 and above dropped | `:1591-1611` | consistent with 2 stages |
| INDEXEDTRIANGLELIST2 (26) | drawn | `:1649-1658` | none |
| INDEXEDLINELIST2 (27) | parsed, **skipped** | `:1708-1711` | hal-16 |
| VIEWPORTINFO (28) | skipped by size | `:1555` | harmless with no guard band |
| WINFO (29) | skipped by size | `:1556` | needed for W-buffer scaling (hal-12; `VH5/D6DP2.C:1621-1700`) |
| SETPALETTE (30) | skipped | `:1557` | no P8 |
| UPDATEPALETTE (31) | header + entries skipped, **wStateCount times** | `:1716-1724` | the DDK sends exactly one record "regardless of the value of wStateCount" (`VH5/D6DP2.C:4054-4058, 4068-4071`, with an ASSERT at `:4071`); mis-steps if that count is not 1 (hal-27) |
| ZRANGE (32), SETMATERIAL (33), CREATELIGHT (35), SETTRANSFORM (36) | skipped by size | `:1558-1561` | T&L only |
| SETLIGHT (34) | not in `rec_size`, so it goes to the parse-unknown callback | `:1552-1568, 1796-1811` | fine (variable size, T&L only) |
| EXT (37) | skipped by dwSize | `:1783-1788` | none |
| TEXBLT (38) | CPU copy per level, uncompressed and DXT | `:1760-1772, 1499-1549` | none |
| **STATESET (39)** | **skipped by size** | `:1562` | hal-07 |
| SETPRIORITY (40), SETTEXLOD (43), SETCLIPPLANE (44) | skipped | `:1563-1565` | SETTEXLOD (LOD clamp) ignored, minor |
| SETRENDERTARGET (41) | executed: retarget, refusal logged, drawable re-evaluated | `:1725-1753` | none |
| CLEAR (42) | colour/Z; stencil dropped | `:1754-1759` | none |
| 11 (DX5 `D3DOP_EXIT`) | ends the stream | `:1773-1782` | none (the Jedi Knight fix) |
| DX8 opcodes 45-67, unknown | parse-unknown callback, else COMMAND_UNPARSED with `dwErrorOffset` | `:1789-1811` | none |

**DrawPrimitives2EX:** there is no index-size handling because none is needed. DX7 DP2 indices are WORDs (`rw()`, `:1383-1386`); 32-bit indices exist only in the DX8 DDI (inferred).

**DP2 flags:** USERMEMVERTICES is honoured (`:1838-1839`). SWAP/REQ buffer flags and `D3DNTHALDP2_EXECUTEBUFFER` are not looked at. That is legal (optional), but it means DX5 execute-buffer streams other than EXIT go to the runtime callback (inferred).

---

## 5. Render states

The numeric values are d3dtypes.h's (inferred). 3dfx's table confirms COLORKEYENABLE = 41 and MIPMAPLODBIAS = 46 (`VH5/D3RSTATE.C:613, 618`). `compute_regs` (`vcrdd_d3d.c:921-1029`) is the only consumer of `c->rs[]`.

| state | ours | line | 3dfx V5 HAL |
|---|---|---|---|
| TEXTUREHANDLE (1) | stage 0 texture | `:1400-1401` | yes |
| ANTIALIAS (2), EDGEANTIALIAS (40) | ignored | none | ignored (user: no AA) |
| TEXTUREADDRESS (3), TEXTUREADDRESSU/V (44/45) | **ignored** | none | mapped (`D3RSTATE.C:2645-2720`) |
| TEXTUREPERSPECTIVE (4) | ignored, always perspective | `:787` | yes |
| WRAPU/V (5/6), WRAP0-7 (128-135) | ignored | none | stored (`D3RSTATE.C:2394-2640`) |
| ZENABLE (7) | TRUE enables depth; **USEW disables depth** | `:986` | USEW = W-buffer (`D3RSTATE.C:1104-1117`) |
| FILLMODE (8) | **ignored**, always solid | none | non-solid via lines (`D3RSTATE.C:3567-3590`) |
| SHADEMODE (9) | **ignored**, always gouraud | none | flat supported (`D3RSTATE.C:3543-3563`) |
| ZWRITEENABLE (14) | `FZ_ZAWRITE` inside ZENABLE | `:988-989` | same; also forced off when Z is off (`D3RSTATE.C:1156-1190`) |
| ALPHATESTENABLE / ALPHAREF / ALPHAFUNC (15/24/25) | mapped, ALPHAREF masked to 8 bits | `:992-994` | same (`D3RSTATE.C:3354-3430`) |
| LASTPIXEL (16) | ignored (no lines) | none | ignored |
| TEXTUREMAG / TEXTUREMIN (17/18) | **ignored**: stage 0 stays POINT (ctx default `:1145-1146`) | none | mapped to stage 0 (`D3RSTATE.C:2168-2340`) |
| SRCBLEND / DESTBLEND (19/20) | mapped | `:996-998` | yes |
| TEXTUREMAPBLEND (21) | **ignored** | none | mapped to stage 0 TSS (`D3RSTATE.C:2851-2940`) |
| CULLMODE (22) | mapped | `:1023-1026` | yes |
| ZFUNC (23) | mapped | `:987` | yes |
| DITHERENABLE (26) | mapped | `:979-980` | yes |
| ALPHABLENDENABLE (27) | mapped; **the alpha channel is always ONE/ZERO** | `:995-999` | per factor |
| FOGENABLE / FOGCOLOR / FOGTABLEMODE / FOGSTART / FOGEND / FOGDENSITY (28/34-38) | mapped (W table or vertex ramp) | `:1004-1018` | same, plus the z-based workaround and the vertex-fog source (hal-13) |
| SPECULARENABLE (29) | **ignored** | none | specular added per vertex (`D3RSTATE.C:3594-3622`; `W9X/DX/D3D/D3TRI.C:293, 313-314`) |
| STIPPLEDALPHA (33), STIPPLEENABLE (39) | ignored | none | ignored |
| COLORKEYENABLE (41) | **ignored** (TRANSPARENCY is advertised) | none | FBI chroma key from the surface key (`D6DP2.C:6815-6967`) |
| BORDERCOLOR (43) | ignored | none | ignored |
| MIPMAPLODBIAS (46) | ignored | none | "dummy" (`D3RSTATE.C:618`); its own bias tables instead (`:1939-1960`) |
| ZBIAS (47) | **ignored**, cap not reported | none | `zaColor` + bias enable, cap reported (`D3RSTATE.C:1274-1335`; `D3INIT.C:2719`) |
| RANGEFOGENABLE (48) | runtime (software vertex processing) | none | none |
| ANISOTROPY (49) | ignored, cap 1 | none | none |
| STENCIL* (52-59) | **ignored**, caps 0, D24S8 listed | none | full stencil on Napalm (`D3RSTATE.C:1344-1790`; `D3INIT.C:3241-3258`) |
| TEXTUREFACTOR (60) | `c1`; **ignored when paired with a texture** (`:699-703`) | `:1020` | TMU `chromaKey`/`chromaRange` hold the texture factor (`D6DP2.C:6943, 7020-7022`) |
| SCENECAPTURE | ignored | none | used for flushes (`D3RSTATE.C:3656`) |
| CLIPPING, LIGHTING and the other T&L states | runtime | none | none |
| COLORWRITEENABLE (DX8) | ignored, cap not reported | none | hardware write enables exist (section 9) |

---

## 6. Texture stage states and the two-stage combine

| TSS | ours | lines |
|---|---|---|
| TEXTUREMAP (the NT DDI's stage texture) | stored per stage | `:1600-1604` |
| COLOROP / ALPHAOP | stage 0: `color_path`; stage 1: `stage1_combine` | `:676-745, 849-874` |
| COLORARG1/2, ALPHAARG1/2 | `ARG_KIND()` = `& D3DTA_SELECTMASK`: **COMPLEMENT and ALPHAREPLICATE dropped**; CURRENT and SPECULAR read as diffuse | `:671, 678-681` |
| ADDRESS (12) | **ignored** (3dfx copies it to U and V, `VH5/D6DP2.C:1490-1494`) | none |
| ADDRESSU/V | WRAP/CLAMP; MIRROR and BORDER fall to WRAP | `:792-795, 830-833` |
| BORDERCOLOR | ignored | none |
| MAG/MINFILTER | LINEAR or better is bilinear | `:788-791, 826-829` |
| MIPFILTER | NONE: top level; **POINT and LINEAR: nearest LOD** (no trilinear, no LOD dither) | `:779-786, 814-824` |
| MIPMAPLODBIAS, MAXMIPLEVEL, MAXANISOTROPY | ignored | none |
| TEXCOORDINDEX | `& 7`; a set beyond the vertex draws the stage untextured, silently | `:1339-1356` |
| TEXTURETRANSFORMFLAGS / projected coordinates | ignored: `vertex()` uses u,v only | `vcrdd_3d.c:210-221` |

**Stage 0 (`color_path`, `:676-745`), op by op.**

- *Correct:*
  - DISABLE
  - SELECTARG1/2 (TEXTURE, TFACTOR or diffuse)
  - MODULATE(TEXTURE, DIFFUSE)
  - ADD(TEXTURE, DIFFUSE)
  - BLENDTEXTUREALPHA and BLENDDIFFUSEALPHA **only with TEXTURE as arg1**
  - the alpha ops SELECTARG1/2, ADD and MODULATE
- *Wrong:*
  - any op whose second argument is TFACTOR while a texture is the other (`:699-703`: "other" = texture, "local" = diffuse, always). TEXTURE*TFACTOR draws TEXTURE*DIFFUSE, and an ALPHAOP MODULATE(TEXTURE, TFACTOR) fade does not fade.
  - BLEND* with arguments reversed (`:708-713`).
  - MODULATE2X/4X, SUBTRACT, ADDSIGNED/2X, ADDSMOOTH, BLENDFACTORALPHA, BLENDCURRENTALPHA, BLENDTEXTUREALPHAPM, the MODULATE*_ADD* family, DOTPRODUCT3 and BUMPENV*: all drawn as MODULATE (`:714-716`). Every alpha op except ADD and SELECT is drawn as MODULATE (`:737-740`).
  - ADD(DIFFUSE, SPECULAR) draws diffuse+diffuse.
- *Two caps, few promises:* only the 7 ops in `TextureOpCaps 0x184f` are promised. But `ValidateTextureStageState` says yes to everything (`:1905-1910`), and many games do not read TextureOpCaps before choosing a path (inferred).

**Two stages (`compute_regs :947-967`, `stage1_combine :849-874`).**

- TMU1 samples stage 0 (`REPLACE`) and TMU0 samples stage 1 and combines; then the FBI applies **stage 0's** op with diffuse (`:977`). D3D's `((tex0 OP0 diffuse) OP1 tex1)` therefore becomes `((tex0 OP1 tex1) OP0 diffuse)`:
  - right for MODULATE/MODULATE;
  - wrong for a stage-1 ADD (an additive layer gets multiplied by diffuse);
  - wrong for SELECTARG(TEXTURE) at stage 1 (diffuse leaks in);
  - wrong for stage-1 BLEND* (`TM_TC_MULT`, `:862`);
  - wrong for a stage-1 MODULATE2X, the usual lightmap op, which renders at half brightness (`:862`).
- Stage 1 is **ignored entirely** when stage 0 has no texture (`two` requires `tex`, `:928`). The same happens when stage 1 has no texture and only uses CURRENT/TFACTOR, a common "tint after texturing" stage.
- 3dfx implemented the whole DX6/7 op list on Napalm (`VH5/D3INIT.C:3264-3288`). It used the extended `combineMode` with inverts, alpha replicate and TFACTOR in the TMU's chroma registers (`VH5/D6MT.C:512-521, 606-693, 753-776`; `VH5/D6DP2.C:7020-7022`). It validated before accepting a combination: more than 2 stages is TOOMANYOPERATIONS (`VH5/D6MT.C:3322-3328`), and specular as an argument is refused (`:3538-3782`).
- `log_combo` (`:876-919`, event 513 what 21-23 at LogLevel 2) already records every combination a game uses. It is the tool to find which wrong mappings a title actually hits.

---

## 7. Textures and surfaces

- **Formats.**
  - Default path: 565/1555/4444 up to 256 (`:2081-2084`, `tex_view :754-799`).
  - With D3DBigTex: also 8888, DXT1-5, up to 2048 (`:2087-2098`, `tex_view_ext :806-837`).
  - **Missing:** L8, A8, A8L8, A4L4, R3G3B2, A8R3G3B2 and P8. 3dfx listed all but P8 on Napalm (`VH5/D3INIT.C:2095, 2139-2165`). P8 is disabled in the vintage tree because "P8 textures sample WHITE" (`VH5/D3INIT.C:524-531`).
  - A title that wants L8 or A8 (font and alpha masks) must convert or fail (inferred) (hal-21).
- **Palettes:** SETPALETTE/UPDATEPALETTE are skipped (section 4). This is consistent with there being no P8 format.
- **Colour-keyed textures:** not implemented (hal-15).
  - The surface's `DDRAWISURF_HASCKEYSRCBLT` and its key are never read.
  - 3dfx expanded the key from the texture's format to xRGB888 and set `SST_ENCHROMAKEY` (`VH5/D6DP2.C:6823-6958`). Its own note warns that FBI chroma keying tests after bilinear filtering (`:6818-6822`).
- **Placement and alignment.**
  - **Default path (no D3DBigTex) is still broken by the 11-08 alignment bug.** `VcrDdD3dCreateMipChain` places only `DDSCAPS_MIPMAP` chains (`:532-539`). Every single-level texture keeps DirectDraw's 4-byte placement, and `texBaseAddr` drops bits 3:0 (`vcr_texlod.c:32`). The 11-08 fix lives only in `mipchain_ext` (`:381-391`), which a fresh install never reaches (hal-22).
  - Render-target textures (`DDSCAPS_3DDEVICE`) keep DirectDraw's placement on both paths (`:389-391`).
  - Chains whose levels' own FOURCC differs from the chain's are left to DirectDraw (`:432-433`). For DXT they are then sized per level (`VcrDdD3dCreateTexSurface :473-505`), placed 4-byte aligned, and the top is sampled with no mip levels.
  - **d3dprobe never creates a mipmapped DXT or A8R8G8B8 texture.** Every DXT/8888 test uses `Levels = 1` (`KMD/tools/d3dprobe.c:488, 536, 718`); only R5G6B5 chains are tested (`:823, 993`). Mipmapped DXT is what UE2 titles use (inferred), and UT2004's in-game frame shows block noise (hal-23).
- **LOD selection.** `TL_LODMIN = TL_LODMAX = top` unless the surface is a placed chain and MIPFILTER > NONE (`:779-786`). There is no LOD bias, so the hardware truncates where 3dfx biased +0.5 to "round to the closest LOD" (`VH5/D3RSTATE.C:1939-1960`).
- **Size and aspect:** power of 2, aspect up to 8 (`vcr_texlod.c:18-25`, `:99-110`). Correctly advertised.
- **Managed textures and TEXBLT.**
  - The D3D8 runtime uploads managed textures by Lock + CPU write, and Unlock flags them dirty (`vcrdd_ddraw.c:676-681`, `vcrdd_d3d.c:606-614`).
  - TEXBLT copies with the CPU and flags dirty (`:1499-1549`).
  - The next draw that samples a dirty texture runs the cache flush (`:1287-1320`, `vcrdd_3d.c:253-270`).
  - **Not flagged:** a DirectDraw `Blt` into a texture done by the 2D engine (`vcrdd_ddraw.c:540-543`, `clipped_blt :453-456`), and a texture the 3D engine has rendered into. The TMU cache can then serve stale texels (hal-24).
- **Z buffers and pitch rules** (`vcr_rtfmt.h`):
  - Accepted pairs: 16 bpp with no Z or a 16-bit Z, or a 32-bit Z drawn as 16/16 (`:79-83`); 32 bpp with no Z or a 32-bit Z (`:84-85`).
  - The target and Z pitch must be at least a row, 16-byte aligned and at most 0x3fff (`:124-164`).
  - A Z not in video memory is refused (`:157-158`).
  - **32 bpp + D16 is refused** (hal-06). `Dd_CanCreateSurface` refuses non-16-bit Z only when D3D32 is off (`vcrdd_ddraw.c:610-631`). `Dd_CreateSurface` never places a Z itself (`:633-645`), so the fix 3dfx used (`VH5/DDSURF.C:619-626`) has no hook yet.

---

## 8. Clear2, flip, contexts, and DirectDraw beside Direct3D

- **Clear2 / DP2 CLEAR.**
  - Each rectangle is a fastfill clipped to the target (`vcrdd_3d.c:136-179`). Colour comes from `c1`, depth from `zaColor`, 16 or 24 bits by format (`:153-155`).
  - `D3DCLEAR_STENCIL` is dropped (`vcrdd_d3d.c:1412-1413`).
  - The clear rewrites `fbzMode`/`c1`/`zaColor`, and the HAL re-sends state afterwards (`:1419-1420`). The `zaColor` rewrite will matter once ZBIAS uses `zaColor` (hal-11).
- **Flip and vsync.**
  - `Dd_Flip` waits for the previous flip (`vcrdd_ddraw.c:330-333`, rule in `KMD/include/vcr_flip.h`), syncs the engine (`:334`), and writes the start address by IOCTL (`KMD/miniport/vcrmp_dd.c` IOCTL_VCR_DDFLIP).
  - NOVSYNC is honoured (`vcrdd_ddraw.c:319-331`, `DDCAPS2_FLIPNOVSYNC :105`). Vsync on (the user's choice) holds the refresh: `ONBOX/session_log.md:15`.
- **Several contexts.**
  - 32 slots (`vcrdd_d3d.c:44`).
  - Target and state are re-sent at the first draw of every DP2 (`w.prepared = 0`, `:1835`), so interleaved contexts are correct.
  - The fog-table cache is per PDEV and compared by parameters (`vcrdd_3d.c:86-98`).
  - ContextDestroyAll works by pid (`:1232-1240`).
- **DirectDraw blits beside 3D.**
  - Windowed present is a clipped blit by the 2D engine (`vcrdd_ddraw.c:408-466`).
  - Dd_Blt does not wait for the 3D engine before the 2D engine reads a surface the 3D engine is still writing. Both go through one PCI FIFO, but the units run independently (inferred). The `present` probe passes (`KMD/README.md:639-640`), so this is a watch item only.
  - Source colour-keyed blits are done by the engine (`vcrdd_2d.c:207-212`). `DDCAPS_COLORKEY`/`DDCKEYCAPS_SRCBLT` are not reported (`vcrdd_ddraw.c:101-111`), so the runtime emulates keyed blits in software (inferred).
  - `DDBLT_DEPTHFILL` is not accepted (`:472-473`) and falls to the HEL. No stretch cap is reported (hal-31).
- **Surface loss, mode change, alt-tab.**
  - `DrvAssertMode(FALSE)` syncs the engine and resets the device to VGA (`vcrdd.c:607-615`; `KMD/miniport/vcrmp.c:677-695`).
  - The HAL re-sends everything at the next DP2, except the fog table (R5).
  - The PDEV lifetime is R4.
  - Exclusive Glide ownership makes DP2 and Clear2 draw nothing (`exclusive_pid`, `vcrdd_d3d.c:1859, 1887`), which is correct.

---

## 9. What the hardware has and the caps do not say (under-reports)

| capability | hardware evidence | 3dfx V5 HAL | our HAL |
|---|---|---|---|
| Z bias | `fbzMode` bit 16 `FZ_ZBIAS` + `zaColor` (`KMD/include/vcr_3dregs.h:115`; `GLIDE/incsrc/h3defs.h:765`) | ZBIAS cap + implementation (`VH5/D3INIT.C:2719`, `D3RSTATE.C:1274-1335`) | none |
| W-buffer | `fbzMode` bit 3 (`vcr_3dregs.h:108`; `h3defs.h:752`), `SST_DEPTH_FLOAT_SEL` (`:772`) | WBUFFER cap, USEW, WINFO scaling (`D3INIT.C:2722`, `D3RSTATE.C:1104-1117`, `D6DP2.C:1621-1700`) | USEW turns depth off |
| Extended combine: 2x/4x, inverts, alpha replicate | `combineMode` (`h3defs.h:385-530`) | 19 texture ops (`D3INIT.C:3264-3288`) | 7 ops, no modifiers |
| Trilinear, LOD dither, LOD bias | `SST_TRILINEAR` bit 30, `SST_TLODDITHER` bit 4, tLOD `SST_LODBIAS`/`LOD_ODD`/`TSPLIT` (`h3defs.h:248, 339, 719-722`) | MIPLINEAR/LINEARMIPLINEAR/MIPFLINEAR (`D3INIT.C:2840-2847`; tables `D3RSTATE.C:1830-1960`) | nearest LOD only |
| Mirror addressing | tLOD `SST_TMIRRORS/T` bits 28-29 (`h3defs.h:731-732`) | MIRROR cap (`D3INIT.C:2872-2878`) | MIRROR becomes WRAP |
| Chroma key (texture colour key) | `fbzMode` bit 1 + `chromaKey` 0x134 (`vcr_3dregs.h:32, 107`) | yes (`D6DP2.C:6815-6967`) | none |
| Stencil (32 bpp) | stencilMode/stencilOp 0x1e4/0x1e8 (`vcr_3dregs.h:37-38`) | full stencil caps (`D3INIT.C:3241-3258`) | none |
| Specular | per-vertex pre-add (an approximation) | SPECULAR shade caps (`D3INIT.C:2798-2801`; `W9X/DX/D3D/D3TRI.C:293`) | none |
| Per-channel colour write | renderMode RGBA write enables, bits 17-20 (`vcr_3dregs.h:198-203`) | none | DX8 COLORWRITEENABLE could be reported (inferred: the D3D8 runtime reads the DX7 misc cap) |
| L8, A8, A8L8, A4L4, 332, 8332 | TMU formats (`D3INIT.C` format list) | listed (`D3INIT.C:2095-2165`) | not listed |
| Guard band | `SST_RM_ENGUARDBAND` / clip1 (`GLIDE/glide3/src/gsst.c:4177-4179`) | -200..2048 x -200..1536 (`D3INIT.C:3221-3236`) | 0 |
| Lines and points | drawn as triangles (inferred: no line primitive) | `D6LINE.C`, `D6POINT.C`, `LINES.C` | parsed and skipped |
| Multi-chip (SLI) | four VSA-100s on `.124` | the vendor's D3D ran SLI (inferred from `DDSURF.C:452` `Compute_SLIAA_Config`) | chip 0 only |

**Hardware limits (correctly not claimed):** hardware T&L, cube maps, volume textures, pixel and vertex shaders, anisotropic filtering, DOTPRODUCT3/BUMPENVMAP (3dfx left them out too, `VH5/D3INIT.C:3282-3284`), BLENDOP, and separate alpha blending (hal-35).

---

## 10. The plan

Ordered by risk to the box first, then by how many games each fix reaches. The project rules apply throughout:

- every fix lands with a regression test asserting the old and the fixed value (`tests/native/test_vcr_kmd_*.c` or `tests/python/test_vcr_kmd_d3d.py`), and for HAL behaviour a `d3dprobe` check;
- new HAL or kernel behaviour lands **default-off** behind a Diag switch, unless the proven path is byte-for-byte unchanged;
- one Diag change per clean boot;
- a deploy is a `vcrdd.dll` swap plus `safe-reboot.py` after an activation check;
- no `vcrctl`/ExtEscape while a D3D game runs; mode switches at least 5 s apart.

To keep the reboot count sane, the proposal uses **two bit-field switches**, like `D3DBigTex`: `Diag\D3DSafe` (robustness) and `Diag\D3DFeat` (features). Proving one bit per clean boot keeps the "one Diag change per boot" rule.

### Phase 0: instrumentation (log-only, no register change, default-on)

T3. Extend the "context gone" event (`vcrdd_d3d.c:1212-1215`) with counters that show which gaps each game really hits:

- legacy texture render states seen (3, 17, 18, 21, 44-46);
- STATESET by operation (to settle hal-07 by measurement, not inference);
- lines, points and LINELIST_IMM skipped;
- SHADEMODE flat, FILLMODE not solid;
- ZBIAS non-zero, ZENABLE = USEW, STENCILENABLE, COLORKEYENABLE, SPECULARENABLE;
- unsupported ops and modifiers seen at stage 0/1;
- textures refused, and the handle-table high-water mark.

This is pure logging, pinned by a Python source test. Then run every D3D title **in-game** once (not to the menu) at LogLevel 2, which also keeps `log_combo`. This one pass turns the "(inferred)" per-title exposure in section 11 into measured fact.

### Phase 1: stability (`Diag\D3DSafe`), before any more game sweeps

1. **bit 0: stale CMDFIFO (R1).** Miniport: stop `cmdFifo0/1` at every mode set with no exclusive owner. Display: refuse and request a reset at ContextCreate/DP2 when the FIFO is enabled and there is no owner.
   - Native test: the decision.
   - On-box: kill a Glide client, then d3dprobe fullscreen and windowed at 32 bpp.
2. **bit 1: D3D-owned baseline (R2).** `combineMode` FBI/TMU0/TMU1 = 0 (with the 12 NOPs), `clipLeftRight1`/`clipBottomTop1` = 0, `chromaRange` and stipple off, `aaCtrl = 0`, once per DP2. Pinned write-for-write in `test_vcr_kmd_d3dseq.c`.
3. **bit 2: dead engine is reported (R3).** DP2/Clear2/Flip return a loss code, ContextCreate fails, recovery through the miniport's engine reset.
4. **Always-on, CPU-only fixes:** context scrub at PDEV/DirectDraw disable (R4), fog-cache invalidation (R5), handle-table sizing and no mip sub-level names (R8). Each gets a Python test. R4 also gets an 86Box alt-tab test.
5. **bit 3: D16 behind a 32 bpp target (hal-06).** Place Z surfaces in `Dd_CreateSurface` with 32-bit storage when `rt32` is on and the request is 16-bit, as 3dfx did (`VH5/DDSURF.C:619-626`). `vcr_rt_format` then sees a 32-bit Z.
   - Test: `d3dprobe render --full --bpp 32 --zfmt d16` must be 40/40.
   - The probe's own pacing must hold: a failed CreateDevice already cost two mode sets 156 ms apart.
6. Then the attended **Max Payne** run (R6).

### Phase 2: correctness quick wins (`Diag\D3DFeat` bits 0-5). Each uses register combinations the HAL already writes.

1. **ValidateTextureStageState tells the truth (hal-10).** Answer `D3DERR_UNSUPPORTEDCOLOROPERATION`/`ALPHAOPERATION`/`ARGUMENT`/`TOOMANYOPERATIONS` exactly where `color_path`/`stage1_combine` would approximate. Test: a d3dprobe `validate` matrix.
2. **Legacy DX5 states onto stage 0 (hal-08)**, mirroring 3dfx's map (`VH5/D3RSTATE.C:2851-2940`):
   - TEXTUREMAPBLEND; TEXTUREMAG/MIN to MAG/MIN/MIPFILTER; TEXTUREADDRESS(U/V) and `D3DTSS_ADDRESS` to ADDRESSU/V;
   - DX5 MODULATE's "texture alpha if it has alpha, else vertex alpha" (3dfx's `0x7ffffffe` sentinel, `:2898-2905`).
   - Test: a **new DX7 probe** (`d3d7probe`). D3D8 cannot send these states, so d3dprobe cannot test them.
3. **ZBIAS (hal-11):** `FZ_ZBIAS` + `zaColor` = minus (bias shifted by 1 at 16 bpp, by 9 at 24 bit) for LESS/LESSEQUAL, re-sent after every clear (`VH5/D3RSTATE.C:1274-1335`). Report `D3DPRASTERCAPS_ZBIAS`. Test: d3dprobe `zbias` (a coplanar decal at bias 1 must win).
4. **Specular and flat shading (hal-14, hal-17):** a saturating per-vertex add of specular RGB into the diffuse (3dfx's CLAMP888), and SHADEMODE FLAT sending the provoking vertex's colour on all three vertices, both in `vertex()`. Report `SPECULARGOURAUDRGB`. Tests: d3dprobe `specular`, `flatshade`.
5. **Lines and points (hal-16):** draw as two-triangle quads (1-pixel width; points 1x1) through the existing setup unit. Report nothing new: the caps already promise lines. Tests: d3dprobe `lines`, `points` (and `linestrip`, `indexedlines`).
6. **Alignment on the default path (hal-22) and coherency (hal-24):**
   - place every texture on the default path (generalise `mipchain_ext`'s single-level placement to `vcr_texlod_compute`);
   - place render-target textures 16-byte aligned;
   - mark a texture dirty when the 2D engine blits into it or the 3D engine renders into it.
   - Tests: Python (placement on both paths), d3dprobe `blt2tex`, `rtt`.

Also, without a switch: UPDATEPALETTE parses exactly one record (hal-27), and `dwMaxTextureRepeat` is reported. Decide SRCALPHASAT by a probe (`srcalphasat`), and stop advertising it if the probe fails (hal-26).

### Phase 3: feature parity with the 3dfx V5 HAL (`Diag\D3DFeat` bits 6+). New register traffic; each proven alone.

1. **Extended combine (hal-09):**
   - implement D3D's two-stage pipeline on `combineMode` for FBI, TMU0 and TMU1: OUTSHIFT for MODULATE2X/4X, INVERT for COMPLEMENT/SUBTRACT/ADDSIGNED, alpha selects for ALPHAREPLICATE, TMU `chromaKey`/`chromaRange` as TFACTOR (3dfx `D6DP2.C:7020-7022`);
   - fix stage ordering (stage 0 with diffuse before stage 1), and handle stage 1 without a stage-0 texture;
   - widen TextureOpCaps one op at a time as each probe passes.
   - Tests: d3dprobe `mod2x`, `mod4x`, `addsigned`, `subtract`, `areplicate`, `complement`, `tfactor_tex`, `tfactor_fade`, `stage1_add`, `stage1_select`, `blendta_rev`, `tint_stage1`; each asserts colours against a computed reference.
2. **W-buffer (hal-12):**
   - ZENABLE USEW sets `FZ_WBUFFER`, and WINFO drives the W scale (3dfx `D6DP2.C:1621-1700`);
   - vertex fog moves off the Wfbi carrier when W-buffering (3dfx's table `D3FOG.C:202-238`);
   - report `D3DPRASTERCAPS_WBUFFER`. Test: d3dprobe `wbuffer`.
3. **Fog fidelity (hal-13):** the Z-based table guard (3dfx PXLFOGLIN: start < 1 and end <= 1 gives no table fog, `D3FOG.C:376-395`); evaluate a direct iterated fog source for vertex fog (3dfx `FOG_Z|FOG_ALPHA`, semantics inferred). Tests: d3dprobe `fogtable_z`, `fogvertex_grad`.
4. **Filtering (hal-18, hal-19):**
   - +0.5 LOD rounding for nearest-mip;
   - MIPMAPLODBIAS and MAXMIPLEVEL in tLOD;
   - LOD dither as the single-TMU trilinear approximation;
   - two-TMU LOD-split trilinear when only one stage is in use (3dfx tables `D3RSTATE.C:1830-1960`);
   - MIRROR via tLOD bits 28-29.
   - Tests: d3dprobe `lodbias`, `trilinear` (a mip-boundary gradient), `mirror`.
5. **Texture colour key (hal-15):** FBI chroma key from the surface's source key, expanded to xRGB888 from the texture format (3dfx `D6DP2.C:6823-6958`). Test: `d3d7probe colorkey`.
6. **Stencil at 32 bpp (hal-20):** stencilMode/stencilOp from the eight stencil states, D3DCLEAR_STENCIL as a fastfill with stencil REF/WMASK, and report the stencil caps. Test: d3dprobe `stencil` (32 bpp, D24S8).
7. **Formats (hal-21):** L8/A8/A8L8/A4L4/332/8332. Test: d3dprobe `tex_l8`, `tex_a8`, ... decode tests. P8 stays out until the palette path is solved (`VH5/D3INIT.C:524-531`).
8. **State blocks (hal-07):** only if Phase 0's counter shows STATESET arriving. Record RS/TSS/texture handles between BEGIN and END without applying them (3dfx `D6DP2.C:1372-1373, 1447-1448`), and implement EXECUTE, CAPTURE and DELETE. Test: d3dprobe `stateblock` and `d3d7probe stateblock`.

### Phase 4: performance

1. Strips and fans through the setup unit's strip/fan mode (`SM_FAN`, `sDrawTriCMD` per added vertex), and packed `sARGB` colour. Both are CPU-side; pin them write-for-write.
2. The CMDFIFO (packet 3) instead of PCI PIO (README roadmap, `KMD/README.md:796`). This makes DP2 asynchronous and shortens display-lock holds (R7). Flip via the FIFO's swap command rather than a CPU idle wait.
3. A guard band (hal-26).
4. Long term: SLI for D3D (hal-32).
- Measure every step with `d3dprobe perf` under a heavy scene (the 17k tris/s run does not load the chip) and with the UT2004 botmatch (vsync off for measurement only).

### Tooling (in parallel)

- **d3dprobe additions:**
  - `mipdxt1`, `mipdxt5`, `mip8888` (hal-23, first)
  - `zbias`, `wbuffer`, `specular`, `flatshade`
  - `lines`, `points`
  - `validate`
  - `tfactor_tex`, `mod2x`, `areplicate`, `stage1_add`
  - `mirror`, `lodbias`, `trilinear`
  - `stencil`, `blt2tex`, `rtt`, `stateblock`
  - `d16_32`
  - an `--after-glide-kill` scenario
- **A DX7 probe** for what D3D8 cannot send: legacy states, colour keys, DX7 state blocks.
- Keep using `log_combo` and the Phase 0 counters on real titles. A render-correct d3dprobe has already once failed to predict a game (`/home/voidsstr/development/retro-3dfx/FINDINGS.md:30-31`).

---

## 11. Per-title exposure

All of this is inferred, and Phase 0 replaces it with measurement.

| title (D3D era) | likely to hit |
|---|---|
| UT2003 / UT2004 (D3D8, UE2) | MODULATE2X lighting (dark, `ONBOX/games/UT2004_botmatch/UT2004_botmatch_090s.png`), mipmapped DXT (hal-23), multi-stage terrain combines and TFACTOR (hal-09), ZBIAS decals, handle-table size (R8) |
| Halo (D3D8) | combines and TFACTOR, render targets for effects (hal-24), state blocks (hal-07), fog modes |
| Max Payne (D3D8) | R1/R3/R7 first; then combines and render-to-texture |
| Red Faction (D3D8) | combines, ZBIAS, lines in the HUD |
| BF1942 (D3D8) | MODULATE2X, DXT mips, ZBIAS |
| Jedi Knight DF2, MotS (DX5 execute buffers) | legacy states: point-sampled, MODULATE only (hal-08); flat shading; colour key |
| Shogo, Rainbow Six, Hidden and Dangerous, AvP, Thief Gold, System Shock 2 (DX5/DX6) | legacy states, colour key (TRANSPARENCY is promised), lines in maps/HUDs, flat shading, Z-based table fog |
| DirectDraw 2D titles (RA2, TS, StarCraft, WC2) | only hal-31 (keyed and stretch blits through the HEL), and R1/R3 if the engine dies |

---

## 12. Open questions and evidence gaps

- **Does XP's D3D runtime send STATESET to a DX7-DDI driver** that reports DRAWPRIMITIVES2EX with software vertex processing? Phase 0 counter (hal-07).
- **Does it forward the legacy DX5 texture states to a DX6+ HAL?** 3dfx's NT code handles them, which suggests yes (hal-08). Phase 0 counter.
- **What draws UT2004's block noise and stripes?** `UT2004_botmatch_090s.png`, `UT2004_botmatch_065s.png`. Candidates: a DXT chain left to DirectDraw (`vcrdd_d3d.c:432-433`), formats it converts, combines. Read 513/19-23 and 513/7 from a LogLevel 2 run (hal-23, hal-33).
- **Is dxg guaranteed to destroy D3D contexts before `DrvDisablePDEV`** on a dynamic mode change (R4)?
- **The Max Payne freeze (R6):** to be settled after R1 and R3.
- **Doc drift:** the Diag table still calls D3DBigTex "NOT yet on silicon" (`KMD/README.md:111`), while the plan records it verified by d3dprobe on 09-28 (`docs/v56k-benchmark-plan.md`, resume point 2026-09-28 13:10) and today's run passed 83/0 at 1024x768x32 (`ONBOX/d3dprobe_textures_a_1024x768x32.txt`).
