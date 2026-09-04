# Per-title resolution status — every staged title, one of three cases

**The rule (user directive, 2026-08-30):** *"all games that can be configured to
run in 1080p and the computer has an lcd with 1080p resolution capabiltiies, the
settings for all appliable games allow for 1080p resolution."*

`FLEETRES.EXE` answers the **panel** half at launch, per box. This file records
the **engine** half — which of three cases each title falls into, and the
measurement behind it. A title silently left at 1024x768 on a 1080p panel is the
failure this exists to prevent, so "genuinely incapable" is written down rather
than implied.

| case | meaning |
|---|---|
| **1080p** | takes the panel's full mode via `FR_W`/`FR_H`; on the four 1080p LCDs that is 1920x1080 |
| **native** | correctly gets the box's own non-1080p native mode (the four CRT boxes) — the same launcher, a different answer |
| **capped / 4:3** | the engine has a real ceiling or no widescreen mode; it gets the largest correctly-proportioned mode it can reach |
| **incapable** | no configurable resolution at all |

## GAMESYNC now applies the same answer at SYNC time (agent v1.81.0+)

Everything below is still true and `FLEETRES.EXE` still runs from every
launcher. What changed is that the **agent** also detects the monitor and writes
each title's own configuration at the end of that title's sync — see
`agent/shared/gameres.h` and the GAMERES section of `CLAUDE.md`.

It exists because the launcher half is structurally unable to fix one case:
`gs_merge_reg()` applies a title's staged `install.reg` **after** any launcher
has run, and `HalfLife1`'s pins the shared GoldSrc mode key — the one key every
GoldSrc title on the box uses — on every machine on every sync.

Two consequences for this table:

* the **`launcher`** column is no longer the only writer for a title whose mode
  lives in a file or the registry. The rule table in `gameres.h` mirrors those
  lines and `tests/python/test_gameres_mirror.py` fails if the two disagree.
* a title whose mode is set purely on a **command line** — Quake 1, Hexen II,
  Descent 3, Halo, Doom 3, Jedi Academy's `+set` — is served ONLY by the
  launcher, deliberately. There is nothing on disk for a sync-time pass to
  write.

## The panels — read back from each box's own DEPLOYED `FLEETRES.EXE`

Not from notes, and not from my copy: this is `FLEETRES.EXE -cmd` run out of
`C:\Games\HalfLife1\` on each machine, i.e. the exact answer the launchers get.

| box | monitor | EDID | panel | wide target | 4:3 target | Hz | `FR_Q2MODE` | `FR_Q3MODE` |
|---|---|---|---|---|---|---|---|---|
| .123 | DELL P2312H | yes | **LCD** | **1920x1080** | 1280x960 | 60 | 8 | 7 (1152x864) |
| .124 | — | **no** | CRT | 1024x768 | 1024x768 | **75** | 6 | 6 |
| .133 | — | **no** | CRT | **1280x960** | 1280x960 | **85** | 8 | 7 |
| .143 | — | **no** | CRT | 1024x768 | 1024x768 | **100** | 6 | 6 |
| .145 | DELL E2414H | yes | **LCD** | **1920x1080** | 1280x960 | 60 | 8 | 7 |
| .171 | Gateway VX1120 | yes | CRT | **800x600** ¹ | 800x600 | 60 | 4 | 4 |
| .240 | DELL E2313H | yes | **LCD** | **1920x1080** | 1280x960 | 60 | 8 | 7 |
| .246 | HP 2511 | yes | **LCD** | **1920x1080** | 1280x960 | 60 | 8 | 7 |

¹ `.171`'s 3D is a **Voodoo 2** (hard 800x600 ceiling) behind an Intel 865G, so
it carries `HKLM\Software\RetroAgent\ResCapW/ResCapH = 800/600`. The tube
itself would take far more; the card would not.

**Three boxes now present no EDID** (`.124`, `.133`, `.143`) — and `.133` had it
this morning, measured at 37x28 cm. It lost it across a reboot, at which point
the old fallback handed it 1280x1024 back: a 5:4 image on a 4:3 tube. No EDID
now assumes a 4:3 tube, which is why `.133` reads 1280x960 above. Putting those
three monitors on a port that carries DDC would turn three inferences back into
measurements.

**The refresh column is why `FR_HZ` exists.** Halo's `-vidmode w,h,hz` shipped a
hardcoded `60`, which throws away 100 Hz on `.143`, 85 on `.133` and 75 on
`.124`. A refresh constant is the same defect as a resolution constant, one
field to the right.

## Every title

| title | case | how the mode is set | evidence |
|---|---|---|---|
| **BF1942** | 1080p | `game.setGameDisplayMode %FR_W% %FR_H% 32 0` in **both** the Default and Custom profiles | a box that has opened the video menu reads Custom, so writing only Default is a silent half-fix |
| **Carmageddon1** | DOSBox | `[sdl] fullresolution %FR_DOSFULLRES%` — `desktop` on an LCD, `original` on a CRT | a DOS game has no 1080p mode; correct-aspect pillarboxing is the right answer, and `original` retargets the whole desktop |
| **Carmageddon2** | **incapable** | render device only (nGlide swap) | 1998 Glide title; its mode comes from the Glide device, which offers 512x384/640x480/800x600 |
| **CounterStrike16** | 1080p | `hl.exe -full -w %FR_W% -h %FR_H%` | GoldSrc; proven earlier |
| **Descent1** | DOSBox **+ 1080p** | the three DOSBox launchers get `%FR_DOSFULLRES%`; **`Play Descent - Rebirth.bat`** is a native Win32 DXX build sharing Descent 2's engine and gets `-kv DESCENT.CFG ResolutionX/Y` | one title, two engines, two right answers |
| **Descent2** | 1080p | `-kv DESCENT.CFG ResolutionX/Y` | DXX-Rebirth; its cfg is bare `key=value` with no `[section]`, which is why `-kv` exists |
| **Descent3** | 1080p | `main.exe -Width %FR_W% -Height %FR_H%` | switches verified in the binary |
| **DeusEx** | 1080p | UE1 `[WinDrv.WindowsClient] FullscreenViewportX/Y` + `StartupFullscreen` | |
| **FarCry** | 1080p | `-setline System.cfg r_Width/r_Height` (owned by the far-cry lane) | |
| **HalfLife1** | 1080p | 5 launchers, `-full -gl -w %FR_W% -h %FR_H%` | was hard-coded `-w 1024 -h 768` |
| **HexenII** | **1080p** | `glh2.exe -width %FR_W% -height %FR_H%` | **measured on .145: window class `HexenII`, 0,0–1920x1080, rendering.** It was assumed to share GLQuake's fixed table. It does not. |
| **HiddenAndDangerous** | **1080p** | `-reg HKLM Software\Lonely Cat Games\...\Config` Display width/height/bitdepth/Fullscreen | **measured on .145: `IGraph_Window` 1920x1080, true 16:9, rendering.** `install.reg` had pinned 800x600 on all eight boxes |
| **JediAcademy** | 1080p | id Tech 3 `r_mode -1` + `r_customwidth/height` | **measured on .145: `jasp.exe` AND `jamp.exe` both 1920x1080** |
| **JediKnightDF2** | **incapable** | — | Sith engine (not id Tech 3): stores a `displayMode` **index** into a list the 3D device supplies; no width/height pair exists |
| **JediKnightMotS** | **incapable** | — | same engine |
| **MaxPayne** | 1080p | `-reg HKCU Software\Remedy Entertainment\Max Payne\Video Settings` | `install.reg` had pinned 800x600; the engine reads only the registry (MFC `SetRegistryKey`) |
| **Quake1** | **capped 1280x960** | `GLQUAKE.EXE -width %FR_W43% -height %FR_H43%`, `-cap 1280 960` | **measured on .145: `Quake Error: "Specified video mode not available"` at BOTH 1920x1080 and 1600x1200; fullscreen at 1280x960.** The old cap of 1024x768 was a guess |
| **Quake2Complete** | **incapable** | `gl_mode %FR_Q2MODE%` | id Tech 2's table is fixed: 320x240 … 1024x768 … 1600x1200, **no custom mode and no 16:9 entry**. Read out of `quake2.exe` |
| **Quake3-TeamArena** | 1080p | `r_mode -1` + custom w/h, both by command line and `fleetres.cfg` | **measured on .145: `ioquake3` SDL_app 1920x1080 through its launcher; `quake3.exe` 1920x1080** |
| **RedAlert2** | 1080p | `-ini RA2.INI / RA2MD.INI [Video] ScreenWidth/Height` | **measured on .145: `Red Alert 2` window 1920x1080**, `.ini` written by the launcher. (The render frame was blocked by another session's installer modal.) |
| **RedFaction** | 1080p | `-reg HKLM SOFTWARE\Volition\Red Faction` Resolution Width/Height/Bit Depth | value names read out of `rf.exe`; nothing in the tree could set them |
| **RedneckRampage** | DOSBox | as Carmageddon 1 | |
| **Shogo** | **1080p** | `-setline autoexec.cfg screenwidth/screenheight`, in **both** launchers | **measured on .145: window class `LithTech` 1920x1080, rendering.** Backticks in the launcher become the double quotes LithTech's format requires |
| **SiNGold** | **incapable** | `gl_mode %FR_Q2MODE%` in base **and** the 2015 mission pack; the software launcher gets `sw_mode %FR_Q2MODE%` | same fixed id Tech 2 table, verified in `sin.exe`. Covering `base/` alone left Wages of SiN pinned at 1024x768 on every box |
| **SoldierOfFortune** | **incapable** | `gl_mode %FR_Q2MODE%` | same table in `SoF.exe` (modes 0–2 marked Unsupported) |
| **SoldierOfFortune2** | **incapable (1152x864 max)** | `r_mode %FR_Q3MODE%` — **not** `-1`, and **not** `FR_Q2MODE` | **measured on .145: with an identical `-1` + custom-1920x1080 config, `quake3.exe`/`jasp.exe`/`jamp.exe` gave 1920x1080 and `sof2mp.exe` gave 640x480.** SoF2's fork has no `-1` branch (though it *does* carry the `r_customwidth` string, so the symbol table proves nothing). Through the index it reaches 1152x864 — **verified: `SoF2 MP` window 1152x864, `fleetres.cfg` `seta r_mode "7"`** |
| **StarCraft** | **incapable** | — | 1.16.1 is hard-locked to 640x480 |
| **SystemShock2** | **incapable** | — | Dark engine **without NewDark** (no `d3dx9_43.dll`, no `NVScript.osm` — Thief 2 has both). `game_screen_size` is a NewDark cvar; vanilla Dark is 640x480 |
| **Thief2** | 1080p | `-setline cam.cfg game_screen_size %FR_W% %FR_H%` | has NewDark, which is genuinely widescreen-aware |
| **ThiefGold** | **incapable** | — | same as System Shock 2: no NewDark in this tree |
| **TiberianSun** | **1080p** | `-ini SUN.INI [Video] ScreenWidth/Height`, **cap removed** | **measured on .123 in-game, and reproduced on .145 from a PURGED tree: `Tiberian Sun` window 1920x1080, `SUN.INI` written to 1920/1080.** Its own Display Options list stops at 800x600, but the CnCNet patch reads SUN.INI directly and bypasses that list |
| **Turok2** | **incapable (1024x768 max)** | one **boolean per mode** in `Data\config.ned`, largest 4:3 entry that fits — **verified on .145: `1024^x^768 1`, every other mode 0** | the list is compiled into `Video_D3D.dll`: 320x240 / 512x384 / 640x480 / 800x600 / 1024x768 / 1280x1024. No 1080p entry, no custom mode. 1280x1024 is explicitly written back to 0 — it is 5:4 |
| **UT2004** | 1080p | UE2 `FullscreenViewportX/Y` | |
| **UnrealGold** | 1080p / **native** | UE1 `FullscreenViewportX/Y` + per-box render device | **verified end to end on .171**: the wrapper is moved aside, `Unreal.ini` gets `GlideDrv.GlideRenderDevice` and 800x600, and `Unreal.log` reads `Found Glide: 2.56.00.0459`, `fbRam=4 nTexelfx=2` (the 12 MB Voodoo 2) and **`grSstOpen` SUCCEEDS** where through the wrapper it failed `(2, 3)`. A Glide fullscreen surface on a pass-through card can never be screenshotted, so the engine log is the evidence |
| **UnrealTournament** | 1080p | UE1 `FullscreenViewportX/Y` | |
| **UnrealTournament436** | 1080p | UE1 `FullscreenViewportX/Y` | |
| **AliensVsPredator** | — | not staged | CLOSED as BLOCKED: graphics fastfile content absent from every distribution |

## Two honest caveats about aspect, unchanged

1. **A 4:3-only engine cannot be made to look right on a 16:9 panel from
   software.** The best available is a correctly proportioned 4:3 mode (1280x960
   / 1152x864 on the four 1080p boxes) *and the monitor's own OSD set to preserve
   aspect rather than fill*. That is a one-time per-monitor setting; the
   alternative is a stretched picture whatever we write.
2. **id Tech 3 and GoldSrc are `vert-`**: at 16:9 with the default FOV you see
   *less* vertically, not more horizontally. `FR_FOV` (106 at 16:9, 90 at 4:3)
   restores the 4:3 vertical field of view and is set for Quake III, Jedi
   Academy and SoF2.

## Only ONE cap remains in the library

`Quake1`, at the measured 1280x960. `tests/python/test_fleetres_staging.py`
fails if a second appears, because both caps this project has shipped were
inherited from taste rather than measured, and both were wrong.

## The id Tech 3 sweep was exhaustive, and here is how

`r_mode -1` is not universal in that family, so "check the others" needed a way
to *find* the others that does not rely on remembering which games are Quake III
forks. Every id Tech 3 binary carries its mode table as strings, and one entry
is distinctive enough to be a fingerprint:

```bash
strings -a <exe> | grep -q "^Mode 11: 856x480 (wide)"
```

Run over every `.exe` in the library, that returns **exactly six** binaries:

| binary | `r_mode -1` | measured on .145 |
|---|---|---|
| `Quake3-TeamArena/quake3.exe` | works | 1920x1080 |
| `Quake3-TeamArena/ioquake3.x86.exe` | works | 1920x1080 (through its launcher) |
| `JediAcademy/jasp.exe` | works | 1920x1080 |
| `JediAcademy/jamp.exe` | works | 1920x1080 |
| `SoldierOfFortune2/sof2mp.exe` | **broken** | **640x480** |
| `SoldierOfFortune2/SoF2.exe` | same engine, same treatment | — |

There is **no** Return to Castle Wolfenstein, Elite Force or Jedi Knight II in
this library, so nothing else on that engine is outstanding.
`JediKnightDF2`/`JediKnightMotS` are the **Sith** engine, not id Tech 3 — they
do not carry the string, which is the same evidence that says they have no
width/height pair to write.
