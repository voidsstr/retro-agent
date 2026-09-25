"""An idle agent must not wake up for nothing.

WHAT WAS WRONG (agent <= 1.84.x)
--------------------------------
Several helpers slept in 1-second slices "so a QUIT is not held up": the log
mirror (60 s period), the retrowall wallpaper keeper (300 s), the hardware
publish (retry waits up to 900 s, then a 6-hour refresh), and log.c's flusher
woke every 250 ms to wait out a 15 s interval. None of those slices bought
anything, because nothing ever waits for a helper: a QUIT ends the accept loop
and agent_run() calls ExitProcess(), which ends every thread wherever it is.

And on every Win9x box the watchdog woke every 8 s to check g_cmd_inflight,
which only handle_client() - single/threaded mode - ever raises. Multiplex mode,
which is what every 9x box runs, never marks a command in flight, so the
watchdog could not fire there at all.

THE FIX (agent 1.85.0)
----------------------
* agent_nap(ms) (bgwork.c) - one long sleep per 60 s slice, returning g_running.
* the log flusher waits on an event log_shutdown() signals.
* the watchdog is started only where it can work (not in multiplex mode).

These tests pin each change and the premise that makes the long naps safe.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"


def read(name):
    return (SRC / name).read_text(errors="replace")


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


# ------------------------------------------------------------ the premise
def test_agent_run_ends_in_exitprocess_so_no_helper_is_ever_waited_for():
    """Long naps are safe ONLY because the process exits hard. If someone ever
    replaces this with a join on the helpers, every nap becomes a shutdown
    delay - and this test is where they find out."""
    b = code_only(body(read("main.c"), "agent_run"))
    assert "ExitProcess(0)" in b
    # spawn_helper closes the handle at once: there is nothing to join on
    sp = code_only(body(read("main.c"), "spawn_helper"))
    assert "CloseHandle(h)" in sp


def test_agent_nap_sleeps_in_long_slices_and_returns_g_running():
    h = read("bgwork.h")
    m = re.search(r"#define BG_NAP_SLICE_MS\s+(\d+)", h)
    assert m and int(m.group(1)) >= 10000, "a nap slice under 10 s is a poll"
    b = code_only(body(read("bgwork.c"), "agent_nap"))
    assert "g_running" in b and "return g_running" in b
    assert "Sleep(1000)" not in b


# --------------------------------------------------------------- each loop
def test_the_poll_loops_are_gone():
    cases = [
        ("retrowall.c", "retrowall_thread"),
        ("hwpublish.c", "hwpublish_thread"),
        ("main.c", "sharelog_thread"),
    ]
    for path, fn in cases:
        b = code_only(body(read(path), fn))
        assert "Sleep(1000)" not in b, f"{fn} still polls every second"
        assert "agent_nap(" in b, f"{fn} should nap with agent_nap()"


def test_the_log_flusher_waits_on_an_event():
    log = code_only(read("log.c"))
    b = body(log, "log_flush_thread")
    assert "Sleep(250)" not in b, "the 250 ms poll is back: 240 wake-ups a minute"
    assert "WaitForSingleObject(g_flush_evt, LOG_FLUSH_MS)" in b
    # and shutdown must signal it, or the 2 s join would time out every time
    sd = body(log, "log_shutdown")
    assert sd.index("g_flush_stop = 1") < sd.index("SetEvent(g_flush_evt)") \
        < sd.index("WaitForSingleObject(g_flush_thread")
    sb = body(log, "log_set_buffered")
    assert sb.index("CreateEventA(") < sb.index("CreateThread(")


# --------------------------------------------------------------- watchdog
def test_the_watchdog_is_not_started_where_it_cannot_fire():
    main = code_only(read("main.c"))
    spawn = main.index("spawn_helper(watchdog_thread")
    guard = main.rindex("if (g_client_mode != MODE_MULTIPLEX)", 0, spawn)
    assert spawn - guard < 200, "the watchdog spawn must sit inside the mode test"


def test_the_premise_multiplex_really_never_marks_a_command_in_flight():
    """The watchdog decides from g_cmd_inflight. If multiplex mode ever starts
    maintaining it, the watchdog CAN fire there and should be started again -
    so this is the test that has to change first."""
    main = code_only(read("main.c"))
    assert "InterlockedIncrement(&g_cmd_inflight)" in body(main, "handle_client")
    assert "g_cmd_inflight" not in body(main, "client_process"), (
        "multiplex mode now marks commands in flight - start the watchdog "
        "in multiplex mode too (main.c) and update this test")
