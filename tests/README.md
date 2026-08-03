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
../retro-3dfx/tests/      VINTAGE H5 / SGL harness — the .143 pure-3dfx lane, NOT our stack
  native/test_texheap_align.c, test_mip_download_addr.c ; test_source_invariants.sh ; predeploy.sh
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
| 0.1.2 SSE float→ubyte color clamp (`fx_pack_ub`) | MesaFX ICD | `native/test_fx_pack_ub.c` |
| transport XOR keystream (involution + derivation) | agent C (crypto.c) | `native/test_crypto.c` |
| discovery packet wire format | Python client | `test_discovery.py` |
| length-prefixed frame codec + status contract | Python client | `test_protocol.py` |
| DOSGAME installed-detection stem match + INSTLD.LST receipts (2026-08-03) | DOS lane (dosgame.c) | `python/test_dosgame_install_detect.py` |
| DOS net bring-up: guarded drivers, PKT.OK written, CHAT auto-calls NETUP | DOS lane (NETUP/PLAY/CHAT.BAT) | `python/test_dosgame_install_detect.py` |
| fleetbook add/search/log contract (brain's solved-problems DB) | scripts/retro_fleetbook.py | `python/test_fleetbook.py` |
| 0.1.34 fullscreen refresh snap-down (`fxBestRefresh`, was hardcoded 60Hz) | MesaFX ICD | `native/test_fx_best_refresh.c` |
| 0.1.35 fullscreen cursor overlay stamp/clip (`fxDrawCursorOverlay`) | MesaFX ICD | `native/test_fx_cursor_overlay.c` |

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
