#!/usr/bin/env python3
"""make_icon.py - builds 3dfxctl.ico, the 3dfx Control Panel's icon: the 3dfx logo.

The artwork is 3dfx's own: `3dfx_logo_src.ico` is the IDI_MAIN_ICON group of
the 3dfx "Driver Setup.exe" in the AmigaMerlin 3.1 R1 package on the share
(Files/Drivers/3DFX/WinXP/Amigamerlin_3.1_R1_MesaFx/Amigamerlin 3.1 R1/), a
single 32x32 16-colour image - the white "3dfx" with the red swoosh on black.
It was extracted unchanged (pefile: RT_GROUP_ICON + its RT_ICON entries).

From it this writes 16, 32 and 48 px entries, each a 32-bit BGRA DIB with an
AND mask - NOT PNG-compressed entries, which only Windows Vista and later can
read (Pillow writes those), so XP shows the icon. The 32 px entry is the
original pixels exactly; 16 and 48 are resampled from it.

The same file is deployed beside the panel as C:\\RETRO_AGENT\\3dfxlogo.ico and
the desktop shortcut points its icon there: XP caches icons by PATH, so a
changed icon inside the same 3dfxctl.exe could keep showing the old one.

    python3 make_icon.py            -> 3dfxctl.ico beside this script
"""
import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
SRC = HERE / "3dfx_logo_src.ico"
SIZES = (16, 32, 48)


def source_rgba():
    im = Image.open(SRC)
    im.size = (32, 32)
    im.load()
    return im.convert("RGBA")


def dib(img):
    n = img.size[0]
    px = img.load()
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(n - 1, -1, -1):          # bottom-up
        for x in range(n):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    andm = bytearray()
    for y in range(n - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(n):
            if px[x, y][3] < 128:           # transparent where alpha is low
                bits[x // 8] |= 0x80 >> (x % 8)
        andm += bits
    return hdr + bytes(xor) + bytes(andm)


def main():
    base = source_rgba()
    imgs = []
    for n in SIZES:
        imgs.append(base if n == 32 else base.resize((n, n), Image.LANCZOS))
    blobs = [dib(i) for i in imgs]
    out = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    off = 6 + 16 * len(SIZES)
    for n, b in zip(SIZES, blobs):
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(b), off)
        off += len(b)
    for b in blobs:
        out += b
    (HERE / "3dfxctl.ico").write_bytes(bytes(out))
    print(f"wrote {HERE / '3dfxctl.ico'} ({len(out)} bytes, sizes {SIZES}) from {SRC.name}")


if __name__ == "__main__":
    main()
