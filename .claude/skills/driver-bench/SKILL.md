---
name: driver-bench
description: Run the driver benchmark/optimization loop on a fleet retro machine (3dfx Voodoo only for now) with every result fully tracked in the specpicks production DB — baseline runs, A/B after each driver change, quality screenshots, all keyed by driver version and exact stack composition. Use when the user says to benchmark a driver build, A/B a driver optimization, run the driver test loop on a machine, or track driver performance/quality results.
---

# Driver Bench — benchmark, optimize, and track (3dfx)

Wraps the retro3dfx benchmark→optimize→track workflow so it runs against **any
fleet machine with a 3dfx card** and every run lands in the **specpicks
production Postgres** with full metadata. Distilled from the 0.1.1→0.1.6
optimization campaign on `.124` (see `retro3dfx/CHANGELOG.md` for the history
and `benchmarks/README.md` for the conventions).

**Scope guard:** 3dfx Voodoo 3/4/5 only for now. Preflight aborts on other GPUs
(the harness assumptions — Glide stack, `retrogl.dll` ICD, 16bpp modes — are
3dfx-specific). Other vendors get their own lane later.

## The one-command runner

```bash
python3 .claude/skills/driver-bench/run_bench.py --ip <target> [options]
```

Does, in order: preflight (agent ≥1.6.0, 3dfx card, CPU MHz) → **stack
detection** (classifies ALL-RETRO3DFX / HYBRID / RETAIL from system32 file
fingerprints + GL_RENDERER) → Q3 timedemo matrix (default 640x480 + 1024x768,
2 runs each) → optional in-engine quality screenshot → **machine upsert + one
DB row per run** in specpicks → JSON drop in `benchmarks/`.

Key options (see `--help` for all): `--modes 3,6` (r_mode list: 3=640x480,
4=800x600, 6=1024x768), `--runs 2`, `--env "FX_GLIDE_SWAPINTERVAL=0"` (launcher
env; empty = none), `--changes "<fork-sha>: what changed"` (REQUIRED when
benchmarking a new driver build — this is the metadata that makes A/Bs
attributable), `--lever performance|quality`, `--screenshot` (adds the
in-engine capture + quality DB row), `--notes`.

### Games: Quake III + Counter-Strike 1.6

`--game q3` (default) | `cs16` | `both`. Both run through the Voodoo's OpenGL
path (our MesaFX `retrogl.dll`), so a single ICD build is measured on two
engines — Q3 (idTech3) and CS 1.6 (GoldSrc, an idTech2/QW descendant). CS 1.6
uses GoldSrc's `timedemo <demo>` console command; `-condebug` mirrors the fps
line to `cstrike\qconsole.log` (same `... frames ... seconds ... fps` shape as
Q3), and `GL_RENDERER` still carries the `[retro3dfx 0.1.N]` stamp so the run
self-documents. DB benchmark name: **`cs16-timedemo`**.

**CS 1.6 prereqs (stage once per box):** a CS install at `--cs16dir`
(default `C:\Counter-Strike 1.6`), a benchmark demo at `cstrike\<--cs16demo>.dem`
(default `bench.dem`), and the OpenGL renderer selected so it uses our ICD
(`hl.exe -gl`, with `retrogl.dll`/`opengl32.dll` resolvable from the CS dir). If
the demo file is absent the run records `None` fps (flagged, not silently
averaged). Record any CS-specific demo/map in `--notes`.

## The optimization loop (one change per version)

1. **Change exactly one thing** in the driver source (fork repos:
   `voidsstr/retro3dfx-gl`, `voidsstr/retro3dfx-glide`; kernel driver in the
   sibling `~/development/retro-3dfx` repo). Commit to the fork with the
   rationale; note the SHA.
2. **Build with a version bump** — `retro3dfx/build-mesafx-retail.sh` for the
   ICD (auto-increments `.buildnum` → `0.1.N`, stamps GL_RENDERER so every log
   self-documents). Verify the stamp + import ABI in the build output.
3. **Deploy** — ICD: UPLOAD to the game dir (`retrogl.dll`) AND
   `%SystemRoot%\system32\retrogl.dll` (kill the game first; you cannot
   overwrite an open DLL). Kernel driver/glide3x: use the **deploy-3dfx-driver**
   skill (SetupAPI; never raw-copy into system32 — WFP reverts it).
4. **A/B** — `run_bench.py --ip <target> --changes "<sha>: <what>"`. Compare
   against the previous version's rows (same machine, same settings) in the DB.
5. **Record** — the runner inserts the rows; also append the version to
   `retro3dfx/CHANGELOG.md` (change, why, measured result — including
   failures/inert changes, they're the most instructive) and update the
   `3dfx-benchmark-optimization-project` memory if the finding changes strategy.
6. **Quality gate** — after any change that touches the render path, rerun with
   `--screenshot` and pixel-diff vs the machine's baseline artifact
   (`benchmarks/quality_*` — mean diff ≲5/255 is animation noise; structural
   differences mean a rendering regression: STOP and investigate).
7. **Commit + tag the optimization (REQUIRED, one per version).** Every
   optimization gets **its own commit** and a **`bench-<ver>` tag** so it can be
   re-benchmarked later:
   - In this repo, commit the CHANGELOG entry + the `benchmarks/*.json` drop for
     the version, message `retro3dfx <ver>: <one-line change> (<fps A/B>)`.
   - `git tag -a bench-<ver> -m "<change> — <result>" <commit>` and
     `git push origin bench-<ver>`. (Existing 0.1.1–0.1.6 + `bench-all-retro3dfx`
     are already tagged; continue the sequence: `bench-0.1.7`, …)
   - The fork commit (retro3dfx-gl) holds the actual code change; reference its
     SHA in `--changes` and the CHANGELOG so the tag here points to the tracked
     result and the fork SHA points to the code.

## Pure-3dfx lane — `3dfx-driver-optimized` (.143 Voodoo5)

A second driver stack, separate from the MesaFX lane above: **our own H5-source
OpenGL ICD `3dfxogl.dll`** (not MesaFX) on top of our `glide3x.dll` + the renamed
WFP-safe display driver `3dfxv5d.dll` + miniport `3dfxv5m.sys`. Built in the
private **retro-3dfx** repo; per-commit history in `retro-3dfx/optimized/CHANGELOG.md`.
Tracked in specpicks as `driver_stack.name = 3dfx-driver-optimized`, machine .143
(id 3). First rendering build **0.1.0 = 74.2 fps** Q3 four @640x480x16.

Run it via the same runner, with the lane flags:
```bash
python3 run_bench.py --ip 192.168.1.143 --gldriver 3dfxogl \
  --stack-name 3dfx-driver-optimized --driver-version 3dfxopt-0.1.N \
  --modes 3 --runs 2 --changes "<commit>: <what changed>"
```
- `--gldriver 3dfxogl` — Q3 `r_glDriver` loads our ICD (not `retrogl`).
- `--driver-version` — explicit; our ICD's `GL_RENDERER` is just `3Dfx`, no
  `[retro3dfx X.Y]` stamp to scrape. Bump `3dfxopt-0.1.N` per optimization.
- `--stack-name` also **captures driver crash logs on a no-fps run**
  (`C:\glide3x.log`, `C:\3dfxogl.log`, Dr Watson) into the run's `crash_logs` —
  essential for the debug loop.

### Deploying a new build in this lane
- **glide3x.dll / 3dfxogl.dll are user-mode** — Q3 loads them, nothing else
  holds them. Swap with **NO reboot**: kill quake3, `copy /Y` the new DLL to
  BOTH `system32\` and the Q3 dir. This is why iteration is fast (~90 s/run).
- **Display driver `3dfxv5d.dll` / miniport `3dfxv5m.sys` need a reboot** and go
  through the WFP-safe rename (see `deploy-3dfx-driver`); avoid churning these.

### Lane gotchas (hard-won on .143 — do not relearn)
- **Corrupt RTC crashes Q3 pre-GL.** .143's clock read year 8326; Q3 NULL-derefs
  in early init (before any GL) on a bad clock. Fix once:
  `date MM-DD-YYYY` + `time HH:MM:SS`. (Symptom: quake3 exits instantly, empty
  qconsole.log, Dr Watson fault in `quake3.exe` not a driver DLL.)
- **q3config.cfg resets `logfile 0`** — force logging via an `autoexec.cfg`
  (execs last) in `C:\q3home\baseq3`, or `+set logfile 2` won't produce a log.
- **Disable WER** so a driver crash exits cleanly instead of hanging a modal
  dialog: `PCHealth\ErrorReporting DoReport/ShowUI=0` + `Control\Windows ErrorMode=2`.
  Install Dr Watson as postmortem (`drwtsn32 -i`, `AeDebug Auto=1`) — it names
  the faulting module+address, the fastest way to localize a driver crash.
- **`+set s_initsound 0 +set com_introPlayed 1`** (the runner sets these) —
  avoids sound/intro-cinematic paths during the timedemo.
- **A failed `grSstWinOpen` garbles the screen** (bad fullscreen mode-set, no
  restore) and may need a power-cycle. Minimize speculative crash-runs: use the
  driver logs (`C:\glide3x.log` phase trace) to localize, FIX, then test — don't
  spam Q3 launches into a known crash.
- The pure-3dfx `glide3x` has verbose startup logging (grSstWinOpen phases); it's
  one-time at mode-set, **no per-frame cost**, so it doesn't skew fps.

## Tracking schema (specpicks production DB)

DSN: env `SPECPICKS_DATABASE_URL`, else the default in `run_bench.py` (same as
`specpicks/CLAUDE.md`). Tables (created; additive only):

- `retro_benchmark_machines` — one row per box, upserted each run: ip (UNIQUE),
  hostname, os, cpu (incl. MHz), ram_mb, gpu, specs jsonb (full
  SYSINFO/VIDEODIAG/PCISCAN).
- `retro_benchmark_runs` — one row per measurement:
  - `benchmark` — `q3-timedemo-four` | `cs16-timedemo` | `q3-screenshot-q3dm1` | `gfxbench-sweep`
  - `settings` jsonb — resolution, r_mode, colorbits, demo, run_index,
    q3_version, env
  - `driver_stack` jsonb — **mandatory**: display_driver, glide3x, icd,
    icd_version, gl_renderer, `stack_composition`
    (`ALL-RETRO3DFX: ...` | `HYBRID: ...` | `RETAIL: ...`), `changes`
    (fork SHA + description — the A/B attribution key)
  - `driver_version` (indexed), `result_fps`, `result` jsonb, `lever`
    (`performance` | `quality`), `notes`, `source`

Rule: **no run without a DB row, no DB row without stack_composition and (for
new builds) changes.** A benchmark that isn't attributable to an exact driver
revision is noise.

## Harness rules (hard-won — do not relearn these)

- **EXECW + `start`, never LAUNCH** for the game (`LAUNCH` returns a PID but
  doesn't execute on some boxes). `EXECW <secs> cmd /c cd /d "<dir>" ^&^& start
  "" quake3.exe ...`.
- **Kill-wait-poll:** after `taskkill`, poll `tasklist` until the process is
  really gone before starting the next run; poll `qconsole.log` for the fps
  line (a kill/start race silently yields no result).
- **Ground truth is `qconsole.log`** (`+set logfile 2`, under
  `fs_homepath\baseq3\`): the `... frames, N seconds: F fps` line and the
  `GL_RENDERER` line (must carry the `[retro3dfx 0.1.N]` stamp — if it doesn't,
  you're benchmarking the wrong DLL).
- **Run 2x per mode; the second run is official** (first warms texture cache).
- **Env discipline:** launcher env at process creation is the only reliable
  Glide config channel (glide3x snapshots env at DLL load; runtime injection,
  registry Device0 writes, and the grBufferSwap argument are all inert on
  retail glide — see CHANGELOG "swap-interval saga"). Our glide3x has sane
  defaults in code; run all-ours benchmarks with `--env ""`.
- **Screenshots:** GDI `SCREENSHOT` of a Glide fullscreen surface is
  dark/interlaced garbage — for quality use the in-engine path
  (`+bind F12 screenshot` + agent `UIKEY f12`, TGA in
  `fs_homepath\baseq3\screenshots\`), which is glReadPixels through the ICD.
- **One agent connection at a time**; close between phases; Win98 boxes need
  graceful closes (RST crashes Winsock — but 3dfx XP boxes are the target here).

## Reference anchors (for "is this number good?")

Voodoo3 3000 + Q3 (16bpp, normal): 640x480 is **CPU-bound** (card ceiling
~100-111 fps; P3-933 + era 3dfx ICD ≈ 90.9), 1024x768 is **fillrate-bound**
(~44.3 era ICD). Sources: thandor.net/benchmark/17, VOGONS wiki "3dfx
Benchmarks", VOGONS t=54517. Our ALL-RETRO3DFX standing (P3-845): 58.8 / 51.3.

## Current fleet targets

| IP | Card | OS | Stack (last known) |
|---|---|---|---|
| 192.168.1.124 | Voodoo3 AGP | XP | ALL-RETRO3DFX / MesaFX (since 2026-07-17) |
| 192.168.1.143 | Voodoo5 5500 AGP | XP | 3dfx-driver-optimized (pure-3dfx ICD; 0.1.0 renders, 74.2 fps @640) |
| 10.0.0.50 | Voodoo5 5500 AGP | Win98 | retail (Win98 = out of scope for the XP kernel driver; ICD/glide benches only) |
