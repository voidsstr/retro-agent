# Retro Agent — AI-Powered Remote Management for Retro PCs

**Give your Pentium II a smarter assistant than most developers had in 2003.**

> ### 🚀 retro3dfx: an open-source 3dfx Voodoo driver stack, optimized past what 3dfx shipped
>
> Using this agent as the remote harness, we built and tuned a **Voodoo 3/4/5 driver stack** — Glide 2/3 and a Mesa-based OpenGL ICD — **from genuinely open source code** (3dfx's 2000 Glide open release and the MIT-licensed Mesa), and iterated on it with a fully tracked benchmark→optimize→measure loop until it **beat the community-standard AmigaMerlin driver on real hardware** (and the era 3dfx official ICD at 1024x768). Games verified on real silicon: Quake III, Quake II, Counter-Strike 1.6, Half-Life, RtCW, UT99 and more — see the [compatibility table](#game-compatibility-on-the-voodoo-3-verified-on-hardware). Our clean-room **kernel display driver (`vcr-disp`) is still in progress**; that layer is honestly accounted for in the [Driver lane policy](#driver-lane-policy--what-this-repo-owns). Start at [voodoo-cleanroom — An Open-Source Driver Stack for 3dfx Voodoo Cards](#retro3dfx--an-open-source-driver-stack-for-3dfx-voodoo-cards) and [The Driver Optimization Process](#the-driver-optimization-process).

> ### 🖥️ New: the Retro Chat **brain** — a full Claude agent, on a 25‑year‑old OS
>
> A standalone Claude agent (built on the [Claude Agent SDK](https://code.claude.com/docs/en/agent-sdk/overview) — the same engine behind Claude Code) runs as an **auto‑starting service** on your modern box. Type a prompt on the retro PC's console and it answers with the **full Claude toolset** (read/edit files, run commands, search the web) and can **operate the rest of the fleet** — no Claude Code window open anywhere.

![A Windows XP machine asks the on-prem Claude agent to re-theme its desktop, live](docs/images/retro-chat-brain-hero.png)

*Real usage on a Windows XP box (`192.168.1.133`): the user types **"change my desktop background themed for geforce 4"** and the agent goes to work — reading files and running tools on the machine — with live status (`[subagent: running: Read]`) and a spinner streaming back to the 16‑color console.*

![The Retro Chat client running on the Windows XP desktop](docs/images/retro-chat-brain-desktop.png)

*`retro_chat.exe` running natively on the XP desktop, driving a real task on the very machine it runs on.*

---

A ~200KB C binary that runs on Windows 98 SE, 2000, and XP (plus Linux). It exposes system info, file IO, registry, process control, UI automation, and hardware diagnostics over a simple TCP protocol — designed from the ground up to be operated by LLMs.

Connect from Python, pipe it to Claude or GPT, and suddenly your vintage hardware has a state-of-the-art AI that can diagnose why your Sound Blaster isn't working, install NVIDIA drivers via GUI automation, or configure your Voodoo 5500 for optimal Glide performance.

The built-in **Retro Chat** client takes it further: type a prompt directly on the retro PC's console, and the **Retro Chat brain** — a standalone Claude agent on your modern dev box — processes it with the *full Claude toolbox*, then streams the response back to your 16-color Win98 terminal. The brain runs as an auto‑starting service and needs no Claude Code session open; see [`scripts/README-chat-brain.md`](scripts/README-chat-brain.md).

Three things are built on top of the agent, each documented below:

| | What it is |
|---|---|
| [**Retro Chat**](#retro-chat--ai-on-a-25-year-old-os) | A Claude agent answering on the retro PC's own console, with live status and streamed output |
| [**Retro DOS**](#retro-dos--a-game-manager-and-the-agentchat-running-on-real-dos) | A 16-bit game manager (browse ~3,000 titles, install over the LAN, play) and the agent+chat as a single real-mode exe |
| [**Fleet AI**](#fleet-ai--training-and-inference-on-vintage-hardware-contributors-welcome) | A dependency-free ML engine: int8 CNN inference, on-device training, distributed data-parallel runs across period hardware |

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
                                                   │  retro_chat_brain.py           │
                                                   │  (Claude Agent SDK service)    │
                                                   │  full tools: Read, Bash, Grep, │
                                                   │  WebSearch + mcp__retro__* fleet│
                                                   └────────────────────────────────┘
```

The **brain** (`scripts/retro_chat_brain.py`) and the **daemon** both run as
`systemctl --user` services that auto‑start on boot — install once with
`bash scripts/install-chat-services.sh`. Full details: [`scripts/README-chat-brain.md`](scripts/README-chat-brain.md).

Three threads run inside `retro_chat.exe`, each on its own socket:
- **`LOG_WAIT`** long-poller — streams response chunks with sub-100ms latency
- **`STATUS_WAIT`** long-poller — shows `[subagent: ...]` live activity in green
- A local spinner thread that animates `* Working... |/-\` with zero network traffic

Zero polling overhead. Zero CPU when idle. The kernel parks each socket in `recv()` until there's something to show.

### Running it, and what breaks

The chat spans three processes on two machines, so when "nothing comes back" it
helps to know where to look. Everything below was learned the hard way on the
fleet's slowest box (a 31 MB Pentium-1 running Win98).

```bash
bash scripts/chat_status.sh          # daemon? brain? claimed agents? pending prompts?
systemctl --user restart retro-chat-brain retro-chat-daemon
```

- **The box must be *claimed* by the daemon.** `chat_status.sh` lists claimed
  agents; if yours isn't there, nothing is polling its prompts and typing
  produces silence. Discovery probes the whole `/24` concurrently with an 8s
  per-host timeout — it used to be 1.5s, which quietly excluded the slow Win98
  box entirely.
- **Client slots.** `retro_chat.exe` holds **three** connections (command, log
  long-poll, status long-poll) and the daemon needs **two** more. The agent
  allows 10 (`MAX_CLIENTS`, raised from 4 in v1.22.0 — at 4 the daemon simply
  could not attach to a box running the chat locally, and operators got
  `ERR max connections reached`).
- **Win9x agents are single-threaded.** They're forced into multiplex mode
  (threaded TLS is unsafe there), so one thread serves every client and a
  blocking 30s long-poll would stall everyone else. Long-polls are clamped to
  1s in that mode; clients just re-issue.
- **Use `RESTART`, never `QUIT`.** Nothing supervises the agent on Win9x — the
  Run key only fires at logon — so a bare `QUIT` takes the box off the network
  until someone walks over to it. `RESTART` spawns a detached relaunch batch
  *before* stopping.
- **Queued tasks expire after 24h.** `scripts/retro_enqueue.py` queues work for
  a machine that's offline now; it runs when the box next appears. A stale
  `QUIT` once fired days later and killed an agent on reconnect.
- **Getting a log off a wedged box.** `retro_agent.exe -l <path>` redirects the
  agent log; point it at the share and you can read it from your dev box even
  when the agent is too busy to answer commands.


---

## Retro DOS — a game manager and the agent/chat, running on real DOS

The fleet isn't only Windows. Win98 boxes boot DOS 7.1 underneath, some machines
run real MS-DOS, and DOSBox is a first-class target — so there is a **DOS lane**
with two 16-bit real-mode programs and the host-side tooling that feeds them.

### `DOSGAME.EXE` — browse, install and play, from DOS

A TUI that turns a DOS box into something like a console games menu:

- **Finds what you already have.** It scans several roots (`C:\GAMES;C:\` by
  default — real machines keep games at the drive root) and classifies each
  folder by what it actually needs: **play** (a runnable entry, preferring one
  named after its folder and accepting `.BAT`/`.COM`), **run setup** (only a
  self-extractor like `DEICE.EXE` beside packed data — the classic Apogee/id
  shareware layout), or **unpack + setup** (only a `.ZIP`). One keypress takes
  an untouched download all the way to playable.
- **Installs from the LAN.** Tab switches to a catalogue of **~3,000 DOS titles**
  from the file share. Start typing to filter; Enter downloads the archive,
  extracts it, and runs its installer when the archive is an installer type
  (which most are). The game then appears on the Installed tab.
- **Shows gameplay previews.** F3 displays a 320x200 256-colour tile of the game
  in VGA mode 13h, auto-rendered by booting each game in DOSBox and screenshotting it.
- **Gives games all of conventional memory.** The menu writes `RUN.BAT` and exits
  with code 42; `DOSGAME.BAT` runs it and loops back, so the menu isn't resident
  while a game plays.

### `DOSCHAT.EXE` — the agent *and* the chat in one real-mode exe

One 88 KB program that is both halves of the retro chat system: it serves the
same framed TCP protocol on port 9898 and broadcasts discovery on 9899, so
`retro_chat_daemon.py` claims a DOS box exactly like a Windows one — and it
renders the chat UI in the same process (there is no loopback socket on DOS, so
the UI talks straight to the shared chat state).

**Code is shared with the Windows build rather than duplicated** — see
`agent/shared/`:

| Module | Also used by | What it is |
|---|---|---|
| `frameproto.h` | Windows agent (`src/protocol.h`) | ports, frame limits, response status bytes |
| `chatcore.[ch]` | Windows agent (`src/chatproxy.c`) | prompt slot, log ring, status sequence |
| `chattext.h` | Windows chat (`tools/retro_chat.c`) | control-byte sanitize + word wrap |

The Windows agent keeps only its NT-specific parts (critical section, the events
behind the long polls). `tests/native/test_chatcore.c` compiles the shared engine
natively and `tests/python/test_doschat_shared.py` asserts the sharing can't
silently regress.

### Installing it on a machine

The share carries a ready bundle at `…\Retro Automation\dos-setup\`: run
**`SETUPDOS.BAT`** from Windows and it copies ~870 KB into place. Then reboot
into MS-DOS mode and type:

```
PLAY          the network comes up and the game menu opens
CHAT          the DOS agent + chat
```

On Windows 9x/ME the agent also **stages these automatically at startup**
(`agent/src/dosstage.c`, v1.19.0+), pulling them from the share into
`C:\DOSGAME` and `C:\DOSCHAT` so the DOS side is ready without anyone copying
files. It is a no-op on the NT family, which has no DOS to boot into.

Registry knobs under `HKLM\Software\RetroAgent`:

| Value | Meaning |
|---|---|
| `DosStage` (DWORD) | `0` disables staging; absent/1 = on for DOS-capable boxes |
| `DosStageTiles` (DWORD) | `1` opts into the ~11 MB preview-tile payload (off by default) |
| `DosStagePath` (SZ) | override the source share |
| `DosStaged` (SZ) | timestamp of the last successful stage |

Staging is **skipped entirely below 6 MB free RAM**, and `DOSSTAGE force` does
not override that: on a 31 MB Pentium-1 the tile stream killed the agent
outright, and a cosmetic feature must never cost a box its agent.

### Networking in real DOS

`PLAY.BAT` auto-detects the network card, trying each packet driver **on its own
interrupt** — a Crynwr driver that fails to find its card still disturbs the
vector it was handed, and a later driver on that vector comes up half-broken
with DHCP timing out for no visible reason. Clean-detecting drivers (3C509,
NE2000) go first, because the ancient 3C50x drivers probe hard enough to claim a
card that isn't theirs. Whichever wins, its interrupt is written into `MTCP.CFG`
for DHCP and HTGET. No card found is never fatal — installed games still play.

Downloads go over **HTTP**, not SMB: the share's archives have long filenames
that DOS mangles to 8.3, so `serve_dosgames.py` (a systemd user unit on port
8181) bridges the share and the DOS client URL-encodes the name.

### Host-side tooling (`scripts/dosgames/`)

| File | Purpose |
|---|---|
| `dosgame.c` → `DOSGAME.EXE` | the menu (Open Watcom, 16-bit large model) |
| `survey_share.py` | reads every share archive's zip directory (no downloads) and classifies install patterns |
| `gen_catalog.py` | survey JSON → `GAMES.CAT` |
| `serve_dosgames.py` | HTTP bridge for catalog, archives and tiles |
| `gen_tiles.py` | boots each game in DOSBox-X and saves a `.PRV` preview tile |
| `dosbox_run.sh` | headless DOS test loop (DOSBox-X under Wine on a private Xvfb) |

A survey of all **3,795** archives on the share is what the install scripting is
built on: `INSTALL.EXE` / `SETUP.EXE` / `INSTALL.BAT` at the archive root covers
96% of the ones that need an installer, and 2,893 archives are flat-root.

### Traps worth knowing before touching a DOS build

- Open Watcom's `wpp` on Linux is **case-sensitive for quoted includes and
  silently skips misses** — you get a blizzard of bogus "incorrectly spelled type
  name" errors. The mTCP tree needs lowercase symlinks.
- In the 16-bit large model a **>64 KB static array silently wraps the data
  segment** — no warning, just corrupted entries past a point.
- mTCP's socket table is **one `malloc` capped at 64 KB**, so raising
  `TCP_SOCKET_RING_SIZE` halves the usable socket count and `initStack` then
  fails with a misleading "packet driver?" message.
- Every mTCP object bakes in the app `.cfg`, so the Makefile must declare
  `$(TCPOBJS): <app>.cfg` or a stale library fails for no visible reason.
- DOSBox-X's `AUTOTYPE` delivers `enter`/`tab`/plain characters but not
  `esc`/function keys — automate tests with timeouts and file assertions.

Full detail: [`scripts/dosgames/README.md`](scripts/dosgames/README.md) and
[`agent/doschat/README.md`](agent/doschat/README.md).

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
    r'EXEC copy "\\server\share\app_setup.exe" C:\TEMP\setup.exe'
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
| `MONITOR <ms> <ticks> [proc]` | v1.15+: stream one status frame per tick (foreground title, window count, watched-process alive, display mode) over one connection — real-time benchmark supervision without reconnect-polling. Client: `RetroConnection.monitor_stream()`. Ends with `END ticks=N`. |

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
| `LICSTATUS` | Report Windows activation/license status (read-only; like `slmgr /xpr`) |

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

### DOS staging & lifecycle
| Command | Description |
|---------|-------------|
| `DOSSTAGE [force]` | Stage `DOSGAME`/`DOSCHAT` from the share to `C:` (Windows 9x/ME only; refuses below 6 MB free, and `force` does not override that). Runs automatically at startup on DOS-capable boxes. |
| `RESTART` | Relaunch the agent via a detached batch, then stop. **Use this instead of `QUIT`** for a remote restart — nothing supervises the agent on Win9x. |
| `QUIT` | Stop the agent (no relaunch). |

### Fleet AI (agent v1.9.0+)
The agent proxies these to a supervised `retro-infer.exe --serve` engine on
`127.0.0.1:9896` (crash-isolated). See the
[Fleet AI docs](retro-infer/README.md#documentation).

| Command | Description | Response |
|---------|-------------|----------|
| `AI_HELLO` | Advertise ML capability: CPU/GPU backends, kernels, ISA, resident models, `ready`, and a host-GPU **`driver_flag`** | JSON |
| `MODEL_LOAD <name>` | Push a `.rim` model (two-frame, like UPLOAD) and make it resident | text |
| `MODEL_LIST` / `MODEL_UNLOAD <name>` | Enumerate / evict resident models | JSON / text |
| `INFER_RUN <name>` | Run inference (two-frame: input bytes → fp32 logits) | binary |
| `TENSOR PUT/GET/DEL <slot>` | Typed tensor store (activations, gradients) | binary/text |
| `AI_RAW` / `AI_RAWP <cmd>` | Pass any engine verb through (NT\*/GB\* training, etc.) | passthrough |
| `AI_RESTART` | Hard-restart the engine (hung GPU backend recovery) | text |

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

## Purpose & Scope

**What this is:** a remote systems-management and diagnostics agent for a
homelab of the maintainer's **own** vintage PCs on an **isolated LAN**. It is the
retro-hardware equivalent of the tools every IT department runs — think an
open-source cousin of an RMM agent, PsExec/PDQ, VNC, or Ansible for machines too
old to run any of them. The whole point of the project is to operate 25-year-old
Windows boxes that can't be managed any modern way, and to let an LLM diagnose
and fix them (drivers, boot errors, hardware faults) instead of walking to each
machine.

**Why it has broad capabilities:** remote administration inherently needs to run
commands, move files, read hardware/registry state, and see the screen — that is
the job, not a side effect. The same capability set describes every legitimate
management agent. What distinguishes this from malware is **how it is built and
operated**, and the project is deliberate about that:

- **Consent & ownership.** It runs only on machines the operator owns and
  installs it on. There is no self-propagation, no spreading, no exploitation of
  a vulnerability to gain access — installation is a deliberate copy + run by the
  owner.
- **No stealth.** It is not hidden or disguised: it runs under its own name,
  logs to a file (`-l`), announces itself on the LAN via UDP discovery, and its
  full source is in this repo. It does not hide processes, evade AV, tamper with
  security controls, or persist covertly. (The optional `AUTH_ENC` XOR layer is
  documented as *transport scrambling on a trusted LAN, not a security
  boundary* — see `agent/src/crypto.c`; tunnel over TLS/SSH for anything beyond
  the LAN.)
- **Authenticated & authorization-gated.** Every connection authenticates with a
  shared secret. The autonomous chat brain adds a **fleet-safety guardrail**
  (`scripts/retro_brain_tools.py`): destructive/irreversible commands (REBOOT,
  SHUTDOWN, DELETE, REGDELETE, PROCKILL, disk-wiping EXEC) are refused unless
  explicitly confirmed, and the brain may only confirm when the user asked.
- **Auditable & defensive.** A built-in [`security-posture`](.claude/skills/security-posture/SKILL.md)
  skill runs an authorized self-assessment of the fleet (secret strength, network
  exposure, update integrity) and proposes hardening — the project actively tests
  its own posture.

**Do not** deploy this on machines you don't own/administer, on untrusted or
internet-exposed networks, or without a strong secret. Used that way it would be
misuse — the same as any admin tool.

## Security

- **Always set a custom secret** with `-s <secret>`. The default is intentionally weak and meant only for initial testing; the `security-posture` skill flags any agent still accepting it.
- The agent listens on **all interfaces**. Restrict access via firewall rules or run on an isolated LAN — do not expose port 9898/9899 to the internet.
- The agent can execute commands, read/write files, and modify the registry. **Only deploy on machines you own and trust the connecting client to fully control.**
- Auto-update pulls binaries from an SMB share — **write-restrict that share to trusted admins** (a writable share means fleet-wide code execution).
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

> **Always publish a new build to the share.** The agents only pick up a new
> `retro_chat.exe` / `retro_agent.exe` from the share's "latest" pointer — a build
> that stays on your dev box reaches no machine. If you can't run `make release`
> with share creds, push the binary to the share's latest pointer **and** the
> versioned folder via an online agent's `copy /Y` (the fleet has the share mapped
> with write access). *(The chat **brain/daemon** are server-side Python and don't
> ship to the fleet — restart their systemd services instead.)*

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
|   +-- shared/             # code shared with the DOS build (frameproto,
|   |                       #   chatcore, chattext) - change here, not a copy
|   +-- doschat/            # DOSCHAT.EXE: agent + chat in one real-mode exe
|   +-- tools/              # retro_chat client (C) + daemon (Python)
|   +-- lib/
|       +-- libmsvcrt.a     # Patched import lib (Win98 compatible)
+-- agent-linux/            # Linux agent (C, native)
|   +-- Makefile
|   +-- src/
+-- client/                 # Python async client library
|   +-- retro_protocol.py   # TCP protocol client (RetroConnection)
|   +-- retro_discovery.py  # UDP LAN discovery
+-- voodoo-cleanroom/              # Open-source 3dfx Voodoo driver stack (see below)
+-- benchmarks/             # Driver benchmark results (JSON per run + conventions)
+-- provisioning/           # Installation scripts and registry templates
|   +-- win98/
+-- scripts/                # Chat brain/daemon, XP activation, benchmark tooling
|   +-- dosgames/           # DOSGAME.EXE + share survey, catalog, tiles, HTTP bridge
+-- retro-infer/            # Dependency-free ML engine for the fleet (Fleet AI)
+-- tests/                  # Regression suite: run_all.sh (Python + native C)
+-- .claude/skills/         # Claude Code skills for fleet operations (see below)
+-- docs/
    +-- images/             # Screenshots for this README
    +-- case-studies/       # Real-world diagnostic walkthroughs
```

## retro3dfx — An Open-Source Driver Stack for 3dfx Voodoo Cards

3dfx died in 2000 and its Windows drivers froze with it. [`voodoo-cleanroom/`](voodoo-cleanroom/README.md)
is our answer: run **Quake 3** (OpenGL) and **Unreal Tournament** (Glide) on real
Voodoo 3/4/5 hardware with **every layer — from the XP kernel display driver up
to the OpenGL ICD — built by us from source**, so the whole stack can be
optimized past what 3dfx ever shipped.

```
   Quake 3 (OpenGL)              Unreal Tournament (Glide)
        │                             │
   [3] OpenGL ICD ────────────────┐  │    retro3dfx-gl (MesaFX 6.2 fork)
        │  gr* calls              │  │    → opengl32.dll / retrogl.dll
        ▼                         ▼  ▼
   [2] glide3x.dll                        our build (retail ABI) or retro3dfx-glide fork
        │  register / FIFO writes
        ▼
   [1] XP kernel display driver           3dfxvsm.sys + 3dfxvs.dll (our build);
        │                                 vcr-disp = clean-room alternative
        ▼
   Voodoo 3 / Voodoo 5 hardware
```

The three layers:

| Layer | What we build | Source base |
|---|---|---|
| **[1] Kernel display driver** | `vcr-disp.dll` — our clean-room XPDM driver (**in progress**; 2D/D3D HAL not complete). The box currently still boots a vintage-source build (`3dfxv3d.dll`) as a **frozen legacy dependency** | `voodoo-cleanroom/vcr-disp/*.c` (original code). ⚠️ The vintage H5 display/D3D source lives in the sibling `retro-3dfx` repo, which is the **Voodoo 5 lane and is off-limits for edits from this repo** — see [Driver lane policy](#driver-lane-policy--what-this-repo-owns) |
| **[2] Glide (glide3x.dll)** | Retail-ABI Glide3 (96 exports, byte-compatible export list with the vintage Nov-2000 DLL) | Our deployed build, plus [voidsstr/retro3dfx-glide](https://github.com/voidsstr/retro3dfx-glide) (fork of sezero/glide) as the gcc-13 cross-built optimization vehicle |
| **[3] OpenGL ICD** | `retrogl.dll` — Mesa 6.2.2 OpenGL-over-Glide3, where the performance work lives | [voidsstr/retro3dfx-gl](https://github.com/voidsstr/retro3dfx-gl) (fork of sezero/MesaFX-6.2) |

**Result (2026-07-17):** the full self-built stack (**ALL-RETRO3DFX**) replaced
the community-standard AmigaMerlin driver on a real Voodoo3 AGP / XP SP3 box and
**beat it** — Quake 3 `timedemo four`, 16bpp, P3-845:

| Stack | 640x480 | 1024x768 |
|---|---|---|
| **ALL-RETRO3DFX** (self-built, all 3 layers) | **58.8 fps** | **51.3 fps** |
| AmigaMerlin hybrid, untuned (baseline) | 53.7 | 38.7 |
| Era reference: 3dfx official ICD on a V3 3000 | 75–91 | 44.3 |

The 1024x768 (fillrate-bound) number beats both AmigaMerlin and the era 3dfx
official ICD reference. The remaining 640x480 gap is CPU-side T&L in the ICD —
the queued deep work (SSE vertex emit, SSE 4-wide cliptest, end-to-end ubyte
colors).

**Open source based, licenses preserved.** The optimization work lives in real
GitHub forks of genuinely open upstreams: Glide under the **3dfx Glide Source
Code General Public License** (3dfx's authentic 2000 open release, open and
redistributable) and MesaFX under the **MIT/Mesa license** (Brian Paul et al.).
Both forks preserve the upstream license files; provenance is documented in
[`voodoo-cleanroom/FORKS.md`](voodoo-cleanroom/FORKS.md). The clean-room `vcr-disp`
display driver is original code, *modeled on* open references (Device3Dfx,
RISCyVoodoo, vmdisp9x) — read for structure, not copied.

Everything is documented in [`voodoo-cleanroom/README.md`](voodoo-cleanroom/README.md)
(architecture, ABI gotchas, build system), [`voodoo-cleanroom/CHANGELOG.md`](voodoo-cleanroom/CHANGELOG.md)
(per-version changes and rationale), [`voodoo-cleanroom/FORKS.md`](voodoo-cleanroom/FORKS.md)
(fork provenance and licenses).

### Stack layout — where the files live, build & test

Our open-source stack lives under [`voodoo-cleanroom/`](voodoo-cleanroom/); the vintage 3dfx
H5 source it's contrasted with lives in the sibling `retro-3dfx` repo. Full
orientation (and the two-ICD gotcha) is in `CLAUDE.md` → "Driver Stack Map".

| | Source | Build | Output / deploy |
|---|---|---|---|
| **OpenGL ICD (MesaFX, ours)** | `voodoo-cleanroom/build/retro3dfx-gl/src/mesa/drivers/glide/fx*.c` (fork of sezero/MesaFX-6.2) | `voodoo-cleanroom/build-stack.sh` once, then `voodoo-cleanroom/build-mesafx-retail.sh` (mingw gcc-13, `-march=pentium3 -mfpmath=sse -ffast-math`) | `voodoo-cleanroom/out/opengl32_retail.dll` (~2.7 MB) → `retrogl.dll` on .124 |
| **Glide (ours)** | `voodoo-cleanroom/build/retro3dfx-glide/` (fork of sezero/glide) | `voodoo-cleanroom/build-stack.sh` | `voodoo-cleanroom/out/glide3x.dll` |
| **Display driver (ours)** | `voodoo-cleanroom/vcr-disp/*.c` (original, GDI_DRIVER) | W2K-DDK | `vcr-disp.dll` (clean-room track) |
| **Glide2 (ours)** | `voodoo-cleanroom/build/retro3dfx-glide/glide2x/h3/` | `voodoo-cleanroom/build-stack.sh` (incl. the dual-ABI `_grFoo@N` relink Glide2 games need) | `voodoo-cleanroom/out/glide2x.dll` — see limitations |
| ⛔ **Vintage H5 display + D3D HAL / SGL ICD** — the **Voodoo 5 lane, NOT ours** | `retro-3dfx/3dfx Driver Code/**` | *(don't build from this repo)* | Read-only reference. `3dfxv3d.dll` on the V3 box is a frozen legacy dependency until `vcr-disp` lands |

- **Version:** `voodoo-cleanroom/VERSION` + `.buildnum` → **0.1.N**; renderer string
  `Mesa Glide v0.62 [voodoo-cleanroom 0.1.N]`. Per-version fixes in `voodoo-cleanroom/CHANGELOG.md`.
- **Tests:** `bash tests/run_all.sh` (Python client + agent-C + MesaFX ICD logic);
  see [`tests/README.md`](tests/README.md). Vintage H5/SGL tests are in
  `retro-3dfx/tests/`.
- **Which OpenGL ICD is this?** MesaFX (ours) = `fx*.c`, mingw, ~2.7 MB, **0.1.x**,
  `[voodoo-cleanroom 0.1.N]`. Vintage SGL = `__glSST*`/`SST_*.c`, MSVC, ~704 KB, **0.3.x**.
  Check before "fixing" or testing an ICD bug.

### The Driver Optimization Process

The stack didn't get fast by accident — it went through a disciplined
benchmark → change → measure → track loop, run entirely **remotely through the
retro agent** against a live fleet machine. Conventions and hard-won harness
rules live in [`benchmarks/README.md`](benchmarks/README.md); the full loop is
packaged as the [`driver-bench` skill](.claude/skills/driver-bench/SKILL.md).

**1. Every build self-identifies.** The ICD build injects an auto-incrementing
version into the `GL_RENDERER` string
(`Mesa Glide v0.62 Voodoo3 (tm) [voodoo-cleanroom 0.1.N]`), so every game log and
benchmark run records exactly which driver build produced it. No "which DLL was
that?" ambiguity, ever.

**2. One change per version, benchmarked before and after.** Each optimization
lands as its own version, is deployed to the test box over the agent, and gets
the same Quake 3 `timedemo four` matrix (640x480 + 1024x768, 16bpp, two runs
per resolution — the first warms the texture cache, the second is official).
The full per-version log is below; negative results are tracked as
deliberately as wins, because a documented dead end is knowledge that never
has to be re-litigated.

**3. Every result lands in a database, keyed by the exact stack.** Each run is
saved as JSON in [`benchmarks/`](benchmarks/) *and* inserted into a production
Postgres with a `driver_stack` JSONB naming the exact binary at all three
layers (plus fork commit SHAs) and a `stack_composition` tag — **`HYBRID`**
(our ICD over the retail AmigaMerlin kernel driver + glide3x) vs
**`ALL-RETRO3DFX`** (every layer self-built) — so rows are only ever compared
like-for-like.

**4. Environment discipline.** `FX_GLIDE_SWAPINTERVAL` alone moves 1024x768
results by ~30%, so the env state is recorded in every result row, and any
cross-run comparison states its tuning (see the swap-interval saga in
`voodoo-cleanroom/CHANGELOG.md` before touching vsync behavior).

**5. Quality is a tracked lever, not an afterthought.** Alongside the fps runs,
an in-engine screenshot of q3dm1 (real `glReadPixels` output, not a GDI capture
of a Glide fullscreen buffer — which is always dark/interlaced garbage) is
diffed against a pristine baseline so a "faster" driver that breaks rendering
can't slip through.

**6. The loop is one command.** `python3 .claude/skills/driver-bench/run_bench.py
--ip <target>` runs preflight (agent version, 3dfx card check), detects the
stack composition from system32 fingerprints + `GL_RENDERER`, runs the timedemo
matrix, optionally captures the quality screenshot, and writes both the JSON
drop and the DB rows.

### Optimization Log — What Changed in Each Driver Version

Every run below is Quake 3 1.32 `timedemo four`, 16bpp, on the test box
(P3-845 no-SSE2, 384 MB, Voodoo3 AGP, XP SP3). "Tuned" means
`FX_GLIDE_SWAPINTERVAL=0` in the process environment. Full rationale in
[`voodoo-cleanroom/CHANGELOG.md`](voodoo-cleanroom/CHANGELOG.md); raw per-run JSON in
[`benchmarks/`](benchmarks/).

**0.1.1 — baseline (versioning introduced).** No functional change — the
version stamp was added to `GL_RENDERER` so every subsequent log
self-documents. Benchmarks: **53.7** @640x480, 50.4 @800x600, **38.7**
@1024x768; tuned env: 57.6 / 51.0.

**0.1.2 — modern compiler codegen.** Build flags `-march=<cpu> -mfpmath=sse
-DNDEBUG` (gcc had been emitting pentiumpro **x87** for every C hot loop) plus
a branchless SSE color pack replacing a store-forwarding-stall float→ubyte
conversion at 7 call sites. The audit had found **zero** SSE instructions in
the vertex-buffer object file — on a CPU with SSE1. After: 2,729 SSE scalar
ops. Result: 54.2 @640x480 (**+0.9%**). Honest read: most of the 640x480 frame
is Q3 engine + Glide time, not the ICD's C loops.

**0.1.3 — batched triangle submission.** One `grDrawVertexArrayContiguous`
call (and 768-vertex chunked pointer arrays on the indexed path) instead of
one `grDrawTriangle` **DLL call per triangle** on Q3 world geometry. Result:
58.1 @640x480 tuned (**+0.9%**), 1024x768 flat. Lesson learned: retail glide3x
loops per-triangle internally, so only the call-boundary overhead was saved.

**0.1.4 — swap-default env injection — INERT.** Tried setting
`FX_GLIDE_SWAPINTERVAL=0` from inside the ICD before `grGlideInit`. No effect:
retail glide3x is static-CRT and snapshots the environment at **DLL load**,
before any ICD code can run. Kept as a documented dead end.

**0.1.5 — Glide state shadow cache.** Shadow copies of the texture
clamp/filter/mipmap/source and color/alpha combine state; identical Glide
calls are skipped. Q3 rebinds a texture per surface, and every bind had been
re-issuing the full 8–10-call register set. Result: 54.9 @640x480 (**+0.7%**).
The q3dm1 quality screenshot after 0.1.2–0.1.5: pristine, no regressions.

**0.1.6 — swap-interval env-read fix.** A bisect proved
`FX_GLIDE_SWAPINTERVAL=0` *alone* was the entire +32% @1024x768 tuning — and
that a system-wide `=1` (planted years ago by a 3dfx tools install) was
reaching the driver. The ICD now reads the variable with its own CRT and
defaults to 0. Result @1024x768 on the hybrid stack: still 38.7 — retail
glide3x reads the env from its own load-time snapshot and **ignores the
`grBufferSwap(interval)` argument entirely**; no ICD-side code can override
it. The real fix was owning the Glide layer ourselves (next entry).

**2026-07-17 — ALL-RETRO3DFX milestone.** A self-built kernel display driver
and our glide3x replaced AmigaMerlin entirely (SetupAPI install via the
`deploy-3dfx-driver` skill). Result: **58.8 / 51.3 with no environment tuning
at all** — our Glide's swap defaults are sane in code. Rendering verified
pristine: mean pixel diff vs the hybrid baseline screenshot is 4.1/255, i.e.
animation noise. *(Caveat, added later: the kernel driver in that run was
compiled from the vintage H5 source in the sibling Voodoo 5 repo — we built it,
but it is not clean-room code and is now frozen. Layers [2] and [3] are ours;
our own display driver, `vcr-disp`, is still in progress. See
[Driver lane policy](#driver-lane-policy--what-this-repo-owns).)*

Cumulative scoreboard:

| Config | 640x480 | 1024x768 |
|---|---|---|
| HYBRID 0.1.1, env untouched (`SWAPINTERVAL=1` system-wide) | 53.7 | 38.7 |
| HYBRID 0.1.6, env untouched | 54.2 (+0.9%) | 38.7 |
| HYBRID 0.1.6 + `FX_GLIDE_SWAPINTERVAL=0` | ~58 (+8%) | ~51 (+32%) |
| **ALL-RETRO3DFX 0.1.6, no env tuning** | **58.8 (+9.5%)** | **51.3 (+32.6%)** |
| Era references (P3-850/933 + V3 3000, 3dfx official ICD) | 75–91 | **44.3 — we beat this** |

### Driver lane policy — what this repo owns

**This repo builds and ships only its own drivers**, from
[`voodoo-cleanroom/`](voodoo-cleanroom/): the MesaFX OpenGL ICD, our Glide2 and
Glide3, and the clean-room `vcr-disp` display driver. That is the entire
supported surface for the **Voodoo 3** box.

The sibling `retro-3dfx` repo (3dfx's own vintage H5/Napalm source — display
driver, D3D/DDraw HAL, miniport, SGL ICD) is the **Voodoo 5 lane**. From this
repo it is **read-only reference**: fine to read to understand the hardware or
to port a *concept* into clean-room code, never to edit, build, or deploy.

One honest exception exists and is frozen: the Voodoo 3 box still boots the
vintage-source `3dfxv3d.dll` because `vcr-disp` cannot yet drive 2D + D3D. That
binary is a legacy dependency, not a development target — new work goes into
`voodoo-cleanroom/`, and a capability gap in our stack is never a reason to
patch the vintage tree. Finishing `vcr-disp` is the open task that retires it.

### Fixes since the 0.1.6 milestone

The optimization log above ends at the 2026-07-17 ALL-RETRO3DFX milestone.
What landed after it (full detail in
[`voodoo-cleanroom/CHANGELOG.md`](voodoo-cleanroom/CHANGELOG.md)):

| Version(s) | Change | Result |
|---|---|---|
| **0.1.7–0.1.10** | CPU-side vertex work: `-O3`/unroll, SSE 4-wide cliptest + `rcpps` perspective divide, SSE viewport emit | **None merged** — the vertex path was already near-optimal; the hand-tuned x86-asm cliptest *beat* C intrinsics. A documented dead end, not a failure |
| **0.1.11** | Quality: default texture LOD bias `-0.5` (`FX_LOD_BIAS`) | Sharper textures on the V3 bilinear + nearest-mip path (the classic 3dfx trick) |
| **0.1.12–0.1.19** | **Quake II support** on our ICD (root cause: a missing/incompatible game-local `glide3x.dll`), plus a window message-pump before `grSstWinOpen`; new `FX_NO_PALETTED_TEXTURE` / `FX_NO_MULTITEXTURE` escape hatches | Q2 **93.6 fps** @640×480×16 vs 75.7 on the stock 3dfx MiniGL (**+23%**) |
| **0.1.30** | Quality defaults: gamma ramp, 4×4 dither, alpha-PFD matcher | 16-bit output no longer renders dark/banded |
| **0.1.34** | **Fullscreen refresh rate**: `fxBestRefresh()` picks the monitor's maximum for the mode (env-overridable) instead of a hardcoded 60 Hz | Games run at the monitor's real rate — verified 60 → **100 Hz**. In fullscreen, Glide programs the video timing itself, so nothing outside the ICD (game `-freq` flags, Windows display settings) could fix this |
| **0.1.35** | **Fullscreen mouse cursor**: a software cursor is composited into the back buffer while the OS says the pointer is visible (`FX_CURSOR=0` disables). Adds `FX_DUMP_FRONT` — dumps the real scanout buffer, the only way to verify fullscreen output remotely | Menu cursors are visible again in fullscreen OpenGL games; free during gameplay (the pointer is hidden there) |
| **Glide2 (2026-08-04)** | **glide2x brought up on XP**: ported the proven Glide3 hardware-mapping fixes (prime the per-process linear map before allocating a context; reject a zero register base instead of faulting) and added the MSVC-style `_grFoo@N` exports that Glide2-era games link against | Glide2 games now **initialize and render** (Unreal Gold verified). ⚠️ Not yet stable under load — see limitations below |

### Game compatibility on the Voodoo 3 (verified on hardware)

Every game below was checked on the live box; fps figures are our own
benchmark runs, not estimates.

| Game | Renderer | Status | Notes |
|---|---|---|---|
| **Quake III Arena** | OpenGL (our ICD) | ✅ Verified | 58.6 fps @640, 50.8 @1024 — beats the era 3dfx official ICD at 1024 |
| **Quake II** | OpenGL (our ICD) | ✅ Verified | 96.6 @640, 47.1 @1024 (+23% vs stock MiniGL) |
| **Counter-Strike 1.6** (GoldSrc) | OpenGL (our ICD) | ✅ Verified | ~41 fps @1024×768×16 @100 Hz. Also runs on D3D (~34 fps). Earlier "GoldSrc unsupported" note is obsolete |
| **Half-Life** | OpenGL (our ICD) | ✅ Verified | Same engine/config as CS |
| **Return to Castle Wolfenstein** | OpenGL (our ICD) | ✅ Verified | ~55 fps (`wolfbench`) |
| **Unreal Tournament 99** | OpenGL (our ICD) | ✅ Verified | ~30 fps (`UTbench`) |
| **Medal of Honor: Allied Assault** | OpenGL (our ICD) | ✅ Renders | Needs its CD image mounted; no bundled demo for an fps number |
| **Heretic II** | OpenGL (our ICD) | ✅ Installed/verified ICD | Uses the retail Glide3 alongside our ICD (documented hybrid) |
| **Descent 3** | OpenGL (our ICD) | ✅ Installed/verified ICD | |
| **SiN** | OpenGL (**bundled 3dfx MiniGL**) | ⚠️ Deliberate exception | Our ICD can't play SiN's demos; the game keeps its original MiniGL on purpose |
| **Unreal Gold** | **Direct3D** | ⚠️ D3D only | Verified stable at 1024×768×16. Its 3dfx/Glide renderer initializes and draws with our Glide2 but wedges the chip under load |
| **Carmageddon 2** | fallback (non-Glide) | ⚠️ Glide disabled | Shipped the nGlide wrapper; neutralized (see below). Verified it still launches |
| **Battlezone, 3DMark2000** | Direct3D | ✅ Runs | Exercise the D3D path, not our ICD |
| **Incoming** | Glide2 | ❌ Skip | Crashes the driver; a Glide2-era title, blocked by the same limitation |

**nGlide warning.** GOG re-releases (Unreal Gold, Carmageddon 2) bundle
**nGlide**, a Glide→Direct3D wrapper built for modern GPUs. On *real* 3dfx
hardware it detects a card it cannot open and retries until the chip hangs —
requiring a physical power cycle. Any `glide*.dll` over ~1 MB in a game folder
is a wrapper, not a driver; ours are ~730 KB (Glide2) and ~787 KB (Glide3).
Both were renamed to `.nglide-disabled` on the fleet box.

### Keeping every game on the verified driver

Windows loads a DLL from the **game's own folder before the system folder**, so
games silently accumulate old driver copies and keep running them long after a
system-wide update. A 2026-08-04 audit of the Voodoo 3 box found **19 stale
copies across five different driver versions** — several games were months
behind while the system copy was current.

The audit is now a standard procedure (fleetbook recipe *"Fleet-wide driver
audit"*):

1. Scan **both volumes** for `opengl32.dll`, `3dfxgl.dll`, `3dfxogl.dll`,
   `retrogl.dll`, `3dfxvgl.dll`, `glide2x.dll`, `glide3x.dll`.
2. Classify each by **exact byte size** against `voodoo-cleanroom/out/` — every
   shipped version has a distinct size.
3. Update stale copies from one staged upload, keeping a `.preNNNN` backup.
   **Never** touch `system32\opengl32.dll` or `dllcache` (Microsoft's, and
   Windows File Protection reverts them).
4. **Verify by renderer string, never by file size** — launch a game with
   logging and confirm
   `GL_RENDERER: Mesa Glide v0.62 Voodoo3 (tm) [voodoo-cleanroom 0.1.N]`.

### Known limitations

- **Glide2 (`glide2x.dll`) is not production-ready.** It initializes and
  renders correctly, but wedges the graphics chip under sustained load at any
  resolution — a hard hang needing a power cycle. Glide2-era games (Unreal
  Gold, Carmageddon 2, Incoming) should use Direct3D or software rendering
  until this is fixed. Suspects and a safe reproduction harness (a standalone
  exerciser that exits cleanly — never a game) are documented in the repo.
- **`vcr-disp`, our clean-room display driver, is incomplete** — no full 2D +
  D3D HAL yet, which is why the fleet box still boots a vintage-source display
  driver.
- **SiN** needs its bundled 3dfx MiniGL; our ICD can't play its demos.
- **The 640×480 gap to era references is CPU-bound**, not driver inefficiency —
  0.1.7–0.1.10 proved the vertex path has no headroom left on this card.
- **Never force-kill a fullscreen Glide2 game.** Terminating one mid-frame
  leaves the command FIFO mid-packet and hangs the hardware. Direct3D and
  OpenGL games exit safely (their waits are bounded).

## Claude Code Skills

The repo ships [Claude Code skills](.claude/skills/) — packaged, battle-tested
operational workflows that Claude Code (or the Retro Chat brain) invokes by
name. Each `SKILL.md` encodes the tribal knowledge a task needs: preflight
checks, fleet gotchas, rollback paths, and the exact commands that work on
25-year-old Windows.

| Skill | What it does |
|---|---|
| [`driver-bench`](.claude/skills/driver-bench/SKILL.md) | One-command 3dfx driver benchmark/optimize/track loop: preflight → stack detection (ALL-RETRO3DFX / HYBRID / RETAIL) → Q3 timedemo matrix → quality screenshot → results to JSON + production DB, keyed by driver version and exact stack composition. |
| [`deploy-3dfx-driver`](.claude/skills/deploy-3dfx-driver/SKILL.md) | Deploy the self-built 3dfx XP driver package to a fleet Voodoo 3/4/5 box: HWID preflight, staged upload, backup, SetupAPI install (never raw-copy into `system32` — Windows File Protection reverts it), verify, rollback plan. Never reboots without explicit approval. |
| [`retro-benchmark`](.claude/skills/retro-benchmark/SKILL.md) | Run the full automated retro GPU benchmark suite (Quake III, Unreal Tournament, Deus Ex, Serious Sam, Giants, 3DMark 99/2000/2001) on a fleet machine unattended and collect FPS/scores into a results folder + ASCII summary. |
| [`retro-wallpaper`](.claude/skills/retro-wallpaper/SKILL.md) | Generate and deploy rotating "system dossier" wallpapers per machine — spec cards, games from the CPU- and GPU-release years, and a tech-milestone collage for the CPU year, cycled through 10 variants by an on-device rotator. |
| [`security-posture`](.claude/skills/security-posture/SKILL.md) | Interactive, authorized security self-assessment and penetration test of the agent and the fleet — auth strength, network exposure, transport, update integrity, brain autonomy controls — with ranked hardening recommendations the user replies to and applies. Scoped to the user's own isolated LAN. |

Skills are how one-off fleet victories become repeatable: once a workflow has
been debugged against real hardware (which agent commands hang on Win98, which
launch pattern actually executes on a given box, which install step needs a
backup first), it gets written down as a skill so the next invocation starts
from the answer instead of the archaeology.

## Fleet AI — training and inference on vintage hardware (contributors welcome)

The fleet is a rack of 1998–2004 machines with real period accelerators (3dfx
Voodoo 3/4/5, GeForce 2/3/4) already wired for remote control and already
carrying an **open, self-built graphics stack** we can bend to non-graphics ends.
The next arc for this project: **train and run real machine-learning models on
that hardware** — a spread from gradient-boosted trees to CNNs to a tiny
transformer — using the period GPUs as compute units and the **retro agent as
the cross-machine transport**, turning the fleet into a tiny distributed ML
cluster. *"A language model, generated one token at a time, on a Pentium II with
a Voodoo doing the matrix math — trained on the whole fleet at once."*

Everything here is **custom code** — none of these machines can run PyTorch,
CUDA, scikit-learn, or even a modern libc. That constraint is the point: it's an
interesting systems problem and every layer stays hackable. **Most of this is
now implemented and fleet-verified** — the engine lives in
[`retro-infer/`](retro-infer), highlights include bit-exact int8 LeNet-5
inference on real P3/Athlon boxes, on-device MNIST training to ≥96%, exact
XNOR GEMM + BNN CIFAR-10 inference on a real Voodoo5, bit-identical 2-node
data-parallel training, and distributed GBDT — see the status table in
[`docs/roadmap-fleet-ai.md`](docs/roadmap-fleet-ai.md) for the
milestone-by-milestone acceptance results and what's still open.


### Operating it — the engine is **opt-in**

`retro-infer.exe` must **not** run by default: on single-core vintage boxes it
steals CPU from games and skews benchmarks. Since agent **v1.17.0** it is gated
behind a persisted flag and stays off until you ask for it:

| Action | How |
|---|---|
| Enable (sets the flag + starts the engine) | `AI_ENABLE` — over chat, `mcp__retro__retro_command` with `command=AI_ENABLE` |
| Disable (clears the flag + kills the engine) | `AI_DISABLE` |
| Health-check the fleet's AI stack | the `fleet-ai-diagnose` skill |
| Live dashboard (ops/sec, loss, per-node state) | `scripts/retro_infer_console.py`, or the `fleet-ai-monitor` skill |
| Run distributed training / inference | the `fleet-ai-train` skill |

The flag lives at `HKLM\Software\RetroAgent\AIEngine` (REG_DWORD; absent or 0
means disabled). Any AI command returns an error while the engine is off.
Benchmarking quiesces it automatically — the `driver-bench` skill issues
`AI_DISABLE` and kills background CPU thieves first, because a benchmark with
any of them live reads several fps low.

### What it looks like — `retro-infer` fleet console

`scripts/retro_infer_console.py` is a live, menu-driven terminal dashboard
(`rich`-based — full color, progress bars, a real-time GPU/throughput gauge),
not a run-once CLI: it keeps repainting while discover/train/dist-infer/
infer/bench/pipeline actions run in the background, fed by a synthesized
status bus (the agent/engine protocol has no native telemetry channel, so
every "live" figure here is timing the orchestration scripts already
capture — see [`retro-infer/docs/OPERATIONS.md`](retro-infer/docs/OPERATIONS.md)
for exactly what's real vs. honestly-labeled-as-estimated). Actual captured
output from a real `dp-train` run sharded across a Pentium III/Voodoo3 and an
Athlon/Voodoo5:

```
 RETRO-INFER FLEET   12:02:48  db:ok  bus:1 active
╭───────────────── fleet ──────────────────╮╭────────── active runs ───────────╮
│  ▶ 192.168.1.124  ADMIN   cpu-sse   READY ││ dp-train-40aa3d  train  ████░ 90%│
│    192.168.1.143  1GHZ    cpu-3dnow READY ││                                  │
╰──────────────────────────────────────────╯╰──────────────────────────────────╯
 gpu / throughput: 192.168.1.124 — no vendor GPU-utilization tool, showing
 measured CPU ops/sec instead (honest > guessed)
 ops/sec (last 60 samples): ▃▄▅▅▅▅▅▅▅▅▅▅▅▅▅▅
 log: epoch=1 step=18 loss=0.19 allreduce_ms=63 nodes=2 elapsed=3s
 [d]iscover [t]rain [n]dist-infer [i]nfer [b]ench [p]ipeline [l]eaderboard [q]uit
```

On the one real NVIDIA box in the fleet (WHITEBEAST, RTX 4080 SUPER), the
gauge instead polls actual `nvidia-smi` utilization/memory/temperature/power
live — the only box with a reliable always-present vendor tool.

### The plan (phased overview)

**Phase 1 — a dependency-free ML runtime (`retro-infer`, C, i586).** A
single-binary engine cross-compiled with the same MinGW toolchain as the agent
(no CRT beyond system DLLs), supporting both **inference and on-device
training**. Ops: dense/GEMM, conv2d, pooling, activations, softmax, plus the
tree/histogram primitives for gradient boosting; float32 with **int8/int4/binary**
quantized paths (the vintage sweet spot). A compact model format (`.rim` —
quantized weights + manifest) and an offline Python converter (ONNX / a small
trainer → `.rim`). Era-vectorized CPU kernels: **SSE** (P3), **3DNow!**
(K6-2/Athlon), **MMX** integer — reusing the SSE work from the 3dfx ICD.

**Phase 2 — GPU compute backends.**
- **3dfx Voodoo / Glide backend.** No shaders, but the multitexture units and
  color-combine/blend hardware are fixed-function **multiply-accumulate** engines.
  Implement GEMM as render-to-texture accumulation: weights/activations encoded
  as textures, combiners as MACs, framebuffer as the accumulator. 8-bit/channel →
  **binarized/int8 nets** map best. Built on our open `retro3dfx` Glide stack —
  the flagship "impossible" backend.
- **NVIDIA combiner / shader backend.** GeForce 2 `NV_register_combiners`;
  GeForce 3/4 DX8 shaders + `NV_texture_shader`; GeForce FX float textures.
  Multi-pass GEMM/conv via render-to-texture — int8 on GF2–4, float16-ish on GF-FX.
- **Shared GPGPU core.** A common "tensor-over-textures" tiling layer both
  backends specialize, so a kernel is written once against an abstract MAC.

**Phase 3 — the retro agent as ML transport + AI-agent discovery.** The agent's
existing length-prefixed TCP protocol becomes the fleet ML fabric. New frames:
`AI_HELLO` (an agent advertises its inference capability — backend, precision,
GFLOP/s, resident models — extending the UDP discovery beacon), `MODEL_LOAD` /
`MODEL_LIST` (push/enumerate `.rim` models), `TENSOR` (typed tensor in/out for
activations and gradients), and `INFER_RUN`. A brain-side **registry** discovers
which boxes are AI-capable and which models live where, exposed as
`mcp__retro__ai_*` tools so the chat can list, load, train, and infer by asking.
All cross-machine movement — activations between pipeline stages, gradient
allreduce during training — rides the agent, so the whole fleet coordinates over
one transport with no extra daemons.

**Phase 4 — fleet training mode (all GPUs at once).** A "train on the whole
fleet" mode with two strategies coordinated by the brain over the agent:
- **Data-parallel SGD** — each GPU trains on a batch shard; gradients are
  averaged (ring/tree **allreduce** over `TENSOR` frames) each step. Straggler
  handling reuses the brain's failover: a dropped node's shard is reassigned.
- **Distributed gradient boosting** — histogram/split-finding for each tree
  distributed across nodes, aggregated per boosting round.
- **Pipeline parallelism** for models too big for one box — one layer/block per
  machine, activations streamed stage→stage (the Pentium II runs layer 3 while
  the Athlon runs layer 7). Checkpoints (`.rim`) land on the SMB share.

**Phase 5 — tracked metrics + benchmark harness.** Mirror the
[`driver-bench`](.claude/skills/driver-bench/SKILL.md) pattern: every training
run and inference run keyed by model, machine, GPU/CPU backend, and precision,
landing in the specpicks DB with the **full ML metric set** — training/val
**loss & error rate**, **accuracy / top-5**, **precision / recall / F1**,
**AUC-ROC**, **confusion matrix**, **log-loss**, **RMSE/MAE** (regression),
**perplexity** (LM), **PSNR** (autoencoders), plus systems metrics
**throughput** (img/tok/sec), **latency p50/p99**, and **energy per inference**.
Leaderboards per model × backend × precision.

### Model zoo to build, train, and benchmark

A deliberate spread of model *families* (classical → boosted → neural → their
variations), chosen to fit the envelope (KBs–low MBs, integer-friendly):

| Family | Model | Task | Key metrics |
|---|---|---|---|
| Linear | logistic regression, perceptron | MNIST / tabular | accuracy, log-loss, error rate |
| Instance | k-NN | MNIST | accuracy, inferences/sec (calibration baseline) |
| **Boosting** | **gradient-boosted trees (GBDT)** | tabular classify/regress | log-loss, AUC-ROC, RMSE, trees/sec |
| Bagging | random forest | tabular | accuracy, F1, OOB error |
| Margin | linear SVM | binary classify | accuracy, precision/recall, hinge loss |
| MLP | 2–3 layer dense | MNIST | accuracy, loss curve, err% |
| **CNN** | **LeNet-5** (int8) | MNIST digits | img/sec on Voodoo-GEMM vs GeForce vs SSE |
| CNN variant | **binarized CNN (BNN, XNOR)** | CIFAR-10 (small) | accuracy, top-5, img/sec — best Voodoo showcase |
| Recurrent | char-RNN / **GRU / LSTM** | text gen | perplexity, chars/sec |
| **Transformer** | **nanoGPT-class** (1–6 layers, tiny vocab, int8) | token generation | **tokens/sec**, perplexity, single-box vs fleet pipeline |
| Generative | small VAE / autoencoder | image denoise | reconstruction PSNR, frames/sec |
| Audio | tiny keyword spotting | wake-word off a real SB16 | detection F1, detections/sec |

The GBDT-on-tabular result is the "ML actually works here" proof; the
fleet-pipelined transformer is the "wow" — an end-to-end language model whose
layers each live on a different 25-year-old computer, talking over the agent.

### Good first contributions

- A Glide render-to-texture **GEMM kernel** (Phase 2) against `retro3dfx`.
- An `NV_register_combiners` GEMM pass for GeForce 2 (Phase 2).
- The **int4/int8 weight packer** + ONNX→`.rim` converter (Phase 1).
- A **distributed GBDT** split-finder over `TENSOR` frames (Phases 1, 4).
- The **allreduce** primitive and pipeline scheduler in the brain (Phases 3, 4).
- The **`retro-infer` ASCII console** + live metric stream (Phase 5).

See [`docs/roadmap-fleet-ai.md`](docs/roadmap-fleet-ai.md) for the full milestone
list (M0–M8), each with its deliverables and the acceptance tests to run when
it's implemented. Open an issue with the piece you want to take — custom code,
weird hardware, real numbers.

## Contributing

The agent is designed to be extended with new commands. Each command is a C function that receives a socket and arguments, dispatched via the command table in `handlers.c`. Add your handler, register it in the table, and rebuild.

The chat proxy protocol is intentionally simple (JSON files in directories) so you can plug in any LLM backend — not just Claude. Write a script that reads from `inbox/`, calls your model, and writes to `outbox/`. The daemon handles the network transport.

## License

The code in this repository is **open source based**:

- **This repository** (agent, chat client, Python client library, scripts,
  retro3dfx build system): **MIT** — see [`LICENSE`](LICENSE).
- **retro3dfx driver forks** preserve their upstream open licenses: Glide is
  under the **3dfx Glide Source Code General Public License** (3dfx's genuine
  2000 open release), MesaFX under the **MIT/Mesa license**. Provenance:
  [`voodoo-cleanroom/FORKS.md`](voodoo-cleanroom/FORKS.md).
- `vcr-disp/` (the clean-room display-driver track) is original,
  MIT-licensed code.
