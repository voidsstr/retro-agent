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

### Resume point (2026-09-25 01:00) — all-ours stack runs; 4-chip open on our Glide is next

**State of `.124`:** cfg 0 (1 chip) boot, CPU 2004 MHz, system ICD still
`retroicd.dll` 0.1.66 (rollback in the 2026-09-24 night point), guardian unit
`box-guardian-124` active. Game-local lanes stage their own DLLs per run.

**Done since the night point** (details: `voodoo-cleanroom/README.md` §13.3,
`CHANGELOG.md` 0.1.67/0.1.68):
- All-ours (our ICD over OUR h5 Glide) cfg 0 matrix, Quake II + Quake III, 5 res ×
  16/32: level with our-ICD-over-AmigaMerlin within 3 % except Q2 1280×960 and Q3
  640×480×32 (−9 % each). Raw: `results/…/allours/`.
- `glide3x_h5_x86.dll` (3dfx asm triangle setup + 3DNow!/SSE, target-derived
  offsets, fork `c41b50d`): +1.7 % Quake II / +0.5 % Quake III at 320×240 (CPU-bound
  one chip), interleaved A/B ×2 (`results/…/allours-ab/`). Built by `build-stack.sh`.
- ICD 0.1.67 profiler: `--env "RETROGL_PROF=C:\RETRO_AGENT\cr\prof_<t>.txt"`, then
  `icdprof.py --fetch 192.168.1.124 <path> --dll retrogl.dll=<icd> --dll glide3x.dll=<glide>`.
  Quake III 320×240: game 47 %, ICD 20 %, Glide 17 %; no single hot spot.
- The "hang in grGlideInit" was a hidden Glide fatal-error dialog (stale mapping
  from force-killed Quake II PIDs). Runner: Quake II now QUITS by itself
  (`nextserver` after `demomap`); ICD logs Glide errors (0.1.67); Glide unmaps
  under the right PID (fork `5439bb8`). Every all-ours launch writes
  `C:\RETRO_AGENT\cr\maplog.txt`, and a failed cell saves its tail to `diag/`.
- A wedged cell's thread stacks are taken with `ntsd -pv` before the kill
  (`diag/*-hangstacks-sym.txt`).
- Another process on the dev host ran `GAMESYNC RESET+START` on `.124` at 00:43
  (not `retro-agent-f3`/`-90`, both asked to keep off `.124`); the one cell it
  overlapped (a profiled run) was discarded. **Check `GAMESYNC STATUS` before
  trusting a number taken tonight.**

**Update 01:45 — four chips WORK on our Glide.** `.124` is now booted at **cfg 5**.
`glideprobe` open at cfg 5: `SLI_AA_REQUEST(open) retVal=1 resStatus=1 chips=4
sliEn=1 nlines=8 analog=1`, 3 frames, clean close. All-ours cfg 5 matrix complete,
Q2 + Q3 × 5 res × 16/32, both Glide builds, 0 failed cells
(`results/…/allours-cfg5/{c,x86}`): level with our-ICD-over-AmigaMerlin; the
asm build is within ±3 % of the C build at four chips (noise). A real Quake II
frame reads back through the LFB under SLI (`frame_compare_0169/`).

**Update 03:50** — ICD now 0.1.74. Quake II single-pass (`FX_SGIS_MULTITEXTURE=1`)
50.8 → 197.5 fps at 640×480 and 49.3 → 176.3 at 1024×768 (4 chips) after four
profiler-found fixes (CHANGELOG 0.1.71-0.1.74); 0.1.69 batching reverted (no
gain). **RtCW:** set `r_glIgnoreWicked3D 1` or it runs Wicked3D whatever
`r_glDriver` says; with it, it reaches the SYSTEM ICD — so the RtCW clean-room
lanes stage `C:\WINDOWS\system32\retroicd.dll` (registration must read
`retroicd.dll`; rollback `reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\3dfx" /v DLL /t REG_SZ /d 3dfxOGL.dll /f`).
All-ours RtCW cfg 5 done (`allours-rtcw/cfg5b`). `.124` agent is 1.85.0 (auto-updated 02:16).

**Update 05:05** — ICD **0.1.75**: Quake II single-pass is the DEFAULT now
(+63..71 % on one chip at every resolution; `FX_SGIS_MULTITEXTURE=0` = two-pass;
rows say which path ran in `notes`). cfg 0 all-ours done on 0.1.74
(`allours-0174-cfg0/`: Q2 both paths, Q3, RtCW). RtCW is GPU-bound on four
chips (~19 % of CPU waiting in grBufferSwap); our LOD bias −0.5 costs up to 9 %
there (left as default - decision for the user). **Wicked3D renders 16-bit at
every requested depth** → its RtCW "32-bit" rows are retracted (dossier #5);
specpicks loader fixed (`f2610e0`) but **not run against the live DB**.
System ICD on `.124` is now `retroicd.dll` **0.1.75**. CS 1.6 unchanged by the
SGIS default.

**Update 06:00 — all-ours matrix COMPLETE** for Quake II (both paths), Quake III
and RtCW at cfg 5 / 2 / 0 on 0.1.75 (cfg 0 on 0.1.74): the table is in
`voodoo-cleanroom/README.md` §13.3. `.124` is booted at **cfg 5**, system ICD
`retroicd.dll` 0.1.75, agent 1.85.0. cfg 2 ≡ cfg 5 inside Glide (same-boot
probe: 77.6 = 77.6); the difference is the boot (dossier 0b update).

**Decisions waiting for the user:** (1) run the fixed specpicks loader
(`f2610e0`) against the live DB to retire the nine RtCW Wicked3D "32-bit" rows;
(2) keep or drop our default LOD bias −0.5 (costs up to 9 % in GPU-bound RtCW,
sharper textures); (3) whether clean-room rows go into the article at all.
**Next technical:** RtCW trails Wicked3D ~15 % at 16-bit (GPU-bound); the
AmigaMerlin-miniport boot state behind cfg 2 vs cfg 5.

### Resume point (2026-09-24 08:39) — `.124` WEDGED during the clean-room smoke test; needs a power cycle

`cleanroom_smoke.py` launched Hexen II (after SoF2, which ran fine on our ICD) and the
box went into the deep form of the wedge: 9898 refused, 9897 accepting, then **445
down too**, 135/139 accepting TCP but the endpoint mapper timing out on bind. No
remote path answers (RPC reboot over 445, over 139 and over `ncacn_ip_tcp` all
fail), so `box-guardian.py` correctly did nothing. PXE hold re-armed at 08:5x.

**After the power cycle:**
1. Read `C:\retrogl.log` (DOWNLOAD) — its tail names the last process that touched
   our ICD, and whether Hexen II got that far.
2. `.124` still has **our ICD 0.1.64 registered as the system ICD**
   (`system32\retroicd.dll`, 0.1.63 kept as `retroicd_0.1.63.dll`). Rollback to
   AmigaMerlin: `reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\3dfx" /v DLL /t REG_SZ /d 3dfxOGL.dll /f`.
3. Our h5 Glide (fork `839143c`, H1–H7) is staged at `C:\RETRO_AGENT\cr\glide3x.dll`
   and passed `glideprobe --noopen`; roadmap step 4 (board open) has NOT been run.

Smoke-test results so far (`results/v56k_cleanroom_192.168.1.124/smoke*.json`), our
ICD as system ICD: GLQuake, WON Half-Life, Quake II (staged bat), SoF, SoF2 MP, CS 1.6
and UT99 OpenGL create a context and keep running; ioquake3 loads our ICD but
creates no context (SDL path, not diagnosed); Jedi Academy and Serious Sam FE
started no process from the harness (launcher not diagnosed); Hexen II → wedge.

### Resume point (2026-09-24 morning) — clean-room lane is running on the V5 6000

**The AmigaMerlin matrix is complete** (cfg 5 / 2 / 0, every title): Quake II was
re-measured after the discovery that every earlier Quake II row had run at
640×480×16 (staged `autoexec.cfg` reset `gl_mode`; now a per-run `fleetres.cfg`
plus a `verify_mode` gate that refuses a row at the wrong mode). RtCW's last cell
(cfg 0 1024×768×32) is 17.9 fps, no wedge. SpecPicks is reloaded: the 20
mislabelled Quake II rows retired, the 30 mode-verified ones and CS 1.6 best-of-3
loaded (specpicks `a9b3ab0`).

**Our voodoo-cleanroom ICD on this card** — rows in
`results/v56k_cleanroom_192.168.1.124/` (never mixed with AmigaMerlin rows):

| lane | how it loads | result |
|---|---|---|
| Quake II / Quake III, `quake2:retrogl` / `quake3:retrogl` | game-local `retrogl.dll` by name (0.1.61/0.1.62) | cfg 0/2/5 complete. Quake II faster than AmigaMerlin in every cell (up to +26 % at 640×480 4-chip); Quake III level on 1 chip, ahead on 4 at ≤1280×960 |
| CS 1.6, UT99 OpenGLDrv | **system ICD** — `system32\retroicd.dll`, `OpenGLDrivers\3dfx\DLL=retroicd.dll` (0.1.63 added the `Drv*` front end) | cfg 0 done: CS best-of-3 26.8 / 41.0 / 63.3 / 93.1 / 126.3 (parity; AmigaMerlin hangs at 1600×1200); UT99 OpenGL 45.2 @1280, 58.3 @800/640 — **runs, where AmigaMerlin's ICD GPFs at init**; 1600×1200 and 1024×768 raised UT's "Critical Error" (not yet diagnosed). cfg 5/2 sweeping (`sysicd/`) |
| RtCW | — | cannot be pointed at another ICD: `WolfMP.exe` loads `system32\gl\openglv5.dll` on a Voodoo regardless of `r_glDriver` |

**`.124` is currently running OUR ICD as the system ICD.** Rollback:
`reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\3dfx" /v DLL /t REG_SZ /d 3dfxOGL.dll /f`
(the AmigaMerlin value). Do this before any further AmigaMerlin measurement.

**Next:** diagnose UT99's Critical Error on our ICD (screenshot the dialog + keep
`UnrealTournament.log`), hardware-verify ICD 0.1.64 (refresh fix, I1), then
restore the AmigaMerlin ICD registration.

### Resume point (2026-09-24 night) — AmigaMerlin matrix done; CS 1.6 in; clean-room lane next

**cfg 5 / 2 / 0 are measured on every title** (best `ok` row per cell, 16 / 32-bit,
1600×1200 → 640×480):

| title | cfg 5 (4-chip) | cfg 2 (2-chip) | cfg 0 (1-chip) |
|---|---|---|---|
| Quake II (ICD game-local) | 171 / 173 … 169 / 172 — flat, CPU-bound | 171 / 172 … 166 / 171 | **81.5 at every cell — suspected vsync cap, verify** |
| Quake III | 79.0/51.3 · 116.3/75.1 · 120.4/101.1 · 119.9/110.3 · 122.3/115.0 | 79.0/51.2 · 115.9/78.5 · 117.3/97.9 · 121.9/110.5 · 122.9/116.5 | gl-init-hung/7.3 · 38.8/16.0 · 54.9/32.8 · 88.1/52.9 · 117.9/78.8 |
| RtCW (Wicked3D `openglv5`) | crash · no mode · 117.9/91.9 · 113.3/111.5 · 128.4/110.3 | crash · no mode · 102.6/94.5 · 125.8/115.1 · 127.1/106.3 | crash · no mode · 37.9/**pending** · 57.8/47.1 · 81.4/65.8 |
| UT99 GlideDrv (16-bit only) | 56.0 · 62.7 · 65.0 · 65.8 · 67.1 | 57.7 · 62.8 · 64.7 · 64.3 · 66.5 | exited · 31.9 · 44.7 · 56.2 · 65.4 |
| UT99 D3DDrv | exits ≥1280 · 69.1/exit · 69.5/68.1 · 70.3/69.1 | same shape | same shape |
| CS 1.6 | see below | | |

- **RtCW 1600×1200 is a deterministic crash in Wicked3D's `openglv5!glReadPixels`**
  (c0000005, every config) — a limit of that wrapper, not the card. 1280×960 is not
  in its mode table. The one open cell, cfg 0 1024×768×32, timed out once; it has the
  shape of the 640×480×32 wedge, so it is retried last, with the box guardian armed.
- **CS 1.6 needed a fix before it could run at all**: the AmigaMerlin ICD is
  Mesa-based and its SSE-exception probe (a deliberate `divps` by zero) is caught by
  GoldSrc's own handler, which shuts the engine down → `MESA_FORCE_SSE=1`
  (retro-agent `c71127f`, FINDINGS 2026-09-23 night).
- **CS 1.6 numbers are RAM-bound on this box, so they are best-of-3.** `.124` has
  **255 MB** of RAM (156 MB available at idle) and `hl.exe` commits ~169 MB, so a run
  pages or not depending on what the OS has trimmed: the same cell read 47–58 fps
  on one boot and 119 on another, the game logs identical but for the fps line, the
  box 97–100 % idle meanwhile (sampled). Single runs are therefore not comparable;
  `results/…/cs16_best3/` runs every cell three times and the article takes the
  best (the unpaged run) with the spread. **Say "256 MB RAM" next to every CS number.**
- **CS 1.6 1600×1200 on ONE chip hangs in the driver** and ignores `taskkill` for
  30 s+ — the runner now records that as `hung-unkillable` (it used to be an
  anonymous `error: TimeoutError`).

**Recovery without a person (new, 2026-09-24):** the V5 display wedge leaves SMB up,
so `.124` now has ForceGuest=0 and `scripts/fleet/safe-reboot.py <ip> --rpc` reboots
it over Windows RPC after arming the PXE hold (proven: down 10 s after the call, agent
back in ~2 min). `scripts/fleet/box-guardian.py 192.168.1.124` runs that automatically
after 6 min of agent silence with 445 up.

**Next: the clean-room lane (roadmap 17.1 Step 1)** — `quake2:retrogl` /
`quake3:retrogl` load our voodoo-cleanroom 0.1.61 ICD game-local over AmigaMerlin's
Glide; rows carry `api = opengl-cleanroom-<ver>` and go to their own outdir
(`results/v56k_cleanroom_192.168.1.124/`), never mixed with the AmigaMerlin rows.

### Resume point (2026-09-23 evening) — cfg 2 nearly complete

Sweep `sweep_cfg2_cfg0_20260923b.log` (titles ordered Quake III **last**) measured
cfg 2 on every title but Quake III, and all 88 rows are loaded into SpecPicks:

| cfg 2 (2-chip, no AA) | 1600×1200 | 1280×960 | 1024×768 | 800×600 | 640×480 |
|---|---|---|---|---|---|
| Quake II 16 / 32-bit | 171.2 / 172.3 | 167.8 / 172.3 | 163.5 / 161.5 | 166.5 / 167.9 | 165.9 / 171.4 |
| UT99 GlideDrv 16-bit | 57.74 | 62.79 | 64.72 | 64.3 | 66.49 |
| UT99 D3DDrv 16 / 32-bit | process-exited | process-exited | 68.97 / exited | 69.9 / 68.12 | 70.6 / 69.11 |
| RtCW (Wicked3D openglv5) 16 / 32 | process-exited | no such mode | 102.6 / 94.5 | 125.8 / 115.1 | 127.1 / **timeout → wedge** |
| Quake III | not reached | | 1280×960: 115.9 / 78.5 (morning) | | |

Same shape as cfg 5: Quake II flat (CPU-bound), UT99 D3D dies above 1024×768.

**Two agent deaths, two outcomes.** At 19:12 (UT99 D3D) the agent died and the
listener-aware watchdog restarted it in 25 s; the sweep rebooted and carried on —
the first unattended recovery. At 19:54 (RtCW 640×480/32) the box went to the
display-driver wedge (9898 refused, 9897 mute) and nothing recovered it: that
needs a power cycle, as the Quake III wedge did this morning.

**Next, after the power cycle:** resume with `--configs 2,0 --titles
quake2,ut99:glide,ut99:d3d,rtcw:openglv5,quake3` (the runner skips measured
cells, so cfg 2 costs one boot for Quake III + RtCW 640/32, then cfg 0 runs).

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
