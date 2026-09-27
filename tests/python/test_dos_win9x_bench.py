"""dos_win9x_bench.py - DOS timedemos on a Win98 box (2026-09-27).

Pinned: fps = gametics * 35 / realtics (Doom's 35 Hz tic); the job batch feeds
stdin (Hexen's "Press any key" blocks forever otherwise), redirects stdout to
the result file and writes an .END marker the host waits for instead of
EXEC-capturing anything (CLAUDE.md: never read files through EXEC on 9x).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "benchmarks"))
import dos_win9x_bench as b  # noqa: E402


def test_parse_uses_the_35hz_tic():
    r = b.parse("W_Init...\ntimed 2134 gametics in 265 realtics\n")
    assert r == {"frames": 2134, "seconds": round(265 / 35.0, 2), "fps": round(2134 * 35.0 / 265, 1)}
    assert b.parse("no result here") is None


def test_job_batch_feeds_stdin_redirects_and_marks_the_end():
    bat = b.job_bat("J0R0", "C:\\DOOM", "DOOM.EXE -timedemo demo3 -nosound")
    lines = bat.split("\r\n")
    assert lines[1] == "C:" and lines[2] == "cd \\DOOM"
    assert lines[3] == "DOOM.EXE -timedemo demo3 -nosound < C:\\BENCH\\KEY.TXT > C:\\BENCH\\RES\\J0R0.TXT"
    assert lines[4] == "echo END> C:\\BENCH\\RES\\J0R0.END"
