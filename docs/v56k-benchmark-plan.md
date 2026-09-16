# Voodoo 5 6000 (Strange God AGP) — benchmark plan, instructions and status

**Purpose:** the single place to pick this campaign back up from. What has been
measured, what is queued, exactly how each thing is run, and where results go.
Keep the status table current as cells land. Linked from `CLAUDE.md`.

Companion documents:
- `docs/specpicks-voodoo5-6000.md` — the editorial dossier: findings, retractions,
  hardware facts, "Where testing stands", learnings.
- `docs/evidence/voodoo5-6000-fsaa/` — evidence frames, charts, `edge_stats.py`.
- `retro-3dfx/FINDINGS.md` — the running findings log (newest first).
- Published series: `https://specpicks.com/reviews/voodoo5-6000-strange-god-part-{1..6}-*-2026`.
- Site data: `specpicks/scripts/lab/` (loader, publisher, deploy).

## 0. Ground rules (each one cost real time — do not relearn them)

1. **One driver setting per clean boot.** `SSTH3_SLI_AA_CONFIGURATION` is written,
   the box is rebooted (`scripts/fleet/safe-reboot.py`, never bare `REBOOT`),
   read back, then a Glide probe (`glideprobe --noopen`) confirms the board.
   Resolutions and titles cycle freely inside that boot.
2. **A setting is applied only when the rendering changes.** Readback is not
   evidence. AA settings (cfg 1/3/4/6/7/8) do NOT engage with AmigaMerlin
   3.1-R11 through the registry/env route — do not spend boots on them until
   the 3dfx Tools route (§4) makes a frame's md5 change.
3. **Never compare hosts.** Host 1 (`.191`, EP-8RDA+, dead BIOS) and host 2
   (`.124`) get separate tables. Only within-host comparisons are valid.
4. **Results go in the repo tree** (`scripts/benchmarks/results/...`), never the
   session scratchpad; the sweep refuses a `/tmp` outdir. Detach long jobs with
   `setsid nohup … &`. Evidence images go under `docs/evidence/`.
5. **Record what ran, on the row** (game md5, driver files by md5, OS, agent,
   renderer string). `v56k_bench.py` does this; `v56k_versions.py` backfills
   older CSVs and marks them retroactive.
6. **Quiesce before measuring**: AI engine, wallpaper rotator, Windows Update,
   `3dfxMan.exe`, error reporters — and any modal dialog. **A modal that
   appears after a game starts freezes it**: on 2026-09-16 two consecutive
   Quake III runs loaded the map and never rendered a frame because the XP
   "Found New Hardware Wizard" (for DAEMON Tools' driverless *SI Pseudo Device
   SCSI Processor Device*) re-launched behind the fullscreen window and took
   focus — it looked exactly like a driver wedge. It was walked to its last
   page with "Don't prompt me again" ticked and has not returned; `quiesce()`
   also kills it by window title. If a run stalls right after `cgame loaded`,
   check `WINLIST` for a dialog before blaming the card.
   The agent watchdog loop (`agentwd.cmd`) polls `tasklist` every 30 s and is a
   candidate perturbation on CPU-bound cells; pause it for repeatability runs
   and restart it afterwards.
7. **The engine takes the picture.** GDI capture of a Glide surface is noise.
   Quake III: `screenshotJPEG` on a fixed `demo four` frame. UT99: `Shot` via
   `UIKEY F11` (UE1 accepts synthetic keys fullscreen; id Tech 3 does not).
8. **UE1 leaves `System\Running.ini` when killed** → the next launch is a modal
   "Recovery Mode" dialog with a 0-byte log. `Unreal1.prepare()` deletes it.
9. **Agent liveness ≠ board liveness.** Probe the board after any failed cell.
   A hang that leaves the game on screen needs a power cycle — tell the user.
10. **`.124` has no SSE2** (fpu/mmx/cmov/3dnow/sse). SFFT 1.9 cannot run there.

## 1. Tools and how to run them

| tool | what | run |
|---|---|---|
| `scripts/benchmarks/v56k_sweep.py` | one-config-per-boot orchestrator; retries a config only when the last pass measured a cell | `python3 scripts/benchmarks/v56k_sweep.py --host 192.168.1.124 --configs 5,0,2 --titles quake3 --resolutions 640x480,800x600,1024x768,1280x960,1600x1200 --depths 16,32 --attempts 3` |
| `scripts/benchmarks/v56k_bench.py` | the campaign runner (titles, resolutions, depths, configs); applies + reads back the AA value, records versions per row | called by the sweep; `--titles quake3,quake2,glquake,ut:glide,ut:opengl,ut:d3d,unrealgold:glide,deusex:glide,serioussam,serioussam2` |
| `scripts/benchmarks/v56k_shots.py` | matched-scene image-quality captures per config (engine screenshots) | `python3 scripts/benchmarks/v56k_shots.py --host 192.168.1.124 --configs 5,7 --games quake3 --res 1024x768 --outdir scripts/benchmarks/results/shots_192.168.1.124` |
| `scripts/benchmarks/v56k_versions.py` | retroactive version capture / backfill of an older CSV | `python3 scripts/benchmarks/v56k_versions.py --host 192.168.1.124 --outdir <results dir> --backfill` |
| `scripts/benchmarks/v56k_article.py` | generates the data tables (driver labels quoted, AA cells flagged "not applied") | `python3 scripts/benchmarks/v56k_article.py <results.csv>` |
| `scripts/fleet/install-agent-watchdog.py` | Run-key agent watchdog (`--check`, `--remove`) | installed on `.124` |
| `voodoo-cleanroom/tools/glideprobe.c` → `C:\RETRO_AGENT\glideprobe.exe` | step-by-step Glide init probe; `--noopen` = board health | used by the sweep |
| `specpicks/scripts/lab/load-voodoo5-6000-lab-results.py` | loads CSVs into `hardware_specs`/`gaming_benchmarks`/`retro_benchmark_runs` | `python3 scripts/lab/load-voodoo5-6000-lab-results.py [--dry-run]` |
| `specpicks/scripts/lab/publish-voodoo5-6000-series.py` | publishes/updates the article series from a parts JSON, with gates | `python3 scripts/lab/publish-voodoo5-6000-series.py content/lab/voodoo5-6000-strange-god/parts.json --dry-run` |
| `specpicks/scripts/lab/deploy-azure.sh` | build + push + container-app update with post-conditions | `bash scripts/lab/deploy-azure.sh <tag-word>` |
| `.claude/skills/driver-bench/run_bench.py` | the fleet's older harness — has the proven **UT99 UTbench.dem** (F9 bind) and **RtCW wolfbench** timedemo routes to port | reference only |

Results live in `scripts/benchmarks/results/<name>/results.csv` (+ `versions.json`).
The durable host-2 set is `v56k_sweep_192.168.1.124/`.

## 2. Status of the matrix (host 2, `.124`, AmigaMerlin 3.1-R11)

| title (api) | 16-bit | 32-bit | notes |
|---|---|---|---|
| Quake III (OpenGL ICD) | cfg 0/2/5 × 5 res ✅; cfg 1 partial | queued | published; cfg 2 measured once (repeat queued) |
| Quake III repeatability with watchdog paused | running 2026-09-16 | — | closes the 640×480 caveat |
| Quake II (game-local `3dfxgl.dll` = a copy of the AmigaMerlin ICD, 2,646,009 B; the real MiniGL is 142,848 B in the library) | queued | queued | `demomap demo1.dm2`; fixed mode table; the runner labels the row by the DLL's real identity |
| GLQuake (MiniGL) | queued | queued | refuses >1280×960 |
| UT99 436 (GlideDrv, native Glide) | queued (UTbench.dem route) | **not possible** — UE1 GlideDrv is 16-bit only (verify on the box, record the log line) | user hit this in the video menu |
| UT99 436 (OpenGLDrv → AmigaMerlin ICD) | queued | queued | the 32-bit route for UT |
| UT99 436 (D3DDrv → AmigaMerlin D3D HAL) | queued | queued | second 32-bit route |
| Unreal Gold (Glide / OpenGL) | queued | queued | staged nGlide `glide2x.dll` was retired to `.wrapper.bak` |
| Deus Ex (Glide / OpenGL) | queued | queued | UE1 rules as UT99 |
| RtCW (OpenGL ICD) | queued | queued | needs a bench class: `wolfbench.dm_60` + `rtcwconsole.log`; no `r_mode -1` (real mode index) |
| Serious Sam TFE / TSE (OpenGL) | queued | queued | class exists in `v56k_bench.py` |
| AA settings cfg 1/3/4/6/7/8 | **blocked** — AA never engages via registry/env | — | unblock via §4 first |
| 128 MB vs 256 MB VBIOS switch | untouched under AmigaMerlin | — | physical switch; user action |
| Other drivers: official 3dfx 1.04.00 (Win2K), SFFT (non-SSE2 build only), in-house stacks | not run | — | each is a full re-run of the matrix |

Host 1 (`.191`): four verified Quake III cells at 640×480 only; board written off.

## 3. Execution plan (this session and next)

1. ✅ Watchdog-paused re-measurement of cfg 5 / cfg 0 at 640×480 → resolve the
   repeatability caveat in the dossier and Part 3/5/6 (update the articles via
   the publisher if the finding changes).
2. Readiness audit of every title on `.124` (script: `scripts/benchmarks/v56k_audit.py`
   once written): exe present, game-local `opengl32/3dfxgl/3dfxogl/glide2x/glide3x/ddraw`
   inventory (wrappers retired?), demos present (`demo1.dm2`, GLQuake `demo1`,
   `UTbench.dem`, `wolfbench.dm_60`, Serious Sam demos), config depth settings,
   UE1 ini render devices. Fix in the STAGED library where a fix is generic
   (CLAUDE.md staged-game rules), on the box where it is bench-only.
3. UT99 32-bit: measure what each render device actually does at
   `FullscreenColorBits=32` (GlideDrv, OpenGLDrv, D3DDrv) — read the log's mode
   line and the frame's bit depth; record the answer in the dossier.
4. Port the UTbench and wolfbench routes into `v56k_bench.py` (`UT99Bench`,
   `RtCW` classes) using the AmigaMerlin ICD (`3dfxogl`/system `opengl32`), not
   the in-house ICD the old harness staged.
5. Sweep: `--configs 5,2,0 --depths 16,32` across all titles, one config per
   boot, detached. Expect ~1.5 h per config. Watch `sweep.log`.
6. Load into the site DB (extend the loader per title), regenerate tables,
   add the results to the published series (Part 3 update + a new Part 7
   "the other games and 32-bit") through the publisher.
7. Then: the 3dfx Tools FSAA route (§4), chip-label experiment (SLI band
   height), the 256 MB switch (user flips it), other drivers.

## 4. Unblocking FSAA (the first experiment after the sweep)

The value alone never engages AA. `3dfxvs.dll` owns enables
(`SSTH3_ANTIALIAS`, `SSTH3_DIGITAL_SLI_AA`, `SSTH3_AA_ENABLE_OUTOFMEMORY`,
`SSTH3_AAJITTER_FORCEFLAG`, dither-matrix selectors) the lab has never written;
x86-secret (2005) got 2×/4×/8× on a real 3700A under XP + AmigaMerlin 3.1 R6
through the **3dfx Tools control panel**. Steps: let `3dfxMan.exe` live; open
Display Properties → Settings → Advanced → 3dfx tab; select 4× then 8×; dump the
whole display-class instance key before/after and diff; write whatever the
panel wrote; then re-run `v56k_shots.py` on Quake III (md5 must change) and the
UT99 edge count (must drop) and a timedemo (fps must fall). Accept an AA cell
only when all three move.

## 5. What "done" looks like for the second instalment

- every row in the matrix above has a number or an explicit "engine cannot"
- AA either engages (with the three proofs) or the route that fails is named
- the loader has the rows, the series has the tables, the dossier has the
  retractions (if any), FINDINGS.md has the pointer
