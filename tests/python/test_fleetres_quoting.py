"""Every `set` FLEETRES.EXE emits must be quoted.

MEASURED ON .171, 2026-08-30. `FLEETRES.EXE -cmd` writes a batch file that each
staged launcher `call`s. One of its variables is a PCI instance id, which is
full of ampersands:

    set FR_GLIDEDEV=VEN_121A&DEV_0002&SUBSYS_00000000&REV_02

cmd.exe splits an UNQUOTED `set` on `&`, so the `call` did two wrong things at
once and both were live on hardware:

    OLD_VALUE=[VEN_121A]                        <- silently truncated
    'DEV_0002' is not recognized ...            <- and cmd RAN the rest
    'SUBSYS_00000000' is not recognized ...
    'REV_02' is not recognized ...

Three error lines in the console of every launcher on that box, and a variable
that any Glide test would then read wrong -- while the launcher still started
the game, so nothing looked broken enough to chase.

With `set "VAR=value"` the same box gives the whole value and a silent call:

    NEW_VALUE=[VEN_121A&DEV_0002&SUBSYS_00000000&REV_02]
    (no output)

`FR_MON` is EDID text straight off the monitor and is equally exposed. The
numeric lines are quoted too: `set "FR_W=1920"` does NOT put the quotes into
the value, and uniformity is worth more than two characters because the next
variable somebody adds will be a string.

(Writing this test's own harness, an unquoted `echo %FR_GLIDEDEV%` split on the
same ampersands and briefly made the FIXED build look broken. The bug class
catches the person fixing it too.)
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "provisioning", "fleetres", "fleetres.c")

# printf("set \"FR_X=%d\"\n", ...)   <- quoted, wanted
QUOTED = re.compile(r'printf\("set \\"FR_[A-Z0-9_]+=%[ds]\\"\\n"')
# printf("set FR_X=%d\n", ...)       <- bare, the bug
BARE = re.compile(r'printf\("set FR_[A-Z0-9_]+=%[ds]\\n"')


def _src():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        return f.read()


def test_no_set_is_emitted_unquoted():
    s = _src()
    bare = BARE.findall(s)
    assert not bare, (
        "%d `set` line(s) are emitted WITHOUT quotes:\n  %s\n\n"
        "cmd.exe splits an unquoted set on &, and FR_GLIDEDEV is a PCI "
        "instance id full of them -- measured on .171, the variable truncated "
        "to VEN_121A and cmd tried to RUN the remaining fragments as commands."
        % (len(bare), "\n  ".join(bare))
    )


def test_the_variables_that_actually_carry_ampersands_are_quoted():
    """Name them, so a future refactor cannot quietly drop the two that matter."""
    s = _src()
    for var in ("FR_GLIDEDEV", "FR_MON"):
        assert re.search(r'printf\("set \\"%s=%%s\\"\\n"' % var, s), (
            "%s is not emitted with quotes. It carries free-form text (a PCI "
            "instance id / EDID monitor name) and is the exact variable that "
            "broke every launcher on .171." % var
        )


def test_all_emitted_sets_are_quoted_not_merely_most():
    """A partial fix is the dangerous outcome: the unquoted survivor is the
    one nobody tests, because the quoted ones look fine."""
    s = _src()
    total = len(re.findall(r'printf\("set ', s))
    quoted = len(QUOTED.findall(s))
    assert total == quoted, (
        "%d of %d emitted `set` lines are quoted -- the remainder are the ones "
        "that will bite, because the working majority hides them" % (quoted, total)
    )
    assert total >= 20, (
        "only %d set lines found; the emitter has changed shape and this test "
        "may be measuring nothing" % total
    )
