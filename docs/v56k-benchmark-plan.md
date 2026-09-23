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
   Two more CPU-bound-cell variables, both measured 2026-09-16: the agent
   watchdog loop (now a filtered `tasklist` every 90 s; the old unfiltered
   30 s loop cost 10–17% at 640×480), and **time since boot** — cfg 2 at
   640×480 read 105.9 as the first cell after its boot and 117.5 two minutes
   later in the same boot (agent startup threads, XP post-logon work). The
   sweep now settles 150 s after the board probe (`--settle`) and
   `v56k_full_sweep.sh` orders resolutions high→low so the CPU-bound cell
   runs last. Pause the watchdog for repeatability runs; restart it after.
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
| `scripts/benchmarks/v56k_diag.py` | **the AmigaMerlin flight recorder and crash capture** — `dump`/`ring` (per-chip scanout over the driver's own `HWCEXT_GET_SLAVE_REGS` escape), `watson` (decode the crash), `quiet` (suppress the crash dialog that makes a crash look like a wedge), `capture` (bundle) | `python3 scripts/benchmarks/v56k_diag.py --host 192.168.1.124 watson` |
| `scripts/benchmarks/v56k_audit.py` | read-only readiness audit per title (exe, game-local DLLs, demo, config depth) | `python3 scripts/benchmarks/v56k_audit.py --host 192.168.1.124` |
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
| Quake III (OpenGL ICD) | cfg 0/2/5 x 5 res OK; cfg 1 partial | **cfg 5 done: 51.3 / 75.1 / 101.1 / 110.3 / 115.0** (1600x1200 -> 640x480) | published. ONE 10:01 cfg-0 stall was `glide3x`'s no-context int3 (likely the wizard's focus loss); 16 of ~20 launches that day were clean. The agent deaths are a separate, unexplained event (FINDINGS) |
| Quake III repeatability, box quiet | OK 640x480: cfg 0 117.5/116.3, cfg 2 117.5 (105.9 as first cell after boot), cfg 5 119.4/121.6 | — | resolved: background load + first-cell-after-boot cost 10-17% on the CPU-bound cell |
| Quake II (game-local `3dfxgl.dll` = a copy of the AmigaMerlin ICD) | **cfg 5 done: 147-173 fps, flat across resolution** | **cfg 5 done: 32-bit = 16-bit** (172.6 vs 172.5 @1600x1200) | entirely CPU-bound on this card; the runner labels the row by the DLL's real identity |
| GLQuake (MiniGL) | **out of the automated sweep** | same | Runs fine on AmigaMerlin (GL_RENDERER Mesa Glide v0.63) - **our `-condebug` flag crashes it**: the ICD's 1,434-byte extension string overruns `Con_DebugLog`'s static 1 KB buffer into the console-text pointer. Without `-condebug` there is no log; `MESA_EXTENSION_OVERRIDE` does not exist in this Mesa vintage. Capture route ready (`v56k_glq_shot.py` photographs the console line), untested |
| UT99 436 (GlideDrv, native Glide) | **cfg 5 done: 56.0 / 62.7 / 65.0 / 65.8 / 67.1** (1600x1200 -> 640x480); 66.4 @640x480 cfg 2 | **renders 16-bit regardless** — the ten "32-bit" rows matched their 16-bit twins to within noise; now `unsupported-by-engine` | parser takes the demo's summary, not the toggle blip |
| UT99 436 (OpenGLDrv -> AmigaMerlin ICD) | **BROKEN** | **BROKEN** | GPF at init in UT's stock v436 `OpenGLDrv.dll` (`UOpenGlRenderDevice::SetRes <- ::Init <- TryRenderDevice <- UGameEngine::Init`, read off the screen). Known-fragile renderer; blaming the driver is an inference |
| UT99 436 (D3DDrv -> AmigaMerlin D3D HAL) | **cfg 5: 1024x768 69.1, 800x600 69.5**; dies at 1280x960+ | **cfg 5: 800x600 68.1 — UT99's real 32-bit route**; dies at 1024x768/32 and above (`process-exited`, records kept) | the answer to "UT99 wouldn't go 32-bit": use D3DDrv at 800x600 |
| RtCW (`rtcw:openglv5`) | **116-127 fps @640x480 cfg 2** | sweeping | loads its bundled Wicked3D `gl/openglv5.dll` on the first launch after an `r_glDriver` change regardless of `+set`/config (mechanism not fully established). Removing the file to force the ICD WEDGED the box — the runner reads back which ICD loaded instead |
| Serious Sam TFE / TSE (OpenGL) | not yet measured | not yet measured | the first harness bypassed the staged **disc-mount launcher** and got the CD check it exists to prevent — a harness fault. The bench launcher is now generated from the fleet mount template; untested on the box |
| Unreal Gold / Deus Ex (Glide) | not run | not run | UE1 `-benchmark` never exits; needs the UTbench-style route |
| AA settings cfg 1/3/4/6/7/8 | **blocked** — AA never engages via registry/env | — | unblock via §4 first |
| 128 MB vs 256 MB VBIOS switch | untouched under AmigaMerlin | — | physical switch; user action |
| Other drivers: official 3dfx 1.04.00 (Win2K), SFFT, in-house stacks | not run | — | each is a full re-run of the matrix |

### Resume point (2026-09-23)

- **Listener-aware watchdog deployed on `.124` and proven**: the agent killed on
  purpose came back by itself in 83 s.
- **cfg 2 resumed**: Quake III 1280x960 = **115.9 (16-bit) / 78.5 (32-bit)**.
  The next cell (1024x768/16) stalled after `cgame loaded` with no modal
  showing, and the box went to 9898 refused / 9897 accepting-but-mute / SMB up.
  **The watchdog did not recover that in 12+ min**: it handles a dead agent,
  not a display-driver wedge. That costs a power cycle (FINDINGS 2026-09-23).
- **Next, after the power cycle:** run the other titles FIRST and Quake III
  LAST, so a Quake III stall cannot cost the rest of the config:

      setsid nohup python3 scripts/benchmarks/v56k_sweep.py --host 192.168.1.124 \
        --configs 2,0 --titles quake2,ut99:glide,ut99:d3d,rtcw:openglv5,quake3 \
        --resolutions 1600x1200,1280x960,1024x768,800x600,640x480 --depths 16,32 \
        --attempts 3 --max-run 420 --outdir scripts/benchmarks/results/v56k_titles_192.168.1.124 \
        > scripts/benchmarks/results/v56k_titles_192.168.1.124/sweep_cfg2_cfg0.log 2>&1 < /dev/null &

### Row statuses a CSV can carry (2026-09-16)

`ok` is the only status the specpicks loader publishes. The others each name a
different next action, which is the point of not collapsing them:

| status | meaning | next action |
|---|---|---|
| `unsupported-by-engine` | the engine declares it cannot do this mode (GLQuake >1280x960, RtCW 1280x960) | none - reportable as an engine limit |
| `blocked-by-modal` | a dialog that never clears (UE1 "Critical Error", Serious Sam "CD check") | fix the title/library; the dialog is named in `notes` |
| `process-exited` | the game process died before any fps line | read the Dr Watson bundle in `diag/` - it names the fault |
| `mount-failed` | the disc-mount launcher wrote `mount-error.txt` (no mounter, or no drive appeared) | fix the box's mounter; the text is in `notes` |
| `driver-mismatch` | the ICD that loaded is not the one the cell asked for (RtCW) | the number is real but for the OTHER driver; do not publish under this one |
| `gl-init-hung` / `mode-rejected-by-card` | renderer never came up / the card refused the mode | the flight recorder (`v56k_diag ring`) is the instrument |
| `no-fps-line(see raw log)` | none of the above matched | the runner could not classify it - look at the log |

Evidence is never overwritten: every Dr Watson fetch lands under its cell label
(`drwtsn32-<label>.log`), and the unlabelled name is only the "latest" copy.

### Diagnosing an AmigaMerlin failure (2026-09-16)

**AmigaMerlin is a RETAIL driver and has no `RLog*` registry ring** — the
flight recorder we rely on in the vintage H5 build does not exist here
(measured: absent from the display class, `3dfxvs`, and `Device0`). The
recorder has to come from outside the driver, and there are exactly two
surfaces, both wrapped by `v56k_diag.py`:

- **`fxscan2 ring`** (built from `retro-3dfx/tools/v56k`, single-sourced there)
  — per-chip scanout registers over the `HWCEXT_GET_SLAVE_REGS` escape the
  SHIPPING driver answers. Verified against AmigaMerlin on `.124`: escape
  `0x3df3`, `121A:0009`, 4 chips. **The only recorder that can see a Glide
  fullscreen session**, since the driver releases the card on
  `DrvAssertMode(DISABLE)`. Start it BEFORE the game.
- **Dr Watson** — which is what identified the Quake III crash.

Two failure classes need different handling, and confusing them wasted a day:

| class | mechanism | handling |
|---|---|---|
| a **crash** (Windows) | int3/GPF raises a dialog that sits BEHIND the exclusive fullscreen surface, so the box reads as wedged | `v56k_diag quiet` suppresses the dialog, keeps the dump |
| a **modal the ENGINE owns** (UE1 "Critical Error", Serious Sam "CD check") | sits forever; `quiet` cannot touch it | `blocking_modal()` detects it; the cell fails in seconds as `blocked-by-modal` |

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
