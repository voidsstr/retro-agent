"""scripts/dosgames/stage_win9x_dos.py - a DOS game staged as a Windows 9x library title.

WHY (2026-09-29). .243 (Win98 SE, Pentium 166, SB16, a gameport flight stick)
keeps its games on a 72 GB E: that real-mode DOS cannot see, and the user asked
for every DOS flight simulator on it with a desktop icon of the game's own art.
A GAMESYNC title does that - the agent copies the tree and makes the shortcut -
so each game becomes a title whose `Play <name>.bat` runs it natively in a Win98
DOS box. These tests pin what was measured on .243, not what was assumed:

* The launcher ends with CLS. Windows 9x closes a DOS box on exit only when its
  screen is empty; a batch that left text behind stayed as a "Finished - ..."
  window, the same batch ending in CLS closed (three PIFs side by side on .243).
* The icon must be one a 9x desktop can draw - the validator's own rule: a
  32-bit-only .ico showed the generic MS-DOS icon there, 8- and 24-bit drew.
* The batch is COMMAND.COM dialect: no %~dp0, no cd /d, no blocks, and no
  parentheses anywhere (CLAUDE.md: a generated name or value never carries one).
"""
import importlib.util
import json
import os
import struct
import zipfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location(
    "stage_win9x_dos", os.path.join(REPO, "scripts", "dosgames", "stage_win9x_dos.py"))
sw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sw)


def _ico(bpp):
    dib = struct.pack("<IiiHH", 40, 32, 64, 1, bpp) + b"\0" * 64
    return struct.pack("<HHH", 0, 1, 1) + struct.pack(
        "<BBBBHHII", 32, 32, 0, 0, 1, bpp, len(dib), 22) + dib


def _spec(tmp_path, **kw):
    t = {"lib": "Flight-Falcon3", "title": "Falcon 3.0", "year": 1991,
         "launch": ["FALCON3.EXE"], "icon": str(tmp_path / "icon.ico")}
    t.update(kw)
    if not os.path.exists(t["icon"]):
        with open(t["icon"], "wb") as fh:
            fh.write(_ico(8))
    return t


def test_the_launcher_is_command_com_dialect_and_ends_with_cls(tmp_path):
    bat = sw.play_bat(_spec(tmp_path, launch=["CD FDUEL", "FD.EXE"])).decode("ascii")
    lines = bat.split("\r\n")
    assert lines[-1] == "" and lines[-2] == "cls", "CLS must be the last command"
    cmds = [l for l in lines if l and not l.lower().startswith("rem")]
    assert cmds == ["@echo off", "CD FDUEL", "FD.EXE", "cls"]
    for l in cmds:
        assert "%~" not in l and "cd /d" not in l.lower() and "start \"" not in l
    assert "(" not in bat and ")" not in bat
    assert "\n" not in bat.replace("\r\n", ""), "CRLF line ends only"


def test_a_batch_launcher_is_called_so_the_cls_after_it_runs(tmp_path):
    # Retribution starts through RET.BAT: without CALL, COMMAND.COM hands the
    # rest of the Play batch over to it and the closing CLS never runs.
    bat = sw.play_bat(_spec(tmp_path, launch=["RET.BAT", "CD X", "GAME.EXE"])).decode("ascii")
    cmds = [l for l in bat.split("\r\n") if l and not l.lower().startswith("rem")]
    assert cmds == ["@echo off", "call RET.BAT", "CD X", "GAME.EXE", "cls"]
    bat = sw.play_bat(_spec(tmp_path, launch=["call ret.bat"])).decode("ascii")
    assert "call call" not in bat.lower()


@pytest.mark.parametrize("bad", ["GAME.EXE > NUL", "GAME.EXE (x)", "TYPE < X"])
def test_a_launch_line_with_brackets_or_redirection_is_refused(tmp_path, bad):
    with pytest.raises(AssertionError):
        sw.play_bat(_spec(tmp_path, launch=[bad]))


def test_display_names_are_legal_filenames_without_parentheses():
    assert sw.clean_name("Aces of the Pacific + WWII:1946") == \
        "Aces of the Pacific and WWII - 1946"
    assert sw.clean_name("Comanche 2.0 (Werewolf vs Comanche 2.0 CD)") == \
        "Comanche 2.0 Werewolf vs Comanche 2.0 CD"
    assert sw.clean_name("Wing Commander & Secret Missions") == \
        "Wing Commander and Secret Missions"
    for n in ("A: B", "C/D", 'E"F', "G?H", "I*J", "K|L", "M<N>"):
        assert not any(c in sw.clean_name(n) for c in '\\/:*?"<>|()')


def test_8_3_names():
    assert sw.is_83("FALCON3.EXE") and sw.is_83("FDUEL/FD.EXE") and sw.is_83("A/B/C.D")
    assert not sw.is_83("Falcon 3.exe") and not sw.is_83("LONGNAME1.EXE")
    assert not sw.is_83("A.JPEG") and not sw.is_83("longdirname/x.y")
    assert sw.is_83("DIR.EXT/X.Y"), "a DOS directory may carry an extension"


def test_a_title_built_from_a_zip(tmp_path):
    z = tmp_path / "Falcon.zip"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("FALCON3/FALCON3.EXE", b"MZ" + b"\0" * 100)
        zf.writestr("FALCON3/SOUND/SB.CFG", b"A220 I5")
        zf.writestr("FALCON3/Read Me.txt", b"dropped")
    t = _spec(tmp_path, zip=str(z), strip_top="FALCON3", drop_non83=["Read Me.txt"])
    work = tmp_path / "work"
    work.mkdir()
    root, mine = sw.build(t, str(work))
    names = sorted(os.path.relpath(os.path.join(d, f), root)
                   for d, _, fs in os.walk(root) for f in fs)
    assert names == sorted(["FALCON3.EXE", os.path.join("SOUND", "SB.CFG"),
                            "Play Falcon 3.0.bat", "ICON.ICO", "launch.txt",
                            "requires.json"])
    launch = open(os.path.join(root, "launch.txt"), "rb").read().decode("latin-1")
    assert launch.split("\r\n")[0] == "Play Falcon 3.0.bat\tFalcon 3.0\tICON.ICO"
    req = json.load(open(os.path.join(root, "requires.json")))
    assert req["max_os"] == "win9x", "no DOSBox is staged: an NT box must not get it"
    assert req["title"] == "Flight-Falcon3" and req["disk_mb"] >= 1
    assert set(mine) == {"Play Falcon 3.0.bat", "ICON.ICO", "launch.txt", "requires.json"}


def test_a_title_built_from_a_prepared_tree(tmp_path):
    # The icon agent's stage/ dirs: a game's own setup run in DOSBox with the
    # target's SB16 (A220 I5 D1 H5), copied as it stands.
    tree = tmp_path / "stage"
    (tree / "FDUEL").mkdir(parents=True)
    (tree / "FDUEL" / "FD.EXE").write_bytes(b"MZ")
    (tree / "FDUEL" / "SOUND.CFG").write_bytes(b"irq=5")
    t = _spec(tmp_path, lib="Flight-FighterDuel", title="Fighter Duel SE",
              tree=str(tree), launch=["CD FDUEL", "FD.EXE"])
    work = tmp_path / "work"
    work.mkdir()
    root, _ = sw.build(t, str(work))
    assert open(os.path.join(root, "FDUEL", "SOUND.CFG"), "rb").read() == b"irq=5"
    assert "Source: stage" in open(os.path.join(root, "launch.txt"), encoding="latin-1").read()


def test_a_long_name_in_the_source_stops_the_build(tmp_path):
    tree = tmp_path / "stage"
    tree.mkdir()
    (tree / "LONGFILENAME.DAT").write_bytes(b"x")
    t = _spec(tmp_path, tree=str(tree))
    work = tmp_path / "work"
    work.mkdir()
    with pytest.raises(SystemExit, match="non-8.3"):
        sw.build(t, str(work))


def test_an_icon_9x_cannot_draw_stops_the_build(tmp_path):
    ico = tmp_path / "xp_only.ico"
    ico.write_bytes(_ico(32))
    tree = tmp_path / "stage"
    tree.mkdir()
    (tree / "GAME.EXE").write_bytes(b"MZ")
    t = _spec(tmp_path, tree=str(tree), icon=str(ico), launch=["GAME.EXE"])
    work = tmp_path / "work"
    work.mkdir()
    with pytest.raises(SystemExit, match="generic MS-DOS icon"):
        sw.build(t, str(work))


# --- the hand-written Win9x DOS launchers in the live library ----------------

LIB = "/mnt/retro-share/Files/Games-Library"
_share = pytest.mark.skipif(not os.path.isdir(LIB),
                            reason="staged library not mounted at %s - the "
                                   "share side was NOT checked" % LIB)

# (title, launcher, the game's own program) - both measured on .243.
WIN9X_DOS_LAUNCHERS = (
    ("Quake1", "Play Quake - DOS.bat", "QUAKE.EXE"),
    ("Descent1", "Play Descent - DOS.bat", "DESCENTR.EXE"),
)


@_share
@pytest.mark.parametrize("title,bat,exe", WIN9X_DOS_LAUNCHERS)
def test_every_win9x_dos_launcher_clears_the_screen_after_the_game(title, bat, exe):
    """Descent's quit left "Finished - Descent - DOS" on .243's desktop; CLS
    after the game closes the window. It must come AFTER the game runs, not
    merely somewhere in the file."""
    lines = [l.strip() for l in open(os.path.join(LIB, title, bat), "rb").read()
             .decode("latin-1").splitlines()]
    cmds = [l for l in lines if l and not l.lower().startswith(("rem", "@echo"))]
    run = next(i for i, l in enumerate(cmds) if l.upper() == exe)
    assert "cls" in [c.lower() for c in cmds[run + 1:]], cmds
    req = json.load(open(os.path.join(LIB, title, "requires.json")))
    assert req["shortcuts"][bat]["max_os"] == "win9x", \
        "an NT box has no DOS box to run it: the shortcut must be 9x-only"


@_share
def test_descent_s_dos_launcher_fixes_the_irq_only_while_it_is_wrong():
    """DESCENT.CFG is DOSBox's (IRQ 7, the other boxes' launchers); .243's SB16
    is on IRQ 5. The launcher copies DESCENT.SB5 over only while the config
    still says 7, so what the game saves on exit survives."""
    d = os.path.join(LIB, "Descent1")
    cfg = open(os.path.join(d, "DESCENT.CFG"), "rb").read()
    sb5 = open(os.path.join(d, "DESCENT.SB5"), "rb").read()
    assert b"DigiIrq=7\r\n" in cfg
    assert sb5 == cfg.replace(b"DigiIrq=7\r\n", b"DigiIrq=5\r\n"), \
        "DESCENT.SB5 must be DESCENT.CFG with only the IRQ changed"
    bat = open(os.path.join(d, "Play Descent - DOS.bat"), "rb").read().decode("latin-1")
    assert 'find "DigiIrq=7" DESCENT.CFG > nul\r\nif not errorlevel 1 copy DESCENT.SB5 DESCENT.CFG > nul' in bat
