"""Verification must be a FULLSCREEN observation. User directive, 2026-08-31.

WHY IT IS A RULE AND NOT A PREFERENCE. Fullscreen is how these games are played
here, and it is the mode where this fleet's real failures live. Every
resolution defect found this session was an exclusive-fullscreen mode set going
wrong:

  * an id Tech mode index naming a resolution the driver does not offer -- the
    engine neither errored nor obeyed, it set the desktop to 1280x960 and drew
    into a WINDOW with r_fullscreen still 1;
  * Hexen II's GL build refusing every widescreen mode and dying before the map
    loaded, so the host launcher opened no server at all;
  * two 4:3 tubes being driven at 5:4 and visibly squashed.

**None of those reproduce in a window.** A title confirmed windowed has not
been confirmed, and recording it as `verified` puts a fact in the database that
is not true of the way the machine is used.

WINDOWED IS A TOOL, NOT THE RESULT. The rule has to survive contact with two
real limitations, which is why the doc keeps them explicit rather than banning
windows outright: id Tech 3 ignores synthetic keyboard input in exclusive
fullscreen and accepts it windowed, and GDI cannot photograph an
exclusive-fullscreen surface on some boxes. Using a window to type a CD key or
grab a frame is correct -- returning to fullscreen and confirming THERE is what
makes it a verification.

This test guards the DOC, because the doc is what every agent reads before
touching a box.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLAUDE = os.path.join(REPO, "CLAUDE.md")


def _text():
    with open(CLAUDE, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_the_rule_is_stated_as_a_requirement():
    t = _text()
    assert "TEST IN FULLSCREEN" in t, (
        "CLAUDE.md no longer carries the fullscreen-testing rule. It is a "
        "standing user directive, and agents read this file before every run.")
    m = re.search(r"### TEST IN FULLSCREEN[^\n]*", t)
    assert m and "REQUIRED" in m.group(0), (
        "the fullscreen rule must be marked REQUIRED like the other standing "
        "directives, or it reads as advice")


def test_it_says_a_windowed_pass_is_not_a_pass():
    """The load-bearing sentence. Without it the rule is just a preference."""
    t = _text().lower()
    assert "a windowed pass is not a pass" in t or (
        "confirmed windowed has not been confirmed" in t), (
        "CLAUDE.md no longer states that a windowed observation does not count "
        "as a verification -- that is the whole point of the rule")


def test_it_still_permits_windowed_as_a_MEANS():
    """Do not let this harden into a ban that blocks real work.

    Two limitations are measured and unavoidable: id Tech 3 ignores synthetic
    keys in exclusive fullscreen, and GDI cannot capture some fullscreen
    surfaces. A rule that forbade windows outright would make a CD key
    un-typeable and some boxes un-photographable, and would then be ignored --
    which is worse than not having it.
    """
    t = _text()
    assert "Windowed mode is a TOOL" in t or "windowed mode is a tool" in t.lower(), (
        "CLAUDE.md no longer explains that a window is a legitimate means to "
        "type or capture; without that this rule blocks work it should not")
    low = t.lower()
    assert "then return the title to fullscreen" in low or "return the title to fullscreen" in low, (
        "the doc must say to return to fullscreen and confirm THERE -- "
        "otherwise 'windowed is allowed' swallows the rule")


def test_it_keeps_the_restore_the_desktop_mode_warning():
    """A game that exits without restoring strands the box at 640x480.

    The next launcher that reads the LIVE mode then pins every later game to
    it. Both .123 and .240 have been found in that state.
    """
    t = _text()
    assert "640x480" in t and ("Leave the box fullscreen" in t
                              or "restoring the desktop" in t), (
        "the desktop-restore warning is gone; a crashed title then silently "
        "pins every later game on that box to 640x480")
