"""Regression: the agent's built-in desktop theme (retrowall.c).

Encodes the 2026-08-04 change making the green-on-black "hacker" theme +
Starfield screensaver the fleet DEFACTO default the agent installs on every
startup — replacing the gray "Windows Standard" scheme it applied before (which
silently reset the fleet to gray on every reboot, undoing the green theme).

Source-invariant tests (parse the C + reg, no Win32 build needed):
1. retrowall.c's color table is green (Window black, text green) — NOT the old
   gray (Window 255,255,255).
2. The agent color table matches scripts/retro-wallpaper/retro_theme.reg, so the
   agent-applied theme and the server-side deploy theme are identical.
3. The agent sets the Starfield screensaver; deploy_rotation stages ssstars.scr
   and points the screensaver at it (works on Win7, which ships none).
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RETROWALL_C = os.path.join(ROOT, "agent", "src", "retrowall.c")
THEME_REG = os.path.join(ROOT, "scripts", "retro-wallpaper", "retro_theme.reg")
DEPLOY = os.path.join(ROOT, "scripts", "retro-wallpaper", "deploy_rotation.py")


def _read(p):
    with open(p, "r", errors="replace") as f:
        return f.read()


def parse_agent_colors():
    """Pull {reg_name: (r,g,b)} from retrowall.c's HACKER_COLORS[] table."""
    src = _read(RETROWALL_C)
    block = src[src.index("HACKER_COLORS[]"):]
    block = block[:block.index("};")]
    out = {}
    for m in re.finditer(r'\{"(\w+)",\s*\w+,\s*(\d+),\s*(\d+),\s*(\d+)\}', block):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return out


def parse_reg_colors():
    out = {}
    for m in re.finditer(r'"(\w+)"="(\d+) (\d+) (\d+)"', _read(THEME_REG)):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return out


def test_agent_theme_is_green_not_gray():
    c = parse_agent_colors()
    assert c["Window"] == (0, 0, 0), "Explorer/window background must be black"
    assert c["WindowText"] == (0, 230, 0), "window text must be green"
    assert c["MenuText"][1] >= 200 and c["MenuText"][0] == 0, "menu text green"
    assert max(c["ButtonFace"]) < 40, "button/dialog face must be near-black"
    # the OLD gray scheme must be gone
    assert c["Window"] != (255, 255, 255), "must NOT be the old white Window"
    assert c["ButtonFace"] != (212, 208, 200), "must NOT be the old gray ButtonFace"


def test_agent_theme_matches_deploy_reg():
    a, r = parse_agent_colors(), parse_reg_colors()
    # every color the agent sets that also appears in the reg must be identical,
    # so a reboot (agent) and a live deploy (reg) produce the same desktop.
    shared = set(a) & set(r)
    assert len(shared) >= 20, "expected the schemes to overlap heavily"
    mism = {k: (a[k], r[k]) for k in shared if a[k] != r[k]}
    assert not mism, "agent theme diverges from retro_theme.reg: %r" % mism


def test_agent_applies_theme_and_starfield_on_startup():
    src = _read(RETROWALL_C)
    startup = src[src.index("retrowall_apply_startup"):]
    assert "apply_hacker_theme()" in startup
    assert "set_starfield_screensaver()" in startup
    assert "apply_windows_classic()" not in startup, "old gray applier must be gone"
    # screensaver setter prefers the staged copy, falls back to system32, and
    # writes the standard screensaver reg values
    ss = src[src.index("set_starfield_screensaver"):]
    assert "ssstars.scr" in ss
    assert "SCRNSAVE.EXE" in ss and "ScreenSaveActive" in ss


def test_deploy_stages_screensaver():
    d = _read(DEPLOY)
    assert "ssstars.scr" in d
    assert "SCRNSAVE.EXE" in d and "ScreenSaveActive" in d
    # ships the .scr next to the deploy script so Win7 (no ssstars.scr) gets it
    assert os.path.exists(os.path.join(ROOT, "scripts", "retro-wallpaper", "ssstars.scr"))
