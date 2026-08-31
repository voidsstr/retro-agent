"""`publish_all.py` must be able to publish. It could not, for any box, ever.

THE DEFECT, found 2026-08-31. `gamegate.get_profile()` wraps its fetch in
`asyncio.run()`. `publish_all.main_async()` called it from **inside a running
event loop**, where `asyncio.run()` can only ever raise:

    RuntimeError: asyncio.run() cannot be called from a running event loop

Measured on .243: the call raised from async context while the identical call
from a sync context returned profile `d931bfe6c33fae5e` -- the box was
answering perfectly the whole time.

WHY IT SURVIVED. The caller caught bare `Exception`, slept 15s, retried six
times, and then printed

    192.168.1.243    UNREACHABLE - not published (its file is left alone)

so a **programming error wore the costume of a powered-off machine** -- on a
fleet that CLAUDE.md says is deliberately powered off most of the time, and
where "an empty sweep is NOT an outage" is a standing rule. The one condition
nobody investigates is exactly the one it impersonated. It also burned 90
seconds per box doing it.

WHAT IT COST. The published `_gamegate/<profile_hash>.txt` files are what the
agent **prefers** over its compiled-in rules, and they are the only route by
which the LLM's adjudication of the marginal band reaches a machine. With the
publisher dead, every box silently fell back to the deterministic header rules
and the published files went stale -- `.243` had no file at all, so a 165 MHz
Pentium was running GAMESYNC against nothing. Nothing *broke*, which is why it
went unnoticed: the fallback is the safe one.

This file therefore asserts BOTH halves, because fixing only the call site
would leave the trap armed for the next caller:
  * an async-safe entry point exists and is what publish_all uses; and
  * the sync wrapper refuses loudly inside a loop instead of raising a generic
    RuntimeError that a broad `except` can mistake for a network fault; and
  * the retry loop no longer treats a non-network exception as unreachability.

No network, no fleet, no share -- pure source and signature checks.
"""
import ast
import asyncio
import inspect
import os
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "scripts"))

GG = os.path.join(REPO, "scripts", "gamegate", "gamegate.py")
PUB = os.path.join(REPO, "scripts", "gamegate", "publish_all.py")


def _src(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_an_async_entry_point_exists():
    from gamegate.gamegate import get_profile_async
    assert inspect.iscoroutinefunction(get_profile_async), (
        "get_profile_async must be a coroutine function -- it is the entry "
        "point every async caller needs so it never reaches asyncio.run()")


def test_publish_all_uses_the_async_entry_point():
    """The actual fix. publish_all runs inside an event loop."""
    src = _src(PUB)
    assert "get_profile_async" in src, (
        "publish_all.py no longer uses get_profile_async. It calls from inside "
        "main_async's running loop, so the sync get_profile() can only raise "
        "and every box will report UNREACHABLE while answering fine.")
    tree = ast.parse(src)
    bare = [n for n in ast.walk(tree)
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
            and n.func.id == "get_profile"]
    assert not bare, (
        "publish_all.py calls the SYNC get_profile() somewhere -- that is the "
        "original defect")


@pytest.mark.parametrize("_i", [0])
def test_the_sync_wrapper_refuses_inside_a_loop_with_a_named_reason(_i):
    """It must fail LOUDLY and specifically, not generically.

    A bare RuntimeError is what let a broad `except Exception` downgrade this
    to "the box is switched off". The message has to name the fix.
    """
    from gamegate.gamegate import get_profile

    async def call_it():
        with pytest.raises(RuntimeError) as ei:
            get_profile("192.0.2.1", None)      # TEST-NET-1, never routed
        return str(ei.value)

    msg = asyncio.run(call_it())
    assert "get_profile_async" in msg, (
        "the refusal must name the async entry point, or the next caller "
        "re-derives the bug from a stack trace")
    assert "event loop" in msg.lower()


def test_the_retry_loop_does_not_treat_a_bug_as_unreachability():
    """Only network-shaped failures may be retried and called UNREACHABLE.

    Retrying a RuntimeError six times over 90 seconds and then printing
    UNREACHABLE is precisely how this hid: the fleet really is off most of the
    time, so that message is never surprising.
    """
    src = _src(PUB)
    tree = ast.parse(src)
    handlers = [h for n in ast.walk(tree) if isinstance(n, ast.Try)
                for h in n.handlers]
    retrying = []
    for h in handlers:
        body = ast.dump(h)
        if "sleep" not in body:
            continue
        if h.type is None:
            retrying.append("bare except")
        elif isinstance(h.type, ast.Name) and h.type.id == "Exception":
            retrying.append("except Exception")
    assert not retrying, (
        "publish_all.py retries-and-sleeps inside a %s. That catches "
        "RuntimeError, TypeError and NameError as though they were a "
        "powered-off machine. Catch OSError/TimeoutError for retries and let "
        "everything else surface as the bug it is." % ", ".join(retrying))


def test_a_non_network_failure_is_reported_as_an_error_not_as_unreachable():
    """The two states must not print the same. Three states, never two."""
    src = _src(PUB)
    assert "not a " in src and "reachability" in src.lower(), (
        "publish_all.py no longer distinguishes a bug from an unreachable box "
        "in its output -- that distinction is the whole lesson here")
