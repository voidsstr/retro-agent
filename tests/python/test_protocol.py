"""
Regression tests for the length-prefixed frame codec (client/retro_protocol.py).

Wire format (contract with the C agent in agent/src/):
  frame  = <uint32 little-endian payload_len> <payload bytes>
  response payload = <status byte> <data...>   (status: 0x00 text, 0x01 binary, 0xFF error)
  payload_len == 0 is a valid empty-OK response.
  payload_len > MAX_FRAME_SIZE (32 MiB) is rejected before allocation.

Exercised via the real RetroConnection methods against fake asyncio streams, so
a change to the header size, endianness, status-byte position, or the size guard
breaks these. The Win98 agent shares this exact framing — treat failures as
"you changed the protocol the fleet speaks".
"""
import os
import struct
import sys
import asyncio
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from client import retro_protocol
from client.retro_protocol import (
    RetroConnection, RetroProtocolError,
    RESP_OK_TEXT, RESP_OK_BINARY, RESP_ERROR, MAX_FRAME_SIZE,
)


class FakeWriter:
    """Captures everything written; satisfies the StreamWriter surface used."""
    def __init__(self):
        self.buf = bytearray()
        self._closing = False

    def write(self, data):
        self.buf.extend(data)

    async def drain(self):
        pass

    def is_closing(self):
        return self._closing


class FakeReader:
    """Feeds a preloaded byte buffer through readexactly()."""
    def __init__(self, data: bytes = b""):
        self.buf = bytearray(data)

    def feed(self, data: bytes):
        self.buf.extend(data)

    async def readexactly(self, n: int) -> bytes:
        if len(self.buf) < n:
            raise asyncio.IncompleteReadError(bytes(self.buf), n)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out


def make_conn(reader_data: bytes = b""):
    c = RetroConnection("127.0.0.1", 9898)
    c._reader = FakeReader(reader_data)
    c._writer = FakeWriter()
    return c


def frame(payload: bytes) -> bytes:
    """Build a wire frame the way the agent would."""
    return struct.pack("<I", len(payload)) + payload


# ---------------------------------------------------------------------------
# _send_frame — outbound framing
# ---------------------------------------------------------------------------

def test_send_frame_prefixes_le_uint32_length():
    c = make_conn()
    asyncio.run(c._send_frame(b"PING"))
    assert bytes(c._writer.buf) == struct.pack("<I", 4) + b"PING"


def test_send_frame_empty_payload():
    c = make_conn()
    asyncio.run(c._send_frame(b""))
    assert bytes(c._writer.buf) == struct.pack("<I", 0)


# ---------------------------------------------------------------------------
# _recv_response — inbound framing + status split
# ---------------------------------------------------------------------------

def test_recv_text_response_splits_status_and_data():
    payload = bytes([RESP_OK_TEXT]) + b"PONG"
    c = make_conn(frame(payload))
    status, data = asyncio.run(c._recv_response())
    assert status == RESP_OK_TEXT
    assert data == b"PONG"


def test_recv_binary_response():
    payload = bytes([RESP_OK_BINARY]) + b"\x00\x01\x02BMP"
    c = make_conn(frame(payload))
    status, data = asyncio.run(c._recv_response())
    assert status == RESP_OK_BINARY
    assert data == b"\x00\x01\x02BMP"


def test_recv_zero_length_frame_is_empty_ok():
    c = make_conn(struct.pack("<I", 0))
    status, data = asyncio.run(c._recv_response())
    assert status == RESP_OK_TEXT
    assert data == b""


def test_recv_rejects_oversize_frame_before_reading_body():
    # header claims > 32 MiB; must raise on the header alone (no body supplied)
    c = make_conn(struct.pack("<I", MAX_FRAME_SIZE + 1))
    with pytest.raises(RetroProtocolError):
        asyncio.run(c._recv_response())


def test_recv_at_max_frame_size_boundary_is_allowed():
    # exactly MAX_FRAME_SIZE is allowed (guard is strictly greater-than)
    c = RetroConnection("127.0.0.1", 9898)
    c._writer = FakeWriter()
    body = bytes([RESP_OK_BINARY]) + b"\x00" * (MAX_FRAME_SIZE - 1)
    c._reader = FakeReader(struct.pack("<I", MAX_FRAME_SIZE) + body)
    status, data = asyncio.run(c._recv_response())
    assert status == RESP_OK_BINARY
    assert len(data) == MAX_FRAME_SIZE - 1


# ---------------------------------------------------------------------------
# command_text / command_binary — the error-status contract
# ---------------------------------------------------------------------------

def test_command_text_raises_on_error_status():
    c = make_conn(frame(bytes([RESP_ERROR]) + b"Cannot open key"))
    with pytest.raises(RetroProtocolError, match="Cannot open key"):
        asyncio.run(c.command_text("REGREAD ..."))


def test_command_text_returns_decoded_text_on_ok():
    c = make_conn(frame(bytes([RESP_OK_TEXT]) + b"hello"))
    assert asyncio.run(c.command_text("PING")) == "hello"


def test_command_binary_raises_on_error_status():
    c = make_conn(frame(bytes([RESP_ERROR]) + b"no such file"))
    with pytest.raises(RetroProtocolError, match="no such file"):
        asyncio.run(c.command_binary("DOWNLOAD X"))


def test_send_command_writes_command_then_binary_payload():
    # UPLOAD sends two frames: the command, then the binary payload.
    c = make_conn(frame(bytes([RESP_OK_TEXT]) + b"ok"))
    asyncio.run(c.send_command("UPLOAD C:\\x", binary_payload=b"\xDE\xAD"))
    expected = frame(b"UPLOAD C:\\x") + frame(b"\xDE\xAD")
    assert bytes(c._writer.buf) == expected
