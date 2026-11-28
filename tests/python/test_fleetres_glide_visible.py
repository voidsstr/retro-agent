#!/usr/bin/env python3
"""FR_GLIDE means "Glide draws where the monitor is", not "a 3dfx card is fitted".

WHY THIS EXISTS (measured on .143, 2026-11-23). The box has a Voodoo5 5500
fitted, but its monitor is on a GeForce 6800 and the V5 is a second adapter
that is not attached to the desktop. FLEETRES set FR_GLIDE=1 from the PCI enum
alone, so "Play Carmageddon 2.bat" moved the game-local nGlide glide2x.dll
aside, CARMA2_HW.EXE loaded the real system32 glide2x.dll, and BRender stopped
with a modal "Failure: Unable to allocate Main Front Screen". The title has no
usable Direct3D path either (its install.reg records Driver=3 crashing inside
ntdll), so moving the wrapper aside left the game with nothing that can draw.
Copying the nGlide wrapper back in ran it to the menu on the same box.

The invariant, which is cheap to undo by accident because "is the card there?"
is the obvious question to ask:

  * a Voodoo 1 / Voodoo 2 is a 3D-ONLY PASSTHROUGH board (Class=MEDIA, absent
    from every display enumeration) - the VGA card's output runs through it, so
    present IS visible and FR_GLIDE stays 1 (.243's Voodoo 2);
  * a Banshee / Voodoo 3 / 4 / 5 IS a display adapter, so it counts only while
    it is ATTACHED_TO_DESKTOP (.124's V5 6000 drives its own screen and keeps
    FR_GLIDE=1; .143's V5 5500 does not and reads 0);
  * HKLM\\Software\\RetroAgent GlideRender=1 forces it on, for a loopback the
    box cannot report;
  * FR_GLIDEFIT keeps the old "silicon is fitted" fact for anything that wants
    it, so the two questions stay separable instead of one overwriting the
    other.

This reads the SOURCE, not a box: fleetres.c is Win32-only and cannot be
compiled or run on the dev host, so the assertions are on the code that ships.
"""
import os
import re

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "provisioning", "fleetres", "fleetres.c")
STAGER = os.path.join(ROOT, "scripts", "fleet", "stage-fleetres.py")


@pytest.fixture(scope="module")
def src():
    with open(SRC, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def test_fr_glide_is_emitted_from_the_visibility_answer(src):
    """FR_GLIDE must come from glide_see, and FR_GLIDEFIT from the PCI count.

    The old line was `FR_GLIDE=%d", glide_n ? 1 : 0` - "a card is fitted" - and
    that is the bug this test pins. Both variables are checked, because the
    cheap way to "restore" the old behaviour is to emit the fitted count under
    the FR_GLIDE name again.
    """
    glide = re.search(r'printf\("set \\"FR_GLIDE=%d\\"\\n",\s*([a-z_]+)', src)
    assert glide, "fleetres.c no longer emits FR_GLIDE"
    assert glide.group(1) == "glide_see", (
        "FR_GLIDE is emitted from %r; it must be the visibility answer "
        "(glide_see), not the fitted count" % glide.group(1))

    fit = re.search(r'printf\("set \\"FR_GLIDEFIT=%d\\"\\n",\s*([a-z_]+)', src)
    assert fit, "FR_GLIDEFIT is gone - the 'is one fitted' fact must stay available"
    assert fit.group(1) == "glide_n"


def test_visibility_is_passthrough_or_operator_flag_or_attached_adapter(src):
    """glide_see has exactly the three ways to be true, and needs silicon first."""
    m = re.search(r"if \(glide_n\)\s*\n\s*glide_see = ([^;]+);", src)
    assert m, "the glide_see decision is not where this test can read it"
    expr = " ".join(m.group(1).split())
    assert expr == "glide_pass || glide_render || glide_on_desktop()", expr


def test_only_the_3d_only_boards_count_as_passthrough(src):
    """Voodoo 1 (DEV_0001) and Voodoo 2 (DEV_0002) - and nothing else.

    DEV_0003 (Banshee), DEV_0005 (Voodoo 3) and DEV_0009 (Voodoo 4/5) have a 2D
    core and drive a monitor themselves, so they must go through the
    attached-to-desktop test instead of being waved past it.
    """
    body = re.search(r"static int glide_dev_is_passthrough\(.*?\n\}", src, re.S)
    assert body, "glide_dev_is_passthrough is gone"
    devs = set(re.findall(r"DEV_[0-9A-Fa-f]{4}", body.group(0)))
    assert devs == {"DEV_0001", "DEV_0002"}, devs


def test_the_desktop_probe_tests_both_attachment_and_the_vendor(src):
    body = re.search(r"static int glide_on_desktop\(void\).*?\n\}", src, re.S)
    assert body, "glide_on_desktop is gone"
    body = body.group(0)
    assert "DISPLAY_DEVICE_ATTACHED_TO_DESKTOP" in body, (
        "an adapter that is merely enumerated is not the one the monitor shows")
    assert "VEN_121A" in body
    # DeviceID, not DeviceString: a driver's marketing name is not an id, and
    # .143's reads "AMIGAMERLIN 3.1-R11 For Voodoo 5 5500 AGP".
    assert "DeviceID" in body


def test_the_launcher_preamble_defines_both_variables():
    """A stale FR_GLIDEFIT from an earlier launcher must not leak into the next."""
    with open(STAGER, encoding="utf-8", errors="replace") as fh:
        s = fh.read()
    assert "\nset FR_GLIDEFIT=\n" in s, "FLEETRES.BAT does not clear FR_GLIDEFIT"
    assert "if not defined FR_GLIDEFIT set FR_GLIDEFIT=" in s, (
        "FLEETRES.BAT gives FR_GLIDEFIT no fallback, so a box whose "
        "FLEETRES.EXE is missing leaves it undefined")


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
