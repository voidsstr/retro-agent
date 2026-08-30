"""The desktop arranger must clear BOTH arrangement settings, not just one.

WHY THIS EXISTS (2026-08-29, found on .246). The fleet wallpaper draws an icon
bay of 76x80 cells and `gs_arrange_icons()` moves every desktop icon into one.
It correctly turned OFF `LVS_AUTOARRANGE` first, because with auto-arrange on
the shell snaps every icon back to the top-left and no position sticks.

But "Align icons to grid" is a SECOND, independent setting - the extended style
`LVS_EX_SNAPTOGRID` - and Windows enables it by default. While it is set the
shell ROUNDS each position we ask for to its own grid, whose row pitch is the
icon spacing PLUS the label. Measured by A/B on 192.168.1.246 at 1920x1080 with
the identical arrange code:

    LVS_EX_SNAPTOGRID set     -> icon rows 103 px apart   (bay cells are 80)
    LVS_EX_SNAPTOGRID cleared -> icon rows  80 px apart   (exact)

So the icons walked out of the drawn cells down the column - the precise failure
`tests/native/test_icon_bay.c` exists to prevent, arriving by a route that test
cannot see because the arithmetic was right all along. It affected every box,
not just the Win7 one.

Two invariants, both source-level (the real calls need Win32):
 1. the arranger clears LVS_EX_SNAPTOGRID before it positions anything;
 2. it clears it with the (mask, value) form of LVM_SETEXTENDEDLISTVIEWSTYLE and
    NOT with a toggle - a toggle would switch the setting ON wherever a box
    happened to have it off, which is exactly the bug already recorded in the
    auto-arrange comment.
"""

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "gamesync.c")


def _arrange_fn():
    with open(SRC, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    i = text.index("static void gs_arrange_icons(")
    j = text.index("\n}\n", i)
    return text[:i], text[i:j]


def test_the_extended_style_constants_are_defined():
    head, _ = _arrange_fn()
    assert "LVS_EX_SNAPTOGRID" in head, "LVS_EX_SNAPTOGRID must be defined"
    assert "0x00080000" in head, "LVS_EX_SNAPTOGRID is 0x00080000"
    assert re.search(r"LVM_SETEXSTYLE_\s+\(LVM_FIRST_ \+ 54\)", head), \
        "LVM_SETEXTENDEDLISTVIEWSTYLE is LVM_FIRST+54"
    assert re.search(r"LVM_GETEXSTYLE_\s+\(LVM_FIRST_ \+ 55\)", head), \
        "LVM_GETEXTENDEDLISTVIEWSTYLE is LVM_FIRST+55"


def test_snap_to_grid_is_cleared_before_any_icon_is_positioned():
    _, body = _arrange_fn()

    assert "LVS_EX_SNAPTOGRID" in body, (
        "gs_arrange_icons() must clear align-to-grid, or the shell rounds every "
        "position to its own ~103 px grid and the icons leave the bay's cells"
    )
    clear_at = body.index("LVM_SETEXSTYLE_")
    move_at = body.index("LVM_SETITEMPOSITION_")
    assert clear_at < move_at, (
        "align-to-grid must be cleared BEFORE the icons are positioned - "
        "clearing it afterwards leaves them on the snapped coordinates"
    )


def test_snap_to_grid_is_cleared_not_toggled():
    _, body = _arrange_fn()
    # the (mask, value) form: mask = LVS_EX_SNAPTOGRID, value = 0
    assert re.search(
        r"SendMessageA\(\s*lv,\s*LVM_SETEXSTYLE_,\s*LVS_EX_SNAPTOGRID,\s*0\s*\)",
        body), (
        "clear it with SendMessage(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, "
        "LVS_EX_SNAPTOGRID, 0) - a toggle would switch it ON where a box had "
        "it off, the same trap already recorded for auto-arrange"
    )
    # and it must be conditional on actually being set, so the log stays honest
    assert re.search(r"if\s*\(\s*exst\s*&\s*LVS_EX_SNAPTOGRID\s*\)", body), \
        "only act (and only log) when the flag is really set"


def test_auto_arrange_handling_is_still_there():
    """The older half of the fix must not be lost while adding the new one."""
    _, body = _arrange_fn()
    assert "LVS_AUTOARRANGE" in body
    assert "FCIDM_SHVIEW_AUTOARRANGE_" in body
    assert "PostMessageA" in body, (
        "auto-arrange is a WM_COMMAND toggle and must stay a PostMessage - a "
        "synchronous send into the shell can block the agent indefinitely"
    )
