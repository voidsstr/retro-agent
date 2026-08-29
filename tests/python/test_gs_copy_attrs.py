"""GAMESYNC must be able to overwrite a HIDDEN or READ-ONLY file, and must not
break its own status JSON when reporting the path that failed.

WHY THIS EXISTS. `CreateFileA(dst, ..., CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
NULL)` fails with ERROR_ACCESS_DENIED (5) when the destination already exists
and carries FILE_ATTRIBUTE_HIDDEN or _READONLY and the call does not pass that
same attribute back. Several staged trees legitimately ship hidden files -
CounterStrike16 alone has BCShield.asi, BCShield.dll, rev.ini,
cstrike\\liblist.gam and restart_debug.bat - so this was not an edge case:

  * it made `failed_files == 0` UNSATISFIABLE on every box in the fleet, and
  * `gs_write_marker()` is skipped when failed_files != 0, so `gamesync.done`
    went stale everywhere as a knock-on.

It stayed hidden for months because `gs_copy_file()` returns success early for
any file already at the right size, so only a hidden file whose size DIFFERS
from the library's copy ever reaches the failing call. Once a staged hidden file
was edited, the box could never accept the new version - permanently.

Two invariants are pinned here, both source-level, because the real call needs
Win32:
  1. SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL) is issued BEFORE the
     destination CreateFileA - order matters, after is useless.
  2. The status JSON escapes `failed_file`. It is the first field carrying a
     full path, and a raw Windows path contains \\G, \\C, \\r ... - invalid or,
     worse, silently valid escapes that corrupt the parse.
"""

import os
import re

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "gamesync.c")


def _src():
    with open(SRC, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _fn(text, signature):
    i = text.index(signature)
    j = text.index("\n}\n", i) + len("\n}\n")
    return text[i:j]


def test_attributes_are_cleared_before_the_destination_is_opened():
    body = _fn(_src(), "static int gs_copy_file(")

    assert "SetFileAttributesA" in body, (
        "gs_copy_file() must clear HIDDEN/READONLY before CREATE_ALWAYS, or a "
        "hidden staged file can never be updated on a box again"
    )
    set_at = body.index("SetFileAttributesA")
    # the DESTINATION open - the second CreateFileA in the function
    opens = [m.start() for m in re.finditer(r"CreateFileA\(dst", body)]
    assert opens, "destination CreateFileA(dst, ...) not found"
    assert set_at < opens[0], (
        "SetFileAttributesA must come BEFORE CreateFileA(dst, ...); clearing "
        "attributes after the open that failed accomplishes nothing"
    )


def test_the_early_size_match_still_short_circuits():
    """The fix must not cost a full re-copy of every already-present file."""
    body = _fn(_src(), "static int gs_copy_file(")
    early = body.index("already == src_size")
    set_at = body.index("SetFileAttributesA")
    assert early < set_at, (
        "the same-size early-out must stay ahead of the attribute clear, or "
        "every resumed sync rewrites attributes across the whole library"
    )


def test_the_first_failing_path_is_recorded():
    src = _src()
    assert "failed_file" in src, (
        "the status must name the file that FAILED; current_file is merely the "
        "one the walker was last on, which points at an unrelated title"
    )
    assert re.search(r"if \(g_gs\.failed_files == 0\)\s*\n\s*lstrcpynA\(g_gs\.failed_file",
                     src), (
        "record the FIRST failure, not the last - the first is the one that "
        "started the trouble"
    )


def test_failed_file_is_json_escaped():
    src = _src()
    assert "gs_json_escape" in src, "the status JSON needs an escaper"
    # it must be applied to failed_file, and the raw field must NOT be the arg
    assert re.search(r"gs_json_escape\(s\.failed_file,\s*esc_failed", src), (
        "failed_file must be escaped into a buffer before formatting"
    )
    # the raw field must not appear in the _snprintf argument list
    call = src[src.index("_snprintf(json, sizeof(json) - 1,"):]
    call = call[:call.index("\n        s.message);") + 20]
    # NB word boundary: "s.failed_files" (the COUNT) legitimately appears here
    # and contains "s.failed_file" as a substring.
    assert not re.search(r"s\.failed_file\b", call), (
        "the RAW s.failed_file must not reach the format arguments - a Windows "
        "path emitted raw produces invalid JSON and loses the whole response"
    )
    assert "esc_failed" in call, "the escaped copy must be what is formatted"


def test_escape_buffer_can_hold_a_fully_escaped_path():
    src = _src()
    m = re.search(r"char\s+esc_failed\[([^\]]+)\]", src)
    assert m, "esc_failed buffer not declared"
    expr = m.group(1)
    assert "* 2" in expr.replace(" ", " ") or "*2" in expr, (
        "the escape buffer must be at least 2n+1: every byte can double"
    )
