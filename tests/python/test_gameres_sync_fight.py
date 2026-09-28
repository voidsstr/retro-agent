"""GAMESYNC and GAMERES must not undo each other - the WIRING.

THE DEFECT (agent 1.90.0, measured on .110 = XP and .243 = Win98 SE,
2026-09-28). gs_run() copies a title, merges its install.reg, then GAMERES
writes THIS box's resolution into the title's own config and registry. The
next sync then:

  * saw a config GAMERES had rewritten as "not the library's file" (the resume
    test is size AND mtime) and copied the library's back - Thief2\\cam.cfg
    800x600 <-> 1024x768, Daggerfall's dosbox conf desktop <-> original,
    Descent1\\DESCENT.CFG on .243 - and GAMERES changed it again;
  * re-merged install.reg, whose constants (CounterStrike16 pins the shared
    GoldSrc key at 800x600; Max Payne, Hidden & Dangerous at 800x600) GAMERES
    then overwrote again.

Five quiet syncs on .110 wrote 22/20/11/11/20 files, GAMERES reported 23 values
changed every run, and every run rebuilt the icon layout. On .243 every run
logged "Descent1 - 2 value(s) set" (the same fight, not the fleetres.cfg
two-writers issue: Descent1 has no fleetres.cfg rule; its library DESCENT.CFG
simply has no ResolutionX/Y lines, so GAMERES appends both every time).

tests/native/test_grledger.c pins the pure logic (agent/shared/grledger.h and
gr_reg_owner in agent/shared/gameres.h). This file pins the wiring in
agent/src/gamesync.c and agent/src/gameres.c that the native test cannot see,
and runs the agent's own registry-owner lookup over every staged install.reg.

Run: pytest tests/python/test_gameres_sync_fight.py
"""
import os
import re
import shutil
import subprocess
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
GAMESYNC = os.path.join(REPO, "agent", "src", "gamesync.c")
GAMERES = os.path.join(REPO, "agent", "src", "gameres.c")
LEDGER_H = os.path.join(REPO, "agent", "shared", "grledger.h")
SHARED = os.path.join(REPO, "agent", "shared")
LIB = "/mnt/retro-share/Files/Games-Library"


def _read(path):
    with open(path, encoding="latin-1") as fh:
        return fh.read()


def _code(src):
    """The source with its comments removed - the prose is allowed to NAME
    the call it forbids."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)


def _func(src, name):
    # a DEFINITION: starts a line with its type (never " * ..." prose that
    # merely mentions the name), and its body closes at column 0
    m = re.search(r"^(?:static )?[A-Za-z_][\w \*]*\b%s\([^;{]*\)\n\{\n.*?\n\}\n" % re.escape(name),
                  src, re.S | re.M)
    assert m, "%s() not found" % name
    return m.group(0)


# ---------------------------------------------------------------------------
# the file half: gs_copy_file consults GAMERES's ledger


def test_copy_consults_the_ledger_only_after_the_resume_test_declined():
    """The ledger can only turn a COPY into a SKIP - never the reverse. So it
    is asked only when the resume test did not already skip, and only for a
    destination that exists."""
    b = _func(_read(GAMESYNC), "gs_copy_file")
    decide = b.index("verdict = gsr_decide(")
    guard = b.index("if (verdict != GSR_SKIP && dst_exists) {")
    look = b.index("gameres_ledger_lookup(dst,")
    assert decide < guard < look
    assert "grl_decide(&le, dst_size, dst_time, (long long)src_size," in b
    assert "grl_decide_source(&le, have_src, src_time)" in b


def test_a_kept_file_is_not_written_and_not_counted_as_written():
    """A ledger skip must return BEFORE the source is opened and before
    gs_desk_note_file() - or 'file(s) written' stays non-zero and the icon
    rebuild comes back on every sync, which is the defect."""
    b = _func(_read(GAMESYNC), "gs_copy_file")
    skip = b.index("if (lv == GRL_SKIP) {")
    block = b[skip:b.index("}", skip)]
    assert "return 1;" in block
    assert "g_gs_gr_kept" in block, "a kept file must be COUNTED - a silent skip is untestable"
    assert skip < b.index("hs = CreateFileA(src, GENERIC_READ")
    assert skip < b.index("gs_desk_note_file();")


def test_a_real_copy_forgets_the_old_record():
    b = _func(_read(GAMESYNC), "gs_copy_file")
    assert "if (gr_entry)\n        gameres_ledger_forget(dst);" in b
    assert b.index("gameres_ledger_forget(dst);") > b.index("gs_desk_note_file();")


def test_a_replaced_adjusted_file_says_why():
    """The rare case - a library update, an edit on the box - is exactly the
    one that explains a non-zero 'file(s) written' on a quiet box."""
    b = _func(_read(GAMESYNC), "gs_copy_file")
    assert "since GAMERES adjusted it - taking the" in b
    assert "the library's copy changed" in b and "the file was changed on this box" in b


def test_the_run_saves_the_ledger_and_reports_what_it_kept():
    run = _func(_read(GAMESYNC), "gs_run")
    loop_end = run.index("g_gs.state = g_gs_abort ? GS_FAILED : GS_DONE;")
    assert run.index("gameres_apply_title(dst, titles[i]") < run.index("gameres_ledger_save();") < loop_end, \
        "the ledger must reach disk before the next sync looks"
    assert "InterlockedExchange((LONG *)&g_gs_gr_kept, 0);" in run, "the count is per run"
    assert "adjusted file(s) kept (library copy unchanged)" in run
    assert "%d value(s) changed" in run, "the settled-box signal stays on the line"


def test_status_reports_the_resolution_pass():
    src = _read(GAMESYNC)
    assert '\\"gameres_changed\\":%d,\\"gameres_kept\\":%ld,' in src
    assert "s.gr_changed, s.gr_kept," in src


def test_the_ledger_lock_is_made_at_startup():
    init = _func(_read(GAMESYNC), "gamesync_init")
    assert "gameres_init();" in init


# ---------------------------------------------------------------------------
# the file half: GAMERES records what it rewrote


def test_gameres_records_before_and_after_each_write():
    b = _func(_read(GAMERES), "gameres_apply_title")
    pre = b.index("have_pre = gr_stat(path, &pre_size, &pre_time);")
    assert pre < b.index("switch (r->op) {"), "the 'before' is read BEFORE the writer"
    note = b.index("gr_ledger_note(path, pre_size, pre_time);")
    assert "if (rc > 0 && have_pre)" in b[note - 80:note], \
        "record only a real change to a file the library ships"
    # the registry rows never reach the file-ledger code
    reg = b.index("if (r->op == GR_OP_REG) {")
    assert reg < pre and "continue;" in b[reg:pre]


def test_win9x_profile_cache_is_flushed_before_the_after_is_read():
    """Win9x caches WritePrivateProfileString; without the flush the ledger
    would record the OLD file's size and time and the next sync would copy."""
    b = _func(_read(GAMERES), "gr_w_ini")
    w = b.index("WritePrivateProfileStringA(sec, key, val, file)")
    f = b.index("WritePrivateProfileStringA(NULL, NULL, NULL, file);")
    assert w < f < b.index("return 1;", w)


def test_ledger_file_is_per_box_bounded_and_win9x_safe():
    src = _read(GAMERES)
    assert '#define GR_LEDGER_PATH "C:\\\\RETRO_AGENT\\\\GRLEDGER.TXT"' in src
    save = _func(src, "gameres_ledger_save")
    assert "grl_format(&g_grl, buf, GRL_FILE_MAX + 1)" in save
    assert "DeleteFileA(GR_LEDGER_PATH);" in save and "MoveFileA(GR_LEDGER_TMP, GR_LEDGER_PATH)" in save
    assert "MoveFileEx" not in _code(src), "MoveFileExA does not exist on Win9x"
    assert "grl_remove(&g_grl, i);" in save, "records for vanished files are pruned"
    load = _func(src, "gr_ledger_load_locked")
    assert "size > GRL_FILE_MAX" in load, "an oversized file is not read at all"
    assert "grl_parse(&g_grl, buf, got, &bad)" in load
    # a damaged ledger is said out loud, and costs only a copy
    assert "is damaged" in load and "dropped" in load


def test_gameres_apply_command_saves_too():
    """GAMERES APPLY rewrites files outside a sync - the next sync would copy
    them all back if the command did not persist what it did."""
    h = _func(_read(GAMERES), "handle_gameres")
    apply_at = h.index('str_starts_with(a, "APPLY")')
    save_at = h.index("gameres_ledger_save();")
    assert apply_at < save_at < h.index("gameres_probe();\n\n    /*")


def test_ledger_shares_the_resume_tests_time_slack():
    h = _code(_read(LEDGER_H))
    assert '#include "gsresume.h"' in h
    assert "gsr_same_time(" in h
    assert "%lld" not in h and "printf" not in h, "Win98's msvcrt has no %lld"


# ---------------------------------------------------------------------------
# the registry half


def test_owned_values_are_captured_before_regedit_and_put_back_after():
    b = _func(_read(GAMESYNC), "gs_merge_reg")
    load = b.index("text = gs_reg_load(reg_path, &len);")
    snap = b.index("nkeep = gs_reg_keep_snapshot(text, len, keep, GS_REG_KEEP_MAX);")
    run = b.index("ran = gs_run_regedit(reg_path, is_9x, why, sizeof(why));")
    restore = b.index("restored = gs_reg_keep_restore(keep, nkeep);")
    verify = b.index("gs_reg_verify(text, len, 0, &t, NULL, NULL);")
    assert load < snap < run < restore < verify
    assert "per-box value(s) left to GAMERES" in b


def test_verify_neither_judges_nor_writes_an_owned_value():
    """The 9x fallback writes what regedit did not land; it must not write
    install.reg's constant over a value GAMERES owns."""
    v = _func(_read(GAMESYNC), "gs_reg_verify")
    own = v.index("if (gs_reg_entry_owned(e)) {")
    assert own < v.index("ok = gs_reg_entry_holds(e);") < v.index("gs_reg_entry_apply(e, &err)")
    assert "t->kept++;" in v[own:own + 120]


def test_ownership_needs_the_owning_title_installed():
    """HalfLife1's install.reg names the key CounterStrike16's rule owns; on a
    box without Counter-Strike nothing else would ever set it."""
    o = _func(_read(GAMESYNC), "gs_reg_entry_owned")
    assert "gameres_reg_owner(rm_root_name(e->root), e->key, e->name)" in o
    assert "GS_DEST, owner" in o and "gs_file_exists(dir)" in o
    assert "e->op != RM_OP_SET" in o


# ---------------------------------------------------------------------------
# the real lookup over the real library

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "regmerge.h"
#include "gameres.h"

int main(int argc, char **argv)
{
    static char buf[1 << 20];
    FILE *f = fopen(argv[1], "rb");
    size_t n;
    rm_parser_t *ps = malloc(sizeof(*ps));
    rm_entry_t *e = malloc(sizeof(*e));
    (void)argc;
    if (!f || !ps || !e)
        return 2;
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    rm_init(ps, buf, n);
    while (rm_next(ps, e)) {
        const char *o;
        if (e->op != RM_OP_SET)
            continue;
        o = gr_reg_owner(rm_root_name(e->root), e->key, e->name);
        printf("%s\t%s\t%s\t%s\n", rm_root_name(e->root), e->key, e->name, o ? o : "-");
    }
    return 0;
}
"""


def _cc():
    for cand in (os.environ.get("CC"), "gcc", "cc", "clang"):
        if cand and shutil.which(cand):
            return cand
    return None


@pytest.fixture(scope="module")
def owner_tool():
    cc = _cc()
    assert cc, "no host C compiler - the lookup the agent runs would go untested"
    d = tempfile.mkdtemp(prefix="growner-")
    src = os.path.join(d, "driver.c")
    with open(src, "w") as fh:
        fh.write(DRIVER)
    exe = os.path.join(d, "driver")
    subprocess.run([cc, "-std=c11", "-O0", "-Wall", "-I", SHARED, src, "-o", exe, "-lm"],
                   check=True, capture_output=True)
    return exe


@pytest.mark.skipif(not os.path.isdir(LIB),
                    reason="LOUD SKIP: %s is not mounted, so the agent's registry-"
                           "owner lookup was NOT checked against the staged library" % LIB)
def test_every_install_reg_value_that_fought_is_recognised(owner_tool):
    """The values measured fighting on .110 must all be recognised as owned by
    the agent's own parser + lookup, from the real staged files - a key the
    parser spells differently from the rule would silently keep the fight."""
    owned = {}
    for t in sorted(os.listdir(LIB)):
        if t.startswith("_"):
            continue
        p = os.path.join(LIB, t, "install.reg")
        if not os.path.isfile(p):
            continue
        out = subprocess.run([owner_tool, p], capture_output=True, text=True,
                             encoding="latin-1", check=True).stdout
        for line in out.splitlines():
            root, key, name, owner = line.split("\t")
            if owner != "-":
                owned.setdefault(t, set()).add(name.lower())
    for title, names in (("CounterStrike16", {"screenwidth", "screenheight",
                                               "enginemodew", "enginemodeh"}),
                         ("HalfLife1", {"screenwidth", "screenheight"}),
                         ("MaxPayne", {"display width", "display height"}),
                         ("HiddenAndDangerous", {"display width", "display height"})):
        assert names <= owned.get(title, set()), \
            "%s: %s not recognised as GAMERES's - the fight comes back" % (title, sorted(names))
