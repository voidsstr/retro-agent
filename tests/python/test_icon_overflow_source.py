"""More desktop icons than bay slots must spill SIDEWAYS, never off the screen.

WHY THIS EXISTS (2026-08-29, found on 192.168.1.143). The wallpaper's icon bay
at 1024x768 is 4 columns x 8 rows = 32 slots. The staged library is 31 titles
producing 65 desktop shortcuts. The arrangers' rule for the overflow was "keep
packing DOWNWARD", so item 65 was placed at

    y = 57 + (64 / 4) * 80 + 6 = 1343

on a screen 768 pixels tall. The XP desktop listview has no scrollbar, so those
icons were not merely outside the drawn panel - they could not be clicked at
all. Half the game library was invisible on every 1024x768 box, while the code
and `tests/native/test_icon_bay.c` were both perfectly correct: the bay geometry
was never the thing that was wrong.

The fix widens into extra COLUMNS instead, bounded by the screen, using the
number of rows that fit ON THE SCREEN rather than the bay's own row count.
`tests/native/test_icon_arrange_overflow.c` pins the arithmetic; this file pins
the fact that BOTH implementations carry it, because there are two arrangers and
they have drifted apart before - `scripts/retro-wallpaper/arrange_icons.c` still
parked icons in the BOTTOM-RIGHT months after the wallpaper moved the bay to the
top-left, and `agent/src/retrowall.c` runs it on every single agent start.
"""

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AGENT = os.path.join(REPO, "agent", "src", "gamesync.c")
TOOL = os.path.join(REPO, "scripts", "retro-wallpaper", "arrange_icons.c")


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _code_only(text):
    """Strip C comments. These files DOCUMENT the bugs they fixed, naming the
    old constants and the old bottom-right well in prose, so a check for
    'the old thing is gone' has to look at code and not at history."""
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def _agent_fn(name):
    text = _read(AGENT)
    i = text.index("static %s" % name)
    return text[i:text.index("\n}\n", i)]


def test_the_agent_widens_instead_of_packing_downward():
    body = _agent_fn("int gs_arrange_cols(")
    assert re.search(r"\(count \+ bay->rows - 1\) / bay->rows", body), \
        "overflow must widen into as many columns as bay.rows demands"
    assert re.search(r"\(screen_w - bay->x\) / bay->cell_w", body), \
        "the widening must be capped by the screen width"
    assert "if (need < bay->cols)" in body, \
        "widening must never make the layout NARROWER than the drawn bay"


def test_the_agent_places_with_the_widened_column_count():
    body = _agent_fn("void gs_arrange_icons(")
    assert "cols = gs_arrange_cols(" in body, \
        "gs_arrange_icons() must ask for the widened column count"
    assert "col = i % cols" in body and "row = i / cols" in body, (
        "the placement loop must use the widened count, not bay.cols - using "
        "bay.cols is what put 29 of .143's 65 icons below the screen"
    )


def test_the_staged_tool_carries_the_same_rule():
    src = _read(TOOL)
    assert "bottom-right" not in _code_only(src), \
        "arrange_icons.exe must park icons in the TOP-LEFT bay the wallpaper draws"
    assert "top-left icon bay" in src, "its own message should say where it puts them"
    assert re.search(r"max_rows\s*=\s*\(scrH\s*-\s*bay_y\s*-\s*6\)\s*/\s*cell_h", src), \
        "same screen-height row budget as the agent"
    assert re.search(r"eff_cols\s*=\s*\(n\s*\+\s*max_rows\s*-\s*1\)\s*/\s*max_rows", src), \
        "same widening rule as the agent"


def test_the_staged_tool_uses_the_wallpapers_bay_geometry():
    src = _read(TOOL)
    # Must mirror icon_bay() in gen_retro_wall.py / gs_icon_bay() in gamesync.c.
    assert "cell_w = 76" in src and "cell_h = 80" in src, "cells are 76x80"
    assert re.search(r"cols\s*=\s*\(int\)\(\(scrW\s*\*\s*0\.34\)\s*/\s*cell_w\)", src), \
        "columns are 34% of the width, as the wallpaper draws them"
    assert "header_h = 34" in src, "the bay starts below a 34px header"


def test_the_staged_tool_clears_align_to_grid_like_the_agent_does():
    src = _read(TOOL)
    assert "LVS_EX_SNAPTOGRID" in src, (
        "without clearing align-to-grid the shell rounds every position to its "
        "own ~103px grid and the icons leave the bay's 80px cells"
    )
    assert re.search(r"SendMessageA\(\s*lv,\s*LVM_SETEXSTYLE,\s*LVS_EX_SNAPTOGRID,\s*0\s*\)", src), \
        "clear it with the (mask, value) form, never a toggle"


def test_the_staged_tool_does_not_post_the_customize_folder_command():
    src = _code_only(_read(TOOL))
    assert "FCIDM_SHVIEW_SNAPTOGRID" not in src, (
        "posting 0x7032 to the shell view raised a modal 'This folder cannot be "
        "customized' on XP (.145, 2026-08-29), and a stray modal blocks every "
        "later launch on this fleet"
    )
