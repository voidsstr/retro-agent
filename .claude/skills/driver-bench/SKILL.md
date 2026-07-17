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

## Tracking schema (specpicks production DB)

DSN: env `SPECPICKS_DATABASE_URL`, else the default in `run_bench.py` (same as
`specpicks/CLAUDE.md`). Tables (created; additive only):

- `retro_benchmark_machines` — one row per box, upserted each run: ip (UNIQUE),
  hostname, os, cpu (incl. MHz), ram_mb, gpu, specs jsonb (full
  SYSINFO/VIDEODIAG/PCISCAN).
- `retro_benchmark_runs` — one row per measurement:
  - `benchmark` — `q3-timedemo-four` | `q3-screenshot-q3dm1` | `gfxbench-sweep`
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
| 192.168.1.124 | Voodoo3 AGP | XP | ALL-RETRO3DFX (since 2026-07-17) |
| 10.0.0.50 | Voodoo5 5500 AGP | Win98 | retail (Win98 = out of scope for the XP kernel driver; ICD/glide benches only) |
