"""An id Tech mode index must name a mode the driver actually offers.

MEASURED ON .246, 2026-08-31. `FR_Q3MODE` resolved to **7 = 1152x864** and
`DISPLAYCFG set 1152 864 32` answered *"mode not supported by display driver"*.
That adapter offers **1152x648** (16:9) and no 1152x864 at all -- the table
entry was assumed to exist because it *fits inside* the target, which is a
different question from whether the hardware has it.

**The failure is the dangerous kind: the engine neither errors nor obeys.** RTCW
set the desktop to 1280x960 and drew into a WINDOW with `r_fullscreen` still 1.
Nothing logged a complaint, `r_mode` read back exactly what was asked for, and
the only symptom was a game that was not fullscreen.

`SoldierOfFortune2` and `JediAcademy` consume `FR_Q3MODE` today and were exposed
to this on all three 1080p boxes.

The file already enumerated the adapter's own mode list (`add_mode`/
`have_mode`) a few hundred lines below and then **never consulted it** in
`q2_mode_for`/`q3_mode_for`. This pins that it does.

**The fallback is deliberate and is asserted too.** Some drivers answer FALSE at
index 0 of the enumeration (the .143 GeForce 6800 case the code already works
around), leaving a stub list. Refusing to answer there would be worse than
answering approximately, so with fewer than four known modes the selector keeps
its old fits-inside behaviour. A guard that turns "I could not enumerate" into
"640x480" would be its own regression.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "provisioning", "fleetres", "fleetres.c")


def _src():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        return f.read()


def _fn(name):
    s = _src()
    i = s.index("static int %s(" % name)
    return s[i:s.index("\n}", i)]


def test_both_selectors_consult_the_driver_mode_list():
    """The whole point: fitting inside the target is not enough.

    This asserts the SELECTION ITSELF is guarded, not merely that the helper is
    named somewhere in the function. An earlier draft of this test checked only
    `"mode_offered" in body` and a mutation that reverted the loop to a bare
    `best = i;` still passed, because the fallback line further down also
    mentions the helper. A test that matches a proxy for the condition instead
    of the condition is the exact fault this file was written about.
    """
    for fn, tab in (("q2_mode_for", "q2tab"), ("q3_mode_for", "q3tab")):
        body = _fn(fn)
        guarded = re.search(
            r"if\s*\(\s*mode_offered\(\s*%s\[i\]\.w\s*,\s*%s\[i\]\.h\s*\)\s*\)\s*"
            r"best\s*=\s*i\s*;" % (tab, tab), body)
        assert guarded, (
            "%s assigns its winning index without first asking whether the "
            "adapter offers that mode. On .246 that produced r_mode 7 = "
            "1152x864 against a driver whose list holds 1152x648 and no "
            "1152x864 -- and the engine neither errored nor obeyed: it set the "
            "desktop to 1280x960 and drew into a WINDOW with r_fullscreen still "
            "1." % fn)
        assert not re.search(r"^\s*best\s*=\s*i\s*;\s*$", body, re.M), (
            "%s still contains an UNGUARDED `best = i;` -- the guarded form "
            "must be the only way the winner is chosen" % fn)


def test_mode_offered_falls_back_when_the_list_is_unusable():
    """Do not turn 'could not enumerate' into 'smallest mode'."""
    body = _fn("mode_offered")
    assert "g_nmodes" in body and "have_mode" in body, (
        "mode_offered no longer consults both the list size and the list")
    assert re.search(r"g_nmodes\s*<\s*[2-9]", body), (
        "mode_offered must treat a stub mode list as 'unknown, allow' -- some "
        "drivers answer FALSE at index 0 and the .143 GeForce 6800 is one of "
        "them. Without this the selector would collapse to the 640x480 floor "
        "on exactly the boxes whose enumeration is flaky.")


def test_the_640x480_floor_still_exists():
    """A selector that can return nothing is worse than one that guesses."""
    for fn in ("q2_mode_for", "q3_mode_for"):
        body = _fn(fn)
        assert "best = 3" in body, (
            "%s lost its 640x480 floor; every id Tech engine has mode 3" % fn)


def test_q3_still_skips_the_5_4_and_tiny_widescreen_entries():
    """The older fix must survive this one.

    id Tech 2's mode 8 is 1280x960 (4:3); id Tech 3's is 1280x1024 (5:4).
    Handing a 5:4 mode to a 16:9 panel is the squashed picture this whole
    mechanism exists to remove.
    """
    body = _fn("q3_mode_for")
    assert "i == 8 || i == 11" in body, (
        "q3_mode_for no longer skips index 8 (5:4) and 11 (a tiny 16:9) -- the "
        "squashed-picture fix has been lost")


def test_the_two_tables_are_still_different():
    """Collapsing them would silently reintroduce the 5:4 bug."""
    s = _src()
    q2 = s[s.index("q2tab[] ="):s.index(";", s.index("q2tab[] ="))]
    q3 = s[s.index("q3tab[] ="):s.index(";", s.index("q3tab[] ="))]
    assert "1280,960" in q2.replace(" ", "") or "{1280,960}" in q2.replace(" ", "")
    assert "1280,1024" in q3.replace(" ", "") or "{1280,1024}" in q3.replace(" ", "")
    assert q2 != q3, "id Tech 2 and id Tech 3 mode tables have been merged"
