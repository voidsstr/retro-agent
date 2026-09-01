"""tests/README.md must name every test file in the suite.

WHY THIS EXISTS
---------------
`tests/README.md` carries the fix -> test map: for each bug that was verified
fixed, what broke and which test pins it.  It is hand-written, which is right -
a generated table cannot say "found by counting rows by hand, which is not a
mechanism" - but hand-written means it drifts silently.

On 2026-09-01 an audit found 53 Python and 11 native test files with no row
anywhere in the file.  The table still READ as a complete index, so the honest
answer to "is this fix covered?" was being taken from a document that had never
been told about half the suite.  That is the same shape as every other bug this
project keeps finding: a tool reporting success.

WHAT THIS ASSERTS
-----------------
Only that every test file's BASENAME appears somewhere in tests/README.md.  It
deliberately does not check where, or in which table, or that the prose is any
good - a row can be moved, rewritten or promoted without this test caring.  It
only makes "added a test and forgot the README" impossible to land.

A file listed in EXEMPT is not a test of the fleet (a helper, a conftest).
"""
import os
import glob
import io

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
README = os.path.join(TESTS, "README.md")

# not tests of the fleet - support files that live in the same directories
EXEMPT = {
    "conftest.py",
    "__init__.py",
}


def _readme():
    return io.open(README, encoding="utf-8").read()


def _suite_files():
    out = []
    for pat in ("python/*.py", "native/*.c"):
        for f in sorted(glob.glob(os.path.join(TESTS, pat))):
            b = os.path.basename(f)
            if b not in EXEMPT:
                out.append(b)
    return out


def test_readme_exists_and_is_not_empty():
    assert os.path.isfile(README), "tests/README.md is missing"
    assert len(_readme()) > 1000, "tests/README.md is suspiciously short"


def test_every_test_file_is_named_in_the_readme():
    text = _readme()
    missing = [b for b in _suite_files() if b not in text]
    assert not missing, (
        "these test files are in the suite but named nowhere in tests/README.md:\n  "
        + "\n  ".join(missing)
        + "\n\nAdd a row to the 'Fix -> test coverage' table saying what broke and how"
        " it was found. A test with no row is a test nobody can find from the fix."
    )


def test_the_readme_does_not_name_a_test_that_no_longer_exists():
    """A stale row points at a guarantee that is gone - worse than no row."""
    import re

    text = _readme()
    present = set(_suite_files())
    named = set()
    for m in re.finditer(r"`(?:python|native)/([A-Za-z0-9_./-]+\.(?:py|c))`", text):
        named.add(os.path.basename(m.group(1)))
    gone = sorted(n for n in named if n not in present)
    assert not gone, (
        "tests/README.md names test files that do not exist:\n  "
        + "\n  ".join(gone)
        + "\n\nEither the file was renamed (update the row) or the guarantee was"
        " dropped (say so, do not just delete the row)."
    )
