#!/usr/bin/env python3
"""`xcopy` issued through the agent must be given a STDIN, or it copies nothing
and says so with exit code 0.

THE MEASUREMENT (.143, 2026-09-01)
----------------------------------
Deploying a game tree from the share stalled on a command that looked perfect:

    EXECW 900 cmd /c xcopy "Z:\\...\\*" "C:\\Games\\X\\" /E /I /Y /H /R

It returned in 0.3 s with NO output, NO error, exit code 0, and an empty target
directory. So did `EXEC cmd /c xcopy /?` — the *help text* produced nothing,
which is what ruled out every theory about paths, quoting, the share and the
box. Adding one redirect fixed it outright:

    EXEC cmd /c xcopy /? < nul          -> the full help
    EXECW 900 cmd /c xcopy ... < nul    -> "147 File(s) copied"

WHY. xcopy asks whether the destination is a file or a directory and reads the
answer from stdin. The agent's `EXEC`/`EXECW` run children hidden with no stdin
handle, so xcopy hits that read immediately, gives up, and exits 0 having done
nothing. It is not "broken on some boxes" and it is not the missing console:
`< nul` works under the ordinary hidden exec, with no console anywhere.

This repo had already paid for the symptom three times over - the skills carried
"xcopy is broken on several fleet XP boxes (RC=0 but copies nothing)", a
robocopy fallback, and a whole `tree_copy_via_batch()` that rebuilds a tree with
`mkdir` + `copy *.*` per subdirectory. All of that was built around a diagnosis
that stopped one step short.

`cmd /c start /wait "" xcopy ...` also works, because a new console brings a
stdin with it. It is accepted here and it is the worse fix: `start` detaches, so
the exit code belongs to `start` and not to xcopy - and "check the exit code of
the thing that actually did the work" is a standing rule in CLAUDE.md.

WHAT THIS TEST GUARDS. Any xcopy this project sends to an agent carries a stdin
redirect (or the `start /wait` form). The failure it prevents is silent by
construction: the tool reports success, the operator believes it, and the empty
directory is found hours later on a box.
"""
import os
import re

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))

#: Where an agent command string can be built. Everything else in the repo that
#: mentions xcopy is prose, and prose is checked separately below.
SCAN_DIRS = ("scripts", "provisioning", "client", "agent", ".claude/skills")
SCAN_EXT = (".py", ".sh", ".bat", ".cmd", ".c", ".h")

#: An xcopy inside a string that also names EXEC/EXECW is one WE issue through
#: the agent. A .bat staged into a game tree runs from a real console (a desktop
#: double-click) and has a stdin already, so it is deliberately not caught here.
AGENT_EXEC = re.compile(r"EXEC(?:W\s+\d+)?\b")
XCOPY = re.compile(r"\bxcopy\b", re.I)
HAS_STDIN = re.compile(r"<\s*nul", re.I)
HAS_CONSOLE = re.compile(r"start\s+/wait", re.I)


def _candidate_lines():
    for rel in SCAN_DIRS:
        root = os.path.join(REPO, rel)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in ("__pycache__", ".git", "build", "out")]
            for fn in filenames:
                if not fn.endswith(SCAN_EXT):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    with open(path, encoding="utf-8", errors="replace") as fh:
                        for n, line in enumerate(fh, 1):
                            if XCOPY.search(line) and AGENT_EXEC.search(line):
                                yield os.path.relpath(path, REPO), n, line.rstrip()
                except OSError:
                    continue


def test_every_agent_issued_xcopy_has_a_stdin():
    offenders = [(p, n, l) for p, n, l in _candidate_lines()
                 if not (HAS_STDIN.search(l) or HAS_CONSOLE.search(l))]
    assert not offenders, (
        "these agent-issued xcopy commands have no stdin, so on a fleet box "
        "they copy NOTHING and return 0:\n" +
        "\n".join("  %s:%d  %s" % o for o in offenders) +
        "\n\nAdd `< nul` (preferred - keeps xcopy's own exit code) or use "
        "`cmd /c start /wait \"\" xcopy ...`.")


def test_the_scan_actually_finds_the_call_sites_it_is_guarding():
    """A guard that matches nothing passes forever. This repo really does issue
    xcopy through the agent, so an empty scan means the pattern rotted."""
    found = list(_candidate_lines())
    assert found, (
        "no agent-issued xcopy found anywhere - either every call site was "
        "removed (then delete this test) or the EXEC/xcopy pattern no longer "
        "matches how commands are built, in which case this test is guarding "
        "nothing while reporting green")


def test_the_skill_docs_name_stdin_as_the_cause_not_a_broken_box():
    """"xcopy is broken on several fleet XP boxes" sent three separate pieces of
    machinery down a fallback path. The doc has to name the real cause or the
    next agent rebuilds the same workaround."""
    doc = os.path.join(REPO, ".claude", "skills", "install-utility", "SKILL.md")
    if not os.path.isfile(doc):
        pytest.skip("SKIPPED LOUDLY: %s absent - the xcopy cause is NOT "
                    "documented anywhere this test can see" % doc)
    with open(doc, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    assert "< nul" in text, (
        "install-utility/SKILL.md no longer prescribes `< nul` for xcopy")
    assert "STDIN" in text.upper(), (
        "install-utility/SKILL.md no longer names stdin as the cause")
