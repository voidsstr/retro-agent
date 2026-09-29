"""No thread that logs or serves may ever wait on the agent's console.

THE DEFECT (agent <= 1.89.1)
----------------------------
`agent/src/log.c` `raw_out()` echoed every log line to the console with
`WriteFile(GetStdHandle(STD_ERROR_HANDLE))` WHILE HOLDING `g_log_cs`. A console
write BLOCKS for as long as the console is not being serviced - a QuickEdit /
Mark selection in the agent's window, a hung conhost/csrss, or a hung display
(on NT the console is drawn by a GUI process). The thread stuck in that write
kept the log lock, so every other thread that logs - the accept loop, every
command handler, the flusher - queued behind it: new TCP connections timed out
and were then refused while the OS was fine. ADMIN-PC (Win7, .195) sat like that
from 2026-09-26 23:46 until a person held the power button 35 hours later
(`.claude/evidence-icons/192.168.1.195/crash/DIAGNOSIS.md`): its Radeon hung
(TDR 0x117, never recovered), and agent.log simply stops. The accept loop's own
`printf("Connection from ...")` would have blocked the same way even without
the log lock.

THE FIX (agent 1.90.0)
----------------------
Loggers only COPY a line into a bounded ring (`agent/shared/conring.h`, logic
tested natively by `tests/native/test_conring.c`); ONE idle-priority thread,
`log_echo_thread()`, writes it to the console with no lock held. A console that
stops accepting output stalls that thread alone; the ring fills and drops (and
counts) instead of waiting. Every other console touch - the startup and
transfer-progress printf's (`con_printf`), the window title - goes the same
way, and on NT the echo thread clears QuickEdit so a click cannot start a
selection.

These are SOURCE invariants, pinned in both directions: the checker below is
also run against the 1.89.1 code and must find the defect there.
"""
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "agent" / "src"


# --------------------------------------------------------------- C helpers
def code_only(text):
    """Strip comments and the CONTENTS of string/char literals, so neither an
    explanatory comment nor a message text can satisfy or trip a check."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c in "\"'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(q + q)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


DEF = re.compile(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*\{", re.M)


def functions(src):
    """{name: body} for every function DEFINED in `src` (already code_only)."""
    fns = {}
    for m in DEF.finditer(src):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "sizeof", "return"):
            continue
        i = src.index("{", m.end() - 1)
        depth = 0
        for j in range(i, len(src)):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    fns[name] = src[i:j + 1]
                    break
    return fns


# A call into the console by API: the console functions and the stdio streams.
CONSOLE_API = (
    r"\b(?:WriteConsole|ReadConsole|SetConsoleTitle|FillConsoleOutput)\w*\s*\("
    r"|\b(?:SetConsoleMode|GetConsoleMode|FlushConsoleInputBuffer)\s*\("
    r"|(?<![\w])(?:printf|vprintf|puts|putchar|_cprintf|_cputs)\s*\("
    r"|\bf(?:printf|puts|flush|write)\s*\([^;]*?\b(?:stdout|stderr)\b")
CONSOLE_API_CALL = re.compile(CONSOLE_API)
# Inside log.c a WriteFile on anything but the log FILE handle is a console
# write too: that is exactly what raw_out did. (Elsewhere WriteFile is file
# I/O; no other source may even obtain a console handle - checked below.)
CONSOLE_CALL = re.compile(r"\bWriteFile\s*\(\s*(?!g_log_h\b)|" + CONSOLE_API)

LOCK_ENTER = "EnterCriticalSection(&g_log_cs)"
LOCK_LEAVE = "LeaveCriticalSection(&g_log_cs)"


def console_touchers(fns):
    """Functions that call into the console directly or through a callee."""
    touch = {f for f, b in fns.items() if CONSOLE_CALL.search(b)}
    changed = True
    while changed:
        changed = False
        for f, b in fns.items():
            if f in touch:
                continue
            if any(re.search(r"\b%s\s*\(" % re.escape(g), b) for g in touch):
                touch.add(f)
                changed = True
    return touch


def locked_regions(body):
    regions, at = [], 0
    while True:
        i = body.find(LOCK_ENTER, at)
        if i < 0:
            return regions
        j = body.find(LOCK_LEAVE, i)
        assert j > i, "an EnterCriticalSection(&g_log_cs) with no Leave after it"
        regions.append(body[i + len(LOCK_ENTER):j])
        at = j + len(LOCK_LEAVE)


def console_calls_under_the_log_lock(src):
    """[(function, what)] for every console call reachable while g_log_cs is
    held - directly in a locked region, or via any function it calls."""
    src = code_only(src)
    fns = functions(src)
    touch = console_touchers(fns)
    bad = []
    for f, b in fns.items():
        for region in locked_regions(b):
            m = CONSOLE_CALL.search(region)
            if m:
                bad.append((f, m.group(0).strip()))
            for g in touch:
                if re.search(r"\b%s\s*\(" % re.escape(g), region):
                    bad.append((f, g + "()"))
    return bad


def read(name):
    return (SRC / name).read_text(errors="replace")


# ---- the 1.89.1 code, verbatim in the parts that matter (log.c raw_out and
# log_msg), so the checker is proven to catch the defect it exists for.
OLD_LOG_C = r'''
static int disk_out(const char *s, DWORD len)
{
    DWORD wr;
    if (g_log_h == INVALID_HANDLE_VALUE || len == 0) return 0;
    SetFilePointer(g_log_h, 0, NULL, FILE_END);
    if (WriteFile(g_log_h, s, len, &wr, NULL)) {
        FlushFileBuffers(g_log_h);
        return 1;
    }
    return 0;
}

static void raw_out(const char *s, DWORD len)
{
    DWORD wr;
    HANDLE e;

    if (g_buffered && g_log_h != INVALID_HANDLE_VALUE) {
        if (g_buf_len + (int)len > LOG_BUF_BYTES)
            flush_locked();
        memcpy(g_buf + g_buf_len, s, len);
        g_buf_len += (int)len;
    } else {
        disk_out(s, len);
    }

    /* Best-effort console echo; guarded so an invalid handle (GUI launch)
     * can never fault us. */
    e = GetStdHandle(STD_ERROR_HANDLE);
    if (e != NULL && e != INVALID_HANDLE_VALUE)
        WriteFile(e, s, len, &wr, NULL);
}

void log_msg(const char *tag, const char *fmt, ...)
{
    char line[2048];
    int n, lifted;
    lifted = log_lift();
    EnterCriticalSection(&g_log_cs);
    raw_out(line, (DWORD)n);
    LeaveCriticalSection(&g_log_cs);
    log_unlift(lifted);
}
'''


# ------------------------------------------------------------------ the fix
def test_no_console_call_is_reachable_while_the_log_lock_is_held():
    bad = console_calls_under_the_log_lock(read("log.c"))
    assert not bad, (
        "a console call under g_log_cs blocks every thread that logs while the "
        "console is frozen (ADMIN-PC, 2026-09-26): %r" % bad)


def test_the_checker_finds_the_old_defect():
    """Both directions: on the 1.89.1 raw_out the same checker must report the
    console write reached from log_msg's locked region."""
    bad = console_calls_under_the_log_lock(OLD_LOG_C)
    assert ("log_msg", "raw_out()") in bad, bad


def test_the_checker_finds_the_old_defect_in_the_real_1_89_1_source():
    """The true source, when git has it: log.c as of 842d9db (agent 1.89.1)."""
    try:
        old = subprocess.run(
            ["git", "-C", str(ROOT), "show", "842d9db:agent/src/log.c"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        pytest.skip("git unavailable: %s" % e)
    if old.returncode != 0:
        pytest.skip("commit 842d9db not in this clone")
    bad = console_calls_under_the_log_lock(old.stdout)
    assert ("log_msg", "raw_out()") in bad, bad


def test_only_the_echo_thread_calls_into_the_console():
    """In log.c the console is touched by the echo thread and the helpers only
    it calls - so whatever the console does, it can stall that thread alone."""
    fns = functions(code_only(read("log.c")))
    direct = {f for f, b in fns.items() if CONSOLE_CALL.search(b)}
    assert direct, "the checker found no console call at all - it is broken"
    assert direct <= {"log_echo_thread", "echo_write", "console_quickedit_off"}, direct
    for helper in ("echo_write", "console_quickedit_off"):
        callers = {f for f, b in fns.items()
                   if f != helper and re.search(r"\b%s\s*\(" % helper, b)}
        assert callers == {"log_echo_thread"}, (helper, callers)
    assert "log_echo_thread" in console_touchers(fns)


def test_a_logger_only_copies_into_the_ring():
    fns = functions(code_only(read("log.c")))
    raw = fns["raw_out"]
    assert "echo_push_locked(" in raw, "the line must still reach the console, via the ring"
    push = fns["echo_push_locked"]
    assert "conring_push(" in push
    for waits in ("WaitFor", "Sleep(", "WriteFile(", "EnterCriticalSection("):
        assert waits not in push, "echo_push_locked must never wait: %s" % waits
    assert '#include "../shared/conring.h"' in read("log.c")


def test_the_echo_thread_runs_idle_and_lifts_itself_for_the_lock():
    body = functions(code_only(read("log.c")))["log_echo_thread"]
    assert "SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE)" in body
    assert body.index("log_lift()") < body.index(LOCK_ENTER), \
        "an IDLE thread preempted inside g_log_cs would hold every logger"
    assert body.index(LOCK_LEAVE) < body.index("log_unlift(")
    region = locked_regions(body)
    assert len(region) == 1 and "conring_take(" in region[0]
    # the console calls come after the lock is released
    leave = body.index(LOCK_LEAVE)
    for call in ("echo_write(", "SetConsoleTitleA("):
        assert body.index(call) > leave, call


def test_the_echo_thread_is_started_with_a_thread_id_and_falls_back_to_off():
    """Win9x rejects CreateThread with a NULL lpThreadId (error 87). If the
    thread cannot start (the 31 MB Deskpro), echo goes OFF - there is no
    non-blocking console write to fall back on, and inline is the defect."""
    body = functions(code_only(read("log.c")))["echo_launch"]
    call = re.search(r"CreateThread\(([^;]*)\)", body).group(1)
    assert "log_echo_thread" in call and call.rstrip().endswith("&tid"), call
    fail = body[body.index("if (!g_echo_thread)"):]
    assert "g_echo_on = 0" in fail
    assert not CONSOLE_CALL.search(fail)


def test_log_init_starts_the_echo_and_announces_it():
    fns = functions(code_only(read("log.c")))
    init = fns["log_init"]
    assert init.index("echo_prepare()") < init.index("raw_out(") < init.index("echo_launch()")
    src = read("log.c")
    # the line the hardware verifier looks for
    assert "log: console echo on its own thread" in src


def test_quickedit_is_cleared_on_nt_only_and_read_back():
    body = functions(code_only(read("log.c")))["console_quickedit_off"]
    ver = body.index("GetVersion() & 0x80000000")
    assert ver < body.index("SetConsoleMode("), "Win9x consoles have no QuickEdit"
    assert "STD_INPUT_HANDLE" in body
    assert "ENABLE_EXTENDED_FLAGS" in body and "~(DWORD)ENABLE_QUICK_EDIT_MODE" in body
    assert body.index("SetConsoleMode(") < body.rindex("GetConsoleMode("), \
        "report the post-condition, not the call"
    src = read("log.c")
    assert re.search(r"#define ENABLE_QUICK_EDIT_MODE\s+0x0040", src)
    assert re.search(r"#define ENABLE_EXTENDED_FLAGS\s+0x0080", src)


def test_shutdown_stops_the_echo_outside_the_lock_and_bounded():
    fns = functions(code_only(read("log.c")))
    sd = fns["log_shutdown"]
    assert sd.index("echo_stop()") < sd.index(LOCK_ENTER), \
        "the echo thread needs g_log_cs to drain: waiting for it under the lock deadlocks"
    stop = fns["echo_stop"]
    m = re.search(r"WaitForSingleObject\(g_echo_thread,\s*(\w+)\)", stop)
    assert m and m.group(1).isdigit(), "a frozen console must not hold up the exit"
    assert "CloseHandle(g_echo_evt)" not in code_only(read("log.c"))


def test_the_flusher_reports_a_console_that_stopped_answering():
    fns = functions(code_only(read("log.c")))
    assert "echo_stall_check()" in fns["log_flush_thread"]
    chk = fns["echo_stall_check"]
    assert "g_echo_busy_since" in chk and "ECHO_STALL_MS" in chk
    assert not CONSOLE_CALL.search(chk)


# ------------------------------------------------------ the rest of the agent
def test_no_other_agent_source_touches_the_console():
    """printf, puts, WriteConsole, SetConsoleTitle, Get/SetConsoleMode, a
    stdout/stderr stream or handle: all go through log.c's echo thread
    (con_printf / log_console_title). The accept loop's printf was one of
    these, on the thread that serves every new connection."""
    std_handle = re.compile(r"GetStdHandle\s*\(\s*STD_(?:OUTPUT|ERROR|INPUT)_HANDLE")
    offenders = []
    for path in sorted(SRC.glob("*.c")):
        if path.name == "log.c":
            continue
        code = code_only(path.read_text(errors="replace"))
        for rx in (CONSOLE_API_CALL, std_handle):
            for m in rx.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                offenders.append("%s:%d %s" % (path.name, line, m.group(0).strip()))
    assert not offenders, offenders


def test_console_printing_is_queued_not_written():
    h = code_only(read("log.h"))
    assert "void con_printf(" in h and "void log_console_title(" in h
    assert "if (!agent_console_quiet()) con_printf(" in h
    assert "CON_FLUSH" not in h, "there is no stdio buffer to flush any more"
    body = functions(code_only(read("log.c")))["con_printf"]
    assert "echo_push_locked(" in body
    assert not CONSOLE_CALL.search(body)
    main = read("main.c")
    assert "log_console_title(title)" in code_only(main)
    # the accept loop: the line that used to be a bare printf on the thread
    # that serves every new connection
    assert 'con_printf("Connection from %s:%d\\n"' in main


def test_a_busy_stamp_is_never_subtracted_raw():
    """agent 1.91.0 (agent/shared/busytick.h): the stamp is GetTickCount()|1, so
    `GetTickCount() - since` reads 1 ms in the FUTURE inside one tick -
    0xFFFFFFFF, "a console write returned after 4294967 s", which echoed and fed
    itself until agent.log rotated its history away every few minutes (.124,
    2026-09-28). Every elapsed time from the stamp goes through
    busytick_elapsed()."""
    src = (SRC / "log.c").read_text(encoding="latin-1")
    assert "busytick_mark(GetTickCount())" in src
    assert src.count("busytick_elapsed(GetTickCount(), since)") == 2
    assert not re.search(r"GetTickCount\(\)\s*-\s*since\b", src), \
        "a raw subtraction from the busy stamp is back"
