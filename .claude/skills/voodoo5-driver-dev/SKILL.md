---
name: voodoo5-driver-dev
description: Build, test, diagnose, fix, and deploy the VINTAGE-source 3dfx H5/Napalm driver stack (retro-3dfx repo — display driver + D3D HAL + miniport + Glide + vintage SGL OpenGL ICD) for the Voodoo 5 box .143 (V5 5500) - and .133 (V5 6000, 4-chip) ONLY if that card is refitted, it was pulled 2026-08-31. Use when the user reports a D3D/DirectDraw/2D/mode-set problem, a Voodoo5 rendering or crash issue, asks to rebuild 3dfxv5d/3dfxv5m/the vintage driver, wants the flight-recorder ring read, or wants a driver fix verified/deployed on .143 or .133. NOT for the clean-room MesaFX stack on .124 (use voodoo3-driver-dev).
---

# Voodoo 5 driver work — the vintage H5 stack (retro-3dfx repo, boxes .143 & .133)

You are working on the **VINTAGE-SOURCE stack**: 3dfx's leaked H5/Napalm tree,
built for XP under Wine, in the sibling repo
**`/home/voidsstr/development/retro-3dfx/`**. Its OpenGL ICD is the vintage
SGI/3dfx SGL (`SST_*.c`, versions **0.2.x–0.3.x**) — NOT our MesaFX. Read
`retro-3dfx/CLAUDE.md` and the **Driver Stack Map in this repo's CLAUDE.md**
before touching anything; conflating the two stacks wastes hours.

**Target boxes:**
- `.143` (192.168.1.143) — Athlon 1GHz "1GHZ", **Voodoo5 5500** AGP, XP SP3.
  The V5 5500 is the **SECOND adapter** - a GeForce 6800 drives the panel, so
  `docs/fleet-inventory.md` and `docs/staged-library.md` both list "GeForce 6800"
  as this box's *Display GPU*. That is not a contradiction and not a swapped
  card; do not "correct" it. Since `.133`'s V5 6000 was pulled (2026-08-31),
  this is the fleet's only Voodoo5.
- `.133` (192.168.1.133) — dual P3-700 "P3-DUAL". **THE V5 6000 IS PHYSICALLY GONE**
  as of the 2026-08-31 rescan - `Enum\PCI` has no `VEN_121A` key at all and the box
  runs a GeForce4 Ti 4600. **Do not target .133 in this lane** without a fresh
  `SYSINFO` proving the card is back. The rest of this entry applies only then:
  **Voodoo5 6000** (4× VSA-100
  behind a HiNT bridge, 256MB mode = 64MB/chip, 4-way SLI verified), XP SP3.
  V56K-specific rules: cooldowns between flat-out timedemos, ONE fullscreen 3D
  app at a time, bench via `bench-safe.py` only, keep `FX_GLIDE_NUM_CHIPS`/
  `FX_GLIDE_FBRAM` unset — see `retro-3dfx/V56K-SLI-FINDINGS.md` and the
  outcome banner atop `retro-3dfx/V56K-256MB-READINESS.md`.

Hard freeze = NIC dead = needs a physical power cycle (tell the user).
This stack's display/D3D HAL also runs on `.124` as `3dfxv3d.dll` (Voodoo3
build) under the hybrid stack — HAL fixes can apply to all three boxes.

**What ships:** `3dfxv5d.dll` (display + DDraw + D3D6/7/8 HAL, WFP-safe
renamed from `3dfxvs.dll`), `3dfxv5m.sys` (miniport, renamed from
`3dfxvsm.sys`), `glide2x/glide3x.dll`, vintage `3dfxogl.dll`/`opengl.dll`.

**Key docs (read before deep work):** `retro-3dfx/CLAUDE.md` (build/test
process), `VINTAGE-FIXES.md` (definitive fix ledger), `FINDINGS.md`
(investigation matrices), `D3D-DRIVER-PLAN.md`, `V56K-PLAN.md` (Voodoo5 6000 —
Phases 0–3 done, see its status header) + `V56K-SLI-FINDINGS.md` (6000
hardware results), `tests/README.md`, `optimized/README.md` (change policy). Fleetbook:
search before diagnosing, `log --host 192.168.1.143` after every change.

## Build (Wine-hosted VC6 + W2K DDK)

Two trees: the **repo tree** (`3dfx Driver Code/H5/...`, source of truth) and
the **build tree** (`toolchain-3dfx/prefix/drive_c/3dfx/H5/...`, what Wine
compiles). **Every repo-tree edit must be cp'd into the prefix tree**, and the
edited file's stale `.obj` purged (`build -cZ` does NOT reliably rebuild):

```bash
cd /home/voidsstr/development/retro-3dfx/toolchain-3dfx
# 1. cp edited files repo tree -> prefix tree
# 2. rm -f prefix/drive_c/3dfx/H5/W2K/Src/Video/Displays/H5/objfre/i386/<file>.obj
export WINEPREFIX=$PWD/prefix PATH=$PWD/wine/bin:$PATH COPYCMD=/Y
ulimit -f 2000000        # 38GB-log hazard guard — NEVER build without it
timeout 560 wine cmd /c 'c:\3dfx\bldw2k.bat c:\3dfx\H5\W2K\Src\Video\Displays\H5'
pkill -9 -x wineserver
# output: prefix/.../Displays/H5/objfre/i386/3dfxvs.dll  (BUILDEXIT=0 = success)
# miniport: same bat with ...\Miniport\H5  -> objfre/i386/3dfxvsm.sys
```

Traps: `build -cZ` on the miniport deletes the *other* .sys (stash first);
POSTBLD rebase failure under Wine is cosmetic; grep vintage sources with
`grep -a` (CRLF/binary chars); instrumented builds set `ENABLE_LOG_FILE=1`.

## Test (REQUIRED order — the predeploy gate is absolute)

1. **Before deploy:** `bash /home/voidsstr/development/retro-3dfx/tests/predeploy.sh`
   — source invariants + native logic tests + built-artifact/stale-obj checks.
   **Non-zero exit = DO NOT deploy.**
2. **After deploy + reboot:** `python3 retro-3dfx/tests/run_target_tests.py`
   (driver/res/ring + 12-mode `d3dlab.exe` D3D matrix vs goldens in
   `tests/golden/d3dlab_golden.json`) AND the OpenGL golden gate (Q3/CS) —
   D3D and GL share the modeset/2D core, so both gates apply.
3. Hardware-verified fix ⇒ regression test in the SAME commit: source
   assertion (`tests/test_source_invariants.sh`), native `tests/native/test_*.c`,
   d3dlab mode + golden, binary marker — whichever layers apply.

## Deploy to .143

Use `mcp__retro__retro_upload` to push binaries and `retro_command` for EXEC
steps (host `192.168.1.143`). Full procedure with preflight/backup/rollback:
the **deploy-3dfx-driver** skill. Essentials:

- **WFP:** ship under the renamed, non-cataloged names `3dfxv5d.dll` /
  `3dfxv5m.sys` (never raw-copy the inbox names into system32 — WFP reverts).
  For a must-keep-name file, seed `system32\dllcache\<name>` with your copy
  first.
- Display dll swap: UPLOAD → `move /Y` the loaded `3dfxv5d.dll` aside (keep
  the `.bak`) → `copy` the new one in → reboot. **Reboot ONLY with explicit
  user approval** (confirm=true on the gated command, and physical access may
  be needed if the driver is bad).
- **Game-local ICD trap:** games load their own dir's `opengl32.dll` /
  `3dfxogl.dll` before system32 — update every copy (Q3 has BOTH names;
  `r_glDriver` decides — keep them identical). Kill GL games first; verify by
  renderer string (`[retro3dfx 0.x.y]`), never by file size.
- After reboot: `run_target_tests.py` + OpenGL golden gate + `VIDEODIAG`
  (640x480 4-bit = driver fell back = failure, roll back). Log in fleetbook.
- Benchmarks: **driver-bench** skill only (it quiesces background CPU
  thieves; a bench without the quiesce reads several fps low).

## Diagnose

- **Flight recorder (in the deployed driver):** registry ring `RLog00..RLog31`
  + `RLogSeq` under `HKLM\SYSTEM\CurrentControlSet\Services\3dfxvs\Device0`
  (REG_BINARY UTF-16LE; newest slot = (RLogSeq-1)&31; survives reboots). Read
  via `REGREAD`/a small script. Lines to look for: `H3MakeRoom STALL/
  WEDGE-BREAK`, `DdFlip WEDGE-BREAK`, `DP2-PARSE-ERR`/`DP2-EXIT-ERR` (the
  failing D3D op), `COMPUTE/PROMOTE/DEMOTE-SLIAA`, `memMgr ALLOC-FAIL`,
  `CTX-CREATE/DESTROY` balance.
- **Registry knobs** (same Device0 key): `SSTH3_SLI_AA_CONFIGURATION`
  (0=single-chip, 2=2-way SLI default on 5500 — Glide reads it too),
  `Retro3dfxLog` (verbose gate), `CmdfifoSize`, `SSTH3_SLI_BAND_HEIGHT`.
- **d3dlab.exe** (`C:\RETRO_AGENT\d3dlab.exe`, windowed) is the truthful D3D
  probe — GDI screenshots garble during exclusive fullscreen.
- Key source files when localizing a D3D bug: `D3TXTR.C` (texture/mip
  download), `D6DP2.C` (DrawPrimitives2 dispatch), `D3CONTXT.C`,
  `DDMEMMGR.C`/`HEAP5.C` (vidmem heaps), `DDFLIP.C` (flip/present),
  `DDFXNT.C` (SLIAA promote/demote), `CFIFO.C` (FIFO). Miniport: `SLIAA.C`.
  The working **Win9x tree** (`3dfx Driver Code/H5/Win9x/`) is the reference
  when W2K misbehaves — diff against it.
- Check `VINTAGE-FIXES.md` + fleetbook FIRST — mip downloads, TEXBLT FourCC,
  GoldSrc D3D crash/white world, gamma washout, unbounded spins are all
  already fixed; a recurrence usually means a stale binary got deployed.

## Fix workflow

1. Localize (ring log + d3dlab + source files above). Confirm which stack:
   `SST_*`/`SWLIBS`/0.3.x = here; `fx*.c`/0.1.x = voodoo3-driver-dev.
2. Edit in the **repo tree**, cp to prefix tree, purge `.obj`, build.
3. `predeploy.sh` green → deploy → reboot (user approval) →
   `run_target_tests.py` + GL golden gate.
4. Same commit: regression test + `VINTAGE-FIXES.md` entry. Append findings
   to `FINDINGS.md`; record the change in fleetbook.
