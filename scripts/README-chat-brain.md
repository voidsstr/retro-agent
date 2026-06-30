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

The chat **daemon** (`nsc-assistant/agent/tools/retro_chat_daemon.py`) is
unchanged — it's a pure network multiplexer that drops prompts into
`/tmp/retro-chat/inbox/` and streams answers from `/tmp/retro-chat/outbox/`. The
brain replaces the old "spawn a Claude Code subagent" processor:

```
retro_chat.exe ─ retro_agent.exe ─ retro_chat_daemon.py ─┬─ inbox/  ◀ prompts
                                                         └─ outbox/ ▶ answers
                                                               ▲
                                               retro_chat_brain.py (this) ── Claude Agent SDK
                                                               │
                                   built-in tools (Bash/Read/Edit/Web…) + mcp__retro__* fleet tools
```

- Per retro machine, a **resumable Agent SDK session** → each PC has a continuous
  conversation.
- Writes `processor.heartbeat` so the existing `chat_status.sh` check "just
  works" (it already looks for that file).
- Live status: each tool call writes a `status_outbox` one-shot the daemon shows
  as `[subagent: running: Bash]` on the retro screen; the full answer follows.

## Files

| File | Purpose |
|------|---------|
| `retro_chat_brain.py` | The processor service: inbox → Agent SDK loop → outbox. |
| `retro_brain_tools.py` | In-process MCP server exposing the fleet as tools (`retro_list_machines`, `retro_command`, `retro_screenshot`). |
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

A user sitting at one machine can say "the GeForce2 box is stuck at 640×480" and
the brain screenshots that machine, reasons over the image, and fixes it.

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
| `RETRO_BRAIN_EFFORT` | `high` | `low`/`medium`/`high`/`xhigh`/`max` |
| `RETRO_BRAIN_MAX_TURNS` | `60` | agentic-loop cap per prompt |
| `RETRO_CHAT_ROOT` | `/tmp/retro-chat` | must match the daemon |
| `ANTHROPIC_API_KEY` | (unset) | uses `claude` CLI login if absent |
