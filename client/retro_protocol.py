"""
retro_protocol.py - TCP protocol client for communicating with retro_agent.

Handles frame I/O, authentication, and command sending.
Protocol: [uint32 LE length][payload] for both directions.
Response: [status_byte][data] where status 0x00=text, 0x01=binary, 0xFF=error.
"""

import asyncio
import socket
import struct
import logging

logger = logging.getLogger("retro_protocol")

RESP_OK_TEXT = 0x00
RESP_OK_BINARY = 0x01
RESP_ERROR = 0xFF

MAX_FRAME_SIZE = 32 * 1024 * 1024  # 32MB


class RetroProtocolError(Exception):
    """Raised when the agent returns an error or protocol fails."""
    pass


class RetroConnection:
    """Async TCP connection to a single retro_agent instance."""

    def __init__(self, host: str, port: int = 9898):
        self.host = host
        self.port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._lock = asyncio.Lock()
        self.hostname: str = ""
        self.os_version: str = ""
        self.os_family: str = ""  # "windows", "linux", "mac_classic"
        # True once the current request's frame(s) have been handed to the
        # socket. Callers that must not repeat a command which may already
        # have run (a LOG_APPEND that timed out waiting for its "OK" has
        # usually been appended) read this after a failure: False means the
        # agent cannot have seen the command, so a retry is safe.
        self.request_sent: bool = False

    async def connect(self, secret: str, timeout: float = 10.0) -> str:
        """Connect and authenticate. Returns the agent's greeting."""
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port),
            timeout=timeout,
        )
        # Disable Nagle for low-latency small commands
        sock = self._writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # Send AUTH frame
        await self._send_frame(f"AUTH {secret}".encode("ascii"))
        # Read response
        status, data = await self._recv_response(timeout=timeout)
        if status == RESP_ERROR:
            raise RetroProtocolError(f"Auth failed: {data.decode('ascii', errors='replace')}")
        greeting = data.decode("ascii", errors="replace")
        # Parse "OK <hostname> <os> [os_family]"
        parts = greeting.split(None, 3)
        if len(parts) >= 2:
            self.hostname = parts[1]
        if len(parts) >= 3:
            self.os_version = parts[2]
        if len(parts) >= 4:
            self.os_family = parts[3]
        else:
            # Infer os_family from os_version for legacy agents
            if self.os_version.startswith("Win"):
                self.os_family = "windows"
            elif self.os_version.startswith("Linux"):
                self.os_family = "linux"
            elif self.os_version.startswith("Mac"):
                self.os_family = "mac_classic"
        return greeting

    async def close(self, graceful: bool = True, drain_timeout: float = 2.0):
        """Close the connection.

        When *graceful* is True (default) we half-close first with
        ``socket.shutdown(SHUT_WR)`` (a TCP FIN), then keep READING until the
        agent closes its side too (EOF), for at most *drain_timeout* seconds,
        and only then close the socket.

        Both halves matter on Win98, where an abrupt close can take the
        agent's Winsock (and the machine) down:

        * the FIN, instead of an RST, starts a clean 4-way close;
        * the drain: closing a socket that still has unread data in its
          receive buffer -- or that receives data after close() -- makes our
          kernel answer with an RST anyway. A reply the agent was still
          writing when we decided to leave (a slow Win9x agent answering a
          timed-out command, a long-poll that completes) used to land on a
          closed socket and draw exactly that RST. Reading until EOF lets the
          late reply arrive and be discarded, so the teardown stays FIN/FIN.
        """
        writer = self._writer
        if writer is None:
            return
        try:
            if graceful:
                sock = writer.get_extra_info("socket")
                if sock is not None:
                    try:
                        sock.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass  # Socket already broken
                await self._drain_until_eof(drain_timeout)
        finally:
            # Always release the socket, even when the drain is cancelled
            # (a shutdown that cannot wait any longer).
            self._writer = None
            self._reader = None
            try:
                writer.close()
            except Exception:
                pass
        try:
            await writer.wait_closed()
        except Exception:
            pass

    async def _drain_until_eof(self, timeout: float):
        """Discard incoming bytes until the peer closes (EOF) or *timeout*."""
        reader = self._reader
        if reader is None or timeout <= 0:
            return
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        try:
            while True:
                remaining = deadline - loop.time()
                if remaining <= 0:
                    return
                chunk = await asyncio.wait_for(reader.read(65536), timeout=remaining)
                if not chunk:
                    return  # EOF: the agent has closed its side too
        except (asyncio.TimeoutError, OSError, ConnectionError, RuntimeError):
            # RuntimeError: another coroutine is still mid-read on this
            # reader (a cancelled command). Nothing more to drain here.
            return

    @property
    def connected(self) -> bool:
        """True only while BOTH directions are usable.

        ``is_closing()`` alone stays False after the agent closes its end
        (the agent drops a silent client: 120 s on NT, 300 s on Win9x), so an
        idle connection kept reporting itself connected and the next command
        failed with "Connection lost". A reader at EOF means the agent has
        gone; the caller should reconnect before sending.
        """
        if self._writer is None or self._writer.is_closing():
            return False
        reader = self._reader
        if reader is not None and reader.at_eof():
            return False
        return True

    async def send_command(
        self, command: str, binary_payload: bytes | None = None, timeout: float = 60.0
    ) -> tuple[int, bytes]:
        """
        Send a command and return (status, data).
        If binary_payload is provided (for UPLOAD), it's sent as a second frame.
        Returns (RESP_OK_TEXT | RESP_OK_BINARY | RESP_ERROR, response_bytes).
        """
        async with self._lock:
            self.request_sent = False
            await self._send_frame(command.encode("ascii"))
            if binary_payload is not None:
                await self._send_frame(binary_payload)
            self.request_sent = True
            return await self._recv_response(timeout=timeout)

    async def command_text(self, command: str, timeout: float = 60.0) -> str:
        """Send command, expect text response. Raises on error."""
        status, data = await self.send_command(command, timeout=timeout)
        text = data.decode("ascii", errors="replace")
        if status == RESP_ERROR:
            raise RetroProtocolError(text)
        return text

    async def command_binary(self, command: str, timeout: float = 120.0) -> bytes:
        """Send command, expect binary response. Raises on error."""
        status, data = await self.send_command(command, timeout=timeout)
        if status == RESP_ERROR:
            raise RetroProtocolError(data.decode("ascii", errors="replace"))
        return data

    async def monitor_stream(self, interval_ms: int = 1000, ticks: int = 60,
                             procname: str = "", frame_timeout: float | None = None):
        """MONITOR streaming (agent v1.15+): async-yields one status line per
        tick until the agent sends the final 'END ticks=N' line (yielded too).
        Holds the connection lock for the whole stream — use a dedicated
        connection for monitoring."""
        if frame_timeout is None:
            frame_timeout = max(10.0, interval_ms / 1000.0 * 5)
        cmd = f"MONITOR {interval_ms} {ticks}"
        if procname:
            cmd += f" {procname}"
        async with self._lock:
            self.request_sent = False
            await self._send_frame(cmd.encode("ascii"))
            self.request_sent = True
            while True:
                status, data = await self._recv_response(timeout=frame_timeout)
                text = data.decode("ascii", errors="replace")
                if status == RESP_ERROR:
                    raise RetroProtocolError(text)
                yield text
                if text.startswith("END "):
                    return

    async def _send_frame(self, payload: bytes):
        """Send a length-prefixed frame."""
        header = struct.pack("<I", len(payload))
        self._writer.write(header + payload)
        await self._writer.drain()

    async def _recv_response(self, timeout: float = 60.0) -> tuple[int, bytes]:
        """Receive a framed response. Returns (status_byte, data_bytes)."""
        header = await asyncio.wait_for(
            self._reader.readexactly(4), timeout=timeout
        )
        (payload_len,) = struct.unpack("<I", header)
        if payload_len > MAX_FRAME_SIZE:
            raise RetroProtocolError(f"Frame too large: {payload_len}")
        if payload_len == 0:
            return (RESP_OK_TEXT, b"")

        payload = await asyncio.wait_for(
            self._reader.readexactly(payload_len), timeout=timeout
        )
        status = payload[0]
        data = payload[1:]
        return (status, data)
