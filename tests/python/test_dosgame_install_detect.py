"""Regression: DOSGAME install-detection + DOS batch dependency handling.

Encodes the 2026-08-03 fixes:

1. mark_installed() vs write_install() stem mismatch (dosgame.c:zip_stem).
   write_install() creates C:\\GAMES\\<stem8> with spaces/dots replaced by
   '_', but mark_installed() compared the UN-replaced stem — so any catalog
   title with a space in its first 8 chars ("1 To Nil ...") was never shown
   as installed after a successful install. Both sides now share zip_stem().

2. Install receipts (INSTLD.LST) so installer-run (kind 'I') games whose
   INSTALL.EXE picks its own destination directory still get the star.

3. "Bad command or file name" spam: every packet-driver/tool invocation in
   NETUP.BAT and the generated RUN.BAT is guarded with `if exist` —
   COMMAND.COM does not set errorlevel for a missing file, so an unguarded
   missing .COM both printed the error AND was mistaken for a loaded driver.

4. DOSCHAT first-try failure: CHAT.BAT gated on C:\\DOSGAME\\NET\\PKT.OK,
   but nothing ever wrote that file. NETUP.BAT (auto-called by both
   CHAT.BAT and PLAY.BAT) now owns network bring-up and writes PKT.OK.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DG = os.path.abspath(os.path.join(HERE, "..", "..", "scripts", "dosgames"))


def _read(name):
    with open(os.path.join(DG, name), "r", errors="replace") as f:
        return f.read()


# --- the stem transform, mirrored from dosgame.c:zip_stem() ---

def zip_stem(zipname):
    stem = zipname[:12]
    dot = stem.rfind(".")
    if dot >= 0:
        stem = stem[:dot]
    stem = stem[:8]
    return "".join("_" if c in " ." else c for c in stem)


def old_buggy_stem(zipname):
    """pre-fix mark_installed(): no ' '/'.'->'_' replacement"""
    stem = zipname[:12]
    dot = stem.rfind(".")
    if dot >= 0:
        stem = stem[:dot]
    return stem[:8]


def test_stem_matches_install_dir_for_spaced_titles():
    # write_install() creates C:\GAMES\1_To_Nil for this zip; DOS reports the
    # directory back in upper case. The fixed stem matches, the old one can't.
    zipname = "1 To Nil Soccer Manager (1992).zip"
    installed_dir_title = "1_TO_NIL"
    assert zip_stem(zipname).upper() == installed_dir_title
    assert old_buggy_stem(zipname).upper() != installed_dir_title  # the bug


def test_stem_replaces_dots_too():
    # strrchr on the 12-char truncation treats the first '.' as the extension
    # separator, so "Dr. ..." stems to "Dr" — the important invariant is that
    # write_install() and mark_installed() agree (both use zip_stem).
    assert zip_stem("Dr. Riptide (1994).zip") == "Dr"
    assert zip_stem("Alley Cat (1984).zip") == "Alley_Ca"


def test_dosgame_c_shares_one_stem_transform():
    src = _read("dosgame.c")
    assert "static void zip_stem(" in src
    # both consumers go through the shared helper
    mark = src[src.index("static void mark_installed(void)\n{"):]
    mark = mark[:mark.index("\n}")]
    assert "zip_stem(" in mark
    inst = src[src.index("static int write_install("):]
    inst = inst[:inst.index("\n}")]
    assert "zip_stem(" in inst
    # write_install writes the INSTLD.LST receipt, mark_installed reads it
    assert "INSTLD.LST" in mark and "INSTLD.LST" in inst


def test_run_bat_guards_tools_and_fetch():
    src = _read("dosgame.c")
    inst = src[src.index("static int write_install("):]
    inst = inst[:inst.index("\n}")]
    assert "goto notool" in inst   # UNZIP/HTGET presence checked up front
    assert "goto nofetch" in inst  # no zip -> no fake empty install


def test_netup_guards_every_packet_driver():
    netup = _read("NETUP.BAT")
    coms = set(re.findall(r"NET\\(\w+\.COM)", netup))
    for com in coms:
        assert re.search(r"if not exist C:\\DOSGAME\\NET\\%s goto" % re.escape(com),
                         netup, re.I), "unguarded driver: " + com
    assert re.search(r"echo ok\s*>\s*C:\\DOSGAME\\NET\\PKT\.OK", netup, re.I)


def test_chat_and_play_auto_call_netup():
    assert re.search(r"call\s+C:\\DOSGAME\\NETUP\.BAT", _read("CHAT.BAT"), re.I)
    assert re.search(r"call\s+C:\\DOSGAME\\NETUP\.BAT", _read("PLAY.BAT"), re.I)
    # the old dead-end gate ("Network not up yet - run PLAY first") is gone
    assert "Network not up yet" not in _read("CHAT.BAT")
