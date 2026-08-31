"""Tests for the game-server probe library and the watchdog's restart policy.

Two things are worth pinning down here, and neither is the string formatting.

**The wire parsers**, against captured bytes from the real servers on .132.
Each engine family needs its own query packet and its own reply layout, and
every one of these fixtures encodes a mistake that produced a wrong number on
the dashboard: the Q3/Q2 infostring being on line 1 while QuakeWorld's is on
line 0, GoldSrc hiding its player count behind four NUL-terminated strings,
and bots being indistinguishable from people on any engine that does not
count them separately (ping 0 is the tell).

**The restart policy**, which is the part that can do damage. A watchdog that
restarts on the first silent probe stamps on map changes; one with no cap
flaps a permanently-broken server forever. Both bounds are asserted.

Run: pytest tests/python/test_gameservers.py
"""

import importlib.util
import os
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_GS_DIR = os.path.join(_HERE, "..", "..", "scripts", "game-servers")


def _load(name):
    path = os.path.join(_GS_DIR, f"{name}.py")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


gameservers = _load("gameservers")
watch = _load("gameservers_watch")


# --- captured from the live servers on 192.168.1.132, 2026-08-28 ------------

CS16 = (b"\xff\xff\xff\xffI0NSC Retro Fleet Arena (CS 1.6)\x00cs_militia\x00"
        b"cstrike\x00Counter-Strike\x00F\x00\x03\x10\x01dl\x00\x00"
        b"1.1.2.7/Stdio\x00")

Q3 = (b"\xff\xff\xff\xffstatusResponse\n"
      b"\\sv_hostname\\NSC Retro Fleet Arena (Q3A)\\sv_maxclients\\16"
      b"\\mapname\\q3dm7\n"
      b'21 0 "Doom"\n11 48 "somebody"\n23 0 "Keel"\n')

Q2 = (b"\xff\xff\xff\xffprint\n"
      b"\\hostname\\NSC Retro Fleet Arena (Q2)\\mapname\\q2dm1\\maxclients\\12\n")

QW = (b"\xff\xff\xff\xffn\\*version\\MVDSV 1.11"
      b"\\hostname\\NSC Retro Fleet Arena (QuakeWorld)\\maxclients\\16"
      b"\\map\\start\\status\\Standby\n\x00")

UT = (b"\\gamename\\ut\\hostname\\NSC Retro Fleet Arena (UT99)"
      b"\\maptitle\\Deck #16 ][\\numplayers\\3\\maxplayers\\10\\final\\")


@pytest.fixture
def canned(monkeypatch):
    """Replace the UDP round trip with a captured reply."""
    def install(payload):
        monkeypatch.setattr(gameservers, "_ask",
                            lambda *a, **k: (payload, 7.5))
    return install


# --- GoldSrc / A2S ----------------------------------------------------------

def test_a2s_reads_name_and_map(canned):
    canned(CS16)
    info = gameservers.probe_a2s(27018)
    assert info["name"] == "NSC Retro Fleet Arena (CS 1.6)"
    assert info["map"] == "cs_militia"


def test_a2s_reads_the_player_counts_behind_the_strings(canned):
    """players/maxplayers/bots sit after four NUL-terminated strings and a
    u16 appid. Reading them at the wrong offset yields plausible-looking
    nonsense rather than an error, so pin the exact values."""
    canned(CS16)
    info = gameservers.probe_a2s(27018)
    assert info["players"] == 3
    assert info["max_players"] == 16
    assert info["bots"] == 1


def test_a2s_answers_the_anti_reflection_challenge(monkeypatch):
    """Since 2020 HLDS may answer `A` + a 4-byte challenge that must be echoed
    back. Not doing so reports every GoldSrc server as down."""
    calls = []

    def fake(port, payload, timeout=None, host=None):
        calls.append(payload)
        if len(calls) == 1:
            return b"\xff\xff\xff\xffA\x01\x02\x03\x04", 1.0
        return CS16, 2.0

    monkeypatch.setattr(gameservers, "_ask", fake)
    info = gameservers.probe_a2s(27018)
    assert info is not None
    assert calls[1].endswith(b"\x01\x02\x03\x04")


def test_a2s_rejects_a_reply_that_is_not_an_info_response(canned):
    canned(b"\xff\xff\xff\xffZjunk")
    assert gameservers.probe_a2s(27018) is None


# --- Quake family -----------------------------------------------------------

def test_q3_infostring_is_on_line_1_not_line_0(canned):
    """Line 0 is `statusResponse`. Parsing it gives an empty dict, which once
    reported every healthy server as unnamed with 0 max players."""
    canned(Q3)
    info = gameservers.probe_q3(27961)
    assert info["name"] == "NSC Retro Fleet Arena (Q3A)"
    assert info["map"] == "q3dm7"
    assert info["max_players"] == 16


def test_q3_counts_players_and_separates_bots_by_ping_zero(canned):
    canned(Q3)
    info = gameservers.probe_q3(27961)
    assert info["players"] == 3
    assert info["bots"] == 2      # the two rows with ping 0


def test_q2_uses_its_own_keys(canned):
    canned(Q2)
    info = gameservers.probe_q2(27910)
    assert info["name"] == "NSC Retro Fleet Arena (Q2)"
    assert info["map"] == "q2dm1"
    assert info["max_players"] == 12
    assert info["players"] == 0


def test_qw_infostring_is_on_line_0(canned):
    """mvdsv puts its `n` header and the infostring on the SAME line, unlike
    Q3/Q2. Using the Q3 offset here loses the map and the max-player count."""
    canned(QW)
    info = gameservers.probe_qw(27502)
    assert info["name"] == "NSC Retro Fleet Arena (QuakeWorld)"
    assert info["map"] == "start"
    assert info["max_players"] == 16


def test_qw_trailing_nul_is_not_counted_as_a_player(canned):
    """The reply ends `\\n\\x00`. `str.strip()` does not remove a NUL, so a
    naive line count reports one phantom player on an empty server."""
    canned(QW)
    assert gameservers.probe_qw(27502)["players"] == 0


def test_unreachable_server_probes_as_none(monkeypatch):
    monkeypatch.setattr(gameservers, "_ask", lambda *a, **k: (None, None))
    for probe in (gameservers.probe_q3, gameservers.probe_q2,
                  gameservers.probe_qw, gameservers.probe_ut,
                  gameservers.probe_a2s, gameservers.probe_t2):
        assert probe(1234) is None


# --- Unreal -----------------------------------------------------------------

def test_ut_takes_the_counts_the_server_gives(canned):
    canned(UT)
    info = gameservers.probe_ut(7798)
    assert info["players"] == 3
    assert info["max_players"] == 10
    assert info["map"] == "Deck #16 ]["


# --- infostring edge cases --------------------------------------------------

def test_infostring_discards_the_leading_segment():
    """Pairing starts at index 1, and that is load-bearing in both directions:
    a Q3 infostring begins with a bare `\\` (so segment 0 is empty), while
    QuakeWorld's begins with its `n` header glued to the first key. Starting
    at 0 would read `n` as a key and shift every pair after it."""
    assert gameservers._infostring("\\a\\1\\b\\2") == {"a": "1", "b": "2"}
    assert gameservers._infostring("n\\a\\1\\b\\2") == {"a": "1", "b": "2"}


def test_infostring_keeps_empty_values():
    """An empty value is normal (`\\g_needpass\\\\mapname\\q3dm7`) and must not
    desynchronise the pairs that follow it."""
    assert gameservers._infostring("\\a\\\\b\\2") == {"a": "", "b": "2"}


def test_infostring_tolerates_a_dangling_key():
    assert gameservers._infostring("\\a\\1\\b") == {"a": "1"}


def test_infostring_of_junk_is_empty_not_an_exception():
    assert gameservers._infostring("no separators here") == {}


# --- aggregation ------------------------------------------------------------

def _collected(monkeypatch, rows, states):
    monkeypatch.setattr(gameservers, "unit_states", lambda units, *a: states)
    monkeypatch.setattr(gameservers, "PROBES",
                        {k: (lambda p, t=None, h=None, _r=rows, _k=k: _r.get(_k))
                         for k in gameservers.PROBES})
    return gameservers.collect(
        servers=[
            {"unit": "quake3-server", "label": "Q3", "engine": "q3",
             "probe": "q3", "port": 27961},
            {"unit": "cs16-server", "label": "CS", "engine": "goldsrc",
             "probe": "a2s", "port": 27018},
            {"unit": "tribes2-server", "label": "T2", "engine": "t2",
             "probe": "t2", "port": 28000},
        ],
        proxies=[])


def test_an_uninstalled_server_is_not_counted_as_down(monkeypatch):
    """`LoadState=not-found` means nobody ever installed it here. Counting it
    as down would leave the wall permanently red on a host that is fine."""
    snap = _collected(
        monkeypatch,
        {"q3": {"players": 4, "bots": 4, "max_players": 16, "rtt_ms": 1},
         "a2s": {"players": 2, "bots": 0, "max_players": 16, "rtt_ms": 9}},
        {"quake3-server": {"state": "active"},
         "cs16-server": {"state": "active"},
         "tribes2-server": {"state": "absent"}})
    assert snap["total"] == 2
    assert snap["up"] == 2
    assert snap["down"] == []
    assert [r["unit"] for r in snap["servers"] if not r["installed"]] \
        == ["tribes2-server"]


def test_humans_excludes_known_bots(monkeypatch):
    snap = _collected(
        monkeypatch,
        {"q3": {"players": 4, "bots": 4, "max_players": 16, "rtt_ms": 1},
         "a2s": {"players": 2, "bots": 0, "max_players": 16, "rtt_ms": 9}},
        {"quake3-server": {"state": "active"},
         "cs16-server": {"state": "active"},
         "tribes2-server": {"state": "absent"}})
    assert snap["players"] == 6
    assert snap["bots"] == 4
    assert snap["humans"] == 2


def test_a_mute_server_is_reported_down_with_its_unit_state(monkeypatch):
    snap = _collected(
        monkeypatch,
        {"a2s": {"players": 0, "bots": 0, "max_players": 16, "rtt_ms": 9}},
        {"quake3-server": {"state": "active"},
         "cs16-server": {"state": "active"},
         "tribes2-server": {"state": "absent"}})
    q3row = next(r for r in snap["servers"] if r["unit"] == "quake3-server")
    assert q3row["up"] is False
    assert q3row["unit_state"] == "active"     # systemd is happy; the game is not
    assert snap["down"] == ["quake3-server"]


# --- the restart policy -----------------------------------------------------

def _row(unit="cs16-server", **kw):
    row = {"unit": unit, "installed": True, "up": True, "unit_state": "active"}
    row.update(kw)
    return row


def test_a_healthy_server_is_left_alone():
    w = watch.Watch()
    assert w.decide(_row(), 1000.0) == (False, None)


def test_one_silent_probe_is_a_map_change_not_a_wedge():
    """Restarting on the first mute cycle would kick everyone off a server
    that was mid map-change. Three cycles is the threshold."""
    w = watch.Watch()
    row = _row(up=False)
    for i in range(watch.PROBE_FAIL_LIMIT):
        w.mute_streak[row["unit"]] = i
        should, why = w.decide(row, 1000.0)
        assert should is False
        assert "waiting" in why


def test_a_persistently_mute_but_active_unit_is_restarted():
    w = watch.Watch()
    row = _row(up=False)
    w.mute_streak[row["unit"]] = watch.PROBE_FAIL_LIMIT
    should, why = w.decide(row, 1000.0)
    assert should is True
    assert "mute" in why


def test_a_failed_unit_is_restarted_immediately():
    """systemd already knows the process is gone; no point waiting out three
    probe cycles for a server that is not running at all."""
    w = watch.Watch()
    should, why = w.decide(_row(up=False, unit_state="failed"), 1000.0)
    assert should is True
    assert "failed" in why


def test_a_restart_is_not_repeated_inside_the_cooldown():
    w = watch.Watch()
    row = _row(up=False, unit_state="failed")
    w.last_restart[row["unit"]] = 1000.0
    should, why = w.decide(row, 1000.0 + watch.COOLDOWN_SEC / 2)
    assert should is False
    assert "cooling down" in why


def test_the_cooldown_expires():
    w = watch.Watch()
    row = _row(up=False, unit_state="failed")
    w.last_restart[row["unit"]] = 1000.0
    should, _ = w.decide(row, 1000.0 + watch.COOLDOWN_SEC + 1)
    assert should is True


def test_a_server_that_cannot_start_is_not_flapped_forever():
    """After MAX_PER_HOUR attempts the watchdog stops and says a human is
    needed, rather than restarting a server with a missing pak file every
    five minutes for a week."""
    w = watch.Watch()
    row = _row(up=False, unit_state="failed")
    now = 100000.0
    w.restart_log[row["unit"]] = [now - 60 * i for i in range(watch.MAX_PER_HOUR)]
    should, why = w.decide(row, now)
    assert should is False
    assert "needs a human" in why


def test_the_hourly_cap_only_counts_the_last_hour():
    w = watch.Watch()
    row = _row(up=False, unit_state="failed")
    now = 100000.0
    w.restart_log[row["unit"]] = [now - 3600 - 60 * i
                                  for i in range(watch.MAX_PER_HOUR * 2)]
    should, _ = w.decide(row, now)
    assert should is True


def test_an_uninstalled_unit_is_never_restarted():
    w = watch.Watch()
    should, why = w.decide(_row(up=False, installed=False, unit_state="absent"),
                           1000.0)
    assert should is False
    assert why == "not installed"


def test_restarts_can_be_switched_off_entirely():
    w = watch.Watch(restart=False)
    should, why = w.decide(_row(up=False, unit_state="failed"), 1000.0)
    assert should is False
    assert why == "restarts disabled"


def test_a_recovered_server_forgets_its_mute_streak():
    """Otherwise a server that goes quiet twice a day for a map change is
    eventually restarted for no reason."""
    w = watch.Watch()
    w.mute_streak["cs16-server"] = 2
    w.decide(_row(), 1000.0)
    assert w.mute_streak["cs16-server"] == 0


# --- docker, the fleet's second process manager -----------------------------
#
# Tribes 2 runs in a container because it needs a 2001 userland. Asking
# systemd about it returns `not-found`, which this module reports as "never
# installed here" -- so a running game server was dropped off the wall
# entirely and an outage on it would have been invisible.


class _Res:
    def __init__(self, stdout="", stderr="", returncode=0):
        self.stdout = stdout
        self.stderr = stderr
        self.returncode = returncode


def test_docker_state_maps_running_to_active(monkeypatch):
    monkeypatch.setattr(gameservers.subprocess, "run", lambda *a, **k: _Res(
        "/tribes2-server\trunning\t2026-08-28T10:00:00.123456789Z\t2\n"))
    out = gameservers.docker_states(["tribes2-server"])
    assert out["tribes2-server"]["state"] == "active"
    assert out["tribes2-server"]["restarts"] == 2
    assert out["tribes2-server"]["uptime_sec"] > 0


def test_docker_exited_container_is_failed(monkeypatch):
    monkeypatch.setattr(gameservers.subprocess, "run", lambda *a, **k: _Res(
        "/tribes2-server\texited\t2026-08-28T10:00:00Z\t0\n"))
    assert gameservers.docker_states(["tribes2-server"])["tribes2-server"]["state"] \
        == "failed"


def test_docker_container_that_does_not_exist_is_absent(monkeypatch):
    monkeypatch.setattr(gameservers.subprocess, "run",
                        lambda *a, **k: _Res("", "No such object", 1))
    assert gameservers.docker_states(["gone"])["gone"] == {"state": "absent"}


def test_no_docker_binary_is_unknown_not_absent(monkeypatch):
    """'docker is not installed' and 'the container is missing' are different
    facts. Reporting the first as the second would quietly drop a real server
    off the board on any host without docker."""
    def missing(*a, **k):
        raise FileNotFoundError("docker")
    monkeypatch.setattr(gameservers.subprocess, "run", missing)
    out = gameservers.docker_states(["tribes2-server"])
    assert out["tribes2-server"]["state"] == "unknown"
    assert "docker not installed" in out["tribes2-server"]["error"]


def test_docker_nanosecond_timestamps_parse():
    """Docker emits RFC3339 with 9 fractional digits, which fromisoformat
    rejected before 3.11 — and a crash here would take the whole sweep down."""
    assert gameservers._parse_docker_time("2026-08-28T10:00:00.123456789Z") > 0
    assert gameservers._parse_docker_time("2026-08-28T10:00:00Z") > 0
    assert gameservers._parse_docker_time("nonsense") is None
    assert gameservers._parse_docker_time("") is None


def test_states_are_fetched_once_per_manager(monkeypatch):
    """One call per manager, not one per server — the sweep runs every 20s."""
    calls = {"systemd": 0, "docker": 0}
    monkeypatch.setattr(gameservers, "unit_states",
                        lambda u, *a: (calls.__setitem__("systemd", calls["systemd"] + 1),
                                       {n: {"state": "active"} for n in u})[1])
    monkeypatch.setattr(gameservers, "docker_states",
                        lambda n, *a: (calls.__setitem__("docker", calls["docker"] + 1),
                                       {c: {"state": "active"} for c in n})[1])
    gameservers._all_states([
        {"unit": "a"}, {"unit": "b"},
        {"unit": "c", "manager": "docker"}, {"unit": "d", "manager": "docker"},
    ])
    assert calls == {"systemd": 1, "docker": 1}


def test_a_manager_we_cannot_reach_is_not_counted_as_a_down_server(monkeypatch):
    monkeypatch.setattr(gameservers, "unit_states", lambda u, *a: {})
    monkeypatch.setattr(gameservers, "docker_states", lambda n, *a: {
        "tribes2-server": {"state": "unknown", "error": "docker not installed"}})
    monkeypatch.setattr(gameservers, "PROBES", {"t2": lambda *a, **k: None})
    snap = gameservers.collect(
        servers=[{"unit": "tribes2-server", "label": "T2", "engine": "t2",
                  "probe": "t2", "port": 28000, "manager": "docker"}],
        proxies=[])
    row = snap["servers"][0]
    assert row["installed"] is False
    assert row["unavailable"] == "docker not installed"
    assert snap["total"] == 0 and snap["down"] == []


def test_restart_dispatches_to_the_owning_manager(monkeypatch):
    seen = []
    monkeypatch.setattr(gameservers, "restart_unit",
                        lambda u, **k: (seen.append(("systemd", u)), (True, "ok"))[1])
    monkeypatch.setattr(gameservers, "restart_container",
                        lambda n, **k: (seen.append(("docker", n)), (True, "ok"))[1])
    gameservers.restart({"unit": "cs16-server"})
    gameservers.restart({"unit": "tribes2-server", "manager": "docker"})
    assert seen == [("systemd", "cs16-server"), ("docker", "tribes2-server")]


# --- the Tribes 2 probe -----------------------------------------------------

def test_t2_requires_the_reply_to_echo_our_key(monkeypatch):
    """Any UDP noise arriving at the socket would otherwise read as 'alive'.
    Tribes 2 answers 0x0E with 0x10 and echoes the four key bytes back."""
    sent = {}

    def fake(port, payload, timeout=None, host=None):
        sent["payload"] = payload
        return bytes([0x10, 0]) + payload[2:6], 0.9

    monkeypatch.setattr(gameservers, "_ask", fake)
    info = gameservers.probe_t2(28000)
    assert info is not None
    assert info["rtt_ms"] == 0.9
    assert sent["payload"][0] == 0x0E


def test_t2_rejects_a_reply_with_the_wrong_key(monkeypatch):
    monkeypatch.setattr(gameservers, "_ask",
                        lambda *a, **k: (bytes([0x10, 0, 9, 9, 9, 9]), 1.0))
    assert gameservers.probe_t2(28000) is None


def test_t2_rejects_a_reply_of_the_wrong_type(monkeypatch):
    def fake(port, payload, timeout=None, host=None):
        return bytes([0x99, 0]) + payload[2:6], 1.0
    monkeypatch.setattr(gameservers, "_ask", fake)
    assert gameservers.probe_t2(28000) is None


def test_t2_reports_no_player_count_rather_than_zero(monkeypatch):
    """TribesNext encrypts the info response, so the count is unknowable from
    off the box. Reporting 0 would assert an empty server we cannot see into."""
    def fake(port, payload, timeout=None, host=None):
        return bytes([0x10, 0]) + payload[2:6], 1.0
    monkeypatch.setattr(gameservers, "_ask", fake)
    info = gameservers.probe_t2(28000)
    assert "players" not in info
    assert info["map"] is None


# ==========================================================================
# NetQuake / Hexen II control protocol, and SoF2's three-number player line
# --------------------------------------------------------------------------
# Added 2026-08-31 with the four servers this fleet gained that day
# (quake1-server :26000, q3ta-server :27962, jka-server :29070,
#  sof2-server :20100).
#
# The failure these pin down is the project's signature shape -- a probe that
# reports a healthy thing as broken, and says nothing about why. Quake 1 and
# Hexen II answer NEITHER `getstatus` NOR `status`: they speak the Quake
# control protocol on the game port and drop the other two in silence, so a
# single-getstatus sweep calls a live host dead. And a Hexen II host answers
# only to the game string "HEXENII" -- send "QUAKE" and it is, again,
# indistinguishable from an unplugged machine.
# ==========================================================================

import struct as _struct


def _nq_reply(address, hostname, level, cur, mx, proto=3, kind=0x83):
    body = (bytes([kind]) + address.encode() + b"\x00" + hostname.encode() + b"\x00"
            + level.encode() + b"\x00" + bytes([cur, mx, proto]))
    return _struct.pack(">I", 0x80000000 | (len(body) + 4)) + body


NQ_QUAKE = _nq_reply("0.0.0.0:26000", "NSC Retro Fleet Arena (Quake)", "e1m1", 2, 16)
NQ_HEXEN2 = _nq_reply("192.168.1.123:26900", "NSC Retro Fleet Hexen II", "demo1", 2, 8, proto=5)

# Captured from sof2-server on 2026-08-31 with .123 and .240 both in mp_shop.
# Note the player lines: THREE numbers before the name, not two.
SOF2 = (b"\xff\xff\xff\xffstatusResponse\n"
        b"\\game_version\\sof2mp-1.02\\g_gametype\\dm\\protocol\\2004"
        b"\\mapname\\mp_shop\\sv_hostname\\NSC Retro Fleet Arena (SoF2)"
        b"\\sv_maxclients\\12\n"
        b'0 0 0 "B123"\n0 5 0 "B240"\n')


def test_nq_request_is_the_control_packet_not_getstatus(monkeypatch):
    """The bytes on the wire are the thing under test: a getstatus here would
    be answered by nothing, and the caller could not tell that from a dead
    server."""
    seen = {}

    def fake(port, payload, timeout=None, host=None):
        seen["payload"] = payload
        return NQ_QUAKE, 1.0

    monkeypatch.setattr(gameservers, "_ask", fake)
    gameservers.probe_nq(26000)
    payload = seen["payload"]
    assert payload[:4] == _struct.pack(">I", 0x80000000 | len(payload))
    assert payload[4] == 0x02                      # CCREQ_SERVER_INFO
    assert payload[5:11] == b"QUAKE\x00"
    assert payload[11] == 3                        # NET_PROTOCOL_VERSION
    assert b"getstatus" not in payload and b"status" not in payload


def test_nq_reads_the_servers_own_player_count(canned):
    canned(NQ_QUAKE)
    info = gameservers.probe_nq(26000)
    assert info["name"] == "NSC Retro Fleet Arena (Quake)"
    assert info["map"] == "e1m1"
    assert info["players"] == 2
    assert info["max_players"] == 16
    # NetQuake has no bots, and the count comes from the server rather than
    # from a ping-0 heuristic, so this is a fact and not a guess.
    assert info["bots"] == 0


def test_hexen2_sends_its_own_game_string(monkeypatch):
    """A Hexen II host ignores b"QUAKE". Sending the wrong string is how a
    live host reads as dead, so the string is asserted, not the parse."""
    seen = {}

    def fake(port, payload, timeout=None, host=None):
        seen["p"] = payload
        return NQ_HEXEN2, 2.0

    monkeypatch.setattr(gameservers, "_ask", fake)
    info = gameservers.probe_hexen2(26900)
    assert b"HEXENII\x00" in seen["p"]
    assert b"QUAKE\x00" not in seen["p"]
    assert info["name"] == "NSC Retro Fleet Hexen II"
    assert info["map"] == "demo1"
    assert info["players"] == 2 and info["max_players"] == 8


def test_nq_rejects_a_reply_that_is_not_a_server_info(canned):
    canned(_nq_reply("x", "y", "z", 0, 8, kind=0x81))   # CCREP_ACCEPT
    assert gameservers.probe_nq(26000) is None


def test_nq_unreachable_is_none_not_an_exception(monkeypatch):
    monkeypatch.setattr(gameservers, "_ask", lambda *a, **k: (None, None))
    assert gameservers.probe_nq(26000) is None
    assert gameservers.probe_hexen2(26900) is None


def test_sof2_counts_both_players_despite_the_third_number(canned):
    canned(SOF2)
    info = gameservers.probe_sof2(20100)
    assert info["name"] == "NSC Retro Fleet Arena (SoF2)"
    assert info["map"] == "mp_shop"
    assert info["players"] == 2


def test_sof2_never_claims_a_bot(canned):
    """SoF2 multiplayer ships no bots at all, and its player line carries an
    EXTRA number, so the shared `<score> <ping> "<name>"` ping-0 rule would be
    reading the wrong field. `0 0 0 "B123"` is a real person on .123 -- calling
    it a bot would hide exactly the two-machine LAN result this server exists
    to make possible."""
    canned(SOF2)
    assert gameservers.probe_sof2(20100)["bots"] == 0


def test_the_new_servers_are_in_the_table():
    """A server that is running but absent from SERVERS is a server the
    watchdog never restarts and the wall never shows."""
    by_unit = {s["unit"]: s for s in gameservers.SERVERS}
    for unit, port, probe in (("quake1-server", 26000, "nq"),
                              ("q3ta-server", 27962, "q3"),
                              ("jka-server", 29070, "q3"),
                              ("sof2-server", 20100, "sof2")):
        assert unit in by_unit, unit
        assert by_unit[unit]["port"] == port
        assert by_unit[unit]["probe"] == probe
        assert by_unit[unit]["probe"] in gameservers.PROBES


def test_quake1_is_not_probed_as_quakeworld():
    """Both are 'Quake', they are different protocols, and they are different
    servers on different ports. Probing 26000 the QuakeWorld way returns
    nothing at all."""
    q1 = next(s for s in gameservers.SERVERS if s["unit"] == "quake1-server")
    qw = next(s for s in gameservers.SERVERS if s["unit"] == "quakeworld-server")
    assert q1["probe"] == "nq" and qw["probe"] == "qw"
    assert q1["port"] != qw["port"]
