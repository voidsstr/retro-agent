"""A per-shortcut measurement must not be invisible to the matrix.

THE DEFECT, found 2026-08-31. `v_compat_matrix` -- and therefore `matrix`,
`status` and `gaps` -- joins on `shortcut=''`, because the grid has one cell per
box x TITLE. `compat.py record --shortcut ...` wrote only the shortcut-level
row, so the observation landed in `compat_render` and appeared in **no view at
all**.

Six real observations were sitting in the database that way on `.246`, four of
them `verified` with evidence. Worse than merely missing: `gaps` listed those
cells as *never looked at*, which is an instruction to go and re-measure
something already proved -- so the failure actively wasted the next agent's
time while the data sat right there.

THE FIX, and why it is shaped this way. A shortcut-level record now also
refreshes the title-level cell, **but only when that cell has no `measured` row
already**. A title-level observation is the stronger statement -- it is about
the game, not about one launcher -- so a single shortcut must never silently
overwrite it. The consequence is deliberate: recording every shortcut of a
title settles the title on whichever was recorded FIRST, which is visible and
correctable, rather than on whichever happened to run LAST.

These tests are pure source/schema checks -- no fleet, no network.
"""
import ast
import os
import sqlite3

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "compat.py")
DB = os.path.expanduser("~/.retro-fleet/fleetbook.db")


def _src():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        return f.read()


def _record_body():
    tree = ast.parse(_src())
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == "cmd_record":
            return ast.dump(node)
    raise AssertionError("cmd_record is gone from compat.py")


def test_record_mirrors_a_shortcut_measurement_to_the_title_level():
    body = _record_body()
    assert "shortcut" in body and "put_render" in body, (
        "cmd_record no longer records renderings at all")
    src = _src()
    i = src.index("def cmd_record")
    j = src.index("\ndef ", i + 1)
    fn = src[i:j]
    assert 'shortcut=""' in fn, (
        "cmd_record no longer writes a TITLE-LEVEL row for a shortcut "
        "measurement. Every view joins on shortcut='', so the observation "
        "would land in the table and appear nowhere -- and `gaps` would list "
        "the cell as never looked at, sending the next agent to re-measure it.")


def test_the_mirror_does_not_clobber_an_existing_title_level_measurement():
    """One launcher must not overwrite a statement about the whole game."""
    src = _src()
    i = src.index("def cmd_record")
    j = src.index("\ndef ", i + 1)
    fn = src[i:j]
    assert "origin='measured'" in fn and "if not have" in fn, (
        "the title-level mirror must be guarded by an existence check. Without "
        "it, recording each shortcut of a title would leave the title showing "
        "whichever launcher ran LAST, silently replacing a stronger "
        "title-level observation.")


def test_no_measurement_in_the_live_db_is_invisible():
    """The regression itself, asserted against the real database.

    Skips loudly rather than silently when the DB is absent -- a guard that
    quietly passes is the failure mode this whole file is about.
    """
    if not os.path.exists(DB):
        pytest.skip("SKIPPED LOUDLY: %s absent - shortcut visibility NOT "
                    "verified" % DB)
    con = sqlite3.connect(DB)
    try:
        cols = [r[1] for r in con.execute("PRAGMA table_info(compat_render)")]
        if not cols:
            pytest.skip("SKIPPED LOUDLY: compat_render missing - not verified")
        hidden = con.execute("""
            SELECT r.ip, r.title, r.shortcut, r.runs FROM compat_render r
            WHERE r.shortcut<>''
              AND NOT EXISTS (SELECT 1 FROM compat_render t
                              WHERE t.ip=r.ip AND t.title=r.title
                                AND t.shortcut='')
        """).fetchall()
    finally:
        con.close()
    assert not hidden, (
        "%d measurement(s) exist only at shortcut level and are invisible to "
        "the matrix, `status` and `gaps`:\n  %s\n\nRe-record them at title "
        "level. `gaps` is currently telling agents these cells were never "
        "looked at." % (len(hidden),
                        "\n  ".join("%s %s [%s] runs=%s" % h for h in hidden)))
