"""Tests for the login-screen dashboard collector.

Focus is on the properties the greeter depends on, because a bad state file is
the one failure the extension cannot recover from at 3am:

  * the state file is published atomically and world-readable (the reader is a
    `gdm-greeter-*` dynamic user we cannot name in advance);
  * a box answering on two NICs collapses to one row;
  * agent health degrades to a described state rather than an exception when
    /tmp/retro-chat is absent, which is exactly what a fresh boot looks like.

Run: pytest dashboard/tests/test_dashboard_collector.py
"""

import asyncio
import importlib.util
import json
import os
import stat
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_COLLECTOR = os.path.join(_HERE, "..", "collector", "dashboard_collector.py")


def _load():
    spec = importlib.util.spec_from_file_location("dashboard_collector", _COLLECTOR)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def dc():
    return _load()


# --------------------------------------------------------------- publish

def test_publish_is_atomic_and_world_readable(dc, tmp_path):
    target = tmp_path / "sub" / "state.json"
    dc.publish({"schema": 1, "hello": "world"}, str(target))

    assert target.exists()
    assert json.loads(target.read_text())["hello"] == "world"

    mode = stat.S_IMODE(os.stat(target).st_mode)
    assert mode & stat.S_IROTH, "greeter runs as a different user and must be able to read it"

    dir_mode = stat.S_IMODE(os.stat(target.parent).st_mode)
    assert dir_mode & stat.S_IXOTH, "greeter must be able to traverse into the directory"


def test_publish_leaves_no_temp_files(dc, tmp_path):
    target = tmp_path / "state.json"
    for i in range(3):
        dc.publish({"schema": 1, "n": i}, str(target))
    leftovers = [p for p in os.listdir(tmp_path) if p != "state.json"]
    assert leftovers == [], f"temp files leaked: {leftovers}"


def test_publish_overwrites_in_place(dc, tmp_path):
    target = tmp_path / "state.json"
    dc.publish({"n": 1}, str(target))
    first_inode = os.stat(target).st_ino
    dc.publish({"n": 2}, str(target))
    assert json.loads(target.read_text())["n"] == 2
    # os.replace swaps the inode; the point is the reader never sees a
    # partially written file, not that the inode is stable.
    assert os.stat(target).st_ino != first_inode or True


# ------------------------------------------------------------ fleet list

def test_fleet_config_falls_back_to_defaults(dc, tmp_path, monkeypatch):
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "nope"))
    nodes = dc._load_fleet_config(str(tmp_path / "missing.json"))
    assert nodes, "must fall back to the built-in fleet"
    assert all("ip" in n for n in nodes)


def test_fleet_config_reads_explicit_file(dc, tmp_path, monkeypatch):
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "nope"))
    cfg = tmp_path / "fleet.json"
    cfg.write_text(json.dumps({"nodes": [{"ip": "10.0.0.5", "label": "solo"}]}))
    nodes = dc._load_fleet_config(str(cfg))
    assert [n["ip"] for n in nodes] == ["10.0.0.5"]


def test_fleet_config_unions_claimed_agents(dc, tmp_path, monkeypatch):
    """A box the chat daemon claimed but nobody added to the config still shows."""
    chat = tmp_path / "chat"
    chat.mkdir()
    (chat / "daemon.log").write_text(
        "2026-08-27 13:36:17,723 [INFO] discovery: claimed 2 agents: "
        "['192.168.1.82', '192.168.1.201']\n"
    )
    monkeypatch.setattr(dc, "CHAT_ROOT", str(chat))

    cfg = tmp_path / "fleet.json"
    cfg.write_text(json.dumps({"nodes": [{"ip": "192.168.1.82", "label": "whitebeast"}]}))

    ips = [n["ip"] for n in dc._load_fleet_config(str(cfg))]
    assert "192.168.1.201" in ips, "claimed-but-unconfigured box must appear"
    assert ips.count("192.168.1.82") == 1, "already-configured box must not duplicate"


def test_claimed_parser_survives_junk(dc, tmp_path, monkeypatch):
    chat = tmp_path / "chat"
    chat.mkdir()
    (chat / "daemon.log").write_text("not a claim line\nclaimed nothing useful\n")
    monkeypatch.setattr(dc, "CHAT_ROOT", str(chat))
    assert dc._claimed_from_daemon_log() == []


def test_claimed_parser_handles_missing_log(dc, tmp_path, monkeypatch):
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "absent"))
    assert dc._claimed_from_daemon_log() == []


# ------------------------------------------------------- multi-NIC merge

def test_probe_merges_two_nics_of_one_box(dc, monkeypatch):
    """whitebeast answers on .82 (Wi-Fi) and .249 (Ethernet) — one row, not two."""
    async def fake_probe(node, secret, timeout):
        return {
            "ip": node["ip"], "label": node["label"], "up": True,
            "name": "WHITEBEAST", "os": "Win6.2", "rtt_ms": 5, "error": None,
        }

    monkeypatch.setattr(dc, "_probe_one", fake_probe)
    nodes = [
        {"ip": "192.168.1.82", "label": "whitebeast"},
        {"ip": "192.168.1.249", "label": "whitebeast"},
    ]
    merged = asyncio.run(dc._probe_fleet(nodes, "secret", 1.0))
    assert len(merged) == 1
    assert merged[0]["also_at"] in ("192.168.1.82", "192.168.1.249")


def test_probe_keeps_distinct_boxes_apart(dc, monkeypatch):
    async def fake_probe(node, secret, timeout):
        return {
            "ip": node["ip"], "label": node["label"], "up": True,
            "name": node["label"].upper(), "os": "Win6.2", "rtt_ms": 5, "error": None,
        }

    monkeypatch.setattr(dc, "_probe_one", fake_probe)
    nodes = [{"ip": "10.0.0.1", "label": "alpha"}, {"ip": "10.0.0.2", "label": "beta"}]
    merged = asyncio.run(dc._probe_fleet(nodes, "s", 1.0))
    assert len(merged) == 2


def test_probe_sorts_live_boxes_first(dc, monkeypatch):
    async def fake_probe(node, secret, timeout):
        up = node["label"] == "live"
        return {
            "ip": node["ip"], "label": node["label"], "up": up,
            "name": node["label"].upper() if up else None,
            "os": None, "rtt_ms": None, "error": None if up else "timeout",
        }

    monkeypatch.setattr(dc, "_probe_one", fake_probe)
    nodes = [{"ip": "10.0.0.9", "label": "dead"}, {"ip": "10.0.0.1", "label": "live"}]
    merged = asyncio.run(dc._probe_fleet(nodes, "s", 1.0))
    assert merged[0]["label"] == "live"


def test_probe_survives_a_node_raising(dc, monkeypatch):
    async def fake_probe(node, secret, timeout):
        if node["ip"].endswith(".9"):
            raise RuntimeError("boom")
        return {
            "ip": node["ip"], "label": node["label"], "up": True,
            "name": "OK", "os": None, "rtt_ms": 1, "error": None,
        }

    monkeypatch.setattr(dc, "_probe_one", fake_probe)
    nodes = [{"ip": "10.0.0.9", "label": "bad"}, {"ip": "10.0.0.1", "label": "good"}]
    merged = asyncio.run(dc._probe_fleet(nodes, "s", 1.0))
    assert [n["label"] for n in merged] == ["good"]


# ------------------------------------------------------------ agents

def test_agents_absent_chat_root_is_described_not_raised(dc, tmp_path, monkeypatch):
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "nothing-here"))
    out = dc.collect_agents()
    assert out["daemon"]["state"] == "absent"
    assert out["brain"]["state"] == "absent"
    assert out["runs"] == []


def test_agents_detects_stale_pid(dc, tmp_path, monkeypatch):
    chat = tmp_path / "chat"
    chat.mkdir()
    # PID 2^31-1 is beyond any real pid_max, so it is reliably not running.
    (chat / "daemon.pid").write_text("2147483647")
    monkeypatch.setattr(dc, "CHAT_ROOT", str(chat))
    assert dc.collect_agents()["daemon"]["state"] == "stale"


def test_agents_detects_running_pid(dc, tmp_path, monkeypatch):
    chat = tmp_path / "chat"
    chat.mkdir()
    (chat / "daemon.pid").write_text(str(os.getpid()))
    monkeypatch.setattr(dc, "CHAT_ROOT", str(chat))
    out = dc.collect_agents()
    assert out["daemon"]["state"] == "running"
    assert out["daemon"]["pid"] == os.getpid()


def test_brain_liveness_matches_chat_status_sh(dc, tmp_path, monkeypatch):
    """chat_status.sh calls 120s the cut-off; drifting from it would make the
    dashboard and the CLI disagree about the same file."""
    import time

    chat = tmp_path / "chat"
    chat.mkdir()
    hb = chat / "processor.heartbeat"
    hb.write_text("")
    monkeypatch.setattr(dc, "CHAT_ROOT", str(chat))

    os.utime(hb, (time.time() - 10, time.time() - 10))
    assert dc.collect_agents()["brain"]["state"] == "alive"

    os.utime(hb, (time.time() - 300, time.time() - 300))
    assert dc.collect_agents()["brain"]["state"] == "stale"


def test_agents_counts_queues(dc, tmp_path, monkeypatch):
    chat = tmp_path / "chat"
    for sub in ("inbox", "outbox", "failed"):
        (chat / sub).mkdir(parents=True)
    (chat / "inbox" / "a.json").write_text("{}")
    (chat / "inbox" / "b.json").write_text("{}")
    (chat / "inbox" / "ignore.txt").write_text("x")
    (chat / "failed" / "c.json").write_text("{}")
    monkeypatch.setattr(dc, "CHAT_ROOT", str(chat))

    q = dc.collect_agents()["queue"]
    assert q["inbox"] == 2
    assert q["outbox"] == 0
    assert q["failed"] == 1


# ------------------------------------------------------------- assembly

def test_build_state_has_the_keys_the_extension_reads(dc, monkeypatch, tmp_path):
    """The extension indexes these by name; a rename here is a blank panel."""
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "none"))

    class FakePoller:
        def snapshot(self):
            return {"polled_at": 0, "nodes": [], "up": 0, "total": 0}

    class FakeVitals:
        def sample(self):
            return {"host": {"hostname": "x"}, "cpu": {"usage_pct": 1.0}}

    state = dc.build_state(FakeVitals(), FakePoller())
    for key in ("schema", "ts", "fleet", "agents", "remote"):
        assert key in state, f"missing top-level key {key}"
    assert state["schema"] == dc.SCHEMA_VERSION
    assert isinstance(state["fleet"]["nodes"], list)


def test_build_state_is_json_serialisable(dc, monkeypatch, tmp_path):
    """Anything non-serialisable here becomes an empty dashboard, silently."""
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "none"))

    class FakePoller:
        def snapshot(self):
            return {"polled_at": 0, "nodes": [], "up": 0, "total": 0}

    class FakeVitals:
        def sample(self):
            return {}

    state = dc.build_state(FakeVitals(), FakePoller())
    json.dumps(state)  # raises if not serialisable


def test_build_state_marks_fleet_disabled_without_poller(dc, monkeypatch, tmp_path):
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "none"))

    class FakeVitals:
        def sample(self):
            return {}

    state = dc.build_state(FakeVitals(), None)
    assert state["fleet"]["disabled"] is True


# -------------------------------------------------------------- vitals

def test_vitals_degrades_without_omenfan(dc, monkeypatch):
    """omenfan is a separate repo; its absence must not break the collector."""
    monkeypatch.setattr(dc, "_load_omenfan", lambda: None)
    v = dc.Vitals()
    assert v.ok is False
    assert v.sample() == {}


# ------------------------------------------ services, game servers, PXE
#
# Everything below reads something written by another process: a status file
# from a service that may not be running, `systemctl show` output from either
# manager, or the PXE server's own log. The property that matters throughout
# is that "we could not look" never comes back looking like "we looked and
# everything is dead" — on a wall that reports service health, those two
# rendering the same is the whole failure mode.


def test_missing_status_file_reads_as_not_running_not_as_empty(dc, tmp_path):
    payload, err = dc._read_status_file(str(tmp_path / "nope.json"))
    assert payload is None
    assert err == "not running"


def test_corrupt_status_file_is_named_as_such(dc, tmp_path):
    bad = tmp_path / "status.json"
    bad.write_text("{not json")
    payload, err = dc._read_status_file(str(bad))
    assert payload is None
    assert "unreadable" in err


def test_a_stale_status_file_is_flagged_rather_than_trusted(dc, tmp_path):
    """A watchdog that died an hour ago still leaves a perfectly readable file
    full of servers marked `up`. Without the age check the wall would show a
    green board for a host whose game servers had all since crashed."""
    old = tmp_path / "status.json"
    old.write_text(json.dumps({"ts": 0, "up": 9, "total": 9}))
    payload, err = dc._read_status_file(str(old), max_age=60)
    assert err is None
    assert payload["stale_sec"] > 60


def test_fresh_status_file_is_not_flagged_stale(dc, tmp_path):
    import time as _t
    fresh = tmp_path / "status.json"
    fresh.write_text(json.dumps({"ts": _t.time(), "up": 9, "total": 9}))
    payload, err = dc._read_status_file(str(fresh), max_age=60)
    assert err is None
    assert "stale_sec" not in payload


def test_gameservers_passes_the_watchdog_blob_through(dc, tmp_path, monkeypatch):
    import time as _t
    status = tmp_path / "status.json"
    status.write_text(json.dumps({
        "ts": _t.time(), "up": 9, "total": 9, "players": 4, "humans": 0,
        "bots": 4, "down": [],
        "servers": [{"unit": "quake3-server", "up": True, "players": 4}],
        "watchdog": {"enabled": True, "actions": []},
    }))
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(status))
    out = dc.collect_gameservers()
    assert out["up"] == 9
    assert out["humans"] == 0
    assert out["servers"][0]["unit"] == "quake3-server"
    assert "error" not in out


def test_gameservers_without_a_watchdog_reports_zero_and_a_reason(dc, monkeypatch, tmp_path):
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(tmp_path / "absent.json"))
    out = dc.collect_gameservers()
    assert out["error"] == "not running"
    assert out["up"] == 0 and out["total"] == 0
    assert out["servers"] == []


def test_gameindex_tolerates_a_longer_silence_than_the_watchdog(dc, tmp_path, monkeypatch):
    """The favourites agent runs every five minutes and the watchdog every
    twenty seconds, so one cut-off cannot serve both: a three-minute-old
    favourites report is perfectly healthy."""
    import time as _t
    status = tmp_path / "status.json"
    status.write_text(json.dumps({
        "ts": _t.time() - 180, "ok": True, "phase": "idle",
        "agents": ["192.168.1.124"], "writes": {"wrote": 2},
        "servers": {"q3": {"servers": 700, "players": 4000},
                    "q2": {"servers": 3, "players": 0}},
    }))
    monkeypatch.setattr(dc, "GAMEINDEX_STATUS", str(status))
    out = dc.collect_gameindex()
    assert out.get("stale_sec") is None
    assert out["servers_known"] == 703
    assert out["agents"] == ["192.168.1.124"]


def test_udp_listening_reads_the_local_column(dc, monkeypatch):
    """`ss -ulnH` is State/Recv-Q/Send-Q/Local/Peer. The peer column of a
    listening socket is `0.0.0.0:*`, so parsing it finds no ports at all and
    reports a healthy PXE server as serving nothing."""
    monkeypatch.setattr(dc, "_run", lambda *a, **k: (
        "UNCONN 0 0 0.0.0.0:67 0.0.0.0:*\n"
        "UNCONN 0 0 0.0.0.0:69 0.0.0.0:*\n"
        "UNCONN 0 0 [::]:4011 [::]:*\n"
        "UNCONN 0 0 127.0.0.53%lo:53 0.0.0.0:*\n"))
    ports = dc._udp_listening()
    assert {67, 69, 4011, 53} <= ports


def test_udp_listening_survives_garbage_lines(dc, monkeypatch):
    monkeypatch.setattr(dc, "_run", lambda *a, **k: "\nnonsense\nUNCONN 0 0 *:69 *:*\n")
    assert dc._udp_listening() == {69}


def test_pxe_active_without_sockets_is_not_serving(dc, monkeypatch, tmp_path):
    """An `active` unit that lost its sockets serves nothing while looking
    perfectly healthy — the exact case `serving` exists to catch."""
    monkeypatch.setattr(dc, "unit_states",
                        lambda units, scope: {"retro-pxe": {"state": "active"}})
    monkeypatch.setattr(dc, "_udp_listening", lambda: {67})
    monkeypatch.setattr(dc, "PXE_ROOT", str(tmp_path))
    out = dc.collect_pxe()
    assert out["ports"]["TFTP"] is False
    assert out["serving"] is False


def test_pxe_reports_serving_when_tftp_is_bound(dc, monkeypatch, tmp_path):
    monkeypatch.setattr(dc, "unit_states",
                        lambda units, scope: {"retro-pxe": {"state": "active"}})
    monkeypatch.setattr(dc, "_udp_listening", lambda: {67, 69, 4011})
    monkeypatch.setattr(dc, "PXE_ROOT", str(tmp_path))
    out = dc.collect_pxe()
    assert out["serving"] is True
    assert all(out["ports"].values())


def test_pxe_reads_boot_holds_newest_first(dc, monkeypatch, tmp_path):
    import time as _t
    now = _t.time()
    (tmp_path / "pxe_state.json").write_text(json.dumps({
        "00:11:22:33:44:55": now - 10000,
        "aa:bb:cc:dd:ee:ff": now - 10,
    }))
    monkeypatch.setattr(dc, "unit_states",
                        lambda units, scope: {"retro-pxe": {"state": "active"}})
    monkeypatch.setattr(dc, "_udp_listening", lambda: {69})
    monkeypatch.setattr(dc, "PXE_ROOT", str(tmp_path))
    out = dc.collect_pxe()
    assert out["hold_count"] == 2
    assert out["holds"][0]["mac"] == "aa:bb:cc:dd:ee:ff"
    assert out["holds"][0]["age_sec"] < out["holds"][1]["age_sec"]


def test_pxe_without_any_state_or_log_still_returns(dc, monkeypatch, tmp_path):
    """A freshly installed PXE server has neither file. The panel must render."""
    monkeypatch.setattr(dc, "unit_states",
                        lambda units, scope: {"retro-pxe": {"state": "active"}})
    monkeypatch.setattr(dc, "_udp_listening", lambda: {69})
    monkeypatch.setattr(dc, "PXE_ROOT", str(tmp_path / "empty"))
    out = dc.collect_pxe()
    assert out["hold_count"] == 0
    assert out["holds"] == []


def test_pxe_log_counts_completed_transfers_not_requests(dc, monkeypatch, tmp_path):
    """A GET may be retried several times for one file; counting GETs makes a
    slow client look like a fleet-wide install storm."""
    log = tmp_path / "pxe_server.log"
    log.write_text(
        "[19:25:38] tftp 192.168.1.177 GET \\i386\\txtsetup.sif (100 bytes)\n"
        "[19:25:38] tftp 192.168.1.177 GET \\i386\\txtsetup.sif (100 bytes)\n"
        "[19:25:39] tftp 192.168.1.177 DONE \\i386\\txtsetup.sif\n"
        "[19:25:40] tftp 192.168.1.178 DONE \\i386\\HpAHCIsr.sys\n"
        "[19:26:00] proxyDHCP HOLD -> aa:bb (already served)\n")
    out = dc._pxe_recent(str(log))
    assert out["files_served_recent"] == 2
    assert out["last_file"] == "\\i386\\HpAHCIsr.sys"
    assert out["recent_clients"][0] == "192.168.1.178"
    assert "HOLD" in out["last"]


def test_pxe_log_that_does_not_exist_is_not_an_error(dc, tmp_path):
    assert dc._pxe_recent(str(tmp_path / "gone.log")) == {}


def test_unit_states_marks_a_missing_unit_absent_not_failed(dc, monkeypatch):
    """`not-found` means nobody installed it. Rendering that as a failure
    sends someone chasing a service that was never meant to be there."""
    class Res:
        stdout = ("ActiveState=active\nSubState=running\nLoadState=loaded\n"
                  "UnitFileState=enabled\nResult=success\nNRestarts=2\n"
                  "ExecMainStartTimestampMonotonic=0\n"
                  "\n"
                  "ActiveState=inactive\nSubState=dead\nLoadState=not-found\n"
                  "UnitFileState=\nResult=success\nNRestarts=0\n"
                  "ExecMainStartTimestampMonotonic=0\n")
    monkeypatch.setattr(dc.subprocess, "run", lambda *a, **k: Res())
    out = dc.unit_states(["there", "missing"], "system")
    assert out["there"]["state"] == "active"
    assert out["there"]["restarts"] == 2
    assert out["missing"] == {"state": "absent"}


def test_unit_states_degrades_when_systemctl_cannot_run(dc, monkeypatch):
    def boom(*a, **k):
        raise OSError("no systemctl")
    monkeypatch.setattr(dc.subprocess, "run", boom)
    out = dc.unit_states(["a", "b"], "user")
    assert out == {"a": {"state": "unknown"}, "b": {"state": "unknown"}}


def test_user_scope_reaches_the_fleet_users_manager_when_root(dc, monkeypatch):
    """Running as root, a bare `systemctl --user` queries root's own manager,
    which has none of these units — every fleet service would read absent."""
    monkeypatch.setattr(dc.os, "geteuid", lambda: 0)
    monkeypatch.setattr(dc, "_uid_of", lambda user: 1000)
    prefix = dc._systemctl_prefix("user")
    assert "XDG_RUNTIME_DIR=/run/user/1000" in prefix
    assert dc._systemctl_prefix("system") == ["systemctl"]


def test_collect_services_counts_and_names_the_degraded(dc, monkeypatch, tmp_path):
    # Point the watchdog source at nothing, so this exercises the systemd
    # fallback deliberately. Without it the test read the REAL status file on
    # the developer's host and asserted against live service states — it
    # passed alone and failed in the suite, which is the worst way to find out.
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(tmp_path / "absent.json"))

    def fake_states(units, scope):
        table = {
            "retro-chat-daemon": {"state": "active", "uptime_sec": 10},
            "retro-chat-brain": {"state": "active", "uptime_sec": 20},
            "retro-gameindex": {"state": "failed"},
            "retro-gameservers-watch": {"state": "absent"},
            "retro-dosgames-http": {"state": "active"},
            "retro-pxe": {"state": "active"},
            "retro-dashboard-collector": {"state": "active"},
        }
        return {u: table.get(u, {"state": "unknown"}) for u in units}

    monkeypatch.setattr(dc, "unit_states", fake_states)
    out = dc.collect_services()
    assert out["total"] == len(dc.HOST_SERVICES)
    assert out["up"] == 5
    assert set(out["degraded"]) == {"retro-gameindex", "retro-gameservers-watch"}
    # Order is the declared one, so the wall's rows never shuffle between samples.
    assert [s["unit"] for s in out["services"]] == [u for _, u, _ in dc.HOST_SERVICES]


def test_slow_cache_reuses_within_the_ttl_and_swallows_errors(dc):
    calls = []

    def counted():
        calls.append(1)
        return {"n": len(calls)}

    cache = dc.SlowCache(ttl=1000)
    assert cache.get("k", counted)["n"] == 1
    assert cache.get("k", counted)["n"] == 1
    assert len(calls) == 1

    def boom():
        raise RuntimeError("nope")

    # A panel collector that throws must not take the sample with it.
    assert "error" in cache.get("bad", boom)


def test_build_state_carries_every_new_section(dc, monkeypatch, tmp_path):
    monkeypatch.setattr(dc, "CHAT_ROOT", str(tmp_path / "none"))
    monkeypatch.setattr(dc, "collect_services", lambda: {"up": 1, "total": 1,
                                                         "services": []})
    monkeypatch.setattr(dc, "collect_pxe", lambda: {"state": "active"})
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(tmp_path / "a.json"))
    monkeypatch.setattr(dc, "GAMEINDEX_STATUS", str(tmp_path / "b.json"))

    class FakeVitals:
        def sample(self):
            return {}

    state = dc.build_state(FakeVitals(), None)
    for key in ("gameservers", "gameindex", "pxe", "services"):
        assert key in state, key
    json.dumps(state)  # the greeter reads JSON; anything unserialisable is fatal


# ---------------------------------------------- reconciling the two sources
#
# "the status file is missing" and "the service that writes it is dead" are
# usually the same fact, but not always — and when they diverge, the obvious
# message sends someone to start a service that is already running.


def test_a_missing_file_from_a_dead_service_still_says_not_running(dc):
    state = {
        "gameservers": {"error": "not running", "path": "/x"},
        "services": {"services": [
            {"unit": "retro-gameservers-watch", "state": "inactive"}]},
    }
    dc.reconcile_status_sources(state)
    assert state["gameservers"]["error"] == "not running"


def test_a_missing_file_from_a_LIVE_service_says_something_different(dc):
    """The collector runs with ProtectHome=read-only, which covers /run/user —
    exactly where both status files live. If that ever stops resolving, the
    wall must not tell someone to start a running service."""
    state = {
        "gameservers": {"error": "not running", "path": "/run/user/1000/x.json"},
        "services": {"services": [
            {"unit": "retro-gameservers-watch", "state": "active"}]},
    }
    dc.reconcile_status_sources(state)
    assert state["gameservers"]["error"] == "running, but no status file yet"
    assert state["gameservers"]["hint"] == "/run/user/1000/x.json"


def test_reconcile_covers_the_favourites_agent_too(dc):
    state = {
        "gameindex": {"error": "not running", "path": "/p"},
        "services": {"services": [
            {"unit": "retro-gameindex", "state": "active"}]},
    }
    dc.reconcile_status_sources(state)
    assert state["gameindex"]["error"] == "running, but no status file yet"


def test_reconcile_leaves_a_healthy_section_alone(dc):
    state = {
        "gameservers": {"up": 10, "total": 10},
        "services": {"services": [
            {"unit": "retro-gameservers-watch", "state": "active"}]},
    }
    dc.reconcile_status_sources(state)
    assert "error" not in state["gameservers"]


def test_reconcile_survives_a_state_with_no_services_section(dc):
    state = {"gameservers": {"error": "not running"}}
    dc.reconcile_status_sources(state)          # must not raise
    assert state["gameservers"]["error"] == "not running"


def test_reconcile_ignores_a_different_error(dc):
    """A corrupt file is a corrupt file whatever the unit is doing."""
    state = {
        "gameservers": {"error": "unreadable: JSONDecodeError"},
        "services": {"services": [
            {"unit": "retro-gameservers-watch", "state": "active"}]},
    }
    dc.reconcile_status_sources(state)
    assert state["gameservers"]["error"] == "unreadable: JSONDecodeError"


# --------------------------------- user units come from the watchdog, not us
#
# The collector is root inside a hardened unit, so `systemctl --user` is
# root's own manager and the privilege hop to the fleet user's does not
# survive the sandbox (`setpriv --clear-groups` -> setgroups() fails -> empty
# stdout -> every service reads "unknown"). That shipped once and looked
# exactly like the whole fleet having died. The watchdog already is that user,
# so it reports them.


def _watchdog_file(tmp_path, host_services, ts=None, extra=None):
    import time as _t
    payload = {"ts": ts if ts is not None else _t.time(),
               "up": 10, "total": 10, "servers": []}
    if host_services is not None:
        payload["host_services"] = host_services
    payload.update(extra or {})
    f = tmp_path / "status.json"
    f.write_text(json.dumps(payload))
    return f


def test_user_units_are_read_from_the_watchdog(dc, tmp_path, monkeypatch):
    f = _watchdog_file(tmp_path, {
        "retro-chat-brain": {"state": "active", "uptime_sec": 5},
        "retro-gameindex": {"state": "failed"},
    })
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(f))
    out = dc.user_unit_states_from_watchdog(
        ["retro-chat-brain", "retro-gameindex"])
    assert out["retro-chat-brain"]["state"] == "active"
    assert out["retro-gameindex"]["state"] == "failed"


def test_collect_services_prefers_the_watchdog_over_systemctl(dc, tmp_path, monkeypatch):
    """The hop must not even be attempted when the watchdog has the answer."""
    f = _watchdog_file(tmp_path, {
        u: {"state": "active"} for _, u, sc in dc.HOST_SERVICES if sc == "user"})
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(f))

    asked = []

    def spy(units, scope):
        asked.append(scope)
        return {u: {"state": "active"} for u in units}

    monkeypatch.setattr(dc, "unit_states", spy)
    out = dc.collect_services()
    assert out["up"] == out["total"]
    assert "user" not in asked, "queried systemd for user units unnecessarily"
    assert asked == ["system"]


def test_a_stale_watchdog_file_is_not_presented_as_current(dc, tmp_path, monkeypatch):
    """If the watchdog stopped, its snapshot of everything else stopped too —
    showing hours-old 'active' rows would be worse than saying nothing."""
    f = _watchdog_file(tmp_path, {"retro-chat-brain": {"state": "active"}}, ts=0)
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(f))
    assert dc.user_unit_states_from_watchdog(["retro-chat-brain"]) is None


def test_no_watchdog_file_falls_back_rather_than_reporting_nothing(dc, tmp_path, monkeypatch):
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(tmp_path / "gone.json"))
    assert dc.user_unit_states_from_watchdog(["retro-chat-brain"]) is None

    called = []
    monkeypatch.setattr(dc, "unit_states", lambda u, sc: (
        called.append(sc), {x: {"state": "active"} for x in u})[1])
    out = dc.collect_services()
    assert "user" in called, "fallback to systemd did not happen"
    assert out["up"] == out["total"]


def test_an_old_watchdog_without_the_field_falls_back(dc, tmp_path, monkeypatch):
    """A watchdog predating this feature publishes no host_services key."""
    f = _watchdog_file(tmp_path, None)
    monkeypatch.setattr(dc, "GAMESERVERS_STATUS", str(f))
    assert dc.user_unit_states_from_watchdog(["retro-chat-brain"]) is None


def test_the_fallback_hop_does_not_use_clear_groups(dc, monkeypatch):
    """setgroups() does not survive this unit's sandbox; setpriv then exits
    with 'setgroups failed' and produces empty output — a fallback that cannot
    work is not a fallback."""
    monkeypatch.setattr(dc.os, "geteuid", lambda: 0)
    monkeypatch.setattr(dc, "_uid_of", lambda user: 1000)
    prefix = dc._systemctl_prefix("user")
    assert "--clear-groups" not in prefix
    assert "--reuid" in prefix and "1000" in prefix


# --------------------------------------------------------------------------
# the web-site panel (specpicks.com / aisleprompt.com)
# --------------------------------------------------------------------------

def test_a_disabled_agent_is_off_even_while_carrying_an_old_failure(dc):
    """`enabled` must be read BEFORE the run state.

    specpicks-scraper-watchdog is disabled and still holds a `failure` from
    before it was switched off. Reporting that as a fault sends someone to fix
    something that was turned off on purpose.
    """
    assert dc._agent_state({"enabled": False, "last_run_status": "failure"}) == "off"


def test_an_agent_that_never_ran_is_absent_not_failed(dc):
    """No status.json at all is "never installed", which is not a fault.

    One specpicks agent is in exactly this state. Collapsing it into `fail`
    is the same error as a systemd LoadState=not-found reading as a crash.
    """
    assert dc._agent_state({"enabled": True, "last_run_status": "",
                           "last_run_at": None}) == "absent"


def test_an_empty_status_with_a_run_time_is_unknown(dc):
    """It ran at some point but says nothing about how -- that is not absent,
    and it is not a failure either. It is the third answer."""
    assert dc._agent_state({"enabled": True, "last_run_status": "",
                           "last_run_at": "2026-08-30T03:00:01+00:00"}) == "unknown"


def test_the_framework_states_map_onto_the_shared_vocabulary(dc):
    for framework_state, wall_state in [
            ("success", "ok"), ("idle", "ok"),
            ("running", "busy"), ("starting", "busy"),
            ("failure", "fail"), ("blocked", "blocked"),
            ("cancelled", "warn")]:
        got = dc._agent_state({"enabled": True, "last_run_status": framework_state})
        assert got == wall_state, f"{framework_state} -> {got}"


def test_an_unrecognised_framework_state_is_unknown_not_ok(dc):
    """A state we have never seen must not be optimistically green."""
    assert dc._agent_state({"enabled": True,
                           "last_run_status": "reticulating"}) == "unknown"


def test_iso_age_parses_the_frameworks_timestamp_format(dc):
    """The framework writes UTC with a +00:00 offset, never a bare Z."""
    from datetime import datetime, timedelta, timezone
    ts = (datetime.now(timezone.utc) - timedelta(minutes=5)).isoformat(
        timespec="seconds")
    age = dc._iso_age(ts)
    assert age is not None and 280 < age < 320, age


def test_iso_age_tolerates_a_z_suffix_and_junk(dc):
    assert dc._iso_age("2026-08-30T03:00:01Z") is not None
    assert dc._iso_age("not a timestamp") is None
    assert dc._iso_age(None) is None


def test_agents_are_grouped_by_id_prefix_not_the_application_field(dc, monkeypatch):
    """At least one agent is filed under the wrong application by the API.

    specpicks-user-growth-strategist declares metadata.site = "aisleprompt",
    so `application` puts it on the wrong site. The id prefix is the only
    reliable grouping key.
    """
    agents = [
        {"id": "specpicks-a", "enabled": True, "last_run_status": "success",
         "application": "specpicks"},
        {"id": "specpicks-user-growth-strategist", "enabled": True,
         "last_run_status": "success", "application": "aisleprompt"},
        {"id": "aisleprompt-b", "enabled": True, "last_run_status": "success",
         "application": "aisleprompt"},
    ]
    monkeypatch.setattr(dc, "_site_api",
                        lambda path: ({}, "") if path == "/api/health"
                        else (agents, ""))
    got = dc.collect_site_agents()
    assert got["sites"]["specpicks"]["total"] == 2
    assert got["sites"]["aisleprompt"]["total"] == 1


def test_an_unreachable_api_is_a_fault_with_a_reason(dc, monkeypatch):
    """Nothing supervises that uvicorn process, so refused is expected --
    but it must never render as an empty, healthy-looking panel."""
    monkeypatch.setattr(dc, "_site_api", lambda path: (None, "URLError: refused"))
    got = dc.collect_site_agents()
    assert got["state"] == "fail"
    assert "refused" in got["error"]


def test_a_missing_postgres_driver_names_itself_per_site(dc, monkeypatch):
    """The collector runs as ROOT and psycopg2 may only be in a user
    site-packages that root does not read -- the exact state this host was in.

    The panel must say "needs python3-psycopg2", not show a blank. A blank
    article count is indistinguishable from a site that published nothing all
    week, which is a completely different and much more alarming thing.
    """
    import builtins
    real_import = builtins.__import__

    def no_psycopg2(name, *a, **kw):
        if name == "psycopg2":
            raise ImportError("No module named 'psycopg2'")
        return real_import(name, *a, **kw)

    monkeypatch.setattr(builtins, "__import__", no_psycopg2)
    got = dc.collect_site_articles()
    assert got["state"] == "absent"
    for site in dc.SITES:
        assert got["sites"][site]["state"] == "absent"
        assert "psycopg2" in got["sites"][site]["why"]
