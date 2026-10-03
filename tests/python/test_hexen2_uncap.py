"""Hexen II's 1024x768 cap is lifted only where the 3dfx card drives the screen
(stage-fleetres.py h2_uncap, 2026-10-02).

The share's launchers call FLEETRES with `-cap 1024 768` because glh2.exe on
.240 (ATI X800) refused 1920x1080, 1280x1024 and 1280x960 ("Specified video
mode not available"). The mode list is the driver's: on .124 (Voodoo 5 6000,
our vcr-kmd + ICD) glh2 opened 1280x960x32@85 in 4-chip SLI, rendered its menu
and quit cleanly. The block re-runs FLEETRES without the cap there and nowhere
else.
"""
import importlib.util
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("sf_h2", REPO / "scripts" / "fleet" / "stage-fleetres.py")
SF = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(SF)

GLIDE = SF.UE1_GLIDE_DEV
SHARE_PLAY = ('@echo off\r\ncd /d "%~dp0"\r\ncall "%~dp0FLEETRES.BAT" -cap 1024 768\r\n'
              '\r\nstart "" glh2.exe -width %FR_W% -height %FR_H% -heapsize 32768 -nocdaudio\r\n'
              'exit\r\n')


def _calls(lines, dev):
    """the FLEETRES calls this box makes, in order: [args, ...]"""
    out = []
    for line in lines:
        x = line.replace("%FR_UE1DEV%", dev or "")
        if x.lower().startswith("rem "):
            continue
        m = re.fullmatch(r'if /i "([^"]*)"=="([^"]*)" call "%~dp0FLEETRES\.BAT"(.*)', x)
        assert m, "unmodelled line: " + line
        if m.group(1).lower() == m.group(2).lower():
            out.append(m.group(3).strip())
    return out


def test_a_glide_box_calls_fleetres_again_without_the_cap():
    assert _calls(SF.h2_uncap(), GLIDE) == [""]


def test_every_other_box_keeps_the_cap():
    for dev in ("D3DDrv.D3DRenderDevice", "", None):
        assert _calls(SF.h2_uncap(), dev) == [], dev


def test_all_three_glh2_launchers_carry_it_before_their_start_line():
    posts = {pb["file"]: pb for pb in SF.TITLES["HexenII"]["post"]}
    assert set(posts) == set(SF.H2_LAUNCHERS)
    for pb in posts.values():
        assert pb["marker"] == "H2_UNCAP" and pb["before"] == SF.H2_START
        assert not any("(" in l or ")" in l for l in pb["lines"][1:])


def test_the_block_lands_after_the_capped_call_and_once(tmp_path):
    t = tmp_path / "HexenII"
    t.mkdir()
    (t / "Play Hexen II.bat").write_bytes(SHARE_PLAY.encode("latin1"))
    pb = [pb for pb in SF.TITLES["HexenII"]["post"] if pb["file"] == "Play Hexen II.bat"][0]
    r = SF.Runner(str(tmp_path), False, False)
    r.post_block(str(t), "HexenII", pb)
    once = (t / "Play Hexen II.bat").read_bytes().decode("latin1")
    cap = once.index('-cap 1024 768')
    again = once.index(pb["lines"][1])
    assert cap < again < once.index(SF.H2_START)
    r.post_block(str(t), "HexenII", pb)
    assert (t / "Play Hexen II.bat").read_bytes().decode("latin1") == once
    assert not r.errors
