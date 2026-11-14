"""Quake II public servers: game DIRECTORY decides joinability, and seeds
reach the address book while alive (2026-11-13: the fleet's Q2 list held only
.132 because neither configured master answered)."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from scripts.gameindex import masters as m
from scripts.gameindex import favorites as f


def test_gamedir_not_gamename():
    assert m._q2_gamedir({"gamename": "OSP Tourney DM v(2.75)", "gamedir": "tourney"}) == "tourney"
    assert m._q2_gamedir({"gamename": "OpenFFA", "game": "OpenFFA"}) == "openffa"
    assert m._q2_gamedir({"gamename": "baseq2"}) == "baseq2"   # no dir key = stock


def test_q2_titles_are_strict_and_refuse_content_mods():
    for t in ("quake2", "q2pro", "yquake2"):
        pol = f.TITLES[t]
        assert pol.get("strict_accepts") is True, t
        assert "baseq2" in pol["accepts"] and "action" not in pol["accepts"], t


def test_seeds_are_marked_seed_and_listed_first(monkeypatch):
    monkeypatch.setattr(m, "_collect", lambda *a, **k: ["1.2.3.4:27910", m.Q2_SEEDS[0]])
    lst = m._q2_list()
    assert lst[:len(m.Q2_SEEDS)] == m.Q2_SEEDS and lst.count(m.Q2_SEEDS[0]) == 1
    monkeypatch.setattr(m, "_q2_probe", lambda a: {"addr": a, "source": "q2master"})
    assert m._q2_any_probe(m.Q2_SEEDS[0])["source"] == "seed"
    assert m._q2_any_probe("1.2.3.4:27910")["source"] == "q2master"
    assert len(m.Q2_SEEDS) >= 5
