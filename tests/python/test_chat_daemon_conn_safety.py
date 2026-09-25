"""The chat daemon's shared send connection must only be touched under its lock.

Each `HostState` has one `send_conn` shared by several coroutines: the host's
sender (answer text and status), the deferred-task drainer, the keepalive and
the connect banner. `state.lock` exists to serialise them. It was only ever
half applied, and the gaps produced errors that looked like flaky hardware:

    [192.168.1.143] STATUS_SET failed: 0 bytes read on a total of 4 expected
    [192.168.1.143] send attempt 1/3 failed: readexactly() called while
                    another coroutine is already waiting for incoming data
    [192.168.1.143] STATUS_SET failed: Connection lost

Two distinct gaps, both real (2026-08-28):

  * **`ensure_send_conn()` outside the lock.** It check-and-creates
    `state.send_conn` and reads the agent's greeting, so unlocked it could
    replace the connection another coroutine was mid-read on. The failure
    fired *after* the `LOG_APPEND` had gone out, so the retry delivered the
    user's answer a second time.
  * **Tearing the connection down outside the lock.**

This file used to assert the invariant by indentation-scanning the source.
Since the 2026-09 rework every command on the send connection goes through
ONE function, `_send_cmd`, which refuses to run unless `state.lock` is held —
so the invariant is enforced at run time, and tested here by behaviour:
hammering one host with concurrent answer text, statuses, queued tasks and
keepalives, and checking the box saw every byte exactly once with no
concurrent-reader error.

Run: NSC_ASSISTANT_DIR=<nsc worktree> pytest tests/python/test_chat_daemon_conn_safety.py
"""

import asyncio
import logging
import re

import pytest

from chat_fake_agent import (DAEMON, FakeAgent, load_daemon, point_at,
                             wait_until, write_outbox, write_status)

pytestmark = pytest.mark.skipif(not DAEMON.is_file(),
                                reason=f"chat daemon not checked out at {DAEMON}")

A = "192.168.1.231"


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 30))


def test_send_cmd_refuses_to_run_without_the_lock(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        st = mod.HostState(A)
        with pytest.raises(RuntimeError, match="without state.lock"):
            await mod._send_cmd(st, "PING", 1)

    run(go())


def test_nothing_talks_to_the_send_connection_except_send_cmd():
    """The runtime guard only protects what goes through _send_cmd."""
    src = DAEMON.read_text()
    direct = [m.group(0) for m in re.finditer(r"send_conn\.(send_command|command_text|"
                                               r"command_binary|_send_frame)\(", src)]
    assert not direct, f"send_conn used directly, bypassing the lock guard: {direct}"
    body = src[src.index("async def _send_cmd("):]
    body = body[:body.index("\nasync def ", 10)]
    assert "state.lock.locked()" in body


def test_concurrent_senders_share_one_connection_safely(tmp_path, caplog):
    caplog.set_level(logging.WARNING)

    async def go():
        mod = load_daemon(tmp_path)
        mod.KEEPALIVE_IDLE_S = 0.0                 # keepalive fires every time it can
        agent = await FakeAgent(append2=True, poll_ms=50).start()
        point_at(mod, {A: agent})
        st = mod.add_host(A)
        assert await wait_until(lambda: st.send_conn is not None, 3)
        want = ""
        for i in range(40):
            line = f"line {i:02d}\n"
            want += line
            write_outbox(mod, A, 1, line)
            write_status(mod, A, f"step {i}")
            if i % 10 == 0:
                mod.enqueue_task(A, [f"EXEC task {i}"], f"t{i}")
            mod.dispatch_queues()
            await asyncio.gather(*(mod.keepalive_send_conn(st) for _ in range(3)),
                                 mod.deliver_status(st))
            mod._kick_task_drain(st)
            await asyncio.sleep(0)
        assert await wait_until(lambda: agent.log.endswith(want), 5)
        assert await wait_until(lambda: not mod._pending_tasks(A), 5)
        await mod.shutdown()
        await agent.stop()
        return agent

    agent = run(go())
    assert agent.log.count("line 17\n") == 1, "a line was delivered twice"
    assert sum(1 for c in agent.commands if c.startswith("EXEC task")) == 4
    bad = [r.getMessage() for r in caplog.records
           if "readexactly" in r.getMessage() or "another coroutine" in r.getMessage()]
    assert not bad, bad
