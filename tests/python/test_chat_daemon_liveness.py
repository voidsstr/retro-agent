"""The chat daemon's connection lifecycle: claim fast, never reap a live box,
keep the idle connection alive, and leave with a FIN — never an RST.

Driven against fake agents on localhost (tests/python/chat_fake_agent.py).
Each test pins a failure from the 2026-09 journals:

  * startup blackout: the first discovery ran before the LAN was up and the
    next was 300 s later, so every box sat unclaimed for five minutes after a
    restart. Now: 15 s while nothing is claimed or a scan finished in <1 s,
    60 s otherwise; claimed hosts are not re-probed; and the agents' own UDP
    announcements are heard.
  * the reaper killed live sessions: a host was reaped on discovery misses
    alone (last_seen was never set), cancelling a PROMPT_WAIT that may have
    carried a prompt the agent had already popped. Now a host releases itself
    only after REAP_IDLE_S without ANY reply, and never mid-command.
  * the idle send connection died silently (the agent drops a client silent
    for 120 s): it is PINGed after 60 s idle.
  * sequence numbers restarted at 1 on every re-add, overwriting history.
  * try_connect leaked its connection when a probe raised.
  * a systemd restart killed sockets abruptly; an RST can crash Win98 Winsock.

Run: NSC_ASSISTANT_DIR=<nsc worktree> pytest tests/python/test_chat_daemon_liveness.py
"""

import asyncio
import os
import signal
import time

import pytest

from chat_fake_agent import (DAEMON, FakeAgent, load_daemon, point_at,
                             wait_until, write_outbox)

pytestmark = pytest.mark.skipif(not DAEMON.is_file(),
                                reason=f"chat daemon not checked out at {DAEMON}")

A, B = "192.168.1.211", "192.168.1.212"


def run(coro, timeout=30):
    return asyncio.run(asyncio.wait_for(coro, timeout))


# --- discovery ---------------------------------------------------------------

def test_discovery_cadence(tmp_path):
    mod = load_daemon(tmp_path)
    assert mod.next_discovery_delay(0, 6.0) == 15, "nothing claimed: rescan fast"
    assert mod.next_discovery_delay(3, 0.2) == 15, "a sub-second /24 sweep: network not up"
    assert mod.next_discovery_delay(3, 6.0) == 60
    assert mod.DISCOVERY_FAST_S == 15 and mod.DISCOVERY_SLOW_S == 60


def test_discovery_never_reprobes_a_claimed_host(tmp_path):
    mod = load_daemon(tmp_path)
    probed = []

    async def fake_probe(ip, timeout=8.0):
        probed.append(ip)
        return None

    mod.try_connect = fake_probe
    run(mod.discover_agents(exclude={A, B}))
    assert A not in probed and B not in probed
    assert len(probed) == 252


def test_first_scan_on_a_dead_network_retries_in_15s_not_300(tmp_path):
    mod = load_daemon(tmp_path)
    delays = []

    async def instant_scan(exclude=()):
        return []                       # network down: every connect fails at once

    real_sleep = asyncio.sleep

    async def fake_sleep(s, *a, **k):
        if s >= 1:
            delays.append(s)
            if len(delays) >= 2:
                raise asyncio.CancelledError
        await real_sleep(0)

    mod.discover_agents = instant_scan
    mod.asyncio.sleep = fake_sleep
    try:
        with pytest.raises(asyncio.CancelledError):
            run(mod.discovery_loop())
    finally:
        mod.asyncio.sleep = real_sleep
    assert delays == [15, 15]


def test_try_connect_closes_its_connection_when_the_probe_fails(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent().start()
        agent.delay["PROXY_GET"] = 1.0       # the probe times out mid-conversation
        point_at(mod, {A: agent})
        assert await mod.try_connect(A, timeout=0.3) is None
        assert await wait_until(lambda: not agent.open_conns(), 3), \
            "a failed probe leaked its connection (and an agent client slot)"
        assert all(c["end"] != "reset" for c in agent.conns)
        await agent.stop()

    run(go())


def test_an_announcing_agent_is_claimed_without_waiting_for_a_sweep(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent().start()
        point_at(mod, {A: agent})
        mod._on_announcement(A)
        assert await wait_until(lambda: A in mod.hosts, 3)
        mod._on_announcement(A)                 # already claimed: no second probe
        await mod.shutdown()
        await agent.stop()

    run(go())


# --- liveness and reaping ----------------------------------------------------

def test_seq_is_seeded_from_the_clock(tmp_path):
    mod = load_daemon(tmp_path)
    st = mod.HostState(A)
    assert st.next_seq >= int(time.time()) - 1, \
        "a re-added host restarted at 1 and overwrote its own history"


def test_last_seen_advances_on_every_reply_and_a_live_host_is_never_stale(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        mod.REAP_IDLE_S = 0.5
        agent = await FakeAgent(poll_ms=100).start()
        point_at(mod, {A: agent})
        st = mod.add_host(A)
        await asyncio.sleep(1.5)                 # three reap windows
        assert A in mod.hosts and not st.is_stale()
        assert mod._now() - st.last_seen < 0.5
        await mod.shutdown()
        await agent.stop()

    run(go())


def test_a_discovery_miss_does_not_reap_a_live_host(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent(poll_ms=100).start()
        point_at(mod, {A: agent})
        st = mod.add_host(A)

        async def scan_misses_everything(exclude=()):
            await asyncio.sleep(0.05)
            return []

        mod.discover_agents = scan_misses_everything
        mod.next_discovery_delay = lambda n, e: 0.05
        loop_task = asyncio.create_task(mod.discovery_loop())
        await asyncio.sleep(1.0)                 # ~20 missed "scans"
        assert mod.hosts.get(A) is st and not st.task.done()
        loop_task.cancel()
        await asyncio.gather(loop_task, return_exceptions=True)
        await mod.shutdown()
        await agent.stop()

    run(go())


def test_a_silent_host_releases_itself_and_its_held_text_survives(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        mod.REAP_IDLE_S = 0.6
        agent = await FakeAgent(poll_ms=100).start()
        point_at(mod, {A: agent})
        mod.add_host(A)
        assert await wait_until(lambda: len(agent.open_conns()) == 2, 3)
        await agent.set_up(False)                # powered off
        p = write_outbox(mod, A, 5, "answer for later", stream=False)
        mod.dispatch_queues()
        assert await wait_until(lambda: A not in mod.hosts, 5)
        assert p.exists(), "a released host's answer text was deleted"
        # it comes back: claimed again, and the held answer is delivered
        await agent.set_up(True)
        mod.add_host(A)
        mod.dispatch_queues()
        assert await wait_until(lambda: "answer for later\n" in agent.log, 5)
        await mod.shutdown()
        await agent.stop()

    run(go())


def test_a_host_is_not_stale_while_our_command_is_in_flight(tmp_path):
    mod = load_daemon(tmp_path)
    st = mod.HostState(A)
    st.last_seen = mod._now() - 10 * mod.REAP_IDLE_S
    assert st.is_stale()
    st.busy_until = mod._now() + 60           # a 15-minute EXECW on a Win9x box
    assert not st.is_stale()


def test_the_idle_send_connection_is_kept_alive(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        mod.KEEPALIVE_IDLE_S = 0.3
        agent = await FakeAgent(poll_ms=100, idle_drop_s=0.8).start()
        point_at(mod, {A: agent})
        st = mod.add_host(A)
        assert await wait_until(lambda: st.send_conn is not None, 3)
        send_rec = agent.conns[-1]
        await asyncio.sleep(2.0)                 # > 2x the agent's idle drop
        assert "PING" in send_rec["cmds"]
        assert send_rec["end"] == "open", "the agent dropped the idle send connection"
        assert st.send_conn.connected
        await mod.shutdown()
        await agent.stop()

    run(go())


def test_a_prompt_is_saved_before_the_next_poll_acknowledges_it(tmp_path):
    """Agents 1.85.0+ keep a taken prompt until that connection's NEXT command;
    the inbox file must exist by then."""
    async def go():
        mod = load_daemon(tmp_path)
        seen = {}

        class Agent(FakeAgent):
            async def _handle(self, cmd):
                if cmd.startswith("PROMPT_WAIT") and "sent" in seen and "inbox" not in seen:
                    seen["inbox"] = [p.name for p in mod.INBOX.glob("*.json")]
                return await super()._handle(cmd)

        agent = await Agent(poll_ms=100).start()
        point_at(mod, {A: agent})
        mod.add_host(A)
        assert await wait_until(lambda: len(agent.open_conns()) == 2, 3)
        agent.push_prompt("install quake")
        seen["sent"] = True
        assert await wait_until(lambda: "inbox" in seen, 3)
        assert len(seen["inbox"]) == 1, "the prompt was acknowledged before it was saved"
        await mod.shutdown()
        await agent.stop()

    run(go())


# --- the daemon as a whole ---------------------------------------------------

def _no_scan(mod, ips=()):
    async def scan(exclude=()):
        await asyncio.sleep(0)
        return [ip for ip in ips if ip not in exclude]
    mod.discover_agents = scan
    mod.listen_announcements = lambda: asyncio.sleep(0)


def test_zero_agents_is_a_normal_resting_state(tmp_path):
    """The fleet is powered on demand; the daemon must stay up with nothing
    to claim (it used to exit, and systemd turned that into a restart loop)."""
    async def go():
        mod = load_daemon(tmp_path)
        _no_scan(mod)
        main = asyncio.create_task(mod.main_async(install_signals=False))
        await asyncio.sleep(0.5)
        assert not main.done()
        mod.request_stop()
        await asyncio.wait_for(main, 5)

    run(go())


def test_sigterm_closes_every_agent_connection_with_a_fin(tmp_path):
    async def go():
        mod = load_daemon(tmp_path)
        agents = {A: await FakeAgent(poll_ms=100).start(),
                  B: await FakeAgent(poll_ms=100, slow_os=True).start()}   # a Win98 box
        point_at(mod, agents)
        _no_scan(mod, [A, B])
        main = asyncio.create_task(mod.main_async(install_signals=True))
        assert await wait_until(
            lambda: all(len(a.open_conns()) == 2 for a in agents.values()), 5)
        assert signal.getsignal(signal.SIGTERM) is not signal.SIG_DFL, \
            "no SIGTERM handler installed - refusing to signal the test process"
        os.kill(os.getpid(), signal.SIGTERM)
        await asyncio.wait_for(main, 10)
        for ip, a in agents.items():
            ends = [c["end"] for c in a.conns]
            assert ends and all(e == "eof" for e in ends), (ip, ends)
        for a in agents.values():
            await a.stop()

    run(go())


def test_a_timed_out_command_to_a_busy_win98_agent_ends_without_an_rst(tmp_path):
    """A Win9x agent running somebody's EXEC answers our command late. We time
    out and let go — but its late reply must be read, not reset."""
    async def go():
        mod = load_daemon(tmp_path)
        agent = await FakeAgent(slow_os=True).start()
        agent.delay["LOG_APPEND"] = 3.0          # longer than any 2 s drain
        point_at(mod, {A: agent})
        st = mod.HostState(A)
        mod.hosts[A] = st
        piece = mod.Piece("hello\n")
        assert await mod.send_piece(st, piece) == "delivered"   # sent, never resent
        assert await wait_until(lambda: not agent.open_conns(), 8)
        ends = [c["end"] for c in agent.conns]
        assert "reset" not in ends, ends
        assert agent.log.count("hello") == 1
        await agent.stop()

    run(go())
