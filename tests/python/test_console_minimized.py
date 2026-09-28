"""The agent's own console must not sit on the desktop icons (2026-09-28).

THE DEFECT
----------
The agent is a console program, so every start opened a ~670x340 console in
the middle of the desktop and left it there. WINLIST after a boot:

    .243 (Win98)          class tty                covered the WHOLE icon bay
                                                   (22,22-674,381)
    .195 / .110 / .124    class ConsoleWindowClass over the top-left icons
                                                   Auto Arrange packs there

and people clicked on it: "console control event 2" (CTRL_CLOSE_EVENT) twice
in .195's agent.log on 2026-09-26 - the first left the box with no agent for
35 minutes - and a click inside starts a QuickEdit selection.

THE FIX, BY PLATFORM
--------------------
* NT (XP/7): the agent minimizes its own console at startup
  (agent/src/consolewin.c; decision in agent/shared/consolemin.h, pinned by
  tests/native/test_consolemin.c). GetConsoleWindow is Windows 2000+ and
  GetConsoleProcessList XP+, so both are resolved through ntdyn.c: a static
  import makes the EXE unloadable on Win98 (test_agent_win9x_imports.py
  asserts the BUILT binary).
* Win9x: there is no GetConsoleWindow, and the console window (class tty)
  does not even carry the SetConsoleTitle text - .243's reads "RETRO_~2" - so
  the agent cannot find it from inside. The launchers start it minimized:
  START.EXE /m in scripts/dosgames/AGENTRUN.BAT (the Run key) and in RESTART's
  batch (handlers.c).

SW_SHOWMINNOACTIVE, never SW_HIDE: the operator must still find a running
agent on the taskbar. Opt-out: HKLM\\Software\\RetroAgent\\ConsoleVisible=1.
"""

import pathlib
import re

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"
SHARED = REPO / "agent" / "shared"
CONSOLEWIN_C = SRC / "consolewin.c"
CONSOLEMIN_H = SHARED / "consolemin.h"
NTDYN_C = SRC / "ntdyn.c"
MAIN_C = SRC / "main.c"
HANDLERS_C = SRC / "handlers.c"
MAKEFILE = REPO / "agent" / "Makefile"
AGENTRUN = REPO / "scripts" / "dosgames" / "AGENTRUN.BAT"
AGENT_EXE = REPO / "agent" / "retro_agent.exe"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


def code_only(s):
    """C source with comments and string literals removed."""
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    s = re.sub(r"//[^\n]*", " ", s)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', s)


def function_body(src, signature):
    start = src.index(signature)
    depth, i = 0, src.index("{", start)
    while True:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1


# ---------------------------------------------------------------------------
# Win9x: the launchers must start the agent MINIMIZED.
# ---------------------------------------------------------------------------
def starts_the_agent_minimized(line):
    """Is this COMMAND.COM line a Win98 START.EXE launch of the agent that
    asks for a minimized window?

    Win98's START takes `/m` (it also accepts /max, /r, /w). It takes NO
    window title: `start "" x` is cmd.exe syntax and START.EXE reads the ""
    as the program (that stranded .243 once already)."""
    ln = line.strip()
    if not re.match(r"(?i)^start\s", ln):
        return False
    toks = ln.split()
    opts = [t.lower() for t in toks[1:] if t.startswith("/")]
    prog = next((t for t in toks[1:] if not t.startswith("/")), "")
    return "/m" in opts and prog != '""' and bool(prog)


def test_the_start_predicate_tells_the_old_line_from_the_new():
    """A checker that can only say OK is the failure this project keeps
    paying for - prove it rejects the pre-fix line."""
    old = r"start %AGDIR%\retro_agent.exe"           # pre-fix AGENTRUN.BAT
    new = r"start /m %AGDIR%\retro_agent.exe"
    assert not starts_the_agent_minimized(old)
    assert starts_the_agent_minimized(new)
    assert not starts_the_agent_minimized(r'start "" /m C:\RETRO_AGENT\retro_agent.exe')
    assert not starts_the_agent_minimized(r"start /w %AGDIR%\retro_agent.exe")


def test_agentrun_starts_the_agent_minimized():
    """The Win9x Run-key launcher (.243). Every line that starts the agent
    must ask for a minimized window - a restored one lands on the icon bay."""
    text = AGENTRUN.read_bytes().decode("ascii")
    starts = [ln for ln in text.split("\r\n")
              if re.match(r"(?i)^\s*start\s", ln) and "retro_agent" in ln.lower()]
    assert starts, "AGENTRUN.BAT no longer starts the agent at all"
    bad = [ln for ln in starts if not starts_the_agent_minimized(ln)]
    assert not bad, ("AGENTRUN.BAT starts the agent restored, over the desktop "
                     "icons: %s - use Win98 START.EXE /m" % bad)
    assert not re.search(r"(?im)^\s*start\s+/min\b", text), (
        "/min is cmd.exe; Win98's START.EXE spells it /m")


def test_agentrun_keeps_its_line_endings():
    """COMMAND.COM batch files are CRLF; a LF-only rewrite of this file is a
    whole-file churn and a batch Win98 may misparse."""
    raw = AGENTRUN.read_bytes()
    assert b"\r\n" in raw
    assert raw.count(b"\n") == raw.count(b"\r\n"), "bare LF in AGENTRUN.BAT"


def test_restart_batch_starts_a_win9x_agent_minimized():
    """RESTART is the other thing that starts an agent on Win9x."""
    body = function_body(read(HANDLERS_C), "void handle_restart(")
    win9x = body.split("GetVersion() & 0x80000000", 1)[1].split("} else {", 1)[0]
    lines = re.findall(r'fprintf\(f,\s*"([^"]*)"', win9x)
    starts = [l.replace("\\r\\n", "") for l in lines if l.startswith("start")]
    assert starts == ["start /m %s"], starts
    assert starts_the_agent_minimized(starts[0].replace("%s", r"C:\RETRO_~1\RETRO_~2.EXE"))


# ---------------------------------------------------------------------------
# NT: the agent minimizes itself.
# ---------------------------------------------------------------------------
def test_get_console_window_is_resolved_dynamically():
    s = read(NTDYN_C)
    assert 'GetProcAddress(h, "GetConsoleWindow")' in s
    assert 'GetProcAddress(h, "GetConsoleProcessList")' in s
    for name in ("GetConsoleWindow", "GetConsoleProcessList"):
        for src in (CONSOLEWIN_C, MAIN_C, NTDYN_C):
            assert not re.search(r"(?<![\w.>])" + name + r"\s*\(", code_only(read(src))), (
                "%s calls %s directly - that is a static import, and Win98's "
                "kernel32 has no such export: the EXE would not LOAD there"
                % (src.name, name))
    assert "ntdyn_GetConsoleWindow()" in read(CONSOLEWIN_C)


def test_the_window_is_minimized_not_hidden_and_not_activated():
    code = code_only(read(CONSOLEWIN_C))
    assert "ShowWindow(hwnd, SW_SHOWMINNOACTIVE)" in code
    for wrong in ("SW_HIDE", "SW_MINIMIZE", "SW_SHOWMINIMIZED", "SW_FORCEMINIMIZE"):
        assert not re.search(r"\b%s\b" % wrong, code), (
            "%s: SW_HIDE loses the taskbar button (a running agent then looks "
            "dead), SW_MINIMIZE/SW_SHOWMINIMIZED activate another window, "
            "SW_FORCEMINIMIZE is 2000+ only" % wrong)
    # consolemin.h is Win32-free and carries the number; the .c pins it.
    assert "CM_SHOW_CMD == SW_SHOWMINNOACTIVE" in code
    assert re.search(r"#define\s+CM_SHOW_CMD\s+7\b", read(CONSOLEMIN_H))


def test_the_result_is_read_back_not_trusted():
    body = function_body(read(CONSOLEWIN_C), "static DWORD WINAPI minimize_thread(")
    assert body.index("ShowWindow(") < body.index("IsIconic(hwnd)")
    assert "DID NOT TAKE" in body, "a failed minimize must say so in the log"


def test_the_blocking_call_is_off_the_startup_path():
    """ShowWindow on the console host's window SENDS it messages; a hung host
    must not be able to stall agent startup. It runs on a helper thread, and
    that thread is started with a real lpThreadId (Win9x rejects NULL)."""
    s = read(CONSOLEWIN_C)
    startup = code_only(function_body(s, "void consolewin_startup("))
    assert "ShowWindow" not in startup
    assert re.search(r"CreateThread\([^;]*minimize_thread[^;]*&tid\)", startup)
    assert "CloseHandle(h)" in startup


def test_opt_out_key_and_host_policy():
    h = read(CONSOLEMIN_H)
    assert '#define CM_OPTOUT_KEY    "Software\\\\RetroAgent"' in h
    assert '#define CM_OPTOUT_VALUE  "ConsoleVisible"' in h
    s = read(CONSOLEWIN_C)
    body = function_body(s, "static int optout_set(")
    assert "HKEY_LOCAL_MACHINE" in body and "CM_OPTOUT_VALUE" in body
    assert "type == REG_DWORD" in body, "only a DWORD opts out"
    startup = function_body(s, "void consolewin_startup(")
    # a Windows 11 box that runs the agent only to be reachable is hands-off,
    # and there the console may be a Windows Terminal tab
    assert "host_manages_this_box()" in startup
    assert "optout_set()" in startup
    assert "ntdyn_GetConsoleProcessList" in startup


def test_main_minimizes_before_anything_can_share_the_console():
    """consolewin_startup asks how many processes share the console (a hand
    start from cmd.exe must not have that shell minimized). A child the agent
    spawned itself would count too, so the call sits right after the title is
    set and before WSAStartup and before any thread or process exists."""
    body = function_body(read(MAIN_C), "void agent_run(void)")
    call = body.index("consolewin_startup(g_service_mode)")
    # Since 1.90.0 (the console-echo wedge fix) the title is QUEUED for log.c's
    # echo thread with log_console_title() - agent_run() never calls
    # SetConsoleTitleA itself (tests/python/test_log_console_echo.py). The
    # minimize still follows the title step. log_init's echo thread already
    # exists by then, but it starts no process, so it cannot inflate the
    # console's process count this call reads.
    assert "SetConsoleTitleA(" not in body
    assert body.index("log_console_title(title)") < call
    for later in ("WSAStartup(", "CreateThread(", "spawn_helper(", "CreateProcess"):
        if later in body:
            assert call < body.index(later), "%s runs before the minimize" % later
    assert '#include "consolewin.h"' in read(MAIN_C)


def test_consolewin_is_built():
    mk = read(MAKEFILE)
    sources = mk[mk.index("SOURCES ="):mk.index("OBJECTS =")]
    assert "$(SRCDIR)/consolewin.c" in sources


def test_built_agent_carries_the_dynamic_names():
    """Post-condition on the binary: the names are STRINGS for GetProcAddress
    (the import-table half - that they are NOT imports - is asserted by
    test_agent_win9x_imports.py, which builds the agent)."""
    if not AGENT_EXE.is_file():
        pytest.skip("%s not built - run `make` in agent/ (or the win9x-imports "
                    "test, which builds it)" % AGENT_EXE)
    data = AGENT_EXE.read_bytes()
    assert b"GetConsoleWindow\0" in data
    assert b"GetConsoleProcessList\0" in data
    assert b"ConsoleVisible\0" in data
