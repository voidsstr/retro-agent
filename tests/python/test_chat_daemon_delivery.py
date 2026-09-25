"""The chat daemon's SEND side: answer text and status reach the right box, in
order, once — and one dead box never holds up another.

Every test here drives the real daemon (nsc-assistant/agent/tools/
retro_chat_daemon.py) against fake agents speaking the real framed protocol on
localhost (tests/python/chat_fake_agent.py). What they pin, each a failure seen
in the 2026-09 journals:

  * one dead box stalled every chat: _process_outbox_files sent serially across
    hosts with retries and sleeps, so a powered-off box delayed every other
    box's answer. Now each host has its own sender.
  * a box that was briefly away had its answer moved to failed/ after three
    tries (and a host the daemon had reaped had its files DELETED as "unknown
    host"). Now text is held, in order, until the box answers again (age-capped).
  * a LOG_APPEND that timed out had usually been appended; the retry resent
    everything and the user saw the answer twice. LOG_APPEND2 (agent 1.85.0+)
    carries a chunk id the agent de-duplicates; on older agents a sent-but-
    unanswered append is not resent.
  * "thinking..." went missing: STATUS_SET met a connection the agent had
    already closed (idle 120 s) and was never retried. `connected` now sees
    the agent's FIN, and a status is retried once after a reset.
  * status flooding: a status file every ~30 characters of thinking, all
    forwarded. Now only the newest per host.

Run: NSC_ASSISTANT_DIR=<nsc worktree> pytest tests/python/test_chat_daemon_delivery.py
"""

import asyncio
import json
import os
import time

import pytest

from chat_fake_agent import (DAEMON, FakeAgent, load_daemon, point_at,
                             wait_until, write_outbox, write_status)

pytestmark = pytest.mark.skipif(not DAEMON.is_file(),
                                reason=f"chat daemon not checked out at {DAEMON}")

A, B = "192.168.1.201", "192.168.1.202"


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 30))


async def _setup(tmp_path, **agents_kw):
    mod = load_daemon(tmp_path)
    agents = {}
    for ip, kw in agents_kw.items():
        agents[ip] = await FakeAgent(**kw).start()
    point_at(mod, agents)
    return mod, agents


def _start_sender(mod, ip):
    st = mod.HostState(ip)
    mod.hosts[ip] = st
    st.sender = asyncio.create_task(mod.host_sender(st))
    return st


async def _teardown(mod, agents):
    for st in list(mod.hosts.values()):
        st.closing = True
        st.wake.set()
        st.reconnected.set()
        if st.sender:
            st.sender.cancel()
    await asyncio.gather(*(st.sender for st in mod.hosts.values() if st.sender),
                         return_exceptions=True)
    await asyncio.gather(*(st.close_all() for st in mod.hosts.values()))
    for a in agents.values():
        await a.stop()


def _appends(agent):
    return [c for c in agent.commands if c.startswith("LOG_APPEND")]


# --- independence and ordering ---------------------------------------------

def test_one_dead_box_does_not_stall_another(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {}, B: {}})
        await agents[B].set_up(False)                  # B is powered off
        _start_sender(mod, A)
        _start_sender(mod, B)
        for i in range(3):
            write_outbox(mod, B, 1, f"b{i} ")
        write_outbox(mod, A, 1, "hello from A", stream=False)
        t0 = time.monotonic()
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].log == "hello from A\n", 3)
        took = time.monotonic() - t0
        # B's text is held on disk, not failed, not deleted
        held = sorted(json.loads(p.read_text())["chunks"][0]
                      for p in mod.OUTBOX.glob("*.json"))
        assert held == ["b0 ", "b1 ", "b2 "]
        assert not list(mod.FAILED.glob("*.json"))
        await _teardown(mod, agents)
        return took

    assert run(go()) < 1.5, "a powered-off box delayed another box's answer"


def test_held_text_arrives_in_order_and_merged_when_the_box_returns(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{B: {"append2": True}})
        await agents[B].set_up(False)
        st = _start_sender(mod, B)
        lines = [f"line {i}\n" for i in range(8)]
        for i, line in enumerate(lines):
            write_outbox(mod, B, 7, line)
        write_outbox(mod, B, 7, "done", stream=False)
        mod.dispatch_queues()
        await asyncio.sleep(0.4)                       # the sender tries and holds
        assert agents[B].log == ""
        await agents[B].set_up(True)
        st.reconnected.set()
        assert await wait_until(lambda: agents[B].log == "".join(lines) + "done\n", 5)
        assert len(_appends(agents[B])) == 1, \
            "nine pending chunks should go out as ONE merged append"
        assert not list(mod.OUTBOX.glob("*.json"))
        await _teardown(mod, agents)

    run(go())


def test_merging_respects_max_chunk_and_resumes_mid_file(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {"append2": True}})
        mod.MAX_CHUNK = 10
        write_outbox(mod, A, 1, "abcdefghijklmnop")          # 16 chars
        write_outbox(mod, A, 2, "QRSTUVWXYZ", stream=False)  # 10 + newline
        _start_sender(mod, A)
        mod.dispatch_queues()
        want = "abcdefghijklmnopQRSTUVWXYZ\n"
        assert await wait_until(lambda: agents[A].log == want, 5)
        sizes = [len(c.split(" ", 2)[2]) for c in _appends(agents[A])]
        assert all(n <= 10 for n in sizes) and sum(sizes) == len(want)
        await _teardown(mod, agents)

    run(go())


def test_order_follows_seq_not_filename(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {}})
        # a later prompt's file whose NAME sorts first must still go last
        write_outbox(mod, A, 1790000002, "second", stream=False, name="0-late.json")
        write_outbox(mod, A, 1790000001, "first", stream=False, name="9-early.json")
        _start_sender(mod, A)
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].log == "first\nsecond\n", 3)
        await _teardown(mod, agents)

    run(go())


# --- at-most-once / exactly-once --------------------------------------------

def test_append2_resend_after_a_timeout_is_deduplicated(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {"append2": True}})
        agents[A].delay["LOG_APPEND2"] = 1.0           # > CMD_TIMEOUT_S: reply is late
        st = _start_sender(mod, A)
        write_outbox(mod, A, 1, "exactly once", stream=False)
        mod.dispatch_queues()
        assert await wait_until(lambda: len(_appends(agents[A])) >= 2, 5)
        agents[A].delay.clear()                        # the box catches up
        st.reconnected.set()
        assert await wait_until(lambda: not list(mod.OUTBOX.glob("*.json")), 5)
        ids = {c.split(" ")[1] for c in _appends(agents[A])}
        assert agents[A].log == "exactly once\n", agents[A].log
        assert len(ids) == 1, "a resend must carry the SAME chunk id"
        await _teardown(mod, agents)

    run(go())


def test_plain_log_append_that_timed_out_after_sending_is_not_resent(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {"append2": False}})
        agents[A].delay["LOG_APPEND"] = 1.0
        _start_sender(mod, A)
        write_outbox(mod, A, 1, "no duplicates", stream=False)
        mod.dispatch_queues()
        assert await wait_until(lambda: not list(mod.OUTBOX.glob("*.json")), 5)
        await asyncio.sleep(0.3)
        plain = [c for c in agents[A].commands if c.startswith("LOG_APPEND ")]
        assert len(plain) == 1, plain
        assert agents[A].log == "no duplicates\n"
        await _teardown(mod, agents)

    run(go())


def test_old_agent_falls_back_to_log_append_once_per_connection(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {"append2": False}})
        _start_sender(mod, A)
        write_outbox(mod, A, 1, "one", stream=False)
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].log == "one\n", 3)
        write_outbox(mod, A, 2, "two", stream=False)
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].log == "one\ntwo\n", 3)
        names = [c.split(" ", 1)[0] for c in _appends(agents[A])]
        assert names == ["LOG_APPEND2", "LOG_APPEND", "LOG_APPEND"], names
        await _teardown(mod, agents)

    run(go())


def test_non_ascii_text_is_delivered_not_stuck(tmp_path):
    """The command is ASCII-encoded before sending; one smart quote used to
    raise every time and hold the host's queue forever."""
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {}})
        _start_sender(mod, A)
        write_outbox(mod, A, 1, "café — ok", stream=False)
        write_outbox(mod, A, 2, "next", stream=False)
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].log.endswith("next\n"), 3)
        assert agents[A].log.startswith("caf? ? ok\n")
        await _teardown(mod, agents)

    run(go())


# --- status -----------------------------------------------------------------

def test_only_the_newest_status_is_forwarded(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {}})
        for i in range(6):
            write_status(mod, A, f"thinking: step {i}", age_s=(6 - i) * 0.5)
        _start_sender(mod, A)
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].statuses, 3)
        await asyncio.sleep(0.2)
        assert agents[A].statuses == ["thinking: step 5"]
        assert not list(mod.STATUS_OUTBOX.glob("*.json"))
        await _teardown(mod, agents)

    run(go())


def test_a_stale_status_is_never_forwarded(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {}})
        write_status(mod, A, "thinking: ages ago", age_s=mod.STATUS_MAX_AGE_S + 5)
        _start_sender(mod, A)
        mod.dispatch_queues()
        await asyncio.sleep(0.3)
        assert agents[A].statuses == []
        assert not list(mod.STATUS_OUTBOX.glob("*.json"))
        await _teardown(mod, agents)

    run(go())


def test_status_after_the_agent_dropped_the_idle_connection(tmp_path):
    """The agent closes a client silent for 120 s (NT). The next STATUS_SET
    used to fail with "Connection lost" and "thinking..." was lost."""
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {"idle_drop_s": 0.3}})
        st = _start_sender(mod, A)
        write_status(mod, A, "first")
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].statuses == ["first"], 3)
        assert await wait_until(
            lambda: any(c["end"] == "agent-dropped-idle" for c in agents[A].conns), 3)
        await asyncio.sleep(0.1)
        assert not st.send_conn.connected, "the agent's FIN must make the connection stale"
        write_status(mod, A, "thinking...")
        mod.dispatch_queues()
        assert await wait_until(lambda: agents[A].statuses == ["first", "thinking..."], 3)
        await _teardown(mod, agents)

    run(go())


def test_status_is_retried_once_when_the_first_try_never_left(tmp_path):
    async def go():
        mod, agents = await _setup(tmp_path, **{A: {}})
        st = _start_sender(mod, A)
        real = mod.HostState.ensure_send_conn
        calls = {"n": 0}

        async def flaky(self):
            calls["n"] += 1
            if calls["n"] == 1:
                raise ConnectionRefusedError("agent restarting")
            return await real(self)

        mod.HostState.ensure_send_conn = flaky
        write_status(mod, A, "thinking...")
        await mod.deliver_status(st)
        assert agents[A].statuses == ["thinking..."]
        await _teardown(mod, agents)

    run(go())


# --- files for hosts that are away ------------------------------------------

def test_outbox_for_an_unclaimed_host_is_kept_then_expired(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        p = write_outbox(mod, "192.168.1.250", 1, "for later", stream=False)
        mod.dispatch_queues()
        assert p.exists(), "text for a box that is only away must not be deleted"
        t = time.time() - mod.OUTBOX_MAX_AGE_S - 10
        os.utime(p, (t, t))
        mod.dispatch_queues()
        assert not p.exists()
        assert (mod.FAILED / p.name).exists(), "expired text goes to failed/, visibly"

    run(go())


def test_unparseable_outbox_waits_out_the_grace_then_fails_visibly(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        p = mod.OUTBOX / "half-written.json"
        p.write_text('{"host": "192.168')
        mod.dispatch_queues()
        assert p.exists()
        t = time.time() - mod.OUTBOX_PARTIAL_GRACE_SEC - 1
        os.utime(p, (t, t))
        mod.dispatch_queues()
        assert (mod.FAILED / p.name).exists()

    run(go())


def test_commit_trims_a_partly_sent_file_and_the_cache_sees_it(tmp_path):
    """A piece that ends inside a file rewrites it with the remainder (and
    keeps its mtime for the age cap). The parse cache must not hand back the
    old text even if the size happens to match."""
    mod = load_daemon(tmp_path)
    mod.MAX_CHUNK = 5
    p = write_outbox(mod, A, 3, "0123456789")
    segs = mod._host_segments(A)
    piece = mod.build_piece(segs)
    assert piece.text == "01234"
    before = p.stat().st_mtime
    mod.commit_piece(A, piece)
    segs = mod._host_segments(A)
    assert [s[2] for s in segs] == ["56789"]
    assert abs(p.stat().st_mtime - before) < 1.0
    hist = (mod.HISTORY / A / "response-0003.txt").read_text()
    assert hist == "01234"


def test_a_half_written_file_holds_its_host_instead_of_being_skipped(tmp_path):
    """Skipping it would deliver the lines written AFTER it first."""
    mod = load_daemon(tmp_path)
    (mod.OUTBOX / f"{A}-1-000001.json").write_text('{"host": "192.168')
    write_outbox(mod, A, 1, "later line\n", name=f"{A}-1-000002.json")
    assert mod._host_segments(A) == []
    (mod.OUTBOX / f"{A}-1-000001.json").write_text(
        json.dumps({"host": A, "seq": 1, "chunks": ["first line\n"], "stream": True}))
    assert [s[2] for s in mod._host_segments(A)] == ["first line\n", "later line\n"]
