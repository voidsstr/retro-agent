"""GAMESYNC must never empty the desktop before it can rebuild it (agent 1.90.0).

THE DEFECT (.110, XP P4, 2026-09-28)
------------------------------------
``gs_run()`` began with ``gs_sweep_desktop()``: every .lnk/.pif/.url on both
desktops moved into ``C:\\retro-desktop-backup``, and each game's icon came
back only when the copy loop reached that title. A run started at 11:17 then
sat in ``state=sizing``, ``message: "enumerating library"``, ``titles_total 0``
for ~100 minutes - a minimized ioquake3 held 96% of the one CPU and the worker
runs at ``THREAD_PRIORITY_IDLE`` by design - with two icons on the desktop
where 97 had been, and nothing saying why. The same window opened on every
run that failed before the copy loop (library unreachable: retried every two
minutes, each retry sweeping again), was aborted, or died with the agent.
Evidence: ``.claude/evidence-icons/192.168.1.110/``.

THE FIX
-------
* The pre-run sample (``gs_desk_snapshot``) is still taken before any shortcut
  is written - the icon-rebuild gate depends on that.
* Nothing is swept at the start. Shortcuts are rewritten IN PLACE, and
  ``gs_sweep_unclaimed()`` runs after the copy loop, removing only entries of
  the pre-run sample that the run neither rewrote nor confirmed - and only when
  ``ds_run_may_sweep()`` says the run considered every title.
* A stall is measured and reported: ``gs_beat()`` at every progress point,
  ``GAMESYNC STATUS`` carries ``since_progress_s``/``stalled_s``/``starved_s``,
  and a starved run says so in its message and its log.

The decision logic is compiled for real by ``tests/native/test_desk_sweep_order.c``;
what a unit test cannot see is WHERE in ``gs_run()`` things happen, which is
what broke - so that is what this file pins.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"
GAMESYNC = SRC / "gamesync.c"


def _code() -> str:
    src = GAMESYNC.read_text(errors="replace")
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def _body(code: str, signature: str) -> str:
    """The body of the DEFINITION - a forward declaration (`...);`) is skipped."""
    start = code.index(signature)
    while True:
        brace, semi = code.find("{", start), code.find(";", start)
        if brace != -1 and (semi == -1 or brace < semi):
            break
        start = code.index(signature, start + 1)
    i = brace
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[i:j + 1]
    raise AssertionError(signature)


def _run() -> str:
    return _body(_code(), "static void gs_run(const char *library)")


def test_nothing_is_swept_before_the_copy_loop_has_rebuilt_the_icons():
    run = _run()
    assert run.count("gs_sweep_unclaimed()") == 1, \
        "exactly one sweep, at the end of the run"
    sweep = run.index("gs_sweep_unclaimed()")
    for later_than in ("gs_desk_snapshot()",
                       "gs_place_tool_shortcuts()",
                       "FindFirstFileA(pat",                 # enumeration
                       "sizes[i] = gs_dir_size(src",         # sizing
                       "gs_make_game_shortcut(dst, titles[i])",
                       "gs_restore_shortcuts_if_installed(titles[i])"):
        assert run.index(later_than) < sweep, (
            "the desktop sweep runs before %s - that is the window in which "
            ".110 sat with two icons for 100 minutes" % later_than)


def test_the_sweep_first_function_is_gone():
    code = _code()
    assert "gs_sweep_desktop" not in code, (
        "gs_sweep_desktop() moved every icon off the desktop at the START of a "
        "run; it must not come back under its old name")
    run = _run()
    head = run[:run.index("FindFirstFileA(pat")]
    for mover in ("MoveFileA", "DeleteFileA"):
        assert mover not in head, (
            "gs_run() touches desktop files (%s) before it has even listed the "
            "library" % mover)


def test_every_early_return_leaves_the_desktop_alone():
    """The unreachable-library path is retried every two minutes; the empty
    library path writes the marker. Neither may have swept anything."""
    run = _run()
    sweep = run.index("gs_sweep_unclaimed()")
    returns = [m.start() for m in re.finditer(r"\breturn\s*;", run)]
    assert len(returns) >= 2, "the unreachable and empty-library returns moved"
    for r in returns:
        assert r < sweep, "a return after the sweep - check this is intended"
    unreachable = run.index("cannot reach library")
    assert any(r > unreachable for r in returns), \
        "the unreachable-library path must still return early"


def test_only_a_run_that_considered_every_title_sweeps():
    run = _run()
    m = re.search(r"if\s*\(\s*ds_run_may_sweep\(\s*g_gs_abort\s*!=\s*0\s*,"
                  r"\s*listing_complete\s*,\s*titles_failed\s*\)\s*\)\s*gs_sweep_unclaimed\(\)", run)
    assert m, ("the sweep must be guarded by ds_run_may_sweep(abort, "
               "listing_complete, titles_failed): an aborted or truncated run never "
               "looked at some titles, and a failed title never re-asserted its "
               "shortcuts - sweeping would take their icons (1.97.1, W98BUILD)")
    # every "finished with errors" is counted
    fe = run.index('"%s finished with errors"')
    assert "titles_failed++" in run[fe:fe + 200]
    lc = re.search(r"listing_complete\s*=\s*!capped\s*&&\s*\(\s*enum_err\s*==\s*0"
                   r"\s*\|\|\s*enum_err\s*==\s*ERROR_NO_MORE_FILES\s*\)", run)
    assert lc, "listing_complete must come from BOTH the cap and FindNextFile's error"
    assert run.index("enum_err = GetLastError()") < lc.start()
    assert "desktop NOT swept" in run, "a skipped sweep must say so in the log"


def test_the_sweep_removes_only_what_was_sampled_and_not_claimed():
    code = _code()
    swp = _body(code, "static void gs_sweep_unclaimed(void)")
    assert "FindFirstFileA" not in swp, (
        "the sweep must walk the PRE-RUN SAMPLE, not re-list the desktop - a "
        "re-list would also take icons that appeared during the run")
    assert "ds_sweep_bits(" in swp and "ds_mark_gone(" in swp
    assert "GS_DESK_BACKUP" in swp, "moved to the backup folder, never deleted outright first"
    assert re.search(r"MoveFileA\(src,\s*dst\)", swp)


def test_the_gate_is_settled_after_the_sweep():
    run = _run()
    assert run.index("gs_sweep_unclaimed()") < run.index("gs_desk_settle_lnks()") \
        < run.index("gs_desk_changed()")


def test_every_shortcut_the_run_keeps_is_claimed():
    code = _code()
    tool = _body(code, "static void gs_tool_shortcut(const char *exe, const char *name, const char *icon)")
    check = tool.index("if (gs_lnk_points_at(lnk, exe) && (!icon || gs_lnk_has_icon(lnk, icon)))")
    early = tool[check:tool.index("return;", check)]
    assert "gs_desk_note_lnk_kept(lnk)" in early, (
        "an already-correct Retro Agent/Retro Chat icon is not rewritten, so it "
        "must be CLAIMED or the end-of-run sweep moves it away")
    line = _body(code, "static void gs_shortcut_from_line(")
    assert "gs_desk_note_lnk_written(lnk)" in line
    written = _body(code, "static void gs_desk_note_lnk_written(const char *lnk_path)")
    kept = _body(code, "static void gs_desk_note_lnk_kept(const char *lnk_path)")
    assert re.search(r"ds_claim\([^;]*,\s*1\)", written), "a write claims with written=1"
    assert re.search(r"ds_claim\([^;]*,\s*0\)", kept), "a keep claims with written=0"
    # ...and both claim the file the shell REALLY wrote (agent 1.93.1): on
    # Windows 9x a shortcut to a .bat or DOS .exe is saved as <name>.pif, and
    # claiming <name>.lnk let the sweep take every game icon off .243.
    for body in (written, kept):
        assert body.index("gs_desk_written_path(lnk_path") < body.index("ds_claim(")


def test_a_rewrite_in_place_clears_the_bits_that_would_block_it():
    """The old sweep cleared attributes before moving a file; the in-place
    rewrite must do the same or a read-only/hidden .lnk is never refreshed and
    then swept away."""
    mk = _body(_code(), "static int gs_make_shortcut(")
    save = mk.index("->Save(")
    head = mk[:save]
    for bit in ("FILE_ATTRIBUTE_READONLY", "FILE_ATTRIBUTE_HIDDEN",
                "FILE_ATTRIBUTE_SYSTEM"):
        assert bit in head, bit
    # ...on the file the shell will really overwrite: the .pif, for a DOS
    # target on Windows 9x (agent 1.93.1).
    assert re.search(r"(\w+)\s*=\s*gs_desk_written_path\(lnk_path,", head), head
    f = re.search(r"(\w+)\s*=\s*gs_desk_written_path\(lnk_path,", head).group(1)
    assert "GetFileAttributesA(%s)" % f in head
    assert "SetFileAttributesA(%s, FILE_ATTRIBUTE_NORMAL)" % f in head


def test_the_snapshot_keeps_whole_names():
    code = _code()
    assert "GS_LNK_NAME" not in code, (
        "the 96-byte GS_LNK_NAME snapshot truncated long names; now that the "
        "snapshot decides what the sweep removes, a truncated name would sweep "
        "the run's own freshly written icon")
    hdr = (REPO / "agent" / "shared" / "deskset.h").read_text()
    assert re.search(r"#define\s+DS_NAME\s+260", hdr)


# --- a starved run must SAY so ------------------------------------------------

def test_progress_points_beat():
    code = _code()
    for fn in ("static __int64 gs_dir_size(const char *dir, int *files)",
               "static void gs_note_progress2(__int64 added, __int64 transferred)",
               "static void gs_set_msg(const char *fmt, ...)"):
        assert "gs_beat()" in _body(code, fn), fn
    run = _run()
    assert re.search(r'gs_set_msg\("sizing %s \(%d of %d\)"', run), (
        "the sizing walk must name the title it is on - 'enumerating library' "
        "for 100 minutes said nothing about where the run was")
    assert run.index('gs_set_msg("enumerating library")') < run.index("FindFirstFileA(pat")


def test_status_reports_progress_and_starvation():
    code = _code()
    h = _body(code, "void handle_gamesync(SOCKET sock, const char *args)")
    for field in ("since_progress_s", "stalled_s", "starved_s", "cpu_busy_pct"):
        assert '\\"%s\\"' % field in h, field
    assert "gsst_verdict(" in h and "gs_stall_describe(" in h, (
        "the status message must carry the stall explanation")
    assert "gsst_note_gap(&st" in h, (
        "the gap still OPEN must be judged at status time - a worker starved "
        "right now cannot close one")
    desc = _body(code, "static void gs_stall_describe(")
    assert "STARVED OF CPU" in desc and "idle priority" in desc


def test_cpu_times_are_resolved_dynamically():
    """GetSystemTimes is XP SP1+: a static import stops the agent loading on
    every Win9x box (and on 2000). tests/python/test_agent_win9x_imports.py
    checks the built binary; this names the file."""
    code = _code()
    assert not re.search(r"(?<![\w.>])GetSystemTimes\s*\(", code)
    assert "ntdyn_GetSystemTimes(" in code
    nt = (SRC / "ntdyn.c").read_text()
    assert 'GetProcAddress(k, "GetSystemTimes")' in nt


# --- the agent's own desktop code, compiled against a fake file system ---------
#
# The functions below are cut out of gamesync.c VERBATIM and run on the host,
# the way test_gamesync_listing_cut_short.py exercises gs_copy_tree. They are
# what decides, on a real box, which files leave the desktop - including the
# path arithmetic (which desktop a .lnk sits on) no source grep can check.

import shutil
import subprocess

import pytest

_DESK_FUNCS = (
    "static const char *gs_basename(const char *p)",
    "static void gs_desk_reset(void)",
    "static void gs_desk_scan_dir(const char *desk, unsigned where)",
    "static void gs_desk_snapshot(void)",
    "static unsigned gs_desk_where(const char *lnk_path)",
    "static const char *gs_desk_written_path(const char *lnk_path, char *buf, size_t cap)",
    "static void gs_desk_note_lnk_written(const char *lnk_path)",
    "static void gs_desk_note_lnk_kept(const char *lnk_path)",
    "static void gs_desk_settle_lnks(void)",
    "static void gs_sweep_unclaimed(void)",
)


def _extract_def(src: str, signature: str) -> str:
    start = src.index(signature)
    while True:
        brace, semi = src.find("{", start), src.find(";", start)
        if brace != -1 and (semi == -1 or brace < semi):
            break
        start = src.index(signature, start + 1)
    depth, i = 0, brace
    while True:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[start:i + 1]
        i += 1


_HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
typedef unsigned long DWORD; typedef void *HANDLE; typedef int BOOL;
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
typedef struct { DWORD dwFileAttributes; char cFileName[MAX_PATH]; } WIN32_FIND_DATAA;
#define LOG_GS "GS"
#define _snprintf snprintf
#include "%(deskset)s"
%(backup)s
static int lstrlenA(const char *s) { return (int)strlen(s); }
static int lstrcmpiA(const char *a, const char *b) { return strcasecmp(a, b); }
static char *lstrcpynA(char *d, const char *s, int n)   /* Win32: n-1 chars + NUL */
{ int k = 0; if (n <= 0) return d; while (k < n - 1 && s[k]) { d[k] = s[k]; k++; } d[k] = 0; return d; }
static void log_msg(const char *t, const char *f, ...)
{ va_list ap; (void)t; va_start(ap, f); printf("LOG "); vprintf(f, ap); printf("\n"); va_end(ap); }

/* ---- a fake file system: full paths ---- */
static char g_fs[64][MAX_PATH]; static int g_nfs;
static int fs_find(const char *p) { int k; for (k = 0; k < g_nfs; k++) if (!strcasecmp(g_fs[k], p)) return k; return -1; }
static void fs_add(const char *p) { if (fs_find(p) < 0) snprintf(g_fs[g_nfs++], MAX_PATH, "%%s", p); }
static int gs_file_exists(const char *p) { return fs_find(p) >= 0; }
static BOOL DeleteFileA(const char *p) { int k = fs_find(p); if (k < 0) return 0; strcpy(g_fs[k], g_fs[--g_nfs]); return 1; }
static BOOL MoveFileA(const char *s, const char *d) { int k = fs_find(s); if (k < 0 || fs_find(d) >= 0) return 0; snprintf(g_fs[k], MAX_PATH, "%%s", d); return 1; }
static BOOL CreateDirectoryA(const char *p, void *sa) { (void)p; (void)sa; return 1; }
static BOOL SetFileAttributesA(const char *p, DWORD a) { (void)p; (void)a; return 1; }
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
static DWORD GetFileAttributesA(const char *p) { return fs_find(p) >= 0 ? FILE_ATTRIBUTE_NORMAL : INVALID_FILE_ATTRIBUTES; }
typedef struct { char dir[MAX_PATH]; int pos; } find_t;
static int next_in(find_t *f, WIN32_FIND_DATAA *fd) {
    size_t n = strlen(f->dir);
    while (f->pos < g_nfs) {
        const char *p = g_fs[f->pos++];
        if (!strncasecmp(p, f->dir, n) && p[n] == '\\' && !strchr(p + n + 1, '\\')) {
            memset(fd, 0, sizeof(*fd)); snprintf(fd->cFileName, MAX_PATH, "%%s", p + n + 1); return 1;
        }
    }
    return 0;
}
static HANDLE FindFirstFileA(const char *pat, WIN32_FIND_DATAA *fd) {
    find_t *f = calloc(1, sizeof(*f)); char *star;
    snprintf(f->dir, MAX_PATH, "%%s", pat); star = strrchr(f->dir, '\\'); if (star) *star = 0;
    if (!next_in(f, fd)) { free(f); return INVALID_HANDLE_VALUE; }
    return f;
}
static BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { return next_in(h, fd); }
static BOOL FindClose(HANDLE h) { free(h); return 1; }

static const char *g_common = "C:\\Documents and Settings\\All Users\\Desktop";
static const char *g_user   = "C:\\Documents and Settings\\User\\Desktop";
static int gs_desktop_dir(char *out, DWORD cch) { snprintf(out, cch, "%%s", g_common); return 1; }
static int gs_user_desktop_dir(char *out, DWORD cch) { snprintf(out, cch, "%%s", g_user); return 1; }

static long     g_gs_desk_files, g_gs_desk_lnks;
static ds_set_t g_gs_dset;
static char     g_gs_desk_common[MAX_PATH];
static char     g_gs_desk_user[MAX_PATH];

%(funcs)s

static void put(const char *desk, const char *name) { char p[MAX_PATH]; snprintf(p, sizeof(p), "%%s\\%%s", desk, name); fs_add(p); }
static void dump(const char *tag) { int k; for (k = 0; k < g_nfs; k++) printf("%%s %%s\n", tag, g_fs[k]); }

/* Windows 98 (.243, agent 1.93.0): one desktop, C:\\WINDOWS\\Desktop, and
 * the shell saves a shortcut to a .bat or DOS .exe as <name>.pif whatever
 * name it was handed. */
static const char *g_9x_games[] = {
    "Quake - 3dfx Voodoo", "Quake - DOS", "Hexen II - 3dfx Voodoo",
    "Quake II", "Quake II - 3dfx Voodoo", "Unreal Tournament - 3dfx Voodoo",
};
static void shell9x_save(const char *desk, const char *name, int dos)
{   /* IPersistFile::Save("<desk>\\<name>.lnk") as Win98's shell does it */
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%%s\\%%s.%%s", desk, name, dos ? "pif" : "lnk");
    fs_add(p);
}
static int main_9x(void) {
    char p[MAX_PATH];
    int k;
    g_common = g_user = "C:\\WINDOWS\\Desktop";
    put(g_common, "Retro Agent.lnk"); put(g_common, "Retro Chat.lnk");
    for (k = 0; k < 6; k++) { snprintf(p, sizeof(p), "%%s.pif", g_9x_games[k]); put(g_common, p); }
    put(g_common, "Removed DOS Game.pif");
    gs_desk_reset();
    gs_desk_snapshot();
    printf("SAMPLED %%d\n", g_gs_dset.n);
    snprintf(p, sizeof(p), "%%s\\Retro Agent.lnk", g_common); gs_desk_note_lnk_kept(p);
    snprintf(p, sizeof(p), "%%s\\Retro Chat.lnk", g_common); gs_desk_note_lnk_kept(p);
    for (k = 0; k < 6; k++) {            /* rewritten in place, as .pif */
        shell9x_save(g_common, g_9x_games[k], 1);
        snprintf(p, sizeof(p), "%%s\\%%s.lnk", g_common, g_9x_games[k]);
        gs_desk_note_lnk_written(p);
    }
    shell9x_save(g_common, "Falcon 3.0", 1);            /* a new DOS title */
    snprintf(p, sizeof(p), "%%s\\Falcon 3.0.lnk", g_common); gs_desk_note_lnk_written(p);
    shell9x_save(g_common, "Unreal Tournament", 0);     /* a new Win32 .exe */
    snprintf(p, sizeof(p), "%%s\\Unreal Tournament.lnk", g_common); gs_desk_note_lnk_written(p);
    printf("NEW %%ld\n", g_gs_desk_lnks);
    gs_sweep_unclaimed();
    gs_desk_settle_lnks();
    dump("FINAL");
    printf("CHANGED %%ld\n", g_gs_desk_lnks);
    return 0;
}

int main(int argc, char **argv) {
    char p[MAX_PATH];
    long written_new = 0;
    if (argc > 1 && !strcmp(argv[1], "9x"))
        return main_9x();
    put(g_common, "Quake III Arena.lnk"); put(g_common, "Removed Title.lnk");
    put(g_common, "Retro Agent.lnk");     put(g_common, "notes.txt");
    put(g_user, "quake iii arena.lnk");   put(g_user, "Vendor Offer.url");
    put(g_user, "Kept By User Path.lnk");

    gs_desk_reset();
    gs_desk_snapshot();
    printf("SAMPLED %%d\n", g_gs_dset.n);
    /* the tool icon is already right: kept, not rewritten */
    snprintf(p, sizeof(p), "%%s\\Retro Agent.lnk", g_common); gs_desk_note_lnk_kept(p);
    /* a game rewritten in place */
    snprintf(p, sizeof(p), "%%s\\Quake III Arena.lnk", g_common); gs_desk_note_lnk_written(p);
    /* a brand-new title */
    snprintf(p, sizeof(p), "%%s\\Far Cry.lnk", g_common); fs_add(p); gs_desk_note_lnk_written(p);
    written_new = g_gs_desk_lnks;
    /* something claimed on the USER desktop stays there */
    snprintf(p, sizeof(p), "%%s\\Kept By User Path.lnk", g_user); gs_desk_note_lnk_kept(p);
    dump("MIDRUN");
    gs_sweep_unclaimed();
    gs_desk_settle_lnks();
    dump("FINAL");
    printf("NEW %%ld CHANGED %%ld\n", written_new, g_gs_desk_lnks);
    return 0;
}
'''


@pytest.fixture(scope="module")
def desk_bin(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - the desktop sweep was NOT exercised")
    src = GAMESYNC.read_text(errors="replace")
    backup = re.search(r"#define GS_DESK_BACKUP\s+\"[^\"]*\"", src).group(0)
    funcs = "\n\n".join(_extract_def(src, sig) for sig in _DESK_FUNCS)
    d = tmp_path_factory.mktemp("desk")
    (d / "t.c").write_text(_HARNESS % {
        "deskset": str(REPO / "agent" / "shared" / "deskset.h"),
        "backup": backup, "funcs": funcs})
    subprocess.run([cc, "-w", "-o", str(d / "t"), str(d / "t.c")], check=True)
    return str(d / "t")


@pytest.fixture(scope="module")
def desk_run(desk_bin):
    return subprocess.run([desk_bin], capture_output=True, text=True,
                          check=True).stdout


@pytest.fixture(scope="module")
def desk_run_9x(desk_bin):
    return subprocess.run([desk_bin, "9x"], capture_output=True, text=True,
                          check=True).stdout


def _files(out, tag):
    return {line.split(" ", 1)[1] for line in out.splitlines()
            if line.startswith(tag + " ")}


def test_mid_run_every_icon_is_still_on_the_desktop(desk_run):
    mid = _files(desk_run, "MIDRUN")
    assert not any("retro-desktop-backup" in f for f in mid), (
        "something reached the backup folder before the run ended")
    for name in ("Quake III Arena.lnk", "Removed Title.lnk", "Retro Agent.lnk",
                 "quake iii arena.lnk", "Vendor Offer.url", "Far Cry.lnk"):
        assert any(f.endswith("\\" + name) for f in mid), name


def test_the_real_sweep_takes_exactly_the_unclaimed(desk_run):
    final = _files(desk_run, "FINAL")
    common = "C:\\Documents and Settings\\All Users\\Desktop\\"
    user = "C:\\Documents and Settings\\User\\Desktop\\"
    backup = "C:\\retro-desktop-backup\\"
    # Compared CASE-INSENSITIVELY, as Windows paths are: the sweep names the
    # moved file by the sampled name, which for the per-user duplicate is the
    # All Users spelling - the same file to Windows.
    final = {f.lower() for f in final}
    assert final == {x.lower() for x in {
        common + "Quake III Arena.lnk",      # rewritten in place
        common + "Retro Agent.lnk",          # kept as it was
        common + "Far Cry.lnk",              # new this run
        common + "notes.txt",                # not a shortcut - never touched
        user + "Kept By User Path.lnk",      # claimed on the USER desktop
        backup + "Removed Title.lnk",        # no longer staged
        backup + "quake iii arena.lnk",      # the per-user duplicate
        backup + "Vendor Offer.url",         # clutter
    }}, sorted(final)


def test_the_real_gate_counts_the_net_change(desk_run):
    m = re.search(r"NEW (\d+) CHANGED (\d+)", desk_run)
    assert m, desk_run
    assert int(m.group(1)) == 1, "only Far Cry was new while the run wrote"
    # Far Cry added; Removed Title and Vendor Offer gone; the Quake duplicate
    # went but the name is still on the desktop, so it is not a change.
    assert int(m.group(2)) == 3, desk_run
    assert "desktop swept: 3 shortcut(s)" in desk_run


# --- Windows 9x: the shell saves a DOS shortcut as <name>.pif (agent 1.93.1) ---

def test_win9x_dos_shortcuts_rewritten_in_place_stay_on_the_desktop(desk_run_9x):
    """.243, agent 1.93.0: "desktop swept: 6 shortcut(s) this run did not put
    back" - every game launcher is a .bat, the shell wrote <name>.pif, the run
    claimed <name>.lnk, and the sweep moved all six game icons away."""
    final = {f.lower() for f in _files(desk_run_9x, "FINAL")}
    desk = "c:\\windows\\desktop\\"
    backup = "c:\\retro-desktop-backup\\"
    for name in ("Quake - 3dfx Voodoo", "Quake - DOS", "Hexen II - 3dfx Voodoo",
                 "Quake II", "Quake II - 3dfx Voodoo",
                 "Unreal Tournament - 3dfx Voodoo", "Falcon 3.0"):
        assert desk + name.lower() + ".pif" in final, (name, sorted(final))
    assert desk + "unreal tournament.lnk" in final, "a Win32 target is a real .lnk"
    assert desk + "retro agent.lnk" in final and desk + "retro chat.lnk" in final
    assert {f for f in final if f.startswith(backup)} == {
        backup + "removed dos game.pif"}, "only what the run did not put back"
    assert not any(f.endswith(".lnk") and "3dfx voodoo" in f for f in final), (
        "no phantom .lnk is invented for a shortcut the shell saved as a .pif")


def test_win9x_gate_counts_only_real_additions_and_removals(desk_run_9x):
    new = int(re.search(r"NEW (\d+)", desk_run_9x).group(1))
    changed = int(re.search(r"CHANGED (\d+)", desk_run_9x).group(1))
    assert new == 2, "Falcon 3.0 (.pif) and Unreal Tournament (.lnk) are new"
    assert changed == 3, "two added, one stale .pif removed - not 12"
    assert "desktop swept: 1 shortcut(s)" in desk_run_9x, desk_run_9x
