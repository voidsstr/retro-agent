# Retro Agent — AI-Powered Remote Management for Retro PCs

**Give your Pentium II a smarter assistant than most developers had in 2003.**

A ~200KB C binary that runs on Windows 98 SE, 2000, and XP (plus Linux). It exposes system info, file IO, registry, process control, UI automation, and hardware diagnostics over a simple TCP protocol — designed from the ground up to be operated by LLMs.

Connect from Python, pipe it to Claude or GPT, and suddenly your vintage hardware has a state-of-the-art AI that can diagnose why your Sound Blaster isn't working, install NVIDIA drivers via GUI automation, or configure your Voodoo 5500 for optimal Glide performance.

The built-in **Retro Chat** client takes it further: type a prompt directly on the retro PC's console, and a Claude Code subagent on your modern dev box processes it with the *full Claude toolbox* — then streams the response back to your 16-color Win98 terminal.

## Retro Chat — AI on a 25-year-old OS

| Welcome screen | Claude is working | Response rendered |
|---|---|---|
| ![welcome](docs/images/retro-chat-welcome.png) | ![working](docs/images/retro-chat-working.png) | ![completed](docs/images/retro-chat-completed.png) |
| *Launch `retro_chat.exe` on any retro PC.* | *Green = what the AI is doing right now. Yellow = waiting for first token.* | *Response streams in real-time, word-wrapped for the console.* |

### How the chat pipe works

```
   Retro PC (Win98/2K/XP)                          Your dev box (Linux/macOS)
 ┌─────────────────────────────────┐      LAN      ┌────────────────────────────────┐
 │  retro_chat.exe  ──localhost──► │               │  retro_chat_daemon.py          │
 │                                 │               │  (pure network multiplexer)    │
 │  retro_agent.exe ◄──── TCP ─────┤───────────────┤         │                      │
 │   (chatproxy + status bus +     │   port 9898   │         ▼ /tmp/retro-chat/     │
 │    long-polling events)         │               │  inbox/*.json  outbox/*.json   │
 └─────────────────────────────────┘               │   status_outbox/*.json         │
                                                   │         │                      │
                                                   │         ▼                      │
                                                   │  Claude Code background agent  │
                                                   │  (full tools: Read, Bash,      │
                                                   │   Grep, RetroConnection, etc.) │
                                                   └────────────────────────────────┘
```

Three threads run inside `retro_chat.exe`, each on its own socket:
- **`LOG_WAIT`** long-poller — streams response chunks with sub-100ms latency
- **`STATUS_WAIT`** long-poller — shows `[subagent: ...]` live activity in green
- A local spinner thread that animates `* Working... |/-\` with zero network traffic

Zero polling overhead. Zero CPU when idle. The kernel parks each socket in `recv()` until there's something to show.

---

## What Can You Do With It?

### Diagnose Hardware Problems

```
You: "My Sound Blaster isn't working after I installed the new video card"

Claude: I'll check your audio setup...
  [subagent: SYSINFO]
  [subagent: AUDIOINFO]
  [subagent: PCISCAN]
  [subagent: REGREAD HKLM System\CurrentControlSet\...]

Your system has a Sound Blaster 16 (PCI ID 1274:1371) but no
wave driver is loaded. The IRQ conflict with your new GeForce
is preventing the driver from initializing.

I've updated the PCI resource assignments and reloaded the
driver. Try playing a WAV file now.
```

### Install Drivers via GUI Automation

The agent can see the screen, click buttons, and type keystrokes — so it can walk through any installer that requires user interaction:

```python
# Upload the driver installer
await conn.send_command('LAUNCH C:\\TEMP\\nvidia_setup.exe')
await asyncio.sleep(3)

# See what's on screen
bmp = await conn.command_binary('SCREENSHOT 0')

# Click "Next" at coordinates the LLM identified
await conn.send_command('UICLICK 450 380')

# Use Tab + Space for tricky dialogs where clicks don't register
await conn.send_command('UIKEY TAB')
await conn.send_command('UIKEY SPACE')
```

### Manage a Fleet of Retro PCs

```python
from client.retro_discovery import discover_retro_pcs

# Find every retro agent on the LAN
pcs = await discover_retro_pcs(timeout=3.0)
for pc in pcs:
    print(f"{pc.hostname} at {pc.ip} — {pc.os}")

# Run diagnostics on each
for pc in pcs:
    conn = RetroConnection(pc.ip, 9898)
    await conn.connect(SECRET, timeout=15.0)
    _, data = await conn.send_command('SYSINFO')
    info = json.loads(data.decode())
    print(f"  RAM: {info['memory']['total_mb']}MB, "
          f"OS: {info['os']['product']}")
    await conn.close()
```

### Monitor System Health

```python
# Check disk health
_, data = await conn.send_command('SMARTINFO')
smart = json.loads(data.decode())
for disk in smart.get('disks', []):
    print(f"Disk {disk['model']}: temp={disk.get('temperature')}C")

# Check video card
_, data = await conn.send_command('VIDEODIAG')
video = json.loads(data.decode())
print(f"GPU: {video['adapters'][0]['name']}")
print(f"Driver: {video['adapters'][0]['driver_version']}")
print(f"Resolution: {video['display']['resolution']}")
```

### Automate Software Installation

```python
# Copy installer from a network share
await conn.command_text(
    r'EXEC copy "\\server\share\game_setup.exe" C:\TEMP\setup.exe'
)

# Launch the installer (LAUNCH for GUI, never EXEC)
await conn.send_command(r'LAUNCH C:\TEMP\setup.exe')

# Wait for it to appear, then automate the install wizard
await asyncio.sleep(5)
bmp = await conn.command_binary('SCREENSHOT 0')
# ... LLM analyzes screenshot, clicks through wizard ...
```

### Fix Win98 Boot Problems

```python
# The built-in SYSFIX command handles common Win98 issues
text = await conn.command_text('SYSFIX check')
print(text)  # Shows what needs fixing

text = await conn.command_text('SYSFIX apply')
print(text)  # Applies all fixes:
             #   - vcache limit for >512MB RAM
             #   - swap file configuration
             #   - DMA settings
             #   - autologon setup
```

### Take and Analyze Screenshots

```python
from PIL import Image
import io

# Capture the screen at full resolution
bmp_data = await conn.command_binary('SCREENSHOT 0')

# Convert raw BMP to PNG for analysis
img = Image.open(io.BytesIO(bmp_data))
img.save('screen.png')

# Crop a specific region for detail
detail = img.crop((100, 100, 500, 400))
detail.save('detail.png')

# Pass to a vision model for analysis
# (Claude, GPT-4V, etc. can read the PNG directly)
```

### Registry Surgery

```python
# Read a registry value
_, data = await conn.send_command(
    r'REGREAD HKLM SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
)
run_keys = json.loads(data.decode())
print(f"Startup programs: {run_keys.get('values', [])}")

# Write a registry value
await conn.send_command(
    r'REGWRITE HKLM SOFTWARE\MyApp\Settings\Volume 75 REG_DWORD'
)

# Delete a registry key
await conn.send_command(
    r'REGDELETE HKLM SOFTWARE\UnwantedApp'
)
```

---

## Architecture

```
                    TCP :9898               UDP :9899
  ┌──────────┐    <------------->    ┌─────────────────┐
  |  Client   |    commands/         |   retro_agent    |
  | (Python)  |    responses         |  (C, ~200KB)     |---> broadcasts
  └──────────┘                       └─────────────────┘    discovery
                                            |
                                     Win98/XP/2K/Linux
```

**Agent** (`agent/`): Cross-compiled C binary for Windows (MinGW-w64, i586 target). Runs as console app or NT service. ~200KB, no runtime dependencies beyond system DLLs.

**Linux Agent** (`agent-linux/`): Native C binary for x86_64 and ARM. Subset of Windows commands adapted for POSIX.

**Python Client** (`client/`): Async TCP client library. Handles framing, authentication, command dispatch, and LAN discovery.

## Protocol

Length-prefixed binary frames over TCP:

```
Send:    [uint32 LE length][payload bytes]
Receive: [uint32 LE length][status_byte][data bytes]

Status bytes:
  0x00 = text response (ASCII)
  0x01 = binary response (screenshots, file downloads)
  0xFF = error (ASCII error message)
```

Authentication: first frame must be `AUTH <secret>`. Set the secret with `-s <secret>` when starting the agent.

Discovery: agents broadcast `RETRO|hostname|ip|port|os|cpu|ram_mb|os_family` on UDP 9899 every 30 seconds.

## Command Reference

### System Info
| Command | Description | Response |
|---------|-------------|----------|
| `PING` | Health check | `PONG` |
| `SYSINFO` | CPU, memory, OS, drives, uptime | JSON |
| `VIDEODIAG` | Video card, driver, PCI IDs, resolution, DirectX | JSON |
| `AUDIOINFO` | Audio device enumeration | JSON |
| `SMARTINFO` | S.M.A.R.T. disk health | JSON |
| `DISPLAYCFG get` | Display resolution, color depth, refresh rate | JSON |
| `DISPLAYCFG set <w> <h> <bpp> [hz]` | Change display mode | JSON |
| `PCISCAN` | PCI device enumeration with vendor/device IDs | JSON |

### Execution
| Command | Description | Response |
|---------|-------------|----------|
| `EXEC <cmd>` | Run hidden, capture stdout/stderr, block (60s timeout) | text |
| `LAUNCH <cmd>` | Run visible, return immediately | JSON `{pid, command}` |

**Important:** Use `EXEC` for CLI commands that produce text output. Use `LAUNCH` for GUI apps (installers, games, etc.). Using `EXEC` for a GUI app runs it invisibly and blocks the agent.

### Process Control
| Command | Description |
|---------|-------------|
| `PROCLIST` | JSON list of running processes |
| `PROCKILL <pid>` | Terminate process by PID |
| `QUIT` | Stop agent gracefully (for updates) |
| `SHUTDOWN` | Power off machine |
| `REBOOT` | Restart machine |

### File Operations
| Command | Description |
|---------|-------------|
| `DIRLIST <path>` | JSON directory listing (name, size, modified, attributes) |
| `UPLOAD <path>` | Upload file (two-frame: command + binary payload) |
| `DOWNLOAD <path>` | Download file (binary response) |
| `MKDIR <path>` | Create directory (recursive) |
| `DELETE <path>` | Delete file |
| `FILECOPY <src>\|<dst>` | Copy file (pipe-delimited src and dst) |

### UI Automation (Windows)
| Command | Description |
|---------|-------------|
| `SCREENSHOT <quality>` | Capture screen as raw 24-bit BMP. 0=full, 1=half, 2=quarter |
| `UICLICK <x> <y> [button]` | Click at coordinates (left/right/middle) |
| `UIDRAG <x1> <y1> <x2> <y2>` | Drag from one point to another |
| `UIKEY <keyname>` | Send keystroke (e.g. `TAB`, `ENTER`, `ALT SPACE`) |
| `WINLIST` | JSON list of visible windows (hwnd, title, class, rect) |

### Registry (Windows)
| Command | Description |
|---------|-------------|
| `REGREAD <root> <path>` | Read value or enumerate keys |
| `REGWRITE <root> <path> <value> <type>` | Write value (REG_SZ, REG_DWORD, etc.) |
| `REGDELETE <root> <path>` | Delete value or key |

### Network (Windows)
| Command | Description |
|---------|-------------|
| `NETMAP <unc> [drive] [user] [pass]` | Map network share |
| `NETUNMAP <drive>` | Disconnect mapped drive |

### Hardware & Diagnostics (Windows)
| Command | Description |
|---------|-------------|
| `DRVSNAPSHOT` | Capture driver configuration state |
| `SYSFIX [check\|apply]` | Check/apply Win98 system fixes (vcache, swap, DMA, autologon) |
| `XPACTIVATE [check\|apply]` | Check/apply Windows XP activation bypass |

### Chat Proxy (Claude Code interface)
| Command | Direction | Purpose |
|---------|-----------|---------|
| `PROMPT_PUSH <text>` | client -> agent | Submit a user prompt |
| `PROMPT_WAIT [timeout_ms]` | daemon -> agent | Long-poll for next prompt |
| `LOG_APPEND <text>` | daemon -> agent | Stream response text to the log buffer |
| `LOG_WAIT <offset> [timeout_ms]` | client -> agent | Long-poll for new response content |
| `STATUS_SET <text>` | daemon -> agent | Set the subagent activity indicator |
| `STATUS_WAIT <seq> [timeout_ms]` | client -> agent | Long-poll for status changes |
| `LOG_CLEAR` | any | Reset prompt, log, and status |
| `PROXY_GET` / `PROXY_SET <host>` | any | Read/write the owning dev box IP |

### Linux-Only
| Command | Description |
|---------|-------------|
| `PKGINSTALL <name>` | Install package (auto-detects apt/yum/pacman) |
| `PKGLIST` | List installed packages |
| `SVCINSTALL` | Manage systemd services |

## Getting Started

### 1. Build the Agent

Requires `i686-w64-mingw32-gcc` (MinGW-w64 cross-compiler):

```bash
cd agent && make clean && make
# Output: retro_agent.exe (~200KB)
```

For Linux targets:
```bash
cd agent-linux && make clean && make
# ARM cross-compile: make arm
```

### 2. Deploy to Retro PCs

Copy the binary and run it:

```cmd
:: On the retro PC
copy \\your-server\share\retro_agent.exe C:\RETRO_AGENT\retro_agent.exe
C:\RETRO_AGENT\retro_agent.exe -s your-secret-here
```

Or use the provided installer batch:
```cmd
:: Edit install_agent.bat first to set your share path and secret
\\your-server\share\install_agent.bat
```

The installer copies the agent + chat client, registers both for autostart, and configures auto-update.

### 3. Connect from Python

```python
import asyncio
from client.retro_protocol import RetroConnection

async def main():
    conn = RetroConnection('10.0.0.50', 9898)  # your agent's IP
    await conn.connect('your-secret-here', timeout=15.0)

    # Get system info
    status, data = await conn.send_command('SYSINFO')
    print(data.decode('ascii'))

    # Run a command
    text = await conn.command_text('EXEC dir C:\\WINDOWS')
    print(text)

    # Take a screenshot
    bmp_data = await conn.command_binary('SCREENSHOT 0')

    await conn.close()

asyncio.run(main())
```

### 4. Set Up the Chat Client (Optional)

On the retro PC, launch the chat client:
```cmd
C:\RETRO_AGENT\retro_chat.exe
```

On your dev box, start the daemon:
```bash
python3 agent/tools/retro_chat_daemon.py &
```

Then run a Claude Code background subagent to process prompts from `/tmp/retro-chat/inbox` and write responses to `/tmp/retro-chat/outbox`. The daemon handles all network IO.

### Agent Flags

| Flag | Description |
|------|-------------|
| `-s <secret>` | Authentication secret (**always set this** — don't use the default) |
| `-p <port>` | TCP port (default: 9898) |
| `-l <logfile>` | Enable file logging |
| `-m` | Force multiplex mode (required for Win98) |
| `-t` | Force threaded mode (NT only) |

## Security

- **Always set a custom secret** with `-s <secret>`. The default is intentionally weak and meant only for initial testing.
- The agent listens on **all interfaces**. In production, restrict access via firewall rules or run on an isolated LAN.
- The agent can execute arbitrary commands, read/write files, and modify the registry. **Only deploy on machines you trust the connecting client to fully control.**
- SMB share credentials in the provisioning scripts are placeholders. Edit them for your environment before deploying.
- The chat proxy (`PROMPT_PUSH`, `LOG_APPEND`, etc.) has no additional authentication beyond the agent secret. Anyone who can connect to the agent can read and write chat messages.

## Auto-Update

The agent checks for newer binaries on a configurable network share ~15 seconds after startup:

1. Compares local binary size vs. share copy
2. If different: downloads new binary to temp location, writes a restart batch, and exits
3. The batch waits for the old process to die, swaps the binary, and relaunches

The chat client (`retro_chat.exe`) is also auto-updated by the agent — it kills the running chat process, copies the new binary, and relaunches.

Configure the update paths in the registry:
```
HKLM\Software\RetroAgent\UpdatePath     = \\your-server\share\retro_agent.exe
HKLM\Software\RetroAgent\ChatUpdatePath = \\your-server\share\retro_chat.exe
```

Or use the installer, which sets these automatically.

## Releasing New Versions

```bash
# Agent
cd agent
make release                  # patch bump
make release BUMP=minor       # minor bump
make release BUMP=major       # major bump

# Chat client
cd agent/tools
make release                  # patch bump
make release BUMP=minor       # minor bump
```

Each release tags the build, compiles, and uploads both a versioned binary (for rollback) and a "latest" pointer (for auto-update).

**Note:** Edit `SMB_CREDS` and `SMB_BASE` in the Makefiles to match your network share.

## Win98 Known Issues

### vcache / MaxFileCache (Critical)

Windows 98 with >512MB RAM requires `MaxFileCache=262144` in `[vcache]` section of `C:\WINDOWS\SYSTEM.INI`. Without this, the disk cache exhausts VxD address space, causing "Windows Protection Error" on boot. The agent's `SYSFIX apply` command fixes this automatically.

### Ghost PCI Entries

Removed hardware leaves registry entries under `HKLM\Enum\PCI` that claim resources and block PnP detection of new cards. Use `PCISCAN` to identify ghosts, then delete via uploaded `.reg` files.

### Win98 RST Crash

Abrupt TCP disconnects (RST packets) can crash Win98's Winsock implementation. Always close connections gracefully. If running the agent behind Docker or a reverse proxy, ensure clean TCP FIN shutdown.

### EXEC and Paths with Spaces

On Win98, `EXEC` wraps commands with `COMMAND.COM /C` which breaks on paths with spaces, even when quoted. Use 8.3 short names (`C:\PROGRA~1` instead of `C:\Program Files`) or the `DIRLIST` command (which handles spaces correctly).

## Game Compatibility & Configuration

Findings and recipes from deploying retro titles across the fleet. Everything in this section can be applied remotely through the agent — no physical access required.

### Reading and writing game configs

```python
# Read an existing config
text = await conn.command_text(
    r'EXEC type "C:\Quake III Arena\baseq3\q3config.cfg"'
)

# Upload a replacement config
with open('q3config_optimized.cfg', 'rb') as f:
    payload = f.read()
await conn.send_command(
    r'UPLOAD C:\Quake III Arena\baseq3\q3config.cfg',
    binary_payload=payload
)
```

### Quake 3 Arena as a reference config

When a Q3-engine game (MoHAA, RTCW, SoF II, etc.) misbehaves, a known-working Q3A install on the same box is the fastest reference. Copy the relevant cvars from `baseq3/q3config.cfg`. The ones that matter for renderer init:

- `r_glDriver "opengl32"`
- `r_colorbits "0"`, `r_depthbits "0"`, `r_stencilbits "0"` — `0` means "match the desktop", which sidesteps a lot of ChoosePixelFormat failures
- `r_fullscreen "1"`
- `r_ext_compressed_textures "0"` — S3TC on Q3 engine is buggy on Voodoo5 and on some NVIDIA NV4x driver combinations

### Quake 3 Arena — swap to Quake3e for protocol-71 servers

**Symptom:** `Server uses protocol version 71 and yours is 66` (or `68`) when trying to join a community server. Stock `quake3.exe` is locked to the protocol it shipped with (66 for 1.30, 68 for 1.32), so it can't reach any ioquake3-era server (protocol 71).

**Fix:** drop Quake3e next to the existing `quake3.exe`. Quake3e is the actively-maintained 32-bit Windows fork that speaks protocols 66/68/71, runs on XP SP3 without a redistributable, and preserves the original pak-file layout (original `quake3.exe` stays put for rollback).

**Prerequisites.** Quake3e refuses to launch with *"Point Release files are missing. Please re-install the 1.32 point release"* unless `baseq3` contains `pak7.pk3` (0x4E4A9 bytes) and `pak8.pk3` (0x6EF4E bytes) from the 1.32 point release. Stock 1.30 installs only ship pak0–pak6.

```python
# One-time staging on the SMB share — pull from upstream:
#   https://github.com/ec-/Quake3e/releases/download/latest/quake3e-windows-mingw-x86.zip
#   https://files.ioquake3.org/quake3-latest-pk3s.zip  (extract baseq3/pak7.pk3, pak8.pk3)

# Per-machine deploy (baseq3 path differs between installs — check each):
await conn.send_command(
    f'UPLOAD {q3_dir}\\quake3e.exe', binary_payload=open('quake3e.exe','rb').read()
)
await conn.send_command(
    f'UPLOAD {baseq3}\\pak7.pk3',    binary_payload=open('pak7.pk3','rb').read()
)
await conn.send_command(
    f'UPLOAD {baseq3}\\pak8.pk3',    binary_payload=open('pak8.pk3','rb').read()
)
# Launch via batch so cwd is the game dir (Q3E looks for baseq3 relative to cwd):
#   cd /d "<q3_dir>" && quake3e.exe
```

**Version interop.** Quake3e is a drop-in replacement for client-side play and can connect to any mix of stock-1.30, stock-1.32, and ioquake3/protocol-71 servers. Your existing `q3config.cfg` carries over untouched. Keep the old `quake3.exe` in place — users can still launch it for single-player or LAN games against an unpatched client.

**Do not use the official `ioquake3` Windows zip** from `ioquake3.org/get-it/` on retro XP machines — it is x86_64 only and will refuse to launch on 32-bit Windows. The `ec-/Quake3e` fork publishes `quake3e-windows-mingw-x86.zip` which is the correct 32-bit build for this fleet.

### MoHAA on GeForce 6 series — requires ForceWare ≤ 71.89

**Symptom:** `GLW_ChoosePFD failed / failed to find an appropriate PIXELFORMAT / could not load OpenGL subsystem` when launching Medal of Honor: Allied Assault. Quake 3 Arena on the *same card and driver* works — the failure is MoHAA-specific.

**Cause:** MoHAA's Q3-engine retry path asks for a 16-bit color PFD. ForceWare 75.19+ on NV4x does not expose any 16-bit PFDs when the desktop is in 32-bit color. The engine walks 35 enumerated PFDs, matches none, falls back to the 3dfx Glide wrapper (`3dfxvgl.dll`) which isn't present, and aborts.

**Remediation:** Install NVIDIA ForceWare **71.89** (last driver that still exposes 16-bit PFDs on NV4x while still supporting GeForce 6 series). Confirmed on a GeForce 6800. GeForce 6800 support starts at ForceWare 61.72 — don't go below that.

```python
# Stage driver from SMB share and install silently (no auto-reboot)
await conn.send_command('NETMAP \\\\server\\share X: user pass')
await conn.send_command(
    'EXEC cmd /c copy /Y "X:\\Drivers\\Nvidia\\Win2K-XP\\71.89_forceware_winxp2k.exe" '
    '"C:\\WINDOWS\\TEMP\\nv71.exe"'
)
await conn.send_command('NETUNMAP X:')
await conn.send_command('LAUNCH C:\\WINDOWS\\TEMP\\nv71.exe -s -y -noreboot')
# Wait for install to complete (poll for setup.exe absence), then REBOOT via the agent.
```

**Related quirk — `sm.*` safe-mode markers:** MoHAA creates zero-byte files named `sm.000`, `sm.001`, ... in the install dir on each launch, and deletes them on clean exit. Any stale marker triggers a "last time crashed" dialog and a progressively stricter safe-mode config that overrides your cvars *and* `+set` CLI args. After the driver fix, delete any leftover `sm.*` once; from then on MoHAA keeps its own accounting. The launcher dialog's button order is Safe Mode / Restart Windows / Normal Mode / Exit — the default focus is Safe Mode so TAB, TAB, SPACE selects Normal Mode.

### Descent 3 retail — skip the CD check

Retail `main.exe` refuses to launch without the CD (physical or DaemonTools-mounted). SafeDisc is **not** involved — it's a plain conditional jump around a `MessageBox("Invalid CDROM!")`. Two versions seen in the fleet:

| Version     | `main.exe` size | Patch offset | Pre-patch MD5                      | Post-patch MD5                     |
|-------------|----------------:|:------------:|------------------------------------|------------------------------------|
| v1.4 retail |       1,728,512 | `0x52af5`    | `6be4be25542d7d59bcc7a3d58d5de971` | `3ab25545a3511c9c7661eada1ec4e1e6` |
| v1.5 retail |       1,724,416 | `0x52b05`    | `c38ddd1e2cd3709c681dba494cf43124` | `8fd5b2cf4a16ae9c08e25b31f494f857` |

Flip the byte at the patch offset from `0x75` (JNZ) to `0xeb` (unconditional JMP). This converts the "CD not found → error" branch into an unconditional "skip error" in the engine's `CheckCD("d3.hog")` loop.

The launcher (`Descent 3.exe`, 1,150,976 bytes on both v1.4 and v1.5) performs its own CD volume-label probe and must be patched separately: at file offset `0x7c50` replace the function prologue `81 ec 08 02 00 00` with `33 c0 c2 04 00 90` (`xor eax, eax; ret 4; nop`). That stubs the drive-scan function to return `0` ("CD found at drive A:"), which the caller treats as success via its `jge` check.

General technique for any pre-SafeDisc 90s CD check: grep for the error string (`strings game.exe | grep -iE "cdrom|insert.*cd|disc"`), locate its VA via the PE section table, search the code for `0x68 <VA-LE>` (x86 `PUSH imm32` — the argument to `MessageBoxA`), walk backward to the guarding `74`/`75` short conditional, flip to `0xeb`. Keep a backup (`FILECOPY src|src.orig`) before writing.

### Older games and paths with spaces

`LAUNCH` breaks on paths containing spaces on Win98. Use 8.3 short names (`C:\PROGRA~1\EAGAME~1\MOHAA` for `C:\Program Files\EA GAMES\MOHAA`) or launch a batch file via `LAUNCH` and put the quoted path inside the batch. On XP this is fine.

### UT2004 Linux dedicated — OldUnreal 3374-preview-17 masterserver crash

**Symptom:** `UCC server DM-Rankin?game=XGame.xDeathMatch -nohomedir` on the Linux server binary from `OldUnreal-UT2004Patch3374-Linux.tar.bz2` segfaults during startup in `AMasterServerLink::eventGetMasterServer` → `AMasterServerUplink::execReconnect`. Happens whether or not `[IpDrv.MasterServerLink]` uses the `Group=` field; the crash is in the OldUnreal code itself, not the config.

```
[ 7]  .../IpDrv.so(_ZN17AMasterServerLink20eventGetMasterServerER7FStringRi+0x94)
[ 8]  .../IpDrv.so(_ZN19AMasterServerUplink13execReconnectER6FFramePvm+0xf1)
```

**Cause:** The 3374 **preview** release is flagged by OldUnreal themselves as "may not work in online play." The masterserver advertise code is the specific preview-only regression we're hitting.

**Workaround (use for dedicated servers on 3374-preview):**

```ini
; UT2004.ini
[Engine.GameEngine]
;ServerActors=IpDrv.MasterServerUplink   ; disabled — eventGetMasterServer segfaults on 3374-P17

[IpDrv.MasterServerUplink]
DoUplink=False                            ; belt and suspenders
```

Server starts cleanly, listens on game port 7777, accepts direct-IP connections. The query port (7778) and master advertisement are gone. If your fleet connects via direct-IP favorites (the NSC retro fleet uses this pattern via `agent/tools/ut2004_favorites.py`) this is fine. If you need the server to be discoverable through the in-game browser, stay on Epic 3369.3 instead.

**Alternative** — downgrade to Epic's stock 3369.3 dedicated tarball and use `ucc-bin-linux-amd64` from that. The OldUnreal .u files will not work with the 3369 binary (version mismatch), so a clean re-install of the Epic server is required to rollback.

### Yamagi Quake 2 — build 8.60 from source on modern Ubuntu

**Why build from source:** Ubuntu 24.04 APT ships `yamagi-quake2 8.30+dfsg-1` (Feb 2024). Yamagi released 8.60 (Sep 2025) with material fixes; there is no PPA yet.

**Build + install to a private prefix** (no sudo needed if you install to `$HOME/local/yamagi-8.60`):

```bash
sudo apt install build-essential libsdl2-dev libopenal-dev \
    libcurl4-openssl-dev zlib1g-dev libvorbis-dev libogg-dev

curl -fSL -o /tmp/q2-8.60.tar.xz https://deponie.yamagi.org/quake2/quake2-8.60.tar.xz
cd /tmp && tar xf q2-8.60.tar.xz && cd quake2-8.60
make -j$(nproc)

mkdir -p ~/local/yamagi-8.60/baseq2
install -m 755 release/q2ded        ~/local/yamagi-8.60/q2ded
install -m 755 release/baseq2/game.so ~/local/yamagi-8.60/baseq2/game.so
```

`q2ded` is the dedicated-only binary (no SDL2/video linkage); use it in the systemd unit instead of the full `quake2` client.

**Line-buffered logs via systemd** — q2ded (and the original stock binary) fully-buffer stdout when no TTY is attached, so `StandardOutput=append:...log` captures nothing until buffer flush. Wrap with `stdbuf -oL`:

```ini
[Service]
ExecStart=/usr/bin/stdbuf -oL -eL %h/local/yamagi-8.60/q2ded \
    -datadir %h/q2-server +set dedicated 1 +exec server.cfg
StandardOutput=append:%h/q2-server/server.log
StandardError=append:%h/q2-server/server.log
```

Line-buffering gives the dashboard log watcher real-time events instead of 30-minute buffer delays. Same technique applies to any other cvar-console engine (Q3 engines, UT2004's UCC) that writes to stdout.

**game.so placement.** Yamagi searches for `baseq2/game.so` in (1) datadir, (2) binary-adjacent dir, (3) `/usr/lib/yamagi-quake2/baseq2/`. Drop the new `game.so` into both the install prefix AND `~/q2-server/baseq2/` to avoid accidentally loading the 8.30 APT version if the install prefix gets wiped.

## LLM Integration Patterns

### Diagnostic Workflow

An LLM receives a problem description and autonomously investigates:

1. `SYSINFO` + `AUDIOINFO` / `VIDEODIAG` / `PCISCAN` to understand hardware
2. `REGREAD` to check driver configuration
3. `SCREENSHOT` to see the current display state
4. `REGWRITE` / `UPLOAD` / `EXEC` to apply fixes
5. Verify the fix with another round of diagnostics

### GUI Automation Workflow

For software that requires a graphical installer:

1. `UPLOAD` or `EXEC copy` to get the installer onto the machine
2. `LAUNCH` the installer (never `EXEC` for GUI apps)
3. Loop: `SCREENSHOT` -> vision model analyzes -> `UICLICK`/`UIKEY` to interact
4. Post-install cleanup via `.reg` file uploads
5. `REBOOT` and verify

### Fleet Management Workflow

For managing multiple retro PCs:

1. `discover_retro_pcs()` to find all agents on the LAN
2. Connect to each, run `SYSINFO` / `PCISCAN` / `VIDEODIAG` for inventory
3. Push updates via `UPLOAD` + auto-update mechanism
4. Monitor health via periodic `PING` checks

### Key Constraints

- **EXEC vs LAUNCH**: EXEC blocks and captures output. LAUNCH returns immediately. Wrong choice hangs the agent.
- **FILECOPY delimiter**: Uses pipe `|` between source and destination, not space.
- **REGREAD format**: `REGREAD HKLM Path\\To\\Key` (root abbreviation + path, space-separated).
- **Screenshot format**: Raw 24-bit BMP. Convert to PNG with Pillow before passing to a vision model.
- **Timeouts**: Default 60s for commands, 120s for binary responses.
- **REBOOT/SHUTDOWN are destructive**: Retro machines may require physical access to recover.

## Project Structure

```
retro-agent/
+-- agent/                  # Windows agent (C, MinGW cross-compiled)
|   +-- Makefile
|   +-- src/                # C source files
|   +-- tools/              # retro_chat client (C) + daemon (Python)
|   +-- lib/
|       +-- libmsvcrt.a     # Patched import lib (Win98 compatible)
+-- agent-linux/            # Linux agent (C, native)
|   +-- Makefile
|   +-- src/
+-- client/                 # Python async client library
|   +-- retro_protocol.py   # TCP protocol client (RetroConnection)
|   +-- retro_discovery.py  # UDP LAN discovery
+-- provisioning/           # Installation scripts and registry templates
|   +-- win98/
+-- docs/
    +-- images/             # Screenshots for this README
    +-- case-studies/       # Real-world diagnostic walkthroughs
```

## Linux Game Servers for the Fleet

The retro fleet spends most of its time waking up and playing 2000s-era
multiplayer games. `scripts/game-servers/` contains idempotent installers
that turn any modern Linux box into a public dedicated server for four of
them, so the Win98/XP machines have something to connect to without hunting
for a live internet server.

| Game | Install script | UDP ports | Masters listed on |
|---|---|---|---|
| Unreal Tournament 2004 | [`install-ut2004-server.sh`](scripts/game-servers/install-ut2004-server.sh) | 7777 / 7778 / 7787 | 333networks, errorist.eu, OpenSpy |
| Unreal Tournament 99 | [`install-ut99-server.sh`](scripts/game-servers/install-ut99-server.sh) | 7797 / 7798 | 333networks, OldUnreal ×2, errorist.eu, OpenSpy, qtracker, hypercoop, telefragged |
| Quake 2 (Yamagi) | [`install-quake2-server.sh`](scripts/game-servers/install-quake2-server.sh) | 27910 | master.yamagi.org, master.quakeservers.net |
| OpenArena (Q3-compatible) | [`install-openarena-server.sh`](scripts/game-servers/install-openarena-server.sh) | 27960 | dpmaster.deathmask.net, master.ioquake3.org |

```bash
cd scripts/game-servers
./install-all.sh    # ~10 min, a few sudo prompts, 1.6 GB of downloads
```

Each script writes a systemd **user** unit (no root service), opens UFW if
active, and enables `loginctl linger` so the servers come back on their own
after a reboot. See [`scripts/game-servers/README.md`](scripts/game-servers/README.md)
for full details, environment variable overrides, router port-forward notes
(including the AT&T BGW MAC-collision gotcha), and external reachability tests.

The UT99 installer uses OldUnreal **v469e** (Nov 2025, actively maintained,
cross-compatible with legacy Epic 436/451 clients). To upgrade the retro
fleet's XP clients to the matching 469e client:

```bash
python3 scripts/game-servers/push-ut99-xp-patch.py 192.168.1.143 192.168.1.133 ...
```

### Patch level (as of 2026-04-19)

| Server | Current version | Notes |
|---|---|---|
| OpenArena (`openarena-server.service`) | OA 0.8.8 · ioq3 1.36+u20240217 (APT) | Current. Both upstreams dormant; Ubuntu package is latest available. |
| Yamagi Q2 (`quake2-server.service`) | **8.60** built from source · `~/local/yamagi-8.60/q2ded` | Upgraded from APT's 8.30. Unit uses `stdbuf -oL` for real-time log capture. See "Yamagi Quake 2 — build 8.60 from source" above. |
| UT99 (`ut99-server.service`) | **OldUnreal 469e** (Linux amd64) | Stable, Nov 2025. Cross-compatible with Epic 436/451 clients AND with the 469e WindowsXP client patch. Ships with 8 community masters pre-configured — no master-mirror mod needed. |
| UT2004 (`ut2004-server.service`) | **Epic 3369.3** (Linux amd64 static binary from archive.org) | Reverted from the earlier OldUnreal 3374-preview-17 experiment because the preview build segfaults in `eventGetMasterServer` with community master lists, preventing public listing. 3369.3 + MasterServerMirror mod uplinks to 3 community masters cleanly; 3374-preview binaries/`.so` files are staged in `System/_overlay-backup/` if you want to re-enable direct-IP play. |

Upgrade a single service via its install script, or manually per the per-game recipe in **Game Compatibility & Configuration** above. After upgrading, `systemctl --user daemon-reload && systemctl --user restart <svc>` and verify:

```bash
for svc in openarena-server quake2-server ut2004-server; do
  systemctl --user is-active "$svc"
done
# Quick version probes:
python3 -c "import socket; s=socket.socket(2,2); s.settimeout(3); \
  s.sendto(b'\\xff\\xff\\xff\\xffstatus\\n',('127.0.0.1',27910)); \
  print(s.recvfrom(4096)[0][:400])"   # Yamagi: look for version\8.60
```

## Contributing

The agent is designed to be extended with new commands. Each command is a C function that receives a socket and arguments, dispatched via the command table in `handlers.c`. Add your handler, register it in the table, and rebuild.

The chat proxy protocol is intentionally simple (JSON files in directories) so you can plug in any LLM backend — not just Claude. Write a script that reads from `inbox/`, calls your model, and writes to `outbox/`. The daemon handles the network transport.

## License

MIT
