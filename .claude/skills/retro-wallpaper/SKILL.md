---
name: retro-wallpaper
description: Generate and deploy an exciting rotating "system dossier" desktop wallpaper for a retro PC - specs, era-appropriate games (CPU-release year + GPU-release year), and a tech/research-milestone collage for the CPU year that cycles through 10 variants. Use when the user asks to make, update, refresh, rotate, or deploy a wallpaper/desktop background for one or more retro fleet machines.
---

# Retro Dossier Wallpaper

Builds a per-machine spec-sheet wallpaper and sets it as the XP desktop
background. Layout (top to bottom): hostname header + CPU/GPU year badges, a row
of spec cards (CPU/GPU/RAM/OS/DISPLAY/STORAGE), two game panels (games from the
**CPU release year** and the **GPU release year**), and a collage of **tech /
research milestone** tiles (image + gradient scrim + caption) for the **CPU
year**. Each machine gets **10 iterations** that an on-device rotator cycles
through, so the collage keeps showing different era-tech content.

All tooling lives in `scripts/retro-wallpaper/`.

## Design rationale (why it looks the way it does)

- **Dark UI, native resolution, 24-bit BMP.** Renders at the machine's exact
  resolution and saves a 24-bit BMP (universally safe as XP/9x wallpaper) plus a
  PNG preview. Left-aligned content + a right-side accent glow keep the busy area
  away from where desktop icons sit (top-left).
- **Two eras.** CPU-year and GPU-year game lists make the machine's age legible
  at a glance; the badges echo the years in the accent colors.
- **Tech/research collage** grounds the machine in the tech + science moment of
  its CPU year (chips, consoles, gadgets, space/research milestones) - image tiles
  with a bottom gradient scrim so white captions stay legible over any photo.
- **Rotation.** Each machine gets 10 wallpaper iterations, each a rotating 6-item
  window over that year's 12-item tech pool (step of 5, coprime to 12, so each
  iteration looks distinct). An on-device rotator cycles them on an interval, which
  keeps the desktop fresh without any XP-native slideshow support (XP has none).
- **Scale.** `gen_wallpaper.py` scales by `min(W/1024, H/768)` so a widescreen
  target doesn't let the fixed-height blocks starve the events grid (a width-only
  scale squashes the collage on 16:9 - already fixed, keep it).
- **Icon well.** The right `ICON_WELL_FRAC` (~0.36) of the content width is left
  as bare dark background for the whole game+events band (bottom-right). The game
  panels and events collage keep to the left `content_w`; desktop icons get moved
  into that well so they stay clearly readable over a plain backdrop instead of
  colliding with busy artwork. Some machines have 25-35 desktop icons, so the well
  spans the full height of that band (not just the events row).

## Workflow

### 1. Gather specs from the machine

```
SYSINFO                       # RAM, drives, OS
VIDEODIAG                     # GPU name, resolution, refresh, depth
EXEC reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ProcessorNameString
EXEC reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ~MHz
```

`~MHz` is hex (e.g. `0x3e8` = 1000). Old PIII boxes don't populate
`ProcessorNameString` - fall back to family/model from SYSINFO and the hostname.
Decide the **CPU release year** and **GPU release year** from the part names.

### 2. Author the profile

For the known fleet, edit `build_profiles.py` (the `MACHINES` list holds each
machine's specs, accent colors, and CPU/GPU years) and run it:

```bash
cd scripts/retro-wallpaper && python3 build_profiles.py
```

`build_profiles.py` resolves every tile image through the **Wikimedia Commons
API** so URLs are real current thumbnails (never hand-guess hashed
`upload.wikimedia.org` paths - they 404). Gotcha: the top search hit is sometimes
wrong (a rocket for "YouTube", a Pentium III for "Pentium 4"). Verify a better
filename (`curl -I .../Special:FilePath/<Name>`) and pin it in the `OVERRIDE` map.
`GAMES` and `TECH` dicts are keyed by year - add a year there if a new machine
needs one. It writes **10 iteration profiles per machine**
(`profiles/<host>.iNN.json`), each a rotating 6-item window over the year's TECH
pool (`iteration_events`).

Keep spec-card values short (~22 chars) or the value wraps past 2 lines and the
clock gets truncated.

### 3. Render all iterations

```bash
for f in profiles/*.i*.json; do python3 gen_wallpaper.py "$f"; done   # out/<host>.iNN.bmp
```

First pass fetches images cold; if any tile renders as a flat placeholder box,
just re-run (the cache is warm the second time). Eyeball a couple of PNGs.

### 4. Deploy the rotation

Binary upload is **not** available over MCP `retro_command` - use the bundled
Python client:

```bash
python3 deploy_rotation.py 192.168.1.XXX [interval_seconds]   # default 60
```

>  **THIS SCRIPT IS SUPERSEDED — DO NOT RUN IT ON A FLEET BOX.** Since 2026-08-31
">  the agent's `retrowall` thread owns the desktop, and it actively *undoes* what
">  this script installs: it deletes the `RetroWallRotate` `Run` value from **both**
">  HKLM and HKCU and renames `rotate_wall.exe`/`arrange_icons.exe` to
">  `.superseded`. Running `deploy_rotation.py` re-installs the rotator, which is
">  the bug that reverted boxes to the old background on every reboot. Stage
">  wallpapers for the agent to pick up instead; the description below is kept
">  only to explain what the old assets on a box are.

Per machine it: stages the 10 BMPs into `C:\retro-wall\wall00..09.bmp`, uploads
`rotate_wall.exe`, sets the HKCU wallpaper style, runs **`arrange_icons.exe`** to
park the desktop icons in the bottom-right well, installs an HKCU `Run` key, and
launches the rotator. **`rotate_wall.exe`** (GUI-subsystem, single-instance mutex)
cycles `wall00..NN.bmp` via `SystemParametersInfo(SPI_SETDESKWALLPAPER)` every
interval. `arrange_icons.exe` drops `LVS_AUTOARRANGE` and `LVM_SETITEMPOSITION`s
each icon into a packed grid (spacing/well in sync with `ICON_WELL_FRAC`).

Both helpers are mingw cross-builds (rebuild only if changed):
```bash
i686-w64-mingw32-gcc -O2 -mwindows -o rotate_wall.exe  rotate_wall.c  -luser32
i686-w64-mingw32-gcc -O2          -o arrange_icons.exe arrange_icons.c -luser32 -lgdi32
i686-w64-mingw32-gcc -O2          -o drag_icon.exe     drag_icon.c     -luser32
```

`deploy_wallpaper.py` still exists for a single static wallpaper (no rotation).
`run_arrange.py <ip>` / `run_drag.py <ip> fx fy tx ty` push+run those helpers
without re-uploading the BMPs.

**Desktop-icon gotchas (why they overlap / drift back):**
- **Auto Arrange must be OFF** or the shell re-snaps icons to the top-left grid
  and every wallpaper-rotation refresh undoes the placement. `arrange_icons.exe`
  toggles it off via the shell menu command `FCIDM_SHVIEW_AUTOARRANGE` (0x7031)
  when the `LVS_AUTOARRANGE` style is set - a plain `SetWindowLong` isn't enough.
- **Snap-to-grid vs. dragging.** `LVM_SETITEMPOSITION` sets exact pixels, but a
  *drag* obeys snap-to-grid on a ~75px screen grid that won't match the well grid,
  so dragging one icon can bump a neighbor to a top cell. Disable snap-to-grid
  before dragging: set `FFlags` in
  `HKCU\...\Shell\Bags\1\Desktop` (clear bit `0x4`, e.g. `0x224`->`0x220`) then
  reload the shell (`taskkill /f /im explorer.exe` + `LAUNCH explorer.exe`; XP
  auto-restarts it). Do NOT use command 0x7032 for snap-to-grid - on the desktop
  it's "Customize This Folder" and pops a modal that hangs a `SendMessage`.
- **A stuck icon** (a folder/shortcut whose saved `ItemPos` the shell keeps
  reasserting, e.g. a game folder pinned over the header) is cleared by the same
  `FFlags`-clear + explorer reload; after the reload `arrange_icons.exe` can move
  it normally. `arrange_icons.exe` also positions a few indices past the reported
  count in case `LVM_GETITEMCOUNT` under-reports by one.
- **Snap-to-grid stuck ON (nonstandard registry).** Some machines keep their
  desktop view under `ShellNoRoam\Bags\<N>\Desktop` (N discovered via `BagMRU`),
  not `Shell\Bags\1\Desktop`, so the `FFlags`-clear can't find it. There, snap
  forces a ~75-78px grid and the icons spread wider than the tight 70px machines
  (still inside the well, just less dense). `set_metrics.exe` shrinks the desktop
  icon spacing live (`SPI_SETICONMETRICS`) to line the snap grid up with the
  arranger - but XP clamps horizontal spacing to ~75px, so on a snap-locked box
  that's the tightest achievable without a logoff. `run_exe.py <ip> <exe>` pushes
  and runs any of these helper exes.

### 5. Verify

On a fleet box the wallpaper is **static** and rotation is gone. Confirm the
agent's pick landed: `reg query "HKCU\Control Panel\Desktop" /v Wallpaper` returns
a `C:\retro-wall\wallNN.bmp` that does **not** change across an interval, and
`tasklist | find "rotate_wall"` finds **nothing**. A *running* `rotate_wall` or a
changing NN is a REGRESSION, not a pass — check that `RetroWallRotate` is absent
from `Run` under both HKLM and HKCU.

`retro_screenshot` the machine. If it comes back **pure black** or shows a saver
(starfield / "Windows XP" logo), the machine is idle: `taskkill /f /im *.scr`
(e.g. `ssstars.scr`, `logon.scr`) or `LAUNCH notepad.exe` to force a repaint,
screenshot, then `taskkill /f /im notepad.exe`. See memory
`123-ati-gdi-black-screenshot`.
Independent proof: `reg query "HKCU\Control Panel\Desktop" /v Wallpaper` and `dir`
the BMP (a 1280x1024x24 file is 3,932,214 bytes).
