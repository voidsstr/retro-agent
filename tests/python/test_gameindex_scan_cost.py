"""GAMEINDEX costs a Pentium almost nothing when nothing changed (agent 1.85.0).

Measured before this change: a pass took 6-10 s on average and up to 65 s, and
ran every 240 s forever - ~68 GetFileAttributesA probes per directory to depth
3, C:\\Games walked twice, every shortcut resolved through COM, at the
process's HIGH priority.

tests/native/test_gimatch.c proves the matcher gives the old probes' answers
and the pass decision. This pins how gameindex.c is wired:

* one listing per directory, fed to gim_note() - no per-signature probe loop;
* a game root met as a child of another walk is not descended twice;
* the thread serves the cached index, then checks a fingerprint every 15 min,
  runs a pass only on a reason, and sleeps on an event GAMESYNC can set;
* GAMEINDEX SCAN is serialised with the background pass and lifts the IDLE
  scanner before waiting for it;
* the answers scripts/gameindex/sync.py consumes keep their shape.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"
GI = SRC / "gameindex.c"


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


def gi():
    return code_only(GI.read_text(errors="replace"))


def define(name):
    m = re.search(r"#define\s+" + name + r"\s+(\d+)", GI.read_text(errors="replace"))
    assert m, name
    return int(m.group(1))


def test_one_listing_per_directory_no_probe_per_signature():
    s = gi()
    d = body(s, "gi_scan_dir")
    assert d.count("FindFirstFileA(") == 1
    assert "gim_note(&hits, &g_gi_tab, fd.cFileName, fd.cAlternateFileName" in d, \
        "match both names a path probe would have matched"
    for fn in ("gi_scan_dir", "gi_emit", "match_dir"):
        b = body(s, fn)
        assert "file_exists(" not in b and "dir_exists(" not in b, (
            f"{fn} probes the filesystem per signature again")
    assert "static void walk(" not in s, "the old double-listing walk is back"


def test_no_game_root_is_walked_twice():
    s = gi()
    d = body(s, "gi_scan_dir")
    assert "gi_is_own_root(child)" in d
    sd = body(s, "scan_drives")
    fill = sd.index("g_gi_roots[g_gi_nroots++]")
    first_walk = sd.index("gi_scan_dir(root, GI_MAX_DEPTH - 2")
    assert fill < first_walk, "the roots must be known before the root walk"


def test_the_cadence():
    assert define("GI_PERIOD_MS_DEF") >= 15 * 60 * 1000, "check period >= 15 min"
    assert define("GI_FULL_EVERY_MS") == 60 * 60 * 1000, "hourly safety net"
    assert define("GI_FIRST_DELAY_MS") == 120000
    assert define("GI_FIRST_DELAY_NOCACHE_MS") < define("GI_FIRST_DELAY_MS")
    assert define("GI_PERIOD_MS_MIN") == 60000, "the registry override floor is unchanged"


def test_the_thread_checks_before_it_scans():
    t = body(gi(), "gameindex_thread")
    assert t.index("thread_background()") < t.index("gi_cache_load()") \
        < t.index("Sleep(")
    loop = t[t.index("while (g_running)"):]
    assert loop.index("gi_fingerprint()") < loop.index("gi_scan_reason(") \
        < loop.index("gi_scan_locked(")
    assert "if (reason != GI_SCAN_SKIP)" in loop
    assert "WaitForSingleObject(g_gi_wake, gi_period_ms())" in loop
    assert loop.index("EnterCriticalSection(&g_gi_scan_lock)") \
        < loop.index("gi_scan_locked(") \
        < loop.index("LeaveCriticalSection(&g_gi_scan_lock)")


def test_scan_is_serialised_and_lifts_the_idle_scanner():
    h = body(gi(), "handle_gameindex")
    scan = h[h.index('str_starts_with(a, "SCAN")'):]
    assert scan.index("SetThreadPriority(g_gi_thread, THREAD_PRIORITY_NORMAL)") \
        < scan.index("EnterCriticalSection(&g_gi_scan_lock)") \
        < scan.index("gi_scan_locked(") \
        < scan.index("LeaveCriticalSection(&g_gi_scan_lock)")
    assert "gi_scan()" not in gi(), "an unserialised pass is back"


def test_the_fingerprint_is_taken_before_the_walk():
    b = body(gi(), "gi_scan_locked")
    assert b.index("gi_fingerprint()") < b.index("scan_drives()")
    assert "gi_cache_save(fp, hash, doc)" in b


def test_gamesync_wakes_the_index_after_a_run_that_changed_something():
    gs = code_only((SRC / "gamesync.c").read_text(errors="replace"))
    run = body(gs, "gs_run")
    assert re.search(r"if \(gs_desk_changed\(\)\)\s*gameindex_poke\(\);", run)


def test_the_answers_sync_py_reads_keep_their_shape():
    s = gi()
    h = body(s, "handle_gameindex")
    assert '{\\"pending\\":true,\\"hash\\":\\"\\",\\"games\\":[]}' in h
    assert 'str_starts_with(a, "HASH")' in h
    j = body(s, "build_json")
    for key in ('"hash"', '"count"', '"scan_ms"', '"games"', '"key"', '"dir"',
                '"exe"', '"launcher"', '"engine"'):
        assert key in j, key
    sync = (REPO / "scripts" / "gameindex" / "sync.py").read_text()
    assert '"GAMEINDEX HASH"' in sync and 'doc.get("pending")' in sync
