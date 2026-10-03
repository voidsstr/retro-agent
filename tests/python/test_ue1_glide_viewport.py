"""Unreal Gold on a Glide box gets an exact 4:3 Glide 2 mode (stage-fleetres.py
ue1_glide_viewport, 2026-09-28).

GlideDrv maps the viewport to a Glide 2 resolution by <= tests. FLEETRES' 4:3
pair on .124's 1280x1024 desktop is 1280x960, which opened 1280x1024 - 5:4 and
the desktop's own size, through Glide's 8 bpp DirectDraw detour - and Unreal
Gold hung in its second Glide open on the V5 6000. At 1024x768 it started in
6.8 s, rendered and exited cleanly ("Glide shut down"). The render devices are
left alone on purpose: WindowedRenderDevice=SoftDrv strands the game on the
software rasterizer after Unreal's splash takes the focus.
"""
import importlib.util
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("sf", REPO / "scripts" / "fleet" / "stage-fleetres.py")
SF = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(SF)


def _run(lines, env):
    """A small model of what cmd.exe does with these single-line ifs. An
    undefined %VAR% expands to nothing, as it does in a batch file."""
    env = {k: v for k, v in env.items() if v is not None}
    writes = {}
    for line in lines:
        if line.lower().startswith("rem "):
            continue
        m = re.fullmatch(r"set /a (\w+)=(\w+)\+0 >nul 2>nul", line)
        if m:
            src = env.get(m.group(2), "")
            if src == "":
                env[m.group(1)] = "0"
            elif re.fullmatch(r"\d+", src):
                env[m.group(1)] = str(int(src))
            continue                       # a non-number: set /a fails, value kept
        x = re.sub(r"%(\w+)%", lambda mm: env.get(mm.group(1), ""), line)
        m = re.fullmatch(r"set (\w+)=(\d*)", x)
        if m:
            if m.group(2):
                env[m.group(1)] = m.group(2)
            else:
                env.pop(m.group(1), None)
            continue
        m = re.fullmatch(r"if (\d+) GEQ (\d+) if (\d+) GEQ (\d+) set (\w+)=(\d+)", x)
        if m:
            if int(m.group(1)) >= int(m.group(2)) and int(m.group(3)) >= int(m.group(4)):
                env[m.group(5)] = m.group(6)
            continue
        m = re.fullmatch(r"if (\S+) GEQ (\d+) set (\w+)=(\d+)", x)
        if m:
            assert re.fullmatch(r"\d+", m.group(1)), "IF on a non-number ends the batch: " + x
            if int(m.group(1)) >= int(m.group(2)):
                env[m.group(3)] = m.group(4)
            continue
        guard = True
        m = re.match(r"if defined (\w+) (.*)", x)
        if m:
            guard, x = m.group(1) in env, m.group(2)
        m = re.fullmatch(r'if /i "([^"]*)"=="([^"]*)" if exist "[^"]+" "[^"]+" -ini "[^"]+" '
                         r"(WinDrv\.WindowsClient|GlideDrv\.GlideRenderDevice|Engine\.Engine) "
                         r"(FullscreenViewport[XY]|RefreshRate|GameRenderDevice|"
                         r"WindowedRenderDevice|RenderDevice) ([\w.]+)", x)
        assert m, "unmodelled line: " + line
        if guard and m.group(1).lower() == m.group(2).lower():
            v = m.group(5)
            writes[m.group(4)] = int(v) if v.isdigit() else v
    return writes


LINES = SF.ue1_glide_viewport("System\\Unreal.ini")
GLIDE = "GlideDrv.GlideRenderDevice"


def test_124_gets_1024x768_not_the_desktop_size():
    w = _run(LINES, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": GLIDE})
    assert (w["FullscreenViewportX"], w["FullscreenViewportY"]) == (1024, 768)
    assert (1280, 1024) != (w["FullscreenViewportX"], w["FullscreenViewportY"])


def test_the_ladder_takes_the_largest_exact_4_3_mode_that_fits():
    for (w43, h43), want in {(800, 600): (800, 600), (1024, 768): (1024, 768),
                             (1600, 1200): (1600, 1200), (1920, 1440): (1600, 1200),
                             (640, 480): (640, 480)}.items():
        w = _run(LINES, {"FR_W43": str(w43), "FR_H43": str(h43), "FR_UE1DEV": GLIDE})
        assert (w["FullscreenViewportX"], w["FullscreenViewportY"]) == want, (w43, h43)


def test_a_d3d_box_is_untouched():
    w = _run(LINES, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": "D3DDrv.D3DRenderDevice",
                     "FR_HZ": "85"})
    assert w == {}


def test_124_glide_refresh_is_the_monitors_85_not_the_staged_60():
    """2026-10-02: the staged RefreshRate=60Hz flickered on .124's CRT and,
    under vsync, capped the game at 60 fps."""
    w = _run(LINES, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": GLIDE, "FR_HZ": "85"})
    assert w["RefreshRate"] == "85Hz"


def test_the_refresh_is_the_largest_glidedrv_rate_not_above_the_monitors():
    for hz, want in {"60": "60Hz", "75": "75Hz", "100": "100Hz", "120": "120Hz",
                     "87": "85Hz", "160": "120Hz", "71": "70Hz"}.items():
        w = _run(LINES, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": GLIDE, "FR_HZ": hz})
        assert w["RefreshRate"] == want, hz
    for hz in ("0", "", None, "59", "85Hz"):          # no measured rate: the ini is left alone
        w = _run(LINES, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": GLIDE, "FR_HZ": hz})
        assert "RefreshRate" not in w, hz
    assert SF.UE1_GLIDE_HZ == (60, 70, 72, 75, 80, 85, 90, 100, 120)


def test_no_line_sits_in_a_parenthesised_block_and_softdrv_is_never_written():
    assert not any("(" in l or ")" in l for l in LINES)
    post = SF.TITLES["UnrealGold"]["post"][0]["lines"]
    assert LINES == post[-len(LINES):], "the ladder must run AFTER the viewport the launcher wrote"
    assert not any("SoftDrv" in l for l in post)


# --------------------------------------------------------------------------
# UT 436 and Deus Ex join Unreal Gold on Glide where the 3dfx card drives the
# screen (ue1_glide_device, 2026-10-02): UTbench on .124 - GlideDrv
# 1024x768x16 63.4 fps, OpenGLDrv 57.0 at 1024x768 and 1280x960 (CPU-bound).
# --------------------------------------------------------------------------
DEVICE = SF.ue1_glide_device("System\\UnrealTournament.ini")


def test_a_glide_box_renders_ut436_and_deus_ex_through_glidedrv():
    w = _run(DEVICE, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": GLIDE, "FR_HZ": "85"})
    assert w == {"GameRenderDevice": GLIDE, "WindowedRenderDevice": GLIDE,
                 "RenderDevice": GLIDE, "FullscreenViewportX": 1024,
                 "FullscreenViewportY": 768, "RefreshRate": "85Hz"}


def test_every_other_box_keeps_its_staged_device():
    for dev in ("D3DDrv.D3DRenderDevice", "", None):
        assert _run(DEVICE, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": dev,
                             "FR_HZ": "85"}) == {}, dev


def test_the_player_launchers_carry_it_after_their_own_viewport_and_the_server_does_not():
    want = {("UnrealTournament436", "Play Unreal Tournament 436.bat"),
            ("UnrealTournament436", "Join fleet UT99 server - 436.bat"),
            ("DeusEx", "Play Deus Ex.bat")}
    got = {(t, pb["file"]) for t in ("UnrealTournament436", "DeusEx")
           for pb in SF.TITLES[t].get("post", []) if pb["marker"] == "UE1_GLIDE"}
    assert got == want
    for t, f in want:
        pb = [pb for pb in SF.TITLES[t]["post"] if pb["file"] == f][0]
        assert pb["before"] == 'cd /d "%~dp0System"'      # after ue_ini's writes
        assert not any("(" in l or ")" in l for l in pb["lines"][1:])
        assert not any("SoftDrv" in l for l in pb["lines"])


def test_the_block_lands_once_between_the_viewport_and_the_cd(tmp_path):
    t = tmp_path / "UnrealTournament436"
    t.mkdir()
    body = ('@echo off\r\ncall "%~dp0FLEETRES.BAT"\r\nif exist "%~dp0FLEETRES.EXE" (\r\n'
            '  "%~dp0FLEETRES.EXE" -ini "%~dp0System\\UnrealTournament.ini" WinDrv.WindowsClient '
            'FullscreenViewportX %FR_W%\r\n)\r\n\r\ncd /d "%~dp0System"\r\n'
            'start "" "UnrealTournament.exe"\r\nexit\r\n')
    (t / "Play Unreal Tournament 436.bat").write_bytes(body.encode("latin1"))
    pb = [pb for pb in SF.TITLES["UnrealTournament436"]["post"]
          if pb["file"] == "Play Unreal Tournament 436.bat"][0]
    r = SF.Runner(str(tmp_path), False, False)
    r.post_block(str(t), "UnrealTournament436", pb)
    once = (t / "Play Unreal Tournament 436.bat").read_bytes().decode("latin1")
    assert once.count(pb["lines"][0]) == 1
    assert once.index("FullscreenViewportX %FR_W%") < once.index(pb["lines"][1]) < once.index('cd /d "%~dp0System"')
    r.post_block(str(t), "UnrealTournament436", pb)
    assert (t / "Play Unreal Tournament 436.bat").read_bytes().decode("latin1") == once
    assert not r.errors
