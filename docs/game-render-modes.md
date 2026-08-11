# Game × render-mode matrix on .124 (Voodoo3, XP)

> ## ⚠️ .124 IS A GeForce2 GTS NOW (2026-08-11) — most of this page is historical
>
> The Voodoo 3 was removed and an **NVIDIA GeForce2 GTS** fitted, running
> **ForceWare 71.89**. That invalidates the premises this whole page was written
> on:
> - **Every "Glide" column is dead.** No Voodoo, no `glide2x`/`glide3x`, and the
>   3dfx OpenGL ICD (`retrogl`/`3dfxgl`/`3dfxvgl`) has been purged from system32
>   and from all 35 game-local copies. Any instruction below that says
>   `gl_driver 3dfxgl` or `r_glDriver retrogl` now means **plain `opengl32`**,
>   which resolves to NVIDIA's `nvoglnt.dll`.
> - **The "D3D wedges the box network for minutes" warning was a vintage-H5-HAL
>   trait and no longer applies** — that HAL is gone with the 3dfx driver.
> - **16-bit-only is no longer true.** The GeForce2 does 32-bit colour and 32-bit
>   Z; the desktop runs 1024×768×32 @100Hz.
>
> Only UT99 has been re-verified on the new card (see the UT99 section below).
> **Every other row is unverified on NVIDIA** — treat it as a starting point, not
> a result, and re-test before trusting it.

How to select each rendering mode for every installed game, and the verified
status of each on the Voodoo3. Our stack: retrogl (Mesa OpenGL ICD, deployed as
a game-local `opengl32.dll`/`3dfxgl.dll` — `opengl32` is NOT a KnownDLL here, so
game-local wins) + our 787 KB h3 glide3x + the vintage H5 D3D HAL (for D3D) +
native Glide (retail `glide3x`/`glide2x`) for games with their own Glide renderer.

**Voodoo3 is 16-bit-only** (no 32-bit color, no >256² textures, no 24/32-bit Z) —
every mode renders 16-bit; set 16bpp where a knob exists.

**⚠ D3D-HAL fullscreen WEDGES the box network for minutes** (vintage HAL trait,
recovers when the game dies). ANY D3D-fullscreen game test MUST use a
self-killing on-box batch + a `retro_enqueue.py <ip> "EXEC ... taskkill"` recovery
net — never a bare launch. Applies to: CS/HL `-d3d`, UT D3DDrv, Carmageddon2 D3D,
Incoming, 3DMark2000.

## Selection levers (verified via research + on-box, 2026-07-27)

| Game | Dir | OpenGL (our ICD) | Glide (native) | Direct3D (vintage HAL) | Software |
|---|---|---|---|---|---|
| **Quake 3** | `C:\Quake III Arena\Quake3` | `+set r_glDriver retrogl` (retrogl.dll) ✅ | — | — (no D3D renderer) | — |
| **Quake 2** | `C:\Games\Quake2` | `+set vid_ref gl +set gl_driver 3dfxgl +set gl_mode 3` ✅ | — | — | `+set vid_ref soft` |
| **RtCW** | `D:\GOG Games\Return to Castle Wolfenstein` | `r_glDriver retrogl` / MP `gl/openglv5.dll` ✅ | — | — | — |
| **MOHAA** | `D:\Program Files\EA GAMES\MOHAA` | game-local `opengl32.dll` ✅ (needs CD1 ISO mounted via D:\ daemon) | — | — | — |
| **CS 1.6 / HL** (GoldSrc) | `D:\...\Bcs16 Romania\CS 1.6` | game-local `opengl32.dll`, `-gl` ✅ | — | `-d3d` (16bpp desktop) ✅⚠ | `-soft` |
| **Unreal Tournament** | `C:\Games\Unreal Tournament (Installed)\System` | ini `GameRenderDevice=OpenGLDrv.OpenGLRenderDevice` ✅ | `GlideDrv.GlideRenderDevice` ✅ | `D3DDrv.D3DRenderDevice` ⚠(wedge) | `SoftDrv.SoftwareRenderDevice` |
| **Descent 3** | `C:\Games\Descent3` | `HKLM\SOFTWARE\Outrage\Descent3 PreferredRenderer=2` + local opengl32.dll | `=4` (Glide) | `=3` (D3D) ⚠ | launcher enum |
| **Carmageddon 2** | `D:\Carmageddon 2 Carpocalypse Now` | — (no OpenGL) | `CARMA2_HW.EXE` (default) | `CARMA2_HW.EXE -d3d` ⚠ | `CARMA2_SW.EXE` |
| **Heretic II** | `D:\Heretic2` | `Heretic2.exe +set vid_ref gl +set gl_driver 3dfxgl +set gl_mode 3` | — | — | `+set vid_ref soft` |
| **SiN** | `D:\SiN` | `Sin.exe +set vid_ref gl +set gl_driver 3dfxgl +set gl_mode 3` | — | — | `+set vid_ref soft` |
| **Incoming** | `D:\Incoming` | — | — | D3D-only, auto ⚠ | — |
| **Quake 1** | `C:\Games\Quake1` | GLQuake: game-local `opengl32.dll` (or `+set gl_driver 3dfxgl`) `-bpp 16` | — | — | WinQuake (DirectDraw), DOS quake (NTVDM) |
| **Red Alert 2** | `C:\Games\...Red Alert 2...V2\RA2` | — | — | — | DirectDraw (aqrit wrapper) ✅ |
| **Doom 2** | `C:\Games\Doom 2` | — | — | — | DOS (NTVDM) |
| **Descent 1/2** | `C:\Games\Descent1`,`Descent2` | — | Descent2: `D2VOODOO.EXE` (DOS Glide) | — | DOS software (NTVDM) |

Notes:
- UT resolution: `[WinDrv.WindowManager] FullscreenViewportX/Y`. `-log` prints
  `GL_RENDERER` (our voodoo-cleanroom string) on the OpenGL bind = ICD proof.
  UT shows a **Recovery-Mode dialog** after any unclean (taskkill) exit — click
  "Run Unreal Tournament" (~513,423 @1024×768) to proceed; it is NOT a crash.
- Descent 3: launch `main.exe -launched` to skip the launcher & use the stored
  PreferredRenderer. OpenGL renderer LoadLibrary's `opengl32.dll` by name.
- Q2-engine trio (Q2/Heretic2/SiN): stage our ICD as `3dfxgl.dll` + 787KB
  `glide3x.dll` in the game dir; `ref_gl.dll` must be present.
## UT99 on the GeForce2 — use OpenGL; D3D crashes, Glide is gone (2026-08-11) ✅

Re-verified end to end after the GPU swap. **Both** UT99 installs are now set to
`OpenGLDrv.OpenGLRenderDevice` in all three keys (`GameRenderDevice`,
`WindowedRenderDevice`, `RenderDevice`):

| Install | Path | State |
|---|---|---|
| **#1 — the desktop shortcut's target** | `C:\Games\Unreal Tournament (Installed)` | ✅ works: loads DM-Codex, clean textures/lighting/depth |
| #2 — copy on the Win98 volume's desktop | `C:\WINDOWS\Desktop\Unreal Tournament` | config fixed, but **blocked by a "Cd Required At Startup" dialog** — it's a retail CD-check copy and there is no UT ISO on the box (only `D:\ISO\MOHAA_CD1.iso`) |

- **Do NOT use `D3DDrv.D3DRenderDevice`.** UT's stock D3D (internal revision
  1.9c, DirectX 7 era) binds fine and detects the card correctly
  (`szDescription=NVIDIA GeForce2 GTS/GeForce2 Pro`, `dwVendorId=4318`,
  `dwDeviceId=336`, 29504K vram) — and then **hard-crashes immediately after
  `Log: Video memory fill is complete` / `VidMem Disposition: 141 1`**, with the
  log truncating mid-word because nothing gets flushed. That trailing pair of
  lines in `System\UnrealTournament.log` is the signature.
- **`GlideDrv` is dead** — no Voodoo in the box. A config left on Glide is the
  other way UT silently fails now; install #2 was still on it.
- **The 2026-07-28 OpenGL polygon-flicker below does NOT reproduce on NVIDIA.**
  That was UT's `OpenGLDrv.dll` mishandling depth against *our retrogl ICD*;
  against `nvoglnt.dll` DM-Codex renders clean — no Z-fighting on floors, walls
  or pickups. So the "use native Glide instead" fix is now both impossible and
  unnecessary; OpenGL is simply correct.
- Renderer proof in the log: `Log: Bound to OpenGLDrv.dll`. The renderer is
  registered by `OpenGlDrv.int`, and `OpenGlDrv.dll` (102,400 B, stock v436) is
  present in both installs — **note install #1 has no `D3DDrv.dll` sibling
  problem; it does ship D3DDrv, so a bad config change will "work" right up
  until it crashes.**
- **Automation gotcha:** UT captures the mouse and uses relative motion, so
  `UICLICK` **cannot drive its in-game menus** — the pointer moves but UT never
  updates its hover, and clicks land on nothing. Drive UT from the command line
  instead (`UnrealTournament.exe DM-Codex.unr?game=BotPack.DeathMatchPlus`) and
  use `UIKEY ESCAPE` for the menu bar. `UICLICK` *does* work on UT's Win32
  dialogs (Recovery Mode, CD-check) because those are real windows.
- Recovery-Mode dialog: still appears after any `taskkill` exit; click
  "Run Unreal Tournament" at **(514, 424)** @1024×768. It clears itself after one
  clean in-game quit.

## UT texture/polygon flicker — FIXED by using native Glide (2026-07-28)

**Symptom:** in UT's **OpenGL** renderer, polygons flicker (Z-fighting-like). Q3,
Q2, RtCW, MOHAA all use the SAME retrogl ICD with zero flicker, so the bug is in
**UT's own retail `OpenGLDrv.dll`** (the notoriously buggy old Mesa-based UT
renderer) mishandling the depth buffer — NOT our driver. Confirmed the context
is fine (retrogl log: colDepth 16, RGB565, aux/depth buffer present), so it's
UT-side depth handling. `UseZTrick=False` + `DetailTextures=False` +
`UsePrecache=True` did NOT resolve it.

**FIX: use UT's native Glide renderer** — set in `System\UnrealTournament.ini`:
```
[Engine.Engine]
GameRenderDevice=GlideDrv.GlideRenderDevice
RenderDevice=GlideDrv.GlideRenderDevice
```
Glide (`GlideDrv.dll` + native `glide2x/glide3x`) is the path UT was designed for
on 3dfx — no OpenGL layer, no depth-negotiation bug — and is typically the
FASTEST renderer on a Voodoo3. User-confirmed clean (no flicker). This is UT's
default renderer going forward on .124.

**Alternate (if OpenGL is ever required):** replace UT's retail `OpenGLDrv.dll`
with the community **UTGLR** renderer (Chris Dohnal) — the modern, correct one.

**Benchmark note:** UT fullscreen (Glide OR our OpenGL) WEDGES .124's network
while running, so the live-interactive timedemo harness (F9) can't drive it;
needs a self-contained on-box timedemo. UT is heavily CPU-bound on the P3 (the
earlier OpenGL ref was ~25 fps @640/1024 — resolution barely matters); Glide is
comparable-or-faster and, more importantly, renders correctly.

## Verification results (2026-07-27 sweep)

**VERIFIED WORKING + STABLE** (sustained in-game run, no crash dialog, on our stack):
| Game | Modes verified | Evidence |
|---|---|---|
| Quake 3 | OpenGL | retrogl ctx SUCCESS, ALIVE 100MB in q3dm1 |
| Quake 2 | OpenGL | retrogl ctx SUCCESS, ALIVE 29MB in q2dm1 |
| RtCW | OpenGL | retrogl ctx SUCCESS, ALIVE 156MB in mp_beach |
| MOHAA | OpenGL | retrogl grSstWinOpen SUCCESS, ALIVE 32MB (CD mounted) |
| CS 1.6 | OpenGL + D3D | GL: retrogl SUCCESS, ALIVE 105MB de_dust; D3D: EngineDLL=hw.dll @1024×768×16 |
| Red Alert 2 | DirectDraw | menu+skirmish render, ALIVE 53MB |
| UT99 | OpenGL + Glide + D3D | GL: retrogl SUCCESS 80MB; Glide: ALIVE 71MB; D3D: HAL fullscreen engaged (wedge signature) |

**BLOCKED — needs media, not a driver problem:**
- **Descent 3** (installed): CD-locked ("Please insert Descent 3 CD 1"); no disc
  image on the share. Renderer levers confirmed (PreferredRenderer 2/3/4) but
  can't launch past the CD check. (Open-source D3 port would remove the CD gate.)

**NOT INSTALLED (empty placeholder dirs):** Heretic II, SiN, Incoming,
Carmageddon 2, Half-Life GOTY/OpFor/BlueShift, Quake 1 (only DEICE installer).
These need a real install before any mode test.

**DOS games** (Doom 2, Descent 1, Descent 2): DOS-software titles that run under
NTVDM — they do NOT use the Voodoo3 3D path (Descent 2's D2VOODOO is DOS-Glide,
needs a DOS Glide TSR unavailable on XP). Fullscreen DOS (mode 13h) takes over
the display and isn't remotely capturable. Out of scope for the 3dfx-driver sweep.
