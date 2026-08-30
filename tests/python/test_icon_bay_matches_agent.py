"""The drawn icon bay and the agent's arranger must agree about the columns.

THERE ARE TWO IMPLEMENTATIONS OF ONE RULE and they have already drifted apart
once, which is what this file exists to stop:

  * `gen_retro_wall.py:icon_bay()` (Python, dev host) DRAWS the bay -- the
    "GAME LIBRARY" panel painted into the wallpaper BMP.
  * `gamesync.c:gs_icon_bay()` + `gs_arrange_cols()` (C, on the box) PLACES the
    icons at arrange time.

The drift: only the C side widened on overflow. At 1024x768 the base bay is
4 columns x 8 rows = 32 slots against a library that is now 78 shortcuts, so
the arranger widened to 10 columns -- correctly, because packing downward
instead would put rows past the 8th BELOW THE BOTTOM OF THE SCREEN where they
cannot be clicked at all. But the wallpaper still painted 4 columns, so six
columns of icons sat outside the frame on bare art. Seen on .143.

Cosmetic in that direction. The dangerous direction is the other one: if the
PYTHON side ever widens further than the C side, the art promises slots the
arranger never fills, and a reader concludes icons are missing.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "scripts", "retro-wallpaper"))
GAMESYNC = os.path.join(REPO, "agent", "src", "gamesync.c")

from gen_retro_wall import icon_bay  # noqa: E402


def _agent_rule(cols, rows, x, cell_w, screen_w, count):
    """A transcription of gs_arrange_cols(). Kept deliberately literal.

    The source assertions below check the C still says this, so a change there
    fails here rather than silently diverging again.
    """
    if count <= cols * rows:
        return cols
    need = (count + rows - 1) // rows
    maxcols = (screen_w - x) // cell_w
    if maxcols < 1:
        maxcols = 1
    if need > maxcols:
        need = maxcols
    if need < cols:
        need = cols
    return need


SCREENS = [(800, 600), (1024, 768), (1280, 1024), (1280, 800),
           (1440, 900), (1600, 1200), (1920, 1080)]
COUNTS = [0, 1, 12, 32, 33, 65, 78, 96, 97, 200, 5000]


def test_drawn_bay_matches_the_arrangers_column_count():
    for w, h in SCREENS:
        base = icon_bay(w, h)
        for n in COUNTS:
            drawn = icon_bay(w, h, n)["cols"]
            placed = _agent_rule(base["cols"], base["rows"], base["x"],
                                 base["cell_w"], w, n)
            assert drawn == placed, (
                "at %dx%d with %d icons the wallpaper draws %d columns but the "
                "agent arranges into %d -- icons will not land in the cells the "
                "art shows" % (w, h, n, drawn, placed)
            )


def test_the_overflow_case_that_was_actually_broken():
    """1024x768 with the real library. This is the .143 regression."""
    base = icon_bay(1024, 768)
    assert base["cols"] * base["rows"] == 32, (
        "the base bay at 1024x768 should still be the 4x8 the wallpaper art "
        "was designed around; if this changed, re-check the overflow maths"
    )
    wide = icon_bay(1024, 768, 78)
    assert wide["cols"] > base["cols"], "78 icons must widen a 32-slot bay"
    assert wide["cols"] * wide["rows"] >= 78, (
        "the widened bay must actually hold every icon -- the whole point is "
        "that nothing lands below the bottom of the screen"
    )
    assert wide["x"] + wide["width"] <= 1024, (
        "the bay must stay on screen; widening past the right edge trades one "
        "unreachable icon for another"
    )


def test_no_widening_when_it_already_fits():
    """1920x1080 has 96 slots; 78 icons must not move a single cell."""
    assert icon_bay(1920, 1080, 78) == icon_bay(1920, 1080)


def test_the_c_source_still_implements_the_transcribed_rule():
    """If gs_arrange_cols changes, fail HERE rather than drift silently."""
    with open(GAMESYNC, "r", encoding="utf-8", errors="replace") as fh:
        code = fh.read()
    body = re.search(r"static int gs_arrange_cols\((.*?)\n}", code, re.S)
    assert body, "gs_arrange_cols() not found -- did it get renamed?"
    body = body.group(1)
    for fragment, why in [
        ("count <= bay->cols * bay->rows", "the no-overflow early return"),
        ("(count + bay->rows - 1) / bay->rows", "columns needed for the rows"),
        ("(screen_w - bay->x) / bay->cell_w", "the screen-width bound"),
        ("need < bay->cols", "the never-narrow floor"),
    ]:
        assert fragment in body, (
            "gs_arrange_cols() no longer contains %r (%s). The Python bay in "
            "gen_retro_wall.py:icon_bay() mirrors this rule -- update BOTH, "
            "then update _agent_rule() in this file." % (fragment, why)
        )
