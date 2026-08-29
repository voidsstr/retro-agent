"""Tests for the bot policy server.

Phase 0's job is to prove the LOOP, so these test the properties a game server
depends on rather than any notion of playing well:

  * a policy that throws must not take the server down, and must not leave a
    game server waiting on a socket — a stalled game server is worse than a
    stupid bot;
  * a malformed or stale-schema request is refused, not fed to a model;
  * schema offsets are resolved once, not scanned per bot per tick (the Phase 0
    sweep measured that mistake at ~1 ms of serve time at 64 bots);
  * an AF_UNIX path over the kernel's 108-byte limit fails with a message that
    names the limit, rather than "AF_UNIX path too long".

Run: pytest tests/python/test_gamebots_policyd.py
"""

import importlib.util
import socket
import sys
import threading
import time
from pathlib import Path

import pytest

_GB = Path(__file__).resolve().parent.parent.parent / "scripts" / "gamebots"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _GB / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


schema = _load("schema")
policyd = _load("policyd")


def _blank():
    return [0.0] * schema.OBS_DIM


def _alive():
    obs = _blank()
    obs[policyd._HEALTH_OFF] = 1.0
    for i in range(schema.NUM_RAYS_H):
        obs[policyd._RAY_H_OFF + i] = 1.0     # open space all round
    return obs


def _put_enemy(obs, slot=0, dir_f=1.0, dir_r=0.0, dir_u=0.0,
               dist=0.5, visible=1.0, teammate=0.0):
    base = policyd._ENT_OFF[slot]
    obs[base] = 1.0                 # present
    obs[base + 1] = teammate
    obs[base + 2], obs[base + 3], obs[base + 4] = dir_f, dir_r, dir_u
    obs[base + 5] = dist
    obs[base + 9] = visible
    return obs


# --- the no-op policy: Phase 0's exit criterion --------------------------

def test_noop_returns_exactly_one_null_action_per_bot():
    """'A bot that stands still because a Python process told it to.'"""
    p = policyd.NoOpPolicy()
    entries = [(i, _alive()) for i in range(5)]
    out = p.act(0, 0, entries)
    assert [a[0] for a in out] == [0, 1, 2, 3, 4]
    for _bid, buttons, pitch, yaw, fwd, side, weapon in out:
        assert (buttons, pitch, yaw, fwd, side, weapon) == (0, 0.0, 0.0, 0.0, 0.0, 0)


def test_noop_handles_an_empty_batch():
    assert policyd.NoOpPolicy().act(0, 0, []) == []


# --- the scripted policy: realistic access pattern -----------------------

def test_scripted_turns_toward_an_enemy_on_the_right():
    out = policyd.ScriptedPolicy().act(
        0, 0, [(0, _put_enemy(_alive(), dir_r=1.0))])
    _b, _btn, _pitch, yaw, _f, _s, _w = out[0]
    assert yaw > 0, "should turn right toward an enemy on the right"


def test_scripted_turns_the_other_way_for_an_enemy_on_the_left():
    out = policyd.ScriptedPolicy().act(
        0, 0, [(0, _put_enemy(_alive(), dir_r=-1.0))])
    assert out[0][3] < 0


def test_scripted_fires_at_a_visible_aligned_enemy():
    out = policyd.ScriptedPolicy().act(
        0, 0, [(0, _put_enemy(_alive(), dir_r=0.0, dist=0.5, visible=1.0))])
    assert out[0][1] & schema.BTN_ATTACK


def test_scripted_holds_fire_when_the_enemy_is_not_visible():
    out = policyd.ScriptedPolicy().act(
        0, 0, [(0, _put_enemy(_alive(), dir_r=0.0, visible=0.0))])
    assert not (out[0][1] & schema.BTN_ATTACK)


def test_scripted_does_not_shoot_teammates():
    """The entity slots hold both teams; a policy that ignores the team flag
    is a team-killing bot, which is the fastest way to make a server unfun."""
    out = policyd.ScriptedPolicy().act(
        0, 0, [(0, _put_enemy(_alive(), dir_r=0.0, visible=1.0, teammate=1.0))])
    assert not (out[0][1] & schema.BTN_ATTACK)


def test_scripted_picks_the_nearest_enemy_not_the_first_slot():
    obs = _alive()
    _put_enemy(obs, slot=0, dir_r=-1.0, dist=0.9)     # far, on the left
    _put_enemy(obs, slot=1, dir_r=1.0, dist=0.1)      # near, on the right
    assert policyd.ScriptedPolicy().act(0, 0, [(0, obs)])[0][3] > 0


def test_scripted_holds_still_when_dead():
    obs = _alive()
    obs[policyd._HEALTH_OFF] = 0.0
    out = policyd.ScriptedPolicy().act(0, 0, [(0, obs)])
    assert out[0][1:] == (0, 0.0, 0.0, 0.0, 0.0, 0)


def test_scripted_holds_still_when_the_server_says_paused():
    out = policyd.ScriptedPolicy().act(0, schema.FLAG_PAUSED, [(0, _alive())])
    assert out[0][1:] == (0, 0.0, 0.0, 0.0, 0.0, 0)


def test_scripted_output_is_always_within_action_bounds():
    """Whatever the observation, the action handed to a game server must be
    physically possible — including for garbage input."""
    rng_obs = [
        _alive(),
        _put_enemy(_alive(), dir_r=99.0, dir_u=-99.0, dist=0.0),
        [float("nan")] * schema.OBS_DIM,
        [1e30] * schema.OBS_DIM,
    ]
    for obs in rng_obs:
        for _b, _btn, pitch, yaw, fwd, side, _w in \
                policyd.ScriptedPolicy().act(0, 0, [(0, obs)]):
            assert abs(pitch) <= schema.MAX_PITCH_DELTA_DEG
            assert abs(yaw) <= schema.MAX_YAW_DELTA_DEG
            assert -1.0 <= fwd <= 1.0 and -1.0 <= side <= 1.0


# --- the performance mistake the sweep caught ----------------------------

def test_schema_offsets_are_resolved_once_not_per_lookup():
    """The first version scanned the 140-entry field table for every entity
    slot, for every bot, on every tick — measured at ~1 ms of serve time with
    64 bots. Offsets must come from the schema, but exactly once."""
    src = (_GB / "policyd.py").read_text()
    hot = src[src.index("class ScriptedPolicy"):]
    assert "_ent_offset(" not in hot, "per-call offset lookup back in the hot loop"
    assert "_ENT_OFF" in hot
    assert isinstance(policyd._ENT_OFF, tuple)
    assert len(policyd._ENT_OFF) == schema.MAX_ENTITIES


def test_entity_subfields_are_derived_by_name_not_hand_counted():
    """The bug this pins: `visible` was read at `base + 7`, which is actually
    the second component of rel_vel, so the scripted policy never fired. A
    trained policy would have shown no error at all — just a silently worse
    model. Sub-offsets come from the schema by name."""
    table = {f[1]: f[2] for f in schema.FIELD_TABLE}
    base0 = table["e0_present"]
    assert policyd._E_TEAMMATE == table["e0_is_teammate"] - base0
    assert policyd._E_DIR == table["e0_dir"] - base0
    assert policyd._E_DIST == table["e0_dist_norm"] - base0
    assert policyd._E_HEALTH == table["e0_health_frac"] - base0
    assert policyd._E_VISIBLE == table["e0_visible"] - base0

    # And the relative layout must hold for every slot, since slot 0's offsets
    # are reused for all of them.
    for i in range(schema.MAX_ENTITIES):
        b = table[f"e{i}_present"]
        assert table[f"e{i}_visible"] - b == policyd._E_VISIBLE
        assert table[f"e{i}_dist_norm"] - b == policyd._E_DIST


def test_no_hand_counted_entity_offsets_remain_in_the_hot_loop():
    src = (_GB / "policyd.py").read_text()
    hot = src[src.index("class ScriptedPolicy"):]
    for bad in ("base + 1]", "base + 2]", "base + 4]", "base + 5]", "base + 7]"):
        assert bad not in hot, f"hand-counted entity offset {bad!r} is back"


def test_offsets_agree_with_the_schema():
    """Precomputing them must not mean hardcoding them."""
    table = {f[1]: f[2] for f in schema.FIELD_TABLE}
    assert policyd._RAY_H_OFF == table["ray_h"]
    assert policyd._HEALTH_OFF == table["health_frac"]
    for i, off in enumerate(policyd._ENT_OFF):
        assert off == table[f"e{i}_present"]


# --- server robustness ---------------------------------------------------

def test_socket_path_over_the_kernel_limit_is_explained(tmp_path):
    long_path = "/tmp/" + "x" * 120 + "/p.sock"
    srv = policyd.PolicyServer(policyd.NoOpPolicy(), long_path)
    with pytest.raises(ValueError) as e:
        srv.check_socket_path()
    msg = str(e.value)
    assert "107" in msg and "shorter path" in msg


def test_a_reasonable_socket_path_passes(tmp_path):
    srv = policyd.PolicyServer(policyd.NoOpPolicy(), str(tmp_path / "p.sock"))
    srv.check_socket_path()


class _Boom(policyd.Policy):
    name = "boom"

    def act(self, tick, flags, entries):
        raise RuntimeError("model exploded")


def test_a_throwing_policy_still_answers_every_bot(tmp_path):
    """A game server blocked on a socket is a stalled game server. If the
    policy raises, the bots get null actions and the adapter falls back — but
    a reply always goes out."""
    sock_path = str(tmp_path / "p.sock")
    server = policyd.PolicyServer(_Boom(), sock_path, status_path=None,
                                  stats_interval=0)
    server.start()
    t = threading.Thread(target=server.run_forever_for_test
                         if hasattr(server, "run_forever_for_test") else server.run,
                         daemon=True)
    t.start()
    try:
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                c.connect(sock_path)
                break
            except OSError:
                time.sleep(0.02)
        c.sendall(schema.pack_request(1, [(0, _alive()), (1, _alive())]))
        c.settimeout(5)
        want = schema.HEADER_SIZE + 2 * schema.ACTION_SIZE
        buf = b""
        while len(buf) < want:
            chunk = c.recv(want - len(buf))
            assert chunk, "server closed instead of answering"
            buf += chunk
        _tick, _flags, actions = schema.unpack_response(buf)
        assert len(actions) == 2
        assert all(a[1:] == (0, 0.0, 0.0, 0.0, 0.0, 0) for a in actions)
        assert server.metrics.errors >= 1
        c.close()
    finally:
        server.stop()


def test_metrics_percentiles_survive_an_empty_reservoir():
    m = policyd.Metrics()
    snap = m.snapshot()
    assert snap["requests"] == 0
    assert snap["serve_us_p50"] is None


def test_metrics_reservoir_is_bounded():
    """This service is meant to run for weeks; an unbounded latency list would
    grow without limit."""
    m = policyd.Metrics()
    for i in range(policyd.Metrics.RESERVOIR * 3):
        m.record(8, float(i))
    assert len(m._lat) == policyd.Metrics.RESERVOIR
    assert m.requests == policyd.Metrics.RESERVOIR * 3
    assert m.snapshot()["bots_served"] == policyd.Metrics.RESERVOIR * 3 * 8


def test_status_publish_is_atomic_and_world_readable(tmp_path):
    """Same convention as the dashboard collector — so this can appear on the
    login-screen wall instead of being another invisible service."""
    import json
    import os
    import stat
    target = tmp_path / "sub" / "status.json"
    policyd.publish_status({"policy": "noop", "requests": 3}, str(target))
    assert json.loads(target.read_text())["requests"] == 3
    assert stat.S_IMODE(os.stat(target).st_mode) & stat.S_IROTH
    assert not list(target.parent.glob("*.tmp*"))


# ------------------------------------------------- the TCP endpoint
#
# Added for UT99: UnrealScript's only outbound networking is `TcpLink`, which
# cannot speak to a Unix socket. Same wire format, different transport.
#
# The security property matters more than the speed one. This endpoint takes
# observations and returns actions with no authentication whatsoever — that is
# fine on loopback and is not fine on a LAN full of retro boxes, so the default
# bind address is the thing to pin.

def test_tcp_defaults_to_loopback_when_only_a_port_is_given(tmp_path):
    srv = policyd.PolicyServer(policyd.NoOpPolicy(), str(tmp_path / "p.sock"),
                               status_path=None, stats_interval=0,
                               tcp_listen="27201")
    srv.sel = policyd.selectors.DefaultSelector()
    srv._listen_tcp("27201")
    try:
        host, _port = srv._tcp_srv.getsockname()
        assert host == "127.0.0.1", \
            "an unauthenticated endpoint must not default to all interfaces"
    finally:
        srv._tcp_srv.close()


def test_tcp_is_off_unless_asked_for(tmp_path):
    srv = policyd.PolicyServer(policyd.NoOpPolicy(), str(tmp_path / "p.sock"))
    assert srv.tcp_listen is None
    assert srv._tcp_srv is None


def test_tcp_round_trip_matches_the_unix_socket(tmp_path):
    """Same bytes in, same bytes out — the transport must not change the
    protocol, or an engine would need a second implementation of it."""
    import socket as _s
    import threading
    import time as _t

    sock_path = str(tmp_path / "p.sock")
    server = policyd.PolicyServer(policyd.ScriptedPolicy(), sock_path,
                                  status_path=None, stats_interval=0,
                                  tcp_listen="127.0.0.1:0")
    server.start()
    t = threading.Thread(target=server.run, daemon=True)
    t.start()
    try:
        obs = [0.0] * schema.OBS_DIM
        obs[policyd._HEALTH_OFF] = 1.0
        entries = [(0, obs), (1, obs)]
        req = schema.pack_request(7, entries)
        want = schema.HEADER_SIZE + 2 * schema.ACTION_SIZE

        def ask(sock):
            sock.sendall(req)
            buf = b""
            while len(buf) < want:
                chunk = sock.recv(want - len(buf))
                assert chunk, "server closed mid-response"
                buf += chunk
            return schema.unpack_response(buf)

        for _ in range(100):          # wait for run() to bind
            if server._tcp_srv is not None:
                break
            _t.sleep(0.02)
        assert server._tcp_srv is not None, "TCP listener never came up"
        port = server._tcp_srv.getsockname()[1]
        tcp = _s.socket()
        tcp.settimeout(5)
        tcp.connect(("127.0.0.1", port))
        uds = _s.socket(_s.AF_UNIX, _s.SOCK_STREAM)
        uds.settimeout(5)
        uds.connect(sock_path)

        tick_a, _fa, acts_a = ask(tcp)
        tick_b, _fb, acts_b = ask(uds)
        assert tick_a == tick_b == 7
        assert [a[0] for a in acts_a] == [a[0] for a in acts_b] == [0, 1]
        tcp.close()
        uds.close()
    finally:
        server.stop()


def test_a_bad_request_over_tcp_is_refused_like_any_other(tmp_path):
    """The transport must not become a way around the schema check."""
    import socket as _s
    import struct as _st
    import threading
    import time as _t

    server = policyd.PolicyServer(policyd.NoOpPolicy(), str(tmp_path / "p.sock"),
                                  status_path=None, stats_interval=0,
                                  tcp_listen="127.0.0.1:0")
    server.start()
    threading.Thread(target=server.run, daemon=True).start()
    try:
        buf = bytearray(schema.pack_request(1, [(0, [0.0] * schema.OBS_DIM)]))
        _st.pack_into("<I", buf, 4, schema.SCHEMA_HASH ^ 0xFFFF)
        for _ in range(100):
            if server._tcp_srv is not None:
                break
            _t.sleep(0.02)
        c = _s.socket()
        c.settimeout(5)
        c.connect(("127.0.0.1", server._tcp_srv.getsockname()[1]))
        c.sendall(bytes(buf))
        c.settimeout(3)
        assert c.recv(64) == b"", "a stale-schema adapter was not disconnected"
        assert server.metrics.rejects_schema >= 1
        c.close()
    finally:
        server.stop()
