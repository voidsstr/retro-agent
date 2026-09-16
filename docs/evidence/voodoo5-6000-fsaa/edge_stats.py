#!/usr/bin/env python3
"""edge_stats.py - is a polygon edge anti-aliased? Measured locally, per column.

For each image column inside a crop that spans a high-contrast boundary, find
the strongest single-step vertical transition and ask whether the boundary
pixel is a BLEND of the two sides (an intermediate sample - what anti-aliasing
produces) or belongs to one side (a hard step). "Blended" means the pixel sits
strictly inside the middle 60% of the range between the pixels one row above
and one row below. Measured locally, because a global min/max over the column
measures the sky's own gradient instead of the edge - the first version of this
did exactly that and reported 48 "intermediate" pixels on a plain staircase.

    python3 edge_stats.py <image> <left> <top> <right> <bottom>

The published figure - 78% hard-step columns on the UT99 8x-AA-requested frame -
came from: edge_stats.py ut99-glide_1024x768_cfg8_8xaa.png 300 430 700 510
"""
import sys
from PIL import Image

def edge_report(path, box):
    im = Image.open(path).convert("L").crop(box)
    w, h = im.size
    px = im.load()
    blended = hard = 0
    for x in range(w):
        col = [px[x, y] for y in range(h)]
        y = max(range(1, h - 1), key=lambda i: abs(col[i + 1] - col[i - 1]))
        above, below, here = col[y - 1], col[y + 1], col[y]
        step = abs(below - above)
        if step < 20:
            continue
        lo, hi = min(above, below), max(above, below)
        margin = 0.20 * step
        if lo + margin < here < hi - margin:
            blended += 1
        else:
            hard += 1
    tot = blended + hard
    return tot, blended, hard

if __name__ == "__main__":
    path = sys.argv[1]
    box = tuple(int(v) for v in sys.argv[2:6])
    tot, blended, hard = edge_report(path, box)
    print(f"{tot} edge columns | blended (AA) {blended} ({100*blended/tot:.0f}%) | hard step {hard} ({100*hard/tot:.0f}%)")
