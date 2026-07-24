# 3dfx Control Panel (`3dfxctl.exe`) — build & deploy

Claude/operator instructions for building the retro3dfx driver settings tool and
getting it onto the fleet share and the Voodoo boxes.

## What it is

A Win32 GUI (`3dfxctl.c`) that reads/writes **every persistent driver-level
setting** of the vintage 3dfx Voodoo stack as registry values, so changes
survive reboots and can be changed again. Covers: core clock, vsync (Glide/GL
and D3D), full-screen refresh, desktop & 3D gamma, SLI/AA, SLI band height,
overlay filter, alpha dither, LOD bias, command-FIFO size, framebuffer tiling,
and the verbose log gate. Each control shows the box's current value on launch.

Type/subkey discipline is baked in (get this wrong and the driver ignores writes):
- `GraphicsClocking`, `CmdfifoSize`, `TiledMode`, gamma LUTs → **`…\3dfxvs\Device0`** (DWORD / BINARY)
- `SSTH3_*` → **`…\Device0\D3D`** (REG_SZ; the driver searches D3D first, Device0 fallback)
- `FX_GLIDE_*` → **`…\Device0\glide`** (REG_SZ)

Safety: the DESKTOP gamma write is clamped so the tool can never re-create the
blown overbright ramp that washes out the 2D desktop (the same guard the display
driver enforces).

## Build

```bash
cd scripts/3dfx/3dfxctl
make            # i686-w64-mingw32-gcc -> 3dfxctl.exe (32-bit, static-libgcc, no extra DLLs)
```
Needs `gcc-mingw-w64-i686` (the same toolchain the rest of the fleet C tools use).

## Deploy — one command

```bash
python3 push_3dfxctl.py <online-agent-ip> --deploy
```
This, via the target agent:
1. uploads `3dfxctl.exe` to `C:\RETRO_AGENT\stage\`;
2. publishes it to the share **`\\192.168.1.122\files\Utility\Retro Automation\3dfx\3dfxctl.exe`**
   (reconnects `Z:` first; skips gracefully if `\\192.168.1.122` is offline);
3. with `--deploy`, copies it to `C:\RETRO_AGENT\3dfxctl.exe` on that box and
   creates an All-Users Start Menu shortcut **"3dfx Control Panel"**.

Publish to the share only (no box install): drop `--deploy`.
Run against any box that has `Z:` mapped writable to (re)publish the share copy.

## Deploy by hand (retro-agent protocol)

```
UPLOAD C:\RETRO_AGENT\3dfxctl.exe            # command + binary_payload
EXEC cmd /c net use Z: \\192.168.1.122\files /persistent:yes
EXEC cmd /c md "Z:\Utility\Retro Automation\3dfx"
EXEC cmd /c copy /Y C:\RETRO_AGENT\3dfxctl.exe "Z:\Utility\Retro Automation\3dfx\3dfxctl.exe"
```
Then the user launches it from the Start Menu ("3dfx Control Panel") or
`C:\RETRO_AGENT\3dfxctl.exe`. **Reboot** (or relaunch the game) for changes to
take effect — every setting is read at boot / mode-set / app-launch.

## Scope note

The vintage driver exposes ~150 registry knobs; this panel surfaces the ~14 a
power user actually wants. The long tail (per-chip/per-sample AA-jitter matrices,
dither-matrix selects, debug-verbosity levels, raw `miscInit1`/`dramInit*` memory
timings) is intentionally **not** exposed — those are raw and dangerous. Add a row
to `gSettings[]` in `3dfxctl.c` if a specific one is needed (one line: label,
subkey, name, type, dword flag, choices/default, help).
