---
name: install-iqlab
description: Install or update the 3dfx Image Quality Lab (iqlab) on a fleet box from the SMB share. Use when asked to install iqlab, put the 3dfx tuning tool on a machine, update iqlab to the latest version, or check which iqlab version a box has.
---

# Install the 3dfx Image Quality Lab on a box

`iqlab.exe` is the tuning console for the Voodoo 3 / Voodoo 5 stack. It detects the
board, offers only the settings that board supports, previews them live, and writes
them either globally or per-game.

It is published to the share by the retro-3dfx lane; **this skill only installs what
is already published.** To build and publish a new version, see
`retro-3dfx/optimized/iqlab/` (`./build.sh` then `python3 publish.py`).

## Where it lives on the share

```
\\192.168.1.122\files\Utility\Retro Automation\iqlab\
    iqlab.exe            <- latest pointer (install this)
    iqlab.exe.ver        <- version sidecar, e.g. "0.4.0"
    iqlab-<ver>.exe      <- versioned archive, to pin an exact build
```

Boxes have the share mapped as `Z:`, so from a box that is `Z:\Utility\Retro
Automation\iqlab\`.

**Version decides the pull, never file size.** Read `iqlab.exe.ver` and compare it
against the installed `C:\RETRO_AGENT\iqlab\iqlab.ver`. A same-size build is a real
thing and a size comparison silently refuses to ship it — that exact bug cost the
DOS lane a day.

## Install / update

Run these through the agent on the target box (`EXECW`):

```bat
net use Z: \\192.168.1.122\files /persistent:yes
if not exist "Z:\Utility\Retro Automation\iqlab\iqlab.exe" echo SHARE-MISSING
md C:\RETRO_AGENT\iqlab
taskkill /f /im iqlab.exe 2>nul
copy /Y "Z:\Utility\Retro Automation\iqlab\iqlab.exe"     "C:\RETRO_AGENT\iqlab\iqlab.exe"
copy /Y "Z:\Utility\Retro Automation\iqlab\iqlab.exe.ver" "C:\RETRO_AGENT\iqlab\iqlab.ver"
copy /Y "C:\RETRO_AGENT\iqlab\iqlab.exe" "%USERPROFILE%\Desktop\iqlab.exe"
```

**Kill any running instance first.** A running `iqlab.exe` holds a lock on the file,
the copy fails, and the box then launches the version you thought you had replaced —
with no error anywhere.

Confirm afterwards:

```bat
type C:\RETRO_AGENT\iqlab\iqlab.ver
```

The version is also shown in the app's title bar and under its heading, so a
screenshot is a valid check.

## Checking what is installed

```bat
if exist C:\RETRO_AGENT\iqlab\iqlab.ver (type C:\RETRO_AGENT\iqlab\iqlab.ver) else (echo NOT-INSTALLED)
```

## Which boxes this is for

Any box with a 3dfx Voodoo board. It self-detects Voodoo 3, Voodoo 4 4500,
Voodoo 5 5500 and Voodoo 5 6000, and hides settings the board cannot do (a Voodoo 3
has no T-buffer, so anti-aliasing is greyed out; 4-way SLI only appears on a
4-chip 6000). On a machine with no 3dfx board it starts and reports "No 3dfx board
detected" rather than offering settings that would do nothing.

## What to tell the user afterwards

- **Apply to games** writes the settings for **every** Glide title. They take effect
  the next time a game starts — Glide reads them once at start-up, so nothing can
  change a game that is already running.
- **Save profile / Launch with profile** applies settings to **one** game only, by
  launching it with that profile in its environment.
- The live preview needs a **16-bit desktop**; the 3dfx windowed path rejects
  32-bit (`MINIHWC/DXDRVR.C:373`). The app says so when that is why the preview is
  blank.

## Do not

- Do not copy iqlab onto the Voodoo 5 box from any other source than this share
  path — retro-3dfx `CLAUDE.md` requires V5 binaries to come from that lane.
- Do not edit the driver's own registry keys by hand to "help"; iqlab writes the
  Glide keys under `HKCU\SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide`,
  which is the documented lookup path, and hand-edits under HKLM will be shadowed
  by whatever iqlab wrote in HKCU.
