"""GAMESYNC must merge a title's install.reg on Windows 9x - and prove it did.

THE DEFECT (agent 1.86.1, .243 = Win98 SE, 2026-09-28):

    [11:24:01][GAMESYNC] HexenII: cannot run regedit (0) - game may not launch

`gs_merge_reg()` started regedit with

    CreateProcessA(NULL, "regedit /s \\"<long path>\\"", ..., 0, NULL, NULL, ...)

- the module left to the search order, a quoted long path, creation flags 0,
the inherited working directory, from the THREAD_PRIORITY_IDLE sync thread.
Windows 98 refused it and gave no error code, so no Win9x box had ever had an
install.reg merged (Hexen II's RAID value is what stops "You need to
re-install Hexen 2"). The 9x path now names %windir%\\REGEDIT.EXE, passes the
8.3 .reg path unquoted, sets the working directory, starts it CREATE_SUSPENDED
and resumes it - the forms proven on that box - waits pumping messages, and
then reads every value in the .reg back from the registry, writing what regedit
did not land and saying so loudly.

tests/native/test_regmerge.c pins the pure logic (agent/shared/regmerge.h).
This file pins the WIRING in agent/src/gamesync.c that the native test cannot
see, and runs the real reader over every staged install.reg, so a future
library file the post-condition cannot judge is caught here rather than by a
box that silently skips its values.
"""

import os
import re
import shutil
import subprocess
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
GAMESYNC = os.path.join(REPO, "agent", "src", "gamesync.c")
HEADER = os.path.join(REPO, "agent", "shared", "regmerge.h")
LIB = "/mnt/retro-share/Files/Games-Library"


def _src():
    with open(GAMESYNC, encoding="latin-1") as fh:
        return fh.read()


def _func(src, name):
    m = re.search(r"\n(?:static )?[\w \*]+\b%s\([^;{]*\)\n\{\n.*?\n\}\n" % re.escape(name),
                  src, re.S)
    assert m, "%s() not found in gamesync.c" % name
    return m.group(0)


# ---------------------------------------------------------------------------
# the launch


def test_old_failing_call_is_gone():
    """The exact 1.86.1 call - flags 0, NULL working directory - and its
    uninformative log line must not come back."""
    src = _src()
    assert "CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)" not in src, \
        "the 1.86.1 regedit launch (flags 0, inherited cwd) is back"
    assert '"regedit /s \\"%s\\""' not in src, \
        "gamesync.c formats its own regedit command again - use rm_build_cmd()"
    assert "cannot run regedit (%lu)" not in src, \
        "the 1.86.1 log line (a bare error number, 0 on Win98) is back"


def test_launch_uses_the_shared_builder_and_its_flags():
    body = _func(_src(), "gs_run_regedit")
    assert "rm_build_cmd(is_9x, regedit, arg, cmd, sizeof(cmd), &flags)" in body
    m = re.search(r"CreateProcessA\(NULL, cmd, NULL, NULL, FALSE, \(DWORD\)flags, NULL, cwd,", body)
    assert m, "CreateProcessA must take the builder's flags and an explicit cwd"
    # 9x: absolute REGEDIT.EXE under the Windows directory, checked first, and
    # the 8.3 form of the .reg path; the working directory is the Windows dir.
    assert "GetWindowsDirectoryA(windir" in body and "REGEDIT.EXE" in body
    assert "gs_file_exists(regedit)" in body, "a missing REGEDIT.EXE must be named, not searched for"
    assert "GetShortPathNameA(reg_path" in body
    assert "cwd = windir;" in body


def test_lasterror_is_captured_before_anything_else():
    """1.86.1 logged GetLastError() as an argument. Now it is read into a local
    as the FIRST statement of the failure branch, and the log says when Windows
    gave no reason at all (0 - exactly what .243 returned)."""
    body = _func(_src(), "gs_run_regedit")
    m = re.search(r"if \(!CreateProcessA\([^\n]*\n\s*gle = GetLastError\(\);", body)
    assert m, "GetLastError() must be the first thing read after CreateProcessA fails"
    assert "Windows gave no reason" in body


def test_suspended_child_is_resumed_and_a_hung_one_killed():
    body = _func(_src(), "gs_run_regedit")
    assert "(flags & RM_CREATE_SUSPENDED) && ResumeThread(pi.hThread)" in body, \
        "a CREATE_SUSPENDED regedit that is never resumed merges nothing"
    assert "gs_wait_pumping(pi.hProcess, GS_REGEDIT_WAIT_MS)" in body
    assert "TerminateProcess(pi.hProcess, 1);" in body, \
        "a regedit stuck on a dialog must not be left behind (9x will not shut down past it)"
    wait = _func(_src(), "gs_wait_pumping")
    assert "MsgWaitForMultipleObjects" in wait and "PeekMessageA" in wait, \
        "the sync thread is a COM apartment - its wait must pump messages"


def test_priority_raised_only_around_the_9x_launch_and_restored():
    body = _func(_src(), "gs_run_regedit")
    raise_at = body.index("SetThreadPriority(me, THREAD_PRIORITY_NORMAL)")
    create_at = body.index("CreateProcessA(")
    restore_at = body.index("SetThreadPriority(me, prio)")
    assert raise_at < create_at < restore_at
    assert "if (is_9x) {\n        prio = GetThreadPriority(me);" in body


def test_no_flag_9x_rejects_is_passed():
    """CREATE_NO_WINDOW / CREATE_UNICODE_ENVIRONMENT make Win95/98 fail
    CreateProcess with error 87. None may reach the regedit launch."""
    body = _func(_src(), "gs_run_regedit")
    for bad in ("CREATE_NO_WINDOW", "CREATE_UNICODE_ENVIRONMENT"):
        assert bad not in body


# ---------------------------------------------------------------------------
# the post-condition


def test_merge_verifies_every_value_and_completes_on_9x():
    body = _func(_src(), "gs_merge_reg")
    run_at = body.index("gs_run_regedit(reg_path, is_9x, why, sizeof(why))")
    verify_at = body.index("gs_reg_verify(text, len, 0, &t, NULL, NULL)")
    apply_at = body.index("gs_reg_verify(text, len, 1, &t, &written, &werr)")
    assert run_at < verify_at < apply_at, "run regedit, THEN read back, THEN complete"
    # The direct write is for 9x, or wherever regedit did not run cleanly;
    # an NT box where regedit ran reports instead of rewriting.
    assert "else if (ran != GS_REGEDIT_OK || is_9x) {" in body
    assert "REGEDIT DID NOT MERGE install.reg" in body, "the fallback must say it is one"
    assert "INSTALL.REG NOT FULLY MERGED" in body
    assert "merged install.reg via regedit - %d/%d value(s) verified" in body


def test_resolution_pass_still_runs_after_the_merge():
    """gs_merge_reg() changed shape; the ordering CLAUDE.md calls THE fix
    (the resolution pass after install.reg) must survive it."""
    src = _src()
    run = src[src.index("if (gs_copy_tree(src, dst)) {"):]
    assert run.index("gs_merge_reg(dst, titles[i]);") < run.index("gameres_apply_title(dst, titles[i]")


def test_verify_checks_values_not_just_keys():
    """HexenII's key (ComputerName\\ComputerName) exists on every Windows box,
    so a key-exists check would pass with the merge never having happened."""
    holds = _func(_src(), "gs_reg_entry_holds")
    assert "RegQueryValueExA(k, e->name[0] ? e->name : NULL" in holds
    assert "rm_value_matches(e," in holds


# ---------------------------------------------------------------------------
# the real reader over the real library

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "regmerge.h"

int main(int argc, char **argv)
{
    static char buf[1 << 20];
    FILE *f = fopen(argv[1], "rb");
    size_t n;
    rm_parser_t *ps = malloc(sizeof(*ps));
    rm_entry_t *e = malloc(sizeof(*e));
    (void)argc;
    if (!f || !ps || !e)
        return 2;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    rm_init(ps, buf, n);
    while (rm_next(ps, e))
        printf("%d\t%d\t%s\t%s\t%u\t%u\t%d\n", e->op, e->root, e->key, e->name,
               e->type, e->len, e->lineno);
    printf("DIALECT\t%d\n", ps->dialect);
    return 0;
}
"""


def _cc():
    for cand in (os.environ.get("CC"), "gcc", "cc", "clang"):
        if cand and shutil.which(cand):
            return cand
    return None


@pytest.fixture(scope="module")
def reader():
    cc = _cc()
    assert cc, "no host C compiler - the reader the agent runs would go untested"
    d = tempfile.mkdtemp(prefix="regmerge-")
    src = os.path.join(d, "driver.c")
    with open(src, "w") as fh:
        fh.write(DRIVER)
    exe = os.path.join(d, "driver")
    subprocess.run([cc, "-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
                    "-I", os.path.dirname(HEADER), src, "-o", exe],
                   check=True, capture_output=True)
    return exe


def _read(exe, path):
    out = subprocess.run([exe, path], capture_output=True, text=True, check=True,
                         encoding="latin-1").stdout.splitlines()
    dialect = int(out[-1].split("\t")[1])
    rows = [ln.split("\t") for ln in out[:-1]]
    return dialect, rows


def _library_regs():
    regs = []
    for t in sorted(os.listdir(LIB)):
        if t.startswith("_"):
            continue
        d = os.path.join(LIB, t)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.lower() == "install.reg":
                regs.append((t, os.path.join(d, f)))
    return regs


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted, so the install.reg reader "
                           "was NOT checked against the staged library" % LIB)
def test_every_staged_install_reg_is_fully_judgeable(reader):
    """Every value every staged install.reg sets must be one the agent can read
    back - otherwise the post-condition silently covers less than the file."""
    regs = _library_regs()
    assert regs, "the library holds no install.reg at all - wrong path?"
    problems = []
    judged = 0
    for title, path in regs:
        dialect, rows = _read(reader, path)
        if dialect != 1:
            problems.append("%s: not REGEDIT4 (dialect %d) - merges nowhere on 9x" % (title, dialect))
        if not rows:
            # BF1942 and Turok2 are comment-only by design ("THIS FILE
            # DELIBERATELY WRITES NOTHING"). Allowed - but only if that is all
            # the file is, which the reader has just shown.
            continue
        judged += 1
        for op, root, key, name, typ, ln, line in rows:
            if op == "4":
                problems.append("%s line %s: a line the agent cannot judge" % (title, line))
    assert not problems, "\n".join(problems)
    assert judged >= len(regs) - 4, \
        "most install.reg files set nothing? the reader has stopped seeing values"


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted" % LIB)
def test_hexen2_install_reg_yields_the_raid_value(reader):
    """The file .243 never merged: its one entry is the RAID string the
    hardware check reads back."""
    dialect, rows = _read(reader, os.path.join(LIB, "HexenII", "install.reg"))
    assert dialect == 1
    assert rows == [["1", "3", r"SYSTEM\CurrentControlSet\Control\ComputerName\ComputerName",
                     "RAID", "1", str(len("Santa needs a new sled!")), rows[0][6]]]
