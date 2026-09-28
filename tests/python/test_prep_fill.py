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

import pytest
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


def test_fill_bat_sets_tz_and_fails_only_on_an_unzip_error():
    """The staged DJGPP UnZip 6.00 exits 1 (Info-ZIP's WARNING) on EVERY run
    when TZ is unset - 'TZ environment variable not found, cannot use UTC
    times!!' - even after a perfect extract. The first .243 fill (2026-09-27)
    logged every title FAIL that way, ARKAN2E6's 11 files sitting right there.
    Reproduced in DOSBox 2026-09-28; `set TZ` makes a good extract return 0."""
    bat = pf.fill_bat([{"stem": "ABCDE123", "files": 3, "bytes": 10}], "W:\\FILES\\GAMES\\DOSFILL", "D:\\GAMES")
    lines = bat.split("\r\n")
    tz = lines.index("set TZ=" + pf.FILL_TZ)
    unzip = next(i for i, l in enumerate(lines) if "UNZIP.EXE" in l)
    assert tz < unzip, "TZ must be set before the first UNZIP"
    assert "if errorlevel 2 echo FAIL ABCDE123>> D:\\GAMES\\FILL.LOG" in lines
    assert "if errorlevel 2 goto done_ABCDE123" in lines
    assert "if errorlevel 1 echo FAIL ABCDE123>> D:\\GAMES\\FILL.LOG" not in lines, \
        "errorlevel 1 is a warning with the files extracted, not a failure"
    assert any(l.startswith("if errorlevel 1 echo WARN ABCDE123") for l in lines), "a warning is still visible"


def test_bat_only_rebuilds_from_the_manifest(tmp_path, monkeypatch):
    man = tmp_path / "M.TXT"
    man.write_text("ABCDE123|Some Game|GAME.EXE|3|10|/src/a.zip\nXYZ00001|Other|RUN.BAT|5|99|/src/b.zip\n")
    monkeypatch.setattr(pf.sys, "argv", ["prep_fill.py", "--out", str(tmp_path / "o"), "--bat-only", str(man)])
    pf.main()
    bat = (tmp_path / "o" / "FILL.BAT").read_bytes().decode()
    assert "echo 3 10> D:\\GAMES\\ABCDE123\\FILL.OK" in bat and "echo 5 99> D:\\GAMES\\XYZ00001\\FILL.OK" in bat


def test_the_staged_unzip_needs_tz_in_dosbox(tmp_path):
    """The real UNZIP.EXE, in DOSBox, when both are available: without TZ a
    perfect extract is errorlevel 1; with the batch's TZ it is 0."""
    import shutil, subprocess, os
    unz = os.environ.get("DOSFILL_UNZIP", "")
    dpmi = os.path.join(os.path.dirname(unz), "CWSDPMI.EXE")
    if not (unz and shutil.which("dosbox") and os.path.exists(unz) and os.path.exists(dpmi)):
        pytest.skip("SKIPPED LOUDLY: no dosbox or no staged DJGPP UNZIP.EXE+CWSDPMI.EXE (DOSFILL_UNZIP=...) - "
                    "the TZ errorlevel behaviour is not re-measured here")
    shutil.copy(unz, tmp_path / "UNZIP.EXE")
    shutil.copy(dpmi, tmp_path / "CWSDPMI.EXE")
    z = zipfile.ZipFile(tmp_path / "T.ZIP", "w", zipfile.ZIP_DEFLATED)
    z.writestr("A.TXT", b"hello\r\n")
    z.close()
    (tmp_path / "T.BAT").write_bytes((
        "@echo off\r\nUNZIP.EXE -qq -o T.ZIP -d O1\r\nif errorlevel 1 echo NOTZ1> R.TXT\r\n"
        "set TZ=%s\r\nUNZIP.EXE -qq -o T.ZIP -d O2\r\nif not errorlevel 1 echo TZ0>> R.TXT\r\n" % pf.FILL_TZ).encode())
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    subprocess.run(["timeout", "60", "dosbox", "-c", "mount c %s" % tmp_path, "-c", "c:", "-c", "T.BAT",
                    "-c", "exit"], env=env, capture_output=True)
    r = (tmp_path / "R.TXT").read_text().split()
    assert r == ["NOTZ1", "TZ0"], r
