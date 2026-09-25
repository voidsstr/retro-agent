"""Regression: the DOS combined agent+chat (agent/doschat) must keep sharing
code with the Windows build, and keep the DOS-specific memory limits that
were emulator-verified on 2026-07-28.

Why these are source invariants: the DOS binary can only be built with the
Open Watcom + mTCP toolchain, so the suite can't compile it here. These
assertions guard the properties whose violation cost real debugging time.
"""

import os
import re

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DC = os.path.join(REPO, "agent", "doschat", "doschat.cpp")
CFG = os.path.join(REPO, "agent", "doschat", "doschat.cfg")
MK = os.path.join(REPO, "agent", "doschat", "Makefile")
CHATPROXY = os.path.join(REPO, "agent", "src", "chatproxy.c")
RETRO_CHAT = os.path.join(REPO, "agent", "tools", "retro_chat.c")
PROTO_H = os.path.join(REPO, "agent", "src", "protocol.h")


def _read(p):
    assert os.path.isfile(p), "%s missing" % p
    return open(p, encoding="utf-8", errors="replace").read()


def test_all_three_binaries_share_the_same_modules():
    """chatcore + chattext + frameproto must be included, not re-implemented."""
    dc = _read(DC)
    assert '#include "../shared/chatcore.c"' in dc
    assert '#include "../shared/chattext.h"' in dc
    assert '#include "../shared/frameproto.h"' in dc

    cp = _read(CHATPROXY)
    assert '#include "../shared/chatcore.c"' in cp
    assert "chatcore_prompt_push" in cp and "chatcore_log_append" in cp
    assert '#include "../shared/frameproto.h"' in _read(PROTO_H)

    rc = _read(RETRO_CHAT)
    assert '#include "../shared/chattext.h"' in rc
    assert "chat_sanitize_chunk" in rc and "chat_wrap_text" in rc


def test_no_duplicate_wire_constants():
    """Ports/status bytes must come from frameproto.h only."""
    for path in (PROTO_H, DC):
        text = _read(path)
        body = text.split("frameproto.h", 1)[1]
        assert not re.search(r"#define\s+AGENT_TCP_PORT\s", body), path
        assert not re.search(r"#define\s+RESP_OK_TEXT\s", body), path


def test_dos_memory_limits_are_pinned():
    """Verified in DOSBox-X: more than 5 sockets overflows mTCP's 64K socket
    malloc unless TCP_SOCKET_RING_SIZE stays at the default 4; DGROUP also
    has to leave room, hence the far-heap scratch buffer."""
    dc, cfg = _read(DC), _read(CFG)
    m = re.search(r"#define\s+MAX_CLIENTS\s+(\d+)", dc)
    assert m, "MAX_CLIENTS missing"
    clients = int(m.group(1))
    assert clients >= 5, (
        "the chat daemon alone holds 3 long-poll connections; fewer than 5 "
        "slots starves normal clients")

    m = re.search(r"#define\s+TCP_MAX_SOCKETS\s+\((\d+)\)", cfg)
    assert m, "TCP_MAX_SOCKETS missing from doschat.cfg"
    assert int(m.group(1)) >= clients + 1, (
        "TCP_MAX_SOCKETS must cover every client plus the listener")

    m = re.search(r"#define\s+TCP_SOCKET_RING_SIZE\s+\((\d+)\)", cfg)
    assert m and int(m.group(1)) <= 4, (
        "a bigger socket ring doubles sizeof(TcpSocket); mTCP's socket table "
        "is a single malloc capped at 64K and initStack then fails")

    assert "_fmalloc" in dc and "far *scratch" in dc, (
        "response scratch must live on the far heap, not in DGROUP")


def test_cfg_change_forces_library_rebuild():
    """Every mTCP object bakes in doschat.cfg — a stale library silently
    fails initStack with a misleading 'packet driver?' message."""
    mk = _read(MK)
    assert re.search(r"\$\(TCPOBJS\):\s*doschat\.cfg", mk), (
        "Makefile must rebuild the mTCP objects when doschat.cfg changes")


def test_dos_agent_speaks_the_chat_bus_and_discovery():
    dc = _read(DC)
    for cmd in ("PROMPT_PUSH", "PROMPT_POP", "PROMPT_WAIT", "LOG_APPEND",
                "LOG_APPEND2", "LOG_READ", "LOG_WAIT", "LOG_CLEAR", "STATUS_SET",
                "STATUS_GET", "STATUS_WAIT", "PING", "SYSINFO", "EXEC"):
        assert '"%s"' % cmd in dc, "DOS agent must handle %s" % cmd
    assert "RETRO|%s|" in dc, "must broadcast the standard discovery packet"
    assert "AGENT_UDP_PORT" in dc


def test_dos_agent_follows_the_shared_chat_semantics():
    """agent 1.85.0 changed three chat-bus semantics in the SHARED engine; the
    DOS agent+chat must use them rather than its own copies.

    - ABSOLUTE log offsets: the DOS UI kept a buffer offset, so after the ring
      dropped its oldest half it pointed past the end and most of a long
      reply never appeared.
    - a prompt is TAKEN (in flight until the connection's next command) and
      put back when the connection drops, instead of popped and lost.
    - LOG_APPEND2 dedupes a resent chunk.
    The shared logic itself is exercised in tests/native/test_chatcore.c;
    this cannot build the 16-bit exe, so it pins the wiring."""
    dc = _read(DC)
    pump = dc.split("static void ui_pump_log(", 1)[1].split("\n}\n", 1)[0]
    assert "chatcore_log_window(&core, ui_log_shown" in pump, (
        "the DOS UI must resolve its offset against the ring")
    assert "core.log + ui_log_shown" not in pump, "buffer offsets are gone"
    assert "chatcore_log_end(&core) != ui_log_shown" in dc
    read = dc.split("static void answer_log_read(", 1)[1].split("\n}\n", 1)[0]
    assert "chatcore_log_window" in read and "chatcore_log_end" in read

    assert "chatcore_prompt_pop(" not in dc, "take, not pop"
    assert "chatcore_prompt_take(&core, out, sizeof(out), owner_of(c))" in dc
    drop = dc.split("static void client_drop(", 1)[1].split("\n}\n", 1)[0]
    assert "chatcore_prompt_requeue(&core, owner_of(c))" in drop
    disp = dc.split("static void dispatch(", 1)[1]
    assert "chatcore_prompt_ack(&core, owner_of(c))" in disp
    assert "isRemoteClosed()" in dc.split("static void service_longpolls(", 1)[1]

    assert '"LOG_APPEND2"' in dc and "chatcore_log_append_once" in dc
    assert "chatcore_push_reply" in dc

    # a reader at the ring's base must get the whole ring in ONE reply: at
    # LOG_MAX_DOS == FRAME_CAP the header pushed it over the cap, unsent
    assert re.search(r"#define\s+LOG_MAX_DOS\s+\(FRAME_CAP - \d+\)", dc)


# --- agent shutdown safety (hardware-found on the Deskpro, 2026-07-29) ---

MAIN_C = os.path.join(REPO, "agent", "src", "main.c")
HANDLERS_C = os.path.join(REPO, "agent", "src", "handlers.c")


def test_shutdown_closes_the_alt_listener_and_exits():
    """A QUIT left :9897 bound with nothing servicing it, so the box looked
    reachable, answered nothing, and needed physical access to recover."""
    s = _read(MAIN_C)
    tail = s.split("clients_cleanup();", 1)[1]
    assert "closesocket(listen_sock_alt)" in tail, (
        "shutdown must close the ALT listener too, not just listen_sock")
    assert "ExitProcess(0)" in tail, (
        "the process must be guaranteed to die; a lingering helper thread "
        "must not be able to keep a quit agent holding its ports")


def test_restart_command_exists_and_relaunches_before_stopping():
    """RESTART is the safe remote restart: QUIT alone strands a Win9x box
    because nothing supervises the agent there."""
    h = _read(HANDLERS_C)
    assert '{ "RESTART"' in h, "RESTART must be registered in the command table"
    body = h.split("void handle_restart(", 1)[1].split("\nvoid ", 1)[0]
    assert "CreateProcessA" in body, "must spawn the relaunch batch"
    assert body.index("CreateProcessA") < body.index("g_running = 0"), (
        "the relauncher must be started BEFORE the agent stops, or a failed "
        "spawn strands the box")
    assert "ping -n" in body, (
        "use ping as the sleep — Win9x COMMAND.COM has no timeout command")


def test_restart_batch_speaks_win9x_start_on_win9x():
    """agent 1.85.0. RESTART wrote `start "" "C:\\...\\retro_agent.exe"` on every
    Windows. The empty title is cmd.exe syntax; Win98's START.EXE takes the ""
    as the program, fails, and the agent never came back on .243 (2026-09-24)
    - networking up, 9898 refused, a person needed. On 9x the batch must use
    the unquoted 8.3 path, the form the auto-update batch has proven there."""
    h = _read(HANDLERS_C)
    body = h.split("void handle_restart(", 1)[1].split("\nvoid ", 1)[0]
    assert "GetVersion() & 0x80000000" in body, "must branch on Win9x"
    win9x, nt = body.split("GetVersion() & 0x80000000", 1)[1].split("} else {", 1)
    assert "GetShortPathNameA" in win9x
    emitted_9x = [ln for ln in win9x.splitlines() if "fprintf(f," in ln]
    assert emitted_9x == ['        fprintf(f, "start %s\\r\\n", shortp);'], emitted_9x
    assert '\\"\\"' not in "\n".join(emitted_9x), "no empty title on Win9x"
    assert 'start \\"\\" \\"%s\\"' in nt.split("fclose(f)")[0], (
        "NT keeps the quoted form - a long path with spaces needs it there")


AUTOUPDATE_C = os.path.join(REPO, "agent", "src", "autoupdate.c")


def test_update_batch_is_bounded_and_always_starts_an_agent():
    """The retry loop used to be unbounded (`if errorlevel 1 goto wait`).
    You cannot overwrite a running exe, so if the old agent failed to exit
    the batch spun forever and the box was left with NO agent — physical
    access required. Stranded the Deskpro on 2026-07-29."""
    s = _read(AUTOUPDATE_C)
    body = s.split("static int build_restart_bat(", 1)[1].split("\nstatic ", 1)[0]
    # Look only at the lines that EMIT batch content, not at commentary.
    emitted = [ln for ln in body.splitlines() if "fprintf(f," in ln]
    emitted_text = "\n".join(emitted)
    assert "goto wait" not in emitted_text, (
        "the unbounded retry loop must not come back")
    assert "UPDATE_SWAP_TRIES" in body, "the retry count must be bounded"
    # after giving up it must still relaunch something
    give_up = emitted_text.split("goto swapped", 1)[1]
    assert "start %s" in give_up, (
        "after exhausting retries the batch must relaunch the existing agent "
        "— an old-version agent beats no agent")


CHATPROXY_C = os.path.join(REPO, "agent", "src", "chatproxy.c")


def test_multiplex_longpolls_park_instead_of_blocking():
    """Win9x forces MULTIPLEX mode: one thread serves every client. A blocking
    30s LOG_WAIT there stalls all other clients — the Deskpro served localhost
    happily while a remote AUTH sat unprocessed for 90 seconds (2026-07-29).

    The 1 s clamp that fixed that still left every poller BLOCKING the one
    thread for its second (the events it waited on can only be set by
    handlers on that same thread, so it never woke early): pollers serialised
    into back-to-back 1 s stalls and every command queued behind them. Since
    agent 1.85.0 a multiplex long-poll is PARKED in its client slot and
    answered by the loop - the DOS agent's model. Behaviour is exercised in
    tests/native/test_chatproxy.c; this pins the wiring."""
    m = _read(MAIN_C)
    proc = m.split("static int client_process(", 1)[1].split("\n}\n", 1)[0]
    assert "chatproxy_set_park_slot(&cl->park)" in proc, (
        "multiplex commands must be handed their slot's park")
    assert "chatproxy_set_park_slot(NULL)" in proc
    loop = m.split("/* Accept loop */", 1)[1]
    assert loop.count("service_parked_polls()") >= 3, (
        "parked polls must be answered after each command, after each select "
        "pass, and when select times out (deadlines)")
    assert "chatproxy_park_ms_left" in loop, (
        "select()'s timeout must be bounded by the nearest parked deadline")
    drop = m.split("static void client_drop(", 1)[1].split("\n}\n", 1)[0]
    assert drop.index("chatproxy_conn_closed(") < drop.index("closesocket(g_clients"), (
        "a dropped client's prompt must be put back before its handle is freed")

    # the clamp survives only as a guard on a BLOCKING wait in multiplex mode
    assert re.search(r"MODE_MULTIPLEX\)\s*\{\s*\n\s*g_longpoll_max_ms\s*=\s*[1-9]",
                     m), "multiplex mode must keep a non-zero blocking clamp"
    cp = _read(CHATPROXY_C)
    wait = cp.split("static DWORD wait_ms(", 1)[1].split("\n}\n", 1)[0]
    assert "g_longpoll_max_ms" in wait and "blocking" in wait
    for fn in ("handle_log_wait", "handle_prompt_wait", "handle_status_wait"):
        body = cp.split("void %s(" % fn, 1)[1].split("\nvoid ", 1)[0]
        assert "g_park_slot" in body, "%s must park in multiplex mode" % fn
        assert re.search(r"wait_ms\([^)]*, 1\)", body), (
            "%s's blocking path must go through the clamp" % fn)


def test_helper_thread_failures_are_logged():
    """A CreateThread failure used to be silent, so a feature that never ran
    left no trace — exactly what hid dosstage on a 0MB-free box.

    The check moved rather than went away: every helper now starts through
    spawn_helper(), which reports the failure AND closes the handle on success
    (discarding it leaked a kernel object per thread started)."""
    m = _read(MAIN_C)
    assert re.search(r"spawn_helper\(dosstage_thread", m), (
        "dosstage must be started through the checked spawner")
    assert re.search(r"static int spawn_helper\(", m), (
        "the checked spawner must exist")
    assert "thread FAILED to start" in m


def test_enough_client_slots_for_local_chat_plus_daemon():
    """retro_chat holds 3 connections (command + log poll + status poll) and
    the fleet daemon needs 2 (wait + send). With only 4 slots the daemon could
    not attach to a box running the chat locally, so nobody polled its prompts
    and typing into that machine's chat produced no reply (2026-08-03)."""
    m = _read(MAIN_C)
    mm = re.search(r"#define\s+MAX_CLIENTS\s+(\d+)", m)
    assert mm, "MAX_CLIENTS missing"
    assert int(mm.group(1)) >= 6, (
        "need room for the local chat client (3) + daemon (2) + an operator; "
        "got %s" % mm.group(1))


def test_win9x_agent_terminates_itself_after_a_clean_shutdown():
    """1.85.0. On .243 (Win98 SE) 1.84.2 logged "shutdown complete; exiting
    process" at QUIT and was still running 120 s later: ExitProcess on 9x runs
    every DLL's PROCESS_DETACH and one of them blocked. The swap tool gave up
    and the box had no agent until a person restarted it. After the log is
    closed, a 9x agent ends itself with TerminateProcess."""
    src = _read(os.path.join(REPO, "agent", "src", "main.c"))
    tail = src[src.index('"shutdown complete; exiting process"'):]
    tail = tail[:tail.index("\nint main(")]
    assert tail.index("log_shutdown();") < tail.index("TerminateProcess(GetCurrentProcess(), 0)"), \
        "the log must be flushed and closed before the hard exit"
    assert "if (GetVersion() & 0x80000000)\n        TerminateProcess(GetCurrentProcess(), 0);" in tail
    assert tail.rstrip().endswith("ExitProcess(0);\n}") or "ExitProcess(0);" in tail
