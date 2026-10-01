"""Descent 1 + 2 in the library: speed, LAN lanes, the DOS game (research R2).

MEASURED, and pinned here so a later edit cannot quietly undo it:

* TOO FAST: the Parallax engine clamps its frame time to AT LEAST 1/150 s - the
  same instructions in DESCENTR.EXE, DESCENT2.EXE, DESCENTW.EXE and D2VOODOO.EXE
  (81 FB B4 01 00 00 7D 05 BB B4 01 00 00 ..., 0x1B4 = 65536/150). Above 150 fps
  the game runs fps/150 times real speed; DOSBox at cycles=max on a 2-3 GHz box
  gets there. Every DOSBox Descent conf caps cycles: `max limit 90000`.
* XP's ipconfig prints "IP Address", Windows 7's "IPv4 Address" - a Host
  launcher that greps for one prints nothing on the other.
* DXX-Rebirth 0.58.1 has NO IPX code: it cannot meet the DOS build, whatever
  the old launcher comment said.
* Descent II's DOS game: the retail DESCENT2.EXE needs the CD; the share's no-CD
  build (md5 1ad00b09...) passes the check. The game loads hmidrv.386/hmidet.386
  by name and the tree had none. DESCENTW.EXE imports iforce.dll, which was not
  in the tree. Its SETUP.EXE's device table gives "Sound Blaster 16" the HMI ids
  {0xe016, 0xe018} and the OPL3 MIDI id 0xa009.
"""
import importlib.util
import json
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


sf = _load("stage_fleetres_desc", os.path.join(FLEET, "stage-fleetres.py"))
sw9 = _load("stage_win9x_desc", os.path.join(FLEET, "stage-win9x.py"))
lm = _load("libmeta_desc", os.path.join(FLEET, "libmeta.py"))

LIB = "/mnt/retro-share/Files/Games-Library"
_share = pytest.mark.skipif(not os.path.isdir(LIB),
                            reason="staged library not mounted at %s - the share "
                                   "side was NOT checked" % LIB)
D1, D2 = sf.TITLES["Descent1"], sf.TITLES["Descent2"]


def _cmds(text):
    return [l.strip() for l in text.replace("\r\n", "\n").split("\n")
            if l.strip() and not l.strip().lower().startswith(("rem", "@echo"))]


# --- speed ------------------------------------------------------------------------

def test_every_dosbox_descent_conf_caps_cycles():
    old, new = sf.DOSBOX_CYCLES_FIX
    assert "cycles=max\r\n" in old and old not in new, \
        "the repair pair must be applied once and recognised as applied"
    assert "cycles=max limit 90000\r\n" in new
    assert D1["fix"]["dosboxD1.conf"] == [sf.DOSBOX_CYCLES_FIX]
    conf = D2["files"]["dosboxD2.conf"]
    assert re.search(r"(?m)^cycles=max limit 90000$", conf)
    assert not re.search(r"(?m)^cycles=max$", conf)
    assert "1/150" in "\n".join(sf.DOSBOX_CYCLES_NOTE), "say WHY, in the conf"


def test_the_d2_dosbox_conf_is_the_d1_pattern():
    conf = D2["files"]["dosboxD2.conf"]
    for line in ("fullscreen=true", "fullresolution=original", "sbtype=sb16",
                 "sbbase=220", "irq=7", "dma=1", "hdma=5", "ipx=true", "machine=svga_s3"):
        assert re.search(r"(?m)^%s$" % re.escape(line), conf), line
    single = D2["files"]["dosboxD2_single.conf"]
    assert "ipx=true" in single and 'mount C ".."' in single
    assert "DESCENT2.EXE -noredbook" in single.split("[autoexec]")[1]


# --- Host launchers print the address on XP AND Windows 7 ---------------------------

XP_IPCONFIG = "        IP Address. . . . . . . . . . . . : 192.168.1.123\n"
W7_IPCONFIG = "   IPv4 Address. . . . . . . . . . . : 192.168.1.197\n"


def _findstr(line, out):
    lits = re.findall(r'/c:"([^"]+)"', line)
    return [l for l in out.splitlines() if any(s.lower() in l.lower() for s in lits)]


def test_the_ipconfig_line_matches_xp_and_windows_7():
    old, new = sf.IPCONFIG_FIX
    assert old not in new
    assert _findstr(new, XP_IPCONFIG) and _findstr(new, W7_IPCONFIG)
    assert not _findstr(new, "   Link-local IPv6 Address . . . . . : fe80::1\n")
    for name in ("Host Descent - LAN.bat",):
        assert sf.IPCONFIG_FIX in D1["fix"][name]
    assert sf.IPCONFIG_FIX in D2["fix"]["Host Descent 2 - LAN.bat"]
    for text in (D1["files"]["Host Descent - Rebirth LAN.bat"],
                 D2["files"]["Host Descent 2 - DOSBox LAN.bat"]):
        assert sf.IPCONFIG_LINE in text and old not in text


# --- Rebirth ------------------------------------------------------------------------

def test_the_rebirth_comments_no_longer_claim_ipx():
    old, new = sf.REBIRTH_IPX_FIX
    assert "speaks IPX" in old and "no IPX" in new
    assert sf.REBIRTH_IPX_FIX in D1["fix"]["Play Descent - Rebirth.bat"]
    assert sf.REBIRTH_IPX_FIX in D2["fix"]["Play Descent 2.bat"]


def _pilot(computername):
    """The pilot lines' arithmetic, step by step, as cmd.exe does it."""
    v = computername
    if not v:
        v = "fleet"
    for ch in "-_. ":
        v = v.replace(ch, "")
    if not v:
        v = "fleet"
    return v[:8]


@pytest.mark.parametrize("name,pilot", [
    ("2004-XP", "2004XP"), ("NSC-C543575F526", "NSCC5435"), ("USER-41EA3B3330", "USER41EA"),
    ("ADMIN-PC", "ADMINPC"), ("DELL", "DELL"), ("---", "fleet"), ("", "fleet")])
def test_every_box_flies_its_own_pilot(name, pilot):
    assert _pilot(name) == pilot


def test_the_pilot_block_is_ordered_safely():
    lines = sf.rebirth_pilot()
    sets = [l for l in lines if not l.lower().startswith("rem")]
    assert sets[0] == 'set "DXXPILOT=%COMPUTERNAME%"'
    sub = sets.index('set "DXXPILOT=%DXXPILOT:~0,8%"')
    assert sets[sub - 1] == 'if not defined DXXPILOT set "DXXPILOT=fleet"', \
        "never take a substring of a variable that may be undefined"
    assert sets[-1].startswith('if not exist "%~dp0%DXXPILOT%.plr" if exist "%~dp0PLAYER.PLR" copy')
    for ch in "-_. ":
        assert 'set "DXXPILOT=%%DXXPILOT:%s=%%"' % ch in sets


@pytest.mark.parametrize("name,join", [("Host Descent - Rebirth LAN.bat", False),
                                       ("Join Descent - Rebirth LAN.bat", True)])
def test_descent1_rebirth_lan_launchers(name, join):
    text = D1["files"][name]
    cmds = _cmds(text)
    start = [c for c in cmds if c.startswith('start "" "%~dp0d1x-rebirth.exe"')]
    want = '-pilot %DXXPILOT%' + (' -udp_hostaddr %HOSTIP%' if join else '')
    assert start == ['start "" "%~dp0d1x-rebirth.exe" ' + want], start
    assert cmds.index(sf.CALL) < cmds.index(start[0])
    assert text.index("per-box PILOT") < text.index('cd /d "%~dp0"')
    assert "(" not in name and ")" not in name
    assert ("set /p" in text) == join


def test_play_rebirth_flies_the_same_pilot():
    pb, = [p for p in D1["post"] if p["file"] == "Play Descent - Rebirth.bat"]
    assert pb["lines"] == sf.rebirth_pilot() and pb["before"] == 'cd /d "%~dp0"', \
        "anchored on the cd - the fix below rewrites the start line"
    assert ('start "" "%~dp0d1x-rebirth.exe" -pilot fleet',
            'start "" "%~dp0d1x-rebirth.exe" -pilot %DXXPILOT%') in \
        D1["fix"]["Play Descent - Rebirth.bat"]


def test_descent1_rules():
    req = D1["requires"]
    for n in ("Host Descent - Rebirth LAN.bat", "Join Descent - Rebirth LAN.bat"):
        rule = req["set"][n]
        assert rule["cpu_features"] == ["cmov"] and rule["min_cpu_mhz"] == 200
        assert rule["min_os"] == "win2k" and rule["min_ram_mb"] == 32
        assert "mmx" not in rule["cpu_features"]
    assert set(req["patch"]) == {"Play Descent.bat", "Host Descent - LAN.bat",
                                 "Join Descent - LAN.bat", "Descent Sound Setup.bat",
                                 "Play Descent - Rebirth.bat"}
    assert all(v == {"min_os": "win2k"} for v in req["patch"].values())
    rows = D1["launch_merge"]["rows"]
    assert {r[0] for r in rows} == set(req["set"])


# --- Descent II: the DOS game ---------------------------------------------------------

def test_the_d2_sound_config_is_setups_sb16():
    cfg = sf.d2_descent_cfg(7)
    for line in ("DigiDeviceID8=0xe016", "DigiDeviceID16=0xe018", "DigiPort=0x220",
                 "DigiIrq=7", "DigiDma8=1", "DigiDma16=5", "MidiDeviceID=0xa009",
                 "MidiPort=0x388", "RedbookEnabled=0"):
        assert (line + "\r\n") in cfg, line
    sb5 = sf.d2_descent_cfg(5)
    assert sb5 == cfg.replace("DigiIrq=7\r\n", "DigiIrq=5\r\n") != cfg
    assert D2["files"]["DESCENT.CFG"] == cfg == D2["files"]["DESCENT.SB7"]
    assert D2["files"]["DESCENT.SB5"] == sb5


def test_the_d2_payload_is_pinned():
    c = D2["copies"]
    assert c["DESCENT2.EXE"]["md5"] == "1ad00b094a46d483ed6c64682e00013e"
    assert c["DESCENT2.EXE"]["member"] == "CRACK/DESCENT2.EXE"
    assert c["IFORCE.DLL"] == {"iso": sf.D2_ISO, "member": "D2DATA/DESCENT2.SOW",
                               "arj": "IFORCE.DLL",
                               "md5": "241453f0c45c5b4b212192e7c9ae3ddf"}
    for n in ("HMIDET.386", "HMIDRV.386", "HMIMDRV.386", "DOSBOX/DOSBox.exe",
              "DOSBOX/SDL.dll", "DOSBOX/SDL_net.dll", "_ipxhost.conf"):
        assert re.match(r"^[0-9a-f]{32}$", c[n]["md5"]), n


@pytest.mark.parametrize("kind", ["play", "host", "join"])
def test_the_d2_dosbox_lane_launchers(kind):
    name = {"play": "Play Descent 2 - DOSBox.bat", "host": "Host Descent 2 - DOSBox LAN.bat",
            "join": "Join Descent 2 - DOSBox LAN.bat"}[kind]
    text = D2["files"][name]
    cmds = _cmds(text)
    run = [c for c in cmds if c.startswith("DOSBox.exe ")]
    assert len(run) == 1 and run[0].startswith('DOSBox.exe -conf "..\\dosboxD2.conf"')
    assert run[0].endswith('-conf "..\\dosboxD2_single.conf"')
    assert ('_ipxhost.conf' in run[0]) == (kind == "host")
    assert ('%JC%' in run[0]) == (kind == "join")
    assert cmds.index(sf.CALL) < cmds.index('cd /d "%~dp0DOSBOX"') < cmds.index(run[0])
    assert any("sdl fullresolution %FR_DOSFULLRES%" in c and "dosboxD2.conf" in c for c in cmds)
    assert sf.D2_SOUND_RESTORE[-1] in cmds, "Rebirth drops the DOS sound keys"
    if kind == "join":
        assert cmds.count('>>"%JC%" echo IPXNET CONNECT %HOSTIP%') == 4


def test_d2_rules_keep_cmd_exe_off_win9x_and_the_dos_launcher_on_it():
    req = D2["requires"]
    for n, rule in req["set"].items():
        assert rule["min_cpu_mhz"] == 350 and rule["min_os"] == "win2k", n
        assert rule["requires_capabilities"] == [], "a DOSBox lane needs no disc mounter"
    assert set(req["patch"]) == {"Play Descent 2.bat", "Host Descent 2 - LAN.bat",
                                 "Join Descent 2 - LAN.bat",
                                 "Play Descent 2 - original Win95.bat"}
    win9x = sw9.TITLES["Descent2"]
    assert win9x["requires"]["set"]["D2DOS.BAT"]["max_os"] == "win9x"
    assert "version" not in win9x["requires"], "only ONE generator owns the version"


def test_d2dos_is_the_descent1_pattern():
    text = sw9.crlf(sw9.TITLES["Descent2"]["bats"]["D2DOS.BAT"])
    assert lm.command_com_problems(text) == []
    cmds = _cmds(text)
    i = cmds.index('find "DigiIrq=7" DESCENT.CFG > nul')
    assert cmds[i + 1] == "if not errorlevel 1 copy DESCENT.SB5 DESCENT.CFG > nul"
    assert cmds[i + 2] == "DESCENT2.EXE -noredbook" and cmds[i + 3] == "cls"


# --- the share ------------------------------------------------------------------------

@_share
def test_the_descent_recipes_are_applied():
    r = sf.Runner(LIB, False, True, writer=object())
    r.run(only={"Descent1", "Descent2"})
    assert r.errors == [], ("the share differs from the Descent recipes - run "
                            "stage-fleetres.py --only Descent1 --only Descent2: %s"
                            % r.errors)


@_share
def test_descentw_finds_every_dll_it_imports_in_the_tree_or_windows():
    import pefile
    pe = pefile.PE(os.path.join(LIB, "Descent2", "DESCENTW.EXE"), fast_load=True)
    pe.parse_data_directories()
    names = {n.lower() for n in os.listdir(os.path.join(LIB, "Descent2"))}
    system = {"gdi32.dll", "winmm.dll", "advapi32.dll", "user32.dll", "kernel32.dll",
              "dsound.dll", "tapi32.dll", "wsock32.dll", "ddraw.dll"}
    for imp in pe.DIRECTORY_ENTRY_IMPORT:
        dll = imp.dll.decode().lower()
        assert dll in system or dll in names, \
            "DESCENTW.EXE imports %s, which neither Windows nor the tree has" % dll
