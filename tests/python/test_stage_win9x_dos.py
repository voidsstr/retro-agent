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
                            "Play Falcon 3.0.bat", "FALCON3.ICO", "launch.txt",
                            "requires.json"])
    launch = open(os.path.join(root, "launch.txt"), "rb").read().decode("latin-1")
    assert launch.split("\r\n")[0] == "Play Falcon 3.0.bat\tFalcon 3.0\tFALCON3.ICO"
    req = json.load(open(os.path.join(root, "requires.json")))
    assert req["max_os"] == "win9x", "no DOSBox is staged: an NT box must not get it"
    assert req["title"] == "Flight-Falcon3" and req["disk_mb"] >= 1
    assert set(mine) == {"Play Falcon 3.0.bat", "FALCON3.ICO", "launch.txt", "requires.json"}


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


# --- a title that runs ONLY in real MS-DOS ----------------------------------------

def _real_dos_title(tmp_path, ems=False):
    tree = tmp_path / "stage"
    tree.mkdir(exist_ok=True)
    (tree / "PRIV.EXE").write_bytes(b"MZ")
    (tree / "JEMM.OVL").write_bytes(b"x")
    rd = {"dir": "PRIV"}
    if ems:
        rd["ems"] = True
    return _spec(tmp_path, lib="Flight-Privateer", title="Privateer", tree=str(tree),
                 launch=["PRIV.EXE"], real_dos=rd)


def _cmds(b):
    return [l for l in b.decode("ascii").split("\r\n") if l and not l.lower().startswith("rem")]


def test_a_real_dos_title_copies_to_c_arms_the_hook_and_restarts_with_force(tmp_path):
    """Privateer's JEMM, USNF's Phar Lap TNT - and in a Win98 DOS box RET8's
    DOS/4GW 1.92 and EF2000's X-32VM page-fault near 2 GB - so the game runs in
    real DOS. Real DOS sees only C: on .243. An MS-DOS mode PIF is NOT the
    route: Windows leaves for it WITHOUT force and the agent's console (a DOS
    VM on Win98) stops the shutdown with a dialog."""
    t = _real_dos_title(tmp_path)
    work = tmp_path / "work"
    work.mkdir()
    root, mine = sw.build(t, str(work))
    assert os.path.isfile(os.path.join(root, "PRIV.EXE")), "the game stays where GAMESYNC puts every title"
    assert set(mine) == {"Play Privateer.bat", "RDGAME.BAT", "RDHOOK.TXT", "PRIVATEE.ICO",
                         "launch.txt", "requires.json"}
    cmds = _cmds(open(os.path.join(root, "Play Privateer.bat"), "rb").read())
    assert "choice /c:yn /t:y,10 Restart now" in cmds and cmds[cmds.index("choice /c:yn /t:y,10 Restart now") + 1] == "if errorlevel 2 goto end"
    assert "xcopy *.* C:\\GAMES\\PRIV\\ /E /I /D /Y /Q > nul" in cmds
    x = cmds.index("xcopy *.* C:\\GAMES\\PRIV\\ /E /I /D /Y /Q > nul")
    assert cmds[x + 1] == "if errorlevel 4 goto nocopy", "a full C: must not arm a half-copied game"
    arm = cmds.index("echo call C:\\GAMES\\PRIV\\RDGAME.BAT> C:\\RUNDOS\\NEXT.BAT")
    boot = cmds.index("rundll32.exe shell32.dll,SHExitWindowsEx 6")      # 2 reboot + 4 FORCE
    assert cmds.index("if not exist C:\\GAMES\\PRIV\\RDGAME.BAT goto nocopy") < arm < boot, \
        "never restart for a copy that did not land"
    assert 'find "rundos v2" C:\\AUTOEXEC.BAT > nul' in cmds, "a hook from an older launcher is not this one"
    assert "if errorlevel 1 type RDHOOK.TXT >> C:\\AUTOEXEC.BAT" in cmds, "appended ONCE, never overwritten"
    assert cmds[-1] == "cls" and "pause" in cmds[cmds.index(":nocopy"):]
    run = _cmds(open(os.path.join(root, "RDGAME.BAT"), "rb").read())
    assert run[:4] == ["@echo off", "C:", "cd \\GAMES\\PRIV", "PRIV.EXE"]
    req = json.load(open(os.path.join(root, "requires.json")))
    assert req["max_os"] == "win9x" and "REAL DOS ONLY" in req["notes"]


def test_the_autoexec_hook_runs_a_game_once_even_if_it_hangs_the_pc():
    """A game that hangs the PC must not run again at every boot after: the
    stale RAN.BAT is deleted BEFORE this boot's NEXT.BAT becomes RAN.BAT. And
    an EMS title's CONFIG.SYS goes back before its game can hang anything."""
    hook = [l for l in sw.RD_HOOK.split("\r\n") if l and not l.startswith("rem")]
    assert hook == ["if exist C:\\RUNDOS\\CONFIG.SAV copy C:\\RUNDOS\\CONFIG.SAV C:\\CONFIG.SYS > nul",
                    "if exist C:\\RUNDOS\\CONFIG.SAV del C:\\RUNDOS\\CONFIG.SAV",
                    "if exist C:\\RUNDOS\\RAN.BAT del C:\\RUNDOS\\RAN.BAT",
                    "if exist C:\\RUNDOS\\NEXT.BAT ren C:\\RUNDOS\\NEXT.BAT RAN.BAT",
                    "if exist C:\\RUNDOS\\RAN.BAT call C:\\RUNDOS\\RAN.BAT"]
    assert sw.RD_HOOK.startswith("\r\n"), "appended after a last line that may lack its CRLF"
    assert "rundos v2" in sw.RD_HOOK, "the launcher finds the hook by this marker"
    for l in sw.RD_HOOK.split("\r\n"):
        if l.startswith("rem"):
            assert "<" not in l and ">" not in l, "COMMAND.COM parses redirection on rem lines"


def test_an_ems_title_picks_the_ems_boot_entry_for_one_boot(tmp_path):
    """Falcon 3.0 and Pacific Strike want EMS; Tornado wants the same entry's
    upper memory (its AMP must LOADHIGH). Measured in the Win98 build VM:
    Falcon 3.0 and Tornado run in real DOS with CONFIG.SYS's EMS block, and
    Tornado reports a conventional-memory shortage without it."""
    t = _real_dos_title(tmp_path, ems=True)
    files = sw.generated(t, 0)
    cmds = _cmds(files["Play Privateer.bat"])
    save = cmds.index("copy C:\\CONFIG.SYS C:\\RUNDOS\\CONFIG.SAV > nul")
    assert cmds[save + 1] == "start /w MENUDEF9.EXE EMS", "START /W: a DOS box does not wait for a Windows program"
    assert save < cmds.index("rundll32.exe shell32.dll,SHExitWindowsEx 6")
    assert files["MENUDEF9.EXE"][:2] == b"MZ"
    assert "MENUDEF9.EXE" not in sw.generated(_real_dos_title(tmp_path), 0), "only an EMS title ships it"


def test_an_ems_title_checks_the_boot_menu_choice_first(tmp_path):
    t = _real_dos_title(tmp_path, ems=True)
    run = _cmds(sw.rd_game_bat(t))
    assert run[2:4] == ["cd \\GAMES\\PRIV", 'if "%CONFIG%"=="EMS" goto run']
    assert run.index(":run") < run.index("PRIV.EXE") < run.index(":done")
    assert "boot menu 2" in sw.generated(t, 0)["requires.json"].decode()


def test_every_title_ships_its_icon_under_its_own_name(tmp_path):
    """.243, 2026-09-29: 39 Flight-* titles each shipped ICON.ICO, at 39
    different paths, and Windows 98 drew F-14 Fleet Defender's art on every
    one of them - its shell caches an icon by FILE NAME. Uniquely named icons
    (DESCENT9.ICO, hexen2.ico, Q2.ico) drew correctly on the same desktop."""
    assert sw.icon_file_name({"lib": "Flight-Falcon3"}) == "FALCON3.ICO"
    assert sw.icon_file_name({"lib": "Flight-PacificAirWar1942"}) == "PACIFICA.ICO"
    assert sw.icon_file_name({"lib": "DOS-Duke3D"}) == "DUKE3D.ICO"
    assert sw.icon_file_name({"lib": "X", "icon_name": "pstrike.ico"}) == "PSTRIKE.ICO"
    with pytest.raises(AssertionError):
        sw.icon_file_name({"lib": "X", "icon_name": "ICON.ICO"})
    src = open(os.path.join(REPO, "scripts", "dosgames", "stage_win9x_dos.py")).read()
    assert "would both ship %s" in src, "two titles in one spec may not share an icon name"


def test_the_icon_never_overwrites_a_file_the_game_ships(tmp_path):
    # Flight-F14 ships F14.ICO and Flight-Longbow LONGBOW.ICO of their own.
    tree = tmp_path / "stage"
    tree.mkdir()
    (tree / "F14.COM").write_bytes(b"x")
    (tree / "F14.ICO").write_bytes(b"the game's own")
    t = _spec(tmp_path, lib="Flight-F14", tree=str(tree), launch=["F14.COM"])
    work = tmp_path / "work"
    work.mkdir()
    with pytest.raises(SystemExit, match="already has a file F14.ICO"):
        sw.build(t, str(work))


# --- fixing a title that is ALREADY staged: --update + the spec rebuilt from the library ---
# 2026-09-30: the 54 Win9x DOS titles' specs and prepared trees lived in a
# session scratchpad that host reboots wiped, and build() never overwrites a
# staged title - so a launcher fix had no route through the generator at all.

_lspec = importlib.util.spec_from_file_location(
    "spec_from_library", os.path.join(REPO, "scripts", "dosgames", "spec_from_library.py"))
sfl = importlib.util.module_from_spec(_lspec)
_lspec.loader.exec_module(sfl)


def _staged(tmp_path, t):
    lib_root = tmp_path / "library"
    lib_root.mkdir()
    root, _ = sw.build(t, str(lib_root))
    return str(lib_root), root


def _recorder():
    wrote = {}
    return wrote, (lambda p, d: wrote.__setitem__(p, d))


def test_an_update_of_an_unchanged_title_writes_nothing(tmp_path):
    t = _spec(tmp_path, tree=str(tmp_path), launch=["FALCON3.EXE"])
    tree = tmp_path / "stage"
    tree.mkdir()
    (tree / "FALCON3.EXE").write_bytes(b"MZ" + b"\0" * 5000)
    t["tree"] = str(tree)
    lib_root, _ = _staged(tmp_path, t)
    wrote, rec = _recorder()
    assert sw.update(t, rec, lib_root=lib_root) == [] and wrote == {}


def test_an_update_rewrites_only_the_launcher_it_changed(tmp_path):
    tree = tmp_path / "stage"
    tree.mkdir()
    (tree / "F19.COM").write_bytes(b"MZ")
    t = _spec(tmp_path, lib="Flight-F19", title="F-19 Stealth Fighter", tree=str(tree),
              launch=["F19.COM"])
    lib_root, root = _staged(tmp_path, t)
    t["launch"] = ["F19.COM /NJ /GM"]          # the fix measured in the Win98 VM
    wrote, rec = _recorder()
    changed = sw.update(t, rec, lib_root=lib_root)
    assert changed == ["Play F-19 Stealth Fighter.bat"], changed
    (path, data), = wrote.items()
    assert path == os.path.join(root, "Play F-19 Stealth Fighter.bat")
    assert b"\r\nF19.COM /NJ /GM\r\n" in data and data.endswith(b"cls\r\n")
    assert open(os.path.join(root, "F19.COM"), "rb").read() == b"MZ", "the game is never touched"


def test_an_update_refuses_a_title_that_is_not_staged(tmp_path):
    t = _spec(tmp_path, lib="Flight-Nothing", tree=str(tmp_path))
    with pytest.raises(SystemExit, match="not staged yet"):
        sw.update(t, lambda p, d: None, lib_root=str(tmp_path))


@pytest.mark.parametrize("real_dos", [False, True])
def test_the_spec_rebuilt_from_a_title_regenerates_it_byte_for_byte(tmp_path, real_dos):
    """spec_from_library.py reads a title's spec back out of what the generator
    wrote. The proof it is faithful: generated() from the rebuilt spec equals
    every file build() wrote - so --update of an unfixed title changes nothing."""
    if real_dos:
        t = _real_dos_title(tmp_path, ems=True)
    else:
        tree = tmp_path / "stage"
        tree.mkdir()
        (tree / "RET.BAT").write_bytes(b"x")
        t = _spec(tmp_path, lib="Flight-Retribution", title="Retribution", tree=str(tree),
                  launch=["RET.BAT", "SET DID=."], notes="needs XMS")
    lib_root, root = _staged(tmp_path, t)
    back = sfl.one(root)
    assert back["launch"] == ["call RET.BAT", "SET DID=."] if not real_dos else back["launch"] == ["PRIV.EXE"]
    assert back["source_label"] == "stage" and back["year"] == t["year"]
    assert bool(back.get("real_dos")) == real_dos
    wrote, rec = _recorder()
    assert sw.update(back, rec, lib_root=lib_root) == [], wrote
