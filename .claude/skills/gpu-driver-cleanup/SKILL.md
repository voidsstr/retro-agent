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

## Notes
- If the removed card is being **replaced**, do cleanup first, then install the
  new driver with `deploy-3dfx-driver` (or the vendor's installer).
- If the card is still physically present and you only want to *reset* its
  driver, uninstalling via Device Manager (screenshot-click `LAUNCH devmgmt.msc`)
  is gentler; this skill is for **full removal**, especially ghost devices.
- Never touch chipset/storage drivers that share the vendor prefix.
