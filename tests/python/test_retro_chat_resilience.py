"""retro_chat.exe survives a wedged or restarting agent, and is cheap to type in
(retro-chat 0.16.0, from the 2026-09-24 chat-path and CPU-cost reviews).

Fix 8  frame_recv() had no timeout and PROMPT_PUSH ran on the input thread,
       so an agent that accepted but never answered froze typing - even
       :quit - for good; and when the one reconnect failed (the agent
       restarting for an auto-update) the prompt was dropped with the
       spinner turning forever.
Fix 9  every frame was two send() calls with Nagle on: up to ~200 ms of
       delayed-ACK stall on every poll.
Fix 5  the client tracks the agent's ABSOLUTE log end (agent >= 1.85.0), so
       a ring truncation can no longer send it back to 0 to reprint 128 KB.
Fix 10 PROMPT_PUSH's "replaced" / "no-listener" flags are shown.
11a    typing at the end of the line writes ONE character, not a full
       erase + redraw (~12 console calls per key through Win9x's 16-bit
       console).
11b    the spinner thread sleeps on an event while idle instead of waking
       4x a second forever.

The frame I/O is compiled out of retro_chat.c and RUN against the fake
Winsock in tests/native/stubs/netfake_env.h; the rest are source invariants
(the UI loop needs a real console).
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "tools" / "retro_chat.c"
NATIVE = REPO / "tests" / "native"


def _text():
    return SRC.read_text(errors="replace")


def _strip_comments(code):
    out, i, n = [], 0, len(code)
    while i < n:
        if code.startswith("/*", i):
            j = code.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif code[i] == '"':
            j = i + 1
            while j < n and code[j] != '"':
                j += 2 if code[j] == "\\" else 1
            out.append(code[i:j + 1])
            i = j + 1
        else:
            out.append(code[i])
            i += 1
    return "".join(out)


def _fn(text, name):
    m = re.search(r"\b%s\s*\([^;{)]*\)\s*\n?\{" % re.escape(name), text)
    assert m, "function %s not found in retro_chat.c" % name
    depth, i = 0, text.index("{", m.start())
    start = text.rfind("\n", 0, m.start()) + 1
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1


# ---------------------------------------------------------------- frame I/O

HARNESS = r'''
#include "munit.h"
#include "stubs/netfake_env.h"
%(fns)s

enum { S = 3 };

TEST(a_reply_that_never_comes_times_out) {
    char *buf = NULL; DWORD len = 0, t0 = fake_now;
    fake_sock_reset();
    CHECK_EQ_I(frame_recv(S, &buf, &len, 40000), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);           /* old frame_recv: 1 */
    CHECK_EQ_U(fake_now - t0, 40000);
}

TEST(a_half_reply_times_out_on_the_same_budget) {
    char *buf = NULL; DWORD len = 0, t0 = fake_now;
    fake_sock_reset();
    fake_sock_feed(S, "\x09\x00\x00\x00\x00PO", 7);  /* 9-byte frame, 3 sent */
    CHECK_EQ_I(frame_recv(S, &buf, &len, 15000), -1);
    CHECK_EQ_I(fake_blocked_forever, 0);
    CHECK_EQ_U(fake_now - t0, 15000);
}

TEST(a_command_is_one_send) {
    fake_sock_reset();
    CHECK_EQ_I(frame_send(S, "LOG_WAIT 0 30000", 16), 0);
    CHECK_EQ_I(fake_socks[S].send_calls, 1);        /* old frame_send: 2 */
    CHECK(memcmp(fake_socks[S].out, "\x10\x00\x00\x00LOG_WAIT 0 30000", 20) == 0,
          "header + payload");
    fake_sock_reset();
    {
        static char big[4000];
        memset(big, 'p', sizeof(big));
        CHECK_EQ_I(frame_send(S, big, sizeof(big)), 0);
        CHECK_EQ_I(fake_socks[S].send_calls, 1);    /* heap path, still one */
        CHECK_EQ_I(fake_socks[S].out_len, 4004);
    }
}

TEST(agent_command_tells_an_error_reply_from_a_dead_connection) {
    char *text = NULL;
    fake_sock_reset();
    fake_sock_feed(S, "\x12\x00\x00\x00\xffPrompt too long!!", 22);
    CHECK_EQ_I(agent_command(S, "PROMPT_PUSH x", &text, NULL, 15000), -2);
    CHECK(text && strcmp(text, "Prompt too long!!") == 0, "error text returned");
    free(text);
    fake_sock_reset();
    CHECK_EQ_I(agent_command(S, "PROMPT_PUSH x", &text, NULL, 15000), -1);
    CHECK(text == NULL, "no reply: connection failure, no text");
    fake_sock_reset();
    fake_sock_feed(S, "\x03\x00\x00\x00\x00OK", 7);
    CHECK_EQ_I(agent_command(S, "PROMPT_PUSH x", &text, NULL, 15000), 0);
    CHECK(text && strcmp(text, "OK") == 0, "success");
    free(text);
    CHECK_EQ_I(agent_command(INVALID_SOCKET, "PING", &text, NULL, 15000), -1);
}

MUNIT_MAIN("retro_chat frame I/O (extracted true source)", {
    RUN(a_reply_that_never_comes_times_out);
    RUN(a_half_reply_times_out_on_the_same_budget);
    RUN(a_command_is_one_send);
    RUN(agent_command_tells_an_error_reply_from_a_dead_connection);
})
'''


def test_frame_io_is_bounded_and_coalesced(tmp_path):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - retro_chat frame I/O NOT exercised")
    text = _text()
    fns = "\n".join(_fn(text, n) for n in
                    ("frame_send", "recv_by", "frame_recv", "agent_command"))
    src = tmp_path / "t.c"
    src.write_text(HARNESS % {"fns": fns})
    exe = tmp_path / "t"
    r = subprocess.run([cc, "-std=c11", "-Wall", "-I", str(NATIVE),
                        "-I", str(NATIVE / "stubs"), str(src), "-o", str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


# ------------------------------------------------------- source invariants

def test_every_reply_wait_is_bounded():
    text = _strip_comments(_text())
    # every agent_command call now names its timeout (the 4-argument form
    # would not compile); none may pass an unbounded one
    calls = re.findall(r"agent_command\([^;]*?\);", text)
    assert calls and all("_MS" in c for c in calls), calls
    assert "INFINITE)" not in re.sub(r"WaitForSingleObject\([^)]*\)", "", text)
    assert re.search(r"frame_recv\(s, &resp, &resp_len, AUTH_TIMEOUT_MS\)",
                     _fn(text, "agent_connect")), "AUTH reply must be bounded"
    for fn in ("wait_thread", "status_thread"):
        assert "WAIT_TIMEOUT_MS + POLL_SLACK_MS" in _fn(text, fn), (
            "%s: a long-poll reply is due at its own timeout + slack" % fn)


def test_nagle_is_off():
    body = _fn(_strip_comments(_text()), "agent_connect")
    assert re.search(r"setsockopt\(s,\s*IPPROTO_TCP,\s*TCP_NODELAY", body)


def test_prompt_push_is_retried_then_reported_never_dropped():
    text = _strip_comments(_text())
    push = _fn(text, "push_prompt")
    assert "agent_command(*ps, cmd, &reply, NULL, CMD_TIMEOUT_MS)" in push
    assert "PUSH_RETRY_MS" in push and "agent_connect_within(" in push, (
        "a failed push must keep reconnecting for PUSH_RETRY_MS")
    assert 'print_notice("[prompt NOT delivered - agent unreachable]", 1)' in push, (
        "an undeliverable prompt must SAY so and stop the spinner")
    assert "rc == -2" in push, "an agent refusal is reported, not retried"
    main = _fn(text, "main")
    assert "push_prompt(&s, saved_prompt)" in main
    assert "PROMPT_PUSH" not in main, "the push must go through push_prompt"
    within = _fn(text, "agent_connect_within")
    assert "give_up" in within and "return INVALID_SOCKET" in within


def test_log_offset_tracks_the_absolute_end():
    body = _strip_comments(_fn(_text(), "wait_thread"))
    assert "g_log_offset = total_size;" in body
    assert "g_log_offset += body_len" not in body, (
        "offset += bytes lands short of the end after the agent skips "
        "dropped bytes, and the same text is printed twice")
    assert re.search(r"if \(total_size < g_log_offset\)\s*\{\s*g_log_offset = 0;",
                     body), "a smaller total still means cleared/restarted"


def test_push_reply_flags_are_shown():
    body = _strip_comments(_fn(_text(), "show_push_reply"))
    assert '"replaced"' in body and '"no-listener "' in body


def test_typing_at_the_end_writes_one_character():
    main = _strip_comments(_fn(_text(), "main"))
    branch = main.split("ch >= 32 && ch < 127", 1)[1].split("} else if", 1)[0]
    assert "WriteConsoleA(g_hOut, &ch, 1, &w, NULL)" in branch, (
        "an appended character must be echoed on its own")
    assert "g_input_cursor == g_input_len" in branch
    assert "g_screen_w - 3" in branch, (
        "the fast path must stop where draw_input_area starts scrolling")
    assert "EnterCriticalSection(&g_console_cs)" in branch, (
        "insert + echo under the console lock, or a concurrent redraw can "
        "draw the character twice")
    set_color = _fn(_text(), "set_color")
    assert "g_cur_color" in set_color, "redundant attribute changes skipped"


def test_idle_spinner_sleeps_on_an_event():
    text = _strip_comments(_text())
    body = _fn(text, "spinner_thread")
    assert "WaitForSingleObject(g_spin_event, INFINITE)" in body
    assert "Sleep(250)" not in body.replace(
        "else\n                Sleep(250);", ""), (
        "the only 250 ms sleep left is the no-event fallback")
    assert "SetEvent(g_spin_event)" in _fn(text, "start_waiting")
    main = _fn(text, "main")
    assert "start_waiting()" in main, "sending a prompt must wake the spinner"
    tail = main.split("g_running = 0;")[-1]
    assert "SetEvent(g_spin_event)" in tail, "exit must wake it too"
