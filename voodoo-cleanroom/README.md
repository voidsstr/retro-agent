# retro3dfx — an open, self-built Voodoo driver stack

Run **Quake 3** (OpenGL) and **Unreal Tournament** (Glide) on real Voodoo 3/4/5
hardware with **every layer from the kernel display driver up to OpenGL built by
us from source** — so the whole stack can be optimized past what 3dfx shipped.

**Status (2026-07-17): done and faster.** The full self-built stack (kernel
display driver + glide3x + our MesaFX ICD) replaced the AmigaMerlin driver on
the real Voodoo3/XP box (`.124`) and beat it — Q3 `timedemo four` 16bpp:
**58.8 fps @640x480, 51.3 @1024x768** vs the untuned AmigaMerlin stack's
53.7 / 38.7. The 1024x768 number also beats the era-reference 3dfx official
ICD result on a Voodoo3 3000 (44.3).

## The three layers

```
   Quake 3 (OpenGL)              Unreal Tournament (Glide)
        │                             │
   [3] OpenGL ICD ────────────────┐  │    retro3dfx-gl (MesaFX 6.2 fork)
        │  gr* calls              │  │    → opengl32.dll / retrogl.dll
        ▼                         ▼  ▼
   [2] glide3x.dll                        private-repo build (ships in the XP
        │  register / FIFO writes         package) or retro3dfx-glide fork
        ▼
   [1] XP kernel display driver           3dfxvsm.sys + 3dfxvs.dll (built in the
        │                                 private retro-3dfx repo); retro3dfx-disp
        ▼                                 is the clean-room track in THIS repo
   Voodoo 3 / Voodoo 5 hardware
```

The two lower layers have two implementations each: a **deployed build produced
in the private `retro-3dfx` repo** (kept separate from this public repo), and an
**open fork we optimize here**. The optimization work in this repo — the OpenGL
ICD and the Glide fork — is built entirely from **open upstreams** (see
`FORKS.md`).

**[1] Kernel display driver** — two implementations:

- **Deployed build** (the one benchmarked): `3dfxvsm.sys` (miniport) +
  `3dfxvs.dll` (XPDM display driver incl. D3D HAL), produced in the sibling
  **private** `retro-3dfx` repo with its Wine-hosted VC6/W2K-DDK toolchain. The
  W2K free build is the XP path (same as AmigaMerlin/SFFT did). Its source and
  toolchain are not part of this public repo.
- **retro3dfx-disp** (`retro3dfx-disp/`) — **the open track in this repo**: our
  original clean-room "cooperative" display driver whose job is to answer the
  HWCEXT escape protocol our Glide fork uses for hardware init. Escape server +
  BAR mapper written and host-tested; modeset/GDI chassis still to write. See its
  README.

**[2] glide3x.dll** — two builds:

- **Deployed glide3x** (on `.124` as part of the XP package): produced in the
  private `retro-3dfx` repo with era MSVC; 96 exports, export list identical to
  the vintage Nov-2000 DLL, retail `_grFoo@N` ABI.
- **retro3dfx-glide** ([voidsstr/retro3dfx-glide](https://github.com/voidsstr/retro3dfx-glide),
  fork of sezero/glide): our gcc-13 cross-built Glide2x/Glide3x for h3 (Voodoo3)
  and h5 (Voodoo4/5) — the optimization vehicle. Its NT hardware init needs a
  display driver that answers HWCEXT (retro3dfx-disp's purpose); on a box with
  only a retail-lineage display driver it hangs at hw init.

**[3] OpenGL ICD** — **retro3dfx-gl**
([voidsstr/retro3dfx-gl](https://github.com/voidsstr/retro3dfx-gl), fork of
sezero/MesaFX-6.2): Mesa 6.2.2 OpenGL-over-Glide3, deployed as the game's
`r_glDriver` DLL (`retrogl.dll`). This is where our performance work lives
(see `CHANGELOG.md`). Fork provenance and licenses: `FORKS.md`.

## ABI: which ICD build binds which glide3x

The Glide entry-point decoration differs between builds, and an ICD linked
against the wrong import lib fails `LoadLibrary` → the game silently falls back
to Microsoft's "Direct3D GL 1.1" software wrapper.

| glide3x.dll | Exports | ICD build that binds it |
|---|---|---|
| retro3dfx-glide (our fork) | `grFoo` **and** `grFoo@N` (no underscore) | `out/opengl32.dll` — default `build-stack.sh` link |
| deployed build (XP package) | `_grFoo@N` (retail MSVC decoration) | `out/opengl32_retail.dll` — `build-mesafx-retail.sh` |
| AmigaMerlin / other retail packs | `_grFoo@N` | `out/opengl32_retail.dll` |

The deployed stack on `.124` runs the **retail-ABI** ICD build on top of the
deployed glide3x. `build-mesafx-retail.sh` sanity-checks the output imports
`_grBufferSwap@4` before declaring success.

## Build system

- **`./build-stack.sh`** — clones + builds our forks with the
  `i686-w64-mingw32` cross toolchain → `out/glide3x.dll` (h5),
  `out/glide3x_h3.dll` (Voodoo3), `out/glide2x.dll`, `out/opengl32.dll`
  (MesaFX linked against **our** glide import lib), plus the shared Glide3 SDK
  in `out/sdk/`. `--debug` for a GDBG tracing glide.
- **`./build-mesafx-retail.sh`** — the retail-ABI MesaFX relink (against
  `scripts/3dfx/glide-sdk/lib/libglide3x_retail.dll.a`). Run `build-stack.sh`
  once first (headers). Also owns **driver versioning**: `VERSION`
  (MAJOR.MINOR) + auto-incrementing `.buildnum` → `0.1.N`, injected into
  `GL_RENDERER` (`Mesa Glide v0.62 Voodoo3 (tm) [retro3dfx 0.1.N]`) so every
  game log and benchmark self-documents the build. Outputs
  `out/opengl32_retail.dll` + versioned archive + `.ver` sidecar.
- **Kernel driver** — built in the sibling repo:
  `~/development/retro-3dfx/toolchain-3dfx/` (portable Wine + VC6 SP5 + MASM
  6.14 + W2K DDK). `package_driver.sh` assembles a deployable
  `dist/3dfx-napalm-xp-<ver>/` package. Read that repo's
  `toolchain-3dfx/README.md` before touching it (Wine gotchas, incl. the
  38 GB-log hazard).

## Deploy (XP)

The XP driver package (`dist/3dfx-napalm-xp-<ver>/` in retro-3dfx) contains
`3dfxvsm.sys`, `3dfxvs.dll`, `glide3x.dll`, `fxoem2x.dll`, trimmed
`voodoo3.inf`/`voodoo5.inf`, `updrv.exe` (SetupAPI helper), and `INSTALL.bat`
(backup + signing policy + install). Deploy with the **`deploy-3dfx-driver`
skill** (`.claude/skills/deploy-3dfx-driver/SKILL.md`) — preflight HWID check,
staged upload, backup, SetupAPI install, verify. Never raw-copy into
`system32` (WFP reverts); never reboot without user approval.

Rollback nets, in order: XP Device Manager **Roll Back Driver** (the in-box
`3dfxvs2k.inf` driver stays in the store), F8 **Last Known Good**, or restore
from the `INSTALL.bat` backup dir + reg export taken during deploy.

The ICD deploys separately (it's just a DLL, no PnP): copy
`out/opengl32_retail.dll` to the game dir **and** `system32` as `retrogl.dll`,
launch with `+set r_glDriver retrogl`. See `RUNNING-GAMES.md` for the exact Q3
recipe and the agent gotchas (`EXEC`+`start`, not `LAUNCH`; trust
`qconsole.log`, not GDI screenshots).

## Status on `.124` (Voodoo3 AGP, XP SP3) — as of 2026-07-17

| Layer | Live binary | Built from | Self-built? |
|---|---|---|---|
| Kernel display driver | `3dfxvsm.sys` + `3dfxvs.dll` (XP package) | private retro-3dfx repo | **yes** |
| glide3x.dll | deployed build (96 exports, retail ABI) | private retro-3dfx repo | **yes** |
| OpenGL ICD (`retrogl.dll`) | `opengl32_retail.dll`, retro3dfx 0.1.6 | retro3dfx-gl fork (open) | **yes** |

Benchmark standing (Q3 1.32 `timedemo four`, 16bpp, P3-845):

| Stack | 640x480 | 1024x768 |
|---|---|---|
| **ALL-RETRO3DFX** (2026-07-17 milestone) | **58.8** | **51.3** |
| AmigaMerlin hybrid, untuned env (0.1.1 baseline) | 53.7 | 38.7 |
| AmigaMerlin hybrid + `FX_GLIDE_SWAPINTERVAL=0` | ~58 | ~51 |
| Era references (P3-850/933 + V3 3000, 3dfx official ICD) | 75–91 | 44.3 |

1024x768 (fillrate-bound) beats both AmigaMerlin and the era reference. The
remaining 640x480 gap to era numbers is CPU-side T&L in the ICD — the queued
deep work (SSE vertex emit, SSE 4-wide cliptest, end-to-end ubyte colors).

## Pointers

- `CHANGELOG.md` — version-by-version ICD changes, results, and the
  swap-interval saga (read before touching vsync behavior).
- `RUNNING-GAMES.md` — per-game deploy recipes + agent gotchas.
- `FORKS.md` — fork provenance and licenses.
- `retro3dfx-disp/README.md` — the clean-room display-driver track.
- `../benchmarks/README.md` — benchmarking process, result format, DB schema.
- `~/development/retro-3dfx/` — private sibling repo: the kernel-driver source
  tree + `toolchain-3dfx/` (kernel-driver builds + dist packages), kept out of
  this public repo.
