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


# ---------------------------------------------------------------------------
# 2026-08-29: the Themes service must be left ALONE on Vista and later.
#
# On XP the Themes service applies "Luna", and stopping + disabling it is what
# drops the box to Classic so the green system colours actually show. On Vista
# and later the SAME service drives the Aero visual style, so the identical call
# strips the compositor and leaves the machine looking broken.
#
# This is not hypothetical. .246 (the fleet's only Windows 7 box) had Aero
# restored by hand, and agent v1.65.0 - which shipped WITHOUT this gate - turned
# it straight back off on its very next start:
#
#     [22:54:21][MAIN ] retrowall: Themes service set to Disabled
#
# leaving HKLM\SYSTEM\CurrentControlSet\services\Themes Start=0x4 (DISABLED) and
# the box in Windows Classic. Every agent restart re-broke it, so no amount of
# fixing the box could stick.
#
# The invariant is a source assertion because the real call needs Win32: the one
# call to stop_and_disable_themes() must sit inside an os_is_xp_or_older() test.
# ---------------------------------------------------------------------------


def test_themes_service_is_only_stopped_on_xp_and_older():
    src = _read(RETROWALL_C)

    assert "os_is_xp_or_older" in src, (
        "retrowall.c must be able to tell XP from Vista+ before touching the "
        "Themes service - the same service drives Luna on XP and Aero on Win7"
    )

    # The version probe must key on the major version being below 6 (Vista).
    probe = src[src.index("static int os_is_xp_or_older"):]
    probe = probe[:probe.index("\n}\n")]
    assert "GetVersionEx" in probe, "the probe must ask Windows for its version"
    assert "dwMajorVersion < 6" in probe, (
        "XP/2003 is major version 5; Vista and later are 6+"
    )

    # Every call to the disabler must be guarded. There is exactly one, and the
    # gate has to be on the line before it - a call anywhere else re-breaks Win7.
    calls = [m.start() for m in re.finditer(r"\bstop_and_disable_themes\(\)\s*;", src)]
    assert len(calls) == 1, (
        "expected exactly one call to stop_and_disable_themes(), found %d" % len(calls)
    )
    before = src[:calls[0]]
    guard = before.rindex("if (os_is_xp_or_older())")
    between = before[guard:]
    # nothing but whitespace/comments may sit between the gate and the call
    assert len(between.split("\n")) <= 2, (
        "stop_and_disable_themes() must be the immediate body of the "
        "os_is_xp_or_older() test, not merely somewhere after it"
    )


def test_vista_and_later_keep_their_own_visual_style():
    """The non-XP branch must exist and must say so in the log, so a Win7 box
    that looks wrong can be diagnosed from agent.log alone."""
    src = _read(RETROWALL_C)
    theme = src[src.index("static void apply_hacker_theme"):]
    theme = theme[:theme.index("\n}\n")]
    assert "else" in theme and "not XP" in theme, (
        "the Vista+ path must log that it deliberately left the visual style "
        "alone - silence here reads as 'the theme code never ran'"
    )


# ---------------------------------------------------------------------------
# 2026-08-29: applying the wallpaper once is not enough on .246.
#
# That box runs a Windows 7 that is not activated - `slmgr /xpr` reports
# "Windows is in Notification mode" - and Windows' own enforcement BLANKS THE
# DESKTOP on its own schedule: it clears HKCU\Control Panel\Desktop\Wallpaper to
# an EMPTY string and paints black, roughly hourly. Measured directly: the agent
# logged "retrowall: wallpaper set to C:\retro-wall\retrowall_1920x1080.bmp",
# and a `reg query` of that value some time later returned nothing at all with a
# black desktop on screen. Nothing in the log said why, because nothing the
# agent did was wrong.
#
# So the wallpaper has to be KEPT, not just set. The keeper must:
#   * only act when the FLEET wallpaper path is in charge (never fight the
#     legacy rotation, and never fight a box with nothing staged),
#   * compare against the exact path we applied, so a deliberate change by a
#     person to some other file is left alone... and only OUR file restored,
#   * poll on a long interval and in short sleep slices, so a QUIT is not held
#     up behind it on a single-threaded Win9x agent.
# ---------------------------------------------------------------------------


def _fn_body(src, signature):
    i = src.index(signature)
    j = src.index("\n}\n", i)
    return src[i:j]


def test_the_fleet_wallpaper_is_kept_not_merely_applied():
    src = _read(RETROWALL_C)
    assert "keep_fleet_wallpaper" in src, (
        "an unactivated Windows blanks the desktop hourly - setting the "
        "wallpaper once at startup does not keep it set"
    )
    body = _fn_body(src, "static int keep_fleet_wallpaper(")
    assert "g_fleet_wall" in body, "it must compare against the path we applied"
    assert "SPI_SETDESKWALLPAPER" in body, "it must re-apply live, not only in the registry"
    assert "lstrcmpiA" in body, "the comparison must be case-insensitive"


def test_the_keeper_does_nothing_when_no_fleet_wallpaper_is_in_charge():
    body = _fn_body(_read(RETROWALL_C), "static int keep_fleet_wallpaper(")
    guard = body.index("g_fleet_wall[0]")
    act = body.index("SPI_SETDESKWALLPAPER")
    assert guard < act, (
        "bail out before touching anything when g_fleet_wall is empty - a box "
        "on the legacy rotation, or with nothing staged, must not be fought"
    )


def test_the_keeper_loop_wakes_often_enough_to_shut_down():
    src = _read(RETROWALL_C)
    body = _fn_body(src, "DWORD WINAPI retrowall_thread(")
    assert "keep_fleet_wallpaper()" in body, "the thread must run the keeper"
    assert "while (g_running)" in body, "and must stop when the agent stops"
    assert "Sleep(1000)" in body, (
        "sleep in ~1s slices, not one long Sleep - on Win9x the agent is "
        "single-threaded and a QUIT must not wait out the whole interval"
    )
    m = re.search(r"#define RETROWALL_KEEP_SEC\s+(\d+)", src)
    assert m, "the keeper interval must be a named constant"
    assert 60 <= int(m.group(1)) <= 900, (
        "a registry read every few minutes is free; every few seconds is noise "
        "and every hour loses the race with the blanker"
    )
