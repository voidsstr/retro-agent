"""Regression: agent resource hygiene (v1.27.0).

All of this came from one operator report on the Win98 box:

    "the agent was still up and it took a while to kill it, and when it was
     killed i was not able to overwrite the retro_agent.exe file ... so
     something was still held by the previous run of the agent ... ive seen
     you not be able to connect even though the agent is running or has been
     closed but not fully releasing OS resources"

Three distinct defects sat behind that:

  * NOTHING STOPPED TWO AGENTS RUNNING AT ONCE. Each start logs
    "Listening on TCP :9898+:9897" but the second only gets whichever port the
    first did not take - so the fleet's port answers nothing while an agent is
    plainly running. And killing "the" agent leaves retro_agent.exe locked by
    the copy still alive, which is exactly the overwrite failure reported.
  * CreateThread's handle was discarded everywhere, including once PER
    CONNECTION in threaded mode - a leaked kernel object for every client that
    ever connects, on an agent that runs for weeks.
  * Client slots were held until the peer disconnected, which a half-open TCP
    connection never does. Ten of those and the agent is unreachable while
    looking perfectly healthy.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", "..", "agent", "src"))


def read(name):
    with open(os.path.join(SRC, name), "r", errors="replace") as f:
        return f.read()


def func_body(src, signature):
    i = src.index(signature)
    j = src.index("\n}", i)
    return src[i:j]


# --------------------------------------------------------- single instance --

def test_second_instance_refuses_to_start():
    """Two agents racing for the ports is the root of both 'cannot overwrite
    the exe' and 'cannot connect though it is running'."""
    main = read("main.c")
    assert "CreateMutexA" in main, "no single-instance guard"
    assert "ERROR_ALREADY_EXISTS" in main, \
        "must detect that another instance owns the mutex"

    # the guard has to run before the agent does anything, or it guards nothing
    guard = main.index("ERROR_ALREADY_EXISTS")
    run = main.rindex("agent_run();")
    assert guard < run, "the instance check must precede agent_run()"


def test_instance_mutex_is_released_on_shutdown():
    """A replacement should be able to start the instant we are gone."""
    main = read("main.c")
    assert "ReleaseMutex(g_instance_mutex)" in main
    # released before the hard exit, not after it
    rel = main.index("ReleaseMutex(g_instance_mutex)")
    ex = main.index("ExitProcess(0)")
    assert rel < ex, "release the mutex before the process is torn down"


# ------------------------------------------------------------ handle leaks --

def test_per_connection_thread_handle_is_not_leaked():
    """Threaded mode starts a thread per connection; discarding the handle
    leaks a kernel object for every client the agent ever serves."""
    main = read("main.c")
    i = main.index("CreateThread(NULL, 0, client_thread")
    region = main[i:i + 300]
    assert "CloseHandle(" in region, \
        "the client thread handle must be closed immediately after creation"
    # the result must be captured, not thrown away
    assert re.search(r"HANDLE \w+ = CreateThread\(NULL, 0, client_thread",
                     main), "client thread handle is still being discarded"


def test_helper_threads_do_not_leak_their_handles():
    main = read("main.c")
    assert "static int spawn_helper(" in main, \
        "helper threads should go through one non-leaking spawner"
    body = func_body(main, "static int spawn_helper(")
    assert "CloseHandle(h)" in body

    # No CreateThread whose result is DISCARDED (statement starts with the
    # call). Assigning it is fine - those handles are closed explicitly.
    leaks = re.findall(r"^\s*CreateThread\(", main, re.M)
    assert not leaks, "%d CreateThread call(s) still discard the handle" % len(leaks)


# ------------------------------------------------------------ connections ---

def test_idle_client_slots_are_reaped():
    """A half-open connection never reports itself closed, so without a
    timeout its slot is held for good and the agent runs out of slots."""
    main = read("main.c")
    m = re.search(r"#define CLIENT_IDLE_MS\s+(\d+)", main)
    assert m, "no idle timeout for client slots"
    idle = int(m.group(1))

    # must outlast the longest legitimate silence: a long-poll
    assert idle >= 60000, "idle timeout shorter than a long-poll would allow"
    assert idle <= 900000, "idle timeout so long a wedged slot outlives the day"

    assert "last_active" in main, "slots must track when they last spoke"
    assert re.search(r"GetTickCount\(\) - g_clients\[i\]\.last_active", main), \
        "nothing actually compares slot activity against the timeout"


# ------------------------------------------------- the Win9x CreateThread trap

def test_createthread_passes_a_thread_id_pointer():
    """On Windows 95/98 CreateThread REQUIRES a non-NULL lpThreadId; only NT
    allows NULL. Every fire-and-forget helper passed NULL, so on the Win98 box
    automap, autoupdate, retrowall, watchdog, ai_status, sharelog and dosstage
    all failed with ERROR_INVALID_PARAMETER (87) and silently never ran.

    That single line is why auto-update did nothing on that machine for four
    versions, why its log was never mirrored to the share, and why the share
    had to be mapped by a batch file. Only dosstage checked its return value,
    and its message blamed memory - on a box with 87MB free.
    """
    main = read("main.c")

    # every CreateThread must hand over a thread-id pointer
    bad = re.findall(r"CreateThread\([^;]*?,\s*NULL\s*\)\s*;", main, re.S)
    assert not bad, (
        "%d CreateThread call(s) still pass NULL for lpThreadId - illegal on "
        "Win9x: %r" % (len(bad), bad[:2]))

    body = func_body(main, "static int spawn_helper(")
    assert "&tid" in body, "spawn_helper must pass a thread-id pointer"
    assert "ERROR_INVALID_PARAMETER" in body, (
        "the failure message must name the real cause rather than guess at "
        "memory - that guess cost days")
