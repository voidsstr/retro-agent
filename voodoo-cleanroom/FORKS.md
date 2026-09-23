# Fork provenance

Our two forks are real GitHub forks under `voidsstr`. `build-stack.sh` clones
them into `build/` (gitignored) and builds them; the rest of our work lives in
this repo. The full picture is in [`README.md`](README.md) — §5, §6 and §15.

| Our fork | Upstream | Fork point | Our branch / head |
|---|---|---|---|
| [voidsstr/retro3dfx-gl](https://github.com/voidsstr/retro3dfx-gl) | [sezero/MesaFX-6.2](https://github.com/sezero/MesaFX-6.2) | sezero's last commit `fd191eb` (2023-02-02) | `master` at `492a0d8` (ICD 0.1.33) |
| [voidsstr/retro3dfx-glide](https://github.com/voidsstr/retro3dfx-glide) | [sezero/glide](https://github.com/sezero/glide) | `ee38094805f778566cc752c6d854f058253234de` | `glide-devel-sezero` at `a71eb3f` |

## Where our changes live

| Change set | Lives in |
|---|---|
| ICD 0.1.1 → 0.1.33 (codegen, batching, state shadow, swap interval, LOD bias, gamma/dither/alpha formats, windowed Glide, keep-Glide-alive, `retrogl.log` tracer) | fork commits `8da4e93` … `492a0d8` |
| ICD 0.1.41 → 0.1.60 (Voodoo 2 lane: SGIS shim, `wglGetProcAddress` order, `point_parameters` withdrawn, `-static-libgcc`, `grTexCombine` shadow, `FX_PROFILE`) | **`patches/mesafx-voodoo2-icd.patch`** in this repo, applied to the clone by `build-stack.sh` |
| ICD version stamp (`[voodoo-cleanroom 0.1.N]`) | injected at build time by `build-mesafx-retail.sh` |
| Glide: P6FENCE; h3 zero-base guard, GETLINEARADDR prime, TLS accessor + lost-context + `grGetString` guard | fork commits `d378d44`, `033912b`, `2387787`, `8b6eb5f`, `a73a159`, `a71eb3f` |
| Glide: four-form exports (`grFoo`, `grFoo@N`, `_grFoo@N`, `_grFoo`), `-static-libgcc`, P3/SSE flags, lane naming | `build-stack.sh` in this repo (applied at build time) |
| Glide h5 ports of the h3 fixes | `patches/h5-bringup-wip.patch` — **not applied by anything** |
| Glide h5 `hwcMapBoard` zero-base guard | an **uncommitted** edit in the local clone (2026-09-12) |

**Lost** (never committed or pushed, gone after the clone was re-created on
2026-09-04): ICD 0.1.34 (monitor-max refresh), ICD 0.1.35 (software cursor,
`FX_DUMP_FRONT`) and glide2x fork commit `79ee51e` (XP bring-up guards). See
README §15.4. The rule since: a fork change is not done until it is pushed or
captured under `patches/`.

## Licenses (upstream, preserved)

- **Glide:** 3dfx Glide Source Code General Public License — the genuine 2000
  open release.
- **MesaFX:** MIT / Mesa license (Brian Paul et al.).

Both allow redistribution; the forks keep the upstream license files.

## Not forks — written by us

- **`vcr-disp/`** and **fxD3D (`../scripts/3dfx/`)** are original code,
  *modelled on* the open Device3Dfx (Linux kernel driver), RISCyVoodoo (NT) and
  vmdisp9x (9x) — read for structure, not copied.
- **`vcr-disp-h5/dist/`** is *not* ours and not clean-room: a prebuilt vintage
  H5 driver package from the `retro-3dfx` lane, kept as a stopgap.
