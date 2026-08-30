"""The agent must not run the legacy bottom-right icon arranger.

WHY THIS EXISTS. Two pieces of code arranged desktop icons and they disagreed:

  * `gs_arrange_icons()` in gamesync.c parks icons in the wallpaper's bay, which
    is TOP-LEFT — matching `gen_retro_wall.py:icon_bay()`, clearing
    LVS_EX_SNAPTOGRID (v1.67.0) so the shell stops rounding to its own 103px
    grid, and widening into extra columns when the library outgrows the bay
    (v1.68.0) so no icon lands below the screen edge.
  * `C:\\retro-wall\\arrange_icons.exe` is the LEGACY arranger. Its own printf
    says "moved %d icons to bottom-right well" — where the wallpaper used to
    reserve space, and no longer does.

`retrowall_apply_startup()` ran the legacy exe unconditionally on EVERY agent
start, so it undid a correct arrangement every boot. That is the worst shape a
defect can take: each manual fix appeared to work and was silently reverted
later, so it presented as "the icons keep moving" rather than as anything
attributable to a specific change.

The exe is still staged — harmless on disk. Only running it was wrong.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RETROWALL = os.path.join(REPO, "agent", "src", "retrowall.c")
GAMESYNC = os.path.join(REPO, "agent", "src", "gamesync.c")


def _read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _strip_comments(text):
    """A comment explaining the bug must not read as the bug."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def test_retrowall_does_not_execute_the_legacy_arranger():
    code = _strip_comments(_read(RETROWALL))
    assert "ARRANGE_EXE" in code, (
        "the constant should remain — the file is still staged, and the log "
        "line that says it is deliberately not run references it"
    )
    assert not re.search(r"run_process\s*\(\s*ARRANGE_EXE", code), (
        "retrowall must NOT run arrange_icons.exe: it parks icons bottom-right "
        "and undoes gs_arrange_icons()'s top-left bay on every agent start"
    )


def test_the_agent_still_arranges_icons_itself():
    """Removing the call must not leave the fleet with no arranger at all."""
    code = _strip_comments(_read(GAMESYNC))
    assert "static void gs_arrange_icons(" in code, (
        "gs_arrange_icons() is what replaced the legacy exe — it must exist"
    )
    assert re.search(r"\bgs_arrange_icons\s*\(\s*\)\s*;", code), (
        "gs_arrange_icons() must actually be called, or icons are never parked"
    )


def test_the_native_arranger_keeps_its_two_fixes():
    """Both were earned on hardware; neither may be lost in a later edit."""
    code = _strip_comments(_read(GAMESYNC))
    assert "SNAPTOGRID" in code, (
        "the align-to-grid clear (v1.67.0) must survive — without it the shell "
        "rounds every position to its own 103px grid against 80px cells"
    )
    assert "SetWindowLongA" in code, (
        "the style must be cleared directly; PostMessage-ing the toggle was "
        "not enough"
    )
