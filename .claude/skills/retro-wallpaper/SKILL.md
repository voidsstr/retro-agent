---
name: retro-wallpaper
description: Generate and deploy an exciting "system dossier" desktop wallpaper for a retro PC - specs, era-appropriate games (CPU-release year + GPU-release year), and a historical-events collage for the CPU year. Use when the user asks to make, update, refresh, or deploy a wallpaper/desktop background for one or more retro fleet machines.
---

# Retro Dossier Wallpaper

Builds a per-machine spec-sheet wallpaper and sets it as the XP desktop
background. Layout (top to bottom): hostname header + CPU/GPU year badges, a row
of spec cards (CPU/GPU/RAM/OS/DISPLAY/STORAGE), two game panels (games from the
**CPU release year** and the **GPU release year**), and a full-width collage of
historical-event tiles (image + gradient scrim + caption) for the **CPU year**.

All tooling lives in `scripts/retro-wallpaper/`.

## Design rationale (why it looks the way it does)

- **Dark UI, native resolution, 24-bit BMP.** Renders at the machine's exact
  resolution and saves a 24-bit BMP (universally safe as XP/9x wallpaper) plus a
  PNG preview. Left-aligned content + a right-side accent glow keep the busy area
  away from where desktop icons sit (top-left).
- **Two eras.** CPU-year and GPU-year game lists make the machine's age legible
  at a glance; the badges echo the years in the accent colors.
- **Events collage** grounds the machine in its moment in history - image tiles
  with a bottom gradient scrim so white captions stay legible over any photo.
- **Scale.** `gen_wallpaper.py` scales by `min(W/1024, H/768)` so a widescreen
  target doesn't let the fixed-height blocks starve the events grid (a width-only
  scale squashes the collage on 16:9 - already fixed, keep it).

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

`build_profiles.py` resolves every event image through the **Wikimedia Commons
API** so URLs are real current thumbnails (never hand-guess hashed
`upload.wikimedia.org` paths - they 404). Gotcha: the top search hit is sometimes
wrong (a flag for "Benedict XVI", a Vita for "PlayStation 2"). Pin those in the
`OVERRIDE` map to an exact verified `File:...` name. `GAMES` and `EVENTS` dicts
are keyed by year - add a year there if a new machine needs one.

Keep spec-card values short (~22 chars) or the value wraps past 2 lines and the
clock gets truncated.

### 3. Render

```bash
python3 gen_wallpaper.py profiles/192.168.1.XXX.json      # writes out/*.bmp + *.png
```

Read the PNG and eyeball it before deploying.

### 4. Deploy

Binary upload is **not** available over MCP `retro_command` - use the bundled
Python client:

```bash
python3 deploy_wallpaper.py 192.168.1.XXX out/192.168.1.XXX.bmp
```

It uploads the BMP to `C:\retro-dossier.bmp`, writes the HKCU wallpaper registry
values via a `.reg` (regedit /s), and refreshes with
`RUNDLL32 USER32.DLL,UpdatePerUserSystemParameters` (no logoff needed).

### 5. Verify

`retro_screenshot` the machine. If it comes back **pure black**, the monitor is
DPMS-asleep - `LAUNCH notepad.exe` to force a repaint, screenshot, then
`taskkill /f /im notepad.exe`. See memory `123-ati-gdi-black-screenshot`.
Independent proof: `reg query "HKCU\Control Panel\Desktop" /v Wallpaper` and `dir`
the BMP (a 1280x1024x24 file is 3,932,214 bytes).
