# Retro Chat Brain — a standalone Claude-Code-grade agent for the retro chat

The chat brain turns the retro chat into a full agentic assistant **without
needing an interactive Claude Code session open on this machine**. It's meant to
be used in lieu of having Claude on the box: a user on a Win98/2K/XP machine can
chat and get a Claude-Code-equivalent agent that can read/edit files, run
commands, search the web — and **operate the retro fleet directly**.

It's built on the official **[Claude Agent SDK](https://code.claude.com/docs/en/agent-sdk/overview)**
(`claude-agent-sdk`), the same engine Claude Code runs on. (Claude Code itself is
closed-source — there is no repo to copy; the Agent SDK is the supported way to
embed that engine.)

## Architecture

The chat **daemon** (`nsc-assistant/agent/tools/retro_chat_daemon.py`) is a
pure network multiplexer that drops prompts into `/tmp/retro-chat/inbox/` and
streams answers from `/tmp/retro-chat/outbox/`. The brain replaces the old
"spawn a Claude Code subagent" processor:

```
retro_chat.exe ─ retro_agent.exe ─ retro_chat_daemon.py ─┬─ inbox/  ◀ prompts
                                                         └─ outbox/ ▶ answers
                                                               ▲
                                               retro_chat_brain.py (this) ── Claude Agent SDK
                                                               │
                                   built-in tools (Bash/Read/Edit/Web…) + mcp__retro__* fleet tools
```

- Per retro machine, a **resumable Agent SDK session** → each PC has a continuous
  conversation. The latest session id per machine is saved in
  `~/.retro-fleet/brain-sessions.json` (`RETRO_BRAIN_SESSIONS`), and the
  transcript is copied into whichever account resumes it, so a brain restart
  or an account failover keeps it.
- Per (machine, account), **one live `claude` process** (`ClaudeSDKClient`),
  created on the first prompt and reused, instead of a fresh CLI per prompt.
  Closed after 30 min idle, recreated on any error or account failover.
- Writes `processor.heartbeat` so the existing `chat_status.sh` check "just
  works" (it already looks for that file).
- Live status: each tool call writes a `status_outbox` one-shot the daemon shows
  as `[subagent: running: Bash]` on the retro screen; the full answer follows.

## Fleetbook — persistent memory of solved problems

The brain is prompted to use **`scripts/retro_fleetbook.py`** (SQLite at
`~/.retro-fleet/fleetbook.db`) on every fix/change cycle: `search`/`show`
before diagnosing ("have we solved this before?" — reusable **recipes** with
the exact fix steps), and `log --host <ip> --summary ... [--recipe <slug>]`
after completing work, building a per-machine **change log** over time.
`history --host <ip>` answers "what changed on this box?". New hard-won fixes
get stored with `add`. Contract tests: `tests/python/test_fleetbook.py`.

## Deferred task queue (run-on-next-connect)

The daemon also drains a **per-host task queue** whenever it (re)connects to a
machine and on each idle poll cycle — so you can queue agent commands for a
machine that's **offline right now** and they run automatically the moment it
comes back online. Plain files in a **durable** directory (it used to be tmpfs
`/tmp/retro-chat/tasks/`, which a host reboot wiped; the daemon migrates
anything left there when it starts):

```
~/.retro-fleet/chat-tasks/<ip>/<ts>-<slug>.json   pending  (oldest name runs first)
~/.retro-fleet/chat-tasks/<ip>/done/<name>        completed (with captured output)
~/.retro-fleet/chat-tasks/<ip>/failed/<name>      unreachable 3x, expired (>24 h),
                                                  or timed out AFTER it was sent
```

A pending file is JSON: `{"cmds": ["EXEC ...", "UICLICK 10 20"], "label": "...",
"attempts": 0}` (`"cmd": "single"` also accepted). A command that reaches the
agent but errors is recorded and the task still completes. Each command's reply
deadline is sized from the command (`EXECW n` → n + 30 s, `EXEC` → 90 s, others
60 s; more on Win9x). A failure **before** a command was sent is retried (the
task resumes at that command, never re-running earlier ones); a command that
got no reply **after** it was sent is never re-run — it may have run — and the
task goes to `failed/` with that reason. `RETRO_CHAT_TASKS` overrides the path.

Queue tasks with either front-end (both just write the same file — no daemon
restart needed):

```bash
# from this repo (self-contained, no imports)
scripts/retro_enqueue.py 192.168.1.123 "EXEC C:\\retro-wall\\arrange_icons.exe" --label "park icons"
scripts/retro_enqueue.py --list                       # show everything pending

# or via the daemon's own CLI
python3 ../nsc-assistant/agent/tools/retro_chat_daemon.py \
    --enqueue 192.168.1.123 --cmd "SYSINFO" --label "inventory"
```

## Reliability — what the daemon and brain guarantee (2026-09)

Each of these was a failure in the journals before it was a rule.

**Prompts are never silently lost.**
- The daemon writes `inbox/<host>-<seq>.json` *before* its next `PROMPT_WAIT`
  (agents 1.85.0+ keep a taken prompt until that next command; if the
  connection drops first they hand it over again).
- The brain moves a prompt `inbox/` → `inbox/queued/` → `inbox/processing/`
  and deletes it only when the answer is done. After a brain restart, queued
  prompts run normally; a prompt that was **running** is *not* replayed (it may
  have been a fleet operation) — the box is told it was interrupted and to
  resend it, and the prompt is kept in `interrupted/`.
- Sequence numbers are seeded from the clock, so a re-claimed box never
  overwrites its own history.

**Answers arrive once, in order, and one dead box never stalls another.**
- One sender per host. It stops at the first failure and **holds** that host's
  files in order until the box answers again (at most 1 h, then `failed/`); it
  never skips ahead, and it never delays another host.
- Consecutive pending chunks are merged into one append (up to 3000 bytes).
- On agents 1.85.0+ text goes out as `LOG_APPEND2 <id> <text>`; a resend after
  a timeout carries the same id and the agent drops the duplicate. Older agents
  answer it with an error, and the daemon falls back to `LOG_APPEND` for that
  connection — where a sent-but-unanswered append is **not** resent (it has
  almost always landed; resending is what showed answers twice).
- Only the **newest** status per host is forwarded; a status older than 60 s is
  dropped. A status that could not be sent is retried once on a fresh
  connection.

**Connections stay alive, and are always closed with a FIN.**
- `RetroConnection.connected` is False once the agent has closed its end (the
  agent drops a client silent for 120 s on NT, 300 s on Win9x), and the idle
  send connection is `PING`ed after 60 s.
- A host is released only after **120 s without any reply** — never because a
  discovery sweep missed it, never while a command of ours is in flight. Its
  held answer text stays on disk for when it is claimed again.
- Discovery probes only unclaimed addresses: every 15 s while nothing is
  claimed or a sweep finished in under 1 s (the network is not up yet — the
  first scan after boot), every 60 s otherwise. The daemon also listens for the
  agents' own UDP announcements on 9899 and claims a box within one of them.
- Timeouts are longer for single-threaded agents (Win9x `Win4.x`, DOS): 30 s
  per command, poll + 15 s for `PROMPT_WAIT`. A connection to such an agent is
  half-closed and **drained in the background** (up to 90 s) instead of closed,
  so a late reply from a busy box is read rather than answered with an RST.
- On SIGTERM the daemon closes every agent connection with a FIN and drains it
  before exiting (a systemd restart used to reset them all).

**Prompts are bounded.** A prompt queued behind another on the same machine
says `queued behind your previous request` on the status line, and a prompt is
stopped after `RETRO_BRAIN_PROMPT_TIMEOUT` (default 15 min) of wall clock with a
message saying so; its `claude` process (and any tool it was running) is ended.

Tests: `tests/python/test_chat_daemon_{delivery,liveness,tasks,conn_safety,reap_survival}.py`,
`tests/python/test_chat_brain_resilience.py`,
`tests/python/test_protocol_connection_state.py` (point `NSC_ASSISTANT_DIR` at
an nsc-assistant worktree to test a daemon change before it lands).

## Files

| File | Purpose |
|------|---------|
| `retro_chat_brain.py` | The processor service: inbox → Agent SDK loop → outbox. |
| `retro_brain_tools.py` | In-process MCP server exposing the fleet as tools (`retro_list_machines`, `retro_command`, `retro_screenshot`, `retro_upload`, `retro_download`, `ai_*`). |
| `retro-chat-brain.service` | `systemctl --user` unit (matches the game-server convention). |
| `retro_chat_brain_supervisor.sh` | Bash auto-restart fallback if you don't use systemd. |
| `.brain-venv/` | Dedicated virtualenv (git-ignored). |

## Setup

```bash
cd scripts
python3 -m venv .brain-venv
.brain-venv/bin/pip install claude-agent-sdk pillow
```

Auth: the brain drives the already-logged-in `claude` CLI, so no API key is
required for personal use. For a fully unattended box, set `ANTHROPIC_API_KEY`
(uncomment it in the `.service` file).

## Run

**systemd (recommended):**
```bash
mkdir -p ~/.config/systemd/user
cp scripts/retro-chat-brain.service ~/.config/systemd/user/
loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable --now retro-chat-brain
journalctl --user -u retro-chat-brain -f
```

**Supervisor fallback:**
```bash
nohup bash scripts/retro_chat_brain_supervisor.sh > /tmp/retro-chat/brain-sup.log 2>&1 &
```

**Foreground (debug):**
```bash
scripts/.brain-venv/bin/python scripts/retro_chat_brain.py
```

## The fleet tools

The brain can operate the retro PCs through `client/retro_protocol.py`:

- `retro_list_machines` — find a machine's IP (known + LAN discovery)
- `retro_command` — run any agent command (`SYSINFO`, `EXEC ...`, `VIDEODIAG`,
  `REGREAD ...`, `UICLICK x y`, ...) on a fleet machine
- `retro_screenshot` — capture a machine's screen as a PNG the model can **see**
  (drives a screenshot→UICLICK GUI-automation loop)
- `retro_upload` — push a file from this host's disk to a machine (two-frame
  `UPLOAD` protocol; how driver DLLs, `.reg` files, and tools get onto a box)
- `retro_download` — pull a file off a machine (logs, `setupapi.log` tails,
  backups) for local analysis

A user sitting at one machine can say "the GeForce2 box is stuck at 640×480" and
the brain screenshots that machine, reasons over the image, and fixes it.

### 3dfx driver development over chat

The brain has the full driver-dev capability of the interactive sessions: both
3dfx driver codebases live on this host with their toolchains and regression
suites, and two skills encode the complete workflows —
`voodoo3-driver-dev` (our clean-room MesaFX/Glide stack in
`voodoo-cleanroom/`, box .124) and `voodoo5-driver-dev` (the vintage H5 stack
in `../retro-3dfx/`, box .143). Each covers build, test gates, diagnosis
(renderer strings, the registry-ring flight recorder, d3dlab), fix policy, and
deploy (via `retro_upload` + the deploy-3dfx-driver / driver-install /
driver-bench skills). So "CS looks washed out on the Voodoo5" typed into retro
chat can end in a built, tested, deployed driver fix.

### ⚠️ Single-connection caveat

The daemon holds a persistent connection to the **originating** machine for the
chat channel. The fleet tools default to that same machine when `host` is
omitted — opening a *second* connection to a single-threaded agent, which can
briefly contend with the live chat (stalls, not crashes; closes are always
graceful). **Prefer operating *other* fleet machines by explicit IP**, or run the
chat from a different box than the one being fixed. (A future enhancement could
route origin-host tool calls through the daemon's existing connection.)

## Tuning (env vars)

| Var | Default | |
|-----|---------|--|
| `RETRO_BRAIN_MODEL` | `claude-opus-4-8` | drop to `claude-haiku-4-5` for cheap/fast |
| `RETRO_BRAIN_EFFORT` | `medium` | `low`/`medium`/`high`/`xhigh`/`max` |
| `RETRO_BRAIN_MAX_TURNS` | `0` | agentic-loop cap per prompt; `0` (default) = unbounded |
| `RETRO_BRAIN_PROMPT_TIMEOUT` | `900` | wall-clock cap per prompt, seconds; `0` = none |
| `RETRO_BRAIN_PERSISTENT` | `1` | `0` = spawn a fresh `claude` per prompt (old path) |
| `RETRO_BRAIN_IDLE_CLOSE` | `1800` | close a machine's idle `claude` process after this |
| `RETRO_BRAIN_SESSIONS` | `~/.retro-fleet/brain-sessions.json` | where the session ids are kept |
| `RETRO_CHAT_ROOT` | `/tmp/retro-chat` | must match the daemon |
| `ANTHROPIC_API_KEY` | (unset) | uses `claude` CLI login if absent |
