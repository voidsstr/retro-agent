#!/usr/bin/env python3
"""
test_fastui.py - unit test for the SCREENDIFF/CLICKSHOT delta reconstruction in
fastui.FastUI._apply. No hardware/agent needed: feed synthetic delta blobs and
assert the reconstructed framebuffer is correct (tile placement + BGR->RGB swap).

Run: python3 test_fastui.py   (exit 0 = pass)
"""
import os, sys, struct, asyncio

sys.path.insert(0, os.path.dirname(__file__))
from fastui import FastUI


def make_blob(w, h, tile, tiles):
    """tiles: list of (x,y,tw,th, rgb_fill=(r,g,b)). Packs the wire format
    (per-tile pixels are TOP-DOWN BGR, matching agent/src/screen.c)."""
    out = bytearray(struct.pack('<HHHH', w, h, tile, len(tiles)))
    for (x, y, tw, th, (r, g, b)) in tiles:
        out += struct.pack('<HHHH', x, y, tw, th)
        out += bytes([b, g, r]) * (tw * th)   # BGR
    return bytes(out)


def px(ui, x, y):
    off = (y * ui.w + x) * 3
    return tuple(ui.buf[off:off + 3])          # (R,G,B)


async def run():
    ui = FastUI('0.0.0.0')

    # 1. full-frame baseline: one tile covering everything, filled red
    W, H = 128, 96
    await ui._apply(make_blob(W, H, 64, [(0, 0, W, H, (200, 10, 20))]))
    assert (ui.w, ui.h) == (W, H), (ui.w, ui.h)
    assert px(ui, 0, 0) == (200, 10, 20), px(ui, 0, 0)          # BGR->RGB correct
    assert px(ui, W - 1, H - 1) == (200, 10, 20)

    # 2. partial delta: paint a green tile at (64,32) 32x16, rest unchanged
    await ui._apply(make_blob(W, H, 64, [(64, 32, 32, 16, (0, 255, 0))]))
    assert px(ui, 64, 32) == (0, 255, 0), px(ui, 64, 32)        # updated tile
    assert px(ui, 95, 47) == (0, 255, 0)                        # tile far corner
    assert px(ui, 0, 0) == (200, 10, 20)                        # outside stays red
    assert px(ui, 63, 32) == (200, 10, 20)                      # just left of tile
    assert px(ui, 96, 32) == (200, 10, 20)                      # just right of tile

    # 3. empty delta (0 dirty tiles) leaves framebuffer intact
    await ui._apply(make_blob(W, H, 64, []))
    assert px(ui, 64, 32) == (0, 255, 0)

    print('test_fastui: 3/3 delta-reconstruction checks passed')


if __name__ == '__main__':
    asyncio.run(run())
