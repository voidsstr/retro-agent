---
name: deploy-3dfx-driver
description: Deploy the self-built 3dfx XP driver package (3dfxvsm.sys + 3dfxvs.dll + glide3x.dll) to a fleet Windows XP machine with a Voodoo 3/4/5 via the retro agent — preflight HWID check, staged upload, backup, SetupAPI install, verify, rollback. Use when the user says to install/deploy/update the 3dfx driver, "put our driver on .124", replace the in-box XP Voodoo driver, or test the freshly built Voodoo3/Voodoo5 driver on a fleet box.
---

# Deploy 3dfx XP Driver to a Fleet Machine

Installs the 3dfx display driver we build in the private **retro-3dfx** repo
(see memory: `3dfx-driver-build-project`) onto a fleet **XP/W2K** box over the
retro agent.
This is a display driver swap on a remote machine that needs physical access to
recover — be deliberate, back up first, and follow the fleet rules in CLAUDE.md.

**Hard rules (non-negotiable):**
- **NEVER REBOOT without explicit user approval** — ask before every reboot.
- **Never raw-copy over `system32\` / `system32\drivers\` files** — XP WFP
  silently reverts them. Install must go through SetupAPI/PnP (the package's
  `updrv.exe`/`INSTALL.bat`), never `FILECOPY` into system32.

**WFP (Windows File Protection) — why files revert, and how to stop it:**
- WFP guards only files that have a **`C:\WINDOWS\system32\dllcache\` twin**
  (its restore-from set). Check with `dir dllcache\<file>`: if it's there, WFP
  tracks it and will revert a raw overwrite to the cached version.
- **Two durable escapes (both proven on .143):**
  1. **WFP-renamed driver files** — ship the display driver under a name the
     inbox catalog doesn't list (we use `3dfxv5d.dll`/`3dfxv5m.sys`, not the
     inbox `3dfxvs.dll`/`3dfxvsm.sys`). A non-cataloged name is never tracked.
     `dir dllcache` confirms ours are absent → WFP can't touch them.
  2. **Seed dllcache with your version** — for any file that MUST keep the
     inbox name, `copy /Y yourfile C:\WINDOWS\system32\dllcache\<name>` first;
     then a WFP "restore" restores *your* file. (Do this before the system32
     copy.)
- **Our ICD test files are game-local** (`<game>\opengl32.dll` / `3dfxogl.dll`)
  and are **never WFP-tracked** — ICD iteration needs no WFP handling at all.
- **Registry disable** (`Winlogon\SFCDisable=0xffffff9d`, `SFCScan=0`): set it
  if asked, but on **XP SP2/SP3 it needs a reboot and strictly an `sfc_os.dll`
  patch to fully take** — do NOT rely on it alone. Prefer the rename / dllcache
  escapes above, which work immediately with no reboot. (.143 has the registry
  values set, 2026-07-20, as belt-and-suspenders.)
- **Confirm with the user before touching the machine** — after preflight,
  present the plan (target, card, HWID, package version) and wait for a go.
- Do not deploy to Win98 boxes. This driver pair is the W2K/XP build only.

## Inputs

- **Target IP** — the XP Voodoo box. Known fit: `.124` ("ADMIN", XP SP3,
  Voodoo3 AGP). If the user doesn't name a machine and more than one candidate
  exists, ask.
- **Package dir** — newest
  `/home/voidsstr/development/retro-3dfx/toolchain-3dfx/dist/3dfx-napalm-xp-*/`
  (glob, sort, take latest — the 3dfx source + toolchain live in the sibling
  **retro-3dfx** repo, not here). If `dist/` has no package yet, build one
  first (next section).

## Package layout (build it if missing)

A deployable package dir contains exactly the files we actually built — nothing
more, or the INF copy step fails on missing files:

```
3dfx-napalm-xp-<ver>/
  3dfxvsm.sys      # miniport  — <retro-3dfx>/toolchain-3dfx/prefix/drive_c/3dfx/H5/W2K/Src/Video/Miniport/H5/objfre/i386/3dfxvsm.sys (195812 B)
  3dfxvs.dll       # display   — .../W2K/Src/Video/Displays/H5/objfre/i386/3dfxvs.dll (595180 B)
  glide3x.dll      # Glide3    — .../H5/BIN/glide3x.dll (335872 B, 96 exports)
  fxoem2x.dll      # OEM lib   — .../H5/BIN/fxoem2x.dll
  voodoo3.inf      # trimmed from .../W2K/Src/Video/Inf/Voodoo3/Voodoo3.inf   (DEV_0005)
  voodoo5.inf      # trimmed from .../W2K/Src/Video/Inf/Voodoo5/3DFXVS2K.INF  (DEV_0009/000B)
  updrv.exe        # SetupAPI installer helper (UpdateDriverForPlugAndPlayDevices), prints "UPDRV: OK|FAIL ..."
  INSTALL.bat      # file backup + driver-signing policy + updrv invocation (does NOT reg-export — do that separately, Step 4)
```

**INF trimming is mandatory.** The stock `[3dfxvs.Display]` CopyFiles section
lists `glide2x.dll`, `3dfxSpl2.dll`, `3dfxSpl3.dll` (and V5's `3dfxOGL.dll`) —
we did **not** build those. Remove every file we don't ship from CopyFiles /
`[SourceDisksFiles]` or the install dies with missing-source-file errors.

**HWID coverage:** the stock Voodoo3.inf does NOT list `.124`'s card
`PCI\VEN_121A&DEV_0005&SUBSYS_1037121A&REV_01` — the `[3dfx.Mfg]` model line
for its exact `SUBSYS` must be present (added during packaging, verified during
preflight).

`updrv.exe` (source: `agent/tools/updrv.c`) loads `newdev.dll` **dynamically** —
do NOT link `-lnewdev`/`-lsetupapi`. Build exactly as its header says:
`i686-w64-mingw32-gcc -Wall -Wextra -Os -s -nostdlib -DWIN32_LEAN_AND_MEAN
-DWINVER=0x0500 -D_WIN32_WINNT=0x0500 -march=i586 -mtune=pentium3
-fno-stack-protector -o updrv.exe updrv.c -Wl,-e,_mainCRTStartup -lkernel32 -luser32`.

## Step 1 — Choose the transport

- **Daemon holds the connection** (agent is single-connection): run
  `bash scripts/chat_status.sh`. If the target IP is in the daemon's *claimed*
  list, do NOT open a direct `RetroConnection` — it will fail or fight the
  daemon. Either queue each command via
  `python3 scripts/retro_enqueue.py <ip> "<agent cmd>" --label 3dfx-deploy`
  (fine for simple EXEC/REGREAD steps, but uploads need the direct protocol) or
  ask the user to release/stop the daemon claim for the deploy window.
- **Direct** (preferred for the deploy itself, since UPLOAD is two-frame):
  `client/retro_protocol.py` → `RetroConnection(ip, 9898)`, secret
  `retro-agent-secret`, and **always `await conn.close()`** between phases.
  Keep sessions short; one connection at a time.

## Step 2 — Preflight (read-only)

1. `SYSINFO` — OS must be XP or W2K (`5.x`). Abort otherwise. Note the agent
   version: **EXECW needs agent ≥ 1.6.0**; `.124` was last seen on 1.5.1, so
   plan around plain `EXEC` (60 s cap) there unless it has auto-updated.
2. `VIDEODIAG` — record current driver, version, INF, resolution. Expected
   starting state on `.124`: MS in-box XP Voodoo3 driver **5.1.2001.0 /
   3dfxvs2k.inf** — this in-box driver stays in the driver store and is the
   rollback safety net.
3. Exact HWID incl. SUBSYS:
   `REGREAD HKLM SYSTEM\CurrentControlSet\Enum\PCI` → enumerate for
   `VEN_121A`, then read the device subkey to get the full
   `VEN_121A&DEV_xxxx&SUBSYS_xxxxxxxx&REV_xx` string.
4. Map device → INF: `DEV_0005` → `voodoo3.inf`; `DEV_0009`/`DEV_000B` →
   `voodoo5.inf`. Anything else: stop, wrong card.
5. **Grep the package INF for the exact SUBSYS.** If it's not in `[3dfx.Mfg]`,
   **STOP** — add the model line to the INF in the build tree, repackage, and
   only then continue. Never install an INF that doesn't match the HWID (PnP
   will silently keep the in-box driver).
6. Present findings to the user and get explicit approval to proceed.

## Step 3 — Stage

```
MKDIR C:\RETRO_AGENT\3dfx-driver
UPLOAD C:\RETRO_AGENT\3dfx-driver\<file>     # two-frame: command + binary_payload, one per package file
```
Verify sizes afterwards with `EXEC cmd /c dir C:\RETRO_AGENT\3dfx-driver`
(compare byte counts against the local package).

## Step 4 — Backup (before any install action)

Into `C:\RETRO_AGENT\3dfx-driver\backup\` (MKDIR first):

```
EXEC reg export "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}" C:\RETRO_AGENT\3dfx-driver\backup\display-class.reg /y
EXEC cmd /c copy /Y C:\WINDOWS\system32\drivers\3dfxvsm.sys C:\RETRO_AGENT\3dfx-driver\backup\
EXEC cmd /c copy /Y C:\WINDOWS\system32\3dfxvs*.dll C:\RETRO_AGENT\3dfx-driver\backup\
EXEC cmd /c copy /Y C:\WINDOWS\system32\glide*.dll C:\RETRO_AGENT\3dfx-driver\backup\
```
(The package `INSTALL.bat` performs the same **file** backups — but it does NOT
do the reg export. Do the reg export explicitly in all cases.)

Also suppress the unsigned-driver GUI prompt (our binaries are unsigned;
without this the install blocks on an invisible dialog):

```
EXEC reg add "HKLM\SOFTWARE\Microsoft\Driver Signing" /v Policy /t REG_BINARY /d 00 /f
EXEC reg add "HKCU\Software\Microsoft\Driver Signing" /v Policy /t REG_BINARY /d 00 /f
```

## Step 5 — Install

```
EXEC C:\RETRO_AGENT\3dfx-driver\INSTALL.bat
```
(or directly — **INF path first, then HWID**:
`updrv.exe C:\RETRO_AGENT\3dfx-driver\voodoo3.inf "PCI\VEN_121A&DEV_0005"`).
Use `EXECW 300 <cmd>` instead of `EXEC` when the agent is ≥ 1.6.0 —
SetupAPI can exceed 60 s.

Parse the output for the `UPDRV:` result line:
- **`UPDRV: OK`** (possibly "reboot required") → proceed to Step 6.
- **`UPDRV: FAIL`** or no line → pull diagnostics before anything else:
  `EXEC cmd /c type C:\WINDOWS\setupapi.log | more` is too big — instead grab
  the tail (e.g. copy last part:
  `EXEC cmd /c copy /Y C:\WINDOWS\setupapi.log C:\RETRO_AGENT\3dfx-driver\` then
  `DOWNLOAD` it and read the final section locally). Typical causes: SUBSYS not
  in INF, a CopyFiles entry for a file we don't ship, signing policy prompt.
  Fix, repackage, re-upload, retry. Do NOT reboot a failed install.

## Step 6 — Reboot + verify

1. **Ask the user for reboot approval** (fleet rule — physical access may be
   needed if the display driver is bad). On approval: `REBOOT`.
2. Reconnect after the box returns (retry connect for a few minutes; if the
   daemon owns the machine, queue a `SYSINFO` via `retro_enqueue.py` instead).
3. `VIDEODIAG` — driver version / INF must now be OUR package (no longer
   `5.1.2001.0` / `3dfxvs2k.inf`), and resolution restored (not 640x480 4-bit —
   that means the driver fell back; treat as failure, go to rollback).
4. `SCREENSHOT 0` → convert BMP→PNG (see CLAUDE.md) → eyeball a sane desktop.
5. Optional: offer the **retro-benchmark** skill for before/after A/B numbers
   against the in-box driver.

## Step 7 — OpenGL ICD: update EVERY game-local copy (CRITICAL)

The display-driver install above does NOT get a new `3dfxogl.dll` into games.
Windows' LoadLibrary checks the **game's own directory before system32**, and
fleet games carry app-local GL DLLs — they silently keep running an old ICD
if you only update system32 (this cost a full day on .143: the "fixed" driver
wasn't the one CS/Q3 loaded).

1. Inventory every copy: `EXECW 180 cmd /c dir /s /b C:\opengl32.dll C:\3dfxogl.dll`
   (known on .143: Q3 dir has BOTH `opengl32.dll` and `3dfxogl.dll` — which one
   loads is decided by `r_glDriver` in q3config.cfg, keep them identical; both
   CS installs, Quake2, and UT GOTY System each have `opengl32.dll`).
2. **Kill every GL game first** (`taskkill /f /im quake3.exe`, `hl.exe`, …) — a
   loaded DLL is locked and `copy /Y` fails; check each copy's output for
   `1 file(s) copied`, don't assume.
3. Stage the new ICD once, `copy /Y` it over every inventoried path (keep a
   `.preNNN` backup on first replacement). NEVER touch
   `system32\opengl32.dll`/`dllcache\opengl32.dll` (Microsoft's, WFP).
4. Verify by renderer string, not file size (builds are often same-size):
   the GL console of any game shows `3Dfx [retro3dfx X.Y.Z]`.
5. Never launch a second fullscreen GL game while one runs — Glide surface
   collision can wedge the box for minutes.

Full write-up ships in the driver package: `dist/<pkg>/DEPLOYMENT.txt`.

## Rollback (three ways, in order of preference)

1. **XP Driver Roll Back (remote, no console needed):** screenshot-click loop —
   `LAUNCH devmgmt.msc`, navigate to Display adapters → the Voodoo → Properties
   → Driver tab → **Roll Back Driver** (`SCREENSHOT 0` + `UICLICK`/`UIKEY`).
   XP kept the in-box 3dfxvs2k driver in the store, so rollback lands there.
2. **Last Known Good:** needs physical/console access (F8 at boot) — tell the
   user; nothing the agent can do here.
3. **Restore backups:** `EXEC regedit /s` the exported `display-class.reg`,
   then reinstall the in-box driver via Device Manager "Have Disk" pointing at
   the in-box INF (`C:\WINDOWS\inf\3dfxvs2k.inf` — the backup dir holds files
   only, no INF; never raw-copy into system32 — WFP). Reboot (with approval)
   and re-verify with `VIDEODIAG`.

## Known context

- `.124` = XP SP3 "ADMIN", Voodoo3 AGP
  `PCI\VEN_121A&DEV_0005&SUBSYS_1037121A&REV_01`, Windows on **D:** (dual-boot
  box — see CLAUDE.md "Dual-boot swap gotcha" before touching agent binaries).
- The Amigamerlin/Win98 install notes in CLAUDE.md do NOT apply here — those
  are for Win98 VxD drivers. XP uses this SetupAPI path exclusively.
