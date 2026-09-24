"""The chat brain must stream its prompt, and must not blame accounts for a crash.

This encodes a real, user-visible failure (2026-09-23). A user on .171 asked
the chat to install a video driver and got back

    [No Claude account on the brain is usable right now (can_use_tool
    callback requires streaming mode. Please provid) ...]

Every account was fine. The prompt had been taken by a stale brain on the old
fleet host (whitebeast), running an older claude-agent-sdk. Those releases
refuse `can_use_tool` unless the prompt is an AsyncIterable, and they raise
before the CLI even starts. The brain passed a plain string. It then counted
that error as "account unusable" on every account in turn and blamed the
accounts.

Two invariants:
  * query() is handed prompt_stream(prompt), never the bare string -- that is
    valid on every SDK version (the old one was measured to raise with a
    string and to accept the stream);
  * the all-accounts-failed message only says "no account" when the error
    actually looks like an auth / limit problem.

Run: pytest tests/python/test_chat_brain_prompt_stream.py
"""

import asyncio
import re
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
_BRAIN = _REPO / "scripts" / "retro_chat_brain.py"


def _helpers():
    """Exec just the helpers; the brain module needs the Agent SDK to import."""
    src = _BRAIN.read_text()
    start = src.index("async def prompt_stream(")
    end = src.index("async def run_prompt(", start)
    ns = {"re": re}
    exec(compile(src[start:end], str(_BRAIN), "exec"), ns)
    return ns


def test_query_is_given_a_stream_not_a_string():
    src = _BRAIN.read_text()
    assert "query(prompt=prompt_stream(prompt)" in src
    assert "query(prompt=prompt," not in src, \
        "a bare-string prompt is back; older SDKs reject it with can_use_tool set"


def test_prompt_stream_yields_one_user_message():
    ns = _helpers()

    async def collect():
        return [m async for m in ns["prompt_stream"]("hello")]

    msgs = asyncio.run(collect())
    assert msgs == [{"type": "user",
                     "message": {"role": "user", "content": "hello"}}]


def test_sdk_error_is_not_reported_as_an_account_problem():
    ns = _helpers()
    msg = ns["all_failed_message"](
        "can_use_tool callback requires streaming mode. Please provide prompt "
        "as an AsyncIterable instead of a string.")
    assert "No Claude account" not in msg          # the old, misleading text
    assert "not an account problem" in msg
    assert "requires streaming mode" in msg        # the real error is shown


def test_auth_errors_still_point_at_the_accounts():
    ns = _helpers()
    for err in ("Not logged in - Please run /login",
                "Failed to authenticate: OAuth session expired",
                "usage limit reached",
                None):
        assert "No Claude account" in ns["all_failed_message"](err), err
