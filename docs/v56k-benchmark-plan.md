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
| AA settings cfg 1/3/4/6/7/8 | **blocked** — AA never engages via registry/env on AmigaMerlin's Glide; OUR Glide turns cfg 1 into a malformed 4-chip request and AmigaMerlin's kernel deep-wedges on it (2026-09-26 03:23); on vcr-kmd cfg 1/3/7 all wedged (2026-09-26 16:50) | — | supervised only. The safety net is built and its refusals proven on 86Box, not on silicon and not on master (resume point 2026-09-27 07:30: deploy dependency + steps 15-20); vendor golden via §4.1 |
| 128 MB vs 256 MB VBIOS switch | untouched under AmigaMerlin | — | physical switch; user action |
| Other drivers: official 3dfx 1.04.00 (Win2K), SFFT, in-house stacks | not run | — | each is a full re-run of the matrix |

### Resume point (2026-09-29 00:00) - Halo + Thief II D3D9 on the armed configuration; agent 1.91.0; full-desktop sweep running

**State (the party configuration):** vcr-kmd `integ-vcrkmd` build with **`Diag\D3D32`=1
and `Diag\D3DBigTex`=7 ARMED BY OPERATOR DECISION** ("keep armed for Halo"; Rainbow Six
fails with them), ICD 0.1.78, our h5 Glide 38a891e8, **agent 1.91.0**, desktop
1280x1024x32@85. Evidence: `scripts/benchmarks/results/v56k_lan_192.168.1.124/`
(`day/fbshots/halo_*`, `thief2_*`; `sweep_armed/`; `crash_avp/`).

| title | result |
|---|---|
| Halo 1.10 | **PASS to the textured main menu from the LIBRARY tree** (purged + GAMESYNC, 119 files / 1,496,651,991 B), fullscreen 1280x960, no dialog; desktop shortcut created by agent 1.90.2 from the published override; the box's own key (`fleet-gamekey-halo-pc-8`) re-applied by the launcher after every sync; `-novideo` where vcr-kmd runs (the Bink intros took 13+ min); exits clean on WM_CLOSE. **Gameplay needs a person** (the menu ignores synthetic input) |
| Thief II | **PASS in mission at 1280x960x32 through NewDark's Direct3D 9 display** (was DX6 at 640x480); the library launcher keeps `cam_ext.cfg` where the switches are armed; quit through its menu (ignores WM_CLOSE in mission) |
| Aliens vs Predator | **CHECK, pre-existing** (same before arming, 02:26 sweep): the mode goes to 640x480 but the game's window never shows - the desktop, garbled - and it has to be force-closed. Its unfocused window is what let the 23:21 sweep's ALT+F4 reach the desktop |
| Battlefield 1942 SP | "Cannot locate the CD-ROM" - the known SafeDisc 2.80 wall; the LAN launchers are the path |

**Landed tonight:** agent 1.90.2 (shortcut gate honours the published verdict), 1.91.0
(UIKEY refuses ALT+F4 to the shell / keys into the shut-down dialog; WINLIST
`foreground` + per-window `pid`; the 4294967-second console log flood); gamegate
operator overrides; Halo per-box keys that survive GAMESYNC; Halo `-novideo` and Thief
II D3D9 launchers; `lan_sweep.py` types only into the game's own focused window.

**Why .124 restarted at 23:24:** the sweep's ALT+F4 reached the desktop ("Shut Down
Windows") and its console-quit RETURN confirmed it - EventLog 6006, no bugcheck. The
only minidump on the box has `3dfxvs.dll` on its stack: an OLD vintage-driver crash,
dated 2003 because the RTC resets. Check the module list before blaming a dump.

**Open:** full-desktop sweep of the armed configuration (running - results below when
done); agent 1.91.1 (refuse close/break keys into the agent's own console - ready,
released after the sweep); Halo keys for `.145`, `.240`, `.123`, `.195` when they are
powered on (`assign_keys.py --map`); GAMESYNC <-> GAMERES rewrite 22 files / 36 values on
every `.124` sync; `.124` <-> NAS 0.55-0.83 MB/s with zero NIC errors (physical path);
AvP on vcr-kmd; Halo's slow Bink path; SoF2 on `.110` unverified (box off).

### Resume point (2026-09-28 13:10) - the requested titles on the all-ours stack; four fixes landed; Halo in progress

**State:** vcr-kmd = the `integ-vcrkmd` build (master + D3DBigTex/DdHeapFloor/fbshot
integration, all new switches default OFF, EXIT fix, vcrdd.dll version 6.14.1.1),
**`Diag\D3D32`=1 and `Diag\D3DBigTex`=7 ARMED for the Halo test - disarm and reboot
before the party**, GoodBoots 35. ICD **0.1.78** (system + Q2/Q3/RtCW game-local;
0.1.76 kept as `*.0176`), our h5 Glide 38a891e8. Evidence:
`scripts/benchmarks/results/v56k_lan_192.168.1.124/day/` (runs + `fbshots/`).

| title | result |
|---|---|
| Soldier of Fortune II MP | PASS soak + 6-map cycle on the fleet server; "very dark" = the engine's `r_overBrightBits 0` default - staged `base\mp\autoexec.cfg` now overbright 1 + picmip 0, redeployed to .124 and .110; luma 47 -> 65. Its startup "0 overbright bits" line is printed before the value is computed |
| Jedi Academy MP | PASS: server-side ClientBegin -> clean disconnect (4.5 min) + 6-map cycle; disc mounts through WinCDEmu (the "needs DAEMON Tools" was a harness quoting bug); GDI gamma now reaches the card |
| Carmageddon 2 | PASS in a race (Glide via the translator, fbshot of the 4-chip overlay) |
| Deathmatch Classic | crashed on 0.1.76 (glapi stub for glPNTrianglesiATI) - **ICD 0.1.78 fixes it**; renders dmc_dm2 1280x960 |
| Team Fortress Classic | renders 2fort 1280x960 |
| Unreal Gold | PASS at 1024x768 (library launcher now picks an exact 4:3 Glide 2 mode); **multiplayer** PASS after the fleet server was replaced by a 226 server (the library client is 226; the 227k server refused it) |
| Jedi Knight DF2 | Direct3D crashed in D3DIM - **vcr-kmd DP2 D3DOP_EXIT fix**; now plays 1280x960x16 D3D, clean quit |
| Mysteries of the Sith | PASS in D3D 1280x960 (7,014 DP2, 342k tris, 0 unparsed), clean quit |
| Halo | tree hand-deployed (gate refuses the GPU); big textures/DXT/A8R8G8B8 verified on silicon by d3dprobe; Halo stops at its own FATAL "video driver known to have serious issues" - trigger under analysis |

**Found and fixed:** vcr-kmd GDI gamma (DrvIcmSetDeviceGammaRamp); ICD 0.1.77 (3DFX
gamma Get never zeros) and 0.1.78 (no synthesized GL stubs); vcr-kmd DP2 EXIT;
Unreal Gold launcher 4:3 Glide mode; 226 Unreal Gold server; lan_check (JA/DMC/TFC,
server-side join proof, Dr. Watson settle). **Open:** Halo's InvalidDriver check;
`.124` <-> NAS SMB runs at 0.5-0.8 MB/s while .124 <-> host (11 MB/s) and .110 <-> NAS
(9.6 MB/s) are fine - physical path to check; SoF2 ignores WM_CLOSE (quit via menu);
disarm D3D32/D3DBigTex.

### Resume point (2026-09-28 07:30) - the night's LAN-party checks on `.124`; box left on the known-good state

**State:** vcr-kmd from master, `Diag\D3D32` ABSENT (armed for one boot, verified,
disarmed), BootAttempts 0 / GoodBoots 31, desktop 1280x1024x32@85, our h5 Glide,
ICD 0.1.76, `HKLM\Software\RetroAgent\GlideRender` = 1 (new: the V5 drives the
screen). Evidence: `voodoo-cleanroom/vcr-kmd/evidence/lan_20260928/night/`,
`.../silicon/d3d32_step16/`, `scripts/3dfx/3dfxctl/evidence/20260928/`.

| check | result |
|---|---|
| Quake III MP soak (fleet server) | PASS 20 min, 8 engine shots; server never rotated |
| Quake III map cycle | PASS 12 loads; a revisited map photographs identical |
| CS 1.6 MP soak v2 (fleet server) | PASS 30 min, **4 maps** (the v1 soak's "PASS" was a harness fault - fixed) |
| UT99 436 MP soak (fleet UT server) | PASS 30 min, **4 map changes**, clean exit |
| Quake II MP soak | PASS 30 min (q2dm1, no rotation) |
| RtCW MP soak / map cycle | PASS 20 min / 12 loads, in game on mp_village |
| Quake (GLQuake) soak | renders; the harness could not quit it (Quake's quit menu - fixed) |
| SoF | menus render at 1280x960; **no level loads on any fleet box** (WON CD check, single player too - library, not driver) |
| Quake / Hexen II "- 3dfx Voodoo" | FIXED in the library: a VSA-100 runs the main launcher (Enum\PCI prefix match) |
| Unreal Gold | GlideRender=1 -> GlideDrv opens Glide 2.70 (translator) then HANGS in its second open; open |
| Desktop sweep, 36 launches one boot | board healthy after every one; agent never died |
| 32 bpp Direct3D (step 16) | verified on silicon (d3dprobe 40/40, 42/42); default stays off (see vcr-kmd README) |
| 3dfx Control Panel | 2D rows showed OFF while ON - fixed, deployed, Apply-without-reboot verified |
| `vcrctl fbshot` (new) | desktop layer verified (matches GDI); Warcraft II menu seen where GDI is black; overlay (Glide) decode **fixed offline, not yet on the card** (branch of 2026-09-28 09:00): memBase1 above lfbMemoryConfig's begin page is a linear tile aperture that merges the SLI bands - inverting the old read on the saved PNG gives the Quake III main menu; 8 bpp CLUT via a read-only miniport kind (the greyscale was reg_op's AllowPoke gate, not exclusive mode) |

**Open, in order:** Unreal Gold's Glide re-open hang (translator + our glide3x);
Rainbow Six with D3D32 on a clean desktop; Thief II NewDark (D3D9);
UT2004 (HAL texture limit 256 -> 2048, FOURCC); Descent DOSBox ddraw at 640x400
scans out black; fbshot's overlay/SLI decode (fixed offline - verify on the card); the host address (.196 vs .132);
AA and the P1120 branch with the user.

### Resume point (2026-09-28 03:00) - LAN-party pass: the priority titles run on the all-ours stack; AA untouched (user away)

**State of `.124` (V5 6000, 256 MB mode, HP P1120):** vcr-kmd from master
(2D patterns/lines on by default, standard modes listed first), our guarded h5
Glide in `system32\glide3x.dll` (md5 38a891e8; AmigaMerlin's kept as
`glide3x_am31.dll`), MesaFX ICD **0.1.76** as the system ICD and as the
game-local `retrogl.dll` in Q2/Q3/RtCW, the 3dfx Control Panel 2.0
(`C:\RETRO_AGENT\3dfxctl.exe`, Start Menu). Glide's settings key on this box is
`Services\3dfxvs\Device0\glide` (cfg 5, FX_GLIDE_REFRESH 75).

**LAN check** (`scripts/benchmarks/lan_check.py`, each title as its desktop
shortcut runs it; evidence `voodoo-cleanroom/vcr-kmd/evidence/lan_20260928`):
| title | path | result |
|---|---|---|
| Quake III | ioquake3 -> system ICD, 1280x960x32 | timedemo 84.7, MP soak on the fleet server |
| Quake II | quake2 gl_driver opengl32, 1280x960 | timedemo 138.5, MP soak |
| CS 1.6 | hl.exe -gl -> system ICD | timedemo 93.1, MP soak (server: entered the game) |
| RtCW MP | WolfMP retrogl 0.1.76, 1152x864@75 | MP soak (spectator in game) |
| UT99 436 | OpenGLDrv -> ICD, 1280x960x16 | startup 220 s -> 5.9 s with 0.1.76; MP soak (DM-Deck16][) |
| Quake (GLQuake) | system ICD, 1280x960x32 | after the mode-order fix: renders, MP soak on NetQuake |
| Quake - 3dfx Voodoo | 3dfx MiniGL (Q2 3.20 build) | the MiniGL cannot drive a VSA-100 (wglCreateContext fails before grSstWinOpen; the Glide 2 path under it works: glide2probe). **Library fix 03:10**: on an NT box with PCI 121A:0009 the launcher runs `Play Quake.bat` (same for Hexen II's Voodoo shortcut); on-box test queued in the night batch |
| SoF | ref_gl, 1280x960x32 | menu renders; F11 crash fixed in the library (`user\scrnshot`) |
| Red Alert 2 | aqrit ddraw wrapper | main menu renders; skirmish not driven |
| Turok 2 MP | T2-Glide3, 640x480x16, 4 chips | hosted LAN game runs; SP needs a CD image the fleet lacks |
| Carmageddon 2 | Glide (translator) | fixed: the whole nGlide set moves aside; Glide opens on 4 chips |

**Against AmigaMerlin, same demos, 1280x960x32:** Quake III 84.7 vs 75.1-78.5,
Quake II 138.5 vs 84.7, CS 1.6 93.1 vs 52.7-54.5 (`v56k_titles` rows).

The rest of the desktop is in `lan_sweep.py`'s run (scripts/benchmarks/results,
gitignored). Next: finish the sweep's follow-ups, the Voodoo-Quake MiniGL, the
host address (the fleet's "Join" launchers point at .132; the host is .196),
then AA with the user at the box (arms in master: SliAAFifoGate first).

### Resume point (2026-09-27 17:00) - follow-ups and engine text landed; the account's weekly limit is spent

Since 07:30, all on master, none on silicon:
- **Follow-ups** (`f146917`..`231d8ee`): flip counters of a same-mode DirectDraw
  session are logged once at DestroyDDLocal (verified on 86Box: 60 / 601, each in
  its own process); `VcrDdRoom` returns at once after a 2D give-up (it spun a
  SPIN_CAP per triangle); `d3dprobe_run --noz`; glidelab `--aa-lfb-read
  --i-am-at-the-box` and `--maplog`; the 86Box `fast_frames` are a guest-clock
  artefact, not early completion; CLAUDE.md: `REGDELETE` deletes a KEY.
- **Engine text** (`e7ff9ee`): DrvTextOut by monochrome expansion, 0 bad on the
  bed, but slower there than the software path, so OPT-IN: `Diag\Accel2DText = 1`
  (read at the next mode change). Evidence `vcr-kmd/evidence/86box_v3/2d_*`.
- **Not done:** 2D patterns and lines (the track never started); why XP's own
  driver draws text 2.4x faster on the bed (suspect: a command FIFO instead of
  PCI FIFO writes).

Add to the supervised checklist below, after the step-16 regression:
- **Text on silicon:** gdilab `--tests base,text,bench` at the desktop mode with
  `Accel2DText` absent, then `REGWRITE ... Diag Accel2DText REG_DWORD 1`, one
  paced mode change, the same run; compare glyphs/s. Keep it only if it beats the
  software path; remove the value with `EXEC reg delete "<Diag key>" /v
  Accel2DText /f` (never REGDELETE - it would erase the whole Diag key).

### Resume point (2026-09-27 07:30) — the AA safety net, the 32 bpp D3D hardening and the flip work are built, reviewed and 86Box-verified; nothing has touched silicon; the supervised checklist

**Where the code is - NOT on master, NOT pushed:**
- retro-agent branch `worktree-vk-int` (`.claude/worktrees/vk-int`): the four
  tracks merged (SLI/AA safety net, D3D32 hardening, flip, glidelab tracing),
  `5e4ca36` integration fix, `a3b71de` 86Box verification, `a381451` docs.
- Glide fork `voodoo-cleanroom/build/retro3dfx-glide`, branch
  `glide-devel-sezero`, **ahead 4 of origin**: `0b21976` (AA-TRACE),
  `631221b` + `7736039` (SLIAA-GUARD), `e767d89` (review fixes).
- `retro-3dfx` branch `worktree-vk-findings` (`.worktrees/vk-findings`,
  `e6b6ad6`): four FINDINGS entries for 2026-09-27.
- What each switch does and how to arm it: `voodoo-cleanroom/vcr-kmd/README.md`
  ("Diag switches", and Status: AA safety net, 32 bpp, flip completion, the
  86Box integration build). 86Box evidence:
  `vcr-kmd/evidence/86box_v3/int_20260927/`.

**Critic plan, steps 1-14** (the 2026-09-27 critic plan; the numbers are the
ones the supervised steps below continue):

| step | what | status |
|---|---|---|
| 1 | commit the AA evidence that lived only in the transcript; correct the AA table | **done offline**, on master (`93fed90`, `2442117`, `2587bd3`) |
| 2 | unbreak the shared Glide clone and the suite; harden the trace | **done offline** (fork `0b21976` + `e767d89`); the fork push is **pending** |
| 3 | recover the four orphaned `.143` compat screenshots | **done** (`~/lan-proof/box143/`; `test_compat_evidence_survives.py` passes) |
| 4 | guard `5be6a59` (32 bpp D3D) before any deploy | **done offline** (`4a9793b` + `46eef4c` + `967b4aa`; `Diag\D3D32` and `Diag\Reset3D` default off) |
| 5 | 86Box: the 32 bpp refusals, D24S8 at 16 bpp, 16 bpp no-regression | **done on 86Box**: every 32 bpp and 16 bpp + D24S8 device is refused by the D3D8 runtime from the caps, before the driver; the HAL's CanCreateSurface guard (511/11) is reachable only the DirectDraw 7 way (`ddlab zsurf`: 24- and 32-bit Z refused), so step 5's "refused by CanCreateSurface" holds for DirectDraw 7 / D3D7 applications only. 16 bpp: gdilab 0 bad, blt -1.7 % (in the spread), d3dprobe 42/0 and 40/0 |
| 6 | flip offline: `vcr_flip.h`, counters, GetScanLine, ddlab per-frame stats; NOT the scanline proposal | **done offline** (`e961bae` + `c664ce8` + `3526483`) |
| 7 | flip on 86Box, 16 and 32 bpp, both orders | **done on 86Box**: no half rate (58.7-64.0 flips/s at 60.35 Hz, 89-96 % by retrace). The `FlipDeadline` 0.889x/0.95x A/B cannot be shown there (~3 ms of emulator overhead per frame) - it moves to step 16 |
| 8 | housekeeping: the PXE pipeline fix + the host outage entry | host-issues-log entry **done** (`097b1f7`); the PXE DevicePath fix **not done** (`run_all.sh` still fails on it) |
| 9 | kernel safety net | **done offline** (`412b03c` + `5fa2741`) |
| 10 | Glide guards | **done offline** (fork `631221b` + `7736039` + `e767d89`, not pushed) |
| 11 | trace coverage + `glidelab --trace` | **done offline** (fork `0b21976` + `e767d89`; retro-agent `b811d37` + `c2bde54`) |
| 12 | flag-gated vendor recipe + read-back | **done offline** (`cebdf4f`, `eafe810`, `5fa2741`; the read-back has its own switch, `Diag\SliAAReadback`) |
| 13 | `vcrctl sliaa` + the refusals on 86Box | **done on 86Box**: the tool's gates, then EDENIED -4 (`SliAA` absent) and EINVAL -1 (`SliAA` = 1: one chip); zero SLI register writes in every run; `sliaa off` from a separate process clears a stale owner (`5e4ca36`) |
| 14 | research the 3dfx Tools route | **done offline** (§4.1) |

**Deploy dependency - read before ANY deploy to `.124`:**
1. **The kernel's AA refusal protects `.124` only together with the
   SLIAA-GUARD `glide3x.dll`.** A Glide built from origin (`d161bd4`) ignores
   the kernel's FAIL and opens its multi-chip AA layout anyway; the SLI
   request comes after HWCSETEXCLUSIVE and the escapes after it are
   unchecked, so nothing in the kernel can stop that open.
2. **Push the fork first** (`glide-devel-sezero`, the four commits above),
   then rebuild with an **explicit workdir**: `bash
   voodoo-cleanroom/build-stack.sh
   /home/voidsstr/development/retro-agent/voodoo-cleanroom/build`. Without it
   the script clones origin and builds a Glide with none of the guards.
3. Land `worktree-vk-int` on master before its `vcr-kmd` build goes to the
   box, and identify the build by file md5 and the recorder's DRIVER_ENTRY
   struct size (0x1388): `vcrctl info` says `build 1` for every build.
4. On the box: the md5 of `C:\Games\Quake2Complete\glide3x.dll` (what glidelab
   loads) must be the new build's, and `glidelab_run`'s plan line must say
   `SLIAA-GUARD yes`. glidelab refuses an AA open on an unguarded Glide
   (rc 13), but a game does not. Until then, confirm the Glide AA
   configuration is 0/2/5 before any Glide app runs.

**Supervised checklist - steps 15-20, with the user at the box.** LICSTATUS
first; `safe-reboot.py` only; one config per CLEAN boot; every mode switch
paced by `vcr_pace`. The `Diag` switches live in
`HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag`: arm with
`REGWRITE HKLM SYSTEM\CurrentControlSet\Services\vcrmp\Diag <name> REG_DWORD 1`
and REGREAD it back; disarm with 0 or `EXEC reg delete
"HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag" /v <name> /f` (the agent's
REGDELETE deletes keys - never point it at `Diag`). `Sli*` act on the next
request; `D3D32`, `Reset3D`, `AllowPoke` at the next boot; `FlipDeadline` at
the next mode set.

15. **First contact, read-only - no Glide, no mode switch.** The boot #18
    read-back itself was done at 00:15 (`93fed90`). Repeat the read-only part,
    because the box may have been used since:
    - `glidelab_run.py 192.168.1.124 fill --collect`: it reads every place
      `SSTH3_SLI_AA_CONFIGURATION` can be (the display class key AND
      `Services\3dfxvs|banshee\Device0\glide`, which is the one our Glide
      reads). If it banners **AA CONFIGURATION ARMED**, run `--collect
      --restore-cfg 5` before any Glide app.
    - `REGREAD` `Services\vcrmp\Diag`: `SliAA`, `SliPersistAll`,
      `SliAAVendorRecipe`, `SliAAReadback`, `D3D32`, `Reset3D` and
      `FlipDeadline` absent or 0.
    - `vcrphases.py 192.168.1.124 --prev`, and `xpminidump` over
      `C:\WINDOWS\Minidump`. Commit what comes back.
16. **Non-AA silicon: deploy, regression, 32 bpp D3D, flip.** No SLI/AA
    config other than 0/2/5. Stop at the first event 512 a=9.
    - Deploy (the dependency above), rollback copy kept. Every `Diag` switch
      absent for the regression boot. Set `RETRO_GLIDE_MAPLOG` for the Glide
      runs and look for `master reset skipped` and `SLI/AA disable escape NOT
      sent`.
    - Regression: glidelab cfg 0/2/5 fill + bands (1124.6 Mpix/s ±1 %, 0 bad
      band lines), Quake II cfg 5 (~173 fps), ddlab and d3dprobe at 16 bpp (D3D
      40/40). Read every 513 what 1 (ContextCreate) line and record whether a
      target or a Z lands at video-memory offset 0 (the heap starts there);
      the 16 bpp runs must pass either way.
    - Flip: `ddlab_run.py 192.168.1.124 flip --res 800x600 --bpp 16|32
      --frames 60|600`, 16 then 32 and the reverse, reading 511/12-13 after
      each (a session in the desktop's own mode logs them only at the next mode
      change - force one paced switch). Healthy, and the half-rate question
      answered: done-by-retrace ≈ frames and `flips_s_first_last` ≈ the
      refresh; `fast_frames` near 0 (86Box ran 2-3 % over the refresh with
      22-29 fast frames per 600). Then the deadline A/B: first check ddlab's `vblank_hz` against
      `refresh_mhz`/1000 (well under 1 %); `--work-us` ≈ 1.05 frames (12353 us
      at 85 Hz); `FlipDeadline` absent vs 1 (picked up at the next mode set,
      which ddlab's own switch provides). Expect ~0.889x vs ~0.95x of the
      refresh. Disarm and read it back absent.
    - 32 bpp D3D, its own boot: `D3D32` = 1, plus `Reset3D` = 1 if Glide runs
      earlier in that boot; confirm 513 what 16 in the recorder. `d3dprobe
      caps` (expect `hal_fullscreen.X8R8G8B8` = 1, `zmatch` X8R8G8B8 with
      D24X8/D24S8 = match, R5G6B5 with D16 = format available and match);
      fullscreen 640x480x32 clear without Z (`d3dprobe_run.py ... --tests
      "clear --noz"` - the runner has no `--noz` flag yet), then with Z, flat +
      ztest, the full list, perf, windowed last; and a 16 bpp fullscreen run
      from the 32 bpp desktop with `D3D32` = 1. On any failure pull the
      recorder: 513/12, 513/15, 513/17, 511/11.
    - `Reset3D` = 1: after a clean Glide cfg 0 session the recorder must show
      604 a=3 c=1; c=2 (chip not idle) or c=3 (Glide's command FIFO still on)
      at release - stop and pull the recorder. A KILLED Glide client needs a
      cold boot before any 32 bpp D3D step (`Reset3D` does not run for it).
    - The unproven 16 bpp stencil premise: a 32 bpp OpenGL session with stencil
      on (`Reset3D` = 0), exit it cleanly, then `d3dprobe render --full --bpp
      16`. A failure where it passed from a cold boot means the chip honours a
      stale stencil enable at 16 bpp; the remedy is `Reset3D` or a D3D-only
      stencil clear behind its own switch, not a change to the default.
    - Disarm `D3D32` and `Reset3D` afterwards.
17. **Step A - the refusals on silicon.** `SliAA` absent, the SLIAA-GUARD Glide
    deployed. ONE `glidelab_run.py 192.168.1.124 fill --res 1024x768 --refresh
    60 --cfg 1 --trace 1`. Expect glidelab's RESULT error "grSstWinOpen not
    made": our Glide refuses cfg 1 on 4 chips before any buffer or mode set, so
    no escape and no HWC_SLIAA phase. The agent PINGs afterwards. Optional, on
    its own clean boot: a shape Glide does send (cfg 7) with `SliAA` absent
    proves the kernel's refusal and the give-back - PhaseLog `SLI_STEP REFUSED
    AA_OFF` and no `SET_BEGIN`; the trace `open REFUSED after the mode set -
    giving the display back`, a ~3 s hold, `release: HWCRLSEXCLUSIVE`; then
    `vcrctl info` shows no exclusive owner, and 2D and the pointer are back.
18. **Step B - kernel state alone** (moderate risk), cfg 3 then cfg 7, each on
    its own clean boot: arm `SliAA` = 1, `SliPersistAll` = 1 (the last phase
    names the write if it wedges) and `SliAAReadback` = 1 for that boot only.
    `vcrctl sliaa 4 1 1 0 1 8 16 <tileMark> 0 <depthlo> <depthhi>
    --i-am-at-the-box` (cfg 3) / `vcrctl sliaa 4 0 1 1 1 ...` (cfg 7). The tool
    refuses AA on a desktop not in 2x mode unless `--force-desktop-pll`. Read
    config space only: `vcrphases.py 192.168.1.124` decodes `Diag\SliAAState`
    (each chip's 0x40, 0x48, 0x80-0x94, 0xAC), `vcrctl pci` for the bridge
    (0x04, 0x1c, 0x3c, 0xC4). Hold 5 s, `vcrctl sliaa off`, read again,
    disarm. Accept: the values match `k_aa_tables` in
    `tests/native/test_vcr_kmd_sli.c` (the dos_mode.c arm; the old kernel's
    cfg 1 state is `k_cfg1_sent_old`), and the box survives, or PhaseLog names
    the last write. The vendor arm (`SliAAVendorRecipe` = 1, the REAL tileMark
    or MEMINFO refuses it) is a further clean boot.
19. **Step C - de-confound cfg 7** (high risk; expect a power cycle). A CLEAN
    boot where cfg 7 is the first Glide app. `SliAA` = 1, `SliPersistAll` = 1;
    `SliAAReadback` and `SliAAVendorRecipe` ABSENT, so cfg 7's register writes
    are the ones that ran on 2026-09-26 (only SET_DONE's persisted value
    changes). Check the plan line says `SLIAA-GUARD yes, AA-TRACE yes`, then
    `glidelab_run.py 192.168.1.124 fill --res 1024x768 --refresh 60 --cfg 7
    --trace 2`. The trace must show `splash: plugin load SKIPPED
    (FX_GLIDE_NO_PLUGIN=1)` and `cfg dumps OFF` (no `--trace-cfg`). Pass: the
    16:25 wedge was an artefact of the cfg 3 session; update this plan. Wedge:
    power cycle, then `glidelab_run.py 192.168.1.124 fill --collect` (step log +
    trace; the last line of `<mode>.log.trace` is the step), `--collect
    --restore-cfg 5` if it banners an armed value, and `vcrphases.py --prev`.
    Disarm `SliAA`.
20. **Step D - the cfg 3 read-back**, one variable per clean boot. (a) Guard
    on: `glidelab_run.py ... bands --cfg 3` must end "grLfbReadRegion refused",
    not a wedge. (b) The opt-in read, `RETRO_GLIDE_AA_LFB_READ=1` in glidelab's
    process environment (glidelab and glidelab_run have no flag for it yet),
    with SLI READ_EN cleared on chips 1-3 (not built). (c) The vendor recipe,
    `SliAAVendorRecipe` = 1 with the opt-in read. Traced `bands` flushes every
    scanline (768 at 1024x768): raise `--timeout` above 180 s. Optional
    afterwards: a vendor AA golden through the 3dfx Tools route (§4.1), and our
    Glide's cfg 3 fill over AmigaMerlin's kernel.

### Resume point (2026-09-27 00:30) — AA post-mortem read back; the AA safety net is being built; AA stays supervised-only

Path key for this section: kernel = `voodoo-cleanroom/vcr-kmd/`; Glide = the
h5 fork (`retro3dfx-glide/glide3x/h5`), line numbers at fork HEAD `d161bd4`;
vendor = `retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Miniport/H5/` (read
for guidance only, nothing copied).

**What the read-back settled** (read-only, 2026-09-27 00:15:
`vcr-kmd/evidence/glidelab/postmortem_20260927/README.md`). `.124` had booted
once since the cfg 1 wedge, so the miniport's `Prev*` history still held
boot #18 - the cfg 1 run:
- `HWC_SLIAA a=4 b=0x102` - **4 chips**, SLI off, AA on, analog
  (b = sliEn | aaEn<<1 | sampleHigh<<4 | analog<<8, `miniport/vcrmp_multi.c:239-241`).
  Not a 1-chip request.
- `SET_BEGIN` 115.750 s -> `CLOCK_6K` -> slave `INIT_BEGIN` x3 -> mode-index
  writes -> `PCIINIT0` x4 -> **`SET_DONE` at 116.328 s, then nothing**. No
  `SLICTRL` steps.
- glidelab's flushed step log ends `step: grSstWinOpen 1024x768 60Hz origin
  upper` with no `-> context` line: the box froze **inside Glide's open, after
  the kernel reported SET_DONE**.
- Armed state at read-back: `Services\3dfxvs\Device0\glide`
  `SSTH3_SLI_AA_CONFIGURATION = 5`, so an unattended Glide app opens at cfg 5
  (SLI, no AA). No minidump from any wedge: hard freezes, not bugchecks.
- NOT settled: the kernel's warn mask (W_NOMUX). The persisted `SET_DONE`
  phase drops the value (`k_log`, `vcrmp_multi.c:86-95`) and the NOMUX step is
  not in the persisted set (`:79-84`). Safety-net item 3 below fixes that.

**cfg 1 is a malformed request - code-proven (hypothesis 1).** Glide lays out
samples for 4 chips (`glide3/src/gsst.c:1535`, the sample switch at
`1737-1767`, `enableSecondaryBuffer=FALSE` at `1839`), applies
`forceSingleChip` only afterwards (`2125-2128`), then sends
`dwChips = pciInfo.numChips = 4` with sliEn 0, aaEn 1, sampleHigh 0, analog 1
(`minihwc/minihwc.c:4927-4962`, analog forced at `4711-4713`). The kernel has
no video-mux branch for that tuple (`miniport/vcrmp_sli.c:836-839` returns
W_NOMUX), but by then it has written snoop, swap, pciInit0 and AA LFB control
to all four chips, and the escape reports a warning as success
(`display/vcrdd_escape.c:154`). The vendor miniport only logs the case
(`SLIAA.C:3371-3373`), and the vendor display driver never sends it: on a
4-chip board it rewrites cfg 1-4 to 0 (§4.1). AmigaMerlin's kernel wedged on
the same request (resume point 2026-09-26 03:40). The freeze mechanism is
unknown, but refusing the tuple before any write fixes it without knowing it.

**cfg 7 is confounded.** It ran at 16:25:05, 20 s after the cfg 3 fill
(16:24:35), in the SAME boot (#17), with no reboot between - ground rule 1
broken. cfg 3's close-time disable (`vcrmp_sli.c:1077-1128`) does not restore
pciInit0, the slave init or the 6000 clock, and whether it reached `OFF_DONE`
cannot now be checked (boot #17 has rotated out of `Prev*`, which keeps one
boot). cfg 3 bands wedged 2.6 h into busy boot #16. **Only cfg 1 was a
clean-boot observation.**

**AA hypotheses, ranked** (critic synthesis of the 2026-09-26 offline reviews):
1. cfg 1 = the malformed 4-chip / no-SLI / 2-sample tuple above. Strongest:
   code-proven trigger, reproduced on two kernels.
2. cfg 3 LFB read-back hang = an ambiguous read owner. Chips 0/1 and 2/3 share
   SLI compare masks with SLI READ_EN on (`vcrmp_sli.c:896-913`); AA LFB
   READ_EN (the inter-chip read handshake) is never set (`935-937`), the
   secondary base is 0, no DIV4, and chip 3 has RD_SLV_WAIT (`962-967`). The
   vendor sets READ_EN, base=tileMark and DIV4 for exactly this tuple
   (`SLIAA.C:2396-2449`). pciInit0 in both goldens has retry 0 / timeout off,
   so a read that never completes may become a permanent bus hold. Register
   state proven, mechanism HYPOTHESIS. For it: fill (no reads) completes; SLI
   reads with unique masks work (cfg 2/5 bands, 0 bad lines).
3. cfg 7 hang in `grSstWinOpen`: UNRESOLVED and confounded. Candidates, in
   rough order: (a) state left by the cfg 3 session or its disable; (b) a
   video-clock-domain stall at the first DAC access (`hwcGammaRGB`,
   `minihwc.c:8594-8620`) after the kernel re-muxes to DIV4; (c) the SLI-off
   4-chip state meeting Glide's open-time idle wait or slave `lfbMemoryConfig`
   reads (`minihwc.c:5201-5310, 2720-2737`); (d) our deviations from the
   vendor for this tuple (depth aperture, base 0, no READ_EN) - weaker, open
   does no LFB access. A clean-boot, traced cfg 7 run is the discriminator.
4. Our bounded idle wait (fork `215a9e7`) resets the master through
   miscInit0/1 after ~2 s (`minihwc.c:2749-2764`); with slaves snooping init
   registers the reset may reach them and turn a busy board into a hard
   freeze. HYPOTHESIS from one review; nothing shows the branch ever ran.
5. Secondary base 0 with CPU_WRITE_EN / DISPATCH_WRITE_EN set aliases
   AA-duplicated LFB writes onto VGA and the command FIFO. Weak for these
   wedges (glidelab writes no LFB); matters for LFB-writing games.
6. Latent: the secondary base is shifted left 4 (`vcrmp_sli.c:932-937`,
   marked UNVERIFIED) where the vendor and Glide use an unshifted byte address.
   Harmless at base 0; fix together with any change that sends tileMark.
7. Low: pciInit0 bit 11 (DISABLE_IO) is set by the vendor and clear on ours;
   meaning unestablished.

**The safety net, being built offline now (NOT on master yet; none of it has
touched silicon)** - *built and 86Box-verified since: see the 07:30 resume
point above for the status and the checklist:*
1. Kernel refuses unsupported tuples: a pure predicate in `vcr_sli_set` that
   accepts only combinations with a video-mux branch; anything else is refused
   (`VCR_SLI_R_COMBO`) **before the first write**, and the escape maps it to
   `VCR_HWC_FAIL`. cfg 0/2/5 write sequences must stay byte-identical.
2. `Diag\SliAA` kill switch, **default 0** = every aaEn request refused before
   writing. Set to 1 for one supervised boot at a time.
3. Persist the warn mask and the NOMUX step, so a post-mortem can read them.
   `HWCEXT PCI_OP` also refuses writes to 0x40, 0x48 and 0x80-0xAC unless
   `allow_poke`.
4. Glide guards (fork branch): cfg 1 on >2 chips sends the chips Glide drives,
   or refuses; tuple check before the escape; honour `retVal`/`resStatus`;
   refuse a READ_ONLY LFB lock at `grPixelSample>1 && chipCount>1` unless
   `RETRO_GLIDE_AA_LFB_READ=1`; no miscInit0/1 reset in the idle wait on
   multi-chip SLI/AA.
5. Flushed Glide trace (plain `getenv`, default off; `glidelab --trace N`
   also sets `FX_GLIDE_NO_SPLASH=1` and pulls the trace next to the mode log):
   brackets on every hardware call between the escape and the first swap,
   including `hwcGammaRGB`, the idle-reset branch and the close-time disable;
   level 2 adds a bounded `grFinish` so the last line names the executed step.
6. Vendor-recipe variant behind `Diag\SliAAVendorRecipe`, **default 0**:
   unshifted masked secondary base; base=tileMark + AA READ_EN + DIV4 for
   1-sample-per-chip tuples; the whole tiled depth aperture for cfg 7;
   `sliCtrl=0` on AA-only requests; a flushed post-`SET_DONE` config readback
   into `Diag\SliAAState`. The expected register tables for cfg 3/7/1 go into
   `tests/native/test_vcr_kmd_sli.c` (both the dos_mode.c and vendor values).
7. `vcrctl sliaa <tuple>` / `sliaa off`: a program-only probe that sends
   `HWCEXT_SLI_AA_REQUEST` through ExtEscape with no Glide, no LFB and no MMIO
   snapshot. Refusal paths are proven off silicon first.

**Supervised sequence** (user at the box; one config per CLEAN boot;
LICSTATUS first, `safe-reboot.py`, every mode switch paced by `vcr_pace`):
0. First contact, read-only: confirm `SSTH3_SLI_AA_CONFIGURATION` is 0/2/5
   before any Glide app runs (it was 5 at 00:15). Then deploy the safety-net
   builds (rollback copy kept, `predeploy`, md5 of `vcrdd.dll`/`vcrmp.sys` and
   of `C:\Games\Quake2Complete\glide3x.dll` - the DLL glidelab loads - checked
   on the box) and prove no regression without AA: glidelab cfg 0/2/5 fill +
   bands, Quake II cfg 5, ddlab/d3dprobe at 16 bpp. Parity: 1124.6 Mpix/s
   ±1 %, 0 bad band lines, Quake II ~173 fps, D3D 40/40.
1. **Step A - the refusals on silicon.** `Diag\SliAA=0`, Glide guards in, ONE
   glidelab cfg 1 open. Accept: `grSstWinOpen` returns an error, PhaseLog
   shows the refusal and no `SET_BEGIN`, the agent PINGs afterwards. Risk low:
   nothing may be written.
2. **Step B - kernel state alone.** cfg 3 then cfg 7, each on its own clean
   boot, `Diag\SliAA=1` for that boot only: `vcrctl sliaa <exact tuple>`, read
   config space only (each chip 0x04, 0x40, 0x48, 0x4c, 0x80-0x94, 0xac; the
   bridge 0x04, 0x1c, 0x3c, 0xc4), hold 5 s, `sliaa off`, read again. Accept:
   values match the item-6 tables, and the box survives (kernel state alone is
   not the trigger) or PhaseLog names the last write. Risk moderate.
3. **Step C - de-confound cfg 7.** A clean boot where cfg 7 is the FIRST Glide
   app: glidelab fill at trace level 2, current (dos_mode) recipe. Pass -> the
   16:25 wedge was an artefact of the cfg 3 session; update this plan. Wedge ->
   the committed trace names the step and settles hypothesis 3. Risk high:
   expect a power cycle.
4. **Step D - the cfg 3 read-back, one variable per clean boot.** (a) guard on:
   expect a clean refusal; (b) opt-in read with SLI READ_EN cleared on chips
   1-3 (master only); (c) the vendor-recipe variant. Accept: bands completes,
   or the trace names the scanline and band pair. Risk high for (b) and (c).
5. Optional, after D: a vendor AA register golden through the 3dfx Tools route
   under AmigaMerlin (§4.1: driver swap, 2 reboots, rollback required), and our
   Glide cfg 3 fill over AmigaMerlin's kernel - the run that separates kernel
   from Glide for the configs in dispute. Commit a
   `golden/sli_amigamerlin_aa_cfgN.json` only with all three AA proofs (§4).

### Resume point (2026-09-26 16:50) — full open stack verified; AA wedges the box; AA work is OFFLINE until the user is present

**Proven on `.124` today, all on our kernel driver (vcr-kmd, on master):**
Glide 4-chip fill 1124.6 Mpix/s = AmigaMerlin (cfg 2/5, 0 bad band lines);
Quake II all-open 173.4 fps 1024x768x16 cfg 5; D3D 40/40 (PARMADJUST fix);
D3D present-immediately 85 -> 151 fps (DDCAPS2_FLIPNOVSYNC); glidelab now
hands Glide its cfg/refresh via _putenv (Glide's getenv never saw
SetEnvironmentVariableA - every earlier `--no-reboot` glidelab run used the
registry's cfg 5).

**AA (FSAA) - three deep wedges (CPU frozen, Num Lock dead, power cycle):**
(table corrected 2026-09-27 after the post-mortem read-back - see the
2026-09-27 resume point above; the first version of this table was written
before cfg 1 was read back and without the run order)
| cfg | what reached the kernel | kernel SLI/AA setup | result |
|---|---|---|---|
| 3 (4 chip, 2-sample) | 4 chips, SLI on (2 units), AA on | SET_DONE (boot #16) | fill completed ONCE (625 Mpix/s - that number alone cannot tell 2-sample AA from 2-unit SLI; no frame checked); bands: `grLfbReadRegion` back buffer hangs, 2.6 h into a busy boot |
| 7 (4 chip, 4-sample) | 4 chips, SLI off, AA on | SET_DONE (boot #17) | hangs inside `grSstWinOpen` - **CONFOUNDED: ran 20 s after the cfg 3 fill in the SAME boot** (ground rule 1 broken); unconfirmed on a clean boot |
| 1 (meant as 1 chip, 2-sample) | **4 chips**, SLI off, AA on, 2-sample: `HWC_SLIAA a=4 b=0x102` - a malformed request | `SET_DONE` at 116.328 s (boot #18; **read back 2026-09-27**) | froze **inside `grSstWinOpen`** after the kernel reported SET_DONE (AmigaMerlin's kernel wedged on the same request) |

Evidence: `vcr-kmd/evidence/glidelab/postmortem_20260927/` (boot #18's
phase history, both on-box step logs); `aa_cfg*.log` beside it are host-side
EXECW timeouts only. **Rule (memory `aa-tests-need-user-present`): no AA
config on `.124` unless the user is at the box.** Retracted: "Quake II at
cfg 3 (no LFB reads) is the one AA test likely to pass" - it rested on the one
cfg 3 fill run, whose boot wedged 20 s later on cfg 7. Next steps: the
2026-09-27 resume point.

**Still open (safe):** 16 bpp DirectDraw flip at half the refresh; 32 bpp D3D
render targets (windowed D3D on a 32 bpp desktop fails CreateDevice).

### Resume point (2026-09-26 12:40) — vcr-kmd on master; the monitor is protected; D3D 40/40 on silicon

**The monitor rule first** (user, 08:04: "how many times are you engaging the
monitor ... make sure that is safe"): the first battery switched all 123
vendor modes with a desktop bounce after each, ~250 re-syncs of the Sony at
2/s. Every live switch now goes through `vcr-kmd/tools/vcr_pace.h` (>= 3 s
apart across processes, modes held >= 3 s, a lock, paced kills), sweeps are
one `vcrctl modeseq` process, capped, EDID-gated on the host (`mon_src` 1/2);
the all-mode register check is `golden_compare.py`, off the box (123/123).
Memory: `pace-monitor-mode-switches`. Plan any new switching run in
switches and print it.

**vcr-kmd landed on master (`0d361cc`)** with 2D engine, DirectDraw, the DX7
D3D HAL, the monitor-safety work and the driver-side EDID fallback. On `.124`
(battery `vcr-kmd/evidence/silicon/vcrkmd-v5-2` and `-4`):
- modes 7/7 in one paced process (38 s), registers identical to AmigaMerlin;
- gdilab 0 bad (32 bpp); DirectDraw flip 0 mismatch / blt 0 bad at 16 and
  32 bpp (blt 467 / 224 Mpix/s);
- D3D 40/40 fullscreen at 640x480 and 1024x768 - after PARMADJUST (without
  it falling gouraud channels stayed constant; retro-3dfx FINDINGS 1c23caa).

**Open, in order:**
1. Glide on vcr-kmd, paced: `glidelab_sweep.py 192.168.1.124 --label vcrkmd
   --cfgs 0,2,5 --res 1024x768 --no-reboot` (9 sessions = 18 switches, under
   the 24 cap), then Quake II all-ours; then land.
2. d3dperf holds 85 fps = refresh with --novsync: the immediate present is
   not honoured (Flip DDFLIP_NOVSYNC?). DirectDraw flip at 16 bpp runs at half
   the refresh (42/s at 87.6 Hz) where 32 bpp runs one per retrace (86/s).
3. The cursor compare differs from AmigaMerlin (hardware-cursor bit clear,
   stale hwCurLoc) - both captures may be of a hidden pointer; check it by eye.
4. AA configs one at a time (step 5 below), with the pace gate.

### Resume point (2026-09-26 08:05) — power-cycled; vendor baselines captured; vcr-kmd (2D + DirectDraw + D3D HAL) installed, battery running

The user power-cycled `.124` after the 03:23 cfg-1 wedge. Steps 1-4 of the
list below are done:

- cfg 5 written and read back; activation clear.
- Vendor hardware-cursor golden: `vcr-kmd/golden/cursor_amigamerlin-3.1-r11_192.168.1.124.json`.
- Vendor glidelab baselines (AmigaMerlin 3.1-R11, one boot each, 60 Hz,
  `vcr-kmd/evidence/glidelab/amigamerlin-3.1-r11.jsonl`): **cfg 0 and cfg 5
  identical** - fill 1124.5 / 1121.7 Mpix/s (blend off/on) at 1024x768,
  1117.2 / 1114.4 at 1600x1200, 4 chips, **0 bad band lines** at both.
- `deploy_box.py install` of vcr-kmd (branch `worktree-vcr-kmd` 687b1c0 +
  evidence, now carrying the 2D engine, DirectDraw blt/flip, and the DX7 D3D
  HAL proven 38/38 on the 86Box Voodoo3) came back on the first boot:
  4 chips, 1280x1024x32@85, `BootAttempts 1`, `LastDecline 0`.
- **Running:** `vcr-kmd/tools/silicon_battery.py 192.168.1.124 --label
  vcrkmd-v5-1 --golden golden/amigamerlin-3.1-r11_192.168.1.124.json` ->
  `vcr-kmd/evidence/silicon/vcrkmd-v5-1.jsonl` (+ `.log`). It stops at the
  step that silences the agent; after a wedge, `vcrphases.py --prev`.

**Next:** Quake II all-ours on vcr-kmd, `glidelab_sweep.py --label vcrkmd
--cfgs 0,2,5 --no-reboot`, `glidelab_run.py ... abandon --then fill`; land the
branch if all hold; then AA one config at a time (step 5 below).

### Resume point (2026-09-26 03:40) — `.124` DEEP-WEDGED on the vendor kernel at cfg 1; needs a power cycle

**State:** AmigaMerlin 3.1-R11 is installed (rolled back from vcr-kmd to capture
vendor goldens), registry `SSTH3_SLI_AA_CONFIGURATION` = 1. `sli_golden_sweep.py`
ran Quake II (our ICD + our Glide) at **cfg 1 - single-chip 2-sample AA - and
the box deep-wedged at 03:23**: 9898 refused, 9897 accepting-and-mute, SMB 445
closed, 139 negotiates nothing - the 2026-09-24 signature. `box-guardian`
correctly did nothing; no RPC path answers. **Needs a person at the power
switch.**

**Why it matters:** AA had never engaged on this box (ground rule 2: the
registry/env route does nothing on AmigaMerlin's own Glide). OUR Glide does
pass cfg 1 through (`gpci.c`: aaSample 2, forceSingleChip) - and AmigaMerlin's
kernel wedges on the request. (2026-09-27: what reaches the kernel is not a
single-chip request but 4 chips / no SLI / AA / 2-sample, a tuple the vendor
never sends - resume point 2026-09-27.) So there is no vendor AA state to capture this
way; AA on the V5 6000 is new ground for our kernel (vcr-kmd's `vcrmp_sli.c`
has the dos_mode.c AA paths, untested on silicon).

Also found: every `sli_golden.py` capture before 03:20 ran Glide's DEFAULT
(all-chip SLI) whatever `--cfg` said - it wrote the registry only; fixed
(`88b06c6`), the cfg 5 goldens stay valid (2 == 5 inside Glide).

**After the power cycle, in this order:**
1. Write cfg 5 (not 1) before anything touches Glide; the vendor stays installed.
2. `vcr-kmd/tools/cursor_golden.py 192.168.1.124 --label amigamerlin-3.1-r11`
   (the vendor's hardware-cursor pattern: the reference for vcr-kmd's cursor).
3. Vendor glidelab baselines, one boot each: `glidelab_sweep.py --label
   amigamerlin-3.1-r11 --cfgs 0,5` (fill + bands, 1024 and 1600, 60 Hz).
4. `deploy_box.py install` (vcr-kmd from branch `worktree-vcr-kmd`: hardware
   cursor + DirectDraw HAL + the primary as a hooked device surface, all
   proven in the VM test bed), then: `mode_sweep.py --golden ...` (the GDI punt
   layer on silicon), `cursor_golden.py --label vcrkmd --compare
   amigamerlin-3.1-r11`, `ddlab_run.py 192.168.1.124 caps|flip|blt` (flip =
   vidDesktopStartAddr on the chip), Quake II all-ours (Glide with a HAL
   present), `glidelab_sweep.py --label vcrkmd --cfgs 0,2,5 --no-reboot`,
   `glidelab_run.py ... abandon --then fill`. If all hold, land the branch.
5. AA (cfg 1, 3, 6, 7, 8) on vcr-kmd ONE config at a time, ideally with someone
   near the box: `glidelab_run.py ... bands --cfg N` first (a register-level
   failure is logged by the flight recorder before the write that hangs).

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

(2026-09-27, from the source - §4.1: of the keys above only
`SSTH3_SLI_AA_CONFIGURATION` selects FSAA, and it is the only AA value the
panel writes. `SSTH3_ANTIALIAS` is D3D edge AA, `SSTH3_DIGITAL_SLI_AA` is
moot on a 4-chip board, `SSTH3_AAJITTER_FORCEFLAG` picks jitter tables.)

### 4.1 What the 3dfx Tools route writes and programs (read 2026-09-27, offline)

Read-only (critic plan step 14): the vintage W2K tree
(`retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/`, guidance only) and the
AmigaMerlin 3.1 R1 and R11 `driver2k\3dfxvs.inf` on the share
(`Files\Drivers\3DFX\WinXP\`). R6, x86-secret's release, is not on the share;
R1 and R11 carry identical AA entries. Bare file names below are in
`Displays/H5/`, except `H3.C`, `H3.H`, `SLIAA.C` and `h3registry.c`
(`Miniport/H5/`).

- **The panel writes ONE AA value.** The 3dfx tab is built from the INF's
  `[3dfxTools]` section; the Direct3D and OpenGL/Glide categories each have a
  `QuadChipAASLI` combo writing `SSTH3_SLI_AA_CONFIGURATION` (REG_SZ) into the
  `D3D` or `Glide` subkey of the adapter's software key, `Tweak Map
  "0,5,6,7,8"` = Single Chip Only / Fastest / 2 / 4 / 8 Sample. **It never
  offers cfg 1-4 on a 4-chip board.** 6000 install defaults
  (`[3dfxTools_Voodoo6]`): `D3D\SSTH3_DIGITAL_SLI_AA="0"`,
  `Glide\FX_GLIDE_ANALOG_SLI="1"`, `SSTH3_SLI_AA_CONFIGURATION="5"` in both.
  Inside Glide 3 == 6 and 4 == 7 (Glide fork `glide3/src/gpci.c:1768-1785`), so the lab's
  cfg 3 / 7 are the panel's "2 Sample" / "4 Sample"; cfg 1 has no panel
  equivalent here.
- **The display driver's D3D path** (`Compute_SLIAA_Config`,
  `DDFXNT.C:3572`, run at 3D-surface creation, `DDSURF.C:452`), on
  4 chips: rewrites cfg 1-4 to 0 (`3632-3639`) - **the vendor never sends cfg
  1's tuple** (hypothesis 1); `DDSCAPS2_HINTANTIALIASING` promotes to cfg 8;
  < 2 buffers -> 0 (`3816`); cfg 8 -> 7 with no secondary heap unless
  `SSTH3_AA_ENABLE_OUTOFMEMORY=1` (`3823-3836`); every `QUAD_*` config forces
  analog (`3868-3890`), so `SSTH3_DIGITAL_SLI_AA` is moot. `Promote_DeviceToSLIAA`
  (`2952`) then sends a **real secondary colour buffer**
  (`ddAAPrimaryStart`, `3053`) and `dwTileMark = ddTiledHeapStart` (`3038`):
  cfg 6 -> {4 chips, SLI 1, AA 1, high 0, analog 1} (the tuple our Glide sends
  for cfg 3), cfg 7 -> {4, 0, 1, 1, 1}, cfg 8 -> {4, 0, 1, 2, 1}. A failed later
  allocation runs `BailOutOfAA` (`~3350-3495`): AA dropped, SLI kept, silently -
  ground rule 2's three proofs stay mandatory.
- **Both routes end in the same miniport call; the panel programs nothing.**
  Glide: `HWCEXT_SLI_AA_REQUEST` -> `hwcSliAARequest` (`HWCEXT.C:2526`, needs
  HWC exclusive mode) -> `IOCTL_3DFX_SLI_AA_ENABLE` (`2592`). D3D: the same
  IOCTL (`DDFXNT.C:3072`). Miniport: `H3.C:4128` -> `EnableSLIAA`
  (`SLIAA.C:3481`). The routes differ only in who fills the request (real
  secondary buffer + tileMark vs our Glide's base 0, hypothesis 5), so a vendor
  D3D capture at cfg 6 / 7 is the register reference for our Glide's cfg 3 / 7.
  (That AmigaMerlin's binaries keep this structure is expected, not proven.)
- **Not FSAA enables:** `SSTH3_ANTIALIAS` (`D3INIT.C:924-930`) only lets the
  D3D render state `D3DRENDERSTATE_ANTIALIAS` through (`D3RSTATE.C:823`) - edge
  AA; AmigaMerlin labels it "Edge-Aliasing (Reboot Required)". The miniport's
  `DIGITAL_SLI_AA` (`h3registry.c:1204-1215` -> `H3.H:1876`) is
  stored and never read there (other hit: `#if 0` default, `H3.C:606`).
  `SSTH3_AAJITTER_FORCEFLAG` picks jitter tables (`D3INIT.C:1355`).
- **Where the readers look:** Glide - environment, then
  `HKCU\SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide`, then the same
  path in HKLM, REG_SZ only (Glide fork `minihwc/minihwc.c` `getRegPath`
  ~1371-1410, `hwcGetenv` ~8961-9005) - **an HKCU copy shadows HKLM**. Display driver - miniport
  query of `<DriverRegistryPath>\D3D`, then `<DriverRegistryPath>`, HKLM only
  (`h3registry.c:1400-1404`, "the search order that the 3dfx tools property
  sheet expects"), REG_SZ only (`ddgetenv`, `DDFXNT.C:1705`).
  `DriverRegistryPath` comes from `ConfigInfo` (`H3.C:839`); on XP normally
  `Control\Video\{GUID}\0000` - read it on the box.

**Capture procedure** (supervised step 5; AmigaMerlin installed, vcr-kmd
rollback kept, user at the box - the one AA request AmigaMerlin's kernel has
seen on `.124` was our malformed cfg 1, and it wedged):
1. Clean boot at cfg 5. `reg export` (via `EXEC`, then `DOWNLOAD`) the
   adapter's `Control\Video\{GUID}\0000` subtree, `HKLM` and `HKCU`
   `...\Services\3dfxvs\Device0` with subkeys, and any `HKCU\Software` key
   the 3dfx Tools applet owns (search `3dfx` case-insensitively).
2. 3dfx tab -> Direct3D -> Anti-Aliasing -> "2 Sample", Apply, export, diff.
   Expected: `D3D\SSTH3_SLI_AA_CONFIGURATION` "5" -> "6" plus the combo's own
   `Value`. Anything else is a finding.
3. Reboot, read back, run a double-buffered fullscreen D3D app, and take
   `sli_golden.py`'s capture while it runs (it drives Quake II through our Glide
   today - it needs a D3D workload option first) plus the three proofs of §4.
4. "4 Sample" (7) on its own clean boot; then the OpenGL/Glide tab.
5. Diff against the expected cfg 3/7 tables and our kernel's
   `Diag\SliAAState` for the same tuple. Roll back; commit the diffs + golden.

## 5. What "done" looks like for the second instalment

- every row in the matrix above has a number or an explicit "engine cannot"
- AA either engages (with the three proofs) or the route that fails is named
- the loader has the rows, the series has the tables, the dossier has the
  retractions (if any), FINDINGS.md has the pointer
