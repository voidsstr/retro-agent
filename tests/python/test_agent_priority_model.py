"""The agent's thread-priority model: commands stay HIGH, helpers go IDLE.

WHAT WAS WRONG (agent <= 1.84.x)
--------------------------------
main.c puts the whole process in HIGH_PRIORITY_CLASS so that a fullscreen game
cannot starve the thread that answers commands - a real outage, measured as
30-60 s of silence during a benchmark. But a Windows thread takes its base
priority from its PROCESS class, so every background helper ran at base 13 as
well: the game-index disk walk, the share log mirror, the theme pass, the
library sync, the self-update, the hardware publish and the DOS staging all ran
above Explorer (8) and above the game the box exists to play. hwpublish and
dosstage "lowered" themselves with THREAD_PRIORITY_BELOW_NORMAL, which inside
a HIGH class is 12 - still four levels above the game.

THE MODEL (agent 1.85.0, bgwork.h)
----------------------------------
* The process class stays HIGH, so every command-serving thread is exactly as
  responsive as before - by construction, not by each thread remembering to
  raise itself (a thread that forgot would silently fall to 8).
* Each background helper calls thread_background() first, which sets
  THREAD_PRIORITY_IDLE - base 1, the only level below a normal-class game
  inside a HIGH-class process.
* The locks a command waits on are protected against an IDLE holder being
  starved: the log lock lifts its holder (it is held across disk I/O), and
  GAMEINDEX SCAN lifts the scanner before waiting for it.

This file pins all three, in both directions.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"


def read(name):
    return (SRC / name).read_text(errors="replace")


def body(src, name):
    """Text of `name`'s body, by brace matching from its definition."""
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
    raise AssertionError(f"unbalanced braces in {name}")


def code_only(text):
    """Strip C comments, so an explanatory comment cannot satisfy a check."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# ---------------------------------------------------------------- arithmetic
# Windows base priority = process class base + thread relative level, with
# IDLE and TIME_CRITICAL special-cased (1 and 15 outside the realtime class).
CLASS_BASE = {"IDLE": 4, "BELOW_NORMAL": 6, "NORMAL": 8, "ABOVE_NORMAL": 10,
              "HIGH": 13}
THREAD_REL = {"IDLE": None, "LOWEST": -2, "BELOW_NORMAL": -1, "NORMAL": 0,
              "ABOVE_NORMAL": 1, "HIGHEST": 2, "TIME_CRITICAL": None}


def base_priority(cls, thread):
    if thread == "IDLE":
        return 1
    if thread == "TIME_CRITICAL":
        return 15
    return CLASS_BASE[cls] + THREAD_REL[thread]


def test_only_idle_puts_a_high_class_thread_below_a_normal_game():
    game = base_priority("NORMAL", "NORMAL")
    assert game == 8
    # the old helper levels - all ABOVE the game
    assert base_priority("HIGH", "NORMAL") == 13 > game
    assert base_priority("HIGH", "BELOW_NORMAL") == 12 > game, \
        "hwpublish/dosstage's old 'below normal' was still above the game"
    assert base_priority("HIGH", "LOWEST") == 11 > game, \
        "LOWEST is not low enough inside a HIGH class"
    # the fixed level
    assert base_priority("HIGH", "IDLE") == 1 < game
    # and the command threads keep what they had
    assert base_priority("HIGH", "NORMAL") == 13


# ------------------------------------------------------------- the helpers
def test_thread_background_is_idle_not_lowest():
    b = code_only(body(read("bgwork.c"), "thread_background"))
    assert "THREAD_PRIORITY_IDLE" in b
    assert "THREAD_PRIORITY_LOWEST" not in b and "BELOW_NORMAL" not in b, (
        "LOWEST/BELOW_NORMAL are 11/12 inside a HIGH class - above a game")
    assert "GetLastError" in b, "a failure to lower must be logged, not hidden"


def test_the_process_class_stays_high():
    """The command paths get their responsiveness from the CLASS. Dropping it
    to NORMAL would put every command thread at 8, level with the game."""
    main = code_only(read("main.c"))
    assert "SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)" in main


BACKGROUND = [
    ("gameindex.c", "gameindex_thread"),
    ("gamesync.c", "gamesync_thread"),
    ("gamesync.c", "gs_worker"),          # started by the thread OR a command
    ("retrowall.c", "retrowall_thread"),
    ("autoupdate.c", "autoupdate_thread"),
    ("hwpublish.c", "hwpublish_thread"),
    ("dosstage.c", "dosstage_thread"),
    ("dosstage.c", "dosstage_run_thread"),  # the DOSSTAGE command's worker
    ("main.c", "sharelog_thread"),
]


def test_every_background_helper_lowers_itself_before_it_works():
    for path, fn in BACKGROUND:
        b = code_only(body(read(path), fn))
        assert "thread_background()" in b, f"{fn} ({path}) runs at base 13"
        at = b.index("thread_background()")
        # before any sleep or work: the first Sleep, and anything that walks
        # a disk or a share, must come after the drop
        for later in ("Sleep(", "gs_run(", "dosstage_run(", "gi_scan("):
            if later in b:
                assert at < b.index(later), f"{fn}: {later} runs before the drop"


def test_no_old_below_normal_left_on_a_helper():
    for path in ("hwpublish.c", "dosstage.c"):
        assert "THREAD_PRIORITY_BELOW_NORMAL" not in code_only(read(path)), (
            f"{path} still uses BELOW_NORMAL (12 in a HIGH class)")


COMMAND_SERVING = [
    ("main.c", "handle_client"),
    ("main.c", "client_thread"),
    ("main.c", "discovery_thread"),
    ("watchdog.c", "watchdog_thread"),
    ("log.c", "log_flush_thread"),      # holds the log lock while it writes
]


def test_command_serving_threads_are_never_lowered():
    for path, fn in COMMAND_SERVING:
        b = code_only(body(read(path), fn))
        assert "thread_background" not in b and "THREAD_PRIORITY_IDLE" not in b, (
            f"{fn} ({path}) serves commands or holds a lock commands need; "
            f"lowering it makes the box unreachable while a game runs")


# ------------------------------------------------------ priority inversion
def test_the_log_lock_lifts_an_idle_holder():
    """g_log_cs is held across WriteFile + FlushFileBuffers. An IDLE helper
    preempted inside it by a busy game would keep the command thread's next
    log line waiting for as long as the game keeps the CPU."""
    log = code_only(read("log.c"))
    lift = body(log, "log_lift")
    assert "GetThreadPriority" in lift and "THREAD_PRIORITY_IDLE" in lift
    assert "THREAD_PRIORITY_NORMAL" in lift
    for fn in ("log_msg", "log_flush"):
        b = code_only(body(log, fn))
        assert b.index("log_lift()") < b.index("EnterCriticalSection(&g_log_cs)"), fn
        assert b.index("LeaveCriticalSection(&g_log_cs)") < b.index("log_unlift("), fn
    # the crash logger must stay lock-free and call-free
    assert "log_lift" not in code_only(body(log, "log_crash"))
