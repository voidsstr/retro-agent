# Retro fleet benchmark results

One JSON per suite run: machine specs, driver stack (with **retro3dfx driver
version**), per-benchmark results. Every run is ALSO inserted into the
**specpicks production Postgres** (`retro_benchmark_machines`,
`retro_benchmark_runs`) — see the `3dfx-benchmark-optimization-project` shared
memory for the DSN pointer and conventions.

Naming: `<ip>_<YYYY-MM-DD>_<driver-version>.json` (e.g.
`192.168.1.124_2026-07-16_retro3dfx-0.1.1.json`).

Benchmarks:
- `q3-timedemo-four` — Quake III 1.32, `timedemo 1; demo four` (four.dm_66),
  16bpp, 2 runs/resolution (2nd run = official; 1st warms texture cache).
- `gfxbench-sweep` — our Glide micro-benchmark (`scripts/3dfx/gfxbench/`),
  fillrate/mode sweep, 300 frames/mode.
- `q3-screenshot-q3dm1` — quality lever: in-engine screenshot of q3dm1,
  diffed against the baseline PNG in this dir.

Reference numbers (Voodoo3, era-correct): thandor.net/benchmark/17, VOGONS wiki
"3dfx Benchmarks", VOGONS t=54517. Rule of thumb: 640x480 CPU-bound (V3 ceiling
~100-111 fps), 1024x768 fillrate-bound (~44-49 fps ceiling on V3 3000).

## Process (the harness pattern, via the retro agent)

Learned on `.124` (XP, agent ≥ 1.6.0); each of these cost a broken run to find.

1. **Launch with `EXECW` + `start`, never `LAUNCH`.** `LAUNCH` on `.124`
   returns a PID but the child never executes. Pattern:
   `EXECW 240 cmd /c cd /d "C:\Quake III Arena\Quake3" ^&^& start "" quake3.exe <args>`
   — `start` detaches the game onto the console desktop; EXECW gives a bound
   longer than EXEC's 60 s.
2. **Q3 timedemo recipe:**
   `quake3.exe +set r_glDriver retrogl +set r_mode <3|6> +set r_fullscreen 1
   +set r_colorbits 16 +set fs_homepath C:\q3home +set logfile 2 +set sv_pure 0
   +set timedemo 1 +demo four` — the fps line lands in
   `C:\q3home\baseq3\qconsole.log`, along with `GL_RENDERER` (which carries the
   `[retro3dfx 0.1.N]` stamp — that line is the ground truth for which driver
   build actually ran). Q3 1.32 plays `four.dm_66`; retail `.dm3` demos do NOT
   play on 1.32. Two runs per resolution; report the second.
3. **Kill–wait–poll.** After `taskkill /f /im quake3.exe`, poll `PROCLIST`
   until the process is really gone before starting the next run, then poll the
   log for the fps line. Racing kill/start yields a run with no fps line
   (`None` fps), which silently poisons averages.
4. **In-engine screenshots** (quality lever): `+wait N; +screenshot` on the
   command line does NOT work. Launch with `+bind F12 screenshot`, wait for the
   map to load, then agent `UIKEY f12`; the TGA lands in
   `fs_homepath\baseq3\screenshots\`, `DOWNLOAD` it. GDI `SCREENSHOT` of a
   Glide fullscreen buffer is dark/interlaced — never use it for verification.
5. **Environment discipline.** `FX_GLIDE_SWAPINTERVAL` in the process (or
   machine-wide) environment changes 1024x768 results by ~30% — record the env
   state in every result row. See the swap-interval saga in
   `../retro3dfx/CHANGELOG.md` before comparing numbers across runs.

## specpicks DB schema

DSN pointer in `~/development/specpicks/CLAUDE.md` (Azure Postgres, db
`specpicks`; use psycopg2 — no psql CLI on this host).

- **`retro_benchmark_machines`** — one row per box: `ip` (UNIQUE), `cpu`,
  `gpu`, `specs` jsonb. (`.124` = id 1: P3 845 MHz no-SSE2, 384 MB, Voodoo3
  AGP, XP 5.1.2600.)
- **`retro_benchmark_runs`** — one row per run: machine ref, benchmark name
  (`q3-timedemo-four`, `gfxbench-sweep`, `q3-screenshot-q3dm1`), `settings`
  jsonb (resolution, colorbits, env), **`driver_stack` jsonb** (names the exact
  binary at all three layers — kernel display driver, glide3x, OpenGL ICD —
  plus fork commit SHAs where applicable), **`driver_version`** (indexed; = the
  retro3dfx ICD version from `GL_RENDERER`), `lever`
  (`performance` | `quality`), `result_fps`, `result` jsonb.

### stack_composition convention

Every `driver_stack` carries a `stack_composition` tag so rows are comparable:

- **`HYBRID`** — our ICD (retro3dfx-gl) over the retail **AmigaMerlin** kernel
  driver + glide3x (`_grFoo@N` ABI, retail-link ICD build). All 0.1.1–0.1.6
  rows through 2026-07-16 are HYBRID.
- **`ALL-RETRO3DFX`** — every layer self-built: H5-source kernel driver
  (`3dfxvsm.sys`+`3dfxvs.dll`) + H5-source `glide3x.dll` + our ICD. First rows
  2026-07-17.

## Iteration ledger (Q3 timedemo four, 16bpp, `.124`)

| ver / stack | change | 640x480 | 1024x768 |
|---|---|---|---|
| 0.1.1 HYBRID | baseline | 53.7 untuned / 57.6 tuned | 38.7 untuned / 51.0 tuned |
| 0.1.2 HYBRID | SSE flags + branchless color pack | 54.2 untuned (+0.9%) | 38.7 untuned |
| 0.1.3 HYBRID | batched triangle submission | 58.1 tuned (+0.9%) | 51.2 tuned (flat) |
| 0.1.4 HYBRID | env-default injection | INERT (glide snapshots env at DLL load) | INERT |
| 0.1.5 HYBRID | Glide state shadow cache | 54.9 no-env (+0.7%) | 38.7 no-env |
| 0.1.6 HYBRID | ICD-side swap env-read fix | 54.2 no-env | 38.7 no-env (retail glide ignores it) |
| **ALL-RETRO3DFX** (2026-07-17) | full self-built stack replaces AmigaMerlin | **58.8** | **51.3** |

The ALL-RETRO3DFX milestone beats the untuned AmigaMerlin hybrid (53.7 / 38.7)
at both resolutions, and its 1024x768 result beats the era 3dfx official ICD
reference on a V3 3000 (44.3). "Tuned" = `FX_GLIDE_SWAPINTERVAL=0` in the
process env — mandatory context for any HYBRID comparison (see the saga in
`../retro3dfx/CHANGELOG.md`).

Quality baseline: `quality_192.168.1.124_q3dm1_retro3dfx-0.1.5.png` (in-engine
glReadPixels, q3dm1 640x480) — pristine, no regressions from the 0.1.2–0.1.5
performance changes.
