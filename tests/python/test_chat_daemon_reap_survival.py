"""Releasing an offline box must not take the whole chat daemon down with it.

This encodes the root cause of a user-visible outage. Someone typed a message
on a retro box and got no reply. The box was never claimed — because:

    2026-08-28 22:26:02,785 [INFO] reaping offline agent 192.168.1.171 ...
    2026-08-28 22:26:03      retro-chat-daemon.service: Failed, status=1

`rediscover()` cancelled a host's `serve_host` task when the box went offline;
`serve_host` correctly cleaned up and re-raised `CancelledError`. But
`main_async` awaited `asyncio.gather(*tasks)` **without**
`return_exceptions=True`, so that cancellation propagated straight out and
killed the process — every reaped machine took the entire chat service down.

Since the 2026-09 rework the daemon no longer gathers host tasks at all: it
waits for a stop request, and each host task's end is reported by a done
callback (a cancellation is normal, anything else is logged as an error).
Hosts are no longer cancelled by discovery either — a silent host releases
itself. These tests pin the behaviour, not the source text.

Run: NSC_ASSISTANT_DIR=<nsc worktree> pytest tests/python/test_chat_daemon_reap_survival.py
"""

import asyncio
import logging

import pytest

from chat_fake_agent import DAEMON, FakeAgent, load_daemon, point_at, wait_until

needs_daemon = pytest.mark.skipif(not DAEMON.is_file(),
                                  reason=f"chat daemon not checked out at {DAEMON}")

A = "192.168.1.241"


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 30))


# --- the asyncio semantics the original bug came from ----------------------

async def _gather_survives_a_cancelled_child(return_exceptions):
    reaped = asyncio.create_task(asyncio.sleep(3600))
    other = asyncio.create_task(asyncio.sleep(3600))

    async def reaper():
        await asyncio.sleep(0.01)
        reaped.cancel()

    asyncio.create_task(reaper())
    try:
        await asyncio.wait_for(
            asyncio.gather(reaped, other, return_exceptions=return_exceptions),
            timeout=0.3)
        return "returned"
    except asyncio.CancelledError:
        return "died"
    except asyncio.TimeoutError:
        return "survived"
    finally:
        other.cancel()


def test_a_bare_gather_dies_when_a_child_is_cancelled():
    """The old behaviour, pinned so the fix has something to be measured against."""
    assert asyncio.run(_gather_survives_a_cancelled_child(False)) == "died"


def test_gather_with_return_exceptions_survives_a_reap():
    assert asyncio.run(_gather_survives_a_cancelled_child(True)) == "survived"


# --- the daemon --------------------------------------------------------------

def _no_scan(mod, ips=()):
    async def scan(exclude=()):
        await asyncio.sleep(0)
        return [ip for ip in ips if ip not in exclude]
    mod.discover_agents = scan
    mod.listen_announcements = lambda: asyncio.sleep(0)


@needs_daemon
def test_a_cancelled_host_task_does_not_take_the_daemon_down(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent(poll_ms=100).start()
        point_at(mod, {A: agent})
        _no_scan(mod, [A])
        main = asyncio.create_task(mod.main_async(install_signals=False))
        assert await wait_until(lambda: A in mod.hosts, 3)
        mod.hosts[A].task.cancel()
        await asyncio.sleep(0.5)
        assert not main.done(), "a cancelled host task killed the whole daemon"
        mod.request_stop()
        await asyncio.wait_for(main, 5)
        await agent.stop()

    run(go())


@needs_daemon
def test_serve_host_closes_gracefully_and_reraises_when_cancelled(tmp_path):
    """Swallowing the cancellation would leave a zombie loop; not closing
    would leak two connections to a single-threaded agent."""
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent(poll_ms=100).start()
        point_at(mod, {A: agent})
        st = mod.add_host(A)
        assert await wait_until(lambda: len(agent.open_conns()) == 2, 3)
        st.sender.cancel()
        st.task.cancel()
        res = await asyncio.gather(st.task, st.sender, return_exceptions=True)
        assert all(isinstance(r, asyncio.CancelledError) for r in res)
        assert await wait_until(lambda: not agent.open_conns(), 3)
        assert all(c["end"] == "eof" for c in agent.conns), [c["end"] for c in agent.conns]
        await agent.stop()

    run(go())


@needs_daemon
def test_a_task_ending_any_other_way_is_reported(tmp_path, caplog):
    caplog.set_level(logging.ERROR)

    async def go():
        mod = load_daemon(tmp_path)

        async def boom():
            raise ValueError("serve loop crashed")

        crashed = asyncio.create_task(boom(), name="serve-x")
        cancelled = asyncio.create_task(asyncio.sleep(10), name="serve-y")
        for t in (crashed, cancelled):
            t.add_done_callback(mod._report_task_end)
        cancelled.cancel()
        await asyncio.gather(crashed, cancelled, return_exceptions=True)
        await asyncio.sleep(0)

    run(go())
    msgs = [r.getMessage() for r in caplog.records]
    assert any("serve-x" in m and "serve loop crashed" in m for m in msgs), msgs
    assert not any("serve-y" in m for m in msgs), "a cancellation is normal, not an error"


@needs_daemon
def test_a_probe_crash_is_not_hidden_by_discovery(tmp_path):
    """discover_agents() gathers 254 short-lived probes that each swallow
    their own network errors; a bug in the probe itself must still surface
    (and discovery_loop's task then reports it)."""
    mod = load_daemon(tmp_path)

    async def broken_probe(ip, timeout=8.0):
        raise KeyError("bug")

    mod.try_connect = broken_probe
    with pytest.raises(KeyError):
        run(mod.discover_agents())


@needs_daemon
def test_a_box_that_appears_later_is_claimed(tmp_path):
    """Staying up with zero hosts is only safe because discovery claims
    machines as they boot."""
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent(poll_ms=100).start()
        point_at(mod, {A: agent})
        found = []
        _no_scan(mod, found)
        mod.next_discovery_delay = lambda n, e: 0.05
        main = asyncio.create_task(mod.main_async(install_signals=False))
        await asyncio.sleep(0.3)
        assert A not in mod.hosts
        found.append(A)                          # the box is switched on
        assert await wait_until(lambda: A in mod.hosts, 3)
        mod.request_stop()
        await asyncio.wait_for(main, 5)
        await agent.stop()

    run(go())
