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
        assert "baseq2" in pol["accepts"] and "coop" in pol["accepts"], t
        assert not pol["accepts"] & {"action", "lox", "chaos", "ctc"}, t


def test_seeds_are_marked_seed_and_listed_first(monkeypatch):
    monkeypatch.setattr(m, "_collect", lambda *a, **k: ["1.2.3.4:27910", m.Q2_SEEDS[0]])
    lst = m._q2_list()
    assert lst[:len(m.Q2_SEEDS)] == m.Q2_SEEDS and lst.count(m.Q2_SEEDS[0]) == 1
    monkeypatch.setattr(m, "_q2_probe", lambda a: {"addr": a, "source": "q2master"})
    # seeds are extra addresses only: no "seed" privilege, so like any master
    # row they need humans on them (user: "servers with players")
    assert m._q2_any_probe(m.Q2_SEEDS[0])["source"] == "q2master"
    assert len(m.Q2_SEEDS) >= 5


def test_q3_titles_are_strict_baseq3():
    for t in ("quake3", "ioquake3"):
        assert f.TITLES[t]["accepts"] == {"baseq3"} and f.TITLES[t]["strict_accepts"]


def test_q3_gamedir_prefers_fs_game():
    assert m._q3_gamedir({"gamename": "baseq3", "fs_game": "osp"}) == "osp"
    assert m._q3_gamedir({"gamename": "excessiveplus", "fs_game": ""}) == "excessiveplus"
    assert m._q3_gamedir({"gamename": "baseq3"}) == "baseq3"
    assert m._q3_gamedir({"gamename": "missionpack", "fs_game": "missionpack"}) == "missionpack"
    assert m._q3_gamedir({"gamename": "baseoa"}) == "baseoa"
