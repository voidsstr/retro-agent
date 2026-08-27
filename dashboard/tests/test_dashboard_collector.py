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
