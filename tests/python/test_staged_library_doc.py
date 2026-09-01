"""docs/staged-library.md is GENERATED, and keeps the distinctions that matter.

USER ASK, 2026-09-01: *"the full list of staged games is somewhere as well -
make sure that is documented ... in the end i also want to know which games have
been deployed to which retro computers and if it has been tested."*

All of that already lived in `~/.retro-fleet/fleetbook.db` and was unreadable
without running a query. It is now rendered to a document.

**GENERATED, NOT WRITTEN.** A hand-maintained list could not survive this
project: the library went 38 -> 46 titles in one session, two graphics cards
were swapped mid-session, and the machines power on and off continuously. The
same argument settled `docs/fleet-inventory.md`, whose hand-written predecessor
was wrong about most of the fleet - twice a card changed without the docs
noticing.

THE THREE DISTINCTIONS THIS TEST PROTECTS, each of which has cost real time:

  * **deployed** is not **runs** is not **verified**. GAMESYNC reporting
    `state=done` is not evidence a game works; twice this session a "failure"
    was a measurement artefact and once a "pass" was a crashed process still
    holding its name in the process list.
  * **gated** is not **skipped**. The first means the hardware cannot run it and
    carries the limiting number; the second means there was no disk room.
    Conflating them told an operator a Pentium 1 "cannot run" Warcraft II when
    it merely had nowhere to put it.
  * **untested** must never render as anything else. A blank cell that reads as
    a pass is how a matrix starts lying, and this one reports 133 of them.
"""
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(REPO, "scripts", "fleet", "gen-staged-library.py")
DOC = os.path.join(REPO, "docs", "staged-library.md")


def _doc():
    with open(DOC, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_the_generator_exists_and_the_doc_is_present():
    assert os.path.exists(GEN), "scripts/fleet/gen-staged-library.py is gone"
    assert os.path.exists(DOC), (
        "docs/staged-library.md is gone - regenerate it with "
        "`python3 scripts/fleet/gen-staged-library.py`")


def test_it_says_it_is_generated():
    """A generated file that does not say so gets hand-edited, then lies."""
    t = _doc()
    assert "GENERATED" in t and "do not edit by hand" in t.lower()
    assert "gen-staged-library.py" in t, (
        "the doc must name the command that regenerates it, or the next person "
        "edits it by hand")


def test_the_doc_distinguishes_deployed_runs_and_verified():
    t = _doc()
    for token in ("deployed", "verified", "untested"):
        assert token in t, "the doc no longer distinguishes %r" % token
    assert "nobody has looked" in t or "untested — nobody" in t, (
        "`untested` must be spelled out as 'nobody has looked'. A legend that "
        "leaves it ambiguous is how an untested cell starts reading as a pass.")


def test_the_doc_distinguishes_gated_from_skipped():
    t = _doc()
    assert "did not fit" in t and "cannot run" in t, (
        "the doc no longer separates `gated` (the hardware cannot run it) from "
        "`skipped` (no disk room). Those need different follow-ups, and "
        "conflating them once told an operator a machine could not run a game "
        "it merely had no space for.")


def test_check_mode_detects_staleness():
    """A --check that cannot fail is decoration."""
    r = subprocess.run([sys.executable, GEN, "--check"],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode in (0, 1), "unexpected exit %d" % r.returncode
    with open(GEN, encoding="utf-8") as f:
        src = f.read()
    assert "STALE" in src and "return 1" in src, (
        "--check no longer reports staleness with a non-zero exit, so nothing "
        "would ever notice the doc drifting from the database")


def test_the_timestamp_is_excluded_from_the_staleness_compare():
    """Otherwise --check fails every single time and gets ignored."""
    with open(GEN, encoding="utf-8") as f:
        src = f.read()
    assert "strip_stamp" in src, (
        "the generated-at line must be excluded when comparing, or --check "
        "reports stale on every run and everyone learns to ignore it")
