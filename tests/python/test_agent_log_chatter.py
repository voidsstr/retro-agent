"""The chat long-polls must not flush the agent log (agent 1.84.3).

On Win9x a long-poll is clamped to 1s, so a local retro_chat wrote a CMD line
and two frame lines every second per poller. The 512 KB agent.log /
agent.log.1 pair rotated in about two hours: on .243 (2026-09-24) the whole
boot - PCIRESCUE, auto-update, GAMESYNC - was gone by the time anyone looked.

Now a long-poll is logged once per connection (so the log still shows who is
polling, which is how a second chat stack stealing prompts was found), and a
frame line is written only for a transfer (>= 1 KB).

cmd_is_longpoll() is compiled from main.c and exercised, not grepped.

Run: pytest tests/python/test_agent_log_chatter.py
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"


def _read(name):
    return (SRC / name).read_text(errors="replace")


def _extract(src, signature):
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


@pytest.fixture(scope="module")
def longpoll(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - cmd_is_longpoll NOT exercised")
    fn = _extract(_read("main.c"), "static int cmd_is_longpoll(")
    d = tmp_path_factory.mktemp("longpoll")
    (d / "t.c").write_text(
        "#include <string.h>\n#include <strings.h>\n#include <stdio.h>\n"
        "typedef unsigned long DWORD;\n#define _strnicmp strncasecmp\n"
        + fn +
        "\nint main(int argc, char **argv) { int i; for (i = 1; i < argc; i++)"
        " printf(\"%d\\n\", cmd_is_longpoll(argv[i], (DWORD)strlen(argv[i]))); return 0; }\n")
    subprocess.run([cc, "-o", str(d / "t"), str(d / "t.c")], check=True)

    def run(*cmds):
        out = subprocess.run([str(d / "t"), *cmds], capture_output=True, text=True, check=True)
        return [int(x) for x in out.stdout.split()]
    return run


def test_the_three_chat_polls_are_recognised(longpoll):
    assert longpoll("LOG_WAIT 39 30000", "STATUS_WAIT 0 30000", "PROMPT_WAIT 30000",
                    "PROMPT_WAIT", "log_wait 1 1000") == [1, 1, 1, 1, 1]


def test_nothing_else_is_silenced(longpoll):
    # a mutating command must never be mistaken for chatter - box-owner.py and
    # every post-mortem read these lines
    assert longpoll("EXEC cmd /c dir", "PING", "LOG_WAITX 1", "GAMESYNC START",
                    "PCIRESCAN", "LOG", "STATUS", "REBOOT") == [0] * 8


def test_both_command_log_sites_log_a_poll_once_per_connection():
    s = _read("main.c")
    assert "int    poll_logged;" in s
    assert "cl->poll_logged = 0;" in s, "reset when a connection authenticates"
    assert s.count("cmd_is_longpoll(") == 3, "the definition plus both log sites"
    assert s.count("further long-polls on this connection not logged") == 2, (
        "the first poll must say its repeats are suppressed, or their absence "
        "reads as the client having gone away")
    for site in ("if (!poll || !cl->poll_logged)", "if (!poll || !poll_logged)"):
        assert site in s


def test_frame_lines_only_for_transfers():
    s = _read("protocol.c")
    assert re.search(r"#define PROTO_LOG_MIN\s+1024", s)
    assert 'if (payload_len >= PROTO_LOG_MIN)\n        log_msg(LOG_PROTO, "frame_recv:' in s
    assert 'if (len >= PROTO_LOG_MIN)\n        log_msg(LOG_PROTO, "frame_send:' in s
    # a framing ERROR is still always logged
    assert 'log_msg(LOG_PROTO, "frame_recv: bad header' in s
