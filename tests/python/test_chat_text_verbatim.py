"""Chat text reaches the agent VERBATIM, and LOG_APPEND2 exists (agent 1.85.0).

handle_command() used to space-trim every command's arguments. For the chat
bus that destroyed text: the brain streams a reply as LOG_APPEND chunks split
wherever it flushes, so a chunk that began with the space between two words
lost it and the words were glued together on screen, and every indented line
of code came out flush left. LOG_APPEND, LOG_APPEND2, PROMPT_PUSH and
STATUS_SET now take everything after exactly ONE separator space; every other
command is still trimmed as before.

cmd_args_of() / cmd_takes_raw_text() are compiled out of handlers.c (and
str_skip_spaces() out of util.c) and exercised, not grepped.

The host daemon falls back from LOG_APPEND2 to LOG_APPEND on an agent that
answers "Unknown command", so that error text is pinned too.
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
def split(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - cmd_args_of NOT exercised")
    h = _read("handlers.c")
    code = "\n".join([
        "#include <stdio.h>",
        "#include <string.h>",
        "#include <strings.h>",
        "#define _stricmp strcasecmp",
        _extract(_read("util.c"), "const char *str_skip_spaces("),
        _extract(h, "static int cmd_takes_raw_text("),
        _extract(h, "static const char *cmd_args_of("),
        r'''
int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        const char *cmd = argv[i];
        char name[32];
        const char *a;
        int n;
        for (n = 0; cmd[n] && cmd[n] != ' ' && n < 31; n++) name[n] = cmd[n];
        name[n] = 0;
        a = cmd_args_of(cmd, n, name);
        if (a) printf("[%s]\n", a); else printf("NULL\n");
    }
    return 0;
}
'''])
    d = tmp_path_factory.mktemp("cmdargs")
    (d / "t.c").write_text(code)
    subprocess.run([cc, "-std=gnu11", "-o", str(d / "t"), str(d / "t.c")], check=True)

    def run(*cmds):
        out = subprocess.run([str(d / "t"), *cmds], capture_output=True, text=True,
                             check=True)
        return out.stdout.splitlines()
    return run


def test_chat_text_keeps_its_leading_blanks(split):
    assert split("LOG_APPEND  words glued",
                 "LOG_APPEND2 7     indented code",
                 "PROMPT_PUSH   spaced question",
                 "STATUS_SET  EXEC dir") == [
        "[ words glued]",           # the space between two chunks survives
        "[7     indented code]",    # LOG_APPEND2 parses its id itself
        "[  spaced question]",
        "[ EXEC dir]",
    ]


def test_chat_command_names_are_case_insensitive_like_dispatch(split):
    assert split("log_append  x", "Status_Set  y") == ["[ x]", "[ y]"]


def test_every_other_command_is_still_trimmed(split):
    # EXEC, REGREAD, ... have always had their arguments trimmed and scripts
    # rely on it; only the four chat text commands changed.
    assert split("EXEC   dir C:\\", "LOG_READ   12", "LOG_WAIT  5 1000",
                 "PROMPT_WAIT   30000") == [
        "[dir C:\\]", "[12]", "[5 1000]", "[30000]"]


def test_no_separator_means_no_arguments(split):
    assert split("LOG_APPEND", "LOG_APPEND ", "PING") == ["NULL", "[]", "NULL"]


def test_dispatch_uses_the_splitter_and_registers_log_append2():
    h = _read("handlers.c")
    body = _extract(h, "void handle_command(")
    assert "cmd_args_of(cmd, i, cmd_name)" in body, (
        "handle_command must split arguments through cmd_args_of")
    assert "str_skip_spaces(cmd + i + 1)" not in body, (
        "the old unconditional trim is back")
    assert re.search(r'\{\s*"LOG_APPEND2"\s*,\s*1\s*,\s*NULL\s*,\s*handle_log_append2\s*,\s*0\s*\}', h)
    assert "void handle_log_append2(SOCKET sock, const char *args);" in _read("handlers.h")


def test_unknown_command_error_is_what_the_daemon_falls_back_on():
    body = _extract(_read("handlers.c"), "void handle_command(")
    assert 'send_error_response(sock, "Unknown command")' in body, (
        "the daemon recognises an agent without LOG_APPEND2 by this error and "
        "falls back to LOG_APPEND - do not reword it")
    dc = (REPO / "agent" / "doschat" / "doschat.cpp").read_text(errors="replace")
    assert 'resp_err(s, "Unknown command (DOS agent subset)")' in dc
