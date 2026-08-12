"""Regression: agent batched logging + the Win9x reboot fix (v1.26.0).

Source invariants, because both changes are about what happens on paths that
are hard to exercise in a test: the process dying, and the machine going down.
Each test names the behaviour it protects and, where the old form is still
expressible, asserts the buggy shape is gone rather than just that the fixed
shape is present.

Context for anyone reading this later:

  * REBOOT returned OK on the Win98 box, the agent closed, and Windows never
    restarted. do_system_power() set g_running = 0 shortly after starting the
    shutdown thread, so the process died while Win9x was still negotiating
    WM_QUERYENDSESSION with every top-level window - which cancels the
    shutdown. The agent had killed itself instead of the machine.

  * Logging used to WriteFile + FlushFileBuffers every single line: a seek and
    a platter write per entry on a 24/7 machine with a 1997 IDE drive. Lines
    now batch, WITHOUT giving up the property that justified the raw-Win32
    logger in the first place - that a silent startup crash still leaves its
    last line on disk.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", "..", "agent", "src"))


def read(name):
    with open(os.path.join(SRC, name), "r", errors="replace") as f:
        return f.read()


def func_body(src, signature):
    """Body from a function signature to the first column-0 closing brace."""
    i = src.index(signature)
    j = src.index("\n}", i)
    return src[i:j]


# ------------------------------------------------------------ reboot -------

def test_win9x_reboot_does_not_kill_the_agent_first():
    """The bug: the agent exited mid-negotiation and cancelled its own
    shutdown. g_running must only be cleared on the NT path."""
    body = func_body(read("handlers.c"), "static void do_system_power(")

    assert "g_running = 0" in body, "NT still needs to stop the agent"
    # the clear must be inside a platform test, not unconditional
    nt_guard = body.index("VER_PLATFORM_WIN32_NT")
    clear = body.index("g_running = 0")
    assert nt_guard < clear, \
        "g_running = 0 must be reached only on the NT branch"

    # and the old unconditional "Sleep(500); g_running = 0;" tail is gone
    assert not re.search(r"CreateThread\([^;]*system_shutdown_thread[^;]*;\s*"
                         r"/\*[^*]*\*/\s*Sleep\(500\);\s*g_running = 0;",
                         body, re.S), "the unconditional exit is back"


def test_win9x_shutdown_thread_keeps_pumping_and_reports_failure():
    body = func_body(read("handlers.c"), "static DWORD WINAPI system_shutdown_thread(")
    assert "PeekMessageA" in body, "must pump messages for the negotiation"
    assert "WIN9X_SHUTDOWN_WAIT_MS" in body
    assert "STILL RUNNING" in body, \
        "a reboot that did not take must say so rather than look like success"
    # wraparound-safe tick comparison, not "GetTickCount() < deadline"
    assert re.search(r"\(long\)\(GetTickCount\(\) - deadline\) < 0", body), \
        "tick comparison must be wraparound-safe"


def test_reboot_flushes_the_log_before_the_machine_goes_down():
    body = func_body(read("handlers.c"), "static void do_system_power(")
    assert "log_flush()" in body, \
        "the record that a reboot was requested must be on disk before it starts"


def test_console_apps_that_can_veto_are_killed():
    """On Win9x every top-level window gets a WM_QUERYENDSESSION vote, and our
    own chat client is a console app too."""
    body = func_body(read("handlers.c"), "static void kill_console_processes(")
    for exe in ("COMMAND.COM", "CMD.EXE", "RETRO_CHAT.EXE"):
        assert exe in body, "%s can hold up a Win9x shutdown" % exe


# ----------------------------------------------------------- logging -------

def test_startup_is_unbuffered_then_switches():
    """The window where a silent crash must be reconstructable from the log is
    startup, so batching may not be on from the beginning - it is turned on
    later, by a timer thread, never inline in the startup path."""
    main = read("main.c")
    assert "log_set_buffered(1)" in main, "batching must be enabled somewhere"

    # the only enable site is inside the delayed thread
    body = func_body(main, "static DWORD WINAPI delayed_buffering_thread(")
    assert "log_set_buffered(1)" in body
    assert main.count("log_set_buffered(1)") == 1, \
        "batching must not also be switched on inline during startup"


def test_crash_path_emits_pending_lines_without_touching_shared_state():
    """log_crash runs LOCK-FREE (the dead thread may hold the lock), so it must
    not mutate g_buf/g_buf_len the way a flush would: a lock-holding thread can
    legitimately be appending right then, and two writers each doing
    SetFilePointer(FILE_END)+WriteFile on one handle can interleave and lose
    the crash record itself. Snapshot, concatenate, write once."""
    body = func_body(read("log.c"), "void log_crash(")

    assert "EnterCriticalSection" not in body, \
        "the crashing thread may hold the lock - taking it could deadlock"
    # match the CALL, not the word in the explanatory comment above it
    assert not re.search(r"^\s*flush_locked\(\);", body, re.M), \
        "flush_locked() writes g_buf_len; the lock-free path must not"
    assert "memcpy" in body and "g_buf" in body, \
        "pending lines must be snapshotted so the crash context survives"
    assert body.count("disk_out(") <= 2, \
        "the snapshot and the crash line should go out as one write"


def test_console_handler_does_not_cancel_our_own_reboot():
    """The trap: CTRL_LOGOFF/CTRL_SHUTDOWN are exactly the events Win9x raises
    while tearing the session down - including the teardown we asked for. If
    the handler stops the agent then, it re-arms the reboot-cancellation bug
    from a second direction. It must flush but NOT exit during a power op."""
    main = read("main.c")
    body = func_body(main, "static BOOL WINAPI console_handler(")
    assert "g_power_pending" in body, \
        "the handler must know a power operation is in flight"
    flush = body.index("log_flush()")
    guard = body.index("if (!g_power_pending)")
    stop = body.index("g_running = 0")
    assert flush < guard < stop, \
        "flush unconditionally, then guard the exit on g_power_pending"

    handlers = read("handlers.c")
    assert "g_power_pending = 1" in handlers, \
        "do_system_power must announce the pending power operation"


def test_flusher_thread_failure_falls_back_to_unbuffered():
    """CreateThread genuinely fails on the 31MB Deskpro. With no flusher and
    g_buffered left on, nothing drains the buffer and lines sit in RAM."""
    body = func_body(read("log.c"), "void log_set_buffered(")
    assert "if (!g_flush_thread)" in body, "CreateThread result must be checked"
    tail = body[body.index("if (!g_flush_thread)"):]
    assert "g_buffered = 0" in tail, "must fall back to unbuffered"


def test_buffering_starts_after_the_work_that_actually_crashes_this_box():
    """"Helper threads spawned" is not the end of the risky window - dosstage
    copying an 11MB payload ~45s in has taken this agent down outright."""
    main = read("main.c")
    m = re.search(r"#define LOG_BUFFER_AFTER_MS\s+(\d+)", main)
    assert m, "buffering must be delayed, not enabled inline"
    assert int(m.group(1)) >= 120000, \
        "delay must clear the dosstage window (~45s) with margin"


def test_clean_exit_marker_is_written_before_the_file_is_closed():
    """log_shutdown() invalidates the handle; a line logged after it reaches
    the console but never the file, so a clean QUIT would read like a kill."""
    main = read("main.c")
    marker = main.index('"shutdown complete; exiting process"')
    close = main.index("log_shutdown();")
    assert marker < close, "the exit marker must precede log_shutdown()"
    assert main.count('"shutdown complete; exiting process"') == 1, \
        "the marker must not be logged twice"


def test_every_process_ending_path_flushes():
    main = read("main.c")

    handler = func_body(main, "static BOOL WINAPI console_handler(")
    assert "log_flush()" in handler, \
        "closing the console window is how this agent usually dies on Win9x"
    for evt in ("CTRL_CLOSE_EVENT", "CTRL_LOGOFF_EVENT", "CTRL_SHUTDOWN_EVENT"):
        assert evt in handler, "%s must also flush" % evt

    assert "log_shutdown()" in main, "the clean-exit path must flush and close"


def test_buffer_full_and_oversized_lines_are_not_dropped():
    body = func_body(read("log.c"), "static void raw_out(")
    assert "flush_locked()" in body, "a full buffer must be written, not dropped"
    assert "disk_out(s, len)" in body, \
        "a line larger than the buffer must still reach the disk"


def test_flush_interval_is_bounded():
    """Batching trades a window of routine logging against disk wear; that
    window has to be small enough to state."""
    log_c = read("log.c")
    m = re.search(r"#define LOG_FLUSH_MS\s+(\d+)", log_c)
    assert m, "no flush interval defined"
    assert int(m.group(1)) <= 30000, "flush interval longer than 30s"

    b = re.search(r"#define LOG_BUF_BYTES\s+(\d+)", log_c)
    assert b, "no buffer size defined"
    assert int(b.group(1)) <= 65536, "log buffer is large for a 31MB machine"


def test_per_line_platter_flush_is_gone_from_the_hot_path():
    """The change is only worth anything if the per-line FlushFileBuffers is
    actually off the buffered path."""
    log_c = read("log.c")
    raw = func_body(log_c, "static void raw_out(")
    assert "FlushFileBuffers" not in raw, \
        "raw_out must not force a platter write per line any more"
    disk = func_body(log_c, "static int disk_out(")
    assert "FlushFileBuffers" in disk, \
        "the batch write must still commit to disk"
