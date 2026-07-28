---
name: driver-install
description: Fully install video drivers on a fleet machine end-to-end — core driver PLUS the per-game binary sweep. For 3dfx Voodoo cards offers two stacks, "vintage" (the real 3dfx H5 driver built from the leaked source in the retro-3dfx repo — display+miniport+D3D HAL+glide2x/glide3x+vintage-source OpenGL ICD) or "cleanroom" (the clean-room MesaFX ICD + open Glide built in this repo). Automatically finds every game that needs opengl32.dll/3dfxgl.dll/glide DLLs copied in (or wrapper DLLs like nGlide/ddraw shims retired) so the driver is fully installed for ALL games, not just system32. Use when the user says "install the 3dfx driver", "set up the Voodoo drivers on .143", "install drivers everywhere", "make sure all games use our driver", or after deploying a new driver build.
---

# Driver Install (core driver + full per-game sweep)

Installing a video driver on a retro box is TWO jobs, and skipping the second
one has repeatedly produced "the fix didn't work" ghosts:

1. **Core install** — display driver / miniport / system32 runtime DLLs.
2. **Per-game sweep** — Windows' LoadLibrary checks the **game's own directory
   before system32**, and retro game installs are full of app-local
   `opengl32.dll`, `glide2x.dll`, `ddraw.dll` copies and modern-PC wrappers
   (GOG ships **nGlide**, repacks ship "Win10 fixed" ddraw shims). Until every
   game-local copy is updated or retired, games silently run the WRONG driver.
   Proven cost: a full day on .143 (games kept loading an old game-local ICD),
   UT99 "garbled textures" (nGlide shadowing the real glide2x), RA2 black
   screen (ddraw wrapper).

**Fleet hard rules apply:** NEVER reboot without explicit user approval; never
raw-copy WFP-tracked names into system32 (see `deploy-3dfx-driver` for the WFP
rules); one fullscreen 3D game at a time.

## Step 0 — Identify card + choose the stack

`VIDEODIAG` + HWID (`REGREAD ... Enum\PCI`, look for `VEN_121A` = 3dfx).

For 3dfx cards there are TWO installable stacks. If the user didn't say which,
ask — one question, these options:

| | **vintage** | **cleanroom** |
|---|---|---|
| What | The real 3dfx H5 driver, built from the leaked source (sibling **retro-3dfx** repo) with our fixes (see its `VINTAGE-FIXES.md`) | Clean-room MesaFX ICD + open Glide, built in THIS repo (`voodoo-cleanroom/`, `scripts/3dfx/`) |
| Covers | Display/2D + **Direct3D HAL** + Glide2/3 + OpenGL ICD (vintage-source, "3Dfx [retro3dfx x.y]") | OpenGL ICD + Glide2/3 only (game-local; no display/miniport — the box keeps its current display driver) |
| Card fit | Voodoo4/5 (H5/Napalm; V3 via WFP package) | Voodoo3 (h3 builds) and Voodoo5 (h5 builds) |
| Proven on | **.143** (V5 5500 — daily driver: D3D games, Glide, GL) | **.124** (Voodoo3 — Q3/Q2/CS on clean-room GL+glide) |
| Pick when | The box should run EVERYTHING (D3D + Glide + GL) on real 3dfx code | No vintage display driver wanted/possible, or A/B-ing the clean-room stack |

Non-3dfx cards: this skill's sweep concepts don't apply; do a normal core
install (SetupAPI) and stop.

## Step 1 — Core install

**vintage, fresh install** (box on in-box/MS driver): follow the
`deploy-3dfx-driver` skill end-to-end (package build, INF trim, HWID check,
SetupAPI, backup, verify). It ends with system32 holding `3dfxv5d.dll` /
`3dfxv5m.sys` (WFP-safe names) + `glide2x.dll`/`glide3x.dll`.

**vintage, upgrade in place** (box already on our vintage driver — check
`EXEC cmd /c dir C:\WINDOWS\system32\3dfxv5d.dll`): the proven direct-swap —
UPLOAD new build to `C:\RETRO_AGENT\`, byte-verify by DOWNLOAD-readback md5,
`move /Y` the live DLL aside to a dated `.bak`, `copy /Y` the new one in,
re-verify md5, then reboot (with approval). Same pattern for glide2x/glide3x
(not WFP-tracked). Artifact sources (sibling repo):
```
display : retro-3dfx/toolchain-3dfx/prefix/drive_c/3dfx/H5/W2K/Src/Video/Displays/H5/objfre/i386/3dfxvs.dll  -> 3dfxv5d.dll
miniport: .../Miniport/H5/objfre/i386/3dfxvsm.sys                                                            -> 3dfxv5m.sys
glide   : .../drive_c/3dfx/H5/BIN/glide2x.dll, glide3x.dll
ICD     : .../drive_c/3dfx/SWLIBS/OPENGL/GLIDE3X/release/opengl.dll   (deployed per-game, Step 2 — NEVER to system32)
```
Run `bash retro-3dfx/tests/predeploy.sh` first — non-zero exit = do not deploy.

**cleanroom**: nothing goes in system32 except (optionally) glide; the stack
is game-local. Artifacts in THIS repo:
```
glide   : scripts/3dfx/out/glide2x.dll, glide3x.dll (+ per-card glide3x_h3_voodoo3 / glide3x_h5_voodoo5 — rename to glide3x.dll for the target card)
ICD     : the MesaFX build output (voodoo-cleanroom/build-mesafx-retail.sh) — game_sweep.py --icd <path> if not auto-found
```

## Step 2 — Per-game sweep (the part everyone forgets)

Run the automated sweep (dry-run first — it prints a plan and touches nothing):

```
python3 .claude/skills/driver-install/game_sweep.py <host> --flavor vintage   # or cleanroom
# review the plan, then:
python3 .claude/skills/driver-install/game_sweep.py <host> --flavor vintage --apply --kill
```

What it does (see the script header for full policy):
- **Scans** all fixed drives for game-local 3dfx-relevant binaries:
  `opengl32.dll`, `3dfxgl.dll`, `3dfxogl.dll`, `glide2x.dll`, `glide3x.dll`,
  `ddraw.dll` — skipping `\WINDOWS\` (system copies are never touched).
- **Classifies** each by md5 against the chosen stack's reference binaries:
  ours-current / ours-stale / **wrapper** (nGlide, dgVoodoo, ddraw shims —
  anything that isn't ours in a game dir) / unknown.
- **Acts** (`--apply`):
  - GL loader names (`opengl32/3dfxgl/3dfxogl.dll`) in game dirs → replaced
    with the chosen ICD (first replacement keeps a `.pre` backup). Multiple GL
    names in one dir are kept identical (Q3 loads whichever `r_glDriver` says).
  - `glide2x/glide3x` shadows in game dirs → **vintage:** retired to
    `.wrapper.bak` so system32's real glide wins; **cleanroom:** replaced with
    the clean-room glide.
  - `ddraw.dll` in a game dir → retired to `.wrapper.bak` (always — wrappers
    break real DirectDraw hardware; RA2 lesson).
  - **UT99** (`UnrealTournament.exe` + ini found) → switches the ini to
    `GlideDrv.GlideRenderDevice` (native Glide beats any GL path on 3dfx) and
    retires its nGlide shadow.
  - `--kill` first taskkills known game processes (a loaded DLL is locked;
    every copy is verified by readback md5, not assumed).
- **Reports** a per-game table: dir, files found, classification, action,
  result — relay this table to the user.

## Step 3 — Verify

- Core: `VIDEODIAG` (vintage: our driver/INF, sane resolution — 640x480x4 =
  fallback = failure), registry ring alive for vintage
  (`REGREAD ... Services\3dfxvs\Device0` → `RLogSeq`).
- Per-game: launch ONE GL game, check the renderer string in its console/log —
  vintage-ICD reports `3Dfx [retro3dfx x.y.z]`. For fullscreen games use the
  game's OWN screenshot (e.g. UT console `SHOT`, Q3 `screenshot`) — the agent's
  GDI `SCREENSHOT` garbles in exclusive fullscreen and cannot judge rendering.
- Offer the `retro-benchmark` / `driver-bench` skill for before/after numbers.

## Known per-game facts (encoded in game_sweep.py, kept here for humans)

- **UT99 GOG**: ships nGlide (`glide2x.dll` 1,310,720 in `System\`) + a modern
  UTGLR `OpenGlDrv.dll` — both wrong on real 3dfx. Native `GlideDrv` +
  real glide2x renders perfectly (~55-58 fps vsync-capped on a V5@1GHz).
- **GoldSrc (CS 1.6/HL)**: GL driver name is `3dfxgl.dll`
  (`HKCU\Software\Valve\Half-Life\Settings\EngineGLDriver`); on the vintage
  stack its Direct3D mode also works (driver ≥ cf3ab3e) and benches faster
  than GL.
- **Quake III**: carries BOTH `opengl32.dll` and `3dfxogl.dll`; `r_glDriver`
  picks — keep them identical.
- **RA2/Yuri**: repack ships a "Win10 fixed" `ddraw.dll` wrapper — must be
  retired; native DirectDraw works on the vintage driver. Launch `RA2MD.exe`.
- **UT2004**: use the Direct3D renderer on the vintage stack
  (`RenderDevice=D3DDrv.D3DRenderDevice`), TextureDetail ≤ Higher (V5 texture
  RAM); its OpenGL path aborts in the ICD.
