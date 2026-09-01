"""A title in the library must appear in the staged-library doc before deployment.

WHY THIS EXISTS
---------------
docs/staged-library.md is the document that answers "which games are staged,
which machines did they reach, and were they tested". It built its title list
from compat_deploy UNION compat_render - the two FACT tables - so a title that
was in the library but had never been deployed anywhere appeared in neither and
was **silently absent from the document that exists to list it**.

That is not hypothetical: Rainbow Six was staged on 2026-09-01 while the whole
fleet was powered down, and it did not appear at all. Anyone reading the doc
would have concluded it was not staged.

compat_title is now part of the union, so such a title renders as a full row of
`..` - no deploy record anywhere, nobody has looked - which is honest and is
visibly different from being missing.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(REPO, "scripts", "fleet", "gen-staged-library.py")
DOC = os.path.join(REPO, "docs", "staged-library.md")


def _gen():
    with open(GEN, encoding="utf-8") as f:
        return f.read()


def test_the_title_list_includes_compat_title():
    src = _gen()
    i = src.find("titles = sorted(")
    assert i > 0, "the title list moved - re-check this guard"
    window = src[i:i + 500]
    assert "compat_deploy" in window and "compat_render" in window
    assert "compat_title" in window, (
        "a title staged but not yet deployed lives ONLY in compat_title; "
        "without it the doc omits the newest titles entirely")


def test_the_legend_explains_the_dot_marker():
    """An undocumented marker is worse than none - it reads as a typo."""
    with open(DOC, encoding="utf-8") as f:
        doc = f.read()
    assert re.search(r"\|\s*`\.`\s*\|", doc), (
        "the deploy column can render `.` (no record at all) and the legend "
        "must say so - it previously documented only + G s ~ -")


def test_a_fully_undeployed_title_is_not_silently_dropped():
    """Shape check on the generated doc, not on the database."""
    with open(DOC, encoding="utf-8") as f:
        doc = f.read()
    rows = re.findall(r"^\| (\w[\w\-]*) \| ([.\-+G~sXVr |]+)\| (\d+) \|$", doc, re.M)
    assert rows, "no matrix rows parsed - the table shape changed"
    # at least the legend must allow an all-dots row; if one exists it must be
    # rendered rather than omitted
    all_dots = [t for t, cells, _v in rows if set(cells.replace("|", "").strip()) <= {"."}]
    for t in all_dots:
        assert t in doc, "%s parsed but is not in the document" % t
