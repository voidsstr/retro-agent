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
                "LOG_READ", "LOG_WAIT", "LOG_CLEAR", "STATUS_SET",
                "STATUS_GET", "STATUS_WAIT", "PING", "SYSINFO", "EXEC"):
        assert '"%s"' % cmd in dc, "DOS agent must handle %s" % cmd
    assert "RETRO|%s|" in dc, "must broadcast the standard discovery packet"
    assert "AGENT_UDP_PORT" in dc


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


def test_multiplex_longpolls_are_clamped():
    """Win9x forces MULTIPLEX mode: one thread serves every client. A blocking
    30s LOG_WAIT there stalls all other clients — the Deskpro served localhost
    happily while a remote AUTH sat unprocessed for 90 seconds (2026-07-29)."""
    m = _read(MAIN_C)
    assert "g_longpoll_max_ms" in m, "the clamp knob must exist"
    assert re.search(r"MODE_MULTIPLEX\)\s*\{\s*\n\s*g_longpoll_max_ms\s*=\s*[1-9]",
                     m), "multiplex mode must set a non-zero clamp"

    cp = _read(CHATPROXY_C)
    # every long-poll handler must honour the clamp
    for fn in ("handle_log_wait", "handle_prompt_wait", "handle_status_wait"):
        body = cp.split("void %s(" % fn, 1)[1].split("\nvoid ", 1)[0]
        assert "g_longpoll_max_ms" in body, (
            "%s must clamp its wait or it can stall every other client" % fn)


def test_helper_thread_failures_are_logged():
    """A CreateThread failure used to be silent, so a feature that never ran
    left no trace — exactly what hid dosstage on a 0MB-free box."""
    m = _read(MAIN_C)
    assert re.search(r"if\s*\(!CreateThread\([^)]*dosstage_thread", m), (
        "dosstage thread creation must be checked")
    assert "FAILED to start" in m
