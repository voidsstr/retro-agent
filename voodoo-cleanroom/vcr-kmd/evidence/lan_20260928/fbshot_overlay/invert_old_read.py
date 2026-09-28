#!/usr/bin/env python3
"""Invert the first overlay build's fbshot read - the proof of the tile aperture.

.124, 2026-09-28 07:01, Quake III on our stack in 4-chip SLI at 1280x960x32:
vidProcCfg 026c0101 (desktop off, overlay on, tiled, RGB32), stride 00280028,
vidCurrOverlayStartAddr 03c3e000 (quake3_overlay.json). That build read the
front buffer as RAW tiled memory: memBase1 + start + ((y/32)*40 + xb/128)*4096
+ (y%32)*128 + xb%128. The picture came out as narrow column blocks with gaps
(quake3_overlay_old_read.jpg; the PNG it came from is lossless).

Hypothesis: from lfbMemoryConfig's tile begin page up, memBase1 is a LINEAR
aperture over the tiled memory - offset o past the begin page is line o/8192,
byte o%8192 of the frame - and Glide puts that page at its first colour buffer,
which is where 03c3e000 is. If so, every pixel the old read returned is the
frame's pixel (x'/4, y') for the aperture byte it hit, and putting each one back
there rebuilds the frame. Run on the lossless PNG it gives the Quake III main
menu (quake3_overlay_inverted.jpg): the logo, SINGLE PLAYER ... CINEMATICS,
continuous across all four chips' 32-line bands - so the aperture also answers
every SLI band from the master's memBase1. Lines 600-959 were never read (the
old read spans 30 tile rows x 160 KB = 600 aperture lines): magenta. The blocky
glitches are the old read's slowness - it took a buffer Quake III was already
redrawing (their edges do not follow the 32-line bands).

usage: invert_old_read.py <old fbshot png> <out png>
"""
import sys
from PIL import Image

W, H, BPP = 1280, 960, 4
STRIDE_TILES, TILE_W, TILE_H, LFB_LINE = 40, 128, 32, 8192


def main(src_path, out_path):
    src = Image.open(src_path).convert('RGB')
    sp = src.load()
    out = Image.new('RGB', (W, H), (255, 0, 255))
    op = out.load()
    for y in range(src.size[1]):
        for x in range(src.size[0]):
            xb = x * BPP
            o = (((y // TILE_H) * STRIDE_TILES + xb // TILE_W) * (TILE_W * TILE_H) +
                 (y % TILE_H) * TILE_W + xb % TILE_W)
            ly, lx = divmod(o, LFB_LINE)
            if lx < W * BPP and ly < H and lx % BPP == 0:
                op[lx // BPP, ly] = sp[x, y]
    out.save(out_path)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
