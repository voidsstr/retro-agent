"""A machine's follow-up chat prompt must resume its conversation.

Real failure (2026-09-25, .184): a user asked the chat about an EPoX BIOS boot
floppy, got an answer, then typed "it still is hung at programming flash
memory" twice. Both follow-ups failed on EVERY account:

  * a user-wide systemd drop-in put the claude-pool shim first on PATH, so the
    brain's `claude` was the shim, which re-picks a profile on every call. The
    first answer was saved under profile-3; the follow-up ran under profile-2
    and `--resume` said "No conversation found with session ID";
  * failover set HOME to a pool profile, where the shim finds no pool at all
    ("No authenticated profiles", exit 70).

Invariants:
  * _find_cli() never returns the pool shim, even when it is first on PATH;
  * portable_resume() copies a transcript into the account that will resume it,
    and returns None (fresh conversation) rather than a sid that cannot resume;
  * SessionStore persists across a restart and follows the host across accounts.

Run: pytest tests/python/test_chat_brain_session_resume.py
"""

import json
import logging
import os
import shutil
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
_BRAIN = _REPO / "scripts" / "retro_chat_brain.py"


def _ns(pool_root):
    """Exec just the helpers; the brain module needs the Agent SDK to import."""
    src = _BRAIN.read_text()
    ns = {"os": os, "json": json, "shutil": shutil, "Path": Path,
          "POOL_ROOT": Path(pool_root), "log": logging.getLogger("t")}
    for a, b in (("def _is_pool_shim(", "_CLI_PATH = _find_cli()"),
                 ("SESSIONS_FILE = ", "def options_for(")):
        start = src.index(a)
        exec(compile(src[start:src.index(b, start)], str(_BRAIN), "exec"), ns)
    return ns


def _exe(path, body):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    path.chmod(0o755)
    return path


def test_find_cli_skips_the_pool_shim(tmp_path, monkeypatch):
    pool = tmp_path / "pool"
    shim = _exe(pool / "bin" / "claude",
                "#!/usr/bin/env bash\nexec python3 claude_pool.py exec -- \"$@\"\n")
    real = _exe(tmp_path / "real" / "claude", "\x7fELF fake binary")
    monkeypatch.delenv("CLAUDE_POOL_REAL_CLAUDE", raising=False)
    monkeypatch.setenv("PATH", f"{shim.parent}{os.pathsep}{real.parent}")
    ns = _ns(pool)
    assert ns["_is_pool_shim"](shim)
    assert not ns["_is_pool_shim"](real)
    assert ns["_find_cli"]() == str(real)          # old code returned the shim


def test_resume_follows_the_transcript_across_accounts(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "default"))
    ns = _ns(tmp_path / "pool")
    p2, p3 = str(tmp_path / "p2"), str(tmp_path / "p3")
    sid = "c9acffdb-aded-4291-9968-f89032fc70d0"
    src = Path(p3) / ".claude/projects/-home-x-retro-agent" / f"{sid}.jsonl"
    src.parent.mkdir(parents=True)
    src.write_text('{"conversation": "epox boot floppy"}\n')

    # Written under profile-3, resumed under profile-2: the transcript moves.
    assert ns["portable_resume"](sid, p2, [None, p2, p3]) == sid
    dst = Path(p2) / ".claude/projects/-home-x-retro-agent" / f"{sid}.jsonl"
    assert dst.read_text() == src.read_text()
    # ...and under the default login (HOME).
    assert ns["portable_resume"](sid, None, [None, p2, p3]) == sid
    # A transcript that exists nowhere: start fresh, never a dead --resume.
    assert ns["portable_resume"]("0000-missing", p2, [None, p2, p3]) is None
    assert ns["portable_resume"](None, p2, [None, p2, p3]) is None


def test_sessions_survive_a_restart_and_follow_the_host(tmp_path):
    ns = _ns(tmp_path / "pool")
    path = tmp_path / "sessions.json"
    s = ns["SessionStore"](path)
    s[("192.168.1.184", "/p3")] = "sid-a"
    s2 = ns["SessionStore"](path)                  # brain restarted
    assert s2.get(("192.168.1.184", None)) == "sid-a"
    assert s2.get(("192.168.1.184", "/p2")) == "sid-a"
    assert s2.get(("192.168.1.171", None)) is None
