"""Reaping an offline box must not take the whole chat daemon down with it.

This encodes the root cause of a user-visible outage. Someone typed a message
on a retro box and got no reply. The proximate reason was that the box was
never claimed — and the reason for *that* was this:

    2026-08-28 22:26:02,785 [INFO] reaping offline agent 192.168.1.171 ...
    2026-08-28 22:26:03      retro-chat-daemon.service: Failed, status=1

Under a second apart, and it had happened all day. `rediscover()` cancels a
host's `serve_host` task when the box goes offline; `serve_host` correctly
cleans up and re-raises `CancelledError`, which is proper asyncio practice.
But `main_async` awaited `asyncio.gather(*tasks)` **without**
`return_exceptions=True`, so that cancellation propagated straight out of
gather and killed the process. Every reaped machine took the entire chat
service down, and because the unit restarts with `RestartSec=5min`, chat
stayed dead for five minutes each time — which is how a box sat unclaimed for
two hours while its user typed into it.

The daemon lives in the sibling `nsc-assistant` repo (it is server-side and
never ships to the fleet), so this asserts against that source plus the
asyncio semantics the fix depends on.

Run: pytest tests/python/test_chat_daemon_reap_survival.py
"""

import asyncio
import os
from pathlib import Path

import pytest

_DAEMON = Path.home() / "development" / "nsc-assistant" / \
    "agent" / "tools" / "retro_chat_daemon.py"

needs_daemon = pytest.mark.skipif(
    not _DAEMON.is_file(),
    reason=f"chat daemon not checked out at {_DAEMON}")


# --- the semantics the fix relies on ----------------------------------------

async def _gather_survives_a_cancelled_child(return_exceptions):
    """Model main_async: one task gets cancelled (a reap), another runs on."""
    reaped = asyncio.create_task(asyncio.sleep(3600))
    other = asyncio.create_task(asyncio.sleep(3600))

    async def reaper():
        await asyncio.sleep(0.01)
        reaped.cancel()          # exactly what rediscover() does

    asyncio.create_task(reaper())
    try:
        await asyncio.wait_for(
            asyncio.gather(reaped, other, return_exceptions=return_exceptions),
            timeout=0.3)
        return "returned"
    except asyncio.CancelledError:
        return "died"            # the cancellation escaped gather
    except asyncio.TimeoutError:
        return "survived"        # still serving the other host
    finally:
        other.cancel()


def test_a_bare_gather_dies_when_a_child_is_cancelled():
    """The old behaviour, pinned so the fix has something to be measured
    against — this is what took the daemon down on every reap."""
    assert asyncio.run(_gather_survives_a_cancelled_child(False)) == "died"


def test_gather_with_return_exceptions_survives_a_reap():
    assert asyncio.run(_gather_survives_a_cancelled_child(True)) == "survived"


# --- the daemon actually uses it --------------------------------------------

def _main_async_body():
    """Just main_async, so the assertion cannot be satisfied (or broken) by
    an unrelated gather elsewhere in the file."""
    src = _DAEMON.read_text()
    start = src.index("async def main_async(")
    return src[start:]


@needs_daemon
def test_main_async_gathers_with_return_exceptions():
    body = _main_async_body()
    assert "asyncio.gather(*tasks, return_exceptions=True)" in body, \
        "a reaped host will kill the whole chat daemon again"
    assert "await asyncio.gather(*tasks)\n" not in body


@needs_daemon
def test_the_discovery_gather_is_left_alone():
    """discover_agents() gathers 254 short-lived probes that each swallow
    their own exceptions and are never cancelled individually — it does not
    need return_exceptions, and changing it would hide a real failure."""
    src = _DAEMON.read_text()
    start = src.index("async def discover_agents(")
    body = src[start:src.index("\n\n\n", start)]
    assert "await asyncio.gather(*tasks)" in body


@needs_daemon
def test_serve_host_still_cleans_up_and_reraises_on_cancel():
    """The fix belongs in the gather, NOT in swallowing the cancellation:
    serve_host must still close its connections and re-raise, or a reaped
    host leaks two TCP connections to a box that is already gone."""
    src = _DAEMON.read_text()
    start = src.index("except asyncio.CancelledError:")
    block = src[start:start + 400]
    assert "close_all()" in block
    assert "raise" in block


@needs_daemon
def test_a_task_failing_for_any_other_reason_is_still_reported():
    """return_exceptions=True hides real crashes unless they are logged.
    A silently swallowed exception here would be worse than the original bug,
    because the daemon would stay up doing nothing."""
    src = _DAEMON.read_text()
    tail = src[src.index("asyncio.gather(*tasks, return_exceptions=True)"):]
    assert "logger.error" in tail[:900]
    assert "CancelledError" in tail[:900], \
        "a cancelled task is normal and must not be logged as an error"
