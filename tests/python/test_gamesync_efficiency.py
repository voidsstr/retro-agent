"""GAMESYNC does not redo work that is already done (agent 1.85.0).

* The two tool shortcuts (Retro Agent, Retro Chat) were rebuilt through COM on
  every agent start although the comment above them promised "cheap no-ops when
  the shortcut already exists". gs_tool_shortcut() now reads the existing .lnk
  first (agent/shared/lnkcheck.h, tests/native/test_lnkcheck.c).
* The resume test cost three metadata calls per file, one of them a round trip
  to the NAS for a time the directory listing had just returned. It now reads
  the destination once and uses the listing's time, asking the source only when
  that disagrees (agent/shared/gsresume.h, tests/native/test_gs_resume_enum.c).
* The sizing pass walked every title's tree on the share, including the ones
  the capability gate then refused; the gate is now asked first. And the
  "already installed" credit walked every installed tree on every sync; it is
  now taken only when it can change the disk verdict.
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


def test_the_resume_test_reads_the_destination_once_and_asks_the_source_rarely():
    b = body(gs(), "gs_copy_file")
    head = b[:b.index("if (verdict == GSR_SKIP)")]
    assert head.count("GetFileAttributesExA(dst") == 1
    assert "gs_file_size(dst)" not in head, "the second destination read is back"
    assert "gsr_decide(" in head
    ask = head.index("if (verdict == GSR_ASK_SOURCE)")
    assert head.index("gs_get_mtime(src") > ask, \
        "the source may be asked ONLY when the listing's time disagrees"


def test_the_tree_walk_hands_over_the_listing_time():
    b = body(gs(), "gs_copy_tree")
    assert "fd.ftLastWriteTime" in b
    assert re.search(r"gs_copy_file\(s, d, sz, &list_ft\)", b)


def test_the_gate_is_asked_before_a_title_is_sized():
    run = body(gs(), "gs_run")
    gate = run.index("gs_gate_allows_title(library")
    size = run.index("sizes[i] = gs_dir_size(src")
    assert gate < size, "a refused title must not be walked on the share"
    between = run[gate:size]
    assert "continue;" in between, "a gated title must skip the sizing walk"
    # ...and the copy loop uses the verdict instead of asking twice
    assert run.count("gs_gate_allows_title(") == 1
    assert "if (gated[i])" in run


def test_the_installed_tree_is_walked_only_when_it_can_change_the_answer():
    run = body(gs(), "gs_run")
    credit = run.index("existing = gs_dir_size(have")
    guard = run.rindex("if (freeb >= 0 && sizes[i] + GS_FREE_MARGIN > freeb)", 0, credit)
    assert credit - guard < 400, "the credit walk must sit inside the no-room test"
    # the verdict itself is re-tested after the credit
    assert run.count("sizes[i] + GS_FREE_MARGIN > freeb") == 2


def test_wallpaper_staging_uses_the_listing_size():
    b = body(gs(), "gs_stage_wallpapers")
    assert "gs_file_size(src)" not in b
    assert "fd.nFileSizeLow" in b
