# retro3dfx Driver Debugging Notes

A growing knowledge base of hard-won debugging findings for the retro3dfx MesaFX
ICD (`retrogl.dll` / `opengl32_retail.dll`) on Voodoo3 hardware. Each entry:
symptom → how we isolated it → root cause → fix → how to reproduce the diagnosis.

Newest first.

---

## 2026-07-20 — In-game resolution switch (vid_restart) wedge — FIXED (0.1.31)

**Symptom.** Changing resolution IN-GAME (e.g. Q2 video menu 640->1024x768, which
runs `vid_restart`) hard-crashes the whole box: quake2.exe dies AND the Voodoo3
board is left wedged, so the agent's next GDI call hangs and the machine goes
unreachable. The watchdog can't recover it (the game already exited, so there's no
hung game to kill). Requires a manual power-cycle.

**Root cause.** `vid_restart` destroys and recreates the GL context in-process:
`grSstWinClose` -> (last context) `grGlideShutdown` -> `grGlideInit` -> `grSstWinOpen`
at the NEW resolution. That is a full Glide teardown+reinit with a hardware
mode-change mid-flight. On the Voodoo3 (Avenger) this in-process mode change is
fragile and wedges the board/display. This is a known 3dfx limitation — many Glide
games required a RESTART to change resolution for exactly this reason. (Launching
fresh at ANY resolution is rock-solid — the full 640..1600x1200 sweeps prove it;
it's only the in-process switch that wedges.)

**Guidance / workaround (per user decision 2026-07-20 — document, don't fix).**
Change resolution by RESTARTING the game at the new resolution, not via the
in-game menu. The Q2 resolution launchers do exactly this and are all stable:
`Quake_II_640x480_120Hz.bat` / `_800x600_` / `_1024x768_` / `_1280x1024_` (all use
the stable `retrogl` ICD + the right `gl_mode`). Same rule for the other idTech
games: pick a resolution at launch. A driver-side fix would be possible (keep Glide
initialized across the recreate instead of shutdown+reinit; reset the display mode
cleanly between close/open) but every test attempt crashes the box, so it needs a
supervised session — deferred by choice.

---

## 2026-07-19 — Q2 "crashes when run normally" = launcher used the stock 3dfxgl path

**Symptom.** User launched Quake II normally and it crashed.

**Root cause.** The Q2 launcher `play_q2.bat` (and it alone) used
`gl_driver 3dfxgl` — the STOCK 3dfx MiniGL path, which is unstable (documented to
crash intermittently / green-screen the display) and needs a 16-bit desktop
switch. The other `Quake_II_<res>.bat` launchers passed *Quake III* cvars
(`r_mode -1 r_customwidth ...`) that Q2 ignores, so they fell back to the config's
`gl_driver` (already `retrogl`) — those were fine, but confusingly wrong.

**Fix.** Rewrote all 5 Q2 launchers (`play_q2.bat` + the four
`Quake_II_<res>_*Hz.bat`) to use OUR stable `retrogl` ICD with the correct Q2
`gl_mode` per resolution (3=640, 4=800, 6=1024, 8=1280) and no desktop switch
(retrogl owns Glide fullscreen from a 32-bit desktop). retrogl is both stable and
faster (96 vs 75.7 fps @640). Verified: all 5 launchers launch+render+exit with no
crash across 640/800/1024/1280. A plain `quake2.exe` (config `gl_driver retrogl`)
was already fine.

**Takeaway.** Keep every Q2 launch path on `retrogl`; never ship the stock
`3dfxgl` path as a default — it is the one unstable Q2 GL path on this box.

---

## 2026-07-18 — Counter-Strike 1.6 RUNS on our ICD: it was the wrong build

**Resolution.** CS 1.6 works on our MesaFX ICD — the blocker was the *build*, not
the driver. The `C:\Program Files\Counter-strike` build crashes right after GL
extension init (dies before its first texture; see the earlier windowed-conflict
analysis). The **BC Romania build** at
`C:\Program Files\Bcs16 Romania\Counter-Strike 1.6` renders fine on the same ICD:
launched with our ICD staged as `System\opengl32.dll` + `glide3x.dll` and
`FX_NO_PALETTED_TEXTURE=1`, it reaches the menu (BCShield 2.5 banner, server/world
modules initialized) AND loads maps — `+map de_dust` loads the world, bots, and
tutor with hl.exe stable in-game. `GL_RENDERER` is our `[retro3dfx]` ICD.

**Gotchas found:** launch with a plain `+map <name>` — combining
`+net_graph 3 +maxplayers 6 +map` made hl.exe exit during load (a startup-cvar/
listen-server quirk, not rendering). As with UT/Q2, the GDI `SCREENSHOT` can't
capture the fullscreen-Glide surface (shows the desktop); trust the console
log + stable process.

**Windowed-Glide path (`fxwindow.c`) — built, opt-in, not needed for CS.** While
chasing the wrong build I implemented real windowed-Glide rendering (DDraw
`DDSCL_NORMAL` offscreen surface + Blt-to-window via `grSurface*Ext`), env-gated
`FX_WINDOWED=1`, falling back to fullscreen. It gets through DDraw surface
creation + `grSurfaceCreateContext` + `grSurfaceSetRenderingSurface`, but two
issues remain for a full windowed context: (1) creating a DDraw surface *after*
`grSurfaceSetRendering` faults — fixed by allocating all surfaces first; (2)
`grSurfaceSetAux` still faults in Glide's `_grGetSurfaceInfo`/alignment path, and
without a depth surface GoldSrc exits post-create. Kept in-tree, opt-in (zero
effect unless `FX_WINDOWED` is set), for a future engine that genuinely needs
windowed GL. Not required now that CS runs fullscreen on the right build.

---

## 2026-07-18 — Unreal Tournament (UT99) "very low fps" = software-GL fallback

**Symptom.** UT99 (`C:\Games\Unreal Tournament (Installed)`, `GameRenderDevice=
OpenGLDrv.OpenGLRenderDevice`) ran at very low fps on the Voodoo3.

**Root cause.** There is **no hardware `opengl32.dll` on .124** — no 3dfx OpenGL
ICD was installed system-wide. UT's `OpenGLDrv.dll` loads `opengl32.dll`, and with
no hardware GL present it fell back to **Microsoft's software OpenGL** (pure CPU
rasterization) → single-digit fps. Same class of issue as Q2's stock path: the
game's GL renderer couldn't find a hardware ICD.

**Fix.** Deploy our MesaFX ICD as `opengl32.dll` into UT's `System\` directory
(plus the known-good retail `glide3x.dll` next to it — the same in-dir binding
lesson as Q2). UT's `OpenGLDrv` then loads our hardware ICD and renders on the
Voodoo3 via Glide:
```
UT System\opengl32.dll  <- retrogl.dll (retro3dfx 0.1.22)
UT System\glide3x.dll   <- C:\Quake III Arena\Quake3\glide3x.dll (344064 B)
```
Result — `Init: glGetString(GL_RENDERER): Mesa Glide v0.62 Voodoo3 (tm)
[retro3dfx 0.1.22]`, DM-Deck16][ at **1024×768×16 ≈ 67 fps in-game** (133 avg incl.
menu), vs single-digit software GL before. UT's ini already selected `OpenGLDrv`,
so no config change was needed beyond staging our ICD.

**Screenshot note (same as the sibling Voodoo5 lane).** The agent's GDI
`SCREENSHOT` cannot capture a fullscreen-Glide surface — it reads the desktop
framebuffer, which shows garbage while the *monitor* renders correctly (proven by
the valid on-screen timedemo fps). Match the desktop resolution to the game's, or
trust the fps counter, rather than the GDI capture. This is a capture limitation,
not a render bug.

**Optional further gains.** 67 fps @1024×768 is present-bound (see
PROFILING-FINDINGS on the sibling lane); 640×480 pushes higher fps, and the
0.1.22 gamma+dither defaults improve UT's 16-bit image for free. The dominant win
is software→hardware.

---

## 2026-07-18 — Counter-Strike 1.6 (GoldSrc) exits on our ICD — architecture mismatch

**Symptom.** Launching CS 1.6 (`hl.exe -game cstrike -gl -full`) with our ICD
staged as `opengl32.dll` in the CS root: the process reaches GL init, logs the
extension banner, then **exits** before drawing anything. Never reaches the menu.

**Isolation.**
- Our ICD **loads and creates its Glide context successfully** — `MESA.LOG`
  (under `MESA_FX_INFO=r`) shows `post grSstWinOpen ctx=...` +
  `Voodoo Screen: 640x480:16 RGB` exactly like Q2/Q3. So the ICD/context is fine.
- `qconsole.log` (`-condebug`) shows GoldSrc getting as far as
  `Found paletted texture extension` → `ARB Multitexture extensions found`, then
  nothing — it dies right after extension enumeration, before the first frame.
- **Not paletted textures**: `FX_NO_PALETTED_TEXTURE=1` (hides
  `GL_EXT_paletted_texture` / `GL_EXT_shared_texture_palette`, forcing GoldSrc to
  the RGBA path) removed that banner line but hl.exe still exited.
- **Not multitexture**: `FX_NO_MULTITEXTURE=1` (hides `GL_ARB_multitexture`)
  produced `NO Multitexture extensions found` and hl.exe **still** exited.
- **Not our driver at all, at the video-mode layer**: with our ICD *removed*
  (GoldSrc falling back to the stock `gldrv\3dfxgl.dll` MiniGL), CS popped a
  **"Video mode change failure — the specified video mode is not supported. The
  game will now run in software mode"** dialog. So GoldSrc's *own*
  `ChangeDisplaySettings` to 640×480 fullscreen fails on this box independent of
  the GL driver.
- Pre-setting the desktop to 640×480×16 (so GoldSrc's mode switch is a no-op)
  did **not** stop our-ICD hl.exe from exiting.

**Root cause (architecture mismatch).** GoldSrc's fullscreen OpenGL model is:
*do a GDI `ChangeDisplaySettings` fullscreen mode switch, then render GL into that
desktop-owned framebuffer* — the model a normal desktop ICD or 3dfx's purpose-
built GoldSrc **MiniGL** (`gldrv\3dfxgl.dll`) is written for. Our MesaFX ICD
instead takes the Voodoo board via **Glide `grSstWinOpen` in exclusive fullscreen
mode**, which owns the display outside GDI. GoldSrc's concurrent GDI mode switch
and its post-switch state checks conflict with Glide's exclusive ownership, and
GoldSrc aborts. Q2 and Q3 work on our ICD precisely because they use *standard GL
fullscreen* and let the GL driver own the mode — they never do a competing GDI
`ChangeDisplaySettings`. This is the same reason 3dfx shipped a dedicated GoldSrc
MiniGL rather than expecting the full ICD to work.

**Status / options.**
- CS 1.6 on our full MesaFX ICD is **not currently supported** — it's a deeper
  engine-integration effort (windowed-GL path, or coordinating the Glide board
  grab with GoldSrc's GDI mode switch), not a quick fix.
- CS *does* run via the stock `gldrv\3dfxgl.dll` MiniGL once the box's 640×480
  fullscreen mode-change issue is resolved (that dialog is separate and about the
  primary display mode list / refresh, not our driver).
- Q2 and Q3 remain the driver-optimization benchmark games on our ICD.

**Useful driver knobs added while isolating this** (both default OFF = old
behavior, gated on env at `fxDDInitExtensions`):
- `FX_NO_PALETTED_TEXTURE=1` — hide the paletted-texture extensions; makes an
  engine that uploads 8-bit paletted textures use the solid RGBA path instead.
- `FX_NO_MULTITEXTURE=1` — hide `GL_ARB_multitexture` (single-texture fallback).

---

## 2026-07-18 — Quake II (`ref_gl`) `wglCreateContext` crash/hang on our ICD

**Symptom.** Launching Quake II with `gl_driver retrogl` (our ICD) produced a
green screen + a Windows "driver has stopped working" dialog, and the process
never rendered. Q3 on the same ICD worked perfectly. The stock `gl_driver 3dfxgl`
path "worked" but was unstable (crashed intermittently, took the agent offline)
and low-quality textures.

**Isolation (instrumented ICD).** Added `[q2diag]` `fprintf(stderr,...)` tracing
around context creation in `fxwgl.c::wglCreateContext` and the `grSstWinOpen` call
in `fxapi.c::fxMesaCreateContext`, gated on `MESA_FX_INFO=r` (which `freopen`s
stderr to `MESA.LOG` in the game's working dir). Launched with
`set MESA_FX_INFO=r` and read `C:\Games\Quake2\MESA.LOG`.

Trace showed the flow reaching **`pre grSstWinOpen`** but **never `post`** — i.e.
the fault/hang is *inside* `grSstWinOpen` (in `glide3x.dll`, a retail binary we
can't instrument). Logged params were **byte-identical** to a working Q3 launch:
```
Q2 (fails): pre grSstWinOpen win=1b00a2 res=7 ref=0 HavePixExt=0 pixFmt=3 aux=1 vis=1 fg=1   <-- no post
Q3 (works): pre grSstWinOpen win=d0104  res=7 ref=0 HavePixExt=0 pixFmt=3 aux=1 vis=1 fg=1   <-- post ctx=valid
```
So it was **not** the resolution, refresh, pixel format, aux-buffer count, or
window visibility/foreground state. The only difference was the process/window.

**Dead ends (ruled out, don't re-try):**
- *Desktop color depth* (16 vs 32-bit): Q2 hung at both. Not it.
- *Message pump before context* (drain `PeekMessage`/`DispatchMessage` +
  `SetActiveWindow` so the freshly-shown window is truly active before Glide
  grabs the board): no change. The pump is *kept* in the source — it's harmless
  and correct-in-principle — but it was not the fix.
- *`ChangeDisplaySettings(NULL,0)` to undo `ref_gl`'s pre-context CDS to a
  fullscreen-exclusive mode*: **counterproductive** — it resized the game window
  to the restored desktop size (640x480 → 1024x768, `res=7` → `res=12`), so the
  ICD then tried to bring up Glide at the wrong resolution. Reverted.

**Root cause.** `grSstWinOpen` was faulting because our `retrogl.dll` was binding
**the wrong `glide3x.dll`**. The Q2 directory had *no* `glide3x.dll` and neither
did `system32`; the Windows loader resolved the import from elsewhere on the
search path to an incompatible build, and `grSstWinOpen` in that build crashed.
Q3 worked purely because its own directory ships the correct
`glide3x.dll` (344064 bytes, AmigaMerlin retail) right next to `quake3.exe`, and
the exe dir is searched before the rest of the path.

**Fix.** Place the known-good `glide3x.dll` (the 344064-byte AmigaMerlin retail
build, copied from `C:\Quake III Arena\Quake3\glide3x.dll`) into the game's own
directory, next to `quake2.exe`:
```
copy /Y "C:\Quake III Arena\Quake3\glide3x.dll" "C:\Games\Quake2\glide3x.dll"
```
After that, Q2 creates the context and renders on our ICD:
```
GL_RENDERER: Mesa Glide v0.62 Voodoo3 (tm) [retro3dfx 0.1.16]
689 frames, 7.2 seconds: 95.5 fps    (640x480x16, demo1)
```
**95.5 fps vs 75.7 fps for stock `3dfxgl`** — our ICD is both faster and carries
the LOD-bias texture sharpening (0.1.11). Any game that loads our ICD must have
the matching retail `glide3x.dll` in its own directory — treat this as a
deploy-time invariant (the launcher/bench harness now stages it).

**`Voodoo ! fallback (%x), raster (%x)` log spam is NOT software rasterization.**
The `fallback`/`raster` values come from `fxDDChooseRenderState` (`fxtris.c`) and
report which *primitive render path* was selected (smooth points, stippled lines,
flat-shaded tris route through the general TNL `rast_tab` path instead of the
fast `fx_render_tab` path) — still hardware-rasterized. True pixel-level software
fallback prints `Voodoo ! enter SW 0x...` (we never saw it). Critically, this
per-state `fprintf` to `MESA.LOG` under `MESA_FX_INFO=r` **destroys the framerate
by itself** — always benchmark with verbose OFF. Only enable `MESA_FX_INFO=r` for
diagnosis.

**Repro the diagnosis.** Build the ICD with the `[q2diag]` traces (present on
branch history around 0.1.12–0.1.16), deploy `retrogl.dll` to the game dir +
`system32`, `set MESA_FX_INFO=r`, launch, and read `<gamedir>\MESA.LOG`. Absence
of a `post grSstWinOpen` line = fault inside Glide → check the `glide3x.dll` the
process actually loaded.

## ALL-OPEN deploy blocked: ICD↔glide underscore-ABI mismatch (2026-07-21)
Attempting the full-open stack on .124 (our glide + our-glide-linked MesaFX):
Q3 fails at `LoadLibrary('retrogl.dll')`. Root cause (objdump): the CURRENT
`build-stack.sh` output `out/opengl32.dll` (0.1.31) IMPORTS glide symbols
**underscore-decorated** (`_grBufferSwap@4`, 65 imports) but our `out/glide3x*.dll`
EXPORT them **non-underscore** (`grBufferSwap@4`, per FORKS.md "export both grFoo
and grFoo@N"). Import-by-name must match exactly → unresolved imports → the ICD
won't load. The **Jul-16** build was internally consistent (ICD imported
NON-underscore, matched the glide exports) and the 2026-07-17 ALL-RETRO3DFX
milestone worked — so the current build-stack.sh MesaFX link step regressed to the
retail/underscore import convention. ALSO unresolved: h3 (V3) vs h5 (V5) glide
export sets differ — the V3 needs the ICD linked against the h3 import lib.
FIX (TODO, build-stack.sh): make the MesaFX ICD link against our glide's
import lib with matching decoration (or have our glide export `_grFoo@N` too), and
build a V3 (h3-linked) ICD variant. Until then .124 stays HYBRID: our MesaFX
(retail-linked retrogl 0.1.31) + retail AmigaMerlin glide + vendored-H5 display —
which works (Q3 57.7). Box rolled back + verified after each failed attempt.

## ABI blocker SOLVED; grSstWinOpen is the next (separate) blocker (2026-07-21)
**Stage 1 (SOLVED): ICD↔glide underscore ABI.** Our MesaFX ICD IMPORTS glide
symbols underscore-decorated (`_grBufferSwap@4`) — same as retail 3dfx glide
EXPORTS — but our pure-mingw glide exported only `grBufferSwap@4` ⇒ `LoadLibrary`
fail. FIX (baked into build-stack.sh `dual_abi_relink`): relink each glide DLL with
an augmented def so it exports BOTH `grFoo@N` and `_grFoo@N`. Proven on .124: with
the dual-ABI glide, `LoadLibrary('retrogl.dll'): succeeded` and Q3 initialized
through mode-set to `GLW_ChoosePFD`. All 65 ICD imports satisfied (objdump).
**Stage 2 (OPEN): grSstWinOpen hardware-init hang.** With the ABI fixed, Q3 now
reaches `GLW_ChoosePFD(16,16,0)` then STALLS in our glide's `grSstWinOpen`
(Glide context / board bring-up). Retail AmigaMerlin glide does NOT stall here on
the same box+display driver (renders 58 fps), so it's specific to OUR glide's
hardware-init path talking to the current H5 `3dfxv3d` display driver. Leading
suspect (per vcr-disp/README): our glide's HWCEXT `GETLINEARADDR` probe — the H5
`3dfxvs`-derived driver answers HWCSETEXCLUSIVE/CONTEXT_DWORD_NT (seen in the reg
ring) but may not answer GETDEVICECONFIG/GETLINEARADDR, so our glide's hwcInit
finds no board → grSstWinOpen wedges. (The 2026-07-17 "ALL-RETRO3DFX" milestone
DID render with our glide + an H5 build, so a working combo exists — the current
H5 build/config or glide build diverged.) NEXT: enable glide/MESA_FX_INFO logging,
capture where grSstWinOpen fails, compare the HWCEXT handshake our glide needs vs
what our H5 3dfxv3d answers; fix on whichever side (add GETLINEARADDR to the H5
build, or align our glide's init path). Box stays on the working retail-glide
hybrid meanwhile (Q3 57.7).

## Stability campaign results across all 3 APIs (2026-07-21, working hybrid stack)
Stack under test: our MesaFX ICD (retrogl 0.1.31) + retail AmigaMerlin glide +
vintage H5 3dfxv3d display driver, on .124 (P3-845, Voodoo3).

**OpenGL + Glide: ROCK SOLID.** Full game×resolution sweep, ZERO crashes
(PING-after OK on every run), recorded to specpicks:
| game | 640 | 800 | 1024 |
|---|---|---|---|
| Quake II | 96.5 | 69.5 | 47.1 |
| Quake III | ~58 | ~58 | 50.9 |
| RtCW wolfbench | 56.0 | 48.1 | 31.8 |
| Unreal Tournament | 35.6 | 24.4 | 23.0 |
(Q2/Q3 idTech2/3, RtCW idTech3, UT Unreal engine — all four engines stable.)

**Direct3D: UNSTABLE (but recovers).** 3DMark2000 Default Benchmark @1024×768
16-bit color / 16-bit textures / 16-bit Z (V3-class config): INCONSISTENT — first
attempt (weeks-stale driver load path) hard-TDR'd ("3dfxv3d stopped responding");
this attempt (clean reboot) "Benchmark Aborted!" GRACEFULLY (reg-ring shows only
clean mode-set transitions + a HWCEXT_UNMAP_MEMORY, NO H3MakeRoom stall, NO wedge;
driver restored the desktop and OpenGL still ran — Q3 57.1 after). So the D3D HAL
sometimes aborts cleanly, sometimes wedges under sustained texture load.
**Diagnostic gap:** the .124 reg-ring instruments DDraw/mode/FIFO/HWCEXT but NOT
the D3D primitive/texture path (D6DP2/D3TXTR) — the .143 lane added
`DP2-PARSE-ERR`/`D3TXTR` ring logging that is NOT in .124's deployed 3dfxv3d. To
pin the exact failing D3D op, rebuild+deploy the H5 display driver from the
current shared source (which has that instrumentation + the mipmap fix + the
50M spin-breaker) and re-run. D3D hardening is the .143 lane's active area
(shared H5 source) — coordinate via [[driver-lane-division]].

**Glide all-open blocker:** unchanged — dual-ABI export fixed (ICD loads), but
grSstWinOpen hardware-init still hangs (see prior note). During testing our glide
faulted at 0x6e40cfd7 (bad memory read) — consistent with hwcInit finding no board.

## Instrumented display driver deployed + glide grSstWinOpen ROOT-CAUSED (2026-07-22)
**Rebuild/redeploy DONE:** built the current shared H5 source → 3dfxvs.dll 957456
(has DP2-PARSE-ERR + D3TXTR mipmap fix + 50M spin-breaker + reg-ring, all confirmed
in the binary), packaged WFP-rename (dist/3dfx-voodoo3-wfp-20260722), deployed to
.124 as 3dfxv3d.dll (move-aside the loaded file + copy + reboot). Loads clean:
957456, driver bound, ring active, OpenGL still 54.9 fps. .124 now has the D3D
primitive/texture instrumentation the old 942668 build lacked.

**Glide grSstWinOpen ROOT CAUSE (precise):** With the instrumented driver, captured
our glide's WinOpen escape handshake vs retail glide's:
- Both send the same visible escapes: ALLOCCONTEXT(0x1), HWCSETEXCLUSIVE(0x8),
  CONTEXT_DWORD_NT(0x17); driver handles all. GETLINEARADDR is opcode **0x03**
  (HWCEXT.C:292) — handled but NOT in the ring's annotated set, so its absence was
  a red herring.
- Our h3 glide IS built with `HWC_EXT_INIT=1` (Makefile.mingw:99) → `hwcMapBoard`
  (minihwc.c:1121) sends GETLINEARADDR(0x03) and uses `res.baseAddresses[0]` as the
  board register base.
- The Q3 fault is a READ at **0x00014717** = a register offset (~0x14717) added to a
  **NULL base** ⇒ the driver returned `baseAddresses[0] = 0` to OUR glide. The
  driver's `hwcGetLinearAddr` returns the mapping *allocated for the curPID*
  (HWCEXT.C:56); if our glide's per-process memory MAP wasn't registered (or the
  hwcExtResult_t struct layout diverged so we read the wrong offset), it reads 0.
  Retail glide gets a valid base on the same driver+handler ⇒ the gap is our fork's
  map-before-GETLINEARADDR sequence OR the `hwcExtResult_t`/`linearAddressRes`
  struct ABI in glide3x/h3/minihwc/hwcext.h vs the driver's.
- **FIX TARGET (next):** add a ring log of the returned baseAddresses in the driver's
  GETLINEARADDR handler (one build) to confirm it returns 0 for our glide; then fix
  on the glide side — ensure the memory-map escape precedes GETLINEARADDR and the
  result struct matches the driver's exactly. This is a bounded fork source fix.

**D3D:** the driver is now instrumented (DP2-PARSE-ERR); the .143 lane earlier
RESOLVED the "warm-rerun degradation" as a 3DMark2001 APP issue, driver provably
clean (FINDINGS.md:518). Next D3D step: reproduce 3DMark's texture test on the
instrumented driver and read DP2-PARSE-ERR/D3TXTR for any real driver-side wedge.

### Glide grSstWinOpen — struct ABI RULED OUT; it's the per-PID map (2026-07-22)
Compared our fork's `hwcExtResult_t` (glide3x/h3/minihwc/hwcext.h) to the driver's
(H5 .../Displays/H5/HWCEXT.H): IDENTICAL union member order (resStatus@0,
linearAddressRes is the 4th union member). So it is NOT a struct-layout mismatch.
⇒ Confirmed: the driver's `hwcGetLinearAddr` returns baseAddresses[0]=0 because
there is no linear-memory mapping registered for our glide's PID at GETLINEARADDR
time. Retail glide registers the mapping (a MAP escape / LINEAR_MAP_OFFSET 0x11)
before GETLINEARADDR; our fork's minihwc sequence does not (or its map call fails
on this driver). **THE fix is in glide3x/h3/minihwc: ensure the per-process linear
memory MAP precedes hwcMapBoard's GETLINEARADDR.** Confirm by adding a one-line
ring log of (PID, mappingFound, baseAddresses[0]) to the driver's GETLINEARADDR
handler, redeploy, run our glide → expect base=0/mappingFound=0.

## GLIDE grSstWinOpen — VERIFIED ROOT CAUSE via glide instrumentation (2026-07-22)
Instrumented our glide's hwcMapBoard (minihwc.c) to log the GETLINEARADDR escape
result. Ran Q3 on the instrumented dual-ABI glide → `C:\GLIDEHWC.LOG`:
  GETLINEARADDR esc=0x3df3 rv=1 resStatus=1 numBase=3 base0=0 base1=0 base2=0
DEFINITIVE: the escape SUCCEEDS (rv=1, resStatus=1) but the driver returns
**all-zero base addresses**. Chain: hwcMapBoard uses baseAddresses[0]=0 as the
board register base → grSstWinOpen adds a register offset → READ at ~0x00014717 →
Application Error. Driver side (HWCEXT.C hwcGetLinearAddr → MISC.C
miscGlideMapMemoryBases): `EngDeviceIoControl(IOCTL_VIDEO_QUERY_GLIDE_ACCESS_RANGES)`
returns SUCCESS but `VideoAccessRange[0..2].VirtualAddress == 0`, so
glideRegBase/glideScreenBase/glideIOBase are all 0. Retail AmigaMerlin glide
tolerates this (HWC_ACCESS_DDRAW path — it takes the board address from a DDraw
surface lock, not GETLINEARADDR); OUR glide relies on the GETLINEARADDR result and
faults. Also ruled out (with proof): NOT the LINEAR-primary warm-rerun degradation
(first-launch ring showed PRIMARY-TILED 640x480x16 and it still crashed), NOT a
struct-ABI mismatch (hwcExtLinearAddrRes_t identical incl. numBaseAddrs).
**Applied guards (glide, minihwc.c):** memset(res) before the escape; treat rv<=0
as failure; treat baseAddresses[0]==0 as failure → grSstWinOpen now returns an
error instead of crashing (STABILITY: no more Application Error). **FULL FIX (two
options):** (a) miniport/driver — make IOCTL_VIDEO_QUERY_GLIDE_ACCESS_RANGES return
non-zero VirtualAddresses for the V3 (needs the DDK build; likely a V3-vs-Napalm
mapping path), or (b) glide — take the register base from the DDraw path
(HWC_ACCESS_DDRAW / dxdrvr.c) when GETLINEARADDR yields 0, as retail glide does.

## D3D FIXED by the instrumented-driver redeploy (2026-07-22)
The old deployed .124 driver (942668, built before the mipmap fix) hard-TDR'd on
3DMark2000's Texture Rendering test. After deploying the fresh 957456 build (current
shared H5 source: D3TXTR mipmap fix + CFIFO 50M spin-breaker + reg-ring), 3DMark2000
Default Benchmark ran the FULL suite — including the Texture Rendering Speed test
(the exact former wedge trigger) — for 3+ minutes with:
- PING alive throughout, NO TDR, NO abort, NO "Stopped Responding".
- Reg-ring advanced 29 -> 862 entries (thousands of D3D ops) with ZERO
  DP2-PARSE-ERR, ZERO H3MakeRoom STALL, ZERO wedge markers.
- The texture-tunnel scene renders CORRECTLY: clean marble textures, correct
  mipmapping (no black/garbage mips) — the D3TXTR mipmap fix is working live.
⇒ The D3D instability was the stale pre-mipmap-fix driver; the current source is
stable on .124. Matches the .143 lane's finding that the warm-rerun "degradation"
was a 3DMark2001 APP issue, driver provably clean. The .124 D3D HAL is now stable
for the 3DMark2000 D3D suite.
