"""A `verified` cell must keep at least one piece of evidence that still exists.

The matrix's whole claim is that `verified` means somebody SAW the game render
and kept the proof. That claim decays quietly: agents write screenshots into
session-scoped temp directories, the directories are cleaned, and the database
keeps pointing at them. Nothing errors -- the cell still reads `verified`, and
the proof is gone.

MEASURED 2026-08-31: of 580 file-backed evidence references, **149 no longer
exist** -- all under `/tmp/retro-screenshots` and `/tmp/lanid`. That sounds
alarming and is not, because they are duplicates: an agent typically records
both the raw `.bmp` and the converted `.png`, and only one was cleaned. Every
one of the 241 verified cells still has at least one surviving file.

**But that is luck, not design.** One more cleanup pass over the wrong
directory turns a verified cell into an assertion with nothing behind it, and
nobody would notice. So this asserts the invariant that actually matters -- not
"every reference resolves", which would fail on harmless duplicates and get
switched off, but "no verified cell has lost ALL of its evidence".

A `.133` agent flagged the same hazard from the other side and declined to
invent a durable location for its screenshots. The right long-term home is
`/home/voidsstr/lan-proof/<box>/`, which several agents already use.
"""
import os
import sqlite3

import pytest

DB = os.path.expanduser("~/.retro-fleet/fleetbook.db")


def _con():
    if not os.path.exists(DB):
        pytest.skip("SKIPPED LOUDLY: %s absent - evidence survival NOT checked" % DB)
    c = sqlite3.connect(DB)
    c.row_factory = sqlite3.Row
    return c


def test_no_verified_cell_has_lost_all_its_evidence():
    c = _con()
    try:
        cols = [r[1] for r in c.execute("PRAGMA table_info(compat_evidence)")]
        if "ref" not in cols:
            pytest.skip("SKIPPED LOUDLY: compat_evidence has no `ref` column")
        rows = c.execute("SELECT ip,title FROM compat_render "
                         "WHERE shortcut='' AND runs='verified'").fetchall()
        orphaned = []
        for r in rows:
            refs = [x[0] for x in c.execute(
                "SELECT ref FROM compat_evidence WHERE ip=? AND title=?",
                (r["ip"], r["title"]))]
            files = [p for p in refs if p and p.startswith("/")]
            # A cell whose evidence is all non-file (a log line, a note) is not
            # what this guards; only one that HAD files and lost every one.
            if files and not any(os.path.exists(p) for p in files):
                orphaned.append("%s %s" % (r["ip"], r["title"]))
    finally:
        c.close()
    assert not orphaned, (
        "%d cell(s) still read `verified` while every screenshot behind them "
        "has been deleted:\n  %s\n\nEither re-measure them or downgrade them. "
        "Evidence written to a session-scoped /tmp directory does not survive; "
        "put fleet evidence in /home/voidsstr/lan-proof/<box>/."
        % (len(orphaned), "\n  ".join(sorted(orphaned))))


def test_verified_cells_are_actually_backed_by_evidence_rows():
    """The weaker precondition: `verified` without any evidence is an opinion."""
    c = _con()
    try:
        n = c.execute("""
            SELECT COUNT(*) FROM compat_render r
            WHERE r.shortcut='' AND r.runs='verified'
              AND NOT EXISTS (SELECT 1 FROM compat_evidence e
                              WHERE e.ip=r.ip AND e.title=r.title)""").fetchone()[0]
    finally:
        c.close()
    assert n == 0, (
        "%d verified cell(s) carry no evidence row at all. `record` warns about "
        "this rather than refusing, so it can happen -- but a verification "
        "nobody can check is an opinion." % n)
