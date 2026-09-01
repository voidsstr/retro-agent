"""New titles must actually reach boxes - GAMESYNC will not do it by itself.

WHY THIS EXISTS
---------------
The agent's GAMESYNC startup thread gates on a bare marker check:

    if (gs_file_exists(GS_MARKER)) { ... "already provisioned" ...; return 0; }

No library comparison, no title count. Once `gamesync.done` exists the thread
idles forever, so a title staged today never reaches a box provisioned
yesterday. Rainbow Six landed on .123 only because a human typed GAMESYNC
RESET. That is a silent failure: the library grows, validate-staged-library
says DEPLOYABLE, every box looks healthy, and the new games are simply absent.

autodeploy.py closes that gap. These tests pin the two design decisions that
make it correct rather than merely busy.
"""
import ast
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "autodeploy.py")
AGENT = os.path.join(REPO, "agent", "src", "gamesync.c")


def _src():
    with open(SRC, encoding="utf-8") as f:
        return f.read()


def test_it_parses_and_has_no_escape_warnings():
    """`\\G` in the docstring is an invalid escape python will one day reject."""
    import warnings
    src = _src()
    with warnings.catch_warnings():
        warnings.simplefilter("error", SyntaxWarning)
        ast.parse(src)          # raises if any escape in the file is invalid


def test_the_marker_gate_it_compensates_for_is_still_a_bare_check():
    """If the agent ever learns to compare libraries, this tool is redundant.

    Pin the assumption rather than let it rot: a future gate that checks the
    library would make a RESET on every library change wasteful, and someone
    should notice.
    """
    with open(AGENT, encoding="utf-8", errors="replace") as f:
        c = f.read()
    i = c.find("already provisioned")
    assert i > 0, "the startup log line moved - re-check the gate"
    window = c[max(0, i - 300):i]
    assert "gs_file_exists(GS_MARKER)" in window, (
        "the startup gate changed shape - if it now compares the library, "
        "autodeploy.py's whole premise needs revisiting")


def test_it_keys_on_the_library_set_not_on_what_is_missing_from_the_box():
    """The obvious design loops forever.

    The capability gate legitimately refuses titles per box - .143 lacks SSE so
    Halo is refused, .133 lacks SSE2, .240 is refused Halo 2 on disk space.
    Those are missing BY DESIGN and never appear, so "the box is missing a
    title" is not a signal that work is needed; a tool keyed on it would
    re-sync those boxes on every pass forever.
    """
    src = _src()
    assert 'rec.get("titles") == titles' in src, (
        "the skip condition must compare the LIBRARY SET the box was last "
        "offered, not the contents of C:\\Games")
    assert "loop" in src.lower() and "gate" in src.lower(), (
        "the reasoning must stay written down next to the code")


def test_a_failed_sync_is_not_recorded_as_success():
    src = _src()
    assert 'st[ip] = {"titles": titles' in src
    i = src.find('st[ip] = {"titles": titles')
    guard = src[max(0, i - 400):i]
    assert 'state") == "done"' in guard and "failed_files" in guard, (
        "state=done ALONE hides partial failures - gs_write_marker is skipped "
        "when failed_files != 0, so the marker goes stale after a bad run")
    assert "NOT recorded" in src, "a failed run must be retried, and say so"


def test_a_box_mid_sync_is_left_alone():
    src = _src()
    assert 'gs not in (None, "idle", "done")' in src
    assert "busy" in src


def test_a_refused_connection_is_retried_not_called_dead():
    """The Win9x agents are single-threaded and refuse while busy."""
    src = _src()
    assert "ConnectionRefusedError" in src
    assert "REFUSAL_RETRIES" in src
    assert "PONG" in src, "a TCP connect is not liveness - require the protocol"


def test_it_never_reboots_anything():
    """Check the CODE, not the prose.

    The module docstring says "NEVER reboots anything", so a naive substring
    search over the whole file finds the very word it is looking for and fails
    on its own safety note. Strip comments and docstrings first - the same
    mistake a sibling test made against Rainbow Six's install.reg header.
    """
    tree = ast.parse(_src())
    code = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            continue                    # docstrings and string literals
        if isinstance(node, ast.Name):
            code.append(node.id)
        elif isinstance(node, ast.Attribute):
            code.append(node.attr)
    joined = " ".join(code).lower()
    for forbidden in ("reboot", "shutdown", "netsh", "restart"):
        assert forbidden not in joined, (
            "autodeploy calls something named %r - two fleet boxes are "
            "unactivated and must never be rebooted" % forbidden)
    # and the command strings it actually sends must be the safe ones
    sent = {n.value for n in ast.walk(tree)
            if isinstance(n, ast.Constant) and isinstance(n.value, str)
            and n.value.startswith("GAMESYNC")}
    assert sent <= {"GAMESYNC RESET", "GAMESYNC START", "GAMESYNC STATUS"}, sent


def test_dry_run_exists_and_changes_nothing():
    src = _src()
    assert "--dry-run" in src
    i = src.find("if a.dry_run:")
    assert i > 0
    assert "continue" in src[i:i + 60], "dry-run must skip the sync entirely"
