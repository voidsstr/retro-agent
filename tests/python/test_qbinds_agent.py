"""QBINDS (agent 1.97.0) - the fleet's WASD layout for Quake 1 and Quake II.

tests/native/test_qbinds.c proves the decisions in agent/shared/qbinds.h - the
bodies, the switch, the engine's own command splitting, and (on the REAL
default.cfg files) that the old staged `unbindall` block lost the weapon keys
while the new FLEETKEY.CFG chain keeps them. Pinned here is what a logic test
cannot see - how agent/src/qbinds.c is wired:

* the startup pass sits in gamesync_thread AFTER the modern-host guard and
  BEFORE the "already provisioned - idle" return. That return is the NORMAL
  path on a fleet box: anything after it runs on almost no machine while
  looking installed (the trap the theme and the screensaver were caught by);
* the per-title hook sits after gameres_apply_title in gs_run, so a title
  deployed by a sync gets its layout in the same sync;
* the agent writes FLEETKEY.CFG and NOTHING ELSE - never autoexec.cfg,
  config.cfg or a pak; the one file it opens for writing is FLEETKEY.TMP, which
  becomes FLEETKEY.CFG by DeleteFile + MoveFile (MoveFileExA does not exist on
  Win9x), and an unusable games folder is refused, never swapped for C:;
* `QBINDS apply` asks the host policy; the report does not need to.

Share-side (skips LOUDLY when the share is not mounted): every QBINDS gamedir
exists in the library, and no title ships FLEETKEY.CFG - a library copy would
make GAMESYNC and the agent fight over the same file at every sync.
"""
import os
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "agent" / "src"
QB = (SRC / "qbinds.c").read_text()
GS = (SRC / "gamesync.c").read_text()
HDR = (ROOT / "agent" / "shared" / "qbinds.h").read_text()
LIBRARY = Path("/mnt/retro-share/Files/Games-Library")


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


def no_strings(text):
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)


# --------------------------------------------------------------------------
# where it runs
# --------------------------------------------------------------------------
def test_the_startup_pass_runs_before_the_provisioned_return():
    t = code_only(body(GS, "gamesync_thread"))
    call = t.index("qbinds_startup()")
    assert t.index("host_manages_this_box") < call, "the modern-host guard comes first"
    assert t.index("gs_place_tool_shortcuts()") < call
    assert call < t.index("gs_file_exists(GS_MARKER)"), (
        "placed after the 'already provisioned - idle' return it would run on almost "
        "no fleet box")
    # unconditional: no if() between the shortcut pass and the call
    seg = t[t.index("gs_place_tool_shortcuts()"):call]
    assert "if" not in re.sub(r"\s", " ", seg).split(), "the startup pass is gated by something"


def test_the_per_title_hook_follows_the_resolution_pass():
    run = code_only(body(GS, "gs_run"))
    sync = run[run.index("if (gs_copy_tree(src, dst)) {"):]
    assert sync.index("gameres_apply_title(dst, titles[i]") < \
        sync.index("qbinds_apply_title(dst, titles[i], &qb_ne)") < \
        sync.index("gs_make_game_shortcut(dst, titles[i])")
    # and it reports, the way the gameres line does - a settled box reads 0
    assert run.index('"gameres: %d title(s)') < run.index('"qbinds: %d bind file(s) written')


def test_status_reports_the_last_run():
    h = body(GS, "handle_gamesync")
    assert '\\"qbinds_written\\":%d,\\"qbinds_not_executed\\":%d,' in h
    assert "s.qb_written, s.qb_notexec," in h
    run = code_only(body(GS, "gs_run"))
    assert "g_gs.qb_written = qb_written;" in run and "g_gs.qb_notexec = qb_notexec;" in run


def test_the_lock_is_made_at_startup_with_the_ledger_lock():
    assert "qbinds_init();" in code_only(body(GS, "gamesync_init"))


# --------------------------------------------------------------------------
# what it touches
# --------------------------------------------------------------------------
def test_the_only_file_opened_for_writing_is_the_temp():
    code = code_only(QB)
    writes = re.findall(r"CreateFileA\(([^,]+),\s*GENERIC_WRITE", code)
    assert writes == ["tmp"], writes
    w = code_only(body(QB, "qb_write"))
    assert '"%s\\\\%s", dir, QB_TMP' in w
    # replace = DeleteFile + MoveFile, then read back
    assert w.index("DeleteFileA(dst)") < w.index("MoveFileA(tmp, dst)") < w.rindex("qb_holds(dst, want, wlen)")
    assert w.index("qb_holds(tmp, want, wlen)") < w.index("DeleteFileA(dst)"), \
        "the temp is read back BEFORE the old file is removed"
    assert "MoveFileExA" not in code, "MoveFileExA does not exist on Win9x"


def test_it_never_writes_a_library_file():
    code = no_strings(code_only(QB))
    for f in ("autoexec.cfg", "config.cfg", "default.cfg", ".pak"):
        assert f not in code
    # autoexec.cfg is named exactly once, as a READ
    assert code_only(QB).count('"%s\\\\autoexec.cfg"') == 1
    d = code_only(body(QB, "qb_do_dir"))
    assert "qb_read(path, ae, sizeof(ae)" in d
    assert "CreateDirectoryA" not in code_only(QB), "a missing gamedir is not created"


def test_no_win9x_unsafe_crt_or_imports():
    code = no_strings(code_only(QB))
    for fn in ("strtok_s", "strtok(", "MoveFileExA", "GetFileAttributesExA", "CopyFileExA"):
        assert fn not in code, fn


def test_a_broken_games_folder_is_refused_never_c():
    code = code_only(QB)
    assert "gs_games_dir_why(" in code
    assert "C:\\\\Games" not in code and "C:\\Games" not in QB.replace("never falls back to C:", "")
    s = code_only(body(QB, "qbinds_startup"))
    assert s.index("gs_games_dir_why(") < s.index("qb_do_title(")
    assert "QUAKE BINDS NOT APPLIED" in s


def test_the_switch_and_the_record():
    assert '#define QB_REG_SWITCH  "QuakeBinds"' in HDR
    assert '#define QB_REG_RESULT  "QuakeBindsBoot"' in HDR
    s = code_only(body(QB, "qbinds_startup"))
    assert "qb_switch(" in s and "qb_store(msg)" in s
    d = code_only(body(QB, "qb_do_dir"))
    assert "mode != QB_MODE_HANDS_OFF" in d, "QuakeBinds=2 must write nothing"


# --------------------------------------------------------------------------
# the command and the policy
# --------------------------------------------------------------------------
def test_the_handler_row_and_the_makefile():
    handlers = (SRC / "handlers.c").read_text()
    assert re.search(r'\{\s*"QBINDS",\s*1,\s*NULL,\s*handle_qbinds,\s*0\s*\}', handlers), \
        "dual-mode like MONPOWER: the flag is 0 and apply guards itself"
    assert "$(SRCDIR)/qbinds.c" in (ROOT / "agent" / "Makefile").read_text()


def test_apply_is_guarded_and_unknown_words_are_refused():
    h = code_only(body(QB, "handle_qbinds"))
    assert "if (apply && !host_manages_this_box())" in h
    assert h.index("host_manages_this_box") < h.index("qb_do_title(")
    assert '_stricmp(w1, "apply")' in h
    assert "qb_title_find(w1)" in h and "is not a title QBINDS manages" in QB
    assert "usage QBINDS [title] | QBINDS apply [title]" in QB


def test_the_startup_pass_asks_the_policy_first():
    s = code_only(body(QB, "qbinds_startup"))
    assert s.index("host_policy_skip") < s.index("qb_switch(")
    t = code_only(body(QB, "qbinds_apply_title"))
    assert "host_manages_this_box()" in t


# --------------------------------------------------------------------------
# the library (share-side)
# --------------------------------------------------------------------------
def _qb_titles():
    rows = re.findall(r'\{\s*"(\w+)",\s*QB_PROFILE_Q\d,\s*\{([^}]*)\}\s*\}', HDR)
    assert rows, "qb_titles[] not found"
    return [(t, re.findall(r'"([^"]+)"', dirs)) for t, dirs in rows]


def _ci_child(parent, name):
    try:
        for e in os.listdir(parent):
            if e.lower() == name.lower():
                return os.path.join(parent, e)
    except OSError:
        return None
    return None


def _library():
    if not LIBRARY.is_dir():
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - the library side of QBINDS "
                    "(gamedirs exist, no FLEETKEY.CFG shipped) was NOT checked" % LIBRARY)
    return LIBRARY


def test_every_qbinds_gamedir_exists_in_the_library():
    lib = _library()
    missing = []
    for title, dirs in _qb_titles():
        t = _ci_child(str(lib), title)
        if not t:
            missing.append(title)
            continue
        for d in dirs:
            if not _ci_child(t, d):
                missing.append("%s/%s" % (title, d))
    assert not missing, "QBINDS names gamedirs the library does not have (case-insensitive): %s" % missing


def test_no_title_ships_fleetkey_cfg():
    """One writer. A FLEETKEY.CFG in the library would be copied by GAMESYNC and
    rewritten by the agent on every sync - the copy fight grledger.h exists to end."""
    lib = _library()
    shipped = []
    for title, _dirs in _qb_titles():
        t = _ci_child(str(lib), title)
        if not t:
            continue
        for root, _d, files in os.walk(t):
            for f in files:
                if f.lower() in ("fleetkey.cfg", "fleetkey.tmp"):
                    shipped.append(os.path.join(root, f))
    assert not shipped, shipped
