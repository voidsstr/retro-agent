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

    async def close(self):
        """Close the connection."""
        if self._writer:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
            self._writer = None
            self._reader = None

    @property
    def connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    async def send_command(
        self, command: str, binary_payload: bytes | None = None, timeout: float = 60.0
    ) -> tuple[int, bytes]:
        """
        Send a command and return (status, data).
        If binary_payload is provided (for UPLOAD), it's sent as a second frame.
        Returns (RESP_OK_TEXT | RESP_OK_BINARY | RESP_ERROR, response_bytes).
        """
        async with self._lock:
            await self._send_frame(command.encode("ascii"))
            if binary_payload is not None:
                await self._send_frame(binary_payload)
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
