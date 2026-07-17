# retro3dfx fork provenance

Our forks are real GitHub forks under `voidsstr`. We commit our modernization +
optimization changes to the forks; `build-stack.sh` clones them and builds.

| Our fork | Upstream | Fork point |
|---|---|---|
| [voidsstr/retro3dfx-glide](https://github.com/voidsstr/retro3dfx-glide) | [sezero/glide](https://github.com/sezero/glide) | `ee38094805f778566cc752c6d854f058253234de` |
| [voidsstr/retro3dfx-gl](https://github.com/voidsstr/retro3dfx-gl) | [sezero/MesaFX-6.2](https://github.com/sezero/MesaFX-6.2) | fork head |

## Our changes so far (committed to the forks)
- **retro3dfx-glide**: build via mingw cross-toolchain; ABI fix so `glide3x.dll`
  loads (export both `grFoo` and `grFoo@N`; import lib without `-U`); host-tool
  P6FENCE portability for 64-bit build hosts. (Applied by `build-stack.sh`.)
- **retro3dfx-gl (MesaFX)**: build on modern gcc 13 — drop `-Werror`, add
  `-fcommon`, `-Wno-array-bounds`; cross-toolchain (`CC/AR/DLLTOOL/RC`).

## Licenses (upstream, preserved)
- Glide: 3dfx Glide Source Code General Public License (genuine 2000 open release).
- MesaFX: MIT/Mesa license (Brian Paul et al.).
Both are open and redistributable; our forks preserve the upstream license files.

## Not forks — written by us
- **retro3dfx-disp** (the display driver) is original code, *modeled on* the open
  Device3Dfx (Linux kernel driver), RISCyVoodoo (NT), and vmdisp9x (9x) — read for
  structure, not copied.
