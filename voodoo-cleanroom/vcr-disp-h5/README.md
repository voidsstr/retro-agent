# vcr-disp-h5 — a borrowed vintage display driver (stopgap, Voodoo 3 only)

> The whole stack is documented on one page: [`../README.md`](../README.md)
> ([§7.3](../README.md#73-vcr-disp-h5--the-stopgap)).

**This is not clean-room code.** It is a prebuilt package of 3dfx's own H5
display driver, built in the sibling `retro-3dfx` repo (the vintage lane), kept
here so the Voodoo 3 stack could be deployed from one place while our own
display driver (`../vcr-disp/`, `../../scripts/3dfx/` fxD3D) is unfinished.

## Contents (`dist/`)

| File | Bytes | md5 | What |
|---|---|---|---|
| `3dfxv3d.dll` | 957,456 | `e7cd7218…` | H5 display driver, renamed so Windows File Protection leaves it alone |
| `3dfxv3m.sys` | 199,644 | `7565c855…` | H5 miniport |
| `voodoo3-wfp.inf` | 24,269 | `5c76fc70…` | Installs the pair as service `3dfxvs`, `InstalledDisplayDrivers=3dfxv3d` |
| `updrv.exe` | 5,632 | `1741d1ff…` | SetupAPI installer (`agent/tools/updrv.c`) |

Byte-identical to `retro-3dfx/toolchain-3dfx/dist/3dfx-voodoo3-wfp-20260722/`.
The source is **not** vendored here (an old version of this README said
`src/` was; that directory is gitignored and absent). Rebuild in `retro-3dfx`.

## Limits

- **Voodoo 3 only.** The INF lists 13 `DEV_0005` subsystem IDs and no
  `DEV_0009`, so it cannot install on a Voodoo 4/5 — and there is no Voodoo 3 in
  the fleet since 2026-08-11.
- **It is the pre-fix build.** A BSOD 0x8E traced to the vintage `3dfxv3d.dll`
  was fixed and deployed in `retro-3dfx` (2026-07-28 → 08-03), but this copy
  was never refreshed. Re-vendor from `retro-3dfx` before using it again.

## Install (when a Voodoo 3 returns)

`updrv.exe voodoo3-wfp.inf "PCI\VEN_121A&DEV_0005"` — or, better, the
`deploy-3dfx-driver` skill, which does the backup, activation check, safe
reboot and rollback.

## Retirement

This directory goes away when fxD3D (or `vcr-disp`) can drive the card.
