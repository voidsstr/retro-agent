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
    test_dosnative.py              The DOS-native lane: DOSGAME.TXT declares a staged
                          title's real-DOS launcher (the 8.3 guess picks a Win32 PE out of a
                          staged tree); a wrapper's cost belongs on its shortcut, not the
                          title; DXX-Rebirth's CMOV floor lives in a load-time import
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
    test_verdict_coverage.c  TRUE-SOURCE: agent/shared/gamegate.h - the guard
                          that makes a SHRUNKEN verdict file visible.
                          gg_verdict_count() (rows present) vs
                          gg_verdict_declared() (the "# titles=N" the writer
                          claimed). Pins the real 2026-08-30 clobber - one row
                          declaring 38 - and the subtle case: a headerless file
                          returns 0, which means "did not say" and must NEVER
                          read as "covers nothing".
    test_profile_hash_pin.c  TRUE-SOURCE: agent/shared/gamegate.h - the gate's
                          cache key PINNED to the eight hashes the fleet's own
                          agents published on 2026-08-30. test_gamegate.c
                          asserts the RELATIVE properties (same box stable,
                          different boxes differ); a change that moves EVERY
                          hash uniformly passes all of those and is caught only
                          here. The hash IS the verdict filename, so drifting
                          it makes all eight boxes lose their LLM verdicts at
                          once, silently.
    test_hwpublish.c      TRUE-SOURCE: agent/shared/hwpub.h — the fleet-inventory
                          publish. The hostname->filename mapping (a NetBIOS name
                          in a path is not a filename: '\' or ".." writes
                          SOMEWHERE ELSE on the share, silently), the BOUNDED
                          retry schedule (an unbounded one against an absent
                          server is what killed the agent on the 31MB Deskpro),
                          and the MAC formatter (offset k*3-1, not k*3 — at k*3
                          the whole address truncates to "00")
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
| **`install.reg` never reached a 32-bit game on 64-bit Windows** — 25 of 30 staged `install.reg` files seed `HKLM\SOFTWARE` and **none** mention `Wow6432Node`; `regedit.exe` is 64-bit so it writes the 64-bit view while the game reads the 32-bit one. Measured on Win11: Halo sits on its EULA imported the default way, reaches the menu with `/reg:32`. The same switch is **fatal on XP**, which has no `/reg:32` — the Rainbow Six `regs.cmd` bug in reverse — so the helper branches per machine (2026-09-01) | `provisioning/win64/apply-install-reg.cmd` | `python/test_win64_install_reg.py` |
| **the disc-mount launcher refused to run on an OS that mounts ISOs itself** — Windows 8+ ships `Mount-DiskImage`, but the template knew only DAEMON Tools and WinCDEmu and answered "NO DISC MOUNTER IS INSTALLED" on Windows 11, where 8 of the 11 disc-backed titles are `.iso`. Native mount is tried **last**, because DT/WinCDEmu can emulate copy protection and `Mount-DiskImage` cannot, and only for `.iso` — `.cue`/`.bin` still report honestly (2026-09-01) | `provisioning/discmount/mount-launcher-template.bat` (`:findnative`) | `python/test_mount_launcher_template.py` |
| **a title staged but not yet deployed was invisible in the doc that exists to list it** — `gen-staged-library.py` built its title list from `compat_deploy` ∪ `compat_render`, the two *fact* tables, so Rainbow Six (staged while the whole fleet was powered down) appeared nowhere and read as "not staged". `compat_title` joins the union; such a title now renders as a full row of `..` and the legend explains the marker (2026-09-01) | `scripts/fleet/gen-staged-library.py` | `python/test_staged_library_shows_undeployed.py` |
| **Rainbow Six would have deployed as a broken install on every XP box** — the GOG build's `regs.cmd` ends all 40 `REG ADD` lines with `/reg:32`, a switch **XP's reg.exe does not have** (Vista added it), so every line fails silently. 33 of the 34 asset-path defaults compiled into `RainbowSix.exe` point at `\data2\` (the CD), so with no registry values the game finds essentially nothing. Replaced by a generated `install.reg` applied with `regedit /s`. The CD check itself is *intact* in the binary — that is the proof the media is not cracked (2026-09-01) | `provisioning/rainbowsix/install.reg` | `python/test_rainbowsix_install_reg.py` |
| **Blue Shift has never run on any box, and the cause was the LIBRARY, not the machines** — it was staged as `hl.exe -game bshift`, grafting its April-2001 game DLLs onto Half-Life GOTY's Sep-2001 engine, which rejects them ("Game DLL version mismatch" → `Host_Error`). It is a STANDALONE product with its own engine; installed from retail in the build VM 2026-09-01, 143 files / 264,492,993 bytes, and `bshift.exe` proven NOT to be the Razor 1911 no-CD crack that sits beside the ISO (same byte size, different md5). Its CD check is still unsatisfied — the volume label and the CD key were both tested and refuted (2026-09-01) | `provisioning/discmount/specs/HalfLife-BlueShift.json` | `python/test_blueshift_spec.py` |
| **a title-level LAN fact was never WITHDRAWN when the title stopped being deployed** — `ingest_lan_doc` refuses to write "needs a person once" onto a box that does not have the game, but the guard only stopped a row being *created*; `.171` still carried Halo's old row long after the gate refused Halo there for lack of hardware T&L. Re-ingesting withdrew 16 such rows. The first draft of the fix said `partner_ip IS NULL`, which matches nothing (`NOT NULL DEFAULT ''`) and would have reported success while deleting none (2026-09-01) | `scripts/fleet/compat.py` (`ingest_lan_doc`) | `python/test_lan_doc_withdraws_stale_rows.py` |
| **Halo 2 was written off as DRM-locked when nobody had ever run the game binary** — `startup.exe` is the disc AUTORUN/INSTALLER, not the launcher, so "it never creates `halo2.exe`, therefore the licence check blocks it" was reasoning about the installer; `halo2.exe` started directly reaches its main menu on `.246`, fullscreen, with no crack and no key ever requested. The staged tree also shipped the disc's **XP-only** 6 KB `dwmapi.dll` stub, which SHADOWS Vista/7's real 67 KB one and killed the process at load with `0xC0000139` before `main()` — a silent instant exit that reads exactly like DRM. It now ships as `dwmapi.dll.xpshim`, placed per box by the launcher (2026-09-01) | `Games-Library/Halo2/Play Halo 2.bat`, `dwmapi.dll.xpshim` | `python/test_halo2_shim_and_launcher.py` |
| **a shared Halo CD key is only a FAULT between boxes that HAVE Halo** — Halo allows one simultaneous player per key, so `audit_keys.py` reports duplicates; its first version flagged `.133`/`.143`, neither of which has `halo.exe` (the gate refuses the title there — no SSE2), which made stale registry state read as a licensing fault (2026-09-01) | `scripts/halo/audit_keys.py` (`classify_duplicates`) | `python/test_halo_key_audit.py` |
| **the secret scanner reported a test fixture as a leaked key** — a key-shaped fixture cannot be written as `XXXXX`, because the encoder only accepts the base-24 alphabet, so `PLACEHOLDER` could never cover one; the narrow exemption is "a consecutive walk of that alphabet", which a real key has ~1 in 24²⁴ chance of matching. Found only after the file was COMMITTED — `git ls-files` cannot see an untracked file, so the suite was green while the leak-shape was still `??` (2026-09-01) | `tests/python/test_no_committed_secrets.py` (`_is_alphabet_walk`) | `python/test_no_committed_secrets.py::test_the_alphabet_walk_exemption_cannot_launder_a_real_key` |
| **tests/README.md had drifted into naming barely half the suite** — 53 Python and 11 native test files had no row anywhere, so the fix→test table read as complete while it was not (2026-09-01) | `tests/README.md` | `python/test_readme_lists_every_test.py` |
| **a verdict file that shrank must SAY so** — `publish --title` rendered only the named title and wrote it over the whole per-box file, leaving seven of eight boxes a one-row file that was perfectly well formed and reported by nothing; found by counting rows by hand, which is not a mechanism (2026-08-30) | `scripts/gamegate/rules.py` (`# titles=N`) + `agent/shared/gamegate.h` | `native/test_verdict_coverage.c` |
| **the gate's cache key is pinned to the fleet's real published hashes** — `test_gamegate.c` only asserts relative stability, so a uniform drift (a field added to the fold, a bucket resized) passes it while every box silently loses the verdict file named by its hash (2026-08-30) | `agent/shared/gamegate.h` | `native/test_profile_hash_pin.c` |
| **agent 1.77.1: the record was landing stamped with the RETRO BOX's clock** — `CopyFile` propagates the source timestamp, handing the staleness test the one clock it was built not to trust (2026-08-30) | agent `hwpublish.c` | `native/test_hwpublish.c`, `python/test_fleet_inventory.py` |
| **agent 1.74.1: a graphics card reported as `"A"`** — `DriverDesc` is a REG_BINARY holding UTF-16 and `RegQueryValueExA` hands REG_BINARY back raw (2026-08-30, .246) | agent `hwextra.c` + `hwprofile.c` `reg_str()` | `native/test_hwpublish.c` |
| **agent 1.74.0: every box publishes its own hardware record on every startup, so the fleet documentation is measured rather than remembered** — the hand-maintained table was wrong about most of the fleet and TWICE missed a graphics card being swapped (2026-08-30) | agent `hwpublish.c` / `hwextra.c` / `hwprofile_json()` + `agent/shared/hwpub.h`, `scripts/fleet/inventory.py` | `native/test_hwpublish.c`, `python/test_fleet_inventory.py` |
| **the generated inventory must tell `current` / `stale` / `never seen` / `unreadable` apart, and a torn record must degrade rather than crash** — "not installed" and "crashed" must never render the same, and a fleet powered on demand always has boxes reporting old data (2026-08-30) | `scripts/fleet/inventory.py` | `python/test_fleet_inventory.py` |
| **agent 1.59.0: force the staged driver over the one XP picks, and never reclaim C:\D before doing it** (2026-08-29, .124) | agent gamesync.c + `agent/shared/drvprefs.h`, `scripts/pxe/{driver-prefs.txt,stage-oem.sh}` | `native/test_driver_prefs.c`, `test_pxe_drivers.py` |
| **image: AutoPlay off on every drive type, so an ISO mount cannot modal over a fullscreen game** (2026-08-29) | `scripts/pxe/stage-oem.sh` | `test_pxe_autoplay.py` |
| **GAMEINDEX saw only 10 of the 29 staged library titles** (2026-08-29) | agent C (gameindex.c `g_sigs[]`) | `python/test_gameindex_staged_library.py` |
| **favourites agent: decide by what is ON THE BOX, not by our recorded intent** — an external rewrite (GAMESYNC restaging `UnrealTournament.ini` on .171) was reported `unchanged` forever (2026-08-30) | `scripts/gameindex/sync.py` | `python/test_gameindex_favorites.py` |
| **favourites slot numbering, verified in each game's own browser on .133/.143: Q2 `adr0..adr8` (0-based), Q3 `server1..server16` (1-based), GoldSrc's four VDF keys** (2026-08-30) | `scripts/gameindex/favorites.py` | `python/test_gameindex_favorites.py` |
| 0.1.2 SSE float→ubyte color clamp (`fx_pack_ub`) | MesaFX ICD | `native/test_fx_pack_ub.c` |
| transport XOR keystream (involution + derivation) | agent C (crypto.c) | `native/test_crypto.c` |
| discovery packet wire format | Python client | `test_discovery.py` |
| length-prefixed frame codec + status contract | Python client | `test_protocol.py` |
| **A staged tree's DOS build is DECLARED, not guessed — `DOSGAME.TXT`** (2026-08-30). The 8.3 guess picks `GLQUAKE.EXE` (a Win32 PE) for Quake and `DESCENT1.BAT` (a cmd.exe batch) for Descent, both unstartable in real DOS | DOS lane (dosgame.c) + staged library | `python/test_dosnative.py`, `scripts/dosgames/tests/test_pick_outcomes.sh` |
| **A wrapper's cost was stated as the title's floor**, so the Pentium 1 was refused every DOS title it can run natively; and a title-level `requires_capabilities` suppressed shortcuts that do not need it (Descent II had no icon on `.123`/`.246`) (2026-08-30) | staged library `requires.json` | `python/test_dosnative.py` |
| **DXX-Rebirth's CMOV floor is in a load-time IMPORT** (`SDL.dll` 286, `SDL_mixer.dll` 117), not in its own 0-CMOV exe; its MMX *is* cpuid-dispatched and must not be declared (2026-08-30) | staged library `requires.json` | `python/test_dosnative.py` |
| DOSGAME installed-detection stem match + install receipts (2026-08-03) | DOS lane (dosgame.c) | `python/test_dosgame_install_detect.py` |
| **DOSGAME 0.2 install→play: registry records the installer's OWN target dir** (2026-08-11) | DOS lane (dosgame.c) | `python/test_dosgame_install_detect.py`, `scripts/dosgames/tests/run_dos_tests.sh` |
| 0.2 collision-free install stem (1,268/2,982 rows shared a directory) | DOS lane + serve_dosgames.py | `python/test_dosgame_stem.py`, `run_dos_tests.sh` |
| 0.2 fetch line fits the measured 126-byte DOS command tail (was 845 rows over) | DOS lane (dosgame.c) | `python/test_dosgame_stem.py` |
| 0.2 crash fixes: path_join bound, draw_footer buf[81], split() NULL init | DOS lane (dosgame.c) | `python/test_dosgame_stability.py`, `run_dos_tests.sh` |
| 0.2 depth-2 scan for non-flat archives (~24% of the share) | DOS lane (dosgame.c) | `run_dos_tests.sh` |
| 0.2 real-mode hardening: 8K stack, kflush, vinit mode reset, INT 24h handler | DOS lane (dosgame.c + Makefile) | `python/test_dosgame_stability.py` |
| 0.2 diagnostic log (DOSGAME.LOG): decisions + batch steps, flushed per line | DOS lane (dosgame.c) | `scripts/dosgames/tests/run_dos_tests.sh` |
| **agent 1.26.0 Win9x REBOOT: don't kill the agent mid-shutdown-negotiation** | agent handlers.c | `python/test_agent_log_and_reboot.py` |
| **agent 1.78.1 Win9x LOAD: the exe must import nothing 9x lacks — 7 NT-only statics (SCM family + `CM_Get_DevNode_Status`) made every build since ~1.31 fail to LOAD on Win98SE, no log line, `main()` never reached** (2026-08-30) | agent ntdyn.c / retrowall.c / gamesync.c / video.c | `python/test_agent_win9x_imports.py` |
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
| desktop icons could always be left scattered, because the agent only tidied them AFTER the fact — replaced by Windows' own Auto Arrange, which the shell re-applies itself on every resolution change, game exit and Explorer restart (fleet-wide, 2026-08-30) | `agent/src/gamesync.c`, `agent/src/retrowall.c` | `native/test_icon_autoarrange.c`, `python/test_icon_autoarrange_source.py` |
| `FCIDM_SHVIEW_AUTOARRANGE` is a TOGGLE, and posting it blindly flips the setting the WRONG way on a box already in the target state — while logging success. The shell toggle was measured silently failing on BOTH `.171` and `.143`, so the `SetWindowLong` fallback and the read-back are load-bearing on a quarter of the fleet (2026-08-30) | `agent/src/gamesync.c:gs_apply_autoarrange` | `native/test_icon_autoarrange.c`, `python/test_icon_autoarrange_source.py` |
| the persisted `Bags\1\Desktop\FFlags` word is NOT uniform across the fleet — `.143` read `0x220`, `.171` read `0x224` — so stamping a constant would have silently changed align-to-grid on some boxes and not others; only bit 0 may move (2026-08-30) | `agent/src/gamesync.c:gs_bag_autoarrange` | `native/test_icon_autoarrange.c` |
| the icon-layout call sited BELOW `retrowall_apply_startup()`'s early returns would run on almost no box, log nothing, and look installed — both returns are the normal path on a fleet machine (2026-08-30) | `agent/src/retrowall.c` | `python/test_icon_autoarrange_source.py`, `python/test_no_conflicting_arranger.py` |
| the desktop icon layout was rebuilt at the end of EVERY gamesync, including the every-boot case where nothing was copied and no shortcut was created - so every machine rebuilt its icons on every boot (user-reported, 2026-08-30) | `agent/src/gamesync.c:gs_run` | `native/test_icon_rebuild_gate.c`, `python/test_icon_autoarrange_source.py` |
| `gs_run()` SWEEPS every .lnk off the desktop before writing any, so "was this shortcut already there?" is always false - every shortcut counted as new and the icon-rebuild gate was true on every box on every sync while reporting itself as working; the icon SET is now sampled before the sweep and compared at the end (found on .171 minutes after the counters shipped, 2026-08-30) | `agent/src/gamesync.c:gs_desk_snapshot` | `native/test_icon_rebuild_gate.c`, `python/test_icon_autoarrange_source.py` |
| the icon-rebuild gate decided silently, so a gate permanently stuck at "changed" - one file whose mtime never stamps re-copies every pass - would restore the every-boot rebuild with NOTHING saying so; `done:` and `GAMESYNC STATUS` now report `files_written`/`shortcuts_changed` (2026-08-30) | `agent/src/gamesync.c:gs_run` | `python/test_icon_autoarrange_source.py` |
| the two obvious change-counters are both true on EVERY run and measure nothing: `gs_copy_file` returns success for a SKIPPED file, and `gs_make_game_shortcut` rewrites a title's `.lnk` every pass (2026-08-30) | `agent/src/gamesync.c` | `native/test_icon_rebuild_gate.c` |
| `LVM_ARRANGE` was posted on every agent startup even when auto-arrange was already ON - pure churn, since the shell already maintains the layout (2026-08-30) | `agent/src/gamesync.c:gs_apply_autoarrange` | `native/test_icon_rebuild_gate.c`, `python/test_icon_autoarrange_source.py` |
| three staged titles drew their desktop icon from the WRONG file — `hl.exe` on Counter-Strike, and two installer stubs that each carry a real but generic icon resource, so no structural check catches it (.143/.145, 2026-08-30) | `\\192.168.1.122\files\Files\Games-Library\<Title>\launch.txt` | `python/test_launch_icon_targets.py` |

| DOSBox `fullresolution=original` changes the WHOLE DESKTOP to 640x480 and hands a 4:3 signal to a 16:9 panel — and `desktop` is wrong on a CRT, so neither can be a staged constant (.145, 2026-08-30) | `provisioning/fleetres/fleetres.c`, `scripts/fleet/stage-fleetres.py`, `scripts/validate-staged-library.py` | `python/test_fleetres_staging.py` |
| id Tech 3 `r_mode`/`r_customwidth`/`r_fullscreen` are `CVAR_LATCH`, so a staged `seta r_mode "6"` BEATS the launcher's `+set` and pinned all eight monitors to 1024x768 (.123, 2026-08-30) | staged `<Title>\<mod>\autoexec.cfg` + `Play <Game>.bat`, generated by `scripts/fleet/stage-fleetres.py` | `python/test_fleetres_staging.py` |
| `r_mode -1` is NOT universal in the id Tech 3 family — `quake3.exe`/`jasp.exe`/`jamp.exe` take it and `sof2mp.exe` silently renders 640x480, though all four carry the `r_customwidth` string (.145, 2026-08-30) | `scripts/fleet/stage-fleetres.py::idtech3_modecfg` | `python/test_fleetres_staging.py::test_sof2_uses_a_mode_index_and_the_right_table` |
| id Tech 2 and id Tech 3 mode tables diverge at index 8 — 1280x960 (4:3) vs 1280x1024 (5:4) — so `FR_Q2MODE` on a Quake III fork asks a 16:9 panel for a squashed picture (.123/.145, 2026-08-30) | `provisioning/fleetres/fleetres.c::q3_mode_for` | `python/test_fleetres_staging.py::test_q3_table_exists_and_skips_the_five_four_mode` |
| a game's own mode menu is not evidence of its ceiling — Tiberian Sun lists up to 800x600 and renders 1920x1080, because CnCNet reads SUN.INI directly (.123/.145, 2026-08-30) | `scripts/fleet/stage-fleetres.py` TiberianSun recipe | `python/test_fleetres_staging.py::test_only_the_measured_ceilings_carry_a_cap` |
| GLQuake really does refuse 1920x1080 AND 1600x1200 and tops out at 1280x960, while Hexen II's `glh2.exe` — same family — takes 1920x1080 on the same box (.145, 2026-08-30) | `scripts/fleet/stage-fleetres.py` Quake1/HexenII recipes | `python/test_fleetres_staging.py::test_glquake_cap_is_the_measured_ceiling_not_the_old_guess` |
| a game-local nGlide `glide2x.dll` SHADOWS the real system32 Glide on the two boxes that have 3dfx silicon, so UE1 falls to the software rasterizer at 100% CPU and it looks like a crash (.171, 2026-08-30) | `scripts/fleet/stage-fleetres.py::glide_swap`, `provisioning/fleetres/fleetres.c::glide_probe` | `python/test_fleetres_staging.py::test_glide_swap_renames_in_both_directions` |
| `%%` outside a FOR loop is a silent no-op — cmd.exe compares the literal text `%FR_GLIDE%`, so the block reads correctly and never runs (2026-08-30) | `scripts/validate-staged-library.py` `fleetres-percent` | `python/test_fleetres_staging.py::test_validator_rejects_a_doubled_percent_in_a_launcher` |
| a staged binary with PE `SubsystemVersion >= 6.0` is refused by XP's loader before it runs, and an impossible `TimeDateStamp` is a scene watermark — but a *printable-ASCII* stamp is NOT evidence on its own (Halo's genuine 1.0.10 build stamps `RhrS`) (2026-08-30) | `scripts/fleet/pe-audit.py` | `python/test_pe_audit.py` |

| a host duty **running but not `enabled`** is invisible until the reboot that loses it — `is-active` says `active` and it silently never comes back (2026-08-30) | `scripts/fleet/host-duties.py` | `python/test_host_duties.py` |
| without **linger**, NO `systemctl --user` unit starts until somebody logs in — every unit still reads `enabled` while the fleet bridge, the brain and all nine game servers stay dead (2026-08-30) | `scripts/fleet/host-duties.py` | `python/test_host_duties.py` |
| "never installed here" (`claude-csbot`, `mohaa-server`) must NOT render as an outage, or the board shows a permanent red light and everyone learns to ignore it (2026-08-30) — and the excuse must be REMOVED the day the thing is installed: `rtcw-server` came off that list on 2026-09-01 and a missing one is now a fault | `scripts/fleet/host-duties.py` | `python/test_host_duties.py::test_rtcw_is_no_longer_excused_as_never_installed` |
| Tribes 2 is a **docker container**, so anything enumerating the game servers through systemd alone gets `not-found` and drops a running server off the board (2026-08-30) | `scripts/fleet/host-duties.py` | `python/test_host_duties.py` |
| a refused TCP connect is a full **listen backlog** under fleet contention, not a dead agent — it returns INSTANTLY, so two back-to-back retries both hit the same backlog and called a healthy .124 unreachable (2026-08-30) | `scripts/fleet/box-owner.py` | `python/test_box_owner.py` |
| a 39 MB ImageMagick `System\magick.exe` sat unreferenced in the UnrealTournament tree at PE `MajorSubsystemVersion` **6.0** — Vista-only, so XP's loader refuses it before a single instruction runs, with no dialog and nothing in any log (2026-08-30) | `\\192.168.1.122\files\Files\Games-Library\<Title>\**\*.exe|*.dll`, `Play Unreal Tournament.bat` | `scripts/validate-staged-library.py` check `pe-subsystem` (suite [6]) |
| `KEY=$(az keyvault secret show ... -o tsv)` **swallows a not-logged-in error into an empty variable**, so a blank product key reaches `winnt.sif` and fails hours later at a dialog nobody is watching (2026-08-30) | `scripts/fleet/keyvault.py` | `python/test_keyvault.py` |
| "not logged in", "no such secret" and "access denied" have three different fixes and must never render the same — and a secret must never be passed on the command line, where argv reaches shell history and `ps` (2026-08-30) | `scripts/fleet/keyvault.py` | `python/test_keyvault.py` |
| a Daemon Tools `.mdf` is **2448-byte sectors**, so ISO sector 16 is at byte 39184 and the `dd` everybody tries first (32768) reads zeros — four Generals discs looked empty or encrypted and were neither (2026-08-30) | `scripts/fleet/mdf2iso.py` | `python/test_mdf2iso.py` |
| an unrecognisable image must fail loudly rather than default to 2048 — a guessed geometry writes an ISO that opens nowhere and the symptom then points at the archive tool (2026-08-30) | `scripts/fleet/mdf2iso.py::detect` | `python/test_mdf2iso.py::test_no_volume_descriptor_raises_rather_than_guessing` |
| a CD key or product key pasted into a tracked file is permanent — rewriting published history is a negotiation, so the cheap moment to catch it is before the commit (2026-08-30) | the whole tracked tree | `python/test_no_committed_secrets.py` |

| the library validator walks ~40 GB over SMB and several agents ran it at once — **15 processes, 9 stuck in uninterruptible IO, the oldest 19 minutes**, none able to finish; three agents read their own stall as a test failure and one nearly reported a PASS that had been SIGTERMed (wrapper exit 0, validator exit 143). It now takes an advisory lock, `--no-wait` returns **75** (distinct from 1 = problems found), and a waiter names the gvfs second transport, which completes when the CIFS mount cannot (2026-08-30) | `scripts/validate-staged-library.py` | `python/test_validator_serialises.py` |

| a UE1 dedicated-server launcher that does not delete `System\Running.ini` **before** it starts the engine opens a "Recovery Mode" dialog instead of the server on the next run — UE1 only removes that file on a CLEAN exit, every automated test taskkills the game, and GAMESYNC cannot clear it because `gs_copy_file` never deletes. From a process list the server looks up; it is sitting on a modal (2026-08-31) | `Games-Library/DeusEx/Host Deus Ex Multiplayer.bat`, `Games-Library/UnrealGold/Host Unreal Gold LAN.bat` | `python/test_ue1_lan_host_launchers.py` |

| the agent's two remaining CMOVs sit in mingw runtime code reached from `___tmainCRTStartup` **before `main`** — they are unreachable only because the linker resolves the pseudo-reloc list bounds to the SAME address, so a CMOV *count* stays at 2 while a new import makes them live and faults a genuine Pentium 1 with `0xC000001D` before any log line exists (2026-08-30) | `agent/Makefile` (`-march=i586`), the link | `python/test_agent_is_pentium1_safe.py` |

| CLAUDE.md made "check activation before you reboot" REQUIRED and **nothing enforced it** — `safe-reboot.py` guarded the PXE re-image risk and never asked the question that actually strands a box. Measured with six agents live: `.123` and `.133` were both running `wpabaln.exe`, so two of seven would not have survived a reboot (2026-08-31) | `scripts/fleet/safe-reboot.py` | `python/test_safe_reboot_activation.py` |

| an id Tech mode index named a mode the driver does not offer — `FR_Q3MODE`=7=1152x864 on `.246`, whose adapter lists 1152x**648** and no 1152x864; the engine neither errored nor obeyed, setting the desktop to 1280x960 and drawing into a **window** with `r_fullscreen` still 1. `SoldierOfFortune2`/`JediAcademy` consume that value on all three 1080p boxes (2026-08-31) | `provisioning/fleetres/fleetres.c` `q2_mode_for`/`q3_mode_for` | `python/test_fleetres_mode_offered.py` |

(The vintage SGL/H5 fixes — garble 0.3.1, mip-download 08fd889 — are tested in
`retro-3dfx/tests/`, the other lane's harness, not here.)

**Backlog for OUR stack** (from `voodoo-cleanroom/CHANGELOG.md`, 0.1.x): swap-interval
env default (0.1.6), LOD-bias default (0.1.11), alpha-PFD matcher +
paletted-default-off (0.1.30), vertex cache (0.1.3), Q2 glide3x-binding (0.1.19);
agent-C — `handle_execw` timeout clamp, `discovery_build_packet` ⇄ Python
`from_packet` round-trip, `util.c` json/hex helpers; render (CSIM track) — filters,
green-world; provisioning — P3 no-SSE2 opcode scan of staged DLLs.

### GAMESYNC library enumeration (box `.243`, 2026-08-31)

| fix | code | test |
|---|---|---|
| `gs_dir_size()` was called from INSIDE the library `FindFirstFile` loop, holding the SMB search handle open across minutes of recursive walking — on Win9x the redirector drops that context and `FindNextFileA` silently truncates the library. `.243` (Win98SE, P1) enumerated **25 of 46** titles and reported `state=done, titles_total: 25` with no error anywhere; 21 titles, one of them gate-approved for that box, were never considered (2026-08-31) | `agent/src/gamesync.c:gs_run` | `python/test_gamesync_enumeration.py` |
| both ways the listing can end early were silent — a `FindNextFile` failure and the (unnamed, bare `64`) titles[] cap. Now `GS_MAX_TITLES`, and both log; the published verdict file carrying MORE rows than were enumerated is also flagged, which reads "covers 46 of 25" on the box that had the bug (2026-08-31) | `agent/src/gamesync.c:gs_run` | `python/test_gamesync_enumeration.py` |
| a gate refusal limited by `disk` was FINAL and counted as `titles_gated` — "this machine cannot run it" — though it compares a DECLARED `disk_mb` against a stale `free_mb` and gives no credit for an install already on the volume. It now defers to GAMESYNC's own room check (real size, current free space, existing tree credited), which counts it as `titles_skipped`. 13 of `.243`'s 22 "gated" titles were merely too big for a 604 MB volume; `.240` read `deploy=gated, runs=verified` for a FarCry installed on that very disk, so a large title could never be patched once the disk filled (2026-08-31) | `agent/src/gamesync.c:gs_gate_limited_by_disk`, `gs_run` | `python/test_gamesync_enumeration.py::test_a_disk_refusal_defers_to_the_real_room_check` |
| **the SAME disk defer had a SECOND call site and only the first got it** — `gs_gate_allows_shortcut()` still treated a `disk` verdict as final, and it runs AFTER the tree is on the volume and after `gs_file_exists(target)`. On `.240` a Far Cry occupying 3609 MB of that box's own 76 GB volume left 1492 MB free against its declared `disk_mb` of 3700, so the tree copied and then the icon was taken off a game recorded `runs=verified` — while `titles_gated` read **0** and the summary line said nothing was gated (agent 1.79.4, 2026-08-31) | `agent/src/gamesync.c:gs_gate_allows_shortcut` | `python/test_gamesync_enumeration.py::test_a_disk_refusal_does_not_suppress_a_SHORTCUT_either` |
| **the AGP Radeon X800 was not in the GPU table, and the X800 family is SM2 not SM3** — only the PCIe `0x5D48-0x5D6F` R4xx row existed, so `.240`'s `1002:4A4B` (fitted 2026-08-31) reported `feature_level: unknown` and the gate was blind to that machine's GPU; the row also claimed SM3, which no R4xx has (agent 1.79.4) | `agent/shared/gamegate.h`, `scripts/gamegate/rules.py` | `native/test_gamegate.c::gpu_table_handles_non_monotonic_ids`, `python/test_gamegate_mirror.py` |
| `gs_sweep_desktop()` takes EVERY icon off the desktop and only the copy branch puts any back, so an installed, playable title that this run gated or skipped **lost its shortcuts permanently**. On `.243` the engine index found `c:\games\HexenII` on the box at 14:25 and an hour later the desktop carried Quake and nothing else — the games were there, the icons were in `C:\retro-desktop-backup`. That is the whole of *"i dont see any games on the desktop"* (2026-08-31) | `agent/src/gamesync.c:gs_restore_shortcuts_if_installed` | `python/test_gamesync_enumeration.py::test_an_installed_title_gets_its_icons_back_even_when_not_copied` |

## Adding a test when a fix is verified

1. Identify the invariant the fix establishes (the exact value/relationship that
   was wrong before and is right after).
2. Add `native/test_<fix>.c` (cite source file:function + fix version in the
   header comment) or a `python/test_*.py` case. Assert both the fixed value AND
   the old buggy value (documents the failure mode).
3. `bash tests/run_all.sh` must stay green.
4. Add a row to the table above and, if the fix is a milestone, a line in
   `CLAUDE.md`.

## Hardware capability gate (agent v1.71.0)

| what it pins | test |
|---|---|
| the gate FAILS OPEN — no requires.json, an unparsable one, an unclassifiable GPU or an unmeasurable clock all deploy | `native/test_gamegate.c::fail_open_on_absent_data` |
| the deterministic rules answer the obvious cases ALONE (a Pentium III vs an SM2.0 title is arithmetic, not an LLM call) | `native/test_gamegate.c::rules_decide_the_obvious_alone` |
| the 25% marginal band has BOTH edges — 845 MHz is marginal against 1126 and a flat no against 1127 | `native/test_gamegate.c::marginal_band_has_both_edges` |
| `profile_hash` (the cache key) ignores measurement jitter and moves on real hardware changes | `native/test_gamegate.c::profile_hash_is_stable_and_sensitive` |
| NVIDIA device ids are NOT monotonic — 0x0150 is a GeForce2 GTS and 0x0160 a GeForce 6200 | `native/test_gamegate.c::gpu_table_handles_non_monotonic_ids` |
| a capability gap (`disc_mount`) is REPORTED, never folded into run/marginal/no | `native/test_gamegate.c::capabilities_are_reported_not_folded_into_the_verdict` |
| FLEETRES.EXE is CMOV-free — it is `call`ed by the FIRST LINE of all 32 staged `Play <Game>.bat` launchers, and CMOV is Pentium Pro and later, so an i686-baseline build takes the whole staged library down on a genuine Pentium 1 with `0xC000001D` | `python/test_fleetres_p5_safe.py` |
| a 2D-ONLY adapter is `none`, not `fixed` — an S3 Trio64 has no 3D pipeline, and `none` against any GPU floor is a flat NO rather than a one-level-short MARGINAL | `native/test_gamegate.c::a_2d_only_adapter_is_none_and_that_is_binary` |
| on WINDOWS 98 the GPU's PCI ids come from `HKLM\Enum\PCI` via the instance's `Driver`=`Display\NNNN` binding — EnumDisplayDevices' DeviceID is often empty there and the 9x class key has no `MatchingDeviceId`, so without this the weakest box in the fleet reports `gpu_ven=0`, reads as UNKNOWN and is the one box never gated | `python/test_hwprofile_win9x_gpu.py` |
| per-shortcut requirements overlay the title and do not leak upward (BF1942's LAN half) | `native/test_gamegate.c::per_shortcut_requirements_override_the_title` |
| the C gate and the Python gate give the SAME answer — compiled and compared over the whole GPU table and a grid of fleet profiles | `python/test_gamegate_mirror.py` |
| the verdict cache hits on the same hardware and misses on a corrected `requirements_version` | `python/test_gamegate_host.py` |
| a malformed LLM reply NEVER becomes "run"; the rule verdict stands | `python/test_gamegate_host.py::test_a_malformed_reply_never_becomes_a_verdict` |
| only MARGINAL escalates to the model | `python/test_gamegate_host.py::test_only_marginal_reaches_the_model` |
| a NetQuake / Hexen II server answers NEITHER `getstatus` NOR `status` — it speaks the Quake CONTROL protocol on the game port and drops the other two in silence, so the wrong packet marks a live server DOWN forever while it is pinned into every box's favourites anyway | `python/test_gameservers.py::test_nq_request_is_the_control_packet_not_getstatus`, `python/test_gameindex.py::test_nq_probe_sends_the_control_packet` |
| a Hexen II host replies ONLY to the game string `HEXENII`; sending `QUAKE` is indistinguishable from a dead box | `python/test_gameservers.py::test_hexen2_sends_its_own_game_string` |
| SoF2's player lines carry THREE numbers before the name, so the shared `<score> <ping> "<name>"` ping-0 bot rule reads the wrong field — SoF2 MP has no bots, so the count is a hard zero rather than a parse | `python/test_gameservers.py::test_sof2_never_claims_a_bot` |
| every server in `LOCAL_SERVERS` has a probe — one without is recorded `down` on every favourites pass while healthy | `python/test_gameindex.py::test_every_local_server_has_a_probe` |
| Hexen II's GL build has a small FIXED mode table and refuses 1920x1080, 1280x1024 **and** 1280x960 — it dies at the video-mode check BEFORE loading the map, so the staged host launcher opened no listen server on any 16:9 box while glh2.exe sat in the process list looking healthy | `python/test_lan_multiplayer_library.py::test_hexen2_gl_launchers_cap_to_a_mode_the_engine_has` |
| `sin.exe +set dedicated 1` with no `+map` never binds UDP 22450 — same shape: healthy process, no socket | `python/test_lan_multiplayer_library.py::test_sin_dedicated_servers_load_a_map` |
| Jedi Knight DF2 / MotS refuse to host or join with "No Valid Characters" until a pilot **and** a multiplayer character exist, and the retail trees ship `player\` empty | `python/test_lan_multiplayer_library.py::test_sith_engine_titles_stage_a_playable_profile` |

## The staged-library suite is not safe to judge under heavy concurrency

`test_staged_library.py` (suite [6]) walks the whole SMB share, and the
PE-subsystem check costs roughly **168 ms per binary over SMB across ~731
binaries**. It is no longer a "seconds" tool. With several agents running
`bash tests/run_all.sh` at once — twelve concurrent runs were observed on
2026-08-30 — the validator sits in **uninterruptible IO** and the suite looks
hung at `-- test_staged_library.py` for many minutes.

**A stall there is contention, not a failure.** Check with
`ps aux | grep -c "[r]un_all.sh"` before concluding anything; if several are
running, wait or re-run when the share is quiet. Reading the stall as a failure
is an easy and expensive mistake.

## LAN multiplayer — GoldSrc + the standalone shooters (2026-08-31)

`python/test_lan_goldsrc.py`. Added while proving two-box LAN play on `.171`
(host) with `.133`/`.124` joining. Every row is a defect that **reported
success**: the validator was green, `GAMESYNC` said `state=done` /
`failed_files: 0`, and the desktop shortcut looked perfect.

| fix / invariant | test |
|---|---|
| a mod's precached `events/*.sc` must resolve in its OWN `events\` or in `valve\events\` — Deathmatch Classic shipped only `events\door\` and could not host at all (`Host_Error: EV_Precache: file events/axe.sc missing from server`), while the same gap on the CLIENT is silent: it connects, holds a slot and sticks on "Server # 1" | `python/test_lan_goldsrc.py::GoldSrcEventResolution` |
| that resolution is CASE-INSENSITIVE — TFC ships `Tf_nail.sc`/`Tf_sg.sc` with a capital T and a case-sensitive audit called them missing | `python/test_lan_goldsrc.py::test_resolution_is_case_insensitive` |
| the `valve` fallback covers only valve's own events, so it must not be assumed to cover a mod's weapons | `python/test_lan_goldsrc.py::test_valve_fallback_is_not_assumed_to_cover_everything` |
| Blue Shift is single-player only — judged on its MAPS (37, all `ba_*` campaign), not on its `mpentity` line, which is inherited boilerplate | `python/test_lan_goldsrc.py::BlueShiftHasNoMultiplayer` |
| a staged launcher must not pass `net_connection_provider` — HDE.exe advertises it in `-help` and does NOT implement it, so the game died on `Unknown command-line option: tcpip` | `python/test_lan_goldsrc.py::HiddenAndDangerousOptionTable`, `::test_hd_launchers_do_not_pass_the_unimplemented_option` |
| Red Faction's `UpdateRate` is a rate in BYTES PER SECOND under **HKCU** (`0x30d40` = T1/LAN), not an enum under HKLM; at 0 the client's MULTI menu and `rf.exe -dedicated` both refuse, and the dedicated server binds UDP 7755 on its way out so `netstat` shows a port for a server that is already dead | `python/test_lan_goldsrc.py::RedFactionConnectionSpeed`, `::test_red_faction_install_reg_seeds_updaterate` |

The share-side half **skips loudly** when `/mnt/retro-share` is absent — a
silent skip would let the library rot back to the broken state unnoticed.

### Disc-mount launchers (2026-08-31)

| fix / invariant | test |
|---|---|
| the ~300-line resilient mount launcher had been hand-copied into TEN staged titles, which is how a fix lands in one and not the others — it is now generated from `provisioning/discmount/mount-launcher-template.bat` plus a per-title spec, and every shipped launcher must still equal what its spec generates | `python/test_mount_launcher_template.py::test_shipped_launcher_matches_its_spec` |
| **never wait on `daemon.exe`** — a DAEMON Tools unit can be LOCKED (measured on `.124` and `.240`), and a direct `-mount` call then blocks forever behind a modal: no game, no banner, no `mount-error.txt`, and a leaked `daemon.exe` + `cmd.exe` per attempt (`.124` had five of each). `start "" /b` lets `:waitdisc` decide on the post-condition instead | `python/test_mount_launcher_template.py::test_template_keeps_safeguard[start "" /b "%DT%"]` |
| a locked unit leaves a stuck `daemon.exe` behind its modal, which then wedges the NEXT title's launcher too — clear it when no drive appeared | `::test_template_keeps_safeguard[if not defined DISCDRV taskkill /f /im daemon.exe]` |
| `REQUIREDISC` is **per title**, not a constant: Descent 2 ships `0` and every other title `1`, so hard-coding `1` would make a title that runs perfectly well without its disc refuse to launch on a box whose mount failed | `::test_requiredisc_is_per_title_not_a_constant` |
| a `MARKER` must be unique to THAT disc — `AUTORUN.INF` made the Descent II launcher match a mounted StarCraft disc, and this test found BF1942 still shipping `Setup.ini` | `::test_spec_marker_is_not_a_generic_cd_file` |
| the volume label is tested BEFORE the marker; the marker is only the fallback | `::test_template_checks_volume_label_before_marker` |
| a launcher's `VOLID` must match the image's real ISO9660 label — Max Payne shipped `Max Payne` against `MAX_PAYNE` and only its marker fallback was saving it. The validator reads the label out of the image (2048 / 2352 / 2448-byte sectors; the PVD is at `16*sector+offset`, so a flat 32768 gets zeros) | `scripts/validate-staged-library.py` check `disc-mount` (suite [6]) |
| the disc image a launcher mounts must exist in the tree, and a `.cue`'s `FILE` line must resolve beside it | same |



## Game servers added 2026-09-01 — fix → test

| the failure | fixed in | test |
|---|---|---|
| **Unreal 227 / Deus Ex answer `\info\`, not `\status\`.** UT99 and UT2004 answer `\status\` with hostname, maptitle and numplayers; Unreal 227 answers the SAME packet with only the basic block, so reusing the UT probe renders a live, hosting server as `? \| map=?` with zero players | `scripts/game-servers/gameservers.py::probe_unreal227`, `scripts/game-servers/healthcheck.py::unreal227`, `scripts/gameindex/masters.py::_gamespy_status` | `python/test_gameservers.py::test_unreal227_reads_the_info_reply`, `::test_the_UT_probe_on_an_unreal227_status_reply_is_nameless` |
| **DOOM 3 answers neither `getstatus` nor `\status\`.** id Tech 4's out-of-band message is `short 0xFFFF` + NUL-terminated command + long, and its `infoResponse` puts the echoed challenge and protocol — eight raw bytes containing NULs — before the key/value pairs, which start at **offset 23**. Splitting from byte 0 reads every value against the wrong key | `scripts/game-servers/gameservers.py::probe_idtech4`, `healthcheck.py::d3bfg`, `scripts/gameindex/masters.py::_idtech4_probe` | `python/test_gameservers.py::test_idtech4_pairs_start_at_offset_23`, `::test_idtech4_rejects_a_reply_to_someone_elses_challenge` |
| **Serious Sam names the level `mapname`, the UT family uses `maptitle`** — reading only `maptitle` reported both Serious Sam servers as `map=?` while they were hosting | `scripts/game-servers/healthcheck.py::ut` | `python/test_gameservers.py::test_serioussam_map_comes_from_mapname_not_maptitle` |
| **`healthcheck.py` and `gameservers.py` disagreed about what this host runs** — 18 vs 20, so `descent3-server` and `farcry-server` were live and unchecked by the tool documented as the one-shot check of EVERY server | `scripts/game-servers/healthcheck.py` | `python/test_gameservers.py::test_healthcheck_and_gameservers_cover_the_same_servers` |
| **`host-duties.py` kept a SECOND hand-written game-server list** and it rotted: nine names against twenty-four running servers, so the after-a-reboot check answered ALL HOST DUTIES UP with fifteen servers it had never heard of | `scripts/fleet/host-duties.py::_game_units` | `python/test_host_duties.py::test_game_servers_come_from_gameservers_py_not_a_second_hand_kept_list` |
| **the watchdog had no start-up grace**, so a Wine-in-docker server still booting (Serious Sam loading a level from a .gro, Shogo's xdotool-driven wizard, ~70s before the port is even bound) read as `active but mute 3 cycles` at 60s and was restarted mid-boot — starting the same slow boot over, and on a host reboot doing it to several servers at once | `scripts/game-servers/gameservers.py` (`slow_start_sec`), `gameservers_watch.py::decide` | `python/test_gameservers.py::test_a_slow_starting_server_is_not_mistaken_for_a_wedged_one`, `::test_the_grace_expires_and_a_genuinely_wedged_slow_server_is_restarted`, `::test_every_wine_in_docker_server_declares_a_slow_start` |

## Backfilled index — the rest of the suite (added 2026-09-01)

The `Fix → test coverage` table above is hand-written and had drifted: 53 Python
and 11 native test files existed with **no row anywhere in this file**, so the
table read as the whole suite while covering barely half of it. That is this
project's signature failure — a document reporting success — so the rows below
were generated from each file's own docstring and the commit that introduced it,
and `python/test_readme_lists_every_test.py` now fails the suite if a new test
file is added without a row.

These rows are **mechanically derived**, not curated. When you touch one of these
tests, rewrite its row in the voice of the table above — what broke, how it was
found — and move it up there.

### `tests/python/`

| Fix | Test |
|-----|------|
| **agent 1.29.0: GAMEINDEX command, and fix the Win98 CRT shim that broke the build** (2026-08-25) — agent/lib/libmsvcrt.a — the Win98 CRT import-lib shim must stay valid | `python/test_agent_crtlib.py` |
| **gameindex host pipeline + a real fleet-scope policy for the chat brain** (2026-08-25) — retro_brain_guard — the chat brain's fleet-scope policy | `python/test_brain_guard.py` |
| **brain: publish queue files atomically - answers were being deleted mid-write** (2026-08-28) — The chat brain must never publish a queue file the daemon can read half-written | `python/test_chat_brain_atomic_outbox.py` |
| **tests: the chat daemon's shared connection must only be touched under its lock** (2026-08-28) — The chat daemon's shared send connection must only be touched under its lock | `python/test_chat_daemon_conn_safety.py` |
| **tests: reaping an offline box must not kill the whole chat daemon** (2026-08-28) — Reaping an offline box must not take the whole chat daemon down with it | `python/test_chat_daemon_reap_survival.py` |
| **compat: guard that a `verified` cell keeps evidence that still EXISTS** (2026-08-31) — A `verified` cell must keep at least one piece of evidence that still exists | `python/test_compat_evidence_survives.py` |
| **compat-publish: the default dashboard URL was never a real Azure hostname** (2026-08-31) — The dashboard URL must be a real Container Apps FQDN | `python/test_compat_publish_url.py` |
| **compat: a per-shortcut measurement was invisible to the matrix** (2026-08-31) — A per-shortcut measurement must not be invisible to the matrix | `python/test_compat_shortcut_visibility.py` |
| **GAMESYNC's startup thread idles once `gamesync.done` exists, so a title staged today never reaches a box provisioned yesterday** (2026-09-01) — `scripts/fleet/autodeploy.py` closes that gap. *Row added by the arranger session so the README guard stays green while that work is still uncommitted; the owning session should rewrite this line when it lands.* | `python/test_autodeploy.py` |
| **cs16: LAN game servers on whitebeast + a vanilla-minus-blood CS 1.6 mod** (2026-08-11) — Source invariants for the CS 1.6 no-blood mod (scripts/game-servers/cs16-noblood) | `python/test_cs16_noblood.py` |
| **library: stage Daggerfall and Postal from the build VM's GOG installs** (2026-09-01) — GOG's own DOSBox conf blocks on a keypress; a wrapper's CPU cost belongs on its shortcut, not the title; and Postal's 2018 SDL2 rebuild has an SSE2 floor the 1997 game never had, which rules it out on the fleet's whole pre-SSE2 half | `python/test_daggerfall_postal_staging.py` |
| **safedisc: an emulator cannot invent a protection the IMAGE does not carry** (2026-09-01) — Comanche 4 is SafeDisc 2.40.011, a version DAEMON Tools targets, and still refuses: its FLT/IGG re-master has 0 bad-EDC sectors where working staged SafeDisc titles have hundreds. Reading the exe's version is only half the question | `python/test_disc_image_carries_protection.py` |
| **hwprofile: WinCDEmu's driver is BazisVirtualCDBus, so we never detected it** (2026-08-31) — The disc-mount probe matches SERVICE names, not product names | `python/test_disc_mount_service_names.py` |
| **compat: map Serious Sam's document name to its library directory** (2026-08-31) — Every title in the LAN document must map to a library directory | `python/test_doc_alias_coverage.py` |
| **library: stage DOOM 3, patched to the OFFICIAL id 1.3 update** (2026-08-31) | `python/test_doom3_staging.py` |
| **fleetdb: one database for which games work on which computers** (2026-08-31) | `python/test_fleet_compat.py` |
| **fleetres: quote every emitted set - a PCI id is full of ampersands** (2026-08-30) — Every `set` FLEETRES.EXE emits must be quoted | `python/test_fleetres_quoting.py` |
| **CLAUDE.md: testing is done in FULLSCREEN - a windowed pass is not a pass** (2026-08-31) — Verification must be a FULLSCREEN observation | `python/test_fullscreen_testing_rule.py` |
| **gamebots/goldsrc: fix the compile, dlopen, and link bugs a real 32-bit build found** (2026-08-29) — Regression: the GoldSrc adapter's Metamod hooks must match the real HLSDK | `python/test_gamebots_goldsrc_hooks.py` |
| **gamebots: the custom model on the 5090, and the C client every adapter links** (2026-08-28) — Tests for the custom policy network and its GPU serving runtime | `python/test_gamebots_model.py` |
| **gamebots: the strategic layer — squad intent at 2Hz on the same GPU** (2026-08-29) — Tests for the strategic layer (the LLM planner) | `python/test_gamebots_planner.py` |
| **gamebots: Phase 0 — the schema, the policy server, and the measurements** (2026-08-28) — Tests for the bot policy server | `python/test_gamebots_policyd.py` |
| **gamebots: build the Quake 2 engine adapter** (2026-08-29) — Source-level tests for the Quake II engine adapter | `python/test_gamebots_quake2.py` |
| **gamebots: the Quake III adapter — bots in a real game, driven by our policy** (2026-08-29) — Source-level tests for the Quake III engine adapter | `python/test_gamebots_quake3.py` |
| **gamebots: Phase 2 — demonstration recorder + BC training pipeline** (2026-08-29) — Tests for the demonstration recorder: the shard container format (record/shard.py), the episode-bookkeeping wrapper policyd calls into (record/recorder.py), and the policyd hook that wires it up (policyd.py's --record) | `python/test_gamebots_record.py` |
| **gamebots: Phase 0 — the schema, the policy server, and the measurements** (2026-08-28) — Tests for the game-bot observation/action schema | `python/test_gamebots_schema.py` |
| **gamebots: Phase 2 — demonstration recorder + BC training pipeline** (2026-08-29) — Tests for the behavioural-cloning training pipeline: dataset loading (train/dataset.py), the BC trainer (train/bc.py), imitation evaluation (train/eval_imitation.py) and the record -> train -> evaluate driver (train/e2e_synthetic.py) | `python/test_gamebots_train.py` |
| **gamebots: add the UT99 (UnrealScript) engine adapter** (2026-08-29) — Source-level tests for the UT99 (UnrealScript) engine adapter | `python/test_gamebots_ut99.py` |
| **gamegate: publish_all could never publish - asyncio.run() inside a running loop** (2026-08-31) — `publish_all.py` must be able to publish | `python/test_gamegate_publish_async.py` |
| **gameindex: never write a favourites file we could not read first** (2026-08-29) — The favourites agent must never destroy settings it did not write | `python/test_gameindex_no_clobber.py` |
| **dashboard: game servers, PXE and the services behind them, on the idle wall** (2026-08-28) — Tests for the favourites agent's own health reporting | `python/test_gameindex_status.py` |
| **gamepatch: audit whether the fleet's clients can actually join our servers** (2026-08-29) — Tests for the fleet LAN patch-level audit (scripts/gamepatch/audit.py) | `python/test_gamepatch_audit.py` |
| **agent: GAMESYNC could never overwrite a hidden file, so failed_files was never 0** (2026-08-29) — GAMESYNC must be able to overwrite a HIDDEN or READ-ONLY file, and must not break its own status JSON when reporting the path that failed | `python/test_gs_copy_attrs.py` |
| **agent: GAMESYNC could never overwrite a hidden file, so failed_files was never 0** (2026-08-29) — tests/native/test_gs_json_escape.c carries a VERBATIM copy of gs_json_escape() from agent/src/gamesync.c - this pins the two together | `python/test_gs_json_escape_mirror.py` |
| **halo: stage Halo PC - build the DigitalProductID, and the 1080 Keystone layout** (2026-08-30) — Halo: Combat Evolved - the DigitalProductID the game reads at startup | `python/test_halo_dpid.py` |
| **land the tools CLAUDE.md already documents, and generate the staged-library doc** (2026-09-01) — One Halo CD key per simultaneous player, and never the same key twice | `python/test_halo_key_assignment.py` |
| **wallpaper: draw the icon bay as wide as the arranger actually fills it** (2026-08-30) — The drawn icon bay and the agent's arranger must agree about the columns | `python/test_icon_bay_matches_agent.py` |
| **game-servers: host Descent 3 and Far Cry on the dev host under Wine** (2026-08-31) | `python/test_lan_dosipx_library.py` |
| **library: stage five LAN titles - RTCW, Serious Sam TFE+TSE, Warcraft II, Shadow Warrior** (2026-08-31) | `python/test_newtitles_staging.py` |
| **parens: the rule covered FILENAMES, so the defect moved into a VALUE** (2026-08-31) — A `)` inside an expanded .bat value closes its enclosing block | `python/test_no_parens_in_generated_values.py` |
| **CLAUDE.md: smbclient IS a publish route, and GDI-black is a Windows 7 fact** (2026-08-31) — Two facts the docs had wrong, both of which cost an agent real work | `python/test_publish_and_capture_facts.py` |
| **fleet: the binary offered to the fleet must be built from master, and its tag must exist** (2026-08-30) — Regression: the binary offered to the fleet must be built from master | `python/test_published_build_is_on_master.py` |
| **pxe: proxyDHCP + TFTP server on whitebeast for network OS installs** (2026-08-20) — Tests for scripts/pxe/pxe_server.py (proxyDHCP + TFTP) | `python/test_pxe_server.py` |
| **agent: stop REGREAD/REGWRITE/REGDELETE truncating a key path at a space** (2026-08-29) — The registry-parsing test's VERBATIM copy must stay identical to the source | `python/test_reg_argparse_mirror.py` |
| **safedisc: the version is three dwords at marker+0x20, not at a fixed 0xfd4** (2026-08-31) — SafeDisc's version is at `marker + 0x20`, never at a fixed file offset | `python/test_safedisc_version_offset.py` |
| **screenshot: a 256-colour screen was photographed in the SHELL's palette** (2026-08-31) — A 256-colour screen must be photographed in ITS colours, not the shell's | `python/test_screenshot_palette.py` |
| **library: Serious Sam is NOT SafeDisc - both Encounters come back as disc-mount titles** (2026-08-31) | `python/test_serioussam_staging.py` |
| **agent: desktop shortcuts show the game's artwork, not the generic .bat icon** (2026-08-29) — Desktop shortcuts must show the GAME's artwork, not a generic .bat icon | `python/test_shortcut_icons.py` |
| **land the tools CLAUDE.md already documents, and generate the staged-library doc** (2026-09-01) — docs/staged-library.md is GENERATED, and keeps the distinctions that matter | `python/test_staged_library_doc.py` |
| **tests: a KILLED validator must not read as a BROKEN library** (2026-08-30) | `python/test_staged_library_wrapper.py` |
| **agent: teach UIKEY PrintScreen and the console key, and flag extended keys** (2026-08-29) — UIKEY must be able to send PrintScreen and the console key, and must flag extended keys | `python/test_uikey_named_keys.py` |
| **ut99: a retail 436 client DOES join our 469e server - pin the route** (2026-08-30) — The non-SSE2 boxes' only route to UT99 multiplayer must stay open | `python/test_ut99_436_compat.py` |
| **staging: install a new title in the BUILD VM, not on a fleet box (REQUIRED)** (2026-09-01) — New titles are installed in the BUILD VM, not on a fleet box | `python/test_vm_staging_is_documented.py` |
| **win10/11 survey: a SafeDisc MARKER is not a SafeDisc BLOCK, and a DOS payload under DOSBox is not 16-bit code on the launch path** (2026-09-01) — `scripts/fleet/win64-compat.py` decides a staged title's Windows 10/11 verdict. Pins the four judgements that were wrong first and would have condemned eight titles later MEASURED running on Windows 11: SafeDisc 1.x has no `stxt*` section (Carmageddon 2), the wrapper is live only when it owns the ENTRY POINT (Max Payne vs Red Alert 2), a `.icd` counts only beside a loader-shaped exe (System Shock 2 vs Tiberian Sun), and a shortcut that starts DOSBox never hands its LE/NE payload to the Windows loader (Shadow Warrior). Also pins the import directory as data-directory entry 1 and the `/reg:32` rule measured on Halo | `python/test_win64_compat.py` |
| **retrowall: the old desktop came back because we only cleared HKLM** (2026-08-31) — The legacy wallpaper rotation must be stopped in BOTH registry hives | `python/test_wallpaper_rotation_stops.py` |
| **fleet: watch for boxes arriving, so a power-cycled machine gets picked up** (2026-08-31) — The fleet watcher must report EDGES, and must not mistake a socket for life | `python/test_watch_fleet.py` |
| **game-servers: host Descent 3 and Far Cry on the dev host under Wine** (2026-08-31) | `python/test_wine_servers.py` |
| **xcopy through the agent copies NOTHING and returns 0 unless it is given a stdin** (2026-09-01) — not "broken on some XP boxes": it blocks on its own file-or-directory prompt, and `< nul` fixes it | `python/test_xcopy_needs_stdin.py` |

### `tests/native/`

| Fix | Test |
|-----|------|
| **agent: refuse to downgrade on self-update** (2026-08-27) — test_autoupdate_version.c - TRUE-SOURCE test of the agent's update gate | `native/test_autoupdate_version.c` |
| **agent: EXEC heartbeat logging + DOWNLOAD of live files (v1.23.0)** (2026-08-03) — test_exec_logging.c — protects the EXEC observability fixes (agent v1.23.0) that came out of the .243 Cirrus refresh session, where two mingw console helpers hung inside Win98's WINOA386 console VM and the single-threaded agent went silent for the full 60s EXEC timeout — looking like a crash with nothing in the log between launch and kill | `native/test_exec_logging.c` |
| **gamebots: the custom model on the 5090, and the C client every adapter links** (2026-08-28) — True-source tests for the shared engine-adapter client | `native/test_gamebots_client.c` |
| **gamebots: add the GoldSrc (CS 1.6 / TS) Metamod adapter** (2026-08-29) — True-source tests for the GoldSrc adapter's engine-independent core | `native/test_gamebots_goldsrc.c` |
| **tests: glide GETLINEARADDR zero-base guard regression test (grSstWinOpen crash fix)** (2026-07-22) — test_glide_linaddr_guard.c Guards the grSstWinOpen crash fix in our glide fork (voidsstr/retro3dfx-glide, glide3x/h3/minihwc/minihwc.c hwcMapBoard; see voodoo-cleanroom DEBUGGING-NOTES "GLIDE grSstWinOpen VERIFIED ROOT CAUSE") | `native/test_glide_linaddr_guard.c` |
| **agent: GAMESYNC could never overwrite a hidden file, so failed_files was never 0** (2026-08-29) — test_gs_json_escape.c - GAMESYNC's status JSON must survive a Windows path | `native/test_gs_json_escape.c` |
| **gamesync: stop treating "same size" as "same file"** (2026-08-29) — test_gs_resume_mtime.c - GAMESYNC must not treat "same size" as "same file" | `native/test_gs_resume_mtime.c` |
| **fleet: give every game a desktop icon, and a wallpaper built around finding them** (2026-08-27) — test_icon_bay.c - the wallpaper's icon bay and the agent's icon arranger must describe the SAME grid | `native/test_icon_bay.c` |
| **agent 1.29.1: three bugs GAMEINDEX found the moment it ran on real hardware** (2026-08-25) — test_json_escape.c — TRUE-SOURCE test: compiles the REAL JSON escaper from agent/src/util.c against the fake Win32 in stubs/ and checks what it emits | `native/test_json_escape.c` |
| **tests: pin launch.txt to one shortcut per line** (2026-08-28) — test_launch_txt.c - launch.txt must yield ONE desktop shortcut per line | `native/test_launch_txt.c` |
| **agent: stop REGREAD/REGWRITE/REGDELETE truncating a key path at a space** (2026-08-29) — test_reg_argparse.c - the registry commands must not truncate a key path at a space | `native/test_reg_argparse.c` |
