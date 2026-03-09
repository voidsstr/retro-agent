# Case Study 001: Voodoo 3 AGP Card - Ghost Devices, Remote Driver Install, and Display Configuration

## Problem Statement

A Windows 98 SE retro gaming PC (Intel 440BX chipset, Pentium III) had a 3dfx Voodoo 3 AGP card physically installed but Windows was running in generic VGA mode (640x480, 4-bit color). The system had a history of hardware changes:

1. A Voodoo 2 PCI card was installed previously but malfunctioned before becoming operational
2. Several Nvidia cards were used successfully after the Voodoo 2
3. A Voodoo 3 AGP card was installed in the AGP slot, confirmed visible in BIOS POST
4. Windows refused to detect the Voodoo 3 - no "New Hardware Found" dialog, no display driver, stuck on generic VGA

Multiple attempts had been made to install 3dfx drivers manually, including official Voodoo 3 driver kits and the Amigamerlin community driver pack, all without success.

## Environment

- **Machine**: Intel 440BX chipset, Pentium III, 383MB RAM
- **OS**: Windows 98 SE (4.10.2222 A)
- **Video Card**: 3dfx Voodoo 3 AGP (VEN_121A&DEV_0005&SUBSYS_1037121A&REV_01)
- **Network**: Intel Pro/100 Ethernet, IP 192.168.1.124
- **File Share**: SMB share at 192.168.1.122 with driver repository
- **Agent**: `retro_agent.exe` running on the Win98 machine
- **Controller**: Claude Code on a Linux workstation, communicating via `test_agent.py` / `retro_protocol.py`

## Phase 1: Investigation - Why Is the Voodoo 3 Invisible?

### Step 1: Initial System Survey

Connected to the agent and ran baseline diagnostics:

```
$ python3 test_agent.py 192.168.1.124 SYSINFO
$ python3 test_agent.py 192.168.1.124 VIDEODIAG
```

**SYSINFO** confirmed the system specs. **VIDEODIAG** revealed:
- **Zero display adapters** in the registry
- Display running at 640x480, 4-bit color depth, 0Hz refresh rate
- DirectX 4.08 installed
- The `Display` class in `HKLM\System\CurrentControlSet\Services\Class\Display` had **no subkeys** at all

### Step 2: PCI Bus Enumeration

```
$ python3 test_agent.py 192.168.1.124 'REGREAD HKLM Enum\PCI'
```

Found all PCI devices on the bus:

| Device | Description |
|--------|-------------|
| VEN_8086&DEV_7190 | Intel 440BX Host Bridge |
| VEN_8086&DEV_7191 | Intel 440BX AGP Bridge |
| VEN_8086&DEV_7110 | Intel PIIX4 ISA Bridge |
| VEN_8086&DEV_7111 | Intel PIIX4 IDE Controller |
| VEN_8086&DEV_7112 | Intel PIIX4 USB Controller |
| VEN_8086&DEV_7113 | Intel PIIX4 Power Management |
| VEN_8086&DEV_1229 | Intel Pro/100 Ethernet |
| **VEN_121A&DEV_0002** (x2) | **3dfx Voodoo 2 (ghost devices)** |
| VEN_1102&DEV_0002 | Creative Sound Blaster Live! |
| VEN_1102&DEV_7002 | Creative EMU10K1 Joystick |

**Critical finding**: `VEN_121A&DEV_0005` (Voodoo 3) was completely absent from the PCI registry despite being physically installed and visible in BIOS. Meanwhile, two entries for `VEN_121A&DEV_0002` (Voodoo 2) existed for hardware that was no longer in the machine.

### Step 3: Ghost Device Analysis

Drilled into the Voodoo 2 entries:

```
$ python3 test_agent.py 192.168.1.124 \
    'REGREAD HKLM Enum\PCI\VEN_121A&DEV_0002&SUBSYS_00000000&REV_02\BUS_00&DEV_0F&FUNC_00'
```

Both ghost Voodoo 2 entries had:
- `ConfigFlags = 00 00 00 00` (Windows considers them "present and working")
- Active LogConfig entries claiming memory ranges and IRQs
- Driver bindings to `MEDIA\0000` and `MEDIA\0003` (3D accelerator class)
- Manufacturer: "3Dfx Interactive, Inc."

These phantom devices were holdovers from the malfunctioned Voodoo 2 card. Windows never cleaned them up because the card failed before it was properly removed through Device Manager.

### Step 4: Driver Artifact Inventory

Searched `C:\WINDOWS\INF` for 3dfx-related files:

| File | Size | Date | Purpose | Matches Hardware? |
|------|------|------|---------|-------------------|
| `Voodoo3.inf` | 5,446 | 1999-11-23 | Official Voodoo 3 driver (DEV_0005) | Wrong card in system |
| `Voodoo.inf` | 59,594 | 2002-11-22 | Amigamerlin 2.9 (V3/V4/V5) | Wrong card in system |
| `DXMM3DFX.INF` | 2,003 | 1999-04-24 | DirectX multimedia (stock Win98) | N/A |
| `DXMM3DFX.PNF` | 4,976 | 2026-01-19 | Precompiled INF cache | Stale |

The INF files targeted the Voodoo 3 (DEV_0005), Voodoo 4/5 (DEV_0009), and Voodoo Banshee - none of which matched the ghost Voodoo 2 entries (DEV_0002), and the actual Voodoo 3 wasn't being detected by Windows to match against anything.

### Step 5: SYSTEM.INI Confirmation

```
$ python3 test_agent.py 192.168.1.124 'DOWNLOAD C:\WINDOWS\SYSTEM.INI'
```

```ini
[boot]
display.drv=pnpdrvr.drv

[boot.description]
display.drv=Standard PCI Graphics Adapter (VGA)

[386Enh]
display=*vdd,*vflatd
```

Confirmed: Windows was using the generic VGA driver with no accelerated display adapter.

## Root Cause

**Ghost PCI device entries** from the previously malfunctioned Voodoo 2 card were blocking Windows 98's PnP manager from properly re-enumerating the PCI/AGP bus.

On Windows 98 SE, PCI enumeration is cached in the registry at `HKLM\Enum\PCI`. When hardware is physically removed without being uninstalled through Device Manager, the registry entries persist as "phantom" devices. These ghosts:

1. **Claim system resources** (IRQs, memory ranges) that may conflict with new hardware on the AGP bus
2. **Confuse PnP enumeration** - Windows sees VEN_121A devices "already installed" and may skip full bus re-scanning
3. **Have ConfigFlags=0** meaning Windows considers them active and working, never triggering re-detection

The AGP bridge (Intel 82443BX, VEN_8086&DEV_7191) was present and functional, but zero devices had ever been enumerated behind it - meaning the Voodoo 3 (which sits on AGP bus 01) was never seen by Windows at all.

## Phase 2: Resolution - Ghost Cleanup and Detection

### Step 1: Remove Ghost Registry Entries

Created a .REG file to delete the phantom Voodoo 2 entries:

```reg
REGEDIT4

[-HKEY_LOCAL_MACHINE\Enum\PCI\VEN_121A&DEV_0002&SUBSYS_00000000&REV_02]
[-HKEY_LOCAL_MACHINE\Enum\PCI\VEN_121A&DEV_0002&SUBSYS_00000004&REV_02]
[-HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\MEDIA\0000]
[-HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\MEDIA\0003]
```

Uploaded and applied via the agent:

```
$ python3 -c "..." # Upload cleanup_ghosts.reg via UPLOAD command
$ python3 test_agent.py 192.168.1.124 \
    'EXEC regedit.exe /s C:\WINDOWS\TEMP\cleanup_ghosts.reg'
```

### Step 2: Remove Stale INF Files

Deleted driver artifacts from failed previous install attempts:

```
$ python3 test_agent.py 192.168.1.124 'EXEC attrib -r C:\WINDOWS\INF\Voodoo3.inf'
$ python3 test_agent.py 192.168.1.124 'DELETE C:\WINDOWS\INF\Voodoo3.inf'
$ python3 test_agent.py 192.168.1.124 'DELETE C:\WINDOWS\INF\Voodoo.inf'
$ python3 test_agent.py 192.168.1.124 'DELETE C:\WINDOWS\INF\DXMM3DFX.PNF'
```

### Step 3: Reboot

After reboot, Windows re-enumerated the PCI/AGP bus, detected the Voodoo 3 as new hardware, and presented the "New Hardware Found" wizard. The wizard was cancelled to allow remote driver installation.

## Phase 3: Remote Driver Installation

### Problem: INF Doesn't Match Card's Subsystem ID

The official Voodoo3.inf only lists specific SUBSYS variants:

```ini
%DeviceDesc%=Driver.Install,PCI\VEN_121A&DEV_0005&SUBSYS_0030121A ; AGP - #773
%DeviceDesc%=Driver.Install,PCI\VEN_121A&DEV_0005&SUBSYS_0037121A ; AGP - #762
; ... 12 specific SUBSYS variants
```

But this card's actual ID is `VEN_121A&DEV_0005&SUBSYS_1037121A` - not in the list. The wizard fell back to "Standard PCI Graphics Adapter" every time, even when pointed directly at the INF.

### Fix: Patch INF with Generic Match

Added a catch-all line to the `[Mfg]` section:

```ini
%DeviceDesc%=Driver.Install,PCI\VEN_121A&DEV_0005 ; Generic Voodoo3 (any SUBSYS)
```

### Stage Driver Files

Read the INF's `[DestinationDirs]` to determine correct file placement:
- `LDID_SYS (11)` = `C:\WINDOWS\SYSTEM` - driver binaries
- `LDID_WIN (10)` = `C:\WINDOWS` - glide2x.ovl
- `C:\WINDOWS\INF` - INF and catalog files

Copied all files from the network share via FILECOPY:

```
$ python3 test_agent.py 192.168.1.124 \
    'FILECOPY G:\Drivers\3DFX\...\3dfx16v3.drv|C:\WINDOWS\SYSTEM\3dfx16v3.drv'
$ python3 test_agent.py 192.168.1.124 \
    'FILECOPY G:\Drivers\3DFX\...\3dfxv3.vxd|C:\WINDOWS\SYSTEM\3dfxv3.vxd'
# ... (10 files total)
```

| File | Destination | Size | Purpose |
|------|-------------|------|---------|
| `3dfx16v3.drv` | `C:\WINDOWS\SYSTEM` | 390,288 | 16-bit GDI display driver |
| `3dfx32v3.dll` | `C:\WINDOWS\SYSTEM` | 502,784 | 32-bit display driver |
| `3dfxv3.vxd` | `C:\WINDOWS\SYSTEM` | 212,423 | VxD mini display driver |
| `glide2x.dll` | `C:\WINDOWS\SYSTEM` | 189,440 | Glide 2.x runtime |
| `glide3x.dll` | `C:\WINDOWS\SYSTEM` | 237,056 | Glide 3.x runtime |
| `3dfxSpl2.dll` | `C:\WINDOWS\SYSTEM` | 1,105,408 | Glide 2 splash |
| `3dfxSpl3.dll` | `C:\WINDOWS\SYSTEM` | 1,105,408 | Glide 3 splash |
| `3dfxOGL.dll` | `C:\WINDOWS\SYSTEM` | 1,211,904 | OpenGL ICD |
| `Vgartd.vxd` | `C:\WINDOWS\SYSTEM` | 25,106 | AGP GART VxD |
| `glide2x.ovl` | `C:\WINDOWS` | 183,347 | Glide overlay |
| `Voodoo3.inf` | `C:\WINDOWS\INF` | 4,962 | Patched INF |
| `Voodoo3.cat` | `C:\WINDOWS\INF` | 1,006 | Catalog file |

### Reboot and PnP Detection

After reboot, the Add New Hardware wizard detected "3dfx Voodoo3" (matching our patched INF) and installed the driver using the pre-staged files.

## Phase 4: Display Configuration

### Problem: Stuck at 640x480

After driver installation, VIDEODIAG showed the Voodoo 3 at 1024x768 32-bit color was supported in the driver's MODES registry, but Display Properties only offered 640x480. Investigation revealed the monitor driver was the bottleneck:

```
$ python3 test_agent.py 192.168.1.124 \
    'REGREAD HKLM System\CurrentControlSet\Services\Class\Monitor\0000'
```

```json
{
  "DriverDesc": "Default Monitor",
  "InfSection": "Unknown.Install"
}
```

The monitor's MODES registry only had a single entry: `640,480`. Windows limits available resolutions to the intersection of the video card's modes and the monitor's modes.

### Fix: Update Monitor Driver

Created a registry file to change the monitor type to "Super VGA 1280x1024" with standard CRT refresh rates:

```reg
REGEDIT4

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000]
"DriverDesc"="Super VGA 1280x1024"
"InfSection"="SVGA.Install.1280"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\640,480]
@="60,72,75,85"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\800,600]
@="56,60,72,75,85"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\1024,768]
@="60,70,75,85"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\1152,864]
@="60,70,75"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\1280,1024]
@="60,75"
```

### Problem: Display Class Numbering Mismatch

During cleanup, the Display class entries were inadvertently renumbered. The PCI enum entry's `Driver` value pointed to `DISPLAY\0001` but the driver entry was now at `DISPLAY\0000`. This caused a "display adapter conflicts with another device" error on boot.

Found the mismatch by inspecting the PCI device instance:

```
$ python3 test_agent.py 192.168.1.124 \
    'REGREAD HKLM Enum\PCI\VEN_121A&DEV_0005&SUBSYS_1037121A&REV_01\000800'
```

```json
{
  "Driver": "DISPLAY\\0001",   // <-- points to nonexistent entry
  "DeviceDesc": "3dfx Voodoo3",
  "ConfigFlags": "00 00 00 00"
}
```

Fixed by updating the Driver binding:

```
$ python3 test_agent.py 192.168.1.124 \
    'REGWRITE HKLM Enum\PCI\VEN_121A&DEV_0005&SUBSYS_1037121A&REV_01\000800 Driver REG_SZ DISPLAY\0000'
```

### Fix: Refresh Rate Optimization

The monitor is a Dell Trinitron G200 (Sony CPD-G200 rebrand) — a 19" CRT capable of 30-96kHz horizontal and 48-120Hz vertical. At 1024x768, it can do up to 85Hz. However, Display Properties didn't offer any refresh rate control because the Display driver's `RefreshRate` was set to `"-1"` (adapter default, typically 60Hz).

Updated the Display driver's default refresh rate and the monitor's MODES to match the G200's actual capabilities:

```reg
REGEDIT4

; Set refresh rate to 85Hz
[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Display\0000\DEFAULT]
"RefreshRate"="85"

; Update monitor to match Dell/Sony G200 Trinitron specs
[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000]
"DriverDesc"="Dell Trinitron G200"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\640,480]
@="60,72,75,85,100,120"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\800,600]
@="56,60,72,75,85,100"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\1024,768]
@="60,70,75,85"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\1280,1024]
@="60,75,85"

[HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Monitor\0000\MODES\1600,1200]
@="60,75"
```

### Final Result

After reboot, VIDEODIAG confirmed full functionality:

```json
{
  "adapters": [{
    "name": "3dfx Voodoo3",
    "pci_vendor_id": "0x121A",
    "pci_device_id": "0x0005",
    "error_code": 0,
    "status": "OK"
  }],
  "display": {
    "resolution": "1024x768",
    "color_depth": 32,
    "driver_desc": "3dfx Voodoo3"
  }
}
```

Display Properties now offers 640x480 through 1280x1024 at 8/16/24/32-bit color, with 85Hz refresh rate at 1024x768 on the Dell Trinitron G200 CRT.

## Phase 5: Startup Automation

Configured the retro PC for unattended operation:

### Agent Installation

```
C:\RETRO_AGENT\
  retro_agent.exe          # agent binary (110KB)
  startup.bat              # auto-start script
  agent.log                # runtime log
```

### Startup Script

```bat
@echo off
REM Start the retro agent - share mapping handled separately
start C:\RETRO_AGENT\retro_agent.exe -l C:\RETRO_AGENT\agent.log
```

Registered in `HKLM\Software\Microsoft\Windows\CurrentVersion\Run` as `RetroAgent`:

```
$ python3 test_agent.py 192.168.1.124 \
    'REGWRITE HKLM Software\Microsoft\Windows\CurrentVersion\Run RetroAgent REG_SZ C:\RETRO_AGENT\startup.bat'
```

On boot, the script launches the agent with file logging. Network share mapping is handled manually by the user through Windows Explorer after login.

### Network Share Mapping: Win98 Limitations

The initial plan was to automate `net use G: \\192.168.1.122\FILES` in the startup script, but this proved unreliable on Win98 SE due to several compounding issues:

**1. Win98 `net use` syntax differs from NT/2000/XP:**
- `/user:admin` — **not supported** on Win98 (only `/SAVEPW:NO`, `/YES`, `/NO`, `/DELETE`, `/HOME`)
- `/persistent:yes` — **not supported** on Win98
- Win98 syntax: `net use G: \\server\share password` (positional password only, no username field)

**2. SMB operations block the single-threaded agent:**
Any SMB call (`net use`, `WNetAddConnection2A` via NETMAP, even `CopyFileA` with UNC paths) goes through the Win98 networking stack and blocks the calling thread. Since the agent is single-threaded (required for Win98 Winsock compatibility), a blocking SMB call hangs the entire agent until the call completes or times out.

**3. SMB1 protocol requirement:**
Win98 can only speak SMB1/CIFS. The file share (a Buffalo NAS at 192.168.1.122) works when accessed manually through Windows Explorer but automated `net use` calls blocked indefinitely, likely due to SMB protocol negotiation issues in non-interactive contexts.

**4. Resource exhaustion from hung SMB calls:**
When `net use` or `wscript` processes hung waiting on SMB, they consumed system resources. After several hung attempts, Win98 could no longer create new processes (`CreateProcessA` returning error 0), requiring a reboot to recover.

The practical solution: the startup script only starts the agent. The user maps `G:` manually through Network Neighborhood after login (which has always worked). The agent's NETMAP command could be made non-blocking (background thread) in a future update if automated share mapping is needed.

## Technical Notes

### Agent Capabilities Used

| Command | Purpose |
|---------|---------|
| `PING` | Verify agent connectivity |
| `SYSINFO` | System specs and drive enumeration |
| `VIDEODIAG` | Display adapter and DirectX diagnostics |
| `REGREAD` | Registry inspection (PCI enum, driver class, monitor, config) |
| `REGWRITE` | Fix Driver binding, set Run key |
| `DOWNLOAD` | Retrieve SYSTEM.INI and INF files for analysis |
| `DIRLIST` | Inventory driver share and INF directory |
| `UPLOAD` | Send .REG files and driver files to target |
| `FILECOPY` | Copy driver files from network share to system directories |
| `EXEC` | Run regedit.exe, attrib.exe, rundll32 (reboot) on target |
| `DELETE` | Remove stale INF files |

### Agent Bug Fixes During Investigation

**EXEC handler (Win9x shell)**: Hardcoded `cmd.exe /c` doesn't exist on Win98 SE. Fixed by detecting the OS platform at runtime:

```c
osvi.dwOSVersionInfoSize = sizeof(osvi);
GetVersionExA(&osvi);
shell = (osvi.dwPlatformId == VER_PLATFORM_WIN32_NT)
        ? "cmd.exe /c " : "command.com /c ";
```

**REBOOT handler (Win9x privilege APIs)**: `OpenProcessToken`/`AdjustTokenPrivileges` are NT-only. On Win98, `ExitWindowsEx` alone doesn't reliably reboot from a console app. Working solution: `EXEC rundll32 shell32.dll,SHExitWindowsEx 6` (EWX_REBOOT|EWX_FORCE via the Shell API).

**Winsock threading**: `recv()` blocks indefinitely on Win98 when called on a socket accepted in a different thread. Fixed by making single-threaded mode the default.

### Lessons Learned

1. **Ghost PCI entries are invisible from the desktop** - no error messages, no Device Manager warnings. Only registry inspection reveals them.

2. **INF SUBSYS matching is strict** - The official Voodoo3.inf lists 12 specific SUBSYS variants but not all. Cards with unlisted subsystem IDs (like SUBSYS_1037121A) need a generic `PCI\VEN_121A&DEV_0005` match line added.

3. **Monitor driver caps resolution** - Even with a video driver supporting 2048x1536, Windows limits available modes to what the monitor driver advertises. A "Default Monitor" with only 640x480 in its MODES registry will lock you to that resolution.

4. **Display class numbering matters** - The PCI enum entry's `Driver` value (e.g., `DISPLAY\0001`) must point to an existing entry under `Services\Class\Display\`. If entries are renumbered, the binding must be updated or Windows reports a device conflict.

5. **Win98 .REG file key deletion is immediate** - Deleting a key like `Display\0000` causes Windows to renumber `Display\0001` to `0000`. A subsequent delete of `Display\0000` in the same .REG file will then delete what was originally `0001`. Always account for renumbering.

6. **Pre-staging driver files works** - Copying all driver binaries to their destination directories (`C:\WINDOWS\SYSTEM`, `C:\WINDOWS`, `C:\WINDOWS\INF`) before PnP detection means the wizard won't prompt for a disk.

7. **RefreshRate "-1" means adapter default** - The Display driver's DEFAULT key has a `RefreshRate` string value. `"-1"` means use the adapter default (typically 60Hz). Setting it to `"85"` explicitly forces 85Hz. The monitor's MODES registry must also list the desired rate for each resolution.

8. **Win98 `net use` is not NT-compatible** - The `/user:` and `/persistent:` flags don't exist on Win98. Authentication uses positional password only, with no way to specify a username. For user-level SMB auth, Win98 uses the logged-in Windows user or the interactive Map Network Drive dialog.

9. **SMB operations can hang the entire system** - On Win98, any SMB network call blocks the calling thread. In a single-threaded agent, this means NETMAP, FILECOPY with UNC paths, or EXEC `net use` will hang the agent. Multiple hung SMB processes can exhaust Win98 system resources, preventing any new process creation (CreateProcessA fails with error 0) until reboot.

10. **ExitWindowsEx doesn't reliably reboot from console apps on Win98** - Even with EWX_REBOOT|EWX_FORCE, it may flicker the screen without rebooting. The working alternative is `rundll32 shell32.dll,SHExitWindowsEx 6` via EXEC, which goes through the Shell API.

## Key Takeaway

What started as "card not detected" required solving seven distinct problems across five phases: ghost PCI entries, INF subsystem matching, driver file staging, monitor driver limitations, Display class registry numbering, refresh rate configuration, and network share automation. Each was invisible from the Win98 desktop but diagnosable through remote registry and file inspection.

The entire process — from initial diagnosis through 1024x768 32-bit color at 85Hz on a Dell Trinitron G200 CRT — was performed remotely without physical access to the machine, using only the retro agent's primitive operations orchestrated by an LLM.
