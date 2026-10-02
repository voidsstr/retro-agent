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

**CORRECTED 2026-10-02 09:45.** The first table (`vsync_q3_cpu_starved/`, 09:32) was
measured beside a Carmageddon 2 process that the sweep's forced close had NOT
ended. It had spun one thread at 100% CPU for 14 hours (below). On a quiet CPU,
ICD 0.1.82, Quake III retail demo four, cfg 5:

| | vsync off | vsync on, 2 buffers | vsync on, 3 buffers |
|---|---|---|---|
| 1280x960x32 (`vsync_q3/`) | **76.8**, 76.8 | 55.4 | 55.3 |
| 1024x768x32 (`vsync_q3_1024/`) | **104.3**, 104.3 | 74.6 | - |

Tear-free costs Quake III ~28% at either resolution: a frame that misses an 85 Hz
refresh waits for the next one. A third buffer does not help on this hardware,
so ICD 0.1.82 keeps two (`CHANGELOG.md`). Titles far above 85 fps (Quake II,
GoldSrc, GLQuake) lose nothing visible.

## A sweep leftover burned the CPU for 14 hours (found 2026-10-02)

`CARMA2_HW.EXE` from the sweep's first title (19:22) was still running at 09:50
the next day. One thread was Ready the whole time, using ~77% kernel + ~22%
user CPU (WMI: ~10.4 h of kernel time), and CPU load was 100%. It held nothing
in our driver (`vcrctl info`: `exclusive_pid` 0, no SLI session), and the
agent's `PROCKILL` ended it at once. The sweep's `taskkill /f` had not, and the
sweep never checked. Every title after it, and the 09:32 measurements, ran
beside it. The sweep results still stand as pass/fail; their timing does not.
`lan_sweep.py` now verifies a forced close (agent `PROCKILL` for a survivor)
and fails and stops on one (`tests/python/test_lan_sweep_force.py`).
Carmageddon 2 had never had the keyboard focus (the agent watchdog's console
had it), which is the likely start of its spin.

## Per-box tuning applied (`scripts/benchmarks/v56k_tune.py`, 2026-10-02 09:42)

48 values in 14 unstaged per-box config files; a second run changes 0.
- Quake II (baseq2/xatrix/rogue/ctf), SiN (base/2015): full-colour textures
  (`gl_ext_palettedtexture 0`), trilinear.
- SoF: trilinear.
- Quake III retail, Jedi Academy SP/MP, SoF II SP/MP, RtCW SP/MP: the
  full-quality preset (`r_picmip 0`, 32-bit textures and colour, lightmaps,
  uncompressed, trilinear, `r_subdivisions 4`).
- `r_lastValidRenderer` set to the deployed ICD's string. An id Tech 3 game that
  sees a "new card" resets to its low preset, and our renderer string carries
  the build number. That is how RtCW had dropped to `r_picmip 2` with vertex
  lighting and SoF II to `r_picmip 3` behind a "New Video card detected" box.
  Re-run the script after every ICD update (or move the build tag out of
  GL_RENDERER - proposed).
- ioquake3 (Quake III / Team Arena shortcuts) was already at full quality.

