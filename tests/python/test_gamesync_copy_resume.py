"""GAMESYNC resumes a file after a failed read instead of abandoning it.

Found on .243 (Win98 SE, 3C509, 2026-09-24): Win98's network client dropped
long SMB reads from the NAS with error 55 ("the specified network resource is
no longer available") 7-40 s into Hexen II's 22 MB and 77 MB paks, on every
run. gs_copy_file() gave up on the first failed ReadFile, so the title never
finished, gamesync.done was never written, and the box re-walked the whole
library every 120 s.

Run: pytest tests/python/test_gamesync_copy_resume.py
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "agent" / "src" / "gamesync.c"


def _copy_fn():
    s = SRC.read_text(errors="replace")
    i = s.index("static int gs_copy_file(")
    return s[i:s.index("\n}\n", i)]


def test_a_failed_read_reopens_the_source_and_seeks_to_where_it_was():
    fn = _copy_fn()
    fail = fn[fn.index("if (!ReadFile(hs, buf, GS_CHUNK, &rd, NULL)) {"):]
    fail = fail[:fail.index("ok = 0;")]
    assert "CloseHandle(hs);" in fail and "CreateFileA(src, GENERIC_READ" in fail
    assert "SetFilePointer(hs, (LONG)(copied & 0xFFFFFFFF), &hi, FILE_BEGIN)" in fail
    assert "continue;" in fail, "a successful reopen must resume the loop"


def test_resumes_are_bounded_and_refill_only_on_progress():
    fn = _copy_fn()
    assert re.search(r"#define GS_READ_RETRIES\s+\d+", SRC.read_text())
    assert "retries < GS_READ_RETRIES" in fn
    assert "copied - last_fail_at >= 8 * 1024 * 1024" in fn, \
        "a source failing at the same byte every time must still give up"


def test_the_offset_counts_only_bytes_actually_written():
    fn = _copy_fn()
    w = fn.index("if (!WriteFile(hd, buf, rd, &wr, NULL) || wr != rd) {")
    assert fn.index("copied += rd;") > w, "count bytes after the write succeeded"


def test_a_file_that_still_fails_is_not_left_looking_complete():
    fn = _copy_fn()
    assert "if (!ok)\n        DeleteFileA(dst);" in fn
