---
name: voodoo3-driver-dev
description: Build, test, diagnose, fix, and deploy OUR clean-room 3dfx driver stack (MesaFX OpenGL ICD + open Glide, voodoo-cleanroom/) for the Voodoo 3 box .124. Use when the user reports a rendering/perf/stability problem in an OpenGL or Glide game on the Voodoo3, asks to rebuild or update "our driver" / MesaFX / retrogl / glide3x, wants a driver fix verified or deployed on .124, or asks to A/B a driver change. NOT for the Voodoo5/.143 vintage stack (use voodoo5-driver-dev).
---

# Voodoo 3 driver work — the clean-room stack (voodoo-cleanroom, box .124)

You are working on **OUR open-source stack**: the MesaFX OpenGL ICD
(`retro3dfx-gl` fork) + open Glide (`retro3dfx-glide` fork), built in
`voodoo-cleanroom/` in THIS repo. Versions are **0.1.N**, renderer string
`Mesa Glide v0.62 ... [voodoo-cleanroom 0.1.N]`. Read the **Driver Stack Map
in CLAUDE.md first** — there are two 3dfx codebases; a `0.3.x` / `SST_*.c` /
`SWLIBS` bug is the OTHER stack (voodoo5-driver-dev), not this one.

**Target box:** `.124` (192.168.1.124) — XP SP3 "ADMIN", Voodoo3 AGP, Windows
on **D:** (dual-boot). Deployed stack is a HYBRID: our ICD + our glide3x on
top of the vintage H5 display/D3D driver (`3dfxv3d.dll`).

**HARD RULE (user directive 2026-08-04): never edit/build/deploy `retro-3dfx/`
driver code — that repo is the Voodoo 5 (`.143`) lane.** Everything you ship
comes from `voodoo-cleanroom/`. The vintage `3dfxv3d.dll` on .124 is a legacy
dependency (our `vcr-disp` can't drive 2D+D3D yet), not an invitation to patch
it. If a fix seems to belong in the display/D3D HAL, that is **out of scope** —
implement it in our stack or tell the user it needs the vcr-disp work; ask
before touching the deployed vintage binary. See CLAUDE.md → Driver Stack Map.

**Key files/docs (read before deep work):**
- Source: `voodoo-cleanroom/build/retro3dfx-gl/src/mesa/drivers/glide/fx*.c`
  (ICD), `voodoo-cleanroom/build/retro3dfx-glide/` (glide2x/glide3x).
- Fix ledger: `voodoo-cleanroom/CHANGELOG.md` (every 0.1.x fix). Debug lore:
  `voodoo-cleanroom/DEBUGGING-NOTES.md`, `RUNNING-GAMES.md`,
  `OPTIMIZATIONS.md`. Cross-repo findings:
  `/home/voidsstr/development/retro-3dfx/FINDINGS.md`.
- Fleetbook: `python3 scripts/retro_fleetbook.py search <keywords>` BEFORE
  diagnosing; `log --host 192.168.1.124 --summary ...` after every change.

## Build

```bash
cd /home/voidsstr/development/retro-agent
bash voodoo-cleanroom/build-stack.sh          # once per session: glide2x/glide3x + GL SDK
bash voodoo-cleanroom/build-mesafx-retail.sh  # -> voodoo-cleanroom/out/opengl32_retail.dll (~2.7MB)
```

- Toolchain: mingw `i686-w64-mingw32-gcc` (gcc-13), `-O2 -ffast-math
  -march=pentium3 -mtune=pentium3 -mfpmath=sse`.
- Version = `voodoo-cleanroom/VERSION` + `.buildnum` → 0.1.N. **Every shipped
  build must carry a NEW 0.1.N** and a CHANGELOG.md entry — verify the version
  is baked in: `strings out/opengl32_retail.dll | grep voodoo-cleanroom`.
- "retail" links the retail AmigaMerlin glide import lib (the deployed
  config); the non-retail path links our retro3dfx-glide.

## Test (gate — before ANY deploy)

```bash
bash tests/run_all.sh    # native + python suite, runs in <1s on this host
```

Non-green = do not deploy. When a fix is hardware-verified, add its
regression test **in the same commit**: a `tests/native/test_<fix>.c`
pure-logic invariant (cite source file:function + fix version, assert BOTH
fixed and old-buggy values) or a `tests/python/test_*.py` case; update
`tests/README.md`'s fix→test table. A fix without a regression test is not
done.

## Deploy to .124 (the game-local shadow rule is CRITICAL)

LoadLibrary checks the **game's directory before system32** — fleet games
carry app-local GL DLLs and will silently keep an old ICD if you only update
system32. Deploy = system32 alias **plus every game-local copy**.

1. Build + test green (above). Use `mcp__retro__retro_upload` to push files;
   `retro_command` for the EXEC steps (host `192.168.1.124`).
2. Inventory copies:
   `EXECW 180 cmd /c dir /s /b C:\opengl32.dll C:\3dfxogl.dll D:\opengl32.dll`
   (Windows is on D:; games may live on either drive).
3. **Kill every GL game first** (`EXEC taskkill /f /im quake3.exe` etc.) — a
   loaded DLL is locked and `copy /Y` silently fails; check each copy output
   for `1 file(s) copied`.
4. Upload once to a staging path (e.g. `C:\RETRO_AGENT\stage\opengl32.dll`),
   keep a `.preNNN` backup of each target on first replacement, then
   `EXEC cmd /c copy /Y` over: system32 alias **`retrogl.dll`** + every
   game-local `opengl32.dll`/`3dfxogl.dll`. **NEVER touch
   `system32\opengl32.dll` or `dllcache\opengl32.dll`** (Microsoft's, WFP).
   Game-local files are never WFP-tracked — no WFP handling needed there.
5. glide3x.dll (when it changed): system32 + game-local copies, same rules.
6. Verify by **renderer string, not file size** (builds are often same-size):
   game GL console must show `[voodoo-cleanroom 0.1.N]` with the NEW N.
   `retro_screenshot` after launching the game, or use the game's console.
7. Log the change in fleetbook. For perf claims, run the **driver-bench**
   skill (it quiesces AI engine/wallpaper/etc. — never bench without that).

Full deploy detail: the **deploy-3dfx-driver** and **driver-install** skills.
Never REBOOT without explicit user approval (fleet rule).

## Diagnose

- **Which ICD is the game actually loading?** — the #1 trap. Renderer string
  first; a "fixed" bug that persists is usually a stale game-local DLL.
- `VIDEODIAG` / `SCREENSHOT` / `PCISCAN` via `mcp__retro__retro_command` for
  machine state; `FX_DUMP_FRONT` env (MesaFX 0.1.35+) dumps the front buffer
  for pixel-level checks; GDI screenshots garble in exclusive fullscreen —
  prefer windowed test modes.
- A/B: swap `.preNNN` backup vs new build game-locally, same map/timedemo.
- Search fleetbook + CHANGELOG.md + FINDINGS.md before rediscovering a known
  gotcha (swap-interval, LOD bias, gamma/dither, refresh snap-down, cursor
  overlay all have history).

## Fix workflow

1. Reproduce + localize (which layer: ICD `fx*.c`, glide2x/glide3x, or the
   display/D3D HAL). **If it localizes to the display/D3D HAL, STOP** — that
   code lives in `retro-3dfx/` (Voodoo 5 lane) and is off-limits. Report it as
   a gap in our stack (`vcr-disp`) and let the user decide; do not patch or
   rebuild the vintage tree.
2. Edit the source in `voodoo-cleanroom/build/retro3dfx-gl/` (or
   `retro3dfx-glide/`), rebuild, bump 0.1.N, CHANGELOG entry.
3. `bash tests/run_all.sh` green → deploy (above) → verify on hardware by
   renderer string + the visual/perf symptom.
4. Same commit: regression test + CHANGELOG + (if milestone) CLAUDE.md line.
   Record findings in fleetbook and in `voodoo-cleanroom/DEBUGGING-NOTES.md`
   (our own docs — `retro-3dfx/FINDINGS.md` belongs to the other lane).
5. **Never regress a shipped fix** — the driver-change policy (memory:
   driver-change-policy) requires gated, default-safe behavior for anything
   risky.
