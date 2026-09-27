"""Regression: agent batched logging + the Win9x reboot fix (v1.26.0).

Source invariants, because both changes are about what happens on paths that
are hard to exercise in a test: the process dying, and the machine going down.
Each test names the behaviour it protects and, where the old form is still
expressible, asserts the buggy shape is gone rather than just that the fixed
shape is present.

Context for anyone reading this later:

  * (2026-09-26, agent 1.85.4) The 9x half of what follows was WRONG in the
    end: the one reboot proven on Win98 hardware was the shell's
    SHExitWindowsEx with the agent exiting normally mid-shutdown, and every
    "stay alive and pump" REBOOT failed. The 9x path is now the shell's call
    and nothing else; see the tests below. The NT half stands.

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

def test_win9x_power_goes_through_the_shell_and_nothing_else():
    """The only reboot proven on Win98 hardware (.243, 2026-09-26) was
    `rundll32.exe shell32.dll,SHExitWindowsEx 6` with the agent and retro_chat
    running and nothing killed. Every REBOOT through the agent's own path -
    kill the consoles, then stay alive pumping (g_power_pending) - left
    Windows up with the agent gone. The 9x path must be exactly the proven
    one: the shell's call, no kills, no g_power_pending, no exit of its own."""
    src = read("handlers.c")
    body = func_body(src, "static void do_system_power_9x(")
    assert 'shell32.dll,SHExitWindowsEx %u' in body
    assert "CREATE_SUSPENDED" in body, "OK and the log line must go out first"
    assert body.index("log_flush()") < body.index("ResumeThread(pi.hThread)")
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    for banned in ("kill_console_processes", "TerminateProcess", "g_power_pending", "g_running"):
        assert banned not in code, "%s is back on the Win9x power path" % banned

    disp = func_body(src, "static void do_system_power(")
    first = disp.index("if (is_win9x()) {")
    assert first < disp.index("kill_console_processes()"), "9x must dispatch before any kill"
    assert first < disp.index("g_power_pending = 1"), "9x must never set g_power_pending"


def test_win9x_reboot_that_did_not_take_says_so():
    body = func_body(read("handlers.c"), "static DWORD WINAPI win9x_power_watch(")
    assert "WIN9X_SHUTDOWN_WAIT_MS" in body and "STILL RUNNING" in body, \
        "a reboot that did not take must say so rather than look like success"
    assert "log_flush()" in body
    assert "g_9x_power_started = 0" in body, "a failed attempt must allow another"


def test_nt_power_path_is_unchanged():
    """NT still hands off to shutdown.exe and stops the agent."""
    src = read("handlers.c")
    disp = func_body(src, "static void do_system_power(")
    assert "g_power_pending = 1" in disp and "g_running = 0" in disp
    thr = func_body(src, "static DWORD WINAPI system_shutdown_thread(")
    assert "shutdown.exe /r /t 0 /f" in thr and "acquire_shutdown_privilege()" in thr


def test_reboot_flushes_the_log_before_the_machine_goes_down():
    body = func_body(read("handlers.c"), "static void do_system_power(")
    assert "log_flush()" in body, \
        "the record that a reboot was requested must be on disk before it starts"


def test_consoles_are_killed_on_nt_only_and_the_chat_client_never():
    """On Win98 a console app's console is a DOS VM (WINOA386.MOD, parented to
    the app). TerminateProcess on the app does not close that VM cleanly, and a
    DOS VM still running is what Win98 refuses to shut down over. So the kill
    list is NT-only, and our own chat client is never on it - it closes itself
    when Windows asks, as it did in the reboot that worked."""
    body = func_body(read("handlers.c"), "static void kill_console_processes(")
    for exe in ("COMMAND.COM", "CMD.EXE"):
        assert exe in body
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S).upper()
    assert "RETRO_CHAT" not in code and "WINOA386" not in code


def test_reboot_never_kills_the_console_host_it_runs_in():
    """On Win9x CONAGENT.EXE hosts the console of every Win32 console app,
    this agent's included. Killing it before ExitWindowsEx killed the agent
    itself, so a remote REBOOT on .243 logged "REBOOT: initiating" and then
    nothing: the machine stayed up and the agent was gone (2026-09-25/26)."""
    body = func_body(read("handlers.c"), "static void kill_console_processes(")
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    assert "CONAGENT" not in code.upper() and "WINOA386" not in code.upper()


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
    """NT: while shutdown.exe runs, a logoff/shutdown event must not stop the
    agent early (g_power_pending). Win9x never sets g_power_pending (1.85.4),
    so there the handler stops the agent when Windows asks - which is exactly
    what happened in the one Win98 reboot proven on hardware."""
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


def test_batching_does_not_depend_on_a_thread():
    """CreateThread genuinely fails on this box (main.c logs exactly that for
    dosstage). An earlier version responded by falling back to unbuffered -
    which put the per-line platter write straight back, on the one machine
    batching was written for, and the operator heard the drive clicking per
    line. Losing the thread must NOT stop batching; log_msg ages the buffer
    out itself."""
    log_c = read("log.c")

    msg = func_body(log_c, "void log_msg(")
    assert "GetTickCount() - g_last_flush" in msg, \
        "log_msg must age the buffer out without relying on the flusher"
    assert "LOG_FLUSH_MS" in msg

    setb = func_body(log_c, "void log_set_buffered(")
    tail = setb[setb.index("if (!g_flush_thread)"):]
    assert "g_buffered = 0" not in tail, \
        "a missing flusher thread must not turn batching off"


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


def test_log_location_needs_no_argument():
    """Starting the agent by hand must not require remembering -l to get a
    log, and every fleet tool should find one in the same place on every box.
    The default resolves to a single known path, with fallbacks."""
    log_c = read("log.c")
    assert '#define AGENT_LOG_FILE' in log_c, "no fixed default log path"
    assert 'AGENT_LOG_DIR "\\\\agent.log"' in log_c or \
           'AGENT_LOG_DIR "\\agent.log"' in log_c, \
        "the default file must live under the known directory"

    body = func_body(log_c, "static void default_log_path(")
    assert "CreateDirectoryA(AGENT_LOG_DIR" in body, \
        "a fresh box has no C:\\RETRO_AGENT yet"
    assert "path_writable" in body, \
        "must fall back rather than log nowhere if the path is not writable"
    assert "GetModuleFileNameA" in body, "exe-dir fallback must remain"

    # -l must still win
    main = read("main.c")
    assert "g_logfile[0] ? g_logfile : NULL" in main, \
        "an explicit -l must still override the default"


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
