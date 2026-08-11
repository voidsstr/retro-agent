---
name: gpu-driver-cleanup
description: Fully remove a previously-installed video card's drivers from a fleet Windows machine via the retro agent — any vendor (NVIDIA, ATI/AMD, 3dfx, Intel, S3, Matrox). Cleans ghost/phantom devices, the display-class registry instance, the driver service, Run keys, driver files, and the OEM INF. Use when the user swaps a GPU, says "remove the old graphics drivers", "clean the NVIDIA/ATI drivers", "the card was removed, clean it up", or before installing a new display driver on a box that had a different card.
---

# GPU Driver Cleanup (any vendor, XP/2000/2003)

Removes every trace of a video card's driver from a fleet box: the ghost/phantom
PCI devnode (for a physically-removed card), the display-class instance, the
kernel service, autostart Run keys, driver binaries, and the OEM INF. Vendor-
agnostic — you supply the card's PCI vendor id (and optionally device id).

Proven on .143 (removed a GeForce 6800 that was physically pulled and replaced
by a Voodoo5). Companion: `deploy-3dfx-driver` (installs the new driver after).

## Key facts about the environment (learned the hard way)

- **The agent runs as `Administrator` on the Console session, but NOT as
  `SYSTEM`.** Admin can delete the display-class instance, the service, and Run
  values via `regedit /s`, BUT it **cannot** delete PCI `Enum` devnode keys or
  some vendor driver files (ACL-restricted to SYSTEM). Those need SYSTEM.
- **Run something as SYSTEM** with a scheduled task fired immediately:
  ```
  schtasks /create /tn gpuclean /tr "C:\RETRO_AGENT\cleanup.bat" /sc once /st 00:00:00 /ru SYSTEM
  schtasks /run /tn gpuclean          # fires NOW, regardless of the clock
  ... wait, read a verify file the bat writes ...
  schtasks /delete /tn gpuclean /f
  ```
  XP `schtasks` needs `/st HH:MM:SS` (with seconds) and rejects `/f` on create.
  Do **not** rely on `at` — fleet boxes sometimes have a corrupt RTC (year 8326
  seen on .143) that makes `at` schedule into the far future. `schtasks /run`
  fires immediately so the clock doesn't matter. (`sc create`+`sc start` of a
  cmd also runs as SYSTEM but is flakier — prefer schtasks.)
- **Disable Windows Error Reporting first** so any driver hiccup during cleanup
  exits cleanly instead of hanging on a modal dialog (critical for automation):
  ```
  reg add "HKLM\SOFTWARE\Microsoft\PCHealth\ErrorReporting" /v DoReport /t REG_DWORD /d 0 /f
  reg add "HKLM\SOFTWARE\Microsoft\PCHealth\ErrorReporting" /v ShowUI /t REG_DWORD /d 0 /f
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Windows" /v ErrorMode /t REG_DWORD /d 2 /f
  ```
- **Locked files** (e.g. a shell-loaded `nvcpl.dll`) that even SYSTEM can't
  delete live: schedule a boot-time delete with `pendmv.exe <path> -`
  (agent/tools/pendmv.c — MoveFileEx DELAY_UNTIL_REBOOT).
- Display-class GUID is always `{4D36E968-E325-11CE-BFC1-08002BE10318}`.
- **Do NOT glob-delete `nv*` / `ati*` blindly** — an nForce/AMD chipset box has
  storage drivers (`nvata*`, `nvraid*`, `atapi`) with the same prefix. Delete
  the *specific* display-driver files, never a wildcard that could catch chipset.

## Procedure

### 1. Transport
Direct `RetroConnection(ip, 9898)` (secret `retro-agent-secret`); the agent
accepts a connection alongside the chat daemon. Keep sessions short, close
gracefully. UPLOAD needs the direct protocol (not the task queue).

### 2. Identify the card to remove (read-only)
- `VIDEODIAG` — lists adapters with `pci_vendor_id`, `driver_version`,
  `inf_path`, and the class `registry_path` (`...\{4D36E968...}\NNNN`).
- Enumerate the display class instances and match the target vendor:
  `REGREAD HKLM SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\NNNN`
  → read `MatchingDeviceId` (`pci\ven_XXXX&dev_YYYY`), `ProviderName`,
  `DriverDesc`, `InfPath` (e.g. `oem10.inf`), and the service under `Service`
  (or find it in `HKLM\SYSTEM\CurrentControlSet\Services\<name>` with
  `ImagePath` pointing at the miniport `.sys`).
- Find the PCI Enum devnode:
  `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\PCI" | findstr /i VEN_XXXX`
  → full key `VEN_XXXX&DEV_YYYY&SUBSYS_...&REV_..` (has an instance subkey).
- Identify driver files: the miniport from the service `ImagePath`
  (`system32\DRIVERS\*.sys`), the display DLL and control-panel DLLs in
  `system32` (from the vendor — confirm each with `dir`), and the OEM INF:
  the `InfPath` value (e.g. `oem10.inf`) → `C:\WINDOWS\inf\oemN.inf` (+`.pnf`).
  **Confirm the oemN.inf is the target vendor** before deleting:
  `type C:\WINDOWS\inf\oemN.inf | findstr /i "Provider Class <VENDOR>"`.

### 3. Back up (before any deletion)
Into a backup dir (`MKDIR` first):
```
reg export "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}" <bak>\display-class.reg
reg export "HKLM\SYSTEM\CurrentControlSet\Services\<svc>" <bak>\svc.reg
reg export "HKLM\SYSTEM\CurrentControlSet\Enum\PCI\VEN_XXXX&DEV_YYYY&..." <bak>\enum.reg
reg export "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" <bak>\run.reg
```
(XP `reg export` has no `/y`; write to a fresh path so it never prompts.)

### 4. Remove what Admin can (regedit /s of a REGEDIT4 delete file)
UPLOAD a REGEDIT4 file (CRLF) that deletes the class instance, the service, and
the vendor Run values — leave everything else (esp. `RetroAgent`, `MapShare`):
```
REGEDIT4

[-HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}\NNNN]

[-HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\<svc>]

[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run]
"<VendorRunVal1>"=-
"<VendorRunVal2>"=-
```
`EXEC regedit /s <file>`, then verify each key is gone with `reg query`.
The Enum devnode will SURVIVE this (ACL) — that's expected.

### 5. Remove what needs SYSTEM (schtasks) — ghost Enum devnode + locked files
UPLOAD a CRLF `cleanup.bat` and run it as SYSTEM (step-1 schtasks recipe):
```
@echo off
reg delete "HKLM\SYSTEM\CurrentControlSet\Enum\PCI\VEN_XXXX&DEV_YYYY&SUBSYS_...&REV_.." /f
attrib -r -s -h "C:\WINDOWS\system32\<file>.dll"
del /f /q "C:\WINDOWS\system32\<file>.dll"
... (each specific vendor display file + the miniport .sys) ...
del /f /q C:\WINDOWS\inf\oemN.inf C:\WINDOWS\inf\oemN.pnf
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\PCI\VEN_XXXX&..." > <bak>\verify.txt 2>&1
dir "C:\WINDOWS\system32\<file>.dll" >> <bak>\verify.txt 2>&1
```
Note: `&` inside a reg key path is safe **only inside the double quotes**.
For files still locked after this (in use by the shell), schedule a boot-time
delete: `pendmv.exe C:\WINDOWS\system32\<file>.dll -` (they clear on next
reboot).

### 6. Verify
- `VIDEODIAG` / `PCISCAN` — the target vendor adapter must be gone (only the
  remaining/new card listed; ghost cleared). For a still-present card whose
  driver you removed, it shows as "Standard VGA" until a driver is installed.
- Confirm the driver files and oemN.inf are gone (`dir` → File Not Found).

## Learned removing a Voodoo3/Voodoo5 stack from .124 (2026-08-11)

Seven things the procedure above did **not** cover. Fold these in every time.

1. **The global OpenGL ICD registration is the leftover that actually breaks the
   new card.** `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLdrivers\<vendor>`
   (3dfx's `oem9.inf` writes `3dfx` → `DLL=3dfxOGL.dll`) makes **every** OpenGL
   app try to load the old vendor's ICD no matter which GPU is fitted. Delete the
   vendor's subkey. A correct NVIDIA install leaves `RIVATNT → nvoglnt` there.
2. **Ghost vs live devnode: a live one has a `Control` subkey; a ghost does not.**
   Cheapest reliable discriminator — `reg query "<devnode>\<instance>" /s` and look
   for `...\Control`. Do this before deleting anything.
3. **Delete the card's companion bridge ghost too.** A Voodoo5 6000 sits behind a
   HiNT PCI-PCI bridge, so pulling it strands **`PCI\VEN_3388&DEV_0021&...`** on the
   AGP slot's instance path — the very path the replacement card now claims.
   Sweeping only the GPU's `VEN_xxxx` leaves it. Check every non-Intel/non-obvious
   `Enum\PCI` key for a missing `Control` subkey.
4. **Find the OEM INFs by content, never by guessing:**
   `findstr /i "Provider=" %windir%\inf\oem*.inf` → take every `Provider=%3dfx%`.
   On .124 the display class named only `oem13`/`oem14`, but **four** INFs were
   3dfx (`oem9`, `oem12`, `oem13`, `oem14`). Delete each `.inf` **and** its `.PNF`.
5. **Safe Mode changes the plan.** Confirm it with `net start Schedule` — the reply
   *"This service cannot be started in Safe Mode"* is the only reliable test
   (screenshots, running `Run`-key programs, WMI and the screensaver all lie, and
   `SafeBoot\Option\OptionValue` persists after a normal boot). Safe Mode is
   *ideal* for the file/registry purge (nothing is loaded), but **Task Scheduler
   cannot run, and a task created there is silently not persisted** — so do the
   SYSTEM-privileged `Enum\PCI` deletes after rebooting to normal mode, and
   recreate the task then.
6. **Game-local cleanup is two jobs, not one.** Sweep the DLLs *and* fix the
   configs that name them:
   - DLL sweep must include **`retrogl*.dll`** and `3dfxvgl.dll`/`3dfxOGL.dll`, not
     just `opengl32/glide2x/glide3x/3dfxgl`.
   - Then fix the settings: Quake3 `q3config.cfg` `seta r_glDriver "retrogl"` →
     `"opengl32"`; UT99 `UnrealTournament.ini` `RenderDevice=`/`GameRenderDevice=`/
     `WindowedRenderDevice=GlideDrv.GlideRenderDevice` → `D3DDrv.D3DRenderDevice`;
     RtCW `wolfconfig.cfg` `r_glDriver "gl/openglv5.dll"` → `"opengl32"` **and**
     restore `gl/openglv5.stock` over the swapped-in MesaFX copy.
   Rename to `*.3dfxbak` rather than deleting — reversible, and proves the sweep.
   Leave a dual-boot Win98 volume's own `C:\WINDOWS\SYSTEM` alone (separate OS).

   **Sweep by EXECUTABLE name, not by where you found DLLs.** A DLL sweep only
   finds installs that were given a game-local ICD. On .124 a second UT99 lived
   in `C:\WINDOWS\Desktop\Unreal Tournament\` (the Win98 volume's desktop folder,
   invisible from the XP desktop) with no game-local DLLs at all — so the DLL
   sweep missed it entirely and it stayed configured for Glide. Do
   `dir /s /b C:\<game>.exe D:\<game>.exe` for each game you are fixing.

   **Verify the renderer you switch TO, don't just switch away from the old one.**
   Setting UT99 to `D3DDrv.D3DRenderDevice` looked right and even initialised
   correctly against the new card — then hard-crashed every launch. Launch the
   game once and confirm it reaches a map. A config that names a renderer which
   loads but dies is indistinguishable from the original breakage to the user.
7. **Never echo a path inside a `( ... )` block in a generated batch.** A game dir
   called `Unreal Tournament (Installed)` closed the block early and aborted the
   whole script with `\System\glide3x.dll was unexpected at this time` — mid-run,
   after partial work. Emit bare `if exist "X" ren "X" "Y"` lines and verify with a
   separate re-listing sweep.

**Verify OpenGL for real**, not just by registry state: run Quake III with
`+set r_glDriver opengl32 +set logfile 2 +timedemo 1 +demo four +quit` and grep
`qconsole.log` for `GL_RENDERER`. Expect the new card (`GeForce2 GTS/AGP/SSE`),
never a `Mesa Glide ... [voodoo-cleanroom]` or `[retro3dfx]` string.

## Notes
- If the removed card is being **replaced**, do cleanup first, then install the
  new driver with `deploy-3dfx-driver` (or the vendor's installer).
- **Installing an NVIDIA card afterwards: pick the driver by checking the INF, not
  by version number.** A GeForce2 **GTS/Pro/Ti/Ultra** is NV15 (`10DE:0150`);
  ForceWare **81.98 and 93.71 do not contain `DEV_0150`** (they carry only the
  GeForce2 **MX**, NV11) and will refuse to bind. Last supporting branch is
  Release 70 → use **71.89** (71.84's English package ships no `nv4_disp.cat`).
  The package is an InstallShield PackageForTheWeb stub: `-s` extracts and starts
  `setup.exe`, but the wizard still needs clicking — and **kill `wuauclt.exe` and
  `net stop wscsvc` + `sc config wscsvc start= disabled` first**, or the Automatic
  Updates / Security Center popups steal focus mid-wizard. `agent/tools/updrv.exe`
  binds an INF headlessly if you can get a flat extract.
- If the card is still physically present and you only want to *reset* its
  driver, uninstalling via Device Manager (screenshot-click `LAUNCH devmgmt.msc`)
  is gentler; this skill is for **full removal**, especially ghost devices.
- Never touch chipset/storage drivers that share the vendor prefix.
