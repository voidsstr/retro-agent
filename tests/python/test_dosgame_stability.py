"""Regression: DOSGAME v0.2 stability fixes (MS-DOS mode on Win98).

Source-level invariants for defects that are cheap to reintroduce and
expensive to notice — on a real box each of these is a hung machine or a
feature that silently does nothing. The behavioural counterparts run in
DOSBox from scripts/dosgames/tests/run_dos_tests.sh.

Each test names the bug it pins and, where the old form is still expressible,
asserts that the buggy shape is gone rather than merely that the fixed shape
is present.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DG = os.path.abspath(os.path.join(HERE, "..", "..", "scripts", "dosgames"))


def _read(name):
    with open(os.path.join(DG, name), "r", errors="replace") as f:
        return f.read()


def _func(src, signature):
    """Body of a function, from its signature to the first column-0 '}'."""
    i = src.index(signature)
    j = src.index("\n}", i)
    return src[i:j]


# --------------------------------------------------------------- crashes ---

def test_path_join_is_bounded():
    """An unbounded sprintf into an 81-byte buffer. `root` is a token of the
    scan= config line (up to 159 chars), so a long games directory smashed the
    far return address and hung the box at startup, before the UI appeared."""
    src = _read("dosgame.c")
    body = _func(src, "static void path_join_n(")
    assert "cap" in body and "return" in body, "path_join must respect a capacity"
    # the unbounded form must be gone
    assert not re.search(r'sprintf\(out,\s*"%s\\\\%s"', src), \
        "path_join still uses an unbounded sprintf"
    # every caller goes through the capacity-carrying macro
    assert "#define path_join(out, root, leaf) path_join_n(" in src


def test_footer_buffer_fits_the_worst_case_search_string():
    """draw_footer() sprintf'd a 74-char literal plus a filter of up to 23
    characters into char[81]; typing a 6-character search smashed the stack."""
    src = _read("dosgame.c")
    body = _func(src, "static void draw_footer(")
    m = re.search(r"char buf\[(\d+)\];", body)
    assert m, "footer buffer not found"
    size = int(m.group(1))

    fmt = re.search(r'sprintf\(buf,\s*"([^"]*)"', body)
    assert fmt, "footer sprintf not found"
    literal = len(fmt.group(1).replace("%s", ""))

    filt = re.search(r"static char cat_filter\[(\d+)\]", src)
    assert filt, "cat_filter not found"
    worst = literal + int(filt.group(1)) - 1 + 1        # text + filter + NUL

    assert size >= worst, \
        "footer buffer is %d bytes but the worst case needs %d" % (size, worst)


def test_split_initialises_every_field_slot():
    """load_catalog() tests fld[4]/fld[5] on lines that may have only 4
    fields; split() used to leave those slots holding a stale far pointer from
    the previous line (or uninitialised stack on the first)."""
    body = _func(_read("dosgame.c"), "static int split(")
    assert re.search(r"for\s*\(\s*i\s*=\s*0;\s*i\s*<\s*max;\s*i\+\+\s*\)\s*fld\[i\]\s*=\s*NULL;",
                     body), "split() must NULL every slot before filling"


# -------------------------------------------------------- DOS batch rules ---

def test_bat_launchers_are_called_not_chained():
    """Chaining to a .BAT abandons the rest of RUN.BAT (verified in DOSBox:
    the line after a bare .BAT never runs, the line after `call GAME.BAT`
    does), which would silently skip the post-install bookkeeping."""
    src = _read("dosgame.c")
    body = _func(src, "static void emit_run(")
    assert "call " in body and "is_bat(" in body

    launch = _func(src, "static int write_launch(")
    assert 'fprintf(f, "%s\\n", g->exe)' not in launch, \
        "write_launch must emit the launcher via emit_run(), not raw"


def test_no_goto_can_reach_a_label_that_was_never_written():
    """COMMAND.COM aborts a script with "Label not found". write_install()
    used to emit `goto notool` and then return early — before the :notool
    label — whenever no share source was configured."""
    src = _read("dosgame.c")
    body = _func(src, "static int write_install(")

    gotos = set(re.findall(r'goto (\w+)\\n"', body))
    labels = set(re.findall(r'":(\w+)\\n"', body))
    assert gotos <= labels, "unreachable labels: %s" % (gotos - labels)

    # the early "no share source" exit must happen before any goto is EMITTED
    # (match the fprintf'd line, not the prose that explains it)
    early = body.index("return -3")
    first_goto = re.search(r'goto \w+\\n"', body)
    assert first_goto, "no goto emitted at all?"
    assert early < first_goto.start(), \
        "the no-share-source check must precede the first emitted goto"


def test_empty_directory_is_not_probed_with_if_exist_star():
    """`if exist DIR\\*.*` is TRUE for an empty directory ('.' and '..'
    match), so batch cannot tell "unzipped nothing" from "unzipped fine".
    That judgement belongs in the exe, reported via errorlevel."""
    body = _func(_read("dosgame.c"), "static int write_install(")
    assert not re.search(r'if not exist %s\\\\\*\.\*', body), \
        "the empty-directory test must not rely on `if exist DIR\\*.*`"
    assert "if errorlevel 1 goto nogame" in body


def test_dosgame_bat_does_not_loop_on_an_unknown_exit_code():
    """`if errorlevel 43 goto menu` turned any abnormal exit — a DOS abort
    sets a code of its own — into an unattended relaunch loop."""
    bat = _read("DOSGAME.BAT")
    m = re.search(r"if errorlevel 43 goto (\w+)", bat, re.I)
    assert m, "the >=43 guard is missing"
    assert m.group(1).lower() != "menu", \
        "errorlevel >=43 must not re-enter the menu loop"
    # a stale RUN.BAT must not be runnable by a later pass
    assert re.search(r"del C:\\DOSGAME\\RUN\.BAT", bat, re.I)


# ------------------------------------------------------------- real mode ---

def test_build_uses_a_larger_stack_than_the_watcom_default():
    """Watcom's default DOS stack is 2K; the scan call chain plus sprintf came
    close enough to be a hazard, and a blown stack on a real box is a hang."""
    mk = _read("Makefile")
    assert re.search(r"-k(\d+)", mk), "no stack size given to wcl"
    assert int(re.search(r"-k(\d+)", mk).group(1)) >= 8192


def test_keyboard_is_drained_before_handing_over_the_screen():
    """Keys mashed to exit a game sat in the BIOS type-ahead buffer; the
    menu's first getkey() consumed a stale Enter and re-launched the game (or
    started a download nobody asked for)."""
    src = _read("dosgame.c")
    assert "static void kflush(void)" in src
    leave = _func(src, "static void leave_ui(")
    assert "kflush()" in leave
    # ...and on the way in, too
    main_body = src[src.index("int main(int argc"):]
    assert re.search(r"vinit\(\);\s*\n\s*cursor_hide\(\);\s*\n\s*kflush\(\);", main_body)


def test_video_mode_is_normalised_before_drawing_into_video_memory():
    """The menu writes straight to B800:0000 at a fixed 80x25, so inheriting a
    40-column, graphics, or non-zero-page state from the previous program drew
    garbage that looks like a crash."""
    body = _func(_read("dosgame.c"), "static void vinit(")
    assert "0x0003" in body, "vinit must be able to force 80x25 text mode"
    assert "r.h.bh" in body, "vinit must check the active display page"


def test_critical_error_handler_is_installed():
    """A scan root on a drive with no disk pops DOS's "Abort, Retry, Fail?"
    over a TUI that owns the screen and keyboard - unanswerable."""
    src = _read("dosgame.c")
    assert "_harderr(" in src and "_HARDERR_FAIL" in src


# --------------------------------------------------------------- catalog ---

def test_catalog_zip_names_are_not_truncated():
    """game_t.path was 81 bytes; 34 of the 2,982 catalog zip names exceed 80.
    A truncated name hashes to a different stem than the server computes, so
    those titles could never be downloaded."""
    src = _read("dosgame.c")
    m = re.search(r"#define MAX_ZIP_L\s+(\d+)", src)
    assert m, "MAX_ZIP_L not defined"
    longest = 0
    cat = os.path.join(DG, "data", "GAMES.CAT")
    if os.path.exists(cat):
        with open(cat, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                parts = line.rstrip("\n").split("|")
                if len(parts) >= 2:
                    longest = max(longest, len(parts[1]))
    assert int(m.group(1)) >= longest, \
        "MAX_ZIP_L=%s but the catalog holds a %d-char zip name" % (
            m.group(1), longest)
    assert "char path[MAX_ZIP_L + 1];" in src


def test_cd_images_are_refused_rather_than_downloaded():
    """kind 'C' rows are ISO/BIN sets up to 648 MB; fetching one over mTCP
    onto a Win98 box is not an install, it is an accident."""
    body = _func(_read("dosgame.c"), "static int write_install(")
    assert "g->kind == 'C'" in body and "return -2" in body


def test_gamedir_is_always_covered_by_the_scan():
    """gamedir= and scan= were independent, so pointing gamedir somewhere the
    scan did not cover made every install vanish from the menu."""
    body = _func(_read("dosgame.c"), "static void load_cfg(")
    assert "cfg_gamedir" in body and "cfg_scan" in body
    assert "stristr(cfg_scan, cfg_gamedir)" in body
