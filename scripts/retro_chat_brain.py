#!/usr/bin/env python3
"""Retro Chat Brain — a standalone Claude-Code-grade agent for the retro chat.

Replaces the "spawn a Claude Code subagent" processor. This is a long-running
service that turns the retro chat into a full agentic assistant WITHOUT needing
an interactive Claude Code session on this machine — exactly so the fleet's
retro PCs can chat with Claude even when no Claude Code window is open.

Pipeline (the daemon is unchanged — same inbox/outbox/heartbeat contract):

    retro_chat.exe ─ retro_agent.exe ─ retro_chat_daemon.py ─┬─ inbox/  ◀ prompts
                                                             └─ outbox/ ▶ answers
                                                                   ▲
                                                   retro_chat_brain.py (this) ─ Claude Agent SDK
                                                                   │
                                                       built-in tools (Bash/Read/Edit/Web…)
                                                       + mcp__retro__* fleet tools

Per machine we keep a resumable Agent SDK session, so each retro PC has a
continuous conversation. The brain has the full Claude Code tool suite on THIS
Linux box plus the retro fleet exposed as tools (see retro_brain_tools.py).

Run:  scripts/.brain-venv/bin/python scripts/retro_chat_brain.py
(usually via the systemd unit or supervisor — see scripts/README-chat-brain.md)
"""

import asyncio
import json
import logging
import os
import shutil
import sys
import time
import unicodedata
from pathlib import Path

# The retro console renders ASCII only (the client strips high-bit bytes, and
# the daemon's send path is ASCII). Fold the model's Unicode down to ASCII so
# replies actually deliver instead of failing on a stray smart-quote or check.
_ASCII_MAP = {
    "–": "-", "—": "-", "−": "-",          # dashes / minus
    "‘": "'", "’": "'", "‚": "'",            # single quotes
    "“": '"', "”": '"', "„": '"',            # double quotes
    "•": "-", "·": "-", "●": "-", "▪": "-",  # bullets
    "…": "...", " ": " ", " ": " ", " ": " ",
    "✓": "[ok]", "✔": "[ok]", "✗": "x", "✘": "x",
    "→": "->", "←": "<-", "⇒": "=>",
    "≥": ">=", "≤": "<=", "×": "x", "°": " deg",
}


def ascii_clean(s):
    for k, v in _ASCII_MAP.items():
        s = s.replace(k, v)
    s = unicodedata.normalize("NFKD", s)
    return s.encode("ascii", "ignore").decode("ascii")

_REPO = Path(__file__).resolve().parent.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from claude_agent_sdk import (  # noqa: E402
    AssistantMessage,
    ClaudeAgentOptions,
    ResultMessage,
    StreamEvent,
    SystemMessage,
    TextBlock,
    ToolUseBlock,
    query,
)

import scripts.retro_brain_tools as fleet  # noqa: E402

# ---------------------------------------------------------------------------
# Daemon contract (must match retro_chat_daemon.py)
# ---------------------------------------------------------------------------
ROOT = Path(os.environ.get("RETRO_CHAT_ROOT", "/tmp/retro-chat"))
INBOX = ROOT / "inbox"
OUTBOX = ROOT / "outbox"
STATUS_OUTBOX = ROOT / "status_outbox"
HEARTBEAT = ROOT / "processor.heartbeat"
BRAIN_LOG = ROOT / "brain.log"

POLL_INTERVAL = 0.05      # tight inbox poll for low prompt-pickup latency
HEARTBEAT_INTERVAL = 20   # chat_status.sh flags the processor stale after 120s

MODEL = os.environ.get("RETRO_BRAIN_MODEL", "claude-opus-4-8")
# 'medium' keeps chat snappy (less pre-output thinking) while staying capable
# enough for fleet ops; bump to high/xhigh via env for heavier tasks.
EFFORT = os.environ.get("RETRO_BRAIN_EFFORT", "medium")
MAX_TURNS = int(os.environ.get("RETRO_BRAIN_MAX_TURNS", "60"))

BUILTIN_TOOLS = [
    "Read", "Write", "Edit", "Bash", "Glob", "Grep",
    "WebSearch", "WebFetch", "Agent",
]
ALLOWED_TOOLS = BUILTIN_TOOLS + fleet.TOOL_NAMES

SYSTEM_APPEND = """
You are operating as the brain of the **Retro Chat** bridge. A user is chatting
with you from a retro PC (Win98 / Win2K / WinXP) through a text relay, so:

- Output PLAIN ASCII ONLY — the retro console cannot render Unicode. No emoji,
  smart quotes, em-dashes, box-drawing, bullets, or check/cross marks. Use -, ',
  ", ... and plain words instead.
- Keep answers concise and readable on a small CRT — lead with the outcome.
- You are autonomous: the user cannot click approval dialogs, so just act.
- You have the full toolset on THIS Linux host (Bash/Read/Edit/Web/...) AND the
  `mcp__retro__*` tools to operate the retro fleet directly:
    * retro_list_machines — find a machine's IP
    * retro_command       — run any agent command (SYSINFO, EXEC, VIDEODIAG,
                            REGREAD, UICLICK x y, ...) on a fleet machine
    * retro_screenshot    — see a machine's screen (drives screenshot→UICLICK)
- The chat is coming from a specific machine; retro tools default to THAT machine
  when you omit `host`. Operating the *originating* machine can briefly contend
  with the live chat channel — prefer operating other fleet machines by explicit
  IP when you can, and never issue REBOOT/SHUTDOWN without the user asking.
- This repo's CLAUDE.md documents the fleet, the Win98 gotchas (vcache, ghost
  PCI, 3dfx drivers), and the agent command reference — consult it.
"""


def setup():
    for d in (ROOT, INBOX, OUTBOX, STATUS_OUTBOX):
        d.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[logging.FileHandler(str(BRAIN_LOG)), logging.StreamHandler(sys.stderr)],
    )


log = logging.getLogger("retro_chat_brain")


def beat():
    try:
        HEARTBEAT.touch()
    except Exception:  # noqa: BLE001
        pass


def write_status(host, text):
    """Tiny one-shot the daemon forwards as '[subagent: <text>]' instantly."""
    try:
        p = STATUS_OUTBOX / f"{host}-{int(time.time()*1000)}.json"
        p.write_text(json.dumps({"host": host, "text": ascii_clean(text)}))
    except Exception:  # noqa: BLE001
        pass


def write_response(host, seq, response):
    p = OUTBOX / f"{host}-{seq}.json"
    p.write_text(json.dumps({"host": host, "seq": seq, "response": response}))


def _find_cli():
    """Locate the `claude` binary — PATH may be bare under systemctl --user."""
    cli = shutil.which("claude")
    if cli:
        return cli
    for cand in (Path.home() / ".local/bin/claude", Path("/usr/local/bin/claude")):
        if cand.exists():
            return str(cand)
    return None


_CLI_PATH = _find_cli()


def options_for(host, resume):
    opts = dict(
        model=MODEL,
        effort=EFFORT,
        system_prompt={"type": "preset", "preset": "claude_code", "append": SYSTEM_APPEND},
        allowed_tools=ALLOWED_TOOLS,
        mcp_servers={"retro": fleet.build_retro_server()},
        permission_mode="bypassPermissions",  # autonomous — no one to approve
        cwd=str(_REPO),                        # loads this repo's CLAUDE.md
        resume=resume,
        max_turns=MAX_TURNS,
        include_partial_messages=True,         # token-level deltas -> live streaming
    )
    if _CLI_PATH:
        opts["cli_path"] = _CLI_PATH
    return ClaudeAgentOptions(**opts)


def tool_status(name, tool_input):
    """One-line 'what the brain is doing' summary for the live status feed."""
    ti = tool_input or {}
    base = os.path.basename
    if name == "Bash":
        return "bash: " + (ti.get("command") or "").replace("\n", " ")[:48]
    if name == "Read":
        return "read: " + base(ti.get("file_path", ""))
    if name in ("Edit", "Write"):
        return name.lower() + ": " + base(ti.get("file_path", ""))
    if name == "Grep":
        return "search: " + str(ti.get("pattern", ""))[:32]
    if name == "Glob":
        return "glob: " + str(ti.get("pattern", ""))[:32]
    if name == "WebSearch":
        return "web search: " + str(ti.get("query", ""))[:40]
    if name == "WebFetch":
        return "fetch: " + str(ti.get("url", ""))[:40]
    if name == "Agent":
        return "subagent: " + str(ti.get("description", ""))[:40]
    if "retro_command" in name:
        return "retro: " + str(ti.get("command", ""))[:40]
    if "retro_screenshot" in name:
        return "retro: screenshot " + str(ti.get("host", "") or "")
    if "retro_list" in name:
        return "retro: list machines"
    return "running: " + name


async def run_prompt(host, seq, prompt, sessions):
    """Stream one prompt through the agent loop, emitting the answer line-by-line
    and a live status feed of what the brain is doing/thinking."""
    fleet.set_origin_host(host)
    write_status(host, "thinking...")

    state = {"buf": "", "idx": 0, "any": False, "think": "", "shown": 0}

    def emit(text):
        if not text:
            return
        state["idx"] += 1
        p = OUTBOX / f"{host}-{seq}-{state['idx']:06d}.json"
        # stream=True => daemon concatenates chunks without per-chunk newlines
        p.write_text(json.dumps(
            {"host": host, "seq": seq, "chunks": [ascii_clean(text)], "stream": True}))
        state["any"] = True

    def feed(delta):
        # Flush only on line boundaries so the console renders clean lines.
        state["buf"] += delta
        if "\n" in state["buf"]:
            cut = state["buf"].rfind("\n") + 1
            emit(state["buf"][:cut])
            state["buf"] = state["buf"][cut:]

    try:
        async for msg in query(prompt=prompt, options=options_for(host, sessions.get(host))):
            if isinstance(msg, StreamEvent):
                if msg.parent_tool_use_id:      # subagent's internal stream — skip
                    continue
                ev = msg.event or {}
                etype = ev.get("type")
                if etype == "content_block_delta":
                    d = ev.get("delta") or {}
                    if d.get("type") == "text_delta":
                        feed(d.get("text", ""))
                    elif d.get("type") == "thinking_delta":
                        t = d.get("thinking", "")
                        if t:
                            state["think"] += t
                            # throttle thinking->status to ~every 30 new chars
                            if len(state["think"]) - state["shown"] >= 30:
                                state["shown"] = len(state["think"])
                                snip = state["think"].replace("\n", " ").strip()
                                if snip:
                                    write_status(host, "thinking: " + snip[-70:])
                elif etype == "content_block_start":
                    if (ev.get("content_block") or {}).get("type") == "thinking":
                        write_status(host, "thinking...")
            elif isinstance(msg, AssistantMessage):
                for block in getattr(msg, "content", []) or []:
                    if isinstance(block, ToolUseBlock):
                        write_status(host, tool_status(block.name, block.input))
            elif isinstance(msg, SystemMessage):
                data = getattr(msg, "data", {}) or {}
                if data.get("session_id"):
                    sessions[host] = data["session_id"]
            elif isinstance(msg, ResultMessage):
                sid = getattr(msg, "session_id", None)
                if sid:
                    sessions[host] = sid
                result = getattr(msg, "result", None)
                if result and not state["any"]:
                    feed(result if result.endswith("\n") else result + "\n")
        if state["buf"]:
            emit(state["buf"])               # final partial line
        if not state["any"]:
            emit("(no response generated)\n")
    except Exception as e:  # noqa: BLE001
        log.exception("query failed for %s seq=%s", host, seq)
        emit(f"[brain error: {e}]\n")


async def heartbeat_loop():
    while True:
        beat()
        await asyncio.sleep(HEARTBEAT_INTERVAL)


async def main():
    setup()
    beat()
    log.info(
        "retro_chat_brain started (model=%s effort=%s, watching %s)",
        MODEL, EFFORT, INBOX,
    )
    asyncio.create_task(heartbeat_loop())
    sessions = {}  # host -> Agent SDK session_id (continuous per machine)

    while True:
        beat()
        files = sorted(INBOX.glob("*.json"))
        for f in files:
            try:
                data = json.loads(f.read_text())
            except Exception:  # noqa: BLE001
                log.warning("bad inbox file %s, removing", f.name)
                f.unlink(missing_ok=True)
                continue
            host = data.get("host")
            seq = data.get("seq")
            prompt = data.get("prompt", "")
            if not host or seq is None:
                f.unlink(missing_ok=True)
                continue
            log.info("prompt host=%s seq=%s: %.80s", host, seq, prompt.replace("\n", " "))
            f.unlink(missing_ok=True)  # consume now so it isn't reprocessed mid-stream
            await run_prompt(host, seq, prompt, sessions)  # streams its own output
            log.info("answered host=%s seq=%s", host, seq)
        await asyncio.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
