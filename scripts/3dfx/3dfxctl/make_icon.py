#!/usr/bin/env python3
"""make_icon.py - draws 3dfxctl.ico, the 3dfx Control Panel's icon.

A dark rounded tile with a Gouraud-shaded triangle (orange -> gold -> cyan):
"3D", legible at 16x16. Written as a classic ICO whose every entry is a
32-bit BGRA DIB with an AND mask - NOT PNG-compressed entries, which only
Windows Vista and later can read (Pillow writes those), so XP shows the icon.

    python3 make_icon.py            -> 3dfxctl.ico beside this script
"""
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
SIZES = (16, 32, 48)
SS = 4                                  # supersampling per axis


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def tile_color(x, y, n):
    """background: navy top-left -> purple bottom-right, in a rounded square"""
    t = (x + y) / (2.0 * n)
    return lerp((24, 30, 64), (74, 34, 110), t)


def inside_round(x, y, n, r):
    cx = min(max(x, r), n - r)
    cy = min(max(y, r), n - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def bary(px, py, a, b, c):
    d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    w0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / d
    w1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / d
    return w0, w1, 1.0 - w0 - w1


def render(n):
    N = n * SS
    r = N * 0.18
    # the triangle: apex top-centre, a slight perspective lean
    A = (N * 0.50, N * 0.14)
    B = (N * 0.12, N * 0.84)
    C = (N * 0.88, N * 0.78)
    cA, cB, cC = (255, 140, 30), (255, 214, 60), (40, 200, 255)
    acc = [[(0.0, 0.0, 0.0, 0.0)] * n for _ in range(n)]
    for y in range(N):
        for x in range(N):
            px, py = x + 0.5, y + 0.5
            if not inside_round(px, py, N, r):
                continue
            w0, w1, w2 = bary(px, py, A, B, C)
            if min(w0, w1, w2) >= 0:
                col = tuple(w0 * cA[i] + w1 * cB[i] + w2 * cC[i] for i in range(3))
            else:
                col = tile_color(px, py, N)
            ax, ay = x // SS, y // SS
            s = acc[ay][ax]
            acc[ay][ax] = (s[0] + col[0], s[1] + col[1], s[2] + col[2], s[3] + 1)
    k = SS * SS
    pix = []
    for y in range(n):
        row = []
        for x in range(n):
            rr, gg, bb, cnt = acc[y][x]
            if cnt == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append((int(rr / cnt + 0.5), int(gg / cnt + 0.5), int(bb / cnt + 0.5),
                            int(255 * cnt / k + 0.5)))
        pix.append(row)
    return pix


def dib(pix):
    n = len(pix)
    hdr = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(n - 1, -1, -1):          # bottom-up
        for (r, g, b, a) in pix[y]:
            xor += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    andm = bytearray()
    for y in range(n - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(n):
            if pix[y][x][3] < 128:          # transparent where alpha is low
                bits[x // 8] |= 0x80 >> (x % 8)
        andm += bits
    return hdr + bytes(xor) + bytes(andm)


def main():
    images = [dib(render(n)) for n in SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    off = 6 + 16 * len(SIZES)
    for n, img in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(img), off)
        off += len(img)
    for img in images:
        out += img
    (HERE / "3dfxctl.ico").write_bytes(bytes(out))
    print(f"wrote {HERE / '3dfxctl.ico'} ({len(out)} bytes, sizes {SIZES})")


if __name__ == "__main__":
    main()
