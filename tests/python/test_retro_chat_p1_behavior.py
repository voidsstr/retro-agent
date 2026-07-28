"""Regression: retro_chat.exe must stay usable on Pentium-1 class fleet boxes
(retro-chat-v0.14.0, 2026-07-28, prompted by the Deskpro 2000 / .243).

Two hardware-verified behaviors, both encoded as source invariants on
agent/tools/retro_chat.c:

1. CPU while "Working": the spinner used to call refresh_input() every 150ms —
   a full erase (FillConsoleOutputCharacter per row) + redraw of the whole
   input area, which visibly dragged a Pentium 1. Fixed: >=500ms tick and the
   tick rewrites ONLY the single spinner cell via WriteConsoleOutputCharacterA
   (no erase, no full redraw), at below-normal thread priority.

2. Startup crash when the agent isn't up yet: the chat used to fprintf an
   error and exit(1) when agent_connect() failed (window vanished — looked
   like a crash, and at boot the chat can win the race against the agent's
   Run-key process). Fixed: agent_connect_wait() announces "Waiting for the
   retro agent to start..." and retries until the agent is listening; all
   three startup connections (main, wait, status) go through it.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "tools" / "retro_chat.c"


def _text():
    assert SRC.is_file(), "agent/tools/retro_chat.c missing"
    return SRC.read_text()


def _fn_body(text, name):
    """Extract a top-level function body by name (brace matching)."""
    m = re.search(r"\b%s\s*\([^;{)]*\)\s*\n?\{" % re.escape(name), text)
    assert m, "function %s not found in retro_chat.c" % name
    depth, i = 0, text.index("{", m.start())
    start = i
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise AssertionError("unbalanced braces in %s" % name)


def test_spinner_tick_is_slow_enough_for_p1():
    """The old 150ms tick redrew the console ~7x/sec on a Pentium 1."""
    text = _text()
    m = re.search(r"#define\s+SPINNER_TICK_MS\s+(\d+)", text)
    assert m, "SPINNER_TICK_MS define missing"
    assert int(m.group(1)) >= 500, (
        "SPINNER_TICK_MS must be >= 500 (P1 CPU); got %s" % m.group(1))


def test_spinner_tick_updates_single_cell_not_full_redraw():
    """The spin loop must write one cell, never erase+redraw the input area."""
    body = _fn_body(_text(), "spinner_thread")
    assert "WriteConsoleOutputCharacterA" in body, (
        "spinner_thread must animate via a single-cell "
        "WriteConsoleOutputCharacterA update")
    assert "refresh_input" not in body, (
        "spinner_thread must NOT call refresh_input() per tick — that is the "
        "full erase+redraw that dragged Pentium-1 boxes (old 0.13.x behavior)")


def test_spinner_thread_runs_below_normal_priority():
    body = _fn_body(_text(), "main")
    assert re.search(
        r"SetThreadPriority\s*\(\s*spin_h\s*,\s*THREAD_PRIORITY_BELOW_NORMAL",
        body), "spinner thread must be set to below-normal priority"


def test_startup_waits_for_agent_instead_of_exiting():
    """All three startup connections must use agent_connect_wait()."""
    text = _text()
    assert "Waiting for the retro agent to start" in text, (
        "the wait announcement message must be present")
    wait_body = _fn_body(text, "agent_connect_wait")
    assert "Sleep(CONNECT_RETRY_MS)" in wait_body
    main_body = _fn_body(text, "main")
    assert len(re.findall(r"agent_connect_wait\s*\(\s*\)", main_body)) >= 3, (
        "main must open all three connections (main/wait/status) via "
        "agent_connect_wait()")
    assert not re.search(r"=\s*agent_connect\s*\(\s*\)", main_body), (
        "main must not call bare agent_connect() — a failure there used to "
        "exit(1) and looked like a crash when the agent wasn't up yet")


def test_reconnect_pacing_not_hot():
    """Thread reconnect loops must pace at >= 1s, not the old 300ms churn."""
    text = _text()
    m = re.search(r"#define\s+RECONNECT_SLEEP_MS\s+(\d+)", text)
    assert m, "RECONNECT_SLEEP_MS define missing"
    assert int(m.group(1)) >= 1000
    for fn in ("wait_thread", "status_thread"):
        body = _fn_body(text, fn)
        assert "Sleep(RECONNECT_SLEEP_MS)" in body, (
            "%s must pace reconnects with RECONNECT_SLEEP_MS" % fn)
        assert "Sleep(300)" not in body
