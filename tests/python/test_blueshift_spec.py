"""Blue Shift's disc-mount spec must keep the facts that were measured for it.

WHY THIS EXISTS
---------------
Blue Shift has never run on any fleet box. Games-Library/HalfLife1 stages it as
`hl.exe -game bshift`, which grafts its April-2001 game DLLs onto Half-Life
GOTY's Sep-2001 engine; that engine rejects them ("Game DLL version mismatch"
-> Host_Error). Blue Shift is a STANDALONE product with its own engine, and on
2026-09-01 a retail install in the build VM produced one.

Three things in that spec are load-bearing and each was paid for:

1. VOLID must be the image's REAL label. The game's own language.inf says
   ShortTitle=BLUESHIFT while the disc reads BLUESHIFT_UK, which is a standing
   invitation to "fix" the spec. It was tested: relabelling a copy in BOTH the
   ISO9660 and Joliet descriptors did NOT satisfy the check. VOLID describes
   the image we have; the validator reads it back out of that image.

2. MARKER must be unique to THIS disc. AUTORUN.INF and SETUP.EXE are on every
   game CD ever pressed, and using one made the Descent II launcher match a
   mounted StarCraft disc.

3. GAMEARGS must use FR_W43/FR_H43. The WON engine has a fixed 4:3 mode table
   and no widescreen mode; handed 16:9 it falls back to 400x300 and takes the
   desktop with it (measured on .240).
"""
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = os.path.join(REPO, "provisioning", "discmount", "specs",
                    "HalfLife-BlueShift.json")


def _spec():
    with open(SPEC, encoding="utf-8") as f:
        return json.load(f)


def test_the_spec_exists_and_parses():
    assert os.path.isfile(SPEC)
    assert _spec()["vars"]


def test_volid_is_the_images_real_label_not_the_one_the_game_wants():
    v = _spec()["vars"]["VOLID"]
    assert v == "BLUESHIFT_UK", (
        "VOLID must be the label the image ACTUALLY carries. language.inf says "
        "BLUESHIFT and that looks like the fix, but relabelling a copy in both "
        "the ISO9660 primary and the Joliet supplementary descriptor was tested "
        "and did NOT satisfy the game's check - so changing this only breaks "
        "the launcher's own volume test.")


def test_the_marker_is_unique_to_this_disc():
    m = _spec()["vars"]["MARKER"]
    generic = {"autorun.inf", "setup.exe", "install.exe", "readme.txt",
               "eula.txt", "version.txt"}
    assert m.lower() not in generic, (
        "%r is on essentially every game CD; a generic marker made the Descent "
        "II launcher match a mounted StarCraft disc" % m)
    assert m == "bsinstall.EXE"


def test_the_game_is_the_standalone_engine_not_half_life():
    g = _spec()["vars"]["GAME"].lower()
    assert "bshift.exe" in g, (
        "the whole point of this tree is that Blue Shift has its OWN engine; "
        "pointing GAME at hl.exe recreates the bug it was built to fix")
    assert "hl.exe" not in g


def test_the_mode_is_the_4_3_pair():
    a = _spec()["vars"]["GAMEARGS"]
    assert "%FR_W43%" in a and "%FR_H43%" in a, (
        "the WON engine is 4:3-only - handed a 16:9 mode it falls back to "
        "400x300 and takes the desktop with it (measured on .240)")
    assert "%FR_W%" not in a.replace("%FR_W43%", "")


def test_it_declares_that_it_needs_the_disc():
    assert _spec()["vars"]["REQUIREDISC"] == "1"


def test_the_note_records_what_was_refuted():
    """The next person must not re-run two experiments that already failed."""
    note = _spec()["_note"]
    low = note.lower()
    assert "wrong disc" in low
    assert "joliet" in low, "the Joliet descriptor is the one Windows reports"
    assert "untested" in low, "the mounter question must stay marked open"
