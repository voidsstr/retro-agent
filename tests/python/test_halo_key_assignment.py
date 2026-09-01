"""One Halo CD key per simultaneous player, and never the same key twice.

MEASURED 2026-08-31 on one server (.246:2302) and one key:

    .145 joins alone .......................... IN-GAME
    .240 joins while .145 is connected ........ "Your CD Key is invalid"
    .145 disconnected, .240 joins alone ....... IN-GAME

Same key, same box, same server; the only variable was whether another machine
was already using that key.

THE TRAP THIS GUARDS. Halo reports the second machine with the SAME text it
uses for a genuinely bad key -- `Your CD Key is invalid` -- and `halo.exe` has
no "already in use" string anywhere in the tree. So the natural inference,
"the game has no such message, therefore no such check", is wrong, and it was
made here: the error was read as a bad key, the fleet's key was swapped, and
the problem moved rather than went away.

`assign_keys.py` therefore refuses to hand the same key to two boxes, because
that configuration produces a misleading error instead of an honest one. These
tests pin that refusal and the no-echo rule, and they run offline.
"""
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(REPO, "scripts", "halo", "assign_keys.py")
# Halo's key alphabet is BCDFGHJKMPQRTVWXY2346789 - it deliberately omits the
# characters that look like each other (A/4, E/3, I/1, L, N, O/0, S/5, U/V, Z/2).
# A fake built from any other letter is rejected by the ENCODER before the
# duplicate check is ever reached, which made an earlier version of these tests
# fail for a reason that had nothing to do with what they were testing.
FAKE_A = "BCDFG-HJKMP-QRTVW-XY234-6789B"
FAKE_B = "CDFGH-JKMPQ-RTVWX-Y2346-789BC"


def _run(keys, boxes):
    fd, path = tempfile.mkstemp()
    try:
        os.write(fd, ("\n".join(keys) + "\n").encode())
        os.close(fd)
        return subprocess.run(
            [sys.executable, TOOL, "--keys-file", path, "--boxes", boxes,
             "--dry-run"], capture_output=True, text=True, timeout=180)
    finally:
        os.unlink(path)


def test_the_tool_exists():
    assert os.path.exists(TOOL), (
        "scripts/halo/assign_keys.py is gone; without it the fleet's Halo keys "
        "get assigned by hand, which is how two boxes end up sharing one")


def test_a_duplicate_key_is_refused():
    """The whole point. A shared key yields a MISLEADING error, not a clear one."""
    r = _run([FAKE_A, FAKE_A], "192.168.1.145,192.168.1.240")
    assert r.returncode != 0, (
        "assigning the SAME key to two boxes was allowed. That is exactly the "
        "configuration that makes Halo tell the second machine its key is "
        "invalid, which reads as a bad key and sends people to replace it.")
    assert "DUPLICATE" in (r.stdout + r.stderr).upper()


def test_distinct_keys_are_accepted():
    r = _run([FAKE_A, FAKE_B], "192.168.1.145,192.168.1.240")
    assert r.returncode == 0, (r.stdout + r.stderr)[:300]


def test_fewer_keys_than_boxes_leaves_the_remainder_ALONE():
    """Never pad with a duplicate to 'cover' every box."""
    r = _run([FAKE_A], "192.168.1.145,192.168.1.240,192.168.1.123")
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[:200]
    assert "192.168.1.240" not in r.stdout, (
        "a box beyond the key count was assigned something; with one key only "
        "one machine can play, and giving a second box the same key would "
        "recreate the misleading failure")
    assert "AT THE SAME TIME" in out, "the shortfall must be stated, not silent"


def test_no_key_is_ever_echoed():
    """Keys must not reach stdout, stderr, or argv."""
    r = _run([FAKE_A, FAKE_B], "192.168.1.145,192.168.1.240")
    blob = r.stdout + r.stderr
    for k in (FAKE_A, FAKE_B, FAKE_A.replace("-", ""), FAKE_B.replace("-", "")):
        assert k not in blob, "a CD key was printed in the tool's output"
    with open(TOOL, encoding="utf-8") as f:
        src = f.read()
    assert "--key-file" in src and '"--key",' not in src, (
        "the key must be passed to make_dpid.py by FILE, never on the command "
        "line: argv lands in shell history, in ps, and in transcripts")


def test_claude_md_records_the_limitation():
    with open(os.path.join(REPO, "CLAUDE.md"), encoding="utf-8",
              errors="replace") as f:
        t = f.read()
    assert "ONE SIMULTANEOUS PLAYER PER CD KEY" in t
    assert "no \"key already in use\" string" in t.lower() or \
           'NO "key already in use" string' in t, (
        "the doc must record WHY the wrong inference is tempting -- the game "
        "has no distinct message, so its absence proves nothing")
