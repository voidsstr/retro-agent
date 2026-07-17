"""Fleet AI helpers over the retro agent protocol (agent v1.9.0+).

Typed wrappers for AI_HELLO / MODEL_LOAD / MODEL_LIST / MODEL_UNLOAD /
INFER_RUN / TENSOR / AI_RESTART, plus the TNSR tensor blob codec used for
activations and gradients (M4/M7 of docs/roadmap-fleet-ai.md).

Usage:
    from client.retro_protocol import RetroConnection
    from client.retro_ai import RetroAI

    conn = RetroConnection(ip, 9898); await conn.connect(secret)
    ai = RetroAI(conn)
    caps = await ai.hello()
    await ai.model_load("lenet", open("lenet5.rim", "rb").read())
    logits = await ai.infer_run("lenet", image_bytes)

NOTE: duplicated into nsc-assistant/shared/ when the protocol changes.
"""
from __future__ import annotations

import json
import struct
from dataclasses import dataclass

import numpy as np

TNSR_MAGIC = b"TNSR"
_DTYPES = {0: "f32", 1: "i8", 2: "u8", 3: "i32", 4: "bin"}
_DTYPES_REV = {v: k for k, v in _DTYPES.items()}
_NP_MAP = {"f32": "<f4", "i8": "i1", "u8": "u1", "i32": "<i4"}


@dataclass
class Tensor:
    dtype: str                  # f32 | i8 | u8 | i32 | bin
    shape: tuple
    data: bytes                 # raw little-endian payload
    scale: float = 0.0
    zero_point: int = 0

    def to_numpy(self):
        if self.dtype == "bin":
            raise ValueError("unpack bin tensors manually")
        a = np.frombuffer(self.data, dtype=_NP_MAP[self.dtype])
        return a.reshape(self.shape)

    @staticmethod
    def from_numpy(a, scale: float = 0.0, zero_point: int = 0) -> "Tensor":
        kind = {"float32": "f32", "int8": "i8", "uint8": "u8",
                "int32": "i32"}.get(str(a.dtype))
        if kind is None:
            raise ValueError(f"unsupported dtype {a.dtype}")
        if len(a.shape) > 4:
            raise ValueError("max 4 dims")
        return Tensor(kind, tuple(a.shape), a.astype(a.dtype).tobytes(),
                      scale, zero_point)

    def pack(self) -> bytes:
        shape4 = list(self.shape) + [1] * (4 - len(self.shape))
        hdr = TNSR_MAGIC + bytes([_DTYPES_REV[self.dtype], len(self.shape),
                                  0, 0])
        hdr += struct.pack("<4i", *shape4)
        hdr += struct.pack("<f", self.scale)
        hdr += struct.pack("<i", self.zero_point)
        return hdr + self.data

    @staticmethod
    def unpack(blob: bytes) -> "Tensor":
        if blob[:4] != TNSR_MAGIC:
            raise ValueError("bad tensor magic")
        dtype = _DTYPES[blob[4]]
        ndim = blob[5]
        shape4 = struct.unpack("<4i", blob[8:24])
        scale = struct.unpack("<f", blob[24:28])[0]
        zp = struct.unpack("<i", blob[28:32])[0]
        return Tensor(dtype, tuple(shape4[:ndim]), blob[32:], scale, zp)


class RetroAI:
    """AI command wrappers bound to an open RetroConnection."""

    def __init__(self, conn):
        self.conn = conn

    async def hello(self) -> dict:
        return json.loads(await self.conn.command_text("AI_HELLO"))

    async def restart(self) -> str:
        return await self.conn.command_text("AI_RESTART", timeout=30)

    async def model_load(self, name: str, rim_bytes: bytes,
                         timeout: float = 120) -> str:
        status, data = await self.conn.send_command(
            f"MODEL_LOAD {name}", binary_payload=rim_bytes, timeout=timeout)
        text = data.decode("ascii", errors="replace")
        if status == 0xFF:
            raise RuntimeError(f"MODEL_LOAD failed: {text}")
        return text

    async def model_list(self) -> dict:
        return json.loads(await self.conn.command_text("MODEL_LIST"))

    async def model_unload(self, name: str) -> str:
        return await self.conn.command_text(f"MODEL_UNLOAD {name}")

    async def infer_run(self, name: str, input_bytes: bytes,
                        timeout: float = 120):
        """Returns fp32 logits as a numpy array."""
        status, data = await self.conn.send_command(
            f"INFER_RUN {name}", binary_payload=input_bytes, timeout=timeout)
        if status == 0xFF:
            raise RuntimeError(
                f"INFER_RUN failed: {data.decode('ascii', errors='replace')}")
        return np.frombuffer(data, dtype="<f4")

    async def tensor_put(self, slot: str, t: Tensor,
                         timeout: float = 120) -> str:
        status, data = await self.conn.send_command(
            f"TENSOR PUT {slot}", binary_payload=t.pack(), timeout=timeout)
        text = data.decode("ascii", errors="replace")
        if status == 0xFF:
            raise RuntimeError(f"TENSOR PUT failed: {text}")
        return text

    async def tensor_get(self, slot: str, timeout: float = 120) -> Tensor:
        status, data = await self.conn.send_command(f"TENSOR GET {slot}",
                                                    timeout=timeout)
        if status == 0xFF:
            raise RuntimeError(
                f"TENSOR GET failed: {data.decode('ascii', errors='replace')}")
        return Tensor.unpack(bytes(data))

    async def tensor_del(self, slot: str) -> str:
        return await self.conn.command_text(f"TENSOR DEL {slot}")
