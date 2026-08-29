"""The chat daemon's shared send connection must only be touched under its lock.

Each `HostState` has one `send_conn` shared by several coroutines: the response
forwarder, the status forwarder, the deferred-task drainer and the connect
banner. `state.lock` exists to serialise them. It was only ever half applied,
and the gaps produced errors that looked like flaky hardware:

    [192.168.1.143] STATUS_SET failed: 0 bytes read on a total of 4 expected
    [192.168.1.143] send attempt 1/3 failed: readexactly() called while
                    another coroutine is already waiting for incoming data
    [192.168.1.143] STATUS_SET failed: Connection lost

Two distinct gaps, both real:

  * **`ensure_send_conn()` outside the lock.** It check-and-creates
    `state.send_conn` and reads the agent's greeting, so unlocked it can
    replace the connection another coroutine is mid-read on. The resulting
    failure fires *after* the `LOG_APPEND` has gone out, so the retry loop
    delivers the user's answer a second time.
  * **Tearing the connection down outside the lock.** The error handlers did
    `close()` then `send_conn = None` unlocked — destroying a connection
    another coroutine was actively reading from.

Rather than test the interleavings (which are timing-dependent and would be
flaky), this asserts the *invariant that prevents them*: every use of
`state.send_conn` sits inside an open `async with state.lock`.

The daemon is server-side and lives in the sibling `nsc-assistant` repo.

Run: pytest tests/python/test_chat_daemon_conn_safety.py
"""

from pathlib import Path

import pytest

_DAEMON = Path.home() / "development" / "nsc-assistant" / \
    "agent" / "tools" / "retro_chat_daemon.py"

needs_daemon = pytest.mark.skipif(
    not _DAEMON.is_file(),
    reason=f"chat daemon not checked out at {_DAEMON}")


def _unlocked_uses():
    """Lines touching state.send_conn with no `async with state.lock` open.

    Indentation-based, which is enough here: the daemon is ordinary async
    Python and the lock is always taken with a plain `async with`.
    """
    lines = _DAEMON.read_text().splitlines()
    depth = None
    unlocked = []
    for n, line in enumerate(lines, 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "async with state.lock" in stripped:
            depth = len(line) - len(line.lstrip())
            continue
        if depth is not None:
            indent = len(line) - len(line.lstrip())
            if indent <= depth:
                depth = None
        touches = ("state.send_conn" in stripped
                   or "ensure_send_conn(" in stripped)
        if touches and "async def" not in stripped and depth is None:
            unlocked.append((n, stripped))
    return unlocked


@needs_daemon
def test_every_send_conn_use_is_under_the_lock():
    """The one exception is the last-resort `state.send_conn = None` in the
    handler that runs when taking the lock ITSELF failed — the connection has
    to be dropped either way, or a dead one is reused forever."""
    offenders = [
        (n, text) for n, text in _unlocked_uses()
        if text != "state.send_conn = None"
    ]
    assert not offenders, (
        "state.send_conn touched outside state.lock:\n" +
        "\n".join(f"  line {n}: {t}" for n, t in offenders))


@needs_daemon
def test_the_only_unlocked_uses_are_the_lock_failure_fallbacks():
    """Pin the exemption so it cannot quietly widen: every remaining unlocked
    line must be the bare reset, and each must sit in an `except` block."""
    src = _DAEMON.read_text().splitlines()
    for n, text in _unlocked_uses():
        assert text == "state.send_conn = None", f"line {n}: {text}"
        preceding = "\n".join(src[max(0, n - 4):n - 1])
        assert "except" in preceding, (
            f"line {n} resets the connection unlocked outside an except block")


@needs_daemon
def test_ensure_send_conn_is_never_called_unlocked():
    """This is the one that duplicated a user's answer: the failure it causes
    fires after the LOG_APPEND has already gone out, so the retry re-sends."""
    bad = [(n, t) for n, t in _unlocked_uses() if "ensure_send_conn(" in t]
    assert not bad, f"ensure_send_conn() called unlocked: {bad}"


@needs_daemon
def test_teardown_closes_and_nulls_together():
    """A handler that closes without nulling leaves a dead connection in
    place; one that nulls without closing leaks the socket."""
    src = _DAEMON.read_text()
    assert src.count("await state.send_conn.close()") == \
        src.count("if state.send_conn:"), \
        "close() and the None-guard have drifted apart"


# --- the fleet is powered on demand: zero agents is not an error ------------

@needs_daemon
def test_no_agents_found_does_not_exit():
    """The retro fleet is deliberately powered on demand, so discovering zero
    agents is the normal resting state. Exiting made `daemon: NOT RUNNING`
    the steady state — so the status check could not tell "fleet is off" from
    "the daemon is broken" — and with Restart=always it became a permanent
    rescan loop over all 254 addresses."""
    src = _DAEMON.read_text()
    start = src.index("discovered = await discover_agents()")
    window = src[start:start + 1200]
    assert "no v1.4.0 agents found, exiting" not in window
    assert "return\n" not in window.split("for ip in discovered:")[0], \
        "main_async still bails out when the fleet is powered down"


@needs_daemon
def test_rediscover_still_adds_hosts_that_appear_later():
    """Staying up with zero hosts is only safe because rediscover() claims
    machines as they boot."""
    src = _DAEMON.read_text()
    start = src.index("async def rediscover(")
    body = src[start:start + 1500]
    assert "new agent online" in body
    assert "asyncio.create_task(serve_host(" in body
