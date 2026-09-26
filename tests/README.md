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
    test_chat_text_verbatim.py     agent 1.85.0: chat text (LOG_APPEND/2, PROMPT_PUSH,
                          STATUS_SET) passes after ONE space, verbatim - the splitter is
                          compiled out of handlers.c and run; LOG_APPEND2 registered
    test_retro_chat_resilience.py  retro-chat 0.16.0: bounded replies + prompt retry, one-send
                          frames + TCP_NODELAY, absolute log offsets, push notices,
                          one-char echo, event-parked spinner (frame I/O run against
                          native/stubs/netfake_env.h)
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
    test_icd_exit_shutdown.c  MesaFX ICD 0.1.62: grGlideShutdown at process exit, kept across vid_restart (fxapi.c)
    test_chatcore.c       TRUE-SOURCE: agent/shared/chatcore.c — the chat-proxy state
                          engine shared by the Windows agent and the DOS DOSCHAT build
                          (absolute log offsets, prompt take/ack/requeue, LOG_APPEND2
                          dedupe, PROMPT_PUSH reply)
    test_chatproxy.c      TRUE-SOURCE: agent/src/chatproxy.c against a fake single-
                          threaded Win32/Winsock (stubs/netfake_env.h): long-polls
                          cannot spin or miss a wake-up, multiplex parking, prompts
                          survive a dead connection, ring truncation, LOG_APPEND2
    test_protocol_stall.c TRUE-SOURCE: agent/src/protocol.c against the same fake: no
                          recv/send waits forever mid-frame; small replies are one send
    stubs/netfake_env.h   fake Win32 events/ticks + Winsock sockets/select: counts waits
                          (a spinner aborts the test) and recvs that would never return
    test_dosstage.c       TRUE-SOURCE: agent/src/dosstage.c against a fake Win32
                          (stubs/dosstage_env.h) — OS gate (never stage on NT),
                          idempotence, ordering/pacing, registry switches
    test_driver_prefs.c   TRUE-SOURCE: agent/shared/drvprefs.h — the PREFER.TXT
                          parse, the line-anchored hardware-id match, and the
                          reclaim gate (force-install BEFORE deleting C:\D)
    test_drvmatch.c       TRUE-SOURCE: agent/shared/drvmatch.h - which C:\D INFs
                          may serve a device: model lines only, most specific id
                          first, family ids refused, payload present, and when
                          C:\D may be reclaimed
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
| **the generated inventory showed a graphics card that was not in the machine — the THIRD missed card swap** — two published records claimed `192.168.1.124` (`NSC-9871C0E9964` a GeForce2, `NSC-AB862B3CF23` a 3dfx Voodoo5) and `match_records()` took the first in directory order, so the alphabetically earlier, retired machine won. Tie-break is now the FILE's mtime, never `reported_at`: those two records were stamped **2003-04-02** and **2004-07-29** by boxes whose clocks had drifted — `.124` was found believing it was July 2004 (2026-09-01) | `scripts/fleet/inventory.py` (`match_records`) | `python/test_inventory_ip_collision.py` |
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
| **agent 1.85.1: the first-logon driver installer finds the INF, and the reclaim no longer deletes C:\D out from under unconfigured devices** — `gs_find_inf_for()` only ever saw a device's FIRST hardware id (the `&REV_xx` one, which no INF names), so on every fresh image the 865G display and AC'97 audio were never installed and `C:\D` was then reclaimed as "serving nothing"; found while PXE-imaging a Dell Dimension 4600 (2026-09-25). The test also pins what two adversarial reviews against the real 3,669-INF tree found in the first fixes: ids counted outside model lines (comments, ExcludeFromSelect, PosDup, AddReg, wrong OS decoration), family ids, HD Audio codecs, `%strkey%` ids, INFs whose payload was never staged, and the keep/reclaim table | agent gamesync.c + `agent/shared/drvmatch.h` | `native/test_drvmatch.c` |
| **image: AutoPlay off on every drive type, so an ISO mount cannot modal over a fullscreen game** (2026-08-29) | `scripts/pxe/stage-oem.sh` | `test_pxe_autoplay.py` |
| **GAMEINDEX saw only 10 of the 29 staged library titles** (2026-08-29) | agent C (gameindex.c `g_sigs[]`) | `python/test_gameindex_staged_library.py` |
| **favourites agent: decide by what is ON THE BOX, not by our recorded intent** — an external rewrite (GAMESYNC restaging `UnrealTournament.ini` on .171) was reported `unchanged` forever (2026-08-30) | `scripts/gameindex/sync.py` | `python/test_gameindex_favorites.py` |
| **favourites slot numbering, verified in each game's own browser on .133/.143: Q2 `adr0..adr8` (0-based), Q3 `server1..server16` (1-based), GoldSrc's four VDF keys** (2026-08-30) | `scripts/gameindex/favorites.py` | `python/test_gameindex_favorites.py` |
| 0.1.2 SSE float→ubyte color clamp (`fx_pack_ub`) | MesaFX ICD | `native/test_fx_pack_ub.c` |
| V5 runner, RtCW: `r_glIgnoreWicked3D 1` on every non-Wicked3D lane (RtCW 1.4 otherwise forces `gl/openglv5.dll` on a 3dfx card - every earlier "other ICD" RtCW row ran on Wicked3D), clean-room lanes stage the SYSTEM ICD and require the exact build in the renderer string, the AmigaMerlin lane rejects our Mesa (2026-09-25) | bench tooling | `python/test_v56k_bench.py` |
| ICD 0.1.75: `GL_SGIS_multitexture` ON by default (`FX_SGIS_MULTITEXTURE=0` off) - Quake II single-pass +63..71 % on one VSA-100; CS 1.6 unchanged; rows record which path ran (2026-09-25) | ICD / bench tooling | `python/test_cleanroom_q2_multitexture_path.py` + `python/test_v56k_bench.py` |
| ICD 0.1.74 + h5 fork `d161bd4`: sub-row texture patches via `grTexDownloadMipMapLevelPartialRowExt`, whose `min_s` alignment (`&= 2`) was broken in the 3dfx source - fixed and advertised as `RETRO3DFX_PARTIALROW`, used by the ICD only when present; Q2 single-pass +7-8 %, pixel-identical (2026-09-25) | ICD + Glide h5 | `python/test_cleanroom_q2_multitexture_path.py` + `native/test_h5_glide_guards.c` + `python/test_h5_glide_fixes.py` |
| ICD 0.1.71-0.1.73 Quake II single-pass (SGIS) wall: `glTexSubImage2D` sends only the changed rows (partial reload rewritten - Glide3 LOD, byte row pointer), SGIS unit select reads its env var once, unit selection marks state dirty instead of flushing; 50.8 -> 184.2 fps at 640x480, 49.3 -> 162.6 at 1024x768, pixel-identical (2026-09-25) | ICD | `python/test_cleanroom_q2_multitexture_path.py` |
| ICD 0.1.69 clipped-path triangle batching: pixel-identical but no measurable gain, REVERTED in 0.1.70 - the negative result is in CHANGELOG; its pixel harness `icd_frame_compare.py` stays (2026-09-25) | ICD / bench tooling | `python/test_v56k_bench.py` (frame-compare cfg) |
| ICD 0.1.67 render-thread sampling profiler (`RETROGL_PROF`): inert unless set, sampler touches only kernel calls while the target is suspended; host symbolizer `icdprof.py` (2026-09-25) | ICD | `python/test_cleanroom_icd_profiler.py` |
| ICD 0.1.67 + h5 fork `5439bb8`: a fatal Glide error is LOGGED (cdecl callback installed before `grGlideInit`, kept by our Glide) instead of an invisible `MessageBox` - the intermittent "hang in grGlideInit"; unmap falls back to the PID the mapping was filed under; 0.1.68 teardown breadcrumbs (2026-09-25) | ICD + Glide h5 | `python/test_cleanroom_glide_error_callback.py` + `python/test_h5_glide_fixes.py` |
| V5 runner: Quake II `nextserver` set AFTER `demomap` (retail `SV_Map` clears it) - every Quake II cell had ended in a force-kill, which on our Glide left the display driver a stale per-PID slot; all-ours launches always carry `RETRO_GLIDE_MAPLOG` (2026-09-25) | bench tooling | `python/test_v56k_bench.py` |
| h5 Glide asm triangle-setup build (2026-09-25): fxgasm offsets derived by the TARGET compiler (`FXGASM_CROSS=1`, fork `c41b50d`) — the host-run fxgasm is 64-bit here and all 50 GC offsets the asm reads were wrong; `build-stack.sh` emits `glide3x_h5_x86.dll` beside the C build | Glide h5 | `python/test_h5_glide_fixes.py` (every `OFFSET()` static-asserted in an i686 compile) |
| V5 runner provenance: a row's `icd_md5`/`glide3x_md5` name the files the TITLE loads (game-local `retrogl.dll`/`glide3x.dll`, system32 `retroicd.dll` for RtCW), not system32's AmigaMerlin ICD - every all-ours 0.1.75 row had carried `3dfxOGL.dll`'s md5 (2026-09-25) | bench tooling | `python/test_v56k_bench.py` |
| box-guardian: silence counts only while SMB answers - a box switched back on after 15 h off was rebooted mid-boot as a "wedge" (2026-09-25) | fleet tooling | `python/test_box_guardian.py` |
| V5 6000 runner: a wedged cell's thread stacks are taken with XP `ntsd -pv` BEFORE the kill and symbolized against our own Glide/ICD; all-ours rows name the Glide build variant (`opengl-allours-<ver>-x86`) (2026-09-25) | bench tooling | `python/test_v56k_bench.py` |
| h5 Glide swap bookkeeping (2026-09-24): `bufferSwaps` loops no longer index one past the array (a stray match wrapped the unsigned `swapsPending` and hung the swap wait forever), both swap waits and `_grBufferNumPending` bounded — fork `468609e` | Glide h5 | `native/test_h5_glide_guards.c` + `python/test_h5_glide_fixes.py` |
| h5 Glide audit fixes (2026-09-24): bounded idle wait that reports, FX_GLIDE_NUM_CHIPS only 1 or real, live-mapping validation before MMIO, 32-bit escape field — fork `215a9e7` | Glide h5 | `native/test_h5_glide_guards.c` + `python/test_h5_glide_fixes.py` |
| h5 Glide H1–H7 (TLS accessor, grGetString guard, board/slave-reg map guards, SLI/AA result, XP escape, FX_GLIDE_BPP) — fork `839143c` | Glide h5 | `python/test_h5_glide_fixes.py` |
| 0.1.66 Mesa's per-span scratch arrays on the heap (SiN Gold stack overflow in an 80 KB `_mesa_unpack_color_span_chan` frame) | MesaFX ICD | `python/test_cleanroom_heap_span_arrays.py` |
| 0.1.65 `wglCreateContext`'s activation pump is bounded and never dispatches WM_PAINT (ioquake3 hung forever on the system ICD) | MesaFX ICD | `python/test_cleanroom_activation_pump.py` |
| 0.1.64 fullscreen refresh = the monitor's best, re-implemented in SOURCE (`fxBestRefresh`, bug I1) | MesaFX ICD | `python/test_cleanroom_refresh_source.py` + `native/test_fx_best_refresh.c` |
| 0.1.63 the ICD is also a Microsoft ICD: 17 `Drv*` exports + the 336-entry dispatch table (`fxicd.c`) | MesaFX ICD | `python/test_cleanroom_icd_frontend.py` |
| 0.1.62 Glide is shut down at process exit, still kept alive across `vid_restart` (`cleangraphics`/`fxCloseHardware`) | MesaFX ICD | `native/test_icd_exit_shutdown.c` |
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
| **agent 1.82.1: REBOOT/SHUTDOWN/DOSSTAGE start their thread on Win9x** (2026-09-24) — `CreateThread` with a NULL lpThreadId fails on Win95/98 (error 87); REBOOT answered OK and did nothing on every Win9x box (log: `FAILED to start the shutdown thread`), found on .243 | `python/test_agent_createthread_win9x.py` |
| **agent 1.83.0: Win9x PCI rescue + accelerators[] on Win9x** (2026-09-24) — .243's Voodoo 2 answers config cycles but Win98's boot-time enumeration misses it (BAR0 left 0; Glide then maps it over RAM and kills the box); the agent now re-enumerates the PCI bus at startup when an installed device has no devnode. hwextra's accelerators[] read only the NT Enum path, so every 9x box reported no 3dfx | `python/test_pcirescue_win9x.py` |
| **agent 1.83.1: no startup GAMESYNC on modern Windows** (2026-09-24) — 1.82.0 refused only the GAMESYNC *command*; the startup thread had no host-policy check and copied 2.6 GB of the library onto WHITEBEAST (Win11). gamesync_thread and gs_start now ask the policy first | `python/test_hostpolicy.py` (`test_startup_appliers_are_guarded`, `test_gamesync_thread_asks_the_policy_before_doing_anything`) |
| **agent 1.83.2 + favourites push: nothing else touches a modern Windows host** (2026-09-24) — an audit of every startup/background path found the watchdog (kills fullscreen games, resets the display mode) and the chat-client updater ungated, and the host-side favourites push had written into WHITEBEAST's own UnrealTournament.ini. HWPROFILE now reports host_policy and sync.py asks it | `python/test_hostpolicy.py`, `python/test_gameindex_modern_host.py` |
| **agent 1.84.0: `glide` gate capability** (2026-09-24) — a present, driver-bound 3dfx device, so a Voodoo-only shortcut (GLQuake on .243's Voodoo 2 behind a Cirrus) appears only where it can run; a pulled card's leftover Enum key does not count | `python/test_glide_capability.py`, `native/test_gamegate.c` |
| **agent 1.84.1: the Auto Arrange command ID is per platform** (2026-09-24) — the agent posted 0x7031, which is no menu command on Win98 or XP (read from each OS's SHELL32.DLL: Win98 0x7041, XP 0x7051, and 0x7051 is Help Topics on Win98). On .243 neither the toggle nor the style bit took, so new icons landed off-screen; the icons are now packed with LVM_ARRANGE even when auto-arrange cannot be set | `python/test_icon_autoarrange_source.py` |
| **agent 1.84.2: GAMESYNC resumes a file after a failed read** (2026-09-24) — Win98's network client dropped long SMB reads (error 55) 7-40 s into Hexen II's paks on .243; one failed ReadFile abandoned the file, so the box never got gamesync.done and re-walked the library every 120 s. It now reopens the source and seeks back, bounded, refilled only by progress | `python/test_gamesync_copy_resume.py` |
| **agent 1.84.3: the boot survives in the log, and the PCI rescue records itself** (2026-09-24) — on Win9x the chat long-polls (clamped to 1 s) wrote three log lines a second and rotated agent.log + agent.log.1 in ~2 h, so .243's whole boot, including whether PCIRESCUE had run, was gone. A long-poll is now logged once per connection, frame lines only for transfers >= 1 KB, and the startup rescue writes `HKLM\Software\RetroAgent\PciRescueBoot` (PCIRESCAN `last_boot`) | `python/test_agent_log_chatter.py`, `python/test_pcirescue_win9x.py` |
| **agent 1.85.0: RESTART brings a Win9x agent back** (2026-09-24) — the relaunch batch said `start "" "<exe>"`, which is cmd.exe syntax; Win98's START.EXE takes the `""` as the program, so RESTART left .243 with networking up and no agent. On 9x it now writes `start <8.3 path>`, the form the auto-update batch has proven there | `python/test_doschat_shared.py` |
| **agent 1.85.0: a clock that is YEARS wrong is set from the NAS** (2026-09-24) — .243's dead CMOS battery booted it into 1980 at every power-on, so its log, its published hardware record (`stale` forever in the inventory) and every file it wrote carried 1980. At startup, only when the year reads < 2024, the agent takes the NAS's HTTP `Date:` and sets UTC; `ClockFixed` records what changed. The parser refuses anything but exact RFC 1123 | `native/test_httpdate.c`, `python/test_hostpolicy.py` |
| **agent 1.85.0: auto-update actually updates a Win9x box** (2026-09-25) — .243 downloaded 1.84.3, "shut down for the swap" and was back on 1.84.2 in 4 s: the swap batch trusted COPY's ERRORLEVEL, which COMMAND.COM never sets, so a copy onto the still-locked exe read as success. On 9x it now renames and checks `if exist`. The chat client never updated on 9x either: Toolhelp reports a full path there, so the kill never matched and CopyFile hit error 32. AGENTRUN.BAT (the boot-time updater) deletes a stale `share.ver` before re-copying it, retries the share for ~30 s, and uses the long quoted share name (Samba's 8.3 alias is a hash, not `RETRO_~1`) | `python/test_autoupdate_win9x_swap.py` (compiles the real batch builder and runs its output in a COMMAND.COM model), `python/test_dosstage_and_batch.py` |
| **agent 1.85.0: GAMESYNC's free-space margin scales down on a period disk** (2026-09-25) — the fixed 300 MB margin is a quarter of .243's 1.2 GB C:, so with 358 MB free it refused the 199 MB Quake II base game while logging "needs 199 MB, only 358 MB free". Under 4 GB the margin is 150 MB, and the SKIP line names the margin | `python/test_gamesync_small_disk_margin.py` |
| **agent 1.85.0: a Win9x agent really exits** (2026-09-25) — 1.84.2 on .243 logged "shutdown complete; exiting process" at QUIT and was still running 120 s later (ExitProcess on 9x runs every DLL's PROCESS_DETACH and one blocked), so the rollback-guarded swap gave up and the box had no agent until a person restarted it. After the log is closed a 9x agent ends itself with TerminateProcess | `python/test_doschat_shared.py` |
| **agent 1.85.0: a directory listing that ends early is a failure, not the end** (2026-09-25) — on .243 the Win98 redirector dropped the SMB session mid-copy (error 55); the file copy survived, but the reconnect invalidated every open search handle and `while (FindNextFileA(...))` took the FALSE for "no more files". Quake2Win9x reported "done: 4/52 copied, 0 file error(s)" with quake2.exe and every pak missing. The listing must now end in ERROR_NO_MORE_FILES, AND no share reconnect may have happened while it was open - measured on .243, Win98 ends a listing whose session was reset with a plain ERROR_NO_MORE_FILES, so the error code alone cannot tell. Otherwise the directory is re-listed (resume makes that cheap) and, if it still breaks, recorded as a failure | `python/test_gamesync_listing_cut_short.py` (compiles gs_copy_tree against a fake file API; the old code copies 3 of 17 files and reports success) |
| **agent 1.85.0 chat path 1: STATUS_WAIT spun the CPU for up to 30 s on XP** (review 2026-09-24) — `g_status_event` was manual-reset and only the slow path reset it, so after any fast-path answer every later wait "woke" at once, saw no change and looped until its deadline at HIGH priority. The long-polls now wait on one-shot GENERATION events (joined under the lock that checked the condition, never reset): no spin, no missed wake-up, every waiter woken | `native/test_chatproxy.c` (the old chatproxy.c aborts its spin guard at 5001 waits) |
| **agent 1.85.0 chat path 2: Win9x long-polls PARK instead of blocking** — in multiplex mode a long-poll blocked the one thread on an event only that same thread could set, so pollers serialised into ~1 s sleeps and every command queued behind them. Parked in the client slot {kind, arg, deadline}, answered after each command and select pass, select bounded by the nearest deadline; the 1 s clamp now only guards a blocking fallback | `native/test_chatproxy.c`, `python/test_doschat_shared.py` (`test_multiplex_longpolls_park_instead_of_blocking`) |
| **agent 1.85.0 chat path 3: no recv/send waits forever mid-frame** — on Win9x (no SO_RCVTIMEO) a peer that vanished part-way through a frame froze the whole agent. select() precedes every in-frame recv (30 s) and send (60 s, 8 KB chunks); per call, so a slow transfer is never cut off; UPLOAD's payload frame must start within 30 s | `native/test_protocol_stall.c` |
| **agent 1.85.0 chat path 4: a prompt is not lost to a dead connection** — PROMPT_WAIT popped the prompt and "sent" it into a half-open socket. A waiter now checks its peer first (select + MSG_PEEK) and leaves the prompt; a taken prompt stays in flight until that connection's next command and is put back if it drops first (unless a newer prompt was pushed) | `native/test_chatcore.c`, `native/test_chatproxy.c`, `python/test_doschat_shared.py` |
| **agent 1.85.0 chat path 5: log offsets are ABSOLUTE** — the ring's drop-oldest-half shrank the total under readers: retro_chat reset to 0 and reprinted up to 128 KB, the DOS UI printed nothing. `log_base` now advances by what a drop discards; behind-base readers resume at the base; past-the-end means cleared. An old retro_chat is fixed by a new agent unchanged (its offset += bytes equals the absolute position) | `native/test_chatcore.c`, `native/test_chatproxy.c` (`ring_truncation_does_not_send_a_reader_back_to_zero`), `python/test_retro_chat_resilience.py` |
| **agent 1.85.0 chat path 6: LOG_APPEND2 `<id> <text>` + chat text verbatim** — the daemon's resend after a timeout duplicated reply text; LOG_APPEND2 remembers the last 16 chunk ids and answers `OK dup`. And the dispatcher's space-trim glued words across chunks and flattened indentation: LOG_APPEND/2, PROMPT_PUSH and STATUS_SET take everything after exactly one space | `native/test_chatproxy.c`, `python/test_chat_text_verbatim.py` |
| **retro-chat 0.16.0: a wedged agent cannot freeze the chat** (chat path 8/9) — replies are bounded (poll + 10 s, 15 s for a push, 10 s AUTH); an undelivered prompt is retried for 30 s, then `[prompt NOT delivered - agent unreachable]` and the spinner stops. Frames go out as ONE send with TCP_NODELAY (the two-send frames with Nagle cost up to ~200 ms per poll); the agent coalesces its replies the same way | `python/test_retro_chat_resilience.py`, `native/test_protocol_stall.c` |
| **agent 1.85.0 / retro-chat 0.16.0 chat path 10: PROMPT_PUSH says `OK replaced` / `OK no-listener <n>s`** — an unpicked prompt was silently overwritten and nothing said nobody was polling; the reply still starts `OK` for old clients, and the chat shows a one-line notice | `native/test_chatcore.c`, `native/test_chatproxy.c`, `python/test_retro_chat_resilience.py` |
| **retro-chat 0.16.0: typing is cheap on Win9x** (CPU review 2026-09-24) — every key was a full erase + redraw (~12 console calls through the 16-bit console); appending at the end of the line now writes one character, and the idle spinner sleeps on an event instead of waking 4x a second forever | `python/test_retro_chat_resilience.py` |
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
| **the per-game driver sweep must never rewrite a DRIVER PACKAGE dir** (2026-09-01, .185: it planned to replace `C:\DRIVERS\amigamerlin-3.1-R11\3dfxOGL.dll` -- the installer you roll back TO) | `.claude/skills/driver-install/game_sweep.py` | `python/test_game_sweep_skip.py` |
| player-profile store: cvar command word preserved per game family, ini applied as a patch, dry-run stays offline | scripts/retro_playerprofile.py | `python/test_playerprofile.py` |
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
| the agent skinned and reconfigured ANY box it ran on, including a modern Windows 10/11 machine that hosts it only so the fleet can reach it (the copier host `.139`) - theme, wallpaper, screensaver, icon layout, autologon and staged content all applied unasked (2026-09-16) | `agent/src/hostpolicy.c`, `agent/src/retrowall.c`, `agent/src/sysfix.c`, `agent/src/handlers.c` | `python/test_hostpolicy.py` |
| `GetVersionEx` reports **6.2** on every Windows 8.1/10/11 for a manifest-less EXE, and `retro_agent.exe` has no manifest - so the obvious `dwMajorVersion >= 10` test is FALSE on exactly the machines it is meant to protect, skins them anyway, and reads as if it works. `RtlGetVersion` (LoadLibrary'd, never imported - Win98 has no ntdll) is the only honest answer (2026-09-16) | `agent/src/hostpolicy.c` | `python/test_hostpolicy.py`, `python/test_agent_win9x_imports.py` |
| a per-handler "is this box ours?" guard would be forgotten by the sixteenth handler and the miss is silent - the box just gets reconfigured; the policy is a FLAG on the command table enforced once in `handle_command`, and the flagged SET is pinned in both directions so neither a missed flag nor a stolen operator command can land quietly (2026-09-16) | `agent/src/handlers.c` | `python/test_hostpolicy.py` |
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
| CS 1.6 OpenGL crashed on the Voodoo 5 6000 ("hl.exe - Application Error ... 0x035e8fa0"): the Mesa-based AmigaMerlin ICD's SSE-exception probe (a deliberate `divps` by zero) is caught by GoldSrc's own handler, which shuts the engine down; `MESA_FORCE_SSE=1` skips the probe (.124, 2026-09-23) | staged `CounterStrike16\Play Counter-Strike.bat` / `Play Half-Life Deathmatch.bat`, `scripts/benchmarks/v56k_bench.py::CS16.launch_bat` | `python/test_goldsrc_mesa_sse.py`, `python/test_v56k_bench.py::test_cs16_is_a_title_and_plays_its_demo_through_timedemo` |
| GLQuake really does refuse 1920x1080 AND 1600x1200 and tops out at 1280x960, while Hexen II's `glh2.exe` — same family — takes 1920x1080 on the same box (.145, 2026-08-30) | `scripts/fleet/stage-fleetres.py` Quake1/HexenII recipes | `python/test_fleetres_staging.py::test_glquake_cap_is_the_measured_ceiling_not_the_old_guess` |
| a game-local nGlide `glide2x.dll` SHADOWS the real system32 Glide on the two boxes that have 3dfx silicon, so UE1 falls to the software rasterizer at 100% CPU and it looks like a crash (.171, 2026-08-30) | `scripts/fleet/stage-fleetres.py::glide_swap`, `provisioning/fleetres/fleetres.c::glide_probe` | `python/test_fleetres_staging.py::test_glide_swap_renames_in_both_directions` |
| `%%` outside a FOR loop is a silent no-op — cmd.exe compares the literal text `%FR_GLIDE%`, so the block reads correctly and never runs (2026-08-30) | `scripts/validate-staged-library.py` `fleetres-percent` | `python/test_fleetres_staging.py::test_validator_rejects_a_doubled_percent_in_a_launcher` |
| Quake III rendered on the Intel 865G on the box with the Voodoo 2 pair, fullscreen and at the right resolution, because a Voodoo 2 is `Class=MEDIA` and registers NO OpenGL ICD — and ioquake3 cannot be pointed at it by ANY route: `r_glDriver` was dropped, `opengl32` is in XP's KnownDLLs, and the bundled SDL hardcodes `OPENGL32.DLL` (.171, 2026-09-01) | staged `Games-Library/Quake3-TeamArena/FLEETGL.BAT` + the three `Play ....bat` launchers | `python/test_q3_voodoo2_gl.py` |
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
| **safe-reboot: read a Win9x box's MAC from HWPROFILE** (2026-09-24) — Windows 9x has no cmd.exe, so the `cmd /c ipconfig` MAC read returned nothing and the script refused to reboot every Win98 machine (found on .243 while installing a Voodoo 2) | `python/test_safe_reboot_win9x_mac.py` |

| an id Tech mode index named a mode the driver does not offer — `FR_Q3MODE`=7=1152x864 on `.246`, whose adapter lists 1152x**648** and no 1152x864; the engine neither errored nor obeyed, setting the desktop to 1280x960 and drawing into a **window** with `r_fullscreen` still 1. `SoldierOfFortune2`/`JediAcademy` consume that value on all three 1080p boxes (2026-08-31) | `provisioning/fleetres/fleetres.c` `q2_mode_for`/`q3_mode_for` | `python/test_fleetres_mode_offered.py` |

(The vintage SGL/H5 fixes — garble 0.3.1, mip-download 08fd889 — are tested in
`retro-3dfx/tests/`, the other lane's harness, not here.)

**Backlog for OUR stack** (from `voodoo-cleanroom/CHANGELOG.md`, 0.1.x): swap-interval
env default (0.1.6), LOD-bias default (0.1.11), alpha-PFD matcher +
paletted-default-off (0.1.30), vertex cache (0.1.3), Q2 glide3x-binding (0.1.19);
agent-C — `handle_execw` timeout clamp, `discovery_build_packet` ⇄ Python
`from_packet` round-trip, `util.c` json/hex helpers; render (CSIM track) — filters,
green-world; provisioning — P3 no-SSE2 opcode scan of staged DLLs.

### GAMESYNC per-box resolution — GAMERES (2026-09-04)

One staged library, eight different monitors. `agent/shared/gameres.h` decides
what mode THIS box's panel wants, and `agent/src/gameres.c` writes it into each
staged title's own config at the end of that title's sync.

| fix | code | test |
|---|---|---|
| **a staged `install.reg` re-pinned the resolution on every sync and nothing corrected it afterwards.** `HalfLife1/install.reg` sets `HKCU\Software\Valve\Half-Life\Settings ScreenWidth`, `gs_merge_reg()` re-applies it on every box on every sync, and that ONE key is the mode for every GoldSrc title on the machine — there is no `Software\Valve\CounterStrike` key at all. Its own comment records that Counter-Strike "ignores -w/-h on the command line for the same reason", so a launcher could never undo it: the value is written AFTER the launcher ran. Measured on `.191`: 800x600 on a box whose panel wants 1024x768 (2026-09-04) | `agent/src/gamesync.c:gs_run` (the pass runs after `gs_merge_reg`), `agent/shared/gameres.h` | `native/test_gameres.c::t_goldsrc_reaches_the_panel`, `python/test_gameres_mirror.py` |
| **`GR_OP_KV` handed the writer the bare VALUE instead of `key=value`.** `gr_w_line(kv=1)` replaces a whole LINE, so it replaced `ResolutionX=1024` with `1024` — which then no longer parses as key=value, so the next pass matched nothing and APPENDED another `1024`. Three passes on `.191` left Descent 2's `DESCENT.CFG` with six junk lines and no resolution at all, every pass reporting success. **Found in seconds because the pass reports how many values it CHANGED and a settled box must report zero** — Descent 1 and Descent 2 kept reporting 2 apiece (2026-09-04) | `agent/shared/gameres.h:gr_kv_line`, `agent/src/gameres.c` `GR_OP_KV` branch | `native/test_gameres.c::t_kv_line_is_composed`, `python/test_gameres_mirror.py::test_the_kv_writer_is_handed_a_composed_line` (a SOURCE assertion — a test of the helper alone passes against the broken caller) |
| the two mode tables diverge at index 8: id Tech 2's is 1280x960 (4:3), id Tech 3's is 1280x1024 (5:4), so one index means two different pictures | `agent/shared/gameres.h:gr_q2_mode_for`, `gr_q3_mode_for` | `native/test_gameres.c::t_q2_q3_tables_differ` |
| the target must come from the PERSISTED desktop mode, never the live one — a game that exits without restoring leaves the desktop at 640x480 and a pass that WRITES that pins the box there. `.191` was measured live at 640x480 with 1024x768 persisted, and answered 1024x768 | `agent/shared/gameres.h:gr_decide` (the signature has no live-mode parameter) | `native/test_gameres.c::t_lcd_gets_native`, `t_crt_never_5_4` |
| no EDID means assume a 4:3 TUBE, not "believe the desktop" — `.133` lost its EDID across a reboot and was handed 1280x1024 back for a tube measured at 37x28 cm | `agent/shared/gameres.h:gr_decide` | `native/test_gameres.c::t_no_edid_assumes_4_3` |
| a mode must be one the driver actually OFFERS; an empty enumeration means "could not ask", not "nothing is offered" (`.143`'s GeForce 6800 returns FALSE at index 0) | `agent/shared/gameres.h:gr_mode_offered` | `native/test_gameres.c::t_only_offered_modes`, `t_empty_list_is_not_a_refusal` |
| **the monitor's highest refresh, PER RESOLUTION.** A rate is offered per mode, not per box — `.191`'s tube does 100 Hz at 1024x768 and 75 at 1280x960 — so one `hz` for the machine is wrong for one of the two engines running at those two targets. The 0/1 Hz "driver default" entries are sentinels and are never a rate (2026-09-04) | `agent/shared/gameres.h:gr_best_hz`, `gr_hz_is_real` | `native/test_gameres.c::t_refresh_is_per_resolution` |
| the EDID refresh ceiling must be applied when a mode is **added**, not when it is read: only the best rate per resolution is kept, so a read-time clamp stores a rate the panel cannot sync and has nothing to fall back to. No EDID means no clamp AND no claim — the answer is 0, "leave it alone", never 60 | `agent/shared/gameres.h:gr_modes_add` (`hz_cap`) | `native/test_gameres.c::t_refresh_is_clamped_to_the_edid` |
| **a file BOTH the agent and the title's launcher write must get the same settings from each.** The launcher rebuilds `fleetres.cfg` at every start and the agent rewrites it whenever one of its settings is missing, so a one-line disagreement (a refresh rate) means each rewrites the other's copy forever and the `0 value(s) changed` signal dies with it | `agent/shared/gameres.h:gr_cfg_body`, `scripts/fleet/stage-fleetres.py:idtech3_cfg` | `python/test_gameres_mirror.py::test_a_shared_cfg_is_written_identically_by_both_writers` |
| the agent's rule table must not become a SECOND answer: every rule is checked against the staged library and against the launcher that already writes the same file | `agent/shared/gameres.h:gr_rules` | `python/test_gameres_mirror.py` |
| `validate-staged-library.py` failed the WHOLE library on Quake III because `FLEETGL.BAT` uses `%FR_*%` without calling `FLEETRES.BAT` — it is a HELPER the Play launchers call after it, so the variables are already in the environment. A validator that cries wolf is one people learn to ignore (2026-09-04) | `scripts/validate-staged-library.py` | `test_staged_library.py` (suite [6]) |

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
| **brain: stream the prompt; never blame accounts for an SDK crash** (2026-09-23) — a user on .171 was told "No Claude account on the brain is usable" while every account worked; a stale brain on an older SDK rejected the bare-string prompt (`can_use_tool callback requires streaming mode`) and the brain reported that as an account failure | `python/test_chat_brain_prompt_stream.py` |
| **brain: follow-up prompts resume their conversation** (2026-09-25) — a .184 user continuing the EPoX boot-floppy chat got "all accounts failed" on every follow-up: the claude-pool shim (first on PATH via a user-wide systemd drop-in) re-picked a profile per call, so `--resume` ran in an account without the transcript ("No conversation found"), and failover HOMEs saw no pool (exit 70). `_find_cli()` skips the shim, transcripts are copied to the resuming account, sessions persist across restarts | `python/test_chat_brain_session_resume.py` |
| **tests: the chat daemon's shared connection must only be touched under its lock** (2026-08-28) — The chat daemon's shared send connection must only be touched under its lock. *Rewritten 2026-09-25 as behaviour*: every send-connection command now goes through `_send_cmd`, which refuses to run without the lock; concurrent text/status/tasks/keepalive against a fake agent deliver every line exactly once | `python/test_chat_daemon_conn_safety.py` |
| **tests: reaping an offline box must not kill the whole chat daemon** (2026-08-28) — Reaping an offline box must not take the whole chat daemon down with it. *Rewritten 2026-09-25 as behaviour* against the reworked daemon (no host-task gather; done-callback reporting; a box that appears later is claimed) | `python/test_chat_daemon_reap_survival.py` |
| **chat daemon: per-host senders, held-in-order text, LOG_APPEND2, newest-status-only** (2026-09-25) — one dead box stalled every chat (serial outbox loop with retries + sleeps); a briefly-away box had its answer moved to failed/ (or DELETED as "unknown host" once reaped); a timed-out LOG_APPEND was resent and the user saw the answer twice; STATUS_SET met a connection the agent had already closed and "thinking..." was lost (~5 of 9 prompts); a status file every ~30 chars was forwarded under the answer's lock. Driven against fake agents on localhost | `python/test_chat_daemon_delivery.py` |
| **chat daemon: claim fast, never reap a live box, keepalive, FIN-only teardown** (2026-09-25) — the first scan ran before the LAN was up and the next was 300 s later; hosts were reaped on discovery misses alone (last_seen never set), cancelling a PROMPT_WAIT that may carry a popped prompt; the idle send connection died silently (NT drops a client at 120 s); seq restarted at 1 on re-add; try_connect leaked its connection; SIGTERM reset every socket; a timed-out command to a busy Win98 agent drew an RST when its late reply arrived | `python/test_chat_daemon_liveness.py` |
| **chat daemon: the deferred task queue is durable and never re-runs a command that may have run** (2026-09-25) — every queued command got 60 s, so `EXECW 300` "failed" and was re-run up to 3x; an interrupted task re-ran completed commands; the queue lived in tmpfs `/tmp` (lost on reboot). Now `~/.retro-fleet/chat-tasks`, migrated from the old path; `scripts/retro_enqueue.py` writes and lists it | `python/test_chat_daemon_tasks.py` |
| **chat brain: restart-safe prompts, one live claude per machine, per-prompt cap** (2026-09-25) — a restart deleted in-flight prompts (the inbox file went on dispatch); a restarted brain's live session must resume the persisted conversation; a runaway prompt held a machine's FIFO forever; each prompt spawned a new `claude` CLI (measured 3.8 s median for "reply PONG", 3.0 s warm on a reused session). Stub Agent SDK, no API calls | `python/test_chat_brain_resilience.py` |
| **protocol: `connected` sees the agent's FIN, `request_sent`, draining close** (2026-09-25) — `connected` only checked `is_closing()`, so a connection the agent had dropped looked healthy; callers could not tell "never sent" from "sent, no reply"; `close()` slept 0.1 s and closed, so a late reply drew an RST (Win98 Winsock). Also pins client/ and nsc-assistant shared/ as identical | `python/test_protocol_connection_state.py` |
| *helper, not a test:* fake retro agent speaking the real framed protocol on localhost (records the chat log, statuses and whether each connection ended in a FIN or an RST) + the daemon loader; `NSC_ASSISTANT_DIR` points the daemon tests at an nsc-assistant worktree | `python/chat_fake_agent.py` |
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
| **bench: the Voodoo 5 6000 chip x FSAA campaign, and the watchdog that lets it run unattended** (2026-09-15) — `v56k_bench.py` applies SSTH3_SLI_AA_CONFIGURATION and READS IT BACK (the old runner recorded an FSAA level it never applied), keeps the CSV header in step when a column is added mid-campaign, and does not mistake an id engine's silent timedemo for a wedge. Also pins the recovery half: the sweep spends another boot on a config only when the last pass actually measured a cell, and the agent watchdog is a Run-key loop that needs no password in argv | `python/test_v56k_bench.py` |
| **bench: record what was RUNNING, and never let a retroactive probe pose as a measurement** (2026-09-15) — every benchmark row carries the game binary's md5, the driver package/version, the Glide and ICD hashes, the OS and the agent; `v56k_versions.py` backfills only EMPTY cells and marks each row it touches | `python/test_v56k_bench.py` |
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

## Agent CPU / IO efficiency on the old boxes (agent 1.85.0, 2026-09-25)

The user's report: *"the retro chat / agent when starting up / connecting can
tax the older cpus"*. The fleet runs down to a Pentium 166 on Win98 (`.243`)
and a 31 MB Deskpro; a PIII 800 on XP is typical. Every row is work the agent
did at every boot or on a timer whether or not anything had changed.

| Fix | Test |
|-----|------|
| **priority: background helpers ran above the game** — the process is HIGH_PRIORITY_CLASS so commands stay reachable during a fullscreen game, but every helper inherited base 13 too (BELOW_NORMAL is still 12 there). Helpers now call `thread_background()` (THREAD_PRIORITY_IDLE, base 1); command-serving threads are never lowered; the log lock lifts an IDLE holder so a starved helper cannot stall the command thread's next log line; the Win9x TSC clock measurement (which feeds the gate's profile hash) runs at TIME_CRITICAL so an IDLE caller cannot skew it | `python/test_agent_priority_model.py` |
| **idle wake-ups: 1 s sleep-polls, a 250 ms log flusher, and a watchdog that could never fire** — the log mirror, the wallpaper keeper and the hardware publish slept in 1 s slices "so a QUIT is not held up", but nothing waits for a helper (agent_run ends in ExitProcess); they now `agent_nap()`. The log flusher waits on an event `log_shutdown()` signals instead of waking 240 times a minute. The watchdog is not started in multiplex mode (every Win9x box), where `g_cmd_inflight` is never raised so it could not fire | `python/test_agent_idle_wakeups.py` (and `python/test_retrowall_theme.py::test_the_keeper_loop_naps_and_stops_with_the_agent`) |
| **share log mirror: 2 copies a minute forever, and a log line per copy** — `agent.log` and `agent.log.1` went to the share every 60 s whether or not a byte had changed (2,880 SMB copies a day per box), and each copy logged "mirrored", so the log always had changed. `log.c` now exposes a write counter and a rotation counter; `agent.log` is copied only when the write counter moved since the last SUCCESSFUL copy, `.1` only after a rotation, the first pass still uploads the previous run's log, a failure is retried with the interval doubling up to 8x, and only working<->failing transitions are logged | `native/test_sharelog.c` (TRUE-SOURCE `agent/shared/sharelog.h`), `python/test_sharelog_wiring.py` |
| **firewall step: two netsh processes before listen(), every boot, "added" regardless** — `ensure_firewall_exception()` ran `netsh firewall` and `netsh advfirewall` (up to 5 s each) before the agent listened, on 9x where there is no netsh, and advfirewall on XP where that context does not exist, logging "added" whenever netsh merely started. Now: skipped on 9x/NT4/2000, the existing exception is looked up in the registry first (XP's AuthorizedApplications list, Vista+'s FirewallRules), advfirewall only on major >= 6, run on a background helper after listen(), and the log carries netsh's exit code and whether the exception is present afterwards | `native/test_fwplan.c` (TRUE-SOURCE `agent/shared/fwplan.h`), `python/test_firewall_step.py` |
| **GAMEINDEX: a 6-65 s disk walk every 4 minutes, forever** — every directory to depth 3 cost ~68 `GetFileAttributesA` probes (one per signature) on top of the listing, C:\Games was walked twice, every shortcut was resolved through COM, unconditionally. Now one listing per directory feeds `gim_note()` (long AND 8.3 name, the two a path probe matched), no game root is descended twice, a cheap fingerprint every 15 min skips the pass when nothing an install touches changed, a full pass is forced hourly and on `gameindex_poke()` after a GAMESYNC that changed the box, the last index is kept in `C:\RETRO_AGENT\gameindex.cache` so a reboot answers at once, and `GAMEINDEX SCAN` is serialised with the background pass (they used to share `g_ents` unguarded) and lifts the IDLE scanner before waiting | `native/test_gimatch.c` (TRUE-SOURCE `agent/shared/gimatch.h`: equivalence with the old probes on 2,000 random listings, the pass decision, fingerprint, cache format), `python/test_gameindex_scan_cost.py` |
| **retrowall: the whole fleet desktop re-applied on every boot** — 29 colour writes + `SetSysColors` (every window repaints) + explicit `WM_SYSCOLORCHANGE`/`WM_THEMECHANGED` broadcasts + `SetSystemVisualStyle`, two screensaver `SPIF_SENDWININICHANGE` broadcasts, a wallpaper reload (6 MB BMP at 1080p) + broadcast, the Themes service rewritten to Disabled, and on XP a `cmd.exe`+`taskkill` spawn for a `rotate_wall.exe` renamed `.superseded` long ago. Each step now compares first (`GetSysColor` for every supported index, `IsThemeActive`, the service's `Start` value, `SPI_GETSCREENSAVE*`, the current `Wallpaper` value as the keeper does) and the kill is a Toolhelp walk matching the image's BASE name (9x's `szExeFile` is a full path); a settled box logs one "already the fleet's" line | `native/test_rwcompare.c` (TRUE-SOURCE `agent/shared/rwcompare.h`), `python/test_retrowall_compare_first.py` |
| **gamesync: the two tool shortcuts were rebuilt through COM on every start** — the comment promised "cheap no-ops when the shortcut already exists", but nothing checked; every start created a ShellLink, resolved and resaved "Retro Agent"/"Retro Chat" (and Explorer refreshed the desktop). `gs_tool_shortcut()` now reads the existing `.lnk` and returns when its NUL-terminated ANSI LocalBasePath is the exe (as given, short or long form); anything else is rebuilt as before. An untouched shortcut is not counted as a desktop change | `native/test_lnkcheck.c` (TRUE-SOURCE `agent/shared/lnkcheck.h`), `python/test_gamesync_efficiency.py` |
| **gamesync: three metadata calls per file, and the gate asked after the share walk** — the resume test (size AND mtime, v1.62.0) read the destination twice and asked the NAS for the source's time although `gs_copy_tree`'s listing had just returned it; now one `GetFileAttributesExA(dst)`, the listing's time, and the source is asked only when that disagrees (same verdicts - pinned on 20,000 random cases - and the one divergence is a file the old test re-copied forever). The sizing pass walked every title's tree on the share, including the ones the gate then refused (22 of 46 on `.243`); the gate is now asked first. The installed-copy disk credit walked every installed tree each sync; it is taken only when it can change the verdict. `test_gs_resume_mtime.c` now compiles the shared header instead of a "verbatim" copy | `native/test_gs_resume_enum.c`, `native/test_gs_resume_mtime.c` (both TRUE-SOURCE `agent/shared/gsresume.h`), `python/test_gamesync_efficiency.py` (and the updated `test_gamesync_enumeration.py`, `test_gs_copy_attrs.py`) |
| **startup: retrowall, GAMESYNC and GAMEINDEX all woke at t+20 s** — the theme broadcasts, the library/desktop work and a full disk walk landed together on a box still finishing its logon. Now 20 / 40 / 120 s (60 s for the index when it has no cache), and no two timed helpers (autoupdate 15, sharelog 10, dosstage 45, hwpublish 90) share a second | `python/test_agent_startup_stagger.py` |
