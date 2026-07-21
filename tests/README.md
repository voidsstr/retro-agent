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
  native/
    munit.h               tiny single-header C test framework
    stubs/windows.h       lets agent C compile natively (funcs use no Win32 API)
    test_crypto.c         TRUE-SOURCE: compiles agent/src/crypto.c, XOR keystream
retro-3dfx/tests/         (display-driver harness — see its own README)
  run_native.sh           builds + runs every native/test_*.c
  native/
    test_texheap_align.c  garble fix 0.3.1  (16-byte texture-heap alignment)
    test_palette_stride.c CS palette fix 0.3.2 (RGBA stride-4)
    test_cook_subtexture.c subtexture OOB fix 0.2.1 (full-width row stride, *bpp skip)
    test_res_cap.c        black-world fix 0.3.6 (modern-platform -1 cap-gate bypass)
  test_source_invariants.sh  grep-presence checks for display-driver/HAL fixes
  predeploy.sh            pre-deploy gate (invariants + native + built-artifact)
```

`run_all.sh` also invokes the retro-3dfx driver harness, so one command covers
the whole stack.

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

| Fix | Component | Test |
|-----|-----------|------|
| 0.3.1 2D text garble (16-byte tex-heap alignment) | MesaFX ICD | `retro-3dfx native/test_texheap_align.c` |
| 0.3.2 CS palette colors (RGBA stride-4) | MesaFX ICD | `retro-3dfx native/test_palette_stride.c` |
| 0.2.1 glTexSubImage2D OOB (full-width stride, *bpp skip) | MesaFX ICD (GLCORE) | `retro-3dfx native/test_cook_subtexture.c` |
| 0.3.6 black world at 800/1024 (platform-classify -1 sentinel) | MesaFX ICD | `retro-3dfx native/test_res_cap.c` |
| transport XOR keystream (involution + derivation) | agent C (crypto.c) | `native/test_crypto.c` |
| discovery packet wire format | Python client | `test_discovery.py` |
| length-prefixed frame codec + status contract | Python client | `test_protocol.py` |

**Backlog** (catalogued, not yet wired — see retro-3dfx `FINDINGS.md` /
`optimized/CHANGELOG.md` and the fix catalog): ICD pure-logic — 0.2.2 NULL-cache
guard, element_size default, 0.1.2 SSE float→ubyte clamp, 0.1.6 swap-interval env
default, 0.1.30 alpha-PFD matcher; agent-C — `handle_execw` timeout clamp,
`discovery_build_packet` ⇄ Python `from_packet` round-trip, `util.c` json/hex
helpers; render (CSIM track) — green-world 2PPC, filters, mip offset; provisioning
— P3 no-SSE2 opcode scan of staged DLLs.

## Adding a test when a fix is verified

1. Identify the invariant the fix establishes (the exact value/relationship that
   was wrong before and is right after).
2. Add `native/test_<fix>.c` (cite source file:function + fix version in the
   header comment) or a `python/test_*.py` case. Assert both the fixed value AND
   the old buggy value (documents the failure mode).
3. `bash tests/run_all.sh` must stay green.
4. Add a row to the table above and, if the fix is a milestone, a line in
   `CLAUDE.md`.
