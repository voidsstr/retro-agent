# vcr-disp-h5 — vintage H5 display driver (stopgap for the clean-room stack)

The `voodoo-cleanroom` set's **display layer**, used until the clean-room
`vcr-disp` (our original cooperative driver) is finished (`vcr-disp` still needs
`disp_modeset.c` + `disp_enable.c` written — see `../vcr-disp/README.md`).

## What this is — and what it is NOT

This is **3dfx's own H5/Napalm display-driver source**, *vendored from the
sibling `retro-3dfx` repo* (3dfx Driver Code/H5/W2K/Src/Video/{Displays,Miniport}/H5).
It is the **vintage** driver — leaked 3dfx source we compile — **not** clean-room
open source. It's here so the `voodoo-cleanroom` stack is self-contained and
deployable (it provides the full D3D/DDraw HAL + 2D + mode-set that `vcr-disp`
does not yet have).

- `src/Displays-H5`, `src/Miniport-H5` — vendored source snapshot (provenance:
  `retro-3dfx`, the vintage lane owned by the other session — see the shared
  memory `driver-lane-division`). The **authoritative** copy still lives in
  `retro-3dfx`; that lane maintains it. Re-vendor from there if it changes.
- `dist/` — the already-built, deployable WFP-safe package: `3dfxv3d.dll`
  (renamed display driver), `3dfxv3m.sys` (miniport), `voodoo3-wfp.inf`,
  `updrv.exe`. This is what deploys to `.124`.

## Build

Needs the Wine/VC6 W2K-DDK toolchain in `retro-3dfx/toolchain-3dfx/`; it is NOT
buildable from this vendored source alone. To rebuild, use the `retro-3dfx` tree
(the vintage lane) and copy the resulting WFP package into `dist/` here.

## Deploy (to .124)

PnP/WFP-rename install: `updrv.exe voodoo3-wfp.inf "PCI\VEN_121A&DEV_0005"` — see
the `deploy-3dfx-driver` skill. The renamed names (`3dfxv3d.dll`/`3dfxv3m.sys`)
are WFP-safe (non-catalogued).

## Retirement

When `vcr-disp` gains mode-set + the 2D GDI chassis (and a D3D HAL, or we accept
Glide/OpenGL-only), this whole directory goes away and the stack becomes fully
clean-room open source.
