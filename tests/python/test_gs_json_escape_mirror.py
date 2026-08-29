"""tests/native/test_gs_json_escape.c carries a VERBATIM copy of
gs_json_escape() from agent/src/gamesync.c - this pins the two together.

gamesync.c cannot be linked into a host test cheaply (Win32, SetupAPI, sockets,
the shell), so the native test copies the function, following the precedent of
tests/native/test_icon_bay.c and test_reg_argparse.c.

A copied implementation is worth something only while it matches. Without this
guard, editing the escaper in gamesync.c and not the test leaves the test
passing against code that no longer exists - which reads as coverage while
protecting nothing, and is worse than having no test at all.
"""

import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "gamesync.c")
MIRROR = os.path.join(REPO, "tests", "native", "test_gs_json_escape.c")
SIG = "static void gs_json_escape("


def _extract(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    i = text.index(SIG)
    j = text.index("\n}\n", i) + len("\n}\n")
    return text[i:j]


def test_mirror_matches_source():
    assert _extract(SRC) == _extract(MIRROR), (
        "tests/native/test_gs_json_escape.c's copy of gs_json_escape() has "
        "drifted from agent/src/gamesync.c. Re-copy it verbatim."
    )
