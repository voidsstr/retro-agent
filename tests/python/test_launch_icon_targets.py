#!/usr/bin/env python3
"""A staged title's desktop icon must be ITS OWN artwork, not a stub's.

WHY THIS EXISTS. The third column of a Games-Library `launch.txt` names the
file a shortcut draws its icon from. Every failure of that column so far has
been SILENT and has looked completely fine to a human: the shortcut exists, it
launches the right game, and it simply wears the wrong badge. Nothing logs a
warning, because from the agent's point of view nothing went wrong — it was
handed a path that resolves, and it used it.

Three were found and fixed on hardware (.143/.145, 2026-08-29/30) by reading
the agent's own `desktop shortcut -> ... (icon: ...)` lines:

  CounterStrike16   hl.exe            -> Counter-Strike.exe
        the Half-Life lambda on the Counter-Strike shortcut. The worst kind,
        because hl.exe IS the engine CS runs on, so the field looked correct
        to anyone reasoning about it rather than looking at the desktop.
  SystemShock2      clokspl.exe       -> shock2.exe
        clokspl.exe is the SafeDisc splash loader. It carries a real icon
        resource, just a generic one — so "does this file have an icon?" is
        NOT a check that would have caught it.
  RedFaction        UpdateLauncher.exe -> RedFaction.exe
        the patcher stub, same story.

scripts/validate-staged-library.py already fails an icon path that does not
RESOLVE. It cannot judge whether the artwork is the right artwork — that is a
content question, and the only durable answer is to pin the verified value. So
this test pins it: if someone edits one of these launch.txt files back to a
stub, or a library restore drops an older copy in, the suite says so.

It SKIPS loudly when the share is not mounted, matching test_staged_library.py:
a dev host without the SMB mount must not fail the suite, but it must also not
quietly claim the library was checked.
"""
import os

import pytest

LIB = "/mnt/retro-share/Files/Games-Library"

# title -> (icon that is CORRECT and verified on hardware,
#           icon that was WRONG and shipped before the fix)
VERIFIED_ICONS = {
    "CounterStrike16": ("Counter-Strike.exe", "hl.exe"),
    "SystemShock2": ("shock2.exe", "clokspl.exe"),
    "RedFaction": ("RedFaction.exe", "UpdateLauncher.exe"),
}


def _icon_fields(title):
    """Every icon field in a title's launch.txt, lowercased.

    Mirrors the agent's parse: tab-separated, `#` comments and blanks ignored,
    column 3 is the icon and may be absent.
    """
    path = os.path.join(LIB, title, "launch.txt")
    icons = []
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            parts = line.rstrip("\r\n").split("\t")
            icons.append(parts[2].strip().lower() if len(parts) >= 3 else "")
    return icons


needs_share = pytest.mark.skipif(
    not os.path.isdir(LIB),
    reason="%s not mounted - the staged library was NOT checked" % LIB,
)


@needs_share
@pytest.mark.parametrize("title,good,bad", [
    (t, g, b) for t, (g, b) in sorted(VERIFIED_ICONS.items())
])
def test_icon_is_the_titles_own_artwork(title, good, bad):
    tdir = os.path.join(LIB, title)
    if not os.path.isdir(tdir):
        pytest.skip("%s is not in the library on this host" % title)

    icons = _icon_fields(title)
    assert icons, "%s/launch.txt has no data lines" % title

    # The fixed value must be there...
    assert good.lower() in icons, (
        "%s/launch.txt should draw its icon from %s (verified on hardware); "
        "icon fields are %r" % (title, good, icons))

    # ...and the old, wrong-artwork value must be gone. Asserting both is what
    # makes this a regression test rather than a restatement of the current
    # file: a revert reintroduces `bad`, and that is the thing to catch.
    assert bad.lower() not in icons, (
        "%s/launch.txt has regressed to the stub icon %s - the shortcut will "
        "wear the wrong badge and nothing will log a warning" % (title, bad))


@needs_share
@pytest.mark.parametrize("title", sorted(VERIFIED_ICONS))
def test_icon_target_is_present_in_the_tree(title):
    """A pinned icon is only worth pinning if it actually ships."""
    tdir = os.path.join(LIB, title)
    if not os.path.isdir(tdir):
        pytest.skip("%s is not in the library on this host" % title)
    good = VERIFIED_ICONS[title][0]
    assert os.path.isfile(os.path.join(tdir, good)), (
        "%s/%s is named as the icon source but is not in the staged tree - it "
        "degrades silently to the auto-resolved icon" % (title, good))


@needs_share
@pytest.mark.parametrize("title", sorted(VERIFIED_ICONS))
def test_every_launcher_declares_an_icon(title):
    """An empty icon column is how a .bat launcher ends up generic.

    These three titles all launch through a `Play <Game>.bat`, and a .bat with
    no icon column renders the plain batch-file icon - which is what a reviewer
    reads as "the game has no artwork" when in fact the column was just never
    filled in.
    """
    tdir = os.path.join(LIB, title)
    if not os.path.isdir(tdir):
        pytest.skip("%s is not in the library on this host" % title)
    icons = _icon_fields(title)
    assert all(icons), (
        "%s/launch.txt has a launcher with no icon column - it will render the "
        "generic batch-file icon" % title)
