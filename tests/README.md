# Retro driver-stack regression suite

Regression tests that lock in the **verified fixes** across the driver stack, so
a later change that breaks one fails here instead of on the retro hardware (where
a regression means a reboot, a reflash, or a wedged Voodoo). The rule
(see repo `CLAUDE.md` → "Regression tests"): **when a fix is verified, add a test
here and note it in the fix's `outcome`, then update this README + `CLAUDE.md`.**

## Run it

```bash
bash tests/run_all.sh          # everything (Python + native C), one exit code
cd tests && pytest             # just the Python client tests
bash ../retro-3dfx/tests/run_native.sh   # just the native C driver-logic tests
```

No hardware, no Wine, no network — everything runs natively on the dev host in
under a second.

## Layout

```
retro-agent/tests/
  run_all.sh              top-level runner: Python + agent-C + driver-C, one exit code
  pytest.ini
  python/
    test_protocol.py      length-prefixed frame codec (client/retro_protocol.py)
    test_discovery.py     discovery packet parser (client/retro_discovery.py)
    test_glide_artifact_naming.py  build-stack.sh must not give h5 the glide3x.dll deploy name
    test_retro_chat_p1_behavior.py retro-chat 0.14.0: P1 CPU (single-cell spinner, >=500ms tick,
                          below-normal priority, 1s reconnect pace) + wait-for-agent startup
    test_dosgames_catalog.py       DOS Game Manager host tooling: survey classification,
                          catalog generation, .PRV preview-tile format
    test_doschat_shared.py         DOSCHAT (DOS agent+chat): shared-module invariants +
                          DOS memory limits (mTCP 64K socket malloc, DGROUP, cfg rebuild dep)
    test_agent_version.py          agent/Makefile's git-tag-derived VERSION must not be
                          older than the newest version claimed in agent/ commits
    test_dosgame_stem.py           DOSGAME install-directory stem: uniqueness across the real
                          catalog, DOS-legal 8.3 shape, and the /z/<STEM> server lookup
    test_dosgame_stability.py      DOSGAME 0.2 source invariants: bounded path_join, footer
                          buffer, split() init, CALLed .BAT, batch label reachability,
                          8K stack, keyboard drain, video-mode reset
  native/                 OUR-stack native C logic tests (see CLAUDE.md "Driver Stack Map")
    munit.h               tiny single-header C test framework
    stubs/windows.h       lets agent C compile natively (funcs use no Win32 API)
    test_crypto.c         TRUE-SOURCE: compiles agent/src/crypto.c, XOR keystream
    test_fx_pack_ub.c     MesaFX ICD 0.1.2: SSE float->ubyte color clamp (fxvbtmp.h)
    test_chatcore.c       TRUE-SOURCE: agent/shared/chatcore.c — the chat-proxy state
                          engine shared by the Windows agent and the DOS DOSCHAT build
    test_dosstage.c       TRUE-SOURCE: agent/src/dosstage.c against a fake Win32
                          (stubs/dosstage_env.h) — OS gate (never stage on NT),
                          idempotence, ordering/pacing, registry switches
    test_driver_prefs.c   TRUE-SOURCE: agent/shared/drvprefs.h — the PREFER.TXT
                          parse, the line-anchored hardware-id match, and the
                          reclaim gate (force-install BEFORE deleting C:\D)
../retro-3dfx/tests/      VINTAGE H5 / SGL harness — the .143 pure-3dfx lane, NOT our stack
  native/test_texheap_align.c, test_mip_download_addr.c ; test_source_invariants.sh ; predeploy.sh

  test_pxe_*.py, test_binl.py    PXE / unattended-image invariants. These live at
                          tests/ rather than tests/python/ because pytest.ini scopes
                          collection to python/ and these are standalone scripts that
                          must SKIP (not error) when the SMB share is not mounted.
                          run_all.sh suite [6] runs them explicitly.
                          test_pxe_devicepath.py  DevicePath decodes as ANSI, the way
                                                  REGEDIT4 is read - not UTF-16
                          test_pxe_drivers.py     the image really INSTALLS a GeForce2
                                                  GTS driver (PREFER.TXT names the
                                                  verified 71.89 build, not the first
                                                  INF that matches), and XP's own
                                                  wdma_ctl.inf is on the media for the
                                                  ISA PnP AWE64
                          test_pxe_autoplay.py    AutoPlay off (0xFF) in both hives, so a
                                                  game's own ISO mount cannot throw a
                                                  modal over a fullscreen title
                          test_pxe_firewall.py    imaged with the firewall OFF
                          test_pxe_txtsetup.py, test_pxe_bind_device.py,
                          test_pxe_boot_hold.py, test_binl.py
```

`run_all.sh` also invokes the retro-3dfx harness (vintage H5 display driver +
SGL ICD) for a whole-machine view, but **the tests we own are the ones above** —
our MesaFX ICD (`retro3dfx-gl`, 0.1.x), the agent, and the client. The vintage
0.2.x–0.3.x SGL/H5 fixes belong to the other lane.

## What kind of tests these are

The driver DLLs are cross-compiled Win32 binaries that only fully run on the
Voodoo hardware, so most tests here are **pure-logic / invariant tests**: they
encode the exact arithmetic a fix established (an alignment, a stride, an offset,
a size) and assert it, with a header comment citing the source file + function +
fix version. Each test also asserts the *old buggy* computation to document the
failure mode, so the test doubles as executable documentation of the bug.

- **Python (protocol / discovery)** — exercise the real client code directly.
  The Win98/XP agent speaks this exact wire format, so these are a true contract
  check on the framing and the discovery beacon.
- **Native C (driver logic)** — guard the invariant of a shipped fix. When the
  fixed logic lives in an in-repo, dependency-light file it is `#include`d and
  tested directly; when it lives in the external `retro3dfx-gl` fork or a
  DDK-heavy driver file, the invariant is replicated with a source citation.
- **CSIM render tests (future track)** — the native VSA-100 simulator
  (`retro-3dfx/toolchain-3dfx/csim-native/`, `libcsim.a`) can render primitives
  on Linux for pixel-accurate checks of the render fixes (garble, green-world,
  palette). It currently builds/inits but the rasterizer trigger isn't wired
  (framebuffer comes back blank), so pixel tests are pending that work.

## Fix → test coverage

Fixes in **OUR stack** (MesaFX ICD `retro3dfx-gl` 0.1.x, agent, client):

| Fix | Component | Test |
|-----|-----------|------|
| **agent 1.59.0: force the staged driver over the one XP picks, and never reclaim C:\D before doing it** (2026-08-29, .124) | agent gamesync.c + `agent/shared/drvprefs.h`, `scripts/pxe/{driver-prefs.txt,stage-oem.sh}` | `native/test_driver_prefs.c`, `test_pxe_drivers.py` |
| **image: AutoPlay off on every drive type, so an ISO mount cannot modal over a fullscreen game** (2026-08-29) | `scripts/pxe/stage-oem.sh` | `test_pxe_autoplay.py` |
| **GAMEINDEX saw only 10 of the 29 staged library titles** (2026-08-29) | agent C (gameindex.c `g_sigs[]`) | `python/test_gameindex_staged_library.py` |
| 0.1.2 SSE float→ubyte color clamp (`fx_pack_ub`) | MesaFX ICD | `native/test_fx_pack_ub.c` |
| transport XOR keystream (involution + derivation) | agent C (crypto.c) | `native/test_crypto.c` |
| discovery packet wire format | Python client | `test_discovery.py` |
| length-prefixed frame codec + status contract | Python client | `test_protocol.py` |
| DOSGAME installed-detection stem match + install receipts (2026-08-03) | DOS lane (dosgame.c) | `python/test_dosgame_install_detect.py` |
| **DOSGAME 0.2 install→play: registry records the installer's OWN target dir** (2026-08-11) | DOS lane (dosgame.c) | `python/test_dosgame_install_detect.py`, `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 collision-free install stem (1,268/2,982 rows shared a directory) | DOS lane + serve_dosgames.py | `python/test_dosgame_stem.py`, `run_dos_tests.sh` |
| 0.2 fetch line fits the measured 126-byte DOS command tail (was 845 rows over) | DOS lane (dosgame.c) | `python/test_dosgame_stem.py` |
| 0.2 crash fixes: path_join bound, draw_footer buf[81], split() NULL init | DOS lane (dosgame.c) | `python/test_dosgame_stability.py`, `run_dos_tests.sh` |
| 0.2 depth-2 scan for non-flat archives (~24% of the share) | DOS lane (dosgame.c) | `run_dos_tests.sh` |
| 0.2 real-mode hardening: 8K stack, kflush, vinit mode reset, INT 24h handler | DOS lane (dosgame.c + Makefile) | `python/test_dosgame_stability.py` |
| 0.2 diagnostic log (DOSGAME.LOG): decisions + batch steps, flushed per line | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **agent 1.26.0 Win9x REBOOT: don't kill the agent mid-shutdown-negotiation** | agent handlers.c | `python/test_agent_log_and_reboot.py` |
| agent 1.26.0 batched logging: unbuffered startup, flush on every exit path | agent log.c / main.c | `python/test_agent_log_and_reboot.py` |
| DOS net bring-up: guarded drivers, PKT.OK written, CHAT auto-calls NETUP | DOS lane (NETUP/PLAY/CHAT.BAT) | `python/test_dosgame_install_detect.py` |
| **0.2 a game you PLAYED is not where the next install went (Duke 3D)** (2026-08-13) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 DOS timestamps pack into 32 bits (`year << 26` overflowed and inverted) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 an orphaned `X` row stops hiding its unpack directory | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| MS-DOS mode gets its own DOSSTART.BAT + a SHELL= sized environment | DOS lane (DOSSTART.BAT, CONFIG.SYS.dosbox) | `python/test_dosstage_and_batch.py` |
| **0.2 fgets off-by-one: a 40-char title reported every install as a failure** (2026-08-13) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 an installer is never recorded as the launcher (post_install + F2 + gen_catalog) | DOS lane + host Python | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 an F2 choice keeps its class (new `S` registry row reloads as kind `I`) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **0.2 CRITICAL: scan de-dup hid 5 installed games behind their installers** (2026-08-13) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 write_install bounded path join (81-byte frame smash on a long gamedir=) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 generated RUN.BAT lines fit COMMAND.COM's 128-byte line buffer | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| no angle bracket in a `rem` (COMMAND.COM redirects there too - the stray `43` files) | DOS lane (*.BAT) | `python/test_dosstage_and_batch.py`, `run_dos_tests.sh` |
| agent 1.28.0 dosstage compares mtime, not size alone; DOSSTAGE force re-stages | agent dosstage.c | `python/test_dosstage_and_batch.py` |
| AGENTRUN.BAT maps its own share session (bare UNC is unreadable on Win9x) | DOS lane (AGENTRUN.BAT) | `python/test_dosstage_and_batch.py` |
| collision-free tile stem (1,268 rows shared 411 .PRV names) + catalogue staleness gate | host-side Python | `scripts/dosgames/tests/check_catalog.py` |
| **0.2 catalogue tie-break for launcher picks (ROTT.EXE not ROTTIPX.EXE)** (2026-08-13) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 Apogee ad bundle + `*HELP.EXE` excluded; unambiguous catalogue name beats first-found | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **0.2 series shell vs episode binary: `KEEN4E.EXE` beats `KEEN.EXE`** (2026-08-13) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 lone LARGE exe + no data = unextracted self-extractor, not a game (`HTIC_V10.EXE`) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 DHCP keyboard drain (`/kflush`) - a buffered key aborted mTCP's lease request | DOS lane (dosgame.c + NETUP.BAT) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **0.2 a DEICE set is entered through `INSTALL.BAT`, not `DEICE.EXE`** (2026-08-25) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 an incomplete multi-disk download is refused before its installer starts (`heretic_shareware1`) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 `NAME.1` counts as a disk-set part (only `NAME._1` did), so a stalled install is not blamed on a bad download | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **0.2 the install script judges the ARTIFACT, not ERRORLEVEL (a working LAN install logged two failures)** (2026-08-25) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 an installed game is named after the catalogue, not its folder (`KEEN1` -> `keen1 shareware`) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **0.2 that title must follow its OWN row - scan_local shuffles `games[]`, and a stale parallel key titled `C:\STARCR~1` "Doom"** (2026-08-25) | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 a disk set with parts missing is labelled INCOMPLETE by the scan, not only on Enter | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **every shipped DOS `.BAT` is CRLF (an LF-only one gives `Bad command or file name`)** (2026-08-26) | DOS lane (*.BAT + .gitattributes) | `scripts/dosgames/tests/run_dos_tests.sh` |
| MS-DOS mode leaves evidence: `DOSSTART.BAT`/`AUTOEXEC.TPL` log a marker before anything that can fail | DOS lane (*.BAT) | `scripts/dosgames/tests/run_dos_tests.sh` |
| agent 1.30.0 stages `DOSSTART.BAT`, and never stages over a box's `AUTOEXEC.BAT`/`CONFIG.SYS` | agent dosstage.c | `native/test_dosstage.c` |
| 0.2 both tabs share one column grid, one `*` marker and one green | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| fleetbook add/search/log contract (brain's solved-problems DB) | scripts/retro_fleetbook.py | `python/test_fleetbook.py` |
| agent default desktop theme = green hacker + Starfield (was gray classic) | agent retrowall.c | `python/test_retrowall_theme.py` |
| 0.1.34 fullscreen refresh snap-down (`fxBestRefresh`, was hardcoded 60Hz) | MesaFX ICD | `native/test_fx_best_refresh.c` |
| 0.1.35 fullscreen cursor overlay stamp/clip (`fxDrawCursorOverlay`) | MesaFX ICD | `native/test_fx_cursor_overlay.c` |
| refreshkeep 100Hz hold: a game that omits DM_DISPLAYFREQUENCY gets 60Hz on XP | `agent/tools/refreshlogic.h` | `native/test_refreshkeep.c` |
| game-refresh stale-cvar strip read the VALUE, not the name, in GoldSrc configs | `scripts/game-refresh/deploy_game_refresh.py` | `python/test_game_refresh_cvars.py` |
| glide2x XP bring-up: linear-base guards + prime-before-ALLOCCONTEXT (79ee51e) | open Glide (glide2x/h3 minihwc) | `native/test_glide2x_mapboard_guards.c` |
| missing v-tags made `make` stamp 1.9.2 on v1.25.1 source (2026-08-11) | agent/Makefile versioning | `python/test_agent_version.py` |
| Voodoo 2 is `VEN_121A&DEV_0002`; `VEN_1102&DEV_0002` is a Creative SB Live! (2026-08-28) | `scripts/voodoo2/install_voodoo2.py` | `python/test_voodoo2_install.py` |
| Voodoo 2 on XP: fxgpio/fxptl/Ntremap must end at `Start=1` (system), not the INF's auto (2026-08-28) | `scripts/voodoo2/install_voodoo2.py` | `python/test_voodoo2_install.py` |
| `REGWRITE` is 5 tokens (`root path name type data`); the 4-token form creates a subkey and answers OK (2026-08-28) | `scripts/voodoo2/install_voodoo2.py` | `python/test_voodoo2_install.py` |
| Voodoo 2 SLI matches on `fbiBoardID`, NOT RAM size — an 8MB+12MB pair runs as 2x8MB (2026-08-29) | `scripts/voodoo2/README.md` | `python/test_voodoo2_install.py` |
| `SUBSYS_00000000` is universal to every Voodoo 2 — the chip has no subsystem registers (2026-08-29) | `scripts/voodoo2/README.md` | `python/test_voodoo2_install.py` |
| `-mfpmath=387` alone does NOT remove SSE; `-march` must drop too (2026-08-29) | `voodoo-cleanroom/build-stack.sh` | `python/test_voodoo2_cvg_stack.py` |
| cvg relink must glob shared `swlibs/newpci/pcilib` objects or it emits NO dll (2026-08-29) | `voodoo-cleanroom/build-stack.sh` | `python/test_voodoo2_cvg_stack.py` |
| `GL_SGIS_multitexture` stays opt-in — advertising it hangs the Q2 timedemo on Voodoo 2 (2026-08-29) | `voodoo-cleanroom/patches/mesafx-sgis-multitexture.patch` | `python/test_voodoo2_cvg_stack.py` |
| Unreal's `WindowedRenderDevice` must be GlideDrv — a Voodoo 2 cannot render windowed, so `SoftDrv` strands the session on the software rasterizer (2026-08-30) | `scripts/voodoo2/fix_glide_games.py` | `python/test_voodoo2_unreal_glide.py` |
| A Voodoo 2 is 16bpp-only and a single 4MB-FBI card cannot exceed 640x480 with 3 colour buffers — the stock ini asked 1024x768x32 (2026-08-30) | `scripts/voodoo2/fix_glide_games.py` | `python/test_voodoo2_unreal_glide.py` |
| A game-local `glide2x.dll` shadows the real driver; tell nGlide (1,310,720) from real 3dfx Glide (226,304) BY SIZE (2026-08-30) | `scripts/voodoo2/fix_glide_games.py` | `python/test_voodoo2_unreal_glide.py` |
| stopping the Themes service strips **Aero** on Vista+ — XP-only, or every agent restart un-fixes Win7 (.246, 2026-08-29) | `agent/src/retrowall.c` | `python/test_retrowall_theme.py` |
| desktop icons landed 103 px apart in an 80 px bay - `LVS_EX_SNAPTOGRID` is a SECOND setting the arranger never cleared (2026-08-29) | `agent/src/gamesync.c` | `python/test_icon_arrange_grid.py` |
| an unactivated Windows blanks the desktop hourly - the fleet wallpaper must be KEPT, not just applied (.246, 2026-08-29) | `agent/src/retrowall.c` | `python/test_retrowall_theme.py` |
| `retro_agent.exe`/`retro_chat.exe` had NO resource directory, so their own desktop shortcuts were generic (2026-08-29) | `agent/Makefile`, `agent/res/` | `python/test_agent_resources.py` |
| 65 shortcuts packed DOWNWARD into a 4x8 bay put 29 icons below the bottom of a 1024x768 screen, unreachable (.143, 2026-08-29) | `agent/src/gamesync.c`, `scripts/retro-wallpaper/arrange_icons.c` | `native/test_icon_arrange_overflow.c`, `python/test_icon_overflow_source.py` |
| `arrange_icons.exe` still parked icons BOTTOM-RIGHT and `retrowall.c` runs it on every agent start, undoing the bay (2026-08-29) | `scripts/retro-wallpaper/arrange_icons.c` | `python/test_icon_overflow_source.py` |
| three staged titles drew their desktop icon from the WRONG file — `hl.exe` on Counter-Strike, and two installer stubs that each carry a real but generic icon resource, so no structural check catches it (.143/.145, 2026-08-30) | `\\192.168.1.122\files\Files\Games-Library\<Title>\launch.txt` | `python/test_launch_icon_targets.py` |

| DOSBox `fullresolution=original` changes the WHOLE DESKTOP to 640x480 and hands a 4:3 signal to a 16:9 panel — and `desktop` is wrong on a CRT, so neither can be a staged constant (.145, 2026-08-30) | `provisioning/fleetres/fleetres.c`, `scripts/fleet/stage-fleetres.py`, `scripts/validate-staged-library.py` | `python/test_fleetres_staging.py` |
| id Tech 3 `r_mode`/`r_customwidth`/`r_fullscreen` are `CVAR_LATCH`, so a staged `seta r_mode "6"` BEATS the launcher's `+set` and pinned all eight monitors to 1024x768 (.123, 2026-08-30) | staged `<Title>\<mod>\autoexec.cfg` + `Play <Game>.bat`, generated by `scripts/fleet/stage-fleetres.py` | `python/test_fleetres_staging.py` |

(The vintage SGL/H5 fixes — garble 0.3.1, mip-download 08fd889 — are tested in
`retro-3dfx/tests/`, the other lane's harness, not here.)

**Backlog for OUR stack** (from `voodoo-cleanroom/CHANGELOG.md`, 0.1.x): swap-interval
env default (0.1.6), LOD-bias default (0.1.11), alpha-PFD matcher +
paletted-default-off (0.1.30), vertex cache (0.1.3), Q2 glide3x-binding (0.1.19);
agent-C — `handle_execw` timeout clamp, `discovery_build_packet` ⇄ Python
`from_packet` round-trip, `util.c` json/hex helpers; render (CSIM track) — filters,
green-world; provisioning — P3 no-SSE2 opcode scan of staged DLLs.

## Adding a test when a fix is verified

1. Identify the invariant the fix establishes (the exact value/relationship that
   was wrong before and is right after).
2. Add `native/test_<fix>.c` (cite source file:function + fix version in the
   header comment) or a `python/test_*.py` case. Assert both the fixed value AND
   the old buggy value (documents the failure mode).
3. `bash tests/run_all.sh` must stay green.
4. Add a row to the table above and, if the fix is a milestone, a line in
   `CLAUDE.md`.
