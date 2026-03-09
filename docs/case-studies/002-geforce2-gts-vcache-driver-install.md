# Case Study 002: GeForce 2 GTS Driver Install — vcache Bug, Windows Protection Errors, and NVIDIA on Win98 with >512MB RAM

## Problem Statement

A Windows 98 SE machine at 192.168.1.133 needed NVIDIA GeForce 2 GTS drivers installed. The machine had 1021MB RAM and a history of video card swaps (TNT2 Ultra, GeForce4 Ti 4600 ghosts in registry). What appeared to be a straightforward driver install turned into a multi-hour debugging session as every driver version caused "Windows Protection Error" on boot — ultimately traced to a Win98 memory management bug completely unrelated to the video drivers.

## Environment

- **Machine**: hostname "2000", 1021MB RAM, drives A/C/D/E/F
- **OS**: Windows 98 SE (4.10.2222)
- **Video Card**: NVIDIA GeForce 2 GTS (PCI ID 10DE:0150, SUBSYS_002E10DE REV_A4 — NVIDIA reference board)
- **Network**: IP 192.168.1.133, agent on port 9898
- **File Share**: SMB at 192.168.1.122 with driver repository on E:\Drivers
- **Dashboard**: Docker container running on the Linux workstation, holding persistent agent connections

## Phase 1: Initial Diagnosis

### System State

Connected to agent via `RetroConnection('192.168.1.133', 9898)`:

- **SYSINFO**: Win98 4.10.2222, 1021MB RAM
- **PCISCAN**: Two ghost devices (TNT2 Ultra 10DE:0029, GeForce4 Ti 4600 10DE:0250), no active display adapter
- **VIDEODIAG**: 640x480 4-bit VGA mode, TNT2 Ultra driver loaded (stale)

### Ghost Cleanup

Deleted ghost PCI enum entries and their Display class bindings:
- `Enum\PCI\VEN_10DE&DEV_0029&...` (TNT2 Ultra)
- `Enum\PCI\VEN_10DE&DEV_0250&...` (GeForce4 Ti 4600)
- Corresponding `Display\0000` and `Display\0001` entries

## Phase 2: Driver Installation Attempts

### Attempt 1: Detonator 5.32 (from driver catalog)

Located driver zip on `E:\Drivers\Geforce 2 GTS nvidia_9x_5.32.zip`. Extracted with 7-Zip on the machine. Manually copied 15 driver files to `C:\WINDOWS\SYSTEM` and wrote registry entries for Display\0000, OpenGL ICD, VGARTD, and CPL extension.

**Result**: Windows Protection Error on boot. Machine stuck on "New Hardware Found" wizard.

### Recovery from Protection Error

- **Safe Mode**: Win98 Safe Mode has no networking — agent couldn't connect to share
- **Safe Mode agent**: Agent crashes immediately (no Winsock in Safe Mode)
- **Step-by-step confirmation boot**: Still got protection error
- **Command prompt only boot**: Renamed NVCORE.VXD, NVMINI.VXD, NVMINI2.VXD, VGARTD.VXD to .bak — machine booted
- **Isolated the culprit**: NVCORE.VXD alone caused the crash. Renaming only NVCORE.VXD fixed boot.

### Attempt 2: Amigamerlin 82.69 (mdgx tweaked)

Cleaned all 5.32 files. Installed 82.69 from `E:\Drivers\nv8269`. Had to walk through New Hardware wizard pointing to files on C:\TEMP.

**Result**: Same crash — NVCORE.VXD causes Windows Protection Error on reboot.

### Attempt 3: Detonator 28.32 (downloaded)

Downloaded from philscomputerlab.com (known Win98-compatible build). Uploaded full 147-file package to `C:\TEMP\nv2832`. Ran the installer via `LAUNCH`.

**Result**: Installer completed successfully. Same crash — NVCORE.VXD causes Windows Protection Error.

### Hardware Swap Test

Swapped GeForce 2 GTS for an ASUS GeForce 2 GTS (initially thought to be a GeForce 256). Same crash pattern with NVCORE.VXD. This ruled out a faulty card — the problem was systemic.

## Phase 3: Root Cause Discovery

### The Clue

Three different driver versions from three different sources all crashed with the same VxD (NVCORE.VXD). The crash happened "while initializing IFSMgr" — the Installable File System Manager, which has nothing to do with video drivers. This pointed to a resource exhaustion problem, not a driver bug.

### The Root Cause: vcache Memory Bug

The machine had **1021MB RAM** with an **empty `[vcache]` section** in `C:\WINDOWS\SYSTEM.INI`.

Win98's vcache (disk cache) has no built-in upper limit. On machines with >512MB RAM, it grows to consume the entire available address space. VxDs (Virtual Device Drivers) share this address space. When NVCORE.VXD loads during early boot display initialization, it requests memory from an already-exhausted address space and crashes.

The crash manifests as "Windows Protection Error while initializing IFSMgr" because IFSMgr is one of the first VxDs to feel the pressure, but the actual trigger is any VxD that loads after the address space fills up.

### The Fix

Added one line to `C:\WINDOWS\SYSTEM.INI`:

```ini
[vcache]
MaxFileCache=262144
```

This limits the disk cache to 256MB (262144 KB), leaving sufficient VxD address space for NVCORE.VXD and other drivers.

**The machine booted perfectly.** All three driver versions would have worked from the start if this fix had been in place.

## Phase 4: Post-Fix Cleanup

### NVIDIA Installer Leftovers

The 28.32 installer created startup entries for utilities that were missing or broken:

| Run Key | Target | Problem |
|---------|--------|---------|
| `NvMediaCenter` | `RUNDLL32.EXE C:\WINDOWS\SYSTEM\NVMCTRAY.DLL,NvTaskbarInit` | NVMCTRAY.DLL doesn't exist |
| `nwiz` | `nwiz.exe /install` | Installer wizard, not needed at boot |

Removed via .reg file:
```reg
REGEDIT4

[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run]
"NvMediaCenter"=-
"nwiz"=-
```

### NVIDIA CPL Shell Extension

The Display Properties shell extension (`Display\0001\shellex\PropertySheetHandlers\NVIDIA CPL Extension`, CLSID `{67E0E3C0-3068-11D3-8BD1-00104B6F7516}`) caused rundll errors when opening Display Properties. The nvcpl.dll file exists but crashes when loaded.

Removed the shellex key:
```reg
REGEDIT4

[-HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Display\0001\shellex]
```

### Ghost Entry from Original Card

After swapping cards, two Display entries existed:
- `Display\0000` → original NVIDIA reference GeForce 2 GTS (SUBSYS_002E10DE) — ghost
- `Display\0001` → ASUS GeForce 2 GTS (SUBSYS_40161043) — active

Deleted Display\0000 and its PCI enum entry via agent commands.

### Resolution Change

The agent's `DISPLAYCFG set` command (which calls `ChangeDisplaySettingsA`) crashed Win98 hard. The display mode was changed by the crash itself (the mode stuck at 1024x768x16 after the crash-reboot). For future resolution changes on this machine, modify the registry `Mode` value in `Display\xxxx\DEFAULT` directly (format: `"bpp,width,height"` e.g., `"16,1024,768"`) and reboot.

### SYSFIX Agent Command

Built a new `SYSFIX` command into the agent to prevent this class of problems in the future. The command checks and applies standard Win98 fixes:

- **vcache**: Limits MaxFileCache for machines with >512MB RAM
- **conservative_swap**: Sets ConservativeSwapfileUsage=1 to reduce unnecessary paging
- **udma**: Enables DMA on IDE channels to prevent PIO mode fallback

New fixes can be added to the `all_fixes[]` table in `agent/src/sysfix.c`.

## Dashboard Connection Contention

A recurring obstacle throughout this case: the dashboard's connection pool held the agent's single TCP connection slot, blocking direct access.

**Problem**: The agent is single-threaded (`g_singlethread=1` in main.c) — it handles one TCP client at a time, inline. The dashboard backend maintains persistent `RetroConnection` objects in its connection pool. When the dashboard is running, it occupies the agent's only connection slot.

**Symptoms**: Direct `RetroConnection` attempts from the workstation time out or get refused. The dashboard auto-restarts via Docker restart policy even after being stopped.

**Solution**: Stop the dashboard container before direct agent work: `docker compose stop dashboard`. In some cases, needed `docker compose down` to prevent auto-restart.

**Agent crash on disconnect**: When the dashboard container is force-stopped, it sends a TCP RST to the Win98 Winsock stack. This can crash the agent or the entire machine. The agent's `recv_exact()` in protocol.c blocks indefinitely with no `SO_RCVTIMEO` — a future improvement.

## Card Identity Confusion

During testing, the user swapped in what they believed was a GeForce 256. PCISCAN reported PCI device ID `10DE:0150` (GeForce 2 GTS), not `10DE:0100` (GeForce 256). The card's subsystem ID `40161043` identifies it as an ASUS board. The ASUS V7700 series with this subsystem are GeForce 2 GTS variants. A true GeForce 256 would report device ID 0100 or 0101 on the PCI bus — the hardware registers don't lie.

## Timeline

| Step | Action | Result |
|------|--------|--------|
| 1 | PCISCAN + VIDEODIAG | Found ghosts (TNT2 Ultra, GF4 Ti4600), VGA mode |
| 2 | Cleaned ghosts, installed Detonator 5.32 | Windows Protection Error (NVCORE.VXD) |
| 3 | Safe Mode recovery, renamed NVCORE.VXD | Boots, card detected but error_code 24 |
| 4 | Installed Amigamerlin 82.69 | Same NVCORE.VXD crash |
| 5 | Installed Detonator 28.32 | Same NVCORE.VXD crash |
| 6 | Swapped to "GeForce 256" (actually ASUS GF2 GTS) | Same crash — ruled out faulty card |
| 7 | Added `MaxFileCache=262144` to SYSTEM.INI | **Boot success** — vcache was the root cause |
| 8 | Cleaned ghost Display entries, removed broken Run keys | Clean boot, 1024x768x16 |
| 9 | Built SYSFIX agent command | Automated vcache + other fixes for future machines |

## Lessons Learned

1. **vcache is the #1 thing to check on Win98 with >512MB RAM.** It causes crashes that perfectly mimic driver bugs. Three different driver versions "failed" when the real problem was a missing INI setting. Always run `SYSFIX apply` before any driver work.

2. **"While initializing IFSMgr" is a memory exhaustion signal**, not a filesystem driver bug. It means VxD address space is exhausted. The fix is always to limit vcache, not to debug the VxD named in the error.

3. **NVCORE.VXD is the canary.** NVIDIA's core VxD is a large, late-loading VxD that pushes address space over the edge. If NVCORE.VXD crashes but renaming it lets the machine boot, suspect vcache before suspecting the driver.

4. **Test with multiple driver versions before blaming hardware.** If three independent driver packages all fail the same way, the problem is environmental (OS config, memory, chipset), not the drivers or the card.

5. **PCI device IDs identify the GPU, not the board.** Users may misidentify their card based on stickers or box labels. The PCI device ID from PCISCAN is authoritative. Subsystem IDs identify the board manufacturer (ASUS, NVIDIA reference, etc.).

6. **NVIDIA 28.32 installer creates broken startup entries on Win98.** Always check `HKLM\...\Run` after installation and remove entries pointing to missing DLLs (NvMediaCenter/NVMCTRAY.DLL, nwiz).

7. **Don't use DISPLAYCFG set on Win98 with NVIDIA drivers.** The `ChangeDisplaySettings` API can crash the system. Change resolution via registry (`Display\xxxx\DEFAULT` Mode value) and reboot.

8. **The dashboard and direct agent access are mutually exclusive.** Plan for this — stop the dashboard before debugging, and restart it when done.

9. **Win98 Safe Mode has no networking.** The agent can't connect to shares, and may crash entirely if it depends on Winsock. Command-prompt-only boot is more useful for recovery — at least you can rename files.

10. **Upload the FULL driver package.** NVIDIA drivers have 100+ localized DLLs. If the installer can't find them, it prompts for a disk interactively. Upload everything to a local temp directory before running the installer.
