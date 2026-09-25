"""GAMESYNC does not redo work that is already done (agent 1.85.0).

* The two tool shortcuts (Retro Agent, Retro Chat) were rebuilt through COM on
  every agent start although the comment above them promised "cheap no-ops when
  the shortcut already exists". gs_tool_shortcut() now reads the existing .lnk
  first (agent/shared/lnkcheck.h, tests/native/test_lnkcheck.c).
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "agent" / "src" / "gamesync.c"


def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
                  src, re.M | re.S)
    assert m, f"{name} not found"
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError(name)


def gs():
    return code_only(SRC.read_text(errors="replace"))


def test_an_existing_tool_shortcut_is_not_rebuilt():
    b = body(gs(), "gs_tool_shortcut")
    check = b.index("if (gs_lnk_points_at(lnk, exe))")
    assert check < b.index("gs_make_shortcut("), "check before the COM rebuild"
    after = b[check:check + 80]
    assert "return;" in after, "an existing, correct shortcut must end the call"
    # and an untouched shortcut is not counted as a desktop change
    assert b.index("gs_desk_note_lnk_written(") > b.index("gs_make_shortcut(")


def test_the_check_reads_the_file_not_com():
    b = body(gs(), "gs_lnk_points_at")
    assert "CoCreateInstance" not in b and "g_gs_CoCreateInstance" not in b
    assert "lnk_bytes_name_path(buf, got, exe)" in b
    # the agent may have been started by its short or its long path
    assert "GetShortPathNameA(" in b and "GetLongPathNameA" in b
    assert 'GetProcAddress(k, "GetLongPathNameA")' in b, \
        "GetLongPathNameA is not on Win95/NT4 - resolve it, never import it"
