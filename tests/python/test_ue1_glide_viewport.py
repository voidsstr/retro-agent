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
    """A small model of what cmd.exe does with these single-line ifs."""
    env = dict(env)
    writes = {}
    for line in lines:
        x = line
        for k, v in env.items():
            x = x.replace("%" + k + "%", v)
        m = re.fullmatch(r"set (\w+)=(\d+)", x)
        if m:
            env[m.group(1)] = m.group(2); continue
        m = re.fullmatch(r"if (\d+) GEQ (\d+) if (\d+) GEQ (\d+) set (\w+)=(\d+)", x)
        if m:
            if int(m.group(1)) >= int(m.group(2)) and int(m.group(3)) >= int(m.group(4)):
                env[m.group(5)] = m.group(6)
            continue
        m = re.fullmatch(r'if /i "([^"]*)"=="([^"]*)" if exist "[^"]+" "[^"]+" -ini "[^"]+" '
                         r"WinDrv\.WindowsClient (FullscreenViewport[XY]) (\d+)", x)
        assert m, "unmodelled line: " + line
        if m.group(1).lower() == m.group(2).lower():
            writes[m.group(3)] = int(m.group(4))
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
    w = _run(LINES, {"FR_W43": "1280", "FR_H43": "960", "FR_UE1DEV": "D3DDrv.D3DRenderDevice"})
    assert w == {}


def test_no_line_sits_in_a_parenthesised_block_and_softdrv_is_never_written():
    assert not any("(" in l or ")" in l for l in LINES)
    post = SF.TITLES["UnrealGold"]["post"][0]["lines"]
    assert LINES == post[-len(LINES):], "the ladder must run AFTER the viewport the launcher wrote"
    assert not any("SoftDrv" in l for l in post)
