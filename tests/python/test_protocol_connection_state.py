"""RetroConnection must tell the truth about a connection, and leave with a FIN.

Three properties of client/retro_protocol.py (mirrored in nsc-assistant's
shared/retro_protocol.py, which the chat daemon and dashboard import):

  * `connected` must be False once the agent has closed its end. The agent
    drops a client silent for 120 s (NT) / 300 s (Win9x); `is_closing()` alone
    stayed False, so the chat daemon's idle send connection looked healthy and
    the next STATUS_SET failed with "Connection lost" (~5 of 9 prompts in the
    2026-09 journal) — the "thinking..." line simply never appeared.
  * `request_sent` says whether a failed command ever left: a LOG_APPEND that
    timed out waiting for "OK" has usually been appended, so the daemon may
    retry only what the agent cannot have seen.
  * `close()` must read until the agent's own FIN (bounded) before closing. A
    late reply landing on a closed socket makes our kernel answer with an
    RST, and an RST can take a Win98 box's Winsock down.

Real sockets on 127.0.0.1 — no fleet access.

Run: pytest tests/python/test_protocol_connection_state.py
"""

import asyncio
import os
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from client.retro_protocol import RetroConnection  # noqa: E402

_REPO = Path(__file__).resolve().parent.parent.parent
_NSC = Path(os.environ.get("NSC_ASSISTANT_DIR",
                           str(Path.home() / "development" / "nsc-assistant")))


def frame(status, text):
    p = bytes([status]) + text.encode()
    return struct.pack("<I", len(p)) + p


async def serve(handler):
    server = await asyncio.start_server(handler, "127.0.0.1", 0)
    return server, server.sockets[0].getsockname()[1]


async def _auth(reader, writer):
    hdr = await reader.readexactly(4)
    await reader.readexactly(struct.unpack("<I", hdr)[0])
    writer.write(frame(0, "OK FAKE Win4.10"))
    await writer.drain()


def run(coro):
    return asyncio.run(asyncio.wait_for(coro, 20))


def test_connected_turns_false_when_the_agent_closes_its_end():
    async def go():
        async def agent(reader, writer):
            await _auth(reader, writer)
            await asyncio.sleep(0.1)
            writer.close()                        # the agent's idle drop (FIN)

        server, port = await serve(agent)
        c = RetroConnection("127.0.0.1", port)
        await c.connect("s", timeout=5)
        assert c.connected
        await asyncio.sleep(0.4)
        assert not c.connected, "a connection the agent closed still reads as connected"
        assert c.os_version == "Win4.10"
        await c.close()
        server.close()

    run(go())


def test_request_sent_distinguishes_never_left_from_no_reply():
    async def go():
        async def silent_agent(reader, writer):
            await _auth(reader, writer)
            await reader.read(100)               # takes the command, never answers
            await asyncio.sleep(5)

        server, port = await serve(silent_agent)
        c = RetroConnection("127.0.0.1", port)
        await c.connect("s", timeout=5)
        with pytest.raises(asyncio.TimeoutError):
            await c.send_command("LOG_APPEND hello", timeout=0.3)
        assert c.request_sent, "the command DID leave; a retry would duplicate it"

        c2 = RetroConnection("127.0.0.1", port)
        await c2.connect("s", timeout=5)
        c2._writer.transport.abort()             # the connection is gone before we send
        await asyncio.sleep(0.05)
        with pytest.raises(Exception):
            await c2.send_command("LOG_APPEND hello", timeout=0.3)
        assert not c2.request_sent, "a command that never left must be retryable"
        await c.close(drain_timeout=0.2)
        server.close()

    run(go())


def test_close_drains_a_late_reply_so_the_agent_never_sees_an_rst():
    """The agent answers AFTER we decided to leave (a slow Win9x agent, a
    long-poll completing). With the old close (FIN, sleep 0.1 s, close) the
    late bytes met a closed socket and the agent got an RST."""
    async def go():
        outcome = {}

        async def slow_agent(reader, writer):
            await _auth(reader, writer)
            try:
                await reader.read()                          # our FIN
                await asyncio.sleep(0.3)
                writer.write(frame(0, "late reply"))         # arrives after we left
                await writer.drain()
                await asyncio.sleep(0.2)
                writer.write(frame(0, "and more"))
                await writer.drain()
                outcome["end"] = "clean"
            except (ConnectionResetError, BrokenPipeError) as e:
                outcome["end"] = f"reset: {e!r}"
            finally:
                writer.close()

        server, port = await serve(slow_agent)
        c = RetroConnection("127.0.0.1", port)
        await c.connect("s", timeout=5)
        await c.close()
        await asyncio.sleep(0.2)
        server.close()
        return outcome

    assert run(go()) == {"end": "clean"}


def test_close_is_bounded_when_the_agent_never_closes():
    async def go():
        async def stubborn(reader, writer):
            await _auth(reader, writer)
            await asyncio.sleep(10)

        server, port = await serve(stubborn)
        c = RetroConnection("127.0.0.1", port)
        await c.connect("s", timeout=5)
        loop = asyncio.get_running_loop()
        t0 = loop.time()
        await c.close(drain_timeout=0.5)
        took = loop.time() - t0
        server.close()
        return took

    assert run(go()) < 1.5


def test_the_nsc_assistant_copy_is_identical():
    """CLAUDE.md: 'When modifying the protocol, update both repos'. The chat
    daemon imports the nsc-assistant copy, so a fix made only here never
    reaches it."""
    theirs = _NSC / "shared" / "retro_protocol.py"
    if not theirs.is_file():
        pytest.skip(f"nsc-assistant not checked out at {_NSC}")
    ours = _REPO / "client" / "retro_protocol.py"
    assert ours.read_text() == theirs.read_text(), \
        f"{ours} and {theirs} have drifted apart"
