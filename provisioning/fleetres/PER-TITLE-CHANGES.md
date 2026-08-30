# Per-title resolution change list for Games-Library

> ## ⚠️ SUPERSEDED — read `PER-TITLE-STATUS.md` instead
>
> This was the **work list**, written before any of it had been applied, and
> several of its judgements were measured wrong afterwards:
>
> * Tiberian Sun is **not** capped at 1024x768 — the engine renders 1920x1080.
> * Quake 1's cap is real but it is **1280x960**, not 1024x768.
> * Hexen II is **not** limited like GLQuake — it takes 1920x1080.
> * SoF2 must **not** use `r_mode -1`; that fork has no such branch and renders
>   640x480. It also must not use `FR_Q2MODE`, whose mode 8 is a different
>   resolution from id Tech 3's.
>
> Everything in it has now been applied, and `PER-TITLE-STATUS.md` records what
> each of the 37 titles actually does, per box, with the measurement behind it.
> Keep this file only as the record of what was planned and why.


Prerequisite for every row marked "launcher": copy `FLEETRES.EXE` (54,784 B)
into the title's root, and paste the standard block from README-FLEETRES.md at
the top of its `Play <Game>.bat`.

Legend — **WIDE** = the engine renders true widescreen, use `FR_W/FR_H`.
**4:3** = the engine has no widescreen support, use `FR_W43/FR_H43` (a
correctly proportioned 4:3 image is the honest best; stretching it to 16:9 is
worse). **NO** = not configurable, leave it alone.

## A. Proven on hardware (.145, DELL E2414H 1920x1080)

| title | class | change |
|---|---|---|
| **Quake3-TeamArena** | WIDE | 3 launchers, each doing BOTH routes (see the example .bat). **DELETE `seta r_mode "6"` and `seta r_fullscreen "1"` from `baseq3\autoexec.cfg`** — once they are gone the command line alone is enough and the `exec` half becomes belt-and-braces. `r_customaspect` is the retail cvar, `r_customPixelAspect` the ioquake3 one — write both; the unused one is harmlessly created. |
| **UnrealTournament** / **UnrealTournament436** | WIDE | `FLEETRES -ini "System\UnrealTournament.ini" WinDrv.WindowsClient FullscreenViewportX %FR_W%` (and `...Y %FR_H%`, and `StartupFullscreen True`). Leave the `[SDLDrv.SDLClient]` block alone. |
| **CounterStrike16** | WIDE (vert-) | `Play Counter-Strike.bat`: `start "" hl.exe -game cstrike -full -w %FR_W% -h %FR_H%`. |
| **Descent1** | DOSBox | `FLEETRES -ini "dosboxD1.conf" sdl fullresolution %FR_DOSFULLRES%`. |

## B. Same mechanism, engine confirmed, not yet run on hardware

| title | class | change |
|---|---|---|
| **SoldierOfFortune2** | WIDE | Identical to Quake III (`r_customwidth` confirmed present in `SoF2.exe` and `sof2mp.exe`, and `r_customaspect` too). Two launchers already exist. Drop `seta r_mode "6"` from `base\autoexec.cfg`. |
| **JediAcademy** | WIDE | Same cvars (`r_customwidth` present in `jasp.exe`/`jamp.exe`; no `r_customaspect`, which is fine — that build derives aspect from w/h). **Needs two NEW `.bat` launchers** — `launch.txt` currently points straight at the exes, so there is nowhere to run FLEETRES. |
| **UT2004** | WIDE | `FLEETRES -ini "System\UT2004.ini" WinDrv.WindowsClient FullscreenViewportX/Y`. |
| **UnrealGold** | WIDE | `FLEETRES -ini "System\Unreal.ini" WinDrv.WindowsClient FullscreenViewportX/Y`. |
| **DeusEx** | WIDE | `FLEETRES -ini "SYSTEM\DeusEx.ini" WinDrv.WindowsClient FullscreenViewportX/Y`. |
| **Thief2** | WIDE | Has **NewDark** (`D3DX9_43.dll`, `NVScript.osm`, `new_mantle` in CAM.CFG), which is genuinely widescreen-aware. `FLEETRES -setline "CAM.CFG" game_screen_size game_screen_size %FR_W% %FR_H%`. Currently `800 600`. |
| **Descent3** | WIDE | `main.exe` accepts `-Width` / `-Height` (verified in the binary). `start "" main.exe -launched -Width %FR_W% -Height %FR_H%`. |
| **Carmageddon1** | DOSBox | `FLEETRES -ini "dosboxCarma.conf" sdl fullresolution %FR_DOSFULLRES%`. |
| **RedneckRampage** | DOSBox | `FLEETRES -ini "dosboxRR.conf" sdl fullresolution %FR_DOSFULLRES%`. |
| **Quake2Complete** | **4:3** | `set gl_mode "%FR_Q2MODE%"` in `baseq2\autoexec.cfg` via a generated `fleetres.cfg` + `exec`. Mode table read out of `quake2.exe`: 0=320x240 … 6=1024x768 … 9=1600x1200, **no custom mode, no `vid_width`** — 1600x1200 is the ceiling and there is no 16:9 entry. Also apply to `xatrix\`, `rogue\`, `ctf\` if they carry their own autoexec. |
| **SiNGold** | **4:3** | Identical table in `sin.exe` (verified). `set gl_mode "%FR_Q2MODE%"` in `base\autoexec.cfg`. Needs a `.bat` for the main game — `launch.txt` line 1 points straight at `sin.exe`. |
| **SoldierOfFortune** | **4:3** | Identical table in `SoF.exe` (modes 0-2 marked "Unsupported"; 3=640x480 … 9=1600x1200). `set gl_mode "%FR_Q2MODE%"` in `base\autoexec.cfg`. Needs a `.bat`. |
| **Quake1** | **4:3** | `GLQUAKE.EXE` takes `-width %FR_W43% -height %FR_H43% -bpp 32`. Needs a `.bat` (launch.txt points at the exe). GLQuake is known to be fussy above 1024x768 on modern drivers — **test before shipping above that**. |

## C. Change the value but NOT to native — a judgement call, test first

| title | class | note |
|---|---|---|
| **TiberianSun** | 4:3 | `SUN.INI [Video] ScreenWidth/ScreenHeight` is **640x480** — the worst value in the library, and on a 1080p panel it is a 3x upscale of a stretched 4:3 image. The CnCNet patch reads these at runtime (that is the whole reason SUN.INI must exist at all). Recommend `%FR_W43%/%FR_H43%` **capped at 1024x768** — a Westwood 2D engine draws sprites 1:1, so at 1920x1080 the units become unreadably small. |
| **RedAlert2** / Yuri | 4:3 | `RA2.INI` and `RA2MD.INI` `[Video] ScreenWidth/ScreenHeight`, currently 1024x768. Same sprite-scale argument, and vanilla RA2 without Ares/CnCNet may refuse anything above 1024x768. **Leave at 1024x768 until a box test proves otherwise.** |
| **SystemShock2** | 4:3 | Dark engine with **no NewDark** in this tree. `cam.cfg` has no `game_screen_size` line at all; adding one is `-setline`'s append path. Cap at 1024x768 and test — vanilla Dark's HUD is not widescreen-aware. |
| **ThiefGold** | 4:3 | Same: no NewDark, `CAM.CFG` carries no `game_screen_size`. Same treatment as SS2. |
| **Shogo** | 4:3? | LithTech 1.0. `autoexec.cfg` lines `"screenwidth" "640"` / `"screenheight" "480"` — `FLEETRES -setline autoexec.cfg screenwidth `screenwidth` `%FR_W43%`` (backticks become quotes). LithTech 1.0 only accepts a mode its D3D/OpenGL device enumerates; **I set 1920x1080 on .145 and reverted it without launching, so this is unverified.** |
| **RedFaction** | WIDE | Config is in the **registry**, not a file: `HKLM\SOFTWARE\Volition\Red Faction` values `Resolution Width`, `Resolution Height`, `Resolution Bit Depth` (names read out of `rf.exe`; on .145 only `InstallPath` exists so far, i.e. the game has never been configured). Launcher: `reg add "HKLM\SOFTWARE\Volition\Red Faction" /v "Resolution Width" /t REG_DWORD /d %FR_W% /f`. Confirm the hive on a box that has run the game. |
| **Descent2** | WIDE | DXX-Rebirth (`d2x-rebirth.exe`, `ResolutionX` string present) keeps the mode in its own `descent.cfg` in the write dir. Not present on .145, so the exact key spelling is unconfirmed — read it off a box that has run the game once, then `-setline`. The second launcher (`DESCENTW.EXE`, original Win95) has a fixed mode list: leave it. |
| **Carmageddon2** | ? | Registry `HKLM\SOFTWARE\SCI\CARMAGEDDON2`; the exe carries `XResolution`/`YResolution` strings but they belong to a different subsystem. Needs a box session with the in-game video menu before anything is written. |

## D. Genuinely not configurable — do not force these

| title | why |
|---|---|
| **StarCraft** / Brood War | 1.16.1 is hard-locked to 640x480. There is no setting. |
| **JediKnightDF2**, **JediKnightMotS** | The Sith engine stores a `displayMode` **index** into a list the 3D device supplies; there is no width/height pair to write and no widescreen entry. |
| **AliensVsPredator** | Already CLOSED as BLOCKED (graphics fastfile content absent from every distribution). Do not touch it. |

## E. id Tech 3 needs BOTH routes until the library drops `seta r_mode "6"`

`r_mode`, `r_customwidth`, `r_customheight` and `r_fullscreen` are all
`CVAR_LATCH` — they only bite at renderer init. Three separate measurements:

* **Command line only** works on a box where `autoexec.cfg` does not set
  `r_mode` — including any box where a `%APPDATA%\Quake3\baseq3\autoexec.cfg`
  shadows the staged one. Verified on .246.
* **Command line only FAILS** where the staged `autoexec.cfg` IS read, because
  its `seta r_mode "6"` runs after `Com_StartupVariable` and before `R_Init`.
  Verified on .123 — 1024x768.
* **`+vid_restart` fixes that** and needs no autoexec change... but it makes
  **ioquake3 exit outright on .246** (Win7 + Radeon HD5450). Measured, twice.
  Not usable fleet-wide.

So the launcher does both, and re-appends `exec fleetres.cfg` to
`baseq3\autoexec.cfg` on every launch so it heals itself after a GAMESYNC
restores the library copy. **Deleting `seta r_mode "6"` from the library's
autoexec.cfg makes all of this unnecessary** — do that and the command line
alone is correct everywhere.

## F. Two honest caveats about aspect

1. **The 4:3-only titles cannot be made to look right on a 16:9 panel from
   software.** Best available is a correctly proportioned 4:3 mode (1280x960 on
   the four 1080p boxes) and the **monitor's own OSD set to preserve aspect**
   rather than fill. That is a one-time per-monitor setting; the alternative is
   a stretched picture whatever we write.
2. **id Tech 3 and GoldSrc are `vert-`**: at 16:9 with the default FOV you see
   *less* vertically than at 4:3, not more horizontally. `FR_FOV` (106 at 16:9,
   90 at 4:3) restores the 4:3 vertical field of view. It is set for Quake III
   in the example launcher; SoF2 and Jedi Academy should get the same
   (`seta cg_fov %FR_FOV%`). Unreal Engine 1 has the same property but its FOV
   lives per-player-class, so it is left alone deliberately.

## G. Two things that are NOT resolution bugs but were found on the way

* **`fullresolution=original` is why fleet desktops keep ending up at
  640x480.** Measured A/B on .145 with Descent 1: with `original`, DOSBox
  changes the *whole desktop* to 640x480 (confirmed by `DISPLAYCFG get`) and
  hands a 4:3 signal to a 16:9 panel; with `desktop` the desktop stays at
  1920x1080 and DOSBox pillarboxes correctly with `aspect=true`. Both boxes
  I found sitting at 640x480 mid-survey (.123 and .240) are consistent with
  this. All three DOSBox confs already ship `fullresolution=original`.
* **`.171` should get a `ResCapW`/`ResCapH` of 800x600** — its 3D is a Voodoo 2
  (hard 800x600 ceiling), invisible to every display-class scan. FLEETRES will
  otherwise hand it 1152x864, which the Intel 865G will render and the Voodoo 2
  cannot.
