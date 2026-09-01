"""A title-level LAN fact must be WITHDRAWN when the title stops being deployed.

WHY THIS EXISTS
---------------
`ingest_lan_doc` applies a title-level fact ("needs a person once", "no
multiplayer") only to boxes where the title is actually deployed. Its own
comment says why: stamping "needs a person once" onto a box that does not have
the game "reads as a pending action that nobody can take, and buries the real
ones".

That guard only stopped a wrong row being CREATED. A row written before the
title was gated - or before the guard existed - was never revisited, so it
survived every later ingest looking exactly like a current fact. Measured
2026-09-01: `.171` still carried Halo's "needs a person / one System Link join"
long after the gate refused Halo there for lack of hardware T&L. Re-running the
ingest withdrew 16 such rows across the library.

What must NOT be withdrawn is a two-box proof. Those name their boxes
explicitly (`partner_ip` is set), and they are the most expensive facts in the
database - somebody screenshotted both ends.

NOTE the predicate this pins: `partner_ip` is `TEXT NOT NULL DEFAULT ''`, so a
title-level row holds the empty string, not NULL. The first draft of the fix
said `partner_ip IS NULL`, which matches nothing and would have deleted nothing
while reporting success.
"""
import os
import sys
import sqlite3

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "scripts", "fleet"))


def _mod():
    import importlib
    return importlib.import_module("compat")


def test_the_withdrawal_matches_the_empty_string_not_null():
    """`partner_ip IS NULL` would silently delete nothing."""
    src = open(os.path.join(REPO, "scripts", "fleet", "compat.py"),
               encoding="utf-8").read()
    i = src.find("DELETE FROM compat_mp")
    assert i > 0, "the withdrawal was removed - stale rows will come back"
    window = src[i:i + 600]
    assert "partner_ip = ''" in window, (
        "the withdrawal must match the empty string; partner_ip is "
        "NOT NULL DEFAULT '' so a NULL test matches nothing")
    # ...and the SQL itself must not test for NULL. Checked only on the lines
    # that are SQL, because the comment beside them says the word too.
    sql = " ".join(l for l in window.splitlines() if "#" not in l)
    assert "IS NULL" not in sql


def test_it_only_touches_lan_doc_title_level_rows():
    src = open(os.path.join(REPO, "scripts", "fleet", "compat.py"),
               encoding="utf-8").read()
    i = src.find("DELETE FROM compat_mp")
    window = src[i:i + 600]
    # never another source's rows, never a hand-recorded fact
    assert "source='lan-doc'" in window
    assert "origin='measured'" in window
    # scoped to one box and one title, never a bulk wipe
    assert "WHERE ip=? AND title=?" in window


def test_a_withdrawal_is_reported_not_silent():
    """A row leaving the matrix changes what the fleet reports."""
    src = open(os.path.join(REPO, "scripts", "fleet", "compat.py"),
               encoding="utf-8").read()
    assert "withdrew %d stale title-level row(s)" in src, (
        "a silent deletion is worse than a stale row - nobody can tell it "
        "happened")


def test_the_guard_that_makes_the_withdrawal_necessary_is_still_there():
    """The write side must still refuse a box that does not have the title."""
    src = open(os.path.join(REPO, "scripts", "fleet", "compat.py"),
               encoding="utf-8").read()
    assert 'd["state"] not in ("deployed", "marginal")' in src


def test_partner_ip_really_is_not_nullable():
    """Pin the schema fact the predicate depends on."""
    schema = open(os.path.join(REPO, "scripts", "fleet", "compat_db.py"),
                  encoding="utf-8").read()
    i = schema.find("CREATE TABLE IF NOT EXISTS compat_mp")
    assert i > 0
    block = schema[i:i + 900]
    line = [l for l in block.splitlines() if "partner_ip" in l]
    assert line, "partner_ip left compat_mp - re-check the withdrawal predicate"
    assert "NOT NULL" in line[0], (
        "partner_ip became nullable; the withdrawal's `= ''` predicate now "
        "misses NULL rows and must be widened")
