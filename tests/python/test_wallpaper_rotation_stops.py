"""The legacy wallpaper rotation must be stopped in BOTH registry hives.

USER REPORT, 2026-08-31: *"on one of the computers the retro agent reverted to
the old desktop."*

WHAT WAS HAPPENING. `stop_wallpaper_rotation()` deleted the `RetroWallRotate`
Run value from **HKLM only**, but `scripts/retro-wallpaper/deploy_rotation.py`
writes it to **HKCU**:

    RUN_KEY = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"

So the agent killed the running rotator, applied the correct fleet wallpaper,
and reported success -- and the next logon started `rotate_wall.exe` again from
HKCU and put the OLD wallpaper back. **The box looked right until it was
rebooted**, which is precisely when nobody is watching.

MEASURED on `.240`: `HKLM\\...\\Run` held no wallpaper entry while
`HKCU\\...\\Run` held `RetroWallRotate = C:\\retro-wall\\rotate_wall.exe 60`,
and the desktop was showing `wall01.bmp` even though `retrowall_1920x1080.bmp`
was staged and matched the 1920x1080 screen exactly. A healthy box (`.123`) had
zero `wall0N.bmp` files at all.

The fix is not "also try HKCU" as an afterthought -- HKCU is where the value
actually lives, and HKLM is the speculative one.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "retrowall.c")
DEPLOY = os.path.join(REPO, "scripts", "retro-wallpaper", "deploy_rotation.py")


def _stop_fn():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        s = f.read()
    i = s.index("static void stop_wallpaper_rotation")
    return s[i:s.index("\n}", i)]


def test_the_run_value_is_deleted_from_both_hives():
    body = _stop_fn()
    assert "HKEY_CURRENT_USER" in body, (
        "stop_wallpaper_rotation() does not clear HKEY_CURRENT_USER. That is "
        "the hive deploy_rotation.py actually writes to, so the rotation "
        "restarts at the next logon and puts the OLD wallpaper back -- the box "
        "looks correct until it is rebooted.")
    assert "HKEY_LOCAL_MACHINE" in body, (
        "HKLM must still be cleared; some boxes may carry it there")


def test_the_deploy_script_still_targets_hkcu():
    """If the deployer ever moves, this test's premise must be rechecked."""
    with open(DEPLOY, encoding="utf-8", errors="replace") as f:
        s = f.read()
    m = re.search(r'RUN_KEY\s*=\s*"([^"]+)"', s)
    assert m, "deploy_rotation.py no longer defines RUN_KEY"
    assert m.group(1).upper().startswith("HKCU"), (
        "deploy_rotation.py now writes the Run value to %r, not HKCU. The "
        "agent clears both hives so this is not a break -- but the reasoning "
        "in retrowall.c names HKCU as the one that matters and should be "
        "updated to match." % m.group(1))


def test_the_fleet_wallpaper_is_preferred_over_the_legacy_rotation():
    """The precedence itself: the new scheme wins, and only then is the old
    one torn down -- so a box that still depends on the rotation is never left
    with no wallpaper at all."""
    with open(SRC, encoding="utf-8", errors="replace") as f:
        s = f.read()
    assert "apply_fleet_wallpaper" in s and "stop_wallpaper_rotation" in s
    i = s.index("stop_wallpaper_rotation(void)")
    call = s.find("stop_wallpaper_rotation()", i)
    assert call > 0, "stop_wallpaper_rotation() is never called"
    assert "retrowall_" in s, (
        "the resolution-named fleet wallpapers are no longer referenced; the "
        "agent would fall back to the legacy rotation on every box")
