#!/usr/bin/env python3
"""UE1 LAN "Host ..." launchers - the two traps that cost a session each.

WHY THIS EXISTS. Two staged Unreal Engine 1 titles ship a dedicated-server
launcher: Deus Ex ("Host Deus Ex Multiplayer.bat", game 7790 / query 7791) and
Unreal Gold ("Host Unreal Gold LAN.bat", game 7777 / query 7778 / beacon 7775,
added 2026-08-31 after a two-box LAN verification on .143 + .246).

Both were caught out by the SAME two UE1 behaviours, and both are silent:

  1. Running.ini. UE1 writes System\\Running.ini on start and deletes it on a
     CLEAN exit. Every screenshot test taskkills the game, so the file survives,
     and the NEXT start opens a "Recovery Mode" dialog instead of the server.
     GAMESYNC cannot undo it either - gs_copy_file only adds and overwrites,
     never deletes. So every direct-exe launcher in the library has to delete it
     BEFORE it starts the engine. This already invalidated one Deus Ex test run
     and it is invisible from a process list: the exe is running, it is just
     sitting on a modal dialog.

  2. Parentheses in the filename. `EXEC cmd /c start "" /D "<dir>" "Host X
     (LAN).bat"` loses its quoting by the time cmd parses it. A desktop
     double-click is unaffected, so the file looks perfect to a human and only
     automation breaks. That is the general repo rule; these are the launchers
     most likely to be named "... (LAN).bat" by reflex.

A third check is here because it is what makes the launcher REACHABLE at all:
the launcher must be named by launch.txt, or the agent makes no shortcut for it
and says nothing.

These tests read the SHARE, because the library is what deploys - a fixture
would pass while the real tree was broken. They SKIP LOUDLY when the share is
not mounted, the same contract as tests/test_staged_library.py.
"""
import glob
import os
import re

import pytest

LIB = "/mnt/retro-share/Files/Games-Library"

pytestmark = pytest.mark.skipif(
    not os.path.isdir(LIB),
    reason="SKIPPED LOUDLY: %s is not mounted - the staged UE1 host launchers "
           "were NOT checked. This is not a pass." % LIB,
)


def _host_launchers():
    """Every staged launcher whose name marks it as a multiplayer host."""
    out = []
    for path in sorted(glob.glob(os.path.join(LIB, "*", "Host *.bat"))):
        out.append(path)
    return out


def _read(path):
    with open(path, "rb") as fh:
        return fh.read().decode("latin-1")


def test_there_is_at_least_one_host_launcher():
    """A zero-length list would make every other test in this file vacuous."""
    found = _host_launchers()
    assert found, (
        "no 'Host *.bat' launcher found anywhere in %s - either the library "
        "lost its dedicated-server launchers or this glob stopped matching, "
        "and both make the rest of this file pass by doing nothing" % LIB
    )


@pytest.mark.parametrize("path", _host_launchers() or [pytest.param(None, marks=pytest.mark.skip)])
def test_host_launcher_filename_has_no_parentheses(path):
    name = os.path.basename(path)
    assert "(" not in name and ")" not in name, (
        "%s: a generated launcher filename must not contain ( or ) - "
        "`start \"\" /D <dir> \"%s\"` loses its quoting in cmd and the agent "
        "cannot launch it, while a desktop double-click still works. Use a "
        "dash." % (path, name)
    )


@pytest.mark.parametrize("path", _host_launchers() or [pytest.param(None, marks=pytest.mark.skip)])
def test_ue1_host_launcher_clears_running_ini_before_starting(path):
    """Only for UE1 trees - identified by the engine's own Running.ini trap."""
    tree = os.path.dirname(path)
    # A UE1 tree is one with a System dir holding Core.dll/Engine.dll.
    sysdirs = [d for d in os.listdir(tree)
               if d.lower() == "system" and os.path.isdir(os.path.join(tree, d))]
    if not sysdirs:
        pytest.skip("%s is not a UE1 tree (no System directory)" % tree)
    entries = {e.lower() for e in os.listdir(os.path.join(tree, sysdirs[0]))}
    if "core.dll" not in entries or "engine.dll" not in entries:
        pytest.skip("%s is not a UE1 tree (no Core.dll/Engine.dll)" % tree)

    text = _read(path)
    lowered = text.lower()

    del_at = None
    for m in re.finditer(r"del\s+[^\r\n]*running\.ini", lowered):
        del_at = m.start() if del_at is None else min(del_at, m.start())
    assert del_at is not None, (
        "%s never deletes System\\Running.ini. UE1 leaves that file behind "
        "whenever the process is killed - which every automated test does - and "
        "the next start then shows a Recovery Mode dialog instead of the "
        "server. GAMESYNC cannot clear it (gs_copy_file never deletes), so the "
        "launcher has to." % path
    )

    start_at = None
    for m in re.finditer(r"(?m)^\s*start\s", lowered):
        start_at = m.start() if start_at is None else min(start_at, m.start())
    assert start_at is not None, (
        "%s has no `start` line - it does not launch anything" % path
    )
    assert del_at < start_at, (
        "%s deletes Running.ini AFTER it starts the engine, which is the same "
        "as not deleting it at all" % path
    )


@pytest.mark.parametrize("path", _host_launchers() or [pytest.param(None, marks=pytest.mark.skip)])
def test_host_launcher_is_named_by_launch_txt(path):
    """An unlisted launcher gets no desktop shortcut, silently."""
    tree = os.path.dirname(path)
    lt = os.path.join(tree, "launch.txt")
    assert os.path.isfile(lt), "%s has a host launcher but no launch.txt" % tree
    # The agent reads only the first 1023 bytes, so check inside that window.
    with open(lt, "rb") as fh:
        head = fh.read(1023).decode("latin-1")
    name = os.path.basename(path)
    targets = []
    for line in head.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        targets.append(line.split("\t")[0].strip().lower())
    assert name.lower() in targets, (
        "%s is not named in the first 1023 bytes of %s, so the agent makes no "
        "shortcut for it and logs nothing. Data lines must come first."
        % (name, lt)
    )
