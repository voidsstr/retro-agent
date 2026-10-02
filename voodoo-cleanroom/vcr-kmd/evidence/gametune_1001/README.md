# Every OpenGL/Glide game on `.124`, on our drivers: stability + tuning (2026-10-01)

User request: run each OpenGL/Glide game on `.124` with our drivers, make sure
it is stable, and tune it for the most the V5 6000 and a 2 GHz Athlon XP (twice
a period CPU) can give.

## Routing as found (`audit.json`, registry, PE imports)

- System ICD: `OpenGLDrivers\3dfx\DLL` = `retroicd.dll` = **our ICD 0.1.80**
  (md5 1f2096fc) - every opengl32-linked game (GoldSrc, UE1/UE2 OpenGLDrv,
  ioquake3, Serious Sam, Tribes 2, the Rebirth ports, GLQuake from the root).
- `system32\glide3x.dll` = **our h5 Glide** (07c96fd9).
- `system32\glide2x.dll` (AmigaMerlin, 94 KB) is a **Glide2->Glide3 wrapper**
  ("GlideXP": imports only KERNEL32/USER32, loads `glide3x.dll` by name) - so
  the Glide 2 titles (UE1 GlideDrv, Turok 2, Carmageddon 2, Descent 3) run on
  our h5 Glide too.
- Off our path: 3dfx MiniGL beside GLQuake (`VOODOO\`, the "3dfx Voodoo"
  shortcuts) and Quake II (`3dfxgl.dll`, unused while `gl_driver` is
  opengl32); RtCW's `gl\openglv5.dll`; a game-local `glide3x.dll` the bench's
  all-ours lane left beside Quake II and Quake III (an older pinned build that
  shadowed system32 in normal play - the bench now removes what it staged).

## Baseline sweep (`sweep_baseline/`, `lan_sweep.py`, 19:22-20:42)

49 shortcuts: **38 PASS, 11 CHECK, 0 FAIL; the board brought Glide up after
every title.** Table: `sweep_baseline/summary.md`. Most CHECKs are forced
closes - the keyboard focus sat on the agent watchdog's console
(`agentwd.cmd`), so the sweep withheld its close keys - not instability. The
real ones: Descent II Win95 (registration-card dialog), SoF II ("New Video
card detected", shown until a clean exit saves the new renderer string), Blue
Shift (SecuROM, known), Rainbow Six (Direct3D, out of scope).

Below the box's 1280x960: Descent 3 640x480, Tribes 2 800x600, UT2003 640x480,
Unreal Gold 1024x768, Hexen II 1024x768 (a fleet-wide cap), Carmageddon 2
640x480. (A Glide game does not change the GDI mode, so the "GDI mode seen"
column is only evidence for OpenGL titles.)

## Routing cleanup (2026-10-02)

Deleted the bench's game-local `glide3x.dll` (2df2f969) and `retrogl.dll`
(0.1.76) beside Quake II and Quake III; retail Quake III's `q3config.cfg`
`r_glDriver` back from `retrogl` to `opengl32` (the system ICD). Our ICD's
own load log (`C:\retrogl.log`) during the sweep: loaded by hl.exe x8,
quake2.exe, GLQUAKE.EXE, glh2.exe, ioquake3, quake3.exe, WolfSP/WolfMP,
jasp/jamp, SoF/SoF2/sof2mp, sin.exe, SeriousSam.exe, Tribes2.exe,
d1x/d2x-rebirth and UnrealTournament.exe (OpenGLDrv). UT2003/UT2004 ran
Direct3D (our HAL), not the ICD.

## Vsync (user: "On - tear-free"), `vsync_q3/`

The ICD puts `FX_GLIDE_SWAPINTERVAL=0` into the process when the environment
does not set it, and Glide applies that value over the game's own swap interval,
so **every OpenGL game ran without vsync**. One system variable decides it for
every Glide/OpenGL game on the box: `HKLM\...\Session
Manager\Environment\FX_GLIDE_SWAPINTERVAL = 1` (set 2026-10-02; new
processes see it after the next log-on). The bench's launchers set 0 themselves,
so timing runs are unaffected.

| Quake III demo four, 1280x960x32, cfg 5, ICD 0.1.81 | 2 buffers | 3 buffers |
|---|---|---|
| vsync off | 65.0 | 64.7 |
| vsync on | 55.9 | 56.0 |

Tear-free costs Quake III ~14% at this resolution (frames that miss an 85 Hz
refresh wait for the next). A third buffer does not help on this hardware, so
ICD 0.1.82 keeps two (`CHANGELOG.md`). Titles that run above 85 fps (Quake II,
GoldSrc, GLQuake) lose nothing visible.
