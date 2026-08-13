"""Regression: DOS staging freshness and COMMAND.COM batch hazards.

Every case here is a defect that was found on the fleet's Win98 box
(192.168.1.243) and verified against its disk state on 2026-08-13.

These are true-source tests: they read agent/src/dosstage.c and the shipped
.BAT files rather than a copy, so the invariant cannot drift away from the code
that ships.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DOSSTAGE_C = os.path.join(ROOT, "agent", "src", "dosstage.c")
DG = os.path.join(ROOT, "scripts", "dosgames")


def _read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# --------------------------------------------------------------- staging ----

def test_staging_does_not_compare_size_alone():
    """A same-size edit must still reach the box.

    The DOS payload is mostly batch scripts, where the ordinary fix preserves
    length exactly: NETUP.BAT's probe io 0x300 -> 0x320, `goto try4` ->
    `goto try5`, PACKETINT 0x62 -> 0x63. With a size-only compare, publishing
    such a fix left every already-staged box on the broken file forever while
    dosstage_run logged "staged 0 file(s)" and wrote the DosStaged marker as if
    it had worked. The agent's own updater carries a .ver sidecar because it
    learned this once already.
    """
    src = _read(DOSSTAGE_C)
    body = src[src.index("static int copy_if_different"):]
    body = body[:body.index("\n}")]
    assert "CompareFileTime" in body, \
        "copy_if_different still decides on size alone"
    # the old unconditional early-out must be gone
    assert not re.search(r"if\s*\(\s*d\s*==\s*s\s*\)\s*return\s*0", body), \
        "the size-equal early-out is still there"


def test_staging_force_actually_restages():
    """DOSSTAGE force used to only bypass the DosStage=0 enable flag.

    An operator looking at a stale file on a box had no way to replace it short
    of deleting it by hand.
    """
    src = _read(DOSSTAGE_C)
    assert "g_stage_force" in src, "force is not threaded into the copy decision"
    body = src[src.index("static int copy_if_different"):]
    body = body[:body.index("\n}")]
    assert "g_stage_force" in body, \
        "copy_if_different ignores force, so DOSSTAGE force cannot re-stage"


def test_staging_reads_size_and_time_in_one_call():
    """FindFirstFileA already returns both; a second call would double the
    per-file cost of a scan that runs over SMB on a Pentium-1."""
    src = _read(DOSSTAGE_C)
    assert "file_stat_of" in src
    body = src[src.index("static int file_stat_of"):]
    body = body[:body.index("\n}")]
    assert body.count("FindFirstFileA") == 1


# ------------------------------------------------------- COMMAND.COM bat ----

def _shipped_bats():
    return [os.path.join(DG, n) for n in sorted(os.listdir(DG))
            if n.upper().endswith(".BAT")]


def test_no_angle_brackets_in_rem_comments():
    """COMMAND.COM parses redirection on EVERY line, `rem` included.

    DOSGAME.BAT's comment about errorlevel 43 was written with angle brackets;
    COMMAND.COM saw the `>`, skipped the `=` (an argument delimiter and an
    illegal FAT character), and created a 0-byte file named `43` -- once per
    pass through the menu loop, in whatever directory it was standing in.
    RUN.BAT cd's into the game's directory and a batch cd persists into its
    caller, which is why the box grew C:\\43, C:\\KEEN\\43, C:\\DUKE\\43 and
    C:\\GAMES\\HERETIC\\43.
    """
    offenders = []
    for path in _shipped_bats():
        for n, line in enumerate(_read(path).splitlines(), 1):
            if re.match(r"^\s*rem\b", line, re.I) and re.search(r"[<>]", line):
                offenders.append("%s:%d: %s" % (os.path.basename(path), n,
                                                line.strip()))
    assert not offenders, (
        "a rem comment contains an angle bracket, which COMMAND.COM will treat "
        "as a redirect and turn into a stray file:\n  " + "\n  ".join(offenders))


def test_agentrun_does_not_rely_on_a_bare_unc_path():
    """On Windows 9x a bare UNC path is unreadable without an authenticated
    session -- FindFirstFile/copy silently find nothing (hardware-confirmed).

    AGENTRUN.BAT depended on MAPSHARE.BAT, a sibling Run-key entry, having
    already created that session; Run-key order is undefined and MAPSHARE
    itself retries for up to ~53s because the network is not up at logon. So on
    a cold boot the whole share half of the updater was skipped silently.
    """
    src = _read(os.path.join(DG, "AGENTRUN.BAT"))
    assert re.search(r"^net use ", src, re.M), \
        "AGENTRUN.BAT does not establish its own authenticated session"
    assert re.search(r"SET AGSHARE=E:", src), \
        "AGENTRUN.BAT never prefers the mapped drive letter over the UNC path"
    assert "share not readable" in src, \
        "a skipped update is still invisible in the log"


def test_batch_files_have_crlf_and_no_long_lines():
    """DOS reads a batch line into a 128-byte buffer and chops the rest."""
    for path in _shipped_bats():
        raw = open(path, "rb").read()
        for n, line in enumerate(raw.split(b"\n"), 1):
            line = line.rstrip(b"\r")
            assert len(line) <= 126, (
                "%s:%d is %d bytes; COMMAND.COM truncates past 126"
                % (os.path.basename(path), n, len(line)))
