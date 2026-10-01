"""scripts/fleet/stage-win9x.py - the Windows 9x lane of titles that also serve NT.

WHY (2026-10-01). .243 (Win98 SE, Pentium 166 non-MMX, Voodoo 2, Cirrus GD5436
with 1 MB, SB16 A220 I5 D1 H5) gets the same staged trees as the XP fleet, and
none of their cmd.exe launchers start under COMMAND.COM. Its launchers used to
be hand-written on the share with nothing in the repo to regenerate or check
them. These tests pin what was MEASURED on .243 and in the Win98 build VM:

* GLQuake over the 3dfx MiniGL runs at 640x480x16 and 800x600x16 and REFUSES
  512x384 ("Specified video mode not available") - so a Voodoo launcher may
  only ever ask for the two measured modes, at 16 bpp.
* DOS QUAKE.EXE stops at "MSCDEX not loaded ... press a key" without
  -nocdaudio and at "CENTER the joystick" without -nojoy, in a Win98 DOS box.
* A Win98 DOS box closes on exit only when its screen is empty: CLS after the
  game, not merely somewhere in the file.
* The Win98 shell stores the 8.3 alias of a shortcut's target in its PIF - the
  Win9x launchers are 8.3 names.
"""
import importlib.util
import io
import json
import os
import re
import sys
import zipfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FLEET = os.path.join(REPO, "scripts", "fleet")
sys.path.insert(0, FLEET)


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


lm = _load("libmeta", os.path.join(FLEET, "libmeta.py"))
ls = _load("libsource", os.path.join(FLEET, "libsource.py"))
sw9 = _load("stage_win9x", os.path.join(FLEET, "stage-win9x.py"))

LIB = "/mnt/retro-share/Files/Games-Library"
_share = pytest.mark.skipif(not os.path.isdir(LIB),
                            reason="staged library not mounted at %s - the share "
                                   "side was NOT checked" % LIB)
NAME83 = re.compile(r"^[A-Z0-9_\-]{1,8}\.(BAT|EXE|COM)$")


def _cmds(text):
    """Executed lines (no REM, no @echo off)."""
    return [l.strip() for l in text.replace("\r\n", "\n").split("\n")
            if l.strip() and not l.strip().lower().startswith(("rem", "@echo"))]


# --- the COMMAND.COM dialect check fires (a check that never fires is a lie) --

GOOD = "@echo off\r\nrem fine\r\nGAME.EXE -x\r\ncls\r\n"


def test_a_clean_command_com_launcher_passes():
    assert lm.command_com_problems(GOOD) == []


@pytest.mark.parametrize("bad,why", [
    ("@echo off\r\ncd /d X\r\n", "cd /d"),
    ('@echo off\r\nstart "" GAME.EXE\r\n', 'start "title"'),
    ("@echo off\r\ncall %~dp0X.BAT\r\n", "%~"),
    ("@echo off\r\nif exist X (\r\n", "parenthesis"),
    ("@echo off\r\nrem a -> b\r\n", "REM"),
    ("@echo off\r\nrem see %windir%\r\n", "REM"),
    ("@echo off\r\nsetlocal\r\n", "setlocal"),
    ("@echo off\r\nGAME.EXE || X\r\n", "||"),
    ("@echo off\r\nGAME.EXE 2>nul\r\n", "2>"),
    ("@echo off\nGAME.EXE\n", "CR"),
    ("@echo off\r\nrem " + "x" * 130 + "\r\n", "bytes"),
])
def test_the_command_com_check_rejects(bad, why):
    probs = lm.command_com_problems(bad)
    assert probs and any(why.lower() in p.lower() for p in probs), probs


def test_the_nt_only_region_is_exempt_and_must_close():
    """Quake's and Hexen II's Voodoo launchers serve .243 AND a Voodoo 4/5 XP
    box: the reg.exe lines sit behind `if not exist ...reg.exe goto novsa`, which
    COMMAND.COM jumps over without parsing (measured working on .243)."""
    body = ("@echo off\r\nif not exist %windir%\\system32\\reg.exe goto novsa\r\n"
            'reg query "HKLM\\X" 2>nul | find /i "A&B" >nul\r\n'
            "rem (NT) only\r\n:novsa\r\ncls\r\n")
    assert lm.command_com_problems(body) == []
    unclosed = body.replace(":novsa\r\n", "")
    assert any("never closed" in p for p in lm.command_com_problems(unclosed))


# --- launch.txt and requires.json are MERGED, never templated ------------------

LAUNCH = ("Play Quake.bat\tQuake\tGLQUAKE.EXE\r\n"
          "Play Quake - Voodoo.bat\tQuake - 3dfx Voodoo\tVOODOO\\GLQUAKE.EXE\r\n"
          "Other.bat\tOther\tX.ICO\r\n"
          "#\r\n# a comment\r\n")


def test_set_launch_rows_replaces_in_place_drops_and_appends_above_comments():
    out = lm.set_launch_rows(LAUNCH, [("Q1V640.BAT", "Quake - 3dfx Voodoo", "V.EXE"),
                                     ("other.BAT", "Other 2", "Y.ICO")],
                             drop=["Play Quake - Voodoo.bat"], note=["#", "# note 1"])
    lines = out.split("\r\n")
    assert lines[:3] == ["Play Quake.bat\tQuake\tGLQUAKE.EXE",
                         "other.BAT\tOther 2\tY.ICO",
                         "Q1V640.BAT\tQuake - 3dfx Voodoo\tV.EXE"], lines
    assert "# a comment" in lines and lines[-3:] == ["#", "# note 1", ""]
    assert lm.set_launch_rows(out, [("Q1V640.BAT", "Quake - 3dfx Voodoo", "V.EXE"),
                                    ("other.BAT", "Other 2", "Y.ICO")],
                              drop=["Play Quake - Voodoo.bat"],
                              note=["#", "# note 1"]) == out, "not idempotent"


def test_launch_problems_catch_what_loses_a_shortcut():
    assert lm.launch_problems(LAUNCH) == []
    assert any("1023" in p for p in lm.launch_problems("#" * 1100 + "\r\nA.BAT\tA\tA.ICO\r\n"))
    assert lm.launch_problems("A (x).BAT\tA\tI.ICO\r\n")
    assert lm.launch_problems("A.BAT\tA: B\tI.ICO\r\n")
    assert lm.launch_problems("A.BAT\tA\r\n"), "no icon field"


REQ = json.dumps({"requirements_version": 3, "title": "T", "notes": "old.",
                  "shortcuts": {"Play T.bat": {"min_os": "win2k", "notes": "keep"},
                                "Gone.bat": {"max_os": "win9x"}}}, indent=2) + "\n"


def test_merge_requires_touches_only_what_it_is_told():
    out = lm.merge_requires(REQ, version=4, set_shortcuts={"NEW.BAT": {"max_os": "win9x"}},
                            patch_shortcuts={"play t.bat": {"cpu_features": ["cmov"]}},
                            drop_shortcuts=["gone.bat"], notes_add="New sentence.")
    doc = json.loads(out)
    assert doc["requirements_version"] == 4
    assert doc["shortcuts"] == {"Play T.bat": {"min_os": "win2k", "notes": "keep",
                                               "cpu_features": ["cmov"]},
                                "NEW.BAT": {"max_os": "win9x"}}
    assert doc["notes"] == "old. New sentence."
    again = lm.merge_requires(out, version=4, set_shortcuts={"NEW.BAT": {"max_os": "win9x"}},
                              patch_shortcuts={"play t.bat": {"cpu_features": ["cmov"]}},
                              drop_shortcuts=["gone.bat"], notes_add="New sentence.")
    assert again == out, "not idempotent"


# --- md5-pinned sources --------------------------------------------------------

def _zip(members):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    return b.getvalue()


def test_a_source_is_refused_unless_it_is_the_pinned_file(tmp_path):
    lib = tmp_path / "Files" / "Games-Library" / "Title" / "Sub"
    lib.mkdir(parents=True)
    (lib / "FILE.EXE").write_bytes(b"MZ123")
    good = ls.md5(b"MZ123")
    assert ls.fetch({"lib": "title/sub/file.exe", "md5": good}, mnt=str(tmp_path)) == b"MZ123", \
        "lookup must be case-insensitive (a Windows tree)"
    with pytest.raises(ls.SourceError, match="not the pinned file"):
        ls.fetch({"lib": "Title/Sub/FILE.EXE", "md5": "0" * 32}, mnt=str(tmp_path))
    with pytest.raises(ls.SourceError, match="pin its md5"):
        ls.fetch({"lib": "Title/Sub/FILE.EXE"}, mnt=str(tmp_path))
    with pytest.raises(ls.SourceError, match="not on the share"):
        ls.fetch({"lib": "Title/NOPE.EXE", "md5": good}, mnt=str(tmp_path))


def test_a_zip_inside_a_zip_is_read(tmp_path):
    """TIE Fighter's dialog-free FRONT.OVL is crack.zip/FRONT.OVL inside the
    floppy-set zip - a member of a member."""
    inner = _zip({"FRONT.OVL": b"ovl-bytes"})
    outer = _zip({"crack.zip": inner, "tie.001": b"x"})
    d = tmp_path / "Files" / "Games"
    d.mkdir(parents=True)
    (d / "Tie.zip").write_bytes(outer)
    got = ls.fetch({"zip": "Files/Games/tie.zip", "member": "CRACK.ZIP/front.ovl",
                    "md5": ls.md5(b"ovl-bytes")}, mnt=str(tmp_path))
    assert got == b"ovl-bytes"


# --- Quake 1 --------------------------------------------------------------------

Q1 = sw9.TITLES["Quake1"]
Q1_BATS = {k: sw9.crlf(v) for k, v in Q1["bats"].items()}

# The executed lines of the 'Play Quake - Voodoo.bat' that ran on .243 (Voodoo 2)
# and redirected on .124 (V5 6000), 2026-09-28. Q1V640.BAT is that launcher with
# corrected comments - its behaviour must not change.
OLD_VOODOO_CMDS = [
    "if not exist VOODOO\\GLQUAKE.EXE goto notree",
    "if not exist %windir%\\system32\\reg.exe goto novsa",
    'reg query "HKLM\\SYSTEM\\CurrentControlSet\\Enum\\PCI" 2>nul | find /i "VEN_121A&DEV_0009" >nul',
    "if errorlevel 1 goto novsa",
    "echo This is a 3dfx Voodoo 4/5. The Voodoo 1/2 MiniGL cannot drive it, so",
    "echo Quake runs through the system OpenGL driver instead.",
    'call "Play Quake.bat"', "goto end", ":novsa",
    "if exist %windir%\\SYSTEM32\\GLIDE2X.DLL goto glide",
    "if exist %windir%\\SYSTEM\\GLIDE2X.DLL goto glide", "echo.",
    "echo  No 3dfx Glide driver is installed on this machine.",
    "echo  This shortcut is for a 3dfx Voodoo card - use Play Quake instead.",
    "echo.", "pause", "goto end", ":glide",
    "start /w VOODOO\\GLQUAKE.EXE -width 640 -height 480 -bpp 16",
    "if exist VOODOO\\DESKFIX9.EXE VOODOO\\DESKFIX9.EXE", "goto end", ":notree",
    "echo VOODOO\\GLQUAKE.EXE is missing - the Quake tree is incomplete.", "pause", ":end"]


def test_every_generated_win9x_launcher_is_command_com_dialect():
    for title, spec in sw9.TITLES.items():
        for rel, text in spec.get("bats", {}).items():
            assert lm.command_com_problems(sw9.crlf(text).encode("ascii")) == [], (title, rel)


def test_quake1_win9x_launchers_are_8_3_names():
    assert sorted(Q1_BATS) == ["Q1DOS.BAT", "Q1DOSM10.BAT", "Q1HIPD.BAT", "Q1HIPV.BAT",
                               "Q1ROGD.BAT", "Q1ROGV.BAT", "Q1V640.BAT", "Q1V800.BAT"]
    for name in Q1_BATS:
        assert NAME83.match(name), name


def test_q1v640_is_the_proven_voodoo_launcher_with_corrected_comments():
    assert _cmds(Q1_BATS["Q1V640.BAT"]) == OLD_VOODOO_CMDS
    assert "cannot hold" not in Q1_BATS["Q1V640.BAT"].split("800x600 with")[0][-200:], \
        "the wrong 'an 8 MB card cannot hold 800x600' reason must be gone"
    assert "Voodoo 1 has no 800x600" in Q1_BATS["Q1V640.BAT"]


def test_voodoo_launchers_ask_only_for_the_measured_modes():
    """.243's Voodoo 2: 640x480x16 and 800x600x16 run; 512x384 is refused."""
    seen = set()
    for name in ("Q1V640.BAT", "Q1V800.BAT"):
        for line in _cmds(Q1_BATS[name]):
            m = re.search(r"-width (\d+) -height (\d+) -bpp (\d+)", line)
            if m:
                seen.add(m.groups())
    assert seen == {("640", "480", "16"), ("800", "600", "16")}


@pytest.mark.parametrize("name,run", [
    ("Q1DOS.BAT", "QUAKE.EXE -nojoy -nocdaudio -winmem 16"),
    ("Q1DOSM10.BAT", "QUAKE.EXE -nojoy -nocdaudio -winmem 16 +vid_mode 10"),
])
def test_dos_launchers_skip_both_prompts_and_cls_after_the_game(name, run):
    cmds = _cmds(Q1_BATS[name])
    assert run in cmds, cmds
    i = cmds.index(run)
    assert cmds[i + 1] == "cls", "CLS must come straight after the game"


def test_quake1_rules_put_each_launcher_on_the_right_boxes():
    rules = Q1["requires"]["set"]
    assert rules["Q1V640.BAT"] == {"requires_capabilities": ["glide"]}, \
        "Q1V640 keeps reaching a Voodoo 4/5 NT box, which it redirects"
    assert rules["Q1V800.BAT"] == {"requires_capabilities": ["glide"], "max_os": "win9x"}
    assert rules["Q1DOS.BAT"] == {"max_os": "win9x"} == rules["Q1DOSM10.BAT"]
    for nt in ("Play Quake.bat", "Play Quake - Software.bat"):
        assert rules[nt]["min_os"] == "win2k", "cmd.exe dialect: never on Win9x"
    assert set(Q1["requires"]["drop"]) == {"Play Quake - Voodoo.bat", "Play Quake - DOS.bat"}, \
        "a rule for a target launch.txt no longer has FAILS gamegate lint"


def test_quake1_launch_rows_keep_line0_and_the_old_display_names():
    rows = Q1["launch"]["rows"]
    assert rows[0] == ("Play Quake.bat", "Quake", "GLQUAKE.EXE"), \
        "stage-fleetres.py pins line 0 (launch_txt_line0)"
    disp = {r[0]: r[1] for r in rows}
    assert disp["Q1V640.BAT"] == "Quake - 3dfx Voodoo"
    assert disp["Q1DOS.BAT"] == "Quake - DOS", \
        "same display name = the Win98 PIF is rewritten in place, not swept"
    fake = lm.set_launch_rows(LAUNCH, rows, Q1["launch"]["drop"], Q1["launch"]["note"])
    assert lm.launch_problems(fake) == []


# --- the runner -----------------------------------------------------------------

class _Rec:
    kind = "rec"

    def __init__(self):
        self.order = []

    def write_bytes(self, path, data):
        self.order.append(os.path.basename(path))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)


def _fake_lib(tmp_path):
    t = tmp_path / "Quake1"
    t.mkdir()
    (t / "launch.txt").write_bytes(LAUNCH.encode())
    (t / "requires.json").write_bytes(REQ.encode())
    return str(tmp_path)


def test_runner_writes_launch_txt_last_and_then_settles(tmp_path):
    lib = _fake_lib(tmp_path)
    rec = _Rec()
    r = sw9.Runner(lib, writer=rec, titles={"Quake1": Q1}).run()
    assert rec.order[-1] == "launch.txt", rec.order
    assert rec.order.index("requires.json") > rec.order.index("Q1V640.BAT")
    assert not r.problems
    rec2 = _Rec()
    r2 = sw9.Runner(lib, writer=rec2, titles={"Quake1": Q1}).run()
    assert rec2.order == [] and r2.current == len(rec.order), "a settled title writes nothing"
    chk = sw9.Runner(lib, check=True, writer=_Rec(), titles={"Quake1": Q1}).run()
    assert chk.problems == []


def test_runner_never_creates_a_title(tmp_path):
    rec = _Rec()
    r = sw9.Runner(str(tmp_path), writer=rec, titles={"Quake1": Q1}).run()
    assert rec.order == [] and r.problems == ["Quake1: not in the library"]


def test_runner_refuses_a_launcher_that_fails_the_dialect_check(tmp_path):
    lib = _fake_lib(tmp_path)
    bad = {"bats": {"BAD.BAT": "@echo off\ncd /d X\n"}}
    with pytest.raises(SystemExit, match="COMMAND.COM"):
        sw9.Runner(lib, writer=_Rec(), titles={"Quake1": bad}).run()


@_share
def test_the_share_is_what_the_generator_makes():
    r = sw9.Runner(LIB, check=True, writer=_Rec()).run()
    assert r.problems == [], ("the library differs from scripts/fleet/stage-win9x.py - "
                              "run it (or fix the generator, never the share): %s" % r.problems)


def test_quake1_mission_packs():
    """Scourge of Armagon and Dissolution of Eternity (2026-10-01): the packs'
    data from the share's TOSEC zips, md5-pinned; each pack gets a Win9x Voodoo
    and a DOS launcher (the -hipnotic / -rogue switch both engines carry) and
    an NT GLQuake launcher from stage-fleetres.py, whose rule it shares."""
    copies = Q1["copies"]
    assert copies["HIPNOTIC/PAK0.PAK"]["md5"] == "0ab83681aaf841c4320269e02941a14a"
    assert copies["ROGUE/PAK0.PAK"]["md5"] == "c38a4e04219c317cd1b02f386bdfe11f"
    assert copies["QROGUE.ICO"]["md5"] == "6fcc4d5491eb6ce40a81d6cdcda74c28"
    for switch, folder, name, vbat, dbat in sw9.Q1_PACKS:
        v, d = Q1_BATS[vbat], Q1_BATS[dbat]
        assert "start /w VOODOO\\GLQUAKE.EXE -%s -width 640 -height 480 -bpp 16" % switch in v
        assert "QUAKE.EXE -%s -nojoy -nocdaudio -winmem 16" % switch in d
        lines = d.replace("\r\n", "\n").split("\n")
        assert lines[lines.index("QUAKE.EXE -%s -nojoy -nocdaudio -winmem 16" % switch) + 2] == "cls"
        for bat in (v, d):
            assert "if not exist %s\\PAK0.PAK goto notree" % folder in bat
        rules = Q1["requires"]["set"]
        assert rules[vbat] == {"requires_capabilities": ["glide"], "max_os": "win9x"}
        assert rules[dbat] == {"max_os": "win9x"}
        nt = "Play Quake - %s.bat" % name
        assert rules[nt] == rules["Play Quake.bat"]
        rows = {r[0]: r for r in Q1["launch"]["rows"]}
        assert nt in rows and vbat in rows and dbat in rows
    assert Q1["requires"]["set_top"]["disk_mb"] >= 200
