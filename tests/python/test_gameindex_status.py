"""Tests for the favourites agent's own health reporting.

The favourites agent (`scripts/gameindex/sync.py --daemon`) is now a
long-running service the login-screen wall reports on, so it has to answer a
question its logs never could: *did the last pass actually happen, and what
did it do?*

That matters because of an asymmetry specific to this fleet. The retro
machines are powered on demand, so a completely healthy pass across zero live
boxes writes nothing, changes nothing, and logs almost nothing — which from
outside is indistinguishable from a service that has stopped running. The
report therefore states plainly that a pass completed, when, and with what
result, rather than leaving the wall to infer health from output volume.

Run: pytest tests/python/test_gameindex_status.py
"""

import importlib.util
import json
import os
import sqlite3
import stat
import sys

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_GI_DIR = os.path.join(_HERE, "..", "..", "scripts", "gameindex")


def _load():
    path = os.path.join(_GI_DIR, "status.py")
    spec = importlib.util.spec_from_file_location("gameindex_status", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def status():
    return _load()


# --- publishing -------------------------------------------------------------

def test_publish_is_atomic_and_world_readable(status, tmp_path):
    """The reader is the dashboard collector running as root, and the write
    must never be observable half-finished."""
    target = tmp_path / "sub" / "status.json"
    status.publish({"ok": True, "phase": "idle"}, str(target))
    assert json.loads(target.read_text())["ok"] is True
    assert stat.S_IMODE(os.stat(target).st_mode) & stat.S_IROTH
    assert not list(target.parent.glob("*.tmp*")), "temp file left behind"


def test_publish_replaces_a_previous_report(status, tmp_path):
    target = tmp_path / "status.json"
    status.publish({"phase": "probing servers"}, str(target))
    status.publish({"phase": "idle"}, str(target))
    assert json.loads(target.read_text())["phase"] == "idle"


def test_default_path_is_in_the_runtime_dir_not_tmp(status, monkeypatch):
    """/tmp is invisible to the GDM greeter (systemd DynamicUser implies
    PrivateTmp), and this project has been bitten by that once already."""
    monkeypatch.setenv("XDG_RUNTIME_DIR", "/run/user/4242")
    path = status.default_status_path()
    assert path.startswith("/run/user/4242/")
    assert "/tmp/" not in path


def test_default_path_falls_back_without_xdg_runtime_dir(status, monkeypatch):
    monkeypatch.delenv("XDG_RUNTIME_DIR", raising=False)
    assert status.default_status_path().startswith("/run/user/")


# --- the report shape -------------------------------------------------------

def test_a_new_report_is_not_yet_ok(status):
    """A report published at the START of a pass must not claim success --
    the wall would show a green pass that had not finished, or one that later
    died mid-flight."""
    rep = status.new_report()
    assert rep["ok"] is False
    assert rep["phase"] == "starting"
    assert rep["ts"] is None
    assert rep["started_at"] > 0


def test_a_new_report_is_json_serialisable(status):
    json.dumps(status.new_report())


def test_write_buckets_start_at_zero_not_absent(status):
    """`0 written` and `we never got that far` must render differently, so the
    counters exist from the start rather than appearing when first used."""
    assert status.new_report()["writes"] == {
        "wrote": 0, "unchanged": 0, "skipped": 0, "failed": 0}


# --- DB summaries -----------------------------------------------------------

def _db():
    con = sqlite3.connect(":memory:")
    con.row_factory = sqlite3.Row
    con.execute("CREATE TABLE servers (engine TEXT, addr TEXT, players INT)")
    con.execute("CREATE TABLE favorites_state (ip TEXT, game_key TEXT, "
                "dir TEXT, hash TEXT, applied_at TEXT, detail TEXT)")
    return con


def test_summarize_servers_groups_by_engine(status):
    con = _db()
    con.executemany("INSERT INTO servers VALUES (?,?,?)", [
        ("q3", "a", 4), ("q3", "b", 2), ("q2", "c", 0)])
    out = status.summarize_servers(con)
    assert out["q3"] == {"servers": 2, "players": 6}
    assert out["q2"] == {"servers": 1, "players": 0}


def test_summarize_servers_handles_null_player_counts(status):
    """A server discovered from a master but not yet probed has NULL players;
    SUM() then returns None and a bare int() on it would raise mid-pass."""
    con = _db()
    con.execute("INSERT INTO servers VALUES ('q3', 'a', NULL)")
    assert status.summarize_servers(con)["q3"]["players"] == 0


def test_summarize_favorites_counts_files_and_boxes(status):
    con = _db()
    con.executemany("INSERT INTO favorites_state VALUES (?,?,?,?,?,?)", [
        ("192.168.1.124", "quake3", "d", "h1", "2026-08-28 19:00", "x"),
        ("192.168.1.124", "quake2", "d", "h2", "2026-08-28 19:00", "x"),
        ("192.168.1.143", "quake3", "d", "h3", "2026-08-28 19:01", "x"),
    ])
    out = status.summarize_favorites(con)
    assert out["files"] == 3
    assert out["boxes"] == 2
    assert out["last_write"] == "2026-08-28 19:01"


def test_summaries_of_an_empty_db_are_zero_not_an_exception(status):
    con = _db()
    assert status.summarize_servers(con) == {}
    assert status.summarize_favorites(con)["files"] == 0


def test_summaries_survive_a_db_without_the_tables(status):
    """A cold DB, or one from an older schema. A status reporter must never be
    the thing that takes the service down."""
    con = sqlite3.connect(":memory:")
    con.row_factory = sqlite3.Row
    assert status.summarize_servers(con) == {}
    assert status.summarize_favorites(con) == {
        "files": 0, "boxes": 0, "last_write": None}


# --- the daemon entry point exists and is wired ------------------------------

def test_sync_exposes_a_daemon_mode(status):
    """The unit runs `sync.py --daemon`; if the flag or the loop goes away the
    service starts, exits 0 immediately, and systemd restart-loops it."""
    src = open(os.path.join(_GI_DIR, "sync.py")).read()
    assert "--daemon" in src
    assert "def run_forever(" in src
    assert "status.publish(" in src


def test_the_unit_runs_the_daemon_and_not_a_oneshot():
    unit = open(os.path.join(_GI_DIR, "retro-gameindex.service")).read()
    assert "--daemon" in unit
    assert "Type=simple" in unit
    assert "Type=oneshot" not in unit
    # The timer it replaced must be gone, or systemd starts a second pass
    # every five minutes that fights the daemon over the same SQLite file.
    assert not os.path.exists(os.path.join(_GI_DIR, "retro-gameindex.timer"))
