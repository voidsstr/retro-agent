# Retro Agent — Claude Code Instructions

## Session Startup — Chat Proxy Status Check (REQUIRED)

**On every Claude Code session start in this directory, IMMEDIATELY run the chat status check and echo a status message to the user.** Do this as your first action, before responding to any user message:

```bash
bash /home/voidsstr/development/retro-agent/scripts/chat_status.sh
```

The output will tell you and the user:
- Whether the `retro_chat_daemon.py` is running (✓ or ✗)
- How many retro agents are claimed
- Whether there are pending prompts in the inbox
- Whether the chat **brain** (processor) is alive (based on `processor.heartbeat`)

Echo a friendly status block to the user, e.g.:

```
Retro chat status:
  ✓ daemon running (pid 12345, claimed: .124, .143)
  ✗ chat brain: NOT RUNNING (or stale, last heartbeat 12 minutes ago)
  ✓ inbox: 0 pending prompts

To start the chat brain:  systemctl --user start retro-chat-brain
To restart the daemon:    bash /home/voidsstr/development/retro-agent/scripts/restart_daemon.sh
```

If the daemon is NOT running, offer to start it. If the brain is down, offer to start the service.

### The chat processor is now a standalone service (the "brain")

The processor that answers chat prompts is **`scripts/retro_chat_brain.py`** — a
long-running service built on the **Claude Agent SDK** (the same engine Claude
Code runs on). It replaces the old "spawn a Claude Code subagent" approach, so
the retro chat works **even when no Claude Code session is open**. It has the full
Claude Code tool suite on this host plus `mcp__retro__*` tools that operate the
fleet. Full docs: [`scripts/README-chat-brain.md`](scripts/README-chat-brain.md).

When the user asks "start the chat processor"/"start the chat brain":
```bash
systemctl --user start retro-chat-brain     # if the unit is installed
# or, without systemd:
nohup bash /home/voidsstr/development/retro-agent/scripts/retro_chat_brain_supervisor.sh \
  > /tmp/retro-chat/brain-sup.log 2>&1 &
```
Do **not** spawn an Agent-tool subagent for this anymore — the service is the processor.

## Repository Context

This repo was extracted from the `nsc-assistant` monorepo. The dashboard, MCP server, and OpenClaw agents remain in `nsc-assistant`. This repo contains only the agent binaries, Python client library, provisioning scripts, and documentation.

The `nsc-assistant` dashboard still imports the Python client (`shared/retro_protocol.py` and `shared/retro_discovery.py`). Those files are duplicated here as `client/`. When modifying the protocol, update both repos.

## Build & Deploy

### Windows Agent

```bash
cd agent && make clean && make
# Output: agent/retro_agent.exe

# Upload to SMB share (distribution point for all retro machines)
curl --upload-file agent/retro_agent.exe -u YOUR-CREDS \
  "smb://YOUR-SERVER/files/Utility/Retro%20Automation/retro_agent.exe"
```

`make release` — bumps patch version, tags, builds, uploads to share in one step. `make release BUMP=minor|major` for minor/major bumps.

After building the agent, also rebuild the `nsc-assistant` dashboard (it embeds the agent binary for the "Update Agent" button):
```bash
cd /home/voidsstr/development/nsc-assistant && docker compose up -d --build dashboard
```

### Linux Agent

```bash
cd agent-linux && make clean && make
curl --upload-file agent-linux/retro_agent_linux -u YOUR-CREDS \
  "smb://YOUR-SERVER/files/Utility/Retro%20Automation/retro_agent_linux"
```

## Using the Agent from an LLM

The retro agent is operated by LLMs through the Python client library in `client/`. An LLM connects to agents over TCP, sends commands, and processes responses — enabling autonomous diagnostics, software installation, hardware configuration, and GUI automation on retro PCs.

### Connecting

```python
import asyncio
from client.retro_protocol import RetroConnection

async def run():
    conn = RetroConnection('10.0.0.50', 9898)
    await conn.connect('retro-agent-secret', timeout=15.0)
    # ... send commands ...
    await conn.close()

asyncio.run(run())
```

When running from the `nsc-assistant` repo, use `from shared.retro_protocol import RetroConnection` instead.

### Command Patterns

```python
# Text command — returns string, raises RetroProtocolError on error
text = await conn.command_text('SYSINFO')

# Binary command — returns bytes (screenshots, file downloads)
bmp_data = await conn.command_binary('SCREENSHOT 0')

# Raw command — returns (status_byte, data_bytes)
status, data = await conn.send_command('EXEC dir C:\\WINDOWS')

# Upload — two-frame protocol (command + binary payload)
await conn.send_command('UPLOAD C:\\path\\file.reg', binary_payload=reg_bytes)
```

### LLM Diagnostic Workflow

A typical LLM-driven diagnostic session:

1. **Connect**: `RetroConnection(host, 9898)` → `connect(secret)`
2. **Assess**: `SYSINFO`, `VIDEODIAG`, `AUDIOINFO`, `PCISCAN` for hardware inventory
3. **Investigate**: `REGREAD` for driver config, `PROCLIST` for running processes, `DIRLIST` for files
4. **Fix**: Upload `.reg` files via `UPLOAD` + `EXEC regedit /s`, copy drivers from share via `EXEC copy`, apply `SYSFIX`
5. **Verify**: Re-run diagnostic commands, take `SCREENSHOT` to confirm
6. **Reboot** if needed (only with user approval — machines may require physical access)

### LLM GUI Automation Workflow

For installing drivers/software with a GUI:

1. Upload or copy installer to machine
2. `LAUNCH installer.exe` (never EXEC for GUI apps — it runs them hidden)
3. **Screenshot-click loop**: `SCREENSHOT 0` → analyze with vision → `UICLICK x y` or `UIKEY keyname`
4. Post-install cleanup: upload `.reg` to delete broken Run keys, shell extensions
5. `REBOOT` and verify with `VIDEODIAG`/`AUDIOINFO`

### LLM Fleet Management

```python
from client.retro_discovery import discover_retro_pcs

# Find all agents on the LAN
pcs = await discover_retro_pcs(timeout=3.0)
for pc in pcs:
    conn = RetroConnection(pc.ip, pc.port)
    await conn.connect(secret)
    # Run inventory, apply updates, check health
    await conn.close()
```

### Critical Rules for LLM Usage

- **One connection at a time** per agent. The agent is single-threaded. Close connections promptly.
- **EXEC = CLI only** (hidden, blocks, captures output). **LAUNCH = GUI only** (visible, returns PID). Mixing them hangs the agent.
- **Win98 shell escaping**: `<>` in `echo` commands are interpreted as redirects by `command.com`. Use `UPLOAD binary_payload` to write files with special characters instead.
- **FILECOPY delimiter**: `FILECOPY src|dst` (pipe, not space).
- **REGREAD format**: `REGREAD HKLM Path\\To\\Key` (root and path space-separated).
- **Screenshots are raw BMP**: Convert to PNG with Pillow before passing to a vision model or saving.
- **REBOOT/SHUTDOWN require confirmation**: These machines need physical access to recover. Never issue without explicit user approval.
- **Win98 RST crash**: Abrupt TCP disconnects crash Win98 Winsock. Always `await conn.close()` gracefully. Never kill connections to Win98 agents.

## Agent Command Reference

### System Info
- **PING** — returns "PONG"
- **SYSINFO** — JSON: CPU, memory, OS, drives, uptime
- **VIDEODIAG** — video card, driver, PCI IDs, resolution, DirectX
- **AUDIOINFO** — audio device enumeration
- **SMARTINFO** — S.M.A.R.T. disk health
- **DISPLAYCFG** — display config and refresh rate
- **PCISCAN** — PCI device enumeration with vendor/device IDs

### Execution
- **EXEC cmd** — run hidden, capture output, block until exit (60s timeout)
- **LAUNCH cmd** — run visible, return immediately with `{pid, command}`

### Process Control
- **PROCLIST** — JSON list of running processes
- **PROCKILL pid** — terminate process by PID
- **QUIT** — stop agent gracefully (for updates)
- **SHUTDOWN** — power off machine
- **REBOOT** — restart machine

### File Operations
- **DIRLIST path** — JSON directory listing
- **UPLOAD path** — upload file (two-frame: command + binary payload)
- **DOWNLOAD path** — download file (binary response)
- **MKDIR path** — create directory (recursive)
- **DELETE path** — delete file
- **FILECOPY src|dst** — copy file (pipe-delimited)

### UI Automation (Windows)
- **SCREENSHOT quality** — raw 24-bit BMP (0=full, 1=half, 2=quarter)
- **UICLICK x y [button]** — click at coordinates (left/right/middle)
- **UIKEY keyname** — send keystroke (uses MapVirtualKey scan codes)
- **WINLIST** — JSON list of visible windows

### Registry (Windows)
- **REGREAD root path** — read value or enumerate keys
- **REGWRITE root path value type** — write value
- **REGDELETE root path** — delete value or key

### Network (Windows)
- **NETMAP unc [drive] [user] [password]** — map network share
- **NETUNMAP drive** — disconnect mapped drive

### Hardware (Windows)
- **DRVSNAPSHOT** — capture driver configuration state
- **SYSFIX [check|apply]** — check/apply Win98 system fixes

### Linux-Only
- **PKGINSTALL name** — install package (auto-detects apt/yum/pacman)
- **PKGLIST** — list installed packages
- **SVCINSTALL** — manage systemd services

## Taking Screenshots

Agent returns raw 24-bit BMP. Always convert to PNG:

```python
import os
from PIL import Image

data = await conn.command_binary('SCREENSHOT 0')
os.makedirs('/tmp/retro-screenshots', exist_ok=True)
with open('/tmp/retro-screenshots/screen.bmp', 'wb') as f:
    f.write(data)
img = Image.open('/tmp/retro-screenshots/screen.bmp')
img.save('/tmp/retro-screenshots/screen.png', optimize=True)
# Now read screen.png to view
```

Zoom into regions for detail: crop with Pillow and resize with `Image.NEAREST`.

## Win98 Known Issues & Fixes

### SYSFIX Command

Built into the agent. Always run `SYSFIX apply` on any new Win98 machine.

- `SYSFIX check` — report issues (read-only)
- `SYSFIX apply` — fix all issues

### vcache (Critical)

Win98 with >512MB RAM and no `MaxFileCache` limit: disk cache exhausts VxD address space → "Windows Protection Error" on boot. Looks like a driver problem but it's a memory bug. `SYSFIX apply` adds `MaxFileCache=262144` to SYSTEM.INI.

### Ghost PCI Entries

Removed hardware leaves registry entries that block PnP. `PCISCAN` shows ghosts. Delete via `.reg` file upload + `EXEC regedit /s`.

### Registry .reg Files on Win98

Win98 has no `reg.exe`. Write `.reg` files and apply with `EXEC regedit /s`:

```python
# Delete a registry key
reg = b'REGEDIT4\r\n\r\n[-HKEY_LOCAL_MACHINE\\Path\\To\\Key]\r\n'
await conn.send_command('UPLOAD C:\\WINDOWS\\TEMP\\fix.reg', binary_payload=reg)
await conn.command_text('EXEC regedit /s C:\\WINDOWS\\TEMP\\fix.reg')
```

### Win98 RST Crash

Abrupt TCP disconnects crash Win98 Winsock, taking down the whole machine. Always close connections gracefully. Never stop a dashboard/proxy while Win98 agents are connected.

## Debugging & Direct Agent Access

Connect directly over TCP for raw protocol access:

```python
import asyncio
from client.retro_protocol import RetroConnection

async def run():
    c = RetroConnection('AGENT_IP', 9898)
    await c.connect('retro-agent-secret', timeout=15.0)
    status, data = await c.send_command('COMMAND_HERE')
    print(data.decode('ascii', errors='replace'))
    await c.close()

asyncio.run(run())
```

### Finding Agents on the LAN

```python
async def scan_subnet():
    tasks = []
    for i in range(1, 255):
        tasks.append(try_host(f'10.0.0.{i}'))
    await asyncio.gather(*tasks)

async def try_host(ip):
    try:
        c = RetroConnection(ip, 9898)
        await c.connect('retro-agent-secret', timeout=2.0)
        status, data = await c.send_command('SYSINFO')
        print(f'{ip}: {data.decode("ascii", errors="replace")[:100]}')
        await c.close()
    except Exception:
        pass
```

## Remote Driver Installation

See the case studies in `docs/case-studies/` for detailed real-world examples of:
- Ghost PCI device cleanup and driver installation (Voodoo3)
- vcache diagnosis and NVIDIA driver installation (GeForce2 GTS)

General pattern:
1. `SYSFIX apply` (always first on Win98)
2. Clean ghost PCI entries via `PCISCAN` + `.reg` file
3. Stage driver files (UPLOAD or copy from SMB share)
4. `LAUNCH` installer, walk GUI via screenshot-click loop
5. Post-install registry cleanup (broken Run keys, CPL extensions)
6. `REBOOT` and verify with `VIDEODIAG`

### 3dfx Driver Installation (Voodoo 3/4/5) — Critical Notes

**DO NOT manually create Display class registry entries for 3dfx drivers.** Unlike NVIDIA, the 3dfx VxD (`3dfxvs.vxd`) requires full Win98 PnP context to initialize the hardware. Manually creating `Display\0000` entries results in VIDEODIAG showing `status: OK` but the display stays at 640x480 4-bit VGA — the VxD never actually initializes and the driver silently falls back.

**DO NOT add `device=3dfxvs.vxd` to `[386Enh]` in SYSTEM.INI.** It's a minivdd (miniport) loaded by `*vdd` during PnP, not a standalone VxD. Loading it via `device=` causes a Windows Protection Error.

**The Amigamerlin `Driver Install.exe` only copies files and INFs** — it does NOT create registry entries or configure PnP.

**What actually works:**
1. `SYSFIX apply`, clean ghost PCI entries from old cards
2. Copy Amigamerlin package to machine from SMB share
3. Run `Driver Install.exe` via `LAUNCH DRIVER~1.EXE` (use short name — spaces in filenames break LAUNCH on Win98)
4. Delete competing old INFs from `C:\WINDOWS\INF\OTHER\` (keep only the Amigamerlin `Voodoo.inf`)
5. Reboot → Win98 PnP detects card → prompts "Insert disk labeled 3dfx Voodoo driver installer disk"
6. Point the disk prompt to the `driver9x\` directory (e.g., `C:\WINDOWS\Desktop\am29win9x\driver9x`) which has `Voodoo.inf` and all driver files
7. Let PnP complete → reboot again → driver activates at correct resolution

**Best driver:** Amigamerlin 2.9 for all Voodoo 3/4/5 cards. INF section `Driver.InstallV3` for Voodoo3, `Driver.InstallV5` for Voodoo4/5.

## Known Machines

| IP | Hostname | OS | Hardware Notes |
|----|----------|----|----|
| 10.0.0.50 | Q0Q1G8 | Win98 4.10 | Voodoo5 5500 AGP, AWE64 PnP |
| 10.0.0.51 | VOIDSSTR-YOR7S5 | Win2K 5.0 | GeForce 2 GTS, SB Live |
| 10.0.0.52 | 2004-XP | Windows XP | 2047MB RAM |

## Windows XP / 2003 Offline Activation (`scripts/xp-activation/`)

Microsoft's XP activation servers (internet + automated phone) are dead, so XP
boxes that fall out of activation can't reach Microsoft. `scripts/xp-activation/`
generates a valid **Confirmation ID** from the on-screen **Installation ID**
completely offline — reproducing what the phone system used to return (not a
crack; patches nothing on the target).

```bash
cd scripts/xp-activation && ./build.sh                  # builds ./xpcid (first run)
./xpcid <9 groups of 6 digits from the activation wizard>   # prints Confirmation ID
```

Get the Installation ID on the XP machine via the lockout screen's **telephone
activation** option (no login needed; Safe Mode works if normal login is blocked),
or `msoobe.exe /a` from a desktop. The same screen accepts the Confirmation ID
back. The `/xp-activation` skill walks the whole flow; see
[`scripts/xp-activation/README.md`](scripts/xp-activation/README.md) for details.

## SMB File Share

`smb://YOUR-SERVER/files/Utility/Retro Automation/` — distribution point for agent binaries.

Upload: `curl --upload-file file -u YOUR-CREDS "smb://YOUR-SERVER/files/Utility/Retro%20Automation/file"`

## Linux Game Servers (`scripts/game-servers/`)

Idempotent installers for UT2004 3369.3, UT99 (OldUnreal 469e), Yamagi Quake 2, and OpenArena (Q3-compatible) dedicated servers. All four run as `systemctl --user` units with `loginctl enable-linger`. See [`scripts/game-servers/README.md`](scripts/game-servers/README.md) for the full walkthrough.

**Quick install:** `cd scripts/game-servers && ./install-all.sh`

Updating the retro XP fleet to the matching **UT99 469e client**:
```bash
python3 scripts/game-servers/push-ut99-xp-patch.py 192.168.1.143 192.168.1.133 ...
```

**Pre-install multiplayer download packs** (Q3 mod paks, UT99 custom maps, UT2004 bonus-pack maps, Q2 mod content) so retro machines skip the auto-download phase when joining public servers:
```bash
./scripts/game-servers/push-all-mp-paks.sh 192.168.1.143 192.168.1.133 ...
```
Share layout: `\\192.168.1.122\files\Game Updates\{Q3,UT99,UT2004,Q2}-Multiplayer\` with subdirs matching each game's on-disk structure. Push scripts use `cmd /c copy /Y` (NOT `xcopy` — xcopy hangs silently when the source is on a NETMAP'd SMB drive on WinXP).

### Fleet install paths — game-specific gotchas

- **Quake 3:** the NSC fleet convention is `C:\Quake III Arena\Quake3\` — note the extra `\Quake3\` subdir. `pak0.pk3` lives at `C:\Quake III Arena\Quake3\baseq3\pak0.pk3`, mod paks go into sibling subdirs (`\Quake3\cpma\`, `\Quake3\osp\`, etc.). The `push-q3-mp-paks.py` candidate-path list has the double-nested path first so it always wins over a hypothetical `C:\Quake III Arena\baseq3\` layout.
- **UT99:** installs can be either `C:\UT\` or `C:\UnrealTournament\`. The push script checks both.
- **Install detection:** don't use `DIRLIST` to probe for a game install — it returns status=0 (failure) for paths containing spaces on some XP agents. Use `EXEC cmd /c if exist "..." (echo FOUND) else (echo MISSING)` instead — that works reliably regardless of spaces.
- **ZIP extraction on XP:** PowerShell is 2.0 so `Expand-Archive` isn't available. The Q3 push script ships a JScript Shell.Application shim (`q3_unzip.js`) and **busy-waits for the destination Items.Count to stabilize** — `CopyHere` is asynchronous and returns before the extract finishes. Forgetting to wait causes the caller to `del` the staging zip mid-extract.

**Known gotchas** (all documented in the scripts themselves):

- **Stock UT2004.ini has `0x1B` (ESC) bytes** embedded in 4 maplist section headers (`[DefaultDM MaplistRecord]`, `[DefaultTDM MaplistRecord]`, `[1on1Deathmatch MaplistRecord]`, `[1on1TeamDeathmatch MaplistRecord]`). Any section-header string comparison fails until those are stripped.
- **Archive.org's UT2004 zip uses LZMA (method 14)** inside ZIP; Info-Zip's `unzip` fails silently ("1660 files skipped because of unsupported compression"). Use Python's `zipfile` or `7z`.
- **Epic's master servers are dead since 2023.** For UT2004, use the MasterServerMirror mod for multi-master uplink to community replacements (333networks, OpenSpy, errorist.eu). UT99's OldUnreal 469e ships with 8 community masters pre-configured — no extra mod needed.
- **Yamagi Quake 2 rejects the `serverinfo` flag suffix** that stock Q2 accepted. `set x "val" serverinfo` → error "flags can only be 'u' or 's'". Use plain `set x "val"`.
- **`openarena-server` Debian package auto-enables a system-wide service** that grabs UDP 27960 with a default-config `noname` instance. Symptom: your user-unit's `sv_hostname` appears in the log but the external status query shows `noname` / `fraglimit 20` / `maxclients 8`. Fix: `sudo systemctl disable --now openarena-server.service`.
- **UT99 OldUnreal 469e ServerPackages references missing skin packs** (`TCowMeshSkins`, `TNaliMeshSkins`, `TSkMSkins`) from the Epic bonus pack. Stock UT99 installs don't include them. Leaving them in `UnrealTournament.ini` aborts server startup with `Failed to load TCowMeshSkins`. Install script removes these lines.
- **UT99 default port (7777) collides with UT2004's.** Shift UT99 to `-port=7797` on the command line; query port auto-shifts to 7798.
- **AT&T BGW gateways bind NAT rules by MAC, not IP.** If your server host has both Wi-Fi and Ethernet, the "2026-5090" device list shows **two** entries — one per NIC — with the same hostname. Picking the Wi-Fi MAC creates asymmetric routing (inbound → .129, outbound from .132) and masters silently drop the listing. Always pick the wired-adapter MAC explicitly in Firewall → NAT/Gaming.
- **AT&T BGW NAT rules can spontaneously re-bind** to whatever device currently holds the IP in the router's DHCP table. If game-server rules suddenly show a different device (e.g. "HP Inc."), `/cgi-bin/apphosting.ha` needs the rules deleted and re-added against the wired MAC. `/tmp/att-router/` has the session scripts used for this.

**Ports to forward (UDP, to the wired LAN IP):**
- `7777-7787` — UT2004 (game, browser, gamespy query)
- `7797-7798` — UT99 (game, query — shifted to avoid UT2004 collision)
- `27910` — Quake 2
- `27960` — OpenArena / Q3
