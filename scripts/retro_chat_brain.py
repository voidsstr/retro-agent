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
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

from claude_agent_sdk import (  # noqa: E402
    AssistantMessage,
    ClaudeAgentOptions,
    ResultMessage,
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

POLL_INTERVAL = 0.5       # inbox poll cadence (prompts are human-paced)
HEARTBEAT_INTERVAL = 20   # chat_status.sh flags the processor stale after 120s

MODEL = os.environ.get("RETRO_BRAIN_MODEL", "claude-opus-4-8")
EFFORT = os.environ.get("RETRO_BRAIN_EFFORT", "high")
MAX_TURNS = int(os.environ.get("RETRO_BRAIN_MAX_TURNS", "60"))

BUILTIN_TOOLS = [
    "Read", "Write", "Edit", "Bash", "Glob", "Grep",
    "WebSearch", "WebFetch", "Agent",
]
ALLOWED_TOOLS = BUILTIN_TOOLS + fleet.TOOL_NAMES

SYSTEM_APPEND = """
You are operating as the brain of the **Retro Chat** bridge. A user is chatting
with you from a retro PC (Win98 / Win2K / WinXP) through a text relay, so:

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
        p.write_text(json.dumps({"host": host, "text": text}))
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
    )
    if _CLI_PATH:
        opts["cli_path"] = _CLI_PATH
    return ClaudeAgentOptions(**opts)


async def run_prompt(host, seq, prompt, sessions):
    """Run one prompt through the agent loop; stream status; return final text."""
    fleet.set_origin_host(host)
    write_status(host, "thinking…")
    collected = []
    session_id = sessions.get(host)
    try:
        async for msg in query(prompt=prompt, options=options_for(host, session_id)):
            beat()
            if isinstance(msg, SystemMessage):
                sid = getattr(msg, "data", {}) or {}
                if sid.get("session_id"):
                    sessions[host] = sid["session_id"]
            elif isinstance(msg, AssistantMessage):
                for block in getattr(msg, "content", []) or []:
                    if isinstance(block, TextBlock) and block.text.strip():
                        collected.append(block.text)
                    elif isinstance(block, ToolUseBlock):
                        write_status(host, f"running: {block.name}")
            elif isinstance(msg, ResultMessage):
                sid = getattr(msg, "session_id", None)
                if sid:
                    sessions[host] = sid
                result = getattr(msg, "result", None)
                if result and not collected:
                    collected.append(result)
    except Exception as e:  # noqa: BLE001
        log.exception("query failed for %s seq=%s", host, seq)
        return f"[brain error: {e}]"
    text = "\n".join(collected).strip()
    return text or "(no response generated)"


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
            answer = await run_prompt(host, seq, prompt, sessions)
            write_response(host, seq, answer)
            f.unlink(missing_ok=True)  # mark consumed (clears 'pending' count)
            log.info("answered host=%s seq=%s (%d chars)", host, seq, len(answer))
        await asyncio.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
