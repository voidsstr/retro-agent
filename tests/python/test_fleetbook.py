"""Regression: retro_fleetbook.py — the fleet's persistent solved-problems DB.

Locks in the contract the chat brain relies on (2026-08-03): add/search/show
round-trip, FTS matching with special characters, log-with-recipe bumping
usage counters, per-host history, and slug collision handling.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FB_PATH = os.path.abspath(os.path.join(HERE, "..", "..", "scripts",
                                       "retro_fleetbook.py"))


def _fb(tmp_path):
    os.environ["RETRO_FLEETBOOK_DB"] = str(tmp_path / "fb.db")
    spec = importlib.util.spec_from_file_location("retro_fleetbook", FB_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    # module reads the env var at import time
    assert mod.DB_PATH == str(tmp_path / "fb.db")
    return mod


def test_add_search_show_roundtrip(tmp_path, capsys):
    fb = _fb(tmp_path)
    fb.main(["add", "--title", "Win98 vcache protection error",
             "--problem", "boot crash with >512MB RAM",
             "--recipe", "SYSFIX apply", "--tags", "win98,vcache"])
    capsys.readouterr()
    fb.main(["search", "protection", "vcache"])
    out = capsys.readouterr().out
    assert "win98-vcache-protection-error" in out
    fb.main(["show", "win98-vcache-protection-error"])
    out = capsys.readouterr().out
    assert "SYSFIX apply" in out and "boot crash" in out


def test_search_special_chars_no_fts_error(tmp_path, capsys):
    fb = _fb(tmp_path)
    fb.main(["add", "--title", "t", "--problem", "p", "--recipe", "r"])
    capsys.readouterr()
    # quotes/operators must not raise an FTS5 syntax error
    fb.main(["search", 'what"s', "up?", "AND", "(NOT)"])
    assert True  # reaching here = no sqlite3.OperationalError


def test_log_with_recipe_bumps_usage(tmp_path, capsys):
    fb = _fb(tmp_path)
    fb.main(["add", "--title", "Fix X", "--problem", "p", "--recipe", "steps"])
    capsys.readouterr()
    fb.main(["log", "--host", "192.168.1.99", "--summary", "applied X",
             "--recipe", "fix-x"])
    capsys.readouterr()
    fb.main(["show", "fix-x"])
    out = capsys.readouterr().out
    assert "used 1x" in out
    assert "192.168.1.99" in out          # linked application shown


def test_history_filters_by_host(tmp_path, capsys):
    fb = _fb(tmp_path)
    fb.main(["log", "--host", "10.0.0.1", "--summary", "alpha change"])
    fb.main(["log", "--host", "10.0.0.2", "--summary", "beta change"])
    capsys.readouterr()
    fb.main(["history", "--host", "10.0.0.2"])
    out = capsys.readouterr().out
    assert "beta change" in out and "alpha change" not in out


def test_slug_collision_gets_suffix(tmp_path, capsys):
    fb = _fb(tmp_path)
    fb.main(["add", "--title", "Same Name", "--problem", "p1", "--recipe", "r1"])
    fb.main(["add", "--title", "Same Name", "--problem", "p2", "--recipe", "r2"])
    out = capsys.readouterr().out
    assert "same-name" in out and "same-name-2" in out
