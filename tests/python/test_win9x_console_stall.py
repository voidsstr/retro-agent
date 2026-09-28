"""A Win9x agent must never share its console with a long-running DOS child.

Measured on .243 (Windows 98 SE, agent 1.86.1) on 2026-09-28: LAUNCH ran
`command.com /c FILL.BAT` with creation flags 0, so the DOS batch ran INSIDE
the agent's own console - on Win9x a DOS virtual machine. The very next
accept() logged "Connection from ..." and then the accept loop's
printf("Connection from ...") blocked for the whole 74 minutes the batch ran
DJGPP UNZIP: the one thread that serves every client in multiplex mode
answered nobody, 9898 refused (backlog full) and 9897 accepted and never
answered - the "dead agent" signature this project has written up before.
It resumed the second the batch ended (FILL.LOG's END at 07:05 UTC = the log's
02:05:04 box time). A GUI program launched the same way (ScanDisk) never
stalled it, because command.com hands it off and returns.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "agent" / "src"


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.M | re.S)
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


def test_launch_gives_a_win9x_child_its_own_console():
    b = body((SRC / "exec.c").read_text(), "handle_launch")
    call = b[b.index("CreateProcessA("):]
    call = call[:call.index(");")]
    assert "CREATE_NEW_CONSOLE" in call, "a DOS child inherits the agent's console VM and stalls it"
    assert "0x80000000" in call, "only Win9x: NT consoles are serviced by csrss and keep their old flags"


def test_the_multiplex_accept_loop_writes_nothing_to_the_console():
    s = (SRC / "main.c").read_text()
    i = s.index('printf("Connection from')
    guard = s[s.rfind("if (", 0, i):i]
    assert "agent_console_quiet()" in guard, \
        "in multiplex mode ONE thread serves every client; a blocked console write stops them all"
    q = body(s, "agent_console_quiet")
    assert "g_client_mode == MODE_MULTIPLEX" in q and "g_service_mode" in q


def test_transfer_progress_never_writes_to_the_console_in_multiplex():
    """UPLOAD/DOWNLOAD/frame_recv over 64 KB printed progress on the serving
    thread, unguarded - and DOWNLOAD is the prescribed 9x recovery route
    (found by the 2026-09-28 audit)."""
    import re as _re
    for f in ("files.c", "protocol.c"):
        s = (SRC / f).read_text()
        bare = [m.start() for m in _re.finditer(r"(?<![_A-Za-z])printf\(", s)]
        assert not bare, f"{f}: a bare printf on the serving path"
        assert "fflush(stdout)" not in s, f"{f}: a bare fflush(stdout)"
    h = (SRC / "log.h").read_text()
    assert "if (!agent_console_quiet()) printf(" in h


def test_exec_keeps_its_console_flags_on_9x_until_proven():
    """The audit's verifiers: CREATE_NEW_CONSOLE on a 16-bit command.com with
    STARTF_USESTDHANDLES pipes is unproven on 9x (capture may come back empty,
    a 'Finished' VM may never signal). EXEC keeps its flags; only its console
    writes on the serving thread are removed. Change this test only with a
    hardware result."""
    s = (SRC / "exec.c").read_text()
    d = body(s, "do_exec")
    assert "CREATE_NO_WINDOW" in d
