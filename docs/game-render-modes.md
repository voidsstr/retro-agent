# Game × render-mode matrix on .124 (Voodoo3, XP)

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
