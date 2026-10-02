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
