"""The host gate measures the disk floor on the box's GAMES volume (2026-10-01).

GAMESYNC writes to HKLM\\Software\\RetroAgent\\GamesDir when it is set (agent
1.93.0; .243 uses E:\\GAMES), and the agent's own gate measures that volume.
The host planner measured C: regardless, so .243's published file said
"not enough free disk (have 433 MB, needs 620)" for Die by the Sword while E:
had 64 GB free - and the agent obeys a published "no" over its own rule.
"""
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts"))
sys.path.insert(0, REPO)
from gamegate import rules            # noqa: E402
from gamegate import gamegate         # noqa: E402

DISKS = [{"root": "C:\\", "free_mb": 449, "total_mb": 1220},
         {"root": "D:\\", "free_mb": 3693, "total_mb": 4016},
         {"root": "E:\\", "free_mb": 64268, "total_mb": 72277}]


def _profile(**extra):
    data = {"hostname": "N5R5L9", "profile_hash": "b9a3b8c66827032c", "disk": DISKS}
    data.update(extra)
    return rules.Profile.from_hwprofile(data, ip="192.168.1.243")


def test_the_games_volume_is_measured():
    assert _profile(games_dir="E:\\GAMES").free_mb == 64268      # .243 (fixed)
    assert _profile().free_mb == 449                              # C:\Games - unchanged


def test_a_malformed_games_dir_falls_back_to_c():
    assert _profile(games_dir="GAMES").free_mb == 449
    assert _profile(games_dir="\\\\server\\share").free_mb == 449


def test_the_fetch_folds_games_dir_into_the_profile_text():
    text = json.dumps({"profile_hash": "x", "disk": DISKS})
    out = json.loads(gamegate._with_games_dir(text, "E:\\GAMES"))
    assert out["games_dir"] == "E:\\GAMES" and out["disk"] == DISKS
    assert gamegate._with_games_dir(text, "") == text            # no value: untouched
    assert gamegate._with_games_dir("not json", "E:\\GAMES") == "not json"


def test_a_published_no_for_disk_reads_the_games_volume():
    p = _profile(games_dir="E:\\GAMES")
    req = rules.Requirements(disk_mb=1800)
    d = rules.decide(p, req)
    assert "disk" not in (getattr(d, "limiting", "") or ""), d
