"""Every CreateThread in the agent must pass a real lpThreadId.

Windows 95/98 reject CreateThread(..., lpThreadId = NULL) with
ERROR_INVALID_PARAMETER (87); NT accepts it. So a NULL there is a thread that
silently never starts on exactly the machines nothing else can supervise.

It has bitten twice:
  * spawn_helper() - the autoupdate / retrowall / watchdog / dosstage /
    sharelog threads never ran on 9x (CLAUDE.md, "A STATIC IMPORT WIN9x LACKS").
  * do_system_power() - REBOOT and SHUTDOWN answered "OK" and did nothing on
    every Win9x box; the log said "FAILED to start the shutdown thread".
    Found on .243 (Win98 SE) 2026-09-24 when safe-reboot.py reported
    "rebooting: OK" and the uptime kept counting.

Run: pytest tests/python/test_agent_createthread_win9x.py
"""
import re
from pathlib import Path

AGENT = Path(__file__).resolve().parents[2] / "agent"
CALL = re.compile(r"\bCreateThread\s*\(", re.S)


def _calls():
    for f in sorted(list(AGENT.glob("src/*.c")) + list(AGENT.glob("shared/*.[ch]"))):
        src = f.read_text(errors="replace")
        for m in CALL.finditer(src):
            depth, i = 1, m.end()
            while depth and i < len(src):
                depth += {"(": 1, ")": -1}.get(src[i], 0)
                i += 1
            args = src[m.end():i - 1]
            line = src.count("\n", 0, m.start()) + 1
            yield f"{f.relative_to(AGENT)}:{line}", args


def _last_arg(args):
    depth, cur, parts = 0, "", []
    for ch in args:
        if ch in "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur); cur = ""
        else:
            cur += ch
    parts.append(cur)
    return " ".join(parts[-1].split())


def test_there_are_calls_to_check():
    assert len(list(_calls())) >= 6


def test_no_createthread_passes_a_null_thread_id():
    bad = [where for where, args in _calls()
           if _last_arg(args) in ("NULL", "0", "(LPDWORD)NULL", "(DWORD *)NULL")]
    assert not bad, ("CreateThread with a NULL lpThreadId fails on Win95/98 "
                     "(error 87): " + ", ".join(bad))


def test_power_commands_do_not_say_ok_before_the_thread_exists():
    src = (AGENT / "src" / "handlers.c").read_text()
    body = src[src.index("static void do_system_power("):]
    body = body[:body.index("\n}\n")]
    assert body.index("CreateThread(") < body.index('send_text_response(sock, "OK")'), \
        "REBOOT answers OK before it knows the shutdown thread started"
    assert "CREATE_SUSPENDED" in body and "ResumeThread(th)" in body
