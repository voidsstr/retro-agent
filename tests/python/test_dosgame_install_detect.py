"""Regression: DOSGAME install-detection + DOS batch dependency handling.

Encodes the 2026-08-03 fixes:

1. mark_installed() vs write_install() stem mismatch (dosgame.c:zip_stem).
   write_install() creates C:\\GAMES\\<stem8> with spaces/dots replaced by
   '_', but mark_installed() compared the UN-replaced stem — so any catalog
   title with a space in its first 8 chars ("1 To Nil ...") was never shown
   as installed after a successful install. Both sides now share zip_stem().

2. Install receipts so installer-run (kind 'I') games whose INSTALL.EXE picks
   its own destination directory still get the star.

   SUPERSEDED 2026-08-11 (v0.2): a receipt could only ever set installed=2,
   which annotates the *catalog* row — it never put the game on the Installed
   tab, so the user could see the star and still had no way to play it. The
   flat INSTLD.LST stem list is replaced by INSTALL.LST, a real registry that
   records the directory the installer actually used and the launcher to run
   there (see the header comment in dosgame.c). The stem-shape tests below
   still apply, and now live alongside test_dosgame_stem.py.

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


# --- the stem transform lives in serve_dosgames.py and is mirrored, byte for
# --- byte, by dosgame.c:zip_stem(). test_dosgame_stem.py pins its contract;
# --- scripts/dosgames/tests/run_dos_tests.sh diffs the two implementations.

def zip_stem(zipname):
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "serve_dosgames", os.path.join(DG, "serve_dosgames.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.zip_stem(zipname)


def old_buggy_stem(zipname):
    """v0.1: first 12 chars, extension stripped, truncated to 8."""
    stem = zipname[:12]
    dot = stem.rfind(".")
    if dot >= 0:
        stem = stem[:dot]
    stem = stem[:8]
    return "".join("_" if c in " ." else c for c in stem)


def test_stem_is_a_legal_dos_directory_name():
    """','  '+' ';' '=' '[' ']' are FAT 8.3 separators, and ',' is also an
    argument separator to COMMAND.COM. v0.1 only replaced ' ' and '.', so
    "Clue, The (1994)...zip" made `mkdir C:\\GAMES\\Clue,_Th` create
    C:\\GAMES\\Clue while UNZIP was handed the full name — the install
    half-landed in the wrong directory and the game never appeared."""
    for zipname in ["Clue, The (1994)(Neo Software).zip",
                    "Scroll, The (1995)(Psygnosis).zip"]:
        stem = zip_stem(zipname)
        for bad in ",+;=[] ":
            assert bad not in stem, (zipname, stem, bad)

    assert "," in old_buggy_stem("Clue, The (1994)(Neo Software).zip")  # the bug


def test_stem_keeps_a_readable_prefix():
    assert zip_stem("Alley Cat (1984).zip").startswith("ALLEY")
    assert zip_stem("DOOM.ZIP").startswith("DOOM_")


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


def test_install_records_a_registry_entry_not_a_bare_receipt():
    """The v0.1 receipt could only set installed=2, which stars the CATALOG
    row; rebuild_view() puts only installed==1 on the Installed tab, so a
    receipt never made anything playable. The registry records the real
    directory and launcher instead."""
    src = _read("dosgame.c")
    assert "INSTALL.LST" in src, "registry file must exist"
    assert "INSTLD.LST" not in src, "the flat receipt list is superseded"

    inst = src[src.index("static int write_install("):]
    inst = inst[:inst.index("\n:end")] if "\n:end" in inst else inst
    # the install script must hand off to the reconciliation pass...
    assert "/snapdirs" in inst and "/postinst" in inst
    # ...and branch on whether it found anything runnable
    assert "if errorlevel 1 goto nogame" in inst

    post = src[src.index("static int post_install(void)"):]
    post = post[:post.index("\n}\n")]
    assert "reg_append('G'" in post, "must record the playable game"
    assert "reg_append('X'" in post, "must retire the spent unpack dir"


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
