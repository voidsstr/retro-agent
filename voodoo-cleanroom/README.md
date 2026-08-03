# retro3dfx — clean-room, open-source Voodoo driver stack

A complete, self-built driver stack for the **3dfx Voodoo 3 (Avenger)** on Windows
XP/2000, assembled from **genuinely open source** — 3dfx's 2000 GPL Glide release +
MIT Mesa — plus **our own clean-room code**. The point is *ownership*: every layer
is source we can read, fix, optimize, instrument, and ship — so per-game rendering
bugs, performance, and stability are ours to control instead of a black-box retail
binary. (Voodoo4/5 are in scope for the Glide/ICD but the display-driver work
targets the Voodoo3.)

> **New here? Read this whole file, then the [Driver Stack Map in CLAUDE.md](../CLAUDE.md)** —
> there are TWO 3dfx codebases in play (our clean-room set and a vintage-source
> lane) and conflating them wastes hours.

---

## 1. What it is — the three layers + current state

```
   Q3 / Q2 / RtCW / MOHAA / CS (OpenGL)        UT / Descent3 (native Glide)
            │                                          │
   [3] OpenGL ICD  (retrogl.dll / opengl32.dll)        │   MesaFX 6.2 fork → our code
            │  gr* calls                                │
            ▼                                           ▼
   [2] Glide3  (glide3x.dll, h3 build)  ◄──────────────┘   sezero/glide (3dfx GPL) fork
            │  MMIO / CMD-FIFO writes
            ▼
   [1] Display driver + Direct3D/DirectDraw HAL + miniport
            │      (2D desktop, mode-set, D3D games, DirectDraw games)
            ▼
        Voodoo 3 hardware
```

| Layer | Clean-room binary | Provenance | On .124 today |
|---|---|---|---|
| **[3] OpenGL ICD** | `retrogl.dll` / game-local `opengl32.dll` (0.1.33, ~2.75 MB) | `retro3dfx-gl` fork of `sezero/MesaFX-6.2` (Brian Paul, MIT) | ✅ **OURS, deployed** |
| **[2] Glide3** | `glide3x.dll` (h3, 787 KB) | `retro3dfx-glide` fork of `sezero/glide` (3dfx 2000 GPL) | ✅ **OURS, deployed** (system32 + game-local) |
| **[1] Display + D3D/DDraw HAL + miniport** | `fxd3ddd.dll` (our clean-room D3D driver) | `scripts/3dfx/` (OUR code, public DDK/DDI + register docs) | ❌ **still vintage** `3dfxv3d.dll`/`3dfxv3m.sys` (see below) |

**So the deployed stack on `.124` is a HYBRID:** our clean-room ICD + our clean-room
Glide, riding on the **vintage H5 display/D3D driver** (leaked/released 3dfx source,
built + hardened in the sibling `retro-3dfx` repo). Layer [1] is the **only** piece
that isn't ours yet. Making it all-open = deploying `fxd3ddd.dll` (**M4d**, not yet
done — see §6). Two clean-room tracks exist for layer [1]:

- **`scripts/3dfx/` — fxD3D / `fxd3ddd.dll`** (the fuller one): a real DX6/7 D3D HAL
  + DDraw + a kernel-mode Glide backend. **Code-complete through M4c-2, host-tested,
  links (45 KB), never yet activated on silicon.** This is the primary track.
- **`vcr-disp/`** (the minimal one, formerly `retro3dfx-disp`): a "cooperative"
  display driver whose only job is to answer the HWCEXT escape our Glide uses for
  hardware init. Escape server + BAR mapper written; mode-set/GDI chassis unwritten.
  A lighter fallback; fxD3D is ahead.

---

## 2. Repos / forks (provenance — full detail in `FORKS.md`)

| Component | Repo | Upstream / basis |
|---|---|---|
| OpenGL ICD | [`voidsstr/retro3dfx-gl`](https://github.com/voidsstr/retro3dfx-gl) | `sezero/MesaFX-6.2` (Mesa 6.2.2) |
| Glide | [`voidsstr/retro3dfx-glide`](https://github.com/voidsstr/retro3dfx-glide) | `sezero/glide` (3dfx 2000 GPL) |
| fxD3D display+D3D driver | `scripts/3dfx/` (this repo) | OUR code — public DX7 DDK DDI + Avenger register docs |
| Vintage H5 lane (reference + the deployed layer-1) | sibling `~/development/retro-3dfx` | 3dfx's leaked/released H5/Napalm driver source |

The fork clones live (gitignored) under `build/retro3dfx-gl` and
`build/retro3dfx-glide`; `build-stack.sh` clones them if absent.

---

## 3. Build

### ICD + Glide (mingw, no DDK needed)
```bash
cd voodoo-cleanroom
./build-stack.sh              # clones+builds the forks with i686-w64-mingw32 (gcc-13):
                              #   out/glide3x.dll        = COPY of the h3 build (deploy name)
                              #   out/glide3x_h3.dll     = Voodoo3 (SHIP THIS)
                              #   out/glide3x_h5.dll     = Voodoo4/5 (⚠ FAULTS at grGlideInit —
                              #                            lacks the 4 h3 bring-up fixes; DO NOT SHIP)
                              #   out/glide2x.dll, out/opengl32.dll, out/sdk/
./build-mesafx-retail.sh      # the RETAIL-ABI ICD relink → out/opengl32_retail.dll,
                              #   auto-bumps .buildnum → 0.1.N, stamps GL_RENDERER
                              #   "Mesa Glide v0.62 Voodoo3 (tm) [voodoo-cleanroom 0.1.N]"
```
Flags: `-O2 -ffast-math -march=pentium3 -mtune=pentium3 -mfpmath=sse`. Version =
`VERSION` (0.1) + `.buildnum`.

**Two ICD builds, two Glide ABIs — link the right pair or `LoadLibrary` fails silently
→ game falls back to MS software GL:** `out/opengl32.dll` links our fork's
`grFoo`/`grFoo@N`; `out/opengl32_retail.dll` links the retail `_grFoo@N` ABI. The
deployed .124 build is the **retail-ABI ICD** on top of our 787 KB glide (which
exports both). `build-mesafx-retail.sh` sanity-checks `_grBufferSwap@4` is imported.

> **glide h5/h3 NAMING TRAP (cost a full games-broken debug session):** `build-stack.sh`
> used to give the **h5** build the deploy name `out/glide3x.dll`. The h5 tree lacks
> the h3 bring-up fixes, so `grGlideInit` **hangs the Voodoo3** — and it drop-in
> replaced the good DLL (identical exports). The deploy name now always carries the
> **h3** build (see the NAMING-TRAP note in `build-stack.sh`; regression test
> `../tests/python/test_glide_artifact_naming.py`).

### fxD3D display driver (needs the DDK)
`fxd3ddd.dll` is a native GDI_DRIVER — it must be built by the **W2K DDK `build`**
(PE subsystem NATIVE, imports resolve only against `WIN32K.SYS`, `/Gz` __stdcall),
never cross-built with mingw/msvcrt (a mis-subsystem image silently fails to load).
Two ways:
- **Wine-hosted DDK loop** (dev host): the harness at
  `~/development/retro-3dfx/toolchain-3dfx/prefix/drive_c/clfxd3d.bat` (rsync repo →
  Wine W2K+DX7 DDK → cl/link → `fxd3ddd.dll`). New `.c` → add to `clfxd3d.bat` AND
  `driver/nt/SOURCES`; `-Fo` must be glued (no space); success prints `LINKEXIT=0`.
  **38 GB-log hazard** — the build script is guarded; never redirect unbounded build
  output to a file.
- **On-box DDK** (a fleet box builds it itself):
  `provisioning/ddk/provision_ddk.py <ip>` once, then `build_driver.py <ip>`.

The **DDK-independent** fxD3D core (DP2 parser, D3D→Glide state/tex/prim, the
kernel-Glide backend logic) builds + unit-tests with plain gcc:
```bash
make -C scripts/3dfx test     # host unit tests, no hardware, <1s
```

---

## 4. Deploy

### ICD + Glide (just DLLs, no PnP)
`opengl32` is **not a KnownDLL** on `.124`, so a **game-local `opengl32.dll` wins**.
Deploy per game under the name the game loads (this is the crux that made each game
work — full matrix in [`../docs/game-render-modes.md`](../docs/game-render-modes.md)):
- **Q3 / RtCW / MOHAA** (idTech3): `retrogl.dll` + `r_glDriver retrogl` (or MOHAA's
  game-local `opengl32.dll`).
- **Q2 / Heretic2 / SiN** (idTech2 `ref_gl`): stage as `3dfxgl.dll`, `+set gl_driver 3dfxgl`.
- **CS / HL** (GoldSrc): game-local **`opengl32.dll`** (GoldSrc imports opengl32 directly).
- Always drop the **787 KB h3 `glide3x.dll`** in the game dir too.

### fxD3D display driver (M4d — not yet executed)
`3dfxv3d.dll`-class names are **WFP-untracked** (no dllcache twin) so no signing is
needed, but the **load selector is the PnP Display-CLASS instance key**, not the
other two aliases people try:
```
HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\NNNN
   InstalledDisplayDrivers = fxd3ddd   (REG_MULTI_SZ, base name, keep Service=3dfxvs)
```
Editing `Services\…\Device0` or `Control\Video\{GUID}\NNNN` does **nothing** (nothing
reads them — that's why attempt #1's edits "never reverted"). The durable path is a
**proper Display INF** installed via `updrv.exe` — see the DEPLOYMENT CONTRACT block
in `scripts/3dfx/driver/nt/chassis.c` (the current `fxd3d.inf` is CopyFiles-only +
mis-decorates its services section — that's a known gap). **Never reboot a fleet box
without user approval.** The `deploy-3dfx-driver` skill automates the safe path.

---

## 5. Debug

- **`C:\retrogl.log` ICD tracer** (`build/retro3dfx-gl/src/mesa/drivers/glide/fxrlog.h`):
  crash-safe per-line trace of the whole context-creation path — pixel-format,
  `wglCreateContext`, and the **`grSstWinOpen` args/return** (the usual failure point),
  plus a **DllMain PROCESS_ATTACH** line that distinguishes "app never loaded our ICD"
  from "loaded but failed before any GL call" (GoldSrc resolves exports via kernel32
  `GetProcAddress`, invisible to the per-call tracer). This tracer is what root-caused
  the entire games bring-up.
- **fxD3D escape ladder** (`scripts/3dfx/driver/nt/fxdbg/`, opcodes
  `FXDBG_PROBE/CLEAR/TRI/TEX/READBACK` via `ExtEscape`): the on-card bring-up tool for
  M4d — probe the driver, clear, draw a triangle, upload a texture, read back pixels,
  **without a full game**. `QUERYESCSUPPORT=0` means fxd3ddd isn't the active driver.
- **Tests / gates** (all native, <1s, no hardware): `make -C scripts/3dfx test`
  (D3D DP2 + gbkernel pure logic + surface math), `bash tests/run_all.sh` (our stack),
  and in the sibling repo `retro-3dfx/tests/predeploy.sh` (**run before deploying any
  vintage driver binary — non-zero = do NOT deploy**) + `codegen_8e_guards.py` (asserts
  the two 0x8E BSOD fixes are in the built driver's machine code).
- **BSOD analysis without WinDbg:** XP small dumps (`D:\WINDOWS\Minidump\*.dmp` — Windows
  is on **D:** on this dual-boot box) carry the bugcheck at file offset `0x28` (code +
  exception + faulting EIP + context). Map EIP→function via the driver's load base
  (from the dump's module list) + `objdump -d`. This is how the two vintage-HAL 0x8E
  NULL-derefs were found. Full method: `retro-3dfx/FINDINGS.md`.
- **Capture gotcha:** a GDI `SCREENSHOT` of a **Glide fullscreen** surface is
  garbage/black (it reads the desktop framebuffer, not the 3D overlay). Prove rendering
  with the in-engine `GL_RENDERER` string / a timedemo / the ICD log — never a screenshot.
  A DirectDraw-wrapper game (RA2) *is* GDI-capturable.
- **Fullscreen wedge / recovery:** a fullscreen Glide or D3D app can wedge `.124`'s
  network for minutes (recovers when the app dies); rarely it hard-hangs and needs a
  power-cycle (auto-login + agent bring it back). Always test fullscreen via a
  **self-killing on-box batch** (`setmode 16 → launch → ping-wait → taskkill → setmode 32`)
  + a queued `retro_enqueue.py <ip> "EXEC ...taskkill"` recovery net.

---

## 6. Modify — the change loop

1. **Which stack?** ICD/Glide → the forks under `build/`. Display/D3D → `scripts/3dfx/`
   (clean-room) or `../../retro-3dfx` (vintage). Get this right first (Stack Map in CLAUDE.md).
2. **Edit + build:** ICD → `build-mesafx-retail.sh` (auto-bumps `0.1.N`, stamps
   `GL_RENDERER` so every log self-documents). fxD3D → the DDK loop; keep
   `make -C scripts/3dfx test` green.
3. **Test:** host tests + (for the vintage lane) `predeploy.sh`. **When a fix is
   verified on hardware, add a regression test in the SAME change** — a source assertion,
   a built-artifact/codegen marker, and/or an on-card `d3dlab`/`fxdbg` golden. A fix
   without a test isn't done (`tests/README.md`, `retro-3dfx/tests/`).
4. **Deploy** (§4) → verify on hardware (`GL_RENDERER` / retrogl.log / escape probe).
5. **Document:** update `CHANGELOG.md` (ICD versions), `FINDINGS.md` (hard-won gotchas),
   and this stack's docs.

---

## 7. Status & remaining work (2026-08)

**Working + stable on `.124`:** Q3, Q2, RtCW, MOHAA, CS (OpenGL + D3D), RA2, UT
(Glide) — all on our clean-room ICD + Glide over the (now BSOD-fixed) vintage HAL.
The two vintage-HAL 0x8E BSODs are fixed and the fix is deployed + verified (zero
crashes since). Refresh-rate persistence (`../agent/tools/setrefresh.c`) + games ×
renderer matrix are documented.

**Remaining (the honest list — full detail from the repo surveys):**
- **Completeness / all-open:** deploy `fxd3ddd.dll` (M4d) to retire the vintage layer-1
  — blocked on a proper Display INF, the Class-key install, stock-miniport pairing, and
  on-card validation of the kernel-Glide backend (never run on silicon).
- **Stability:** the fullscreen hard-wedge (Glide/ICD path, needs a supervised repro);
  the D3D-fullscreen network wedge (real fix = clean-room HAL reusing Glide's fullscreen);
  clean-room-glide `vid_restart` + fullscreen edge crashes; a few residual raw spins in
  the vintage HAL.
- **Quality:** `texture_env_combine` disabled on V3 (lightmap/overbright games render
  dark) — biggest visible gap; UT OpenGL depth flicker (worked around via Glide; real fix
  = UTGLR renderer); ARB pixel-format stubs; S3TC/DXT unavailable on V3.
- **Hardware ceilings (not fixable):** 16-bit-only 3D + Z (Avenger).

---

## Pointers

- [`CHANGELOG.md`](CHANGELOG.md) — ICD version-by-version changes + the swap-interval saga
- [`DEBUGGING-NOTES.md`](DEBUGGING-NOTES.md) — the long-form ICD/Glide debugging trail
- [`FORKS.md`](FORKS.md) — fork provenance + licenses
- [`RUNNING-GAMES.md`](RUNNING-GAMES.md) — per-game deploy recipes + agent gotchas
- [`REVIEW-FINDINGS.md`](REVIEW-FINDINGS.md) / [`OPTIMIZATIONS.md`](OPTIMIZATIONS.md) — ICD review + opt research
- [`vcr-disp/README.md`](vcr-disp/README.md) — the minimal clean-room display-driver track
- [`../scripts/3dfx/README.md`](../scripts/3dfx/README.md) — fxD3D clean-room D3D driver (build/test)
- [`../docs/3dfx-d3d-hal-design.md`](../docs/3dfx-d3d-hal-design.md) + [`../docs/3dfx-gbkernel-design.md`](../docs/3dfx-gbkernel-design.md) — fxD3D + kernel-Glide design
- [`../docs/game-render-modes.md`](../docs/game-render-modes.md) — game × renderer matrix + results
- [`../CLAUDE.md`](../CLAUDE.md) — the Driver Stack Map (READ FIRST) + fleet ops
- `~/development/retro-3dfx/` — sibling repo: vintage H5 source, the DDK toolchain, `FINDINGS.md`, `VINTAGE-FIXES.md`
