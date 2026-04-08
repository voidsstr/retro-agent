# Retro Agent

Remote management agent for retro PCs (Windows 98 SE, Windows 2000, Windows XP) and Linux machines. A lightweight C binary runs on target machines, exposing system information, file operations, process control, UI automation, and hardware diagnostics over a simple TCP protocol.

## Architecture

```
                    TCP :9898               UDP :9899
  ┌──────────┐    ◄───────────►    ┌─────────────────┐
  │  Client   │    commands/       │   retro_agent    │
  │ (Python)  │    responses       │  (C, ~110KB)     │──► broadcasts
  └──────────┘                     └─────────────────┘    discovery
                                          │
                                   Win98/XP/2K/Linux
```

**Agent** (`agent/`): Cross-compiled C binary for Windows (MinGW-w64, i586 target). Runs as console app or NT service. ~110KB, no runtime dependencies beyond system DLLs.

**Linux Agent** (`agent-linux/`): Native C binary for x86_64 and ARM. Subset of Windows agent commands adapted for POSIX.

**Python Client** (`client/`): Async TCP client library for connecting to agents. Handles framing, authentication, command dispatch, and LAN discovery.

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

Authentication: first frame must be `AUTH <secret>`. Agent responds with `OK <hostname> <os_version> [os_family]`.

Discovery: agents broadcast `RETRO|hostname|ip|port|os|cpu|ram_mb|os_family` on UDP 9899 every 30 seconds. Clients can send `DISCOVER` to trigger immediate responses.

## Commands

### System Info
| Command | Description | Response |
|---------|-------------|----------|
| `PING` | Health check | `PONG` |
| `SYSINFO` | CPU, memory, OS, drives, uptime | JSON |
| `VIDEODIAG` | Video card, driver, PCI IDs, resolution, DirectX | JSON |
| `AUDIOINFO` | Audio device enumeration | JSON |
| `SMARTINFO` | S.M.A.R.T. disk health | JSON |
| `DISPLAYCFG` | Display configuration and refresh rate | JSON |
| `PCISCAN` | PCI device enumeration with vendor/device IDs | JSON |

### Execution
| Command | Description | Response |
|---------|-------------|----------|
| `EXEC <cmd>` | Run hidden, capture stdout/stderr, block until exit (60s) | text |
| `LAUNCH <cmd>` | Run visible, return immediately | JSON `{pid, command}` |

**EXEC** is for CLI commands. **LAUNCH** is for GUI apps. Never use EXEC for GUI apps (runs hidden, blocks agent).

### Process Control
| Command | Description |
|---------|-------------|
| `PROCLIST` | JSON list of running processes |
| `PROCKILL <pid>` | Terminate process by PID |
| `QUIT` | Stop agent (for updates) |
| `SHUTDOWN` | Power off machine |
| `REBOOT` | Restart machine |

### File Operations
| Command | Description |
|---------|-------------|
| `DIRLIST <path>` | JSON directory listing |
| `UPLOAD <path>` | Upload file (two-frame: command + binary payload) |
| `DOWNLOAD <path>` | Download file (binary response) |
| `MKDIR <path>` | Create directory (recursive) |
| `DELETE <path>` | Delete file |
| `FILECOPY <src>\|<dst>` | Copy file (pipe-delimited) |

### UI Automation (Windows)
| Command | Description |
|---------|-------------|
| `SCREENSHOT <quality>` | Capture screen as raw 24-bit BMP. 0=full, 1=half, 2=quarter |
| `UICLICK <x> <y> [button]` | Click at coordinates (left/right/middle) |
| `UIKEY <keyname>` | Send keystroke |
| `WINLIST` | JSON list of visible windows |

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

### Linux-Only
| Command | Description |
|---------|-------------|
| `PKGINSTALL <name>` | Install package (auto-detects apt/yum/pacman) |
| `PKGLIST` | List installed packages |
| `SVCINSTALL` | Manage systemd services |

## Building

### Windows Agent (cross-compile from Linux)

Requires `i686-w64-mingw32-gcc` (MinGW-w64).

```bash
cd agent && make clean && make
# Output: retro_agent.exe (~110KB)
```

Version is auto-detected from git tags (`v1.2.0` -> `1.2.0`).

```bash
# Release: bump version, tag, build, upload to SMB share
make release              # patch bump (1.0.0 -> 1.0.1)
make release BUMP=minor   # minor bump (1.0.1 -> 1.1.0)
make release BUMP=major   # major bump (1.1.0 -> 2.0.0)
```

### Linux Agent

```bash
cd agent-linux && make clean && make
# Output: retro_agent_linux

# ARM cross-compile (Raspberry Pi)
make arm
```

X11 support is auto-detected via pkg-config for screenshot capability.

## Installation

### Windows 98/XP

The `provisioning/win98/install_agent.bat` script handles first-time setup:

1. Copies binary from SMB share to `C:\RETRO_AGENT\`
2. Imports registry keys for auto-start on boot
3. Starts the agent

Manual installation:
```
copy \\server\share\retro_agent.exe C:\RETRO_AGENT\retro_agent.exe
retro_agent.exe -s <secret>
```

### Linux

```bash
./retro_agent_linux -s <secret>
```

### Agent Flags

| Flag | Description |
|------|-------------|
| `-s <secret>` | Set authentication secret (default: `retro-agent-secret`) |
| `-p <port>` | Set TCP port (default: 9898) |
| `-l <logfile>` | Enable file logging |
| `-m` | Force multiplex mode (required for Win98) |
| `-t` | Force threaded mode (NT only) |
| `-1` | Single client mode (legacy) |

## Using the Agent from an LLM

The retro agent is designed to be operated by LLMs (Claude, GPT, etc.) through the Python client library. An LLM can connect to agents, run diagnostic workflows, install software, configure hardware, and automate GUI interactions — all through natural language instructions translated to agent commands.

### Python Client Library

```python
import asyncio
from client.retro_protocol import RetroConnection

async def main():
    conn = RetroConnection('192.168.1.124', 9898)
    await conn.connect('your-secret', timeout=15.0)

    # System diagnostics
    status, data = await conn.send_command('SYSINFO')
    print(data.decode('ascii'))

    # Execute commands
    text = await conn.command_text('EXEC dir C:\\WINDOWS')

    # Download files (binary response)
    file_data = await conn.command_binary('DOWNLOAD C:\\path\\to\\file')

    # Upload files
    with open('local_file', 'rb') as f:
        payload = f.read()
    await conn.send_command('UPLOAD C:\\remote\\path', binary_payload=payload)

    # Screenshots (raw BMP, convert to PNG for viewing)
    bmp_data = await conn.command_binary('SCREENSHOT 0')

    await conn.close()

asyncio.run(main())
```

### LAN Discovery

```python
from client.retro_discovery import discover_retro_pcs

pcs = await discover_retro_pcs(timeout=3.0)
for pc in pcs:
    print(f"{pc.hostname} at {pc.ip} - {pc.os} ({pc.os_family})")
```

### LLM Workflow Patterns

**Diagnostic workflow**: An LLM receives a problem description ("no sound from the Sound Blaster"), then autonomously:
1. Connects to the agent via `RetroConnection`
2. Runs `SYSINFO` and `AUDIOINFO` to understand the hardware
3. Checks registry for driver configuration via `REGREAD`
4. Takes `SCREENSHOT` to see the current display state
5. Applies fixes via `REGWRITE`, file uploads, or `EXEC` commands
6. Verifies the fix worked

**GUI automation workflow**: For installing drivers or software that requires a GUI:
1. Upload installer via `UPLOAD` or copy from network share via `EXEC copy`
2. `LAUNCH` the installer (never EXEC for GUI apps)
3. Loop: `SCREENSHOT` -> analyze the screen -> `UICLICK`/`UIKEY` to interact
4. Post-install registry cleanup via uploaded `.reg` files
5. `REBOOT` and verify via `VIDEODIAG`/`AUDIOINFO`

**Hardware management workflow**: For managing a fleet of retro PCs:
1. `discover_retro_pcs()` to find all agents on the LAN
2. Connect to each, run `SYSINFO`, `PCISCAN`, `VIDEODIAG` for inventory
3. Apply updates via `UPLOAD` + `EXEC` or push agent updates via the auto-update mechanism
4. Monitor health via periodic `PING` checks

### Key Constraints for LLM Usage

- **Single-threaded protocol**: Only one connection at a time per agent. Close connections when done.
- **EXEC vs LAUNCH**: EXEC blocks and captures output (for CLI). LAUNCH returns immediately (for GUI). Using the wrong one hangs the agent.
- **Win98 shell escaping**: `<>` characters in `echo` commands are interpreted as redirects. Use `UPLOAD` with `binary_payload` to write files with special characters.
- **FILECOPY delimiter**: Uses pipe `|` between source and destination, not space.
- **REGREAD format**: `REGREAD HKLM Path\\To\\Key` (root abbreviation and path space-separated).
- **Screenshot format**: Raw 24-bit BMP. Convert to PNG with Pillow before passing to a vision model.
- **Timeouts**: Default 60s for commands, 120s for binary responses. Adjust for slow operations (file copies over network, large screenshots).
- **REBOOT/SHUTDOWN are destructive**: These machines may require physical access to recover. Always confirm before issuing.

### Example: Diagnosing Audio Issues

```python
import asyncio, json
from client.retro_protocol import RetroConnection

async def diagnose_audio(host: str):
    conn = RetroConnection(host, 9898)
    await conn.connect('retro-agent-secret', timeout=15.0)

    # Check what audio hardware is present
    status, data = await conn.send_command('AUDIOINFO')
    audio = json.loads(data.decode('ascii'))
    print(f"Wave devices: {audio['wave_out_count']}")
    for dev in audio['wave_out_devices']:
        print(f"  {dev['name']} - {dev['driver_version']}")

    # Check PCI devices for sound cards
    status, data = await conn.send_command('PCISCAN')
    pci = json.loads(data.decode('ascii'))
    # Look for audio devices (class code 0401xx)

    # Check driver registry entries
    status, data = await conn.send_command(
        'REGREAD HKLM System\\CurrentControlSet\\control\\MediaResources\\wave'
    )
    wave = json.loads(data.decode('ascii'))
    print(f"Registered wave drivers: {wave.get('subkeys', [])}")

    # Run SYSFIX to apply standard Win98 fixes
    text = await conn.command_text('SYSFIX apply')
    print(f"SYSFIX: {text}")

    await conn.close()

asyncio.run(diagnose_audio('192.168.1.124'))
```

## Win98 Known Issues

### vcache / MaxFileCache (Critical)

Windows 98 with >512MB RAM requires `MaxFileCache=262144` in `[vcache]` section of `C:\WINDOWS\SYSTEM.INI`. Without this, the disk cache exhausts VxD address space, causing "Windows Protection Error" on boot. Run `SYSFIX apply` to fix automatically.

### Ghost PCI Entries

Removed hardware leaves registry entries under `HKLM\Enum\PCI` that claim resources and block PnP detection of new hardware. Use `PCISCAN` to identify ghosts, then delete via `.reg` file upload.

### Win98 RST Crash

Abrupt TCP disconnects (RST packets) can crash Win98's Winsock implementation, taking down the entire machine. Avoid forcefully closing connections to Win98 agents.

## Retro Chat — Claude Code on retro PCs

Each retro machine can run `retro_chat.exe`, a small console UI that turns the machine into a Claude Code-style chat terminal. The user types prompts on the retro PC; a Claude Code subagent on the dev box (running on the user's normal Max subscription) processes them with full tools and streams the response back. Operations the subagent performs on the retro PC are surfaced live as `[subagent: ...]` activity above the input line, so the user always knows what's happening.

```
┌──────────────────┐  TCP localhost  ┌──────────────────┐  TCP LAN  ┌──────────────────────┐  files  ┌─────────────────┐
│  retro_chat.exe  │ ◄─────────────► │  retro_agent.exe │ ◄───────► │  retro_chat_daemon   │ ◄─────► │ Claude Code     │
│  (user types)    │                 │  (chatproxy +    │           │  (network multiplexer│         │ subagent on Max │
│                  │                 │   status bus)    │           │   on dev box)        │         │ subscription    │
└──────────────────┘                 └──────────────────┘           └──────────────────────┘         └─────────────────┘
       ▲                                       ▲                              │                              │
       │ STATUS_WAIT (long-poll)               │ STATUS_SET (push)            │ writes status_outbox/*.json  │
       │ LOG_WAIT    (long-poll)               │ LOG_APPEND  (push)           │ writes outbox/*.json         │
       └───────────────────────────────────────┴──────────────────────────────┴──────────────────────────────┘
```

### Components

- **`agent/tools/retro_chat.c`** — the console client. Connects to `127.0.0.1:9898` (the local agent), runs three threads: input loop, `LOG_WAIT` long-poller for response chunks, and `STATUS_WAIT` long-poller for subagent activity. Sub-100ms latency, zero polling traffic.
- **`agent/src/chatproxy.c`** — the agent-side message bus. Stores a single in-flight prompt slot, a 256KB log ring buffer, and a 512-byte status slot, all signaled via Windows events for instant wake-up of waiters.
- **`provisioning/win98/install_agent.bat`** — installs both `retro_agent.exe` and `retro_chat.exe`, registers each for autostart on boot, and configures the auto-update paths.
- **Auto-update** (`agent/src/autoupdate.c`) — runs ~15 seconds after the agent starts. Checks the SMB share for newer binaries and updates both `retro_agent.exe` (via batch-restart) and `retro_chat.exe` (in-place: kill, copy, relaunch) without user intervention.

### Chat Proxy Commands

| Command | Direction | Purpose |
|---------|-----------|---------|
| `PROMPT_PUSH <text>` | client → agent | User submits a prompt (called by `retro_chat.exe`). |
| `PROMPT_POP` | subagent → agent | Subagent pulls next pending prompt (returns empty if none). |
| `PROMPT_WAIT [timeout_ms]` | subagent → agent | Long-polling variant — blocks up to `timeout_ms` for a new prompt. |
| `LOG_APPEND <text>` | subagent → agent | Append a chunk of response text to the log ring buffer. |
| `LOG_READ [offset]` | client → agent | Read log content starting at byte offset. Returns `<total_size>\n<bytes>`. |
| `LOG_WAIT <offset> [timeout_ms]` | client → agent | Long-polling variant of `LOG_READ` — blocks until new content past `offset`. |
| `LOG_CLEAR` | either | Reset prompt slot, log buffer, and status. |
| `STATUS_SET <text>` | subagent → agent | Set the current subagent activity (e.g. `EXEC dir C:\WINDOWS`). Increments a sequence counter and signals all `STATUS_WAIT` waiters. |
| `STATUS_GET` | client → agent | Read current status. Returns `<seq>\n<status_text>`. |
| `STATUS_WAIT <last_seq> [timeout_ms]` | client → agent | Long-polling variant — blocks until `seq` advances. |
| `PROXY_GET` / `PROXY_SET <host>` | either | Read/write the dev box that owns this agent (persisted in `HKLM\Software\RetroAgent\ProxyHost`). |

All long-polling commands cap at 60s server-side, use Windows events for sub-100ms wake-up latency, and consume zero CPU/network when idle (the kernel parks the recv()).

### Subagent Status Channel

The status channel is what the user actually sees as "[subagent: doing X]" above the input line. It's separate from the response log so it can update independently of streaming output — ideal for surfacing what's happening during slow operations (file copies, screenshots, registry walks).

The Claude Code subagent writes one tiny JSON file per tool invocation to `/tmp/retro-chat/status_outbox/<host>-<counter>.json`:

```json
{"host": "192.168.1.124", "text": "EXEC dir C:\\WINDOWS"}
```

The daemon (`retro_chat_daemon.py`) picks it up within ~20ms and forwards it via `STATUS_SET` on the agent's send connection. The retro_chat client's `STATUS_WAIT` long-poll wakes immediately and redraws the input area. End-to-end latency from "subagent decides to run a tool" to "user sees the activity on the retro PC" is well under 100ms on a healthy LAN.

When the subagent finishes its response, it writes one final status with empty `text` to clear the line.

### Installation

The installer batch on the SMB share installs everything in one shot:

```cmd
\\192.168.1.122\files\Utility\Retro Automation\install_agent.bat
```

It will:
1. Copy `retro_agent.exe` and `retro_chat.exe` to `C:\RETRO_AGENT\`
2. Register both for autostart on boot (`HKLM\...\Run\RetroAgent` and `RetroChat`)
3. Apply OS-specific autologon registry (so the agent gets a session immediately on boot)
4. Set the auto-update paths in `HKLM\Software\RetroAgent\{UpdatePath, ChatUpdatePath}`
5. Start both processes

After the initial install, both binaries auto-update from the share on every reboot — push a new release to the share with `make release` and every retro PC picks it up the next time it boots.

### Releasing New Versions

```bash
# Agent (retro_agent.exe)
cd retro-agent/agent
make release                  # 1.4.0 -> 1.4.1
make release BUMP=minor       # 1.4.1 -> 1.5.0

# Chat client (retro_chat.exe)
cd retro-agent/agent/tools
make release                  # 0.10.2 -> 0.10.3
make release BUMP=minor       # 0.10.3 -> 0.11.0
```

Each `make release` tags the build, compiles, and uploads to:
- The versioned filename (e.g. `retro_agent/retro_agent_v1.5.0.exe`) — preserved forever for rollback
- The latest pointer at the share root (e.g. `retro_agent.exe`) — what installers and the auto-update use

## Project Structure

```
retro-agent/
├── agent/                  # Windows agent (C, MinGW cross-compiled)
│   ├── Makefile
│   ├── src/                # 27 source files
│   └── lib/
│       └── libmsvcrt.a     # Patched import lib (Win98 compatible)
├── agent-linux/            # Linux agent (C, native)
│   ├── Makefile
│   └── src/                # 17 source files
├── client/                 # Python async client library
│   ├── retro_protocol.py   # TCP protocol client (RetroConnection)
│   └── retro_discovery.py  # UDP LAN discovery
├── provisioning/           # Installation scripts
│   └── win98/              # Batch files, registry entries, INF files
└── docs/
    └── case-studies/       # Real-world diagnostic walkthroughs
```
