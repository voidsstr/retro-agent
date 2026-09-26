#!/usr/bin/env python3
"""86box-shot.py [out.png] [--display :21] - capture the 86Box test bed's Xvfb screen."""
import struct, subprocess, sys
from PIL import Image
args = [a for a in sys.argv[1:] if not a.startswith('--display')]
disp = ':21'
for i, a in enumerate(sys.argv):
    if a == '--display':
        disp = sys.argv[i + 1]
out = args[0] if args and args[0] != disp else '/tmp/claude-1000/86box.png'
raw = subprocess.run(['xwd', '-root', '-silent', '-display', disp], check=True, capture_output=True).stdout
h = struct.unpack('>25I', raw[:100])
hsize, w, ht, border, bpp, bpl, ncolors = h[0], h[4], h[5], h[7], h[11], h[12], h[19]
off = hsize + ncolors * 12
# Xvfb depth 24 reports bits_per_pixel 24 and really packs 3 bytes a pixel
# (bytes_per_line is still padded wider) - decode by bits_per_pixel, not bpl/w
mode = {24: ('RGB', 'BGR'), 32: ('RGBX', 'BGRX')}[bpp]
img = Image.frombuffer(mode[0], (w, ht), raw[off:off + bpl * ht], 'raw', mode[1], bpl, 1).convert('RGB')
bbox = img.getbbox()
(img.crop(bbox) if bbox else img).save(out)
print(out, img.size, bbox)
