"""Win9x 3dfx Voodoo launchers for library titles (research R1, 2026-10-01).

A Voodoo 1/2 box (.243: Pentium 166, Voodoo 2, Win98 SE) cannot start any of a
title's cmd.exe launchers, and a game-local Glide WRAPPER in the tree hides its
real card: game-local wins at load time. These tests pin the design:

* Every Win9x Voodoo launcher is COMMAND.COM dialect, checks for Glide itself
  (agents older than 1.84.0 ignore the glide rule), runs the game with
  `start /w`, repaints the desktop with DESKFIX9 afterwards, and is gated
  `glide` + `max_os win9x`; the title's cmd.exe launchers become `min_os win2k`.
* The game-local wrappers SHIP INERT (nGlide as *.nglide, IPXWrapper as *.ipxw):
  on Win98 nGlide cannot even load (it imports d3d9.dll) and IPXWrapper's stubs
  abort() the game on their first network call (ipxwrapper.dll imports
  kernel32!GetSystemWindowsDirectoryA, which Win98 SE lacks - MEASURED, R1). No
  launcher edits a library-shipped file: NT's existing render-device block
  already restores nGlide from .nglide, and IPXWrapper is COPIED into place on
  NT, so GAMESYNC never has anything to put back.
"""
import importlib.util
import os
import re
import sys

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


sw9 = _load("stage_win9x_vd", os.path.join(FLEET, "stage-win9x.py"))
sf = _load("stage_fleetres_vd", os.path.join(FLEET, "stage-fleetres.py"))
lm = _load("libmeta_vd", os.path.join(FLEET, "libmeta.py"))
vl = _load("validate_vd", os.path.join(REPO, "scripts", "validate-staged-library.py"))

LIB = "/mnt/retro-share/Files/Games-Library"
_share = pytest.mark.skipif(not os.path.isdir(LIB),
                            reason="staged library not mounted at %s - the share "
                                   "side was NOT checked" % LIB)
B5 = ("SiNGold", "HalfLife1", "Quake2Complete", "UnrealGold", "Carmageddon2")


def _cmds(text):
    return [l.strip() for l in text.replace("\r\n", "\n").split("\n")
            if l.strip() and not l.strip().lower().startswith(("rem", "@echo"))]


def _voodoo_bats():
    for title in B5:
        for name, text in sw9.TITLES[title]["bats"].items():
            yield title, name, sw9.crlf(text)


@pytest.mark.parametrize("title,name,text", list(_voodoo_bats()))
def test_every_voodoo_launcher_has_the_proven_shape(title, name, text):
    assert lm.command_com_problems(text) == []
    assert "(" not in name and ")" not in name
    cmds = _cmds(text)
    glide = cmds.index(":glide")
    assert "if exist %windir%\\SYSTEM\\GLIDE2X.DLL goto glide" in cmds[:glide]
    runs = [c for c in cmds if c.startswith("start /w ")
            and not c.startswith("start /w regedit")]
    assert len(runs) == 1 and cmds.index(runs[0]) > glide, runs
    assert cmds[cmds.index(runs[0]) + 1:][:3].count("if exist DESKFIX9.EXE DESKFIX9.EXE") \
        or "cd .." in cmds, "repaint the desktop after the Glide game"
    fix = cmds.index("if exist DESKFIX9.EXE DESKFIX9.EXE")
    assert cmds[fix + 1:fix + 3] == ["cls", "goto end"], \
        "Windows 9x closes the DOS box on exit only when its screen is empty"
    assert "DESKFIX9.EXE" in sw9.TITLES[title]["copies"]
    assert sw9.TITLES[title]["copies"]["DESKFIX9.EXE"]["md5"] == \
        "7b71bd5bc0a322072d214778243d9059"
    assert sw9.TITLES[title]["requires"]["set"][name] == \
        {"requires_capabilities": ["glide"], "max_os": "win9x"}
    assert ".nglide" not in text.lower(), \
        ("a launcher that names .nglide must move it both ways (validator "
         "fleetres-glide); the Win9x lane never moves the wrapper at all")


@pytest.mark.parametrize("title", B5)
def test_the_cmd_exe_launchers_never_reach_win9x(title):
    req = sw9.TITLES[title]["requires"]
    patched = req.get("patch", {})
    assert patched and all(v.get("min_os") == "win2k" for v in patched.values())
    rows = {r[0] for r in sw9.TITLES[title]["launch"]["rows"]}
    assert rows == set(req["set"]), "every new launch.txt row carries its own rule"


def test_sin_writes_its_own_preset_and_sets_the_renderer_early():
    for name, moddir, game in (("Play SiN - Voodoo.bat", "base", ""),
                               ("Play Wages of SiN - Voodoo.bat", "2015", "+set game 2015 ")):
        cmds = _cmds(sw9.crlf(sw9.TITLES["SiNGold"]["bats"][name]))
        cfg = [c for c in cmds if c.endswith(">>%s\\fleetres.cfg" % moddir)]
        for want in ('set vid_ref "gl"', 'set gl_driver "3dfxgl"', 'set gl_mode "3"',
                     'set con_texmode "1"', 'set vid_fullscreen "1"'):
            assert "echo %s>>%s\\fleetres.cfg" % (want, moddir) in cfg, want
        assert not any("restart" in c for c in cfg), "never vid_restart / snd_restart"
        run = [c for c in cmds if c.startswith("start /w sin.exe")][0]
        assert run == ("start /w sin.exe %s+set vid_ref gl +set gl_driver 3dfxgl "
                       "+set gl_mode 3 +set vid_fullscreen 1 +set s_khz 11" % game)
        assert len(run) <= 126


def test_half_life_names_the_mini_driver_it_staged():
    hl = sw9.TITLES["HalfLife1"]
    assert hl["copies"]["3dfxgl.dll"] == hl["copies"]["gldrv/3dfxgl.dll"] == \
        {"lib": "CounterStrike16/gldrv/3dfxgl.dll", "md5": "d3d7c67f2004431488d03b801804c601"}
    reg = sw9.crlf(hl["files"]["HLV9X.REG"])
    assert reg.startswith("REGEDIT4\r\n"), "REGEDIT4 - Win9x ignores the v5 dialect"
    for v in ('"EngineModeBPP"=dword:00000010', '"ScreenBPP"=dword:00000010',
              '"ScreenWidth"=dword:00000280', '"ScreenHeight"=dword:000001e0',
              '"EngineModeW"=dword:00000280', '"EngineModeH"=dword:000001e0'):
        assert v + "\r\n" in reg, v       # 640x480x16 - what -w/-h say, too
    for name, game in (("Play Half-Life - Voodoo.bat", ""),
                       ("Play Team Fortress Classic - Voodoo.bat", " -game tfc"),
                       ("Play Deathmatch Classic - Voodoo.bat", " -game dmc")):
        cmds = _cmds(sw9.crlf(hl["bats"][name]))
        i = cmds.index("if exist HLV9X.REG start /w regedit /s HLV9X.REG")
        assert cmds[i + 1] == ("start /w hl.exe -nosierra -full -gl -gldrv 3dfxgl.dll "
                               "-w 640 -h 480 -toconsole" + game)
    assert not any("gearbox" in t or "bshift" in t for t in hl["bats"].values()), \
        "Opposing Force and Blue Shift are broken in this tree"


def test_the_quake2_packs_reuse_the_proven_mechanism():
    q = sw9.TITLES["Quake2Complete"]
    for name, mod in (("Play Quake II - The Reckoning - Voodoo.bat", "xatrix"),
                      ("Play Quake II - Ground Zero - Voodoo.bat", "rogue")):
        cmds = _cmds(sw9.crlf(q["bats"][name]))
        assert 'echo set gl_driver "3dfxgl">>%s\\fleetres.cfg' % mod in cmds
        assert 'echo set gl_mode "3">>%s\\fleetres.cfg' % mod in cmds
        assert "start /w quake2.exe +set game %s" % mod in cmds
    assert q["requires"]["drop_top"] == ["min_os"], \
        "a title-level min_os win2k refuses the whole tree on Win9x"


# --- Unreal Gold's Voodoo.ini -----------------------------------------------------

INI = ("[Engine.Engine]\r\nGameRenderDevice=D3DDrv.D3DRenderDevice\r\n"
       "WindowedRenderDevice=SoftDrv.SoftwareRenderDevice\r\n\r\n"
       "[WinDrv.WindowsClient]\r\nFullscreenViewportX=1024\r\n\r\n"
       "[Galaxy.GalaxyAudioSubsystem]\r\nOutputRate=22050Hz\r\n")


def test_ini_set_replaces_in_place_and_adds_at_the_section_end():
    out = sw9.ini_set(INI, [("Engine.Engine", "GameRenderDevice", "GlideDrv.GlideRenderDevice"),
                            ("winDrv.windowsclient", "fullscreenviewportx", "640"),
                            ("WinDrv.WindowsClient", "FullscreenColorBits", "16")])
    assert "GameRenderDevice=GlideDrv.GlideRenderDevice\r\n" in out
    assert "FullscreenViewportX=640\r\nFullscreenColorBits=16\r\n\r\n[Galaxy" in out, \
        "a missing key goes at the END of its own section, before the blank line"
    assert "WindowedRenderDevice=SoftDrv.SoftwareRenderDevice" in out
    assert "\n" not in out.replace("\r\n", ""), "the file's own CRLF"
    with pytest.raises(SystemExit, match="sections not in the ini"):
        sw9.ini_set(INI, [("Nope.Section", "X", "1")])


def test_unreal_voodoo_ini_keeps_glide_in_a_window_too():
    edits = {(s, k): v for s, k, v in sw9.UNREAL_VOODOO}
    for k in ("GameRenderDevice", "WindowedRenderDevice", "RenderDevice"):
        assert edits[("Engine.Engine", k)] == "GlideDrv.GlideRenderDevice", k
    assert edits[("WinDrv.WindowsClient", "FullscreenColorBits")] == "16"
    assert (edits[("WinDrv.WindowsClient", "FullscreenViewportX")],
            edits[("WinDrv.WindowsClient", "FullscreenViewportY")]) == ("640", "480")
    cmds = _cmds(sw9.crlf(sw9.TITLES["UnrealGold"]["bats"]["Play Unreal Gold - Voodoo.bat"]))
    run = cmds.index("start /w Unreal.exe -INI=Voodoo.ini")
    assert cmds.index("if exist System\\GLIDE2X.DLL del System\\GLIDE2X.DLL") < run
    assert cmds.index("if exist Running.ini del Running.ini") < run


def test_unreal_runs_a_fresh_copy_of_a_template_never_a_library_file():
    """UE1 writes its system ini back on exit - and after a failed device start
    it saves the fallback, SoftDrv. A library-shipped ini run directly would be
    edited on the box (GAMESYNC recopies it, the icons are rebuilt) and a
    fallback would stick; a fresh copy of a template at every start has neither."""
    ug = sw9.TITLES["UnrealGold"]
    assert set(ug["derive"]) == {"System/Voodoo.tpl"}, "the library ships the TEMPLATE"
    cmds = _cmds(sw9.crlf(ug["bats"]["Play Unreal Gold - Voodoo.bat"]))
    cd, run = cmds.index("cd System"), cmds.index("start /w Unreal.exe -INI=Voodoo.ini")
    cp = cmds.index("copy /y Voodoo.tpl Voodoo.ini > nul")
    assert cd < cp < run, "copied in System\\, before the game, at EVERY start"
    assert ug["remove"] == {"System/Voodoo.ini": "26b61ecc0a3fa74c486765acbc4dc402"}


class _RecRm:
    kind = "rec"

    def __init__(self):
        self.written, self.removed = [], []

    def write_bytes(self, path, data):
        self.written.append(os.path.basename(path))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)

    def remove(self, path):
        self.removed.append(os.path.basename(path))
        os.remove(path)


def _rm_lib(tmp_path, body):
    t = tmp_path / "T"
    (t / "System").mkdir(parents=True)
    if body is not None:
        (t / "System" / "VOODOO.INI").write_bytes(body)
    return str(tmp_path)


def test_remove_takes_only_the_bytes_it_pins_and_goes_last(tmp_path):
    import hashlib
    old = b"[Engine.Engine]\r\nGameRenderDevice=GlideDrv.GlideRenderDevice\r\n"
    spec = {"files": {"System/Voodoo.tpl": "new\n"},
            "remove": {"System/Voodoo.ini": hashlib.md5(old).hexdigest()}}
    lib = _rm_lib(tmp_path, old)
    chk = sw9.Runner(lib, check=True, writer=_RecRm(), titles={"T": spec}).run()
    assert any("should be removed" in p for p in chk.problems), chk.problems
    rec = _RecRm()
    r = sw9.Runner(lib, writer=rec, titles={"T": spec}).run()
    assert r.problems == [] and rec.written == ["Voodoo.tpl"] and rec.removed == ["VOODOO.INI"]
    assert "T/System/Voodoo.ini [removed]" in r.changed
    again = sw9.Runner(lib, writer=_RecRm(), titles={"T": spec}).run()
    assert again.changed == [] and again.current == 2, "absent is current"
    # somebody else's file under that name is left alone, loudly
    lib2 = _rm_lib(tmp_path / "x", b"edited on purpose\r\n")
    rec2 = _RecRm()
    r2 = sw9.Runner(lib2, writer=rec2, titles={"T": spec}).run()
    assert rec2.removed == [] and any("left alone" in p for p in r2.problems)

def _run_moves(lines, files):
    """The `if [not] exist A if exist B move/copy B A` lines of a launcher block,
    against a set of file names - enough to say what each box ends up with."""
    files = set(files)
    for line in lines:
        m = re.match(r'\s*if not exist "%~dp0([^"]+)" if exist "%~dp0([^"]+)" '
                     r'(move|copy) /y "%~dp0([^"]+)" "%~dp0([^"]+)"', line)
        if m:
            dst, src, verb = m.group(1), m.group(2), m.group(3)
            if dst not in files and src in files:
                files.add(dst)
                if verb == "move":
                    files.discard(src)
            continue
        m = re.match(r'\s*if exist "%~dp0([^"]+)" move /y "%~dp0([^"]+)" "%~dp0([^"]+)"', line)
        if m and m.group(1) in files:
            files.discard(m.group(1))
            files.add(m.group(3))
    return files


def test_the_library_ships_the_wrappers_inert():
    c2 = sf.TITLES["Carmageddon2"]["renames"]
    assert set(c2) == {"glide2x.dll", "glide3x.dll", "glide.dll", "wsock32.dll",
                       "mswsock.dll", "dpwsockx.dll", "ipxwrapper.dll"}
    for old, (new, md5) in c2.items():
        assert new in (old + ".nglide", old + ".ipxw") and re.match(r"^[0-9a-f]{32}$", md5)
    ug = sf.TITLES["UnrealGold"]["renames"]
    assert ug == {"System/glide2x.dll": ("System/glide2x.dll.nglide",
                                         "e4867e049c2ece234254c15f441ffc48")}


def test_an_nt_box_without_glide_still_gets_nglide_and_ipxwrapper():
    nglide = [l for pb in sf.TITLES["Carmageddon2"]["post"] for l in pb["lines"]]
    lib = {n + ".nglide" for n in sf.NGLIDE_MD5} | {n + ".ipxw" for n in sf.IPXW_SET}
    # FR_GLIDE=0: the else branch of glide_swap runs, then the IPXWrapper copy
    swap = sf.glide_swap("glide2x.dll", "glide3x.dll", "glide.dll")
    restore = swap[swap.index(") else (") + 1:-1]
    box = _run_moves(restore + sf.ipxwrapper_live(), lib)
    assert {"glide2x.dll", "glide3x.dll", "glide.dll"} <= box
    assert set(sf.IPXW_SET) <= box
    assert lib <= box, \
        "COPIED, not moved: the library's files stay, GAMESYNC has nothing to put back"
    assert any("ipxwrapper_live" in l for l in nglide)


@pytest.mark.parametrize("glide", (True, False))
@pytest.mark.parametrize("synced_before", (True, False))
def test_no_launch_ever_takes_a_library_file_away(glide, synced_before):
    """Steady state: whatever the box and its history, after a launch every
    file the library ships is still on the box - so the next GAMESYNC writes
    nothing (a box must report 0 file(s) written when nothing changed)."""
    lib = {n + ".nglide" for n in sf.NGLIDE_MD5} | {n + ".ipxw" for n in sf.IPXW_SET}
    box = set(lib)
    if synced_before:                     # the live names an older library shipped
        box |= set(sf.NGLIDE_MD5) | set(sf.IPXW_SET)
    swap = sf.glide_swap("glide2x.dll", "glide3x.dll", "glide.dll")
    cut = swap.index(") else (")
    branch = swap[2:cut] if glide else swap[cut + 1:-1]
    for _ in range(2):                    # and again at the next launch
        box = _run_moves(sf.ipxwrapper_live() + branch, box)
        assert lib <= box, sorted(lib - box)
    if glide:
        assert not box & set(sf.NGLIDE_MD5), "the wrapper must not shadow a real Voodoo"
    else:
        assert set(sf.NGLIDE_MD5) <= box


def test_an_nt_box_with_glide_keeps_the_wrapper_out_of_the_way():
    swap = sf.glide_swap("glide2x.dll", "glide3x.dll", "glide.dll")
    aside = swap[2:swap.index(") else (")]
    old_box = {"glide2x.dll", "glide3x.dll", "glide.dll"}         # synced before
    assert _run_moves(aside, old_box) == {n + ".nglide" for n in sf.NGLIDE_MD5}
    fresh = {n + ".nglide" for n in sf.NGLIDE_MD5}
    assert _run_moves(aside, fresh) == fresh


def test_the_validator_accepts_both_lanes(tmp_path):
    d = tmp_path / "Carmageddon2"
    d.mkdir()
    nt = "\r\n".join(["@echo off", sf.CALL] + sf.ipxwrapper_live()
                     + sf.glide_swap("glide2x.dll", "glide3x.dll", "glide.dll")
                     + ['start "" CARMA2_HW.EXE']) + "\r\n"
    (d / "Play Carmageddon 2.bat").write_bytes(nt.encode())
    (d / "Play Carmageddon 2 - Voodoo.bat").write_bytes(
        sw9.crlf(sw9.CARMA2_BAT).encode())
    (d / "FLEETRES.BAT").write_bytes(b"@echo off\r\n")
    (d / "FLEETRES.EXE").write_bytes(b"MZ")
    (d / "launch.txt").write_bytes(b"Play Carmageddon 2.bat\tC2\tFLEETRES.EXE\r\n")
    probs = [p for p in vl.check_title(str(tmp_path), "Carmageddon2")
             if p.check.startswith("fleetres")]
    assert probs == [], [p.as_dict() for p in probs]


# --- the share ---------------------------------------------------------------------

@_share
def test_the_wrappers_are_inert_on_the_share():
    r = sf.Runner(LIB, False, True, writer=object())
    r.run(only={"UnrealGold", "Carmageddon2"})
    assert r.errors == [], r.errors
    for rel in ("UnrealGold/System/glide2x.dll", "Carmageddon2/glide2x.dll",
                "Carmageddon2/wsock32.dll"):
        assert not os.path.exists(os.path.join(LIB, rel)), rel
