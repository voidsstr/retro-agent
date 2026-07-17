"""Shared helpers for the .rim tooling (see FORMAT.md)."""
import numpy as np

# Manifest dtype string -> numpy dtype (little-endian payload, flags bit0 = 1).
DTYPES = {
    "f32": np.dtype("<f4"),
    "i8": np.dtype("i1"),
    "i32": np.dtype("<i4"),
    "u8": np.dtype("u1"),
    # packed bits (BNN): manifest shape is the PACKED byte shape
    # [n_out, ceil(n_in/8)], LSB-first, bit 1 <=> +1 (see bnn/BNN-SPEC.md)
    "bin": np.dtype("u1"),
}

MAGIC = b"RIM1"
HEADER_LEN = 12  # magic + flags + manifest_len


def round_half_away(x):
    """Round half away from zero, matching C roundf() — NOT numpy banker's rounding."""
    x = np.asarray(x)
    return np.sign(x) * np.floor(np.abs(x) + 0.5)


def align16(n):
    return (n + 15) // 16 * 16
