# DDK toolchain on the fleet — build drivers on the retro boxes themselves

This turns any fleet Windows box (XP/2000/2003) into a **driver-build machine**,
deployed entirely over the retro-agent. It's the "fleet builds its own drivers"
path: the box gets the Windows DDK, then compiles the fxD3D host display driver
(`scripts/3dfx/driver/`) on-device — no local Windows toolchain required.

```
provisioning/ddk/
  provision_ddk.py   deploy + configure the DDK on a target box (idempotent)
  build_driver.py    package fxD3D sources -> upload -> `build` remotely -> retrieve
  ddk_setenv.bat     DDK build-environment wrapper (-> C:\DDK on the box)
  build_fxd3d.bat    the build recipe run on the box (-> C:\build\out\)
  README.md          this file
```

## One-time: stage the DDK on the share

The Microsoft DDK is not redistributed in this repo (it's Microsoft's), so —
exactly like the game ZIPs for onboarding — you stage it on the share once:

- **Which DDK:** the **Windows Server 2003 DDK, build 3790.1830**. It's the
  canonical, freely-available DDK, includes `build.exe` + the display-driver
  headers (`winddi.h`, `ddrawint.h`, `d3dhal.h`) + libs (`win32k.lib`), and
  targets **Windows XP** display drivers (`setenv ... WXP`). Archived widely
  (e.g. archive.org "Windows Server 2003 DDK 3790"); it was a free download from
  Microsoft.
- **Make the package:** install/extract the DDK so you have a `3790\` tree with
  `3790\bin\build.exe`, then ZIP it so it extracts to `C:\WINDDK\3790\...`:
  ```
  # the zip's top-level entry must be  3790\  (so it lands at C:\WINDDK\3790)
  zip -r winddk-3790.zip 3790
  ```
- **Put it on the share:**
  `\\192.168.1.122\files\Utility\Retro Automation\DDK\winddk-3790.zip`

(You can trim the package to just `bin\`, `inc\`, `lib\` to shrink it — those are
all `build.exe` needs for our driver.)

## Provision a box

```bash
python3 provision_ddk.py <target-ip>
```
Idempotent. It maps the share, copies the DDK ZIP local (`copy /Y`, **not**
`xcopy` — xcopy hangs on NETMAP'd SMB on XP), extracts it to `C:\WINDDK\3790`
with the JScript unzip shim, drops the env wrappers into `C:\DDK`, and verifies
`build.exe` runs. Skips the extract if the DDK is already there (`--force` to
redo).

Pick a **capable box** to be the builder — the DDK runs on XP/2000/2003. One
provisioned box can build drivers for the whole fleet.

## Build the fxD3D driver on that box

```bash
python3 build_driver.py <target-ip>            # checked build (default)
python3 build_driver.py <target-ip> --bld fre  # free/release build
```
It packages the exact source subset (preserving the `driver/ d3dhal/ glide-sdk/`
layout the `SOURCES` relative paths need), uploads + extracts it, runs
`C:\DDK\build_fxd3d.bat` (which calls the DDK `build`), and downloads
`fxd3ddd.dll` to `provisioning/ddk/out/`. This compiles the **`-DHAVE_DDK`** path
of `driver/nt/enable.c` — the same portable core + glue already verified on the
Linux host, now inside a real display driver.

## After a successful build

1. Stage `fxd3ddd.dll` (+ the miniport when added) and `scripts/3dfx/driver/
   fxd3d.inf` on the share.
2. Install on a Voodoo box **via PnP** ("Update Driver" / have-disk) — never
   hand-write `Display\0000` (VSA-100 needs full PnP context; see CLAUDE.md).
3. Bring up in **86Box** first, then the real card, driving the screenshot loop
   over the agent.

## Notes / scope

- **XP/2003 path** (this toolchain) covers the fleet's XP boxes and the `nt/`
  driver. The **Win98 driver** (`driver/win9x/`) builds with the **98 DDK +
  Open Watcom** instead — a separate package; add `provision_ddk.py --zip
  win98ddk.zip` staging when we get to the 9x host.
- Everything here is **inert until you stage the DDK ZIP** and a box is online —
  same safety model as fleet onboarding. The scripts are idempotent and retry-
  friendly.
- The build box keeps the DDK, so subsequent `build_driver.py` runs are fast
  (only the small source package moves each time).
