"""prep_fill.py - host-side packing for a bulk DOS-title fill (2026-09-27).

DOSGAME installs one title at a time (~30 s each on .243), too slow to fill a
disk, so titles are pre-packed on the host. Pinned:
  - each title lands in serve_dosgames.zip_stem(<zip name>), the directory
    DOSGAME would create itself;
  - a member that is not 8.3 REFUSES the title unless that title explicitly
    allows dropping named extras (Windows launchers, docs) - never silent;
  - DOSGAME.TXT names the launcher, with parentheses replaced (a ')' in a
    value closes a cmd block - CLAUDE.md);
  - FILL.BAT resumes (skips titles with FILL.OK) and logs failures.
"""
import importlib.util
import io
import json
import sys
import zipfile
from pathlib import Path

DG = Path(__file__).resolve().parents[2] / "scripts" / "dosgames"
sys.path.insert(0, str(DG))
spec = importlib.util.spec_from_file_location("prep_fill", DG / "prep_fill.py")
pf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pf)
from serve_dosgames import zip_stem  # noqa: E402


def _zip(tmp, name, members):
    p = tmp / name
    with zipfile.ZipFile(p, "w") as z:
        for m, data in members.items():
            z.writestr(m, data)
    return p


def test_title_lands_in_dosgames_own_stem_and_top_dir_is_stripped(tmp_path):
    src = _zip(tmp_path, "Some Game (1995)(Pub).zip", {"game/GAME.EXE": b"MZ", "game/DATA.DAT": b"x"})
    out = tmp_path / "out"; out.mkdir()
    r = pf.pack({"zip": str(src), "title": "Some Game (1995)", "launcher": "game.exe"}, str(out), b"OVL")
    assert r["stem"] == zip_stem("Some Game (1995)(Pub).zip")
    names = zipfile.ZipFile(out / (r["stem"] + ".ZIP")).namelist()
    assert "GAME.EXE" in names and "DATA.DAT" in names
    txt = zipfile.ZipFile(out / (r["stem"] + ".ZIP")).read("DOSGAME.TXT").decode()
    assert txt.startswith("GAME.EXE\t") and "(" not in txt and ")" not in txt


def test_non_83_member_refuses_unless_explicitly_dropped(tmp_path):
    src = _zip(tmp_path, "X.zip", {"GAME.EXE": b"MZ", "long name.txt": b"doc"})
    out = tmp_path / "out"; out.mkdir()
    r = pf.pack({"zip": str(src), "title": "X", "launcher": "GAME.EXE"}, str(out), b"")
    assert "refused" in r
    r = pf.pack({"zip": str(src), "title": "X", "launcher": "GAME.EXE", "drop_non83": True}, str(out), b"")
    assert r["dropped"] == ["long name.txt"]


def test_glide_titles_get_the_v2_overlay(tmp_path):
    src = _zip(tmp_path, "G.zip", {"GAME.EXE": b"MZ"})
    out = tmp_path / "out"; out.mkdir()
    r = pf.pack({"zip": str(src), "title": "G", "launcher": "G3D.BAT", "glide": True,
                 "batches": {"G3D.BAT": "@echo off\ngame\n"}}, str(out), b"V2OVL")
    z = zipfile.ZipFile(out / (r["stem"] + ".ZIP"))
    assert z.read("GLIDE2X.OVL") == b"V2OVL"
    assert z.read("G3D.BAT") == b"@echo off\r\ngame\r\n"


def test_fill_bat_resumes_and_logs():
    bat = pf.fill_bat([{"stem": "ABCDE123", "files": 3, "bytes": 10}], "W:\\FILES\\GAMES\\DOSFILL", "D:\\GAMES")
    assert "if exist D:\\GAMES\\ABCDE123\\FILL.OK goto done_ABCDE123" in bat
    assert "C:\\DOSGAME\\UNZIP.EXE -qq -o W:\\FILES\\GAMES\\DOSFILL\\ABCDE123.ZIP -d D:\\GAMES\\ABCDE123" in bat
    assert "FAIL ABCDE123" in bat and bat.endswith("\r\n")
