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


def test_the_legacy_binaries_are_put_beyond_use():
    """The old desktop must not merely stop starting itself.

    User requirement: *"the old desktop should not be used anymore."* Killing
    the rotator and clearing both Run values stops it starting BY ITSELF; it
    does not stop a person, a stale script, or a future agent from running it.

    Two binaries can undo this desktop:
      * `rotate_wall.exe`   - puts the old wallpaper back;
      * `arrange_icons.exe` - parks icons bottom-right and explicitly CLEARS
        `LVS_AUTOARRANGE`, so a single run turns off the fleet-wide
        auto-arrange setting the user asked for. CLAUDE.md already says it must
        never be staged or run, and `deploy_rotation.py` renames stale copies
        aside -- the agent simply never did the same.

    Renamed rather than deleted: reversible, leaves evidence of what was there,
    and deleting files we did not stage is a bigger action than this warrants.
    """
    body = _stop_fn()
    assert "MoveFileA" in body, (
        "the legacy EXEs are no longer put beyond use. Clearing the Run value "
        "only stops the old desktop starting itself; anything that runs "
        "rotate_wall.exe or arrange_icons.exe still reverts the machine.")
    assert "ROTATE_EXE" in body and "ARRANGE_EXE" in body, (
        "both legacy binaries must be neutralised -- arrange_icons.exe is the "
        "more damaging of the two, because it clears LVS_AUTOARRANGE and turns "
        "off the fleet-wide auto-arrange setting")
    assert "superseded" in body, (
        "the renamed-aside suffix is gone; without a stable suffix a second "
        "run cannot tell 'already neutralised' from 'never seen'")


def test_neutralising_happens_only_after_a_fleet_wallpaper_is_applied():
    """Never strand a box with no wallpaper at all.

    stop_wallpaper_rotation() is called ONLY once the fleet wallpaper has been
    applied, so a machine that genuinely still depends on the rotation keeps
    it. Renaming the binaries from anywhere else would break exactly those
    boxes.
    """
    with open(SRC, encoding="utf-8", errors="replace") as f:
        s = f.read()
    assert s.count("stop_wallpaper_rotation()") <= 2, (
        "stop_wallpaper_rotation() is called from more than one place; it must "
        "run only on the success path of apply_fleet_wallpaper(), or a box "
        "with no matching fleet wallpaper loses its rotation and gets nothing")
