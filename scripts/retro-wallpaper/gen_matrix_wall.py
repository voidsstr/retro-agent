#!/usr/bin/env python3
"""gen_matrix_wall.py — a "Matrix / hacker terminal" desktop wallpaper for a
retro box: black background, green digital-rain columns, and a translucent
terminal panel (hostname + specs) in the top-right, with the BOTTOM-LEFT left
deliberately dark and empty as the icon well (arrange_icons_ll.exe parks the
desktop icons there).

  python3 gen_matrix_wall.py --w 1280 --h 1024 --host 2004-XP \
      --spec "CPU: Athlon 64 4000+" --spec "GPU: Radeon HD 3850" ... --out wall.bmp

Pure PIL, deterministic (seeded by hostname so a box's rain looks stable across
regens). Output is a 24-bit BMP (what the agent/rotate_wall path expects).
"""
import argparse
import hashlib
from PIL import Image, ImageDraw, ImageFont

# Katakana + digits + latin — the classic Matrix glyph soup.
GLYPHS = ("0123456789"
          "ｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿﾀﾁﾂﾃﾅﾆﾇﾉﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓﾔﾕﾖﾗﾘﾚﾛﾜﾝ"
          "ABCDEFGHJKLMNPRSTUVWXYZ:.=*+<>|/\\")


def _rng(seed):
    """Tiny deterministic PRNG (xorshift) seeded from a string — no global state."""
    s = int(hashlib.sha256(seed.encode()).hexdigest()[:8], 16) or 0x1234567
    def nxt():
        nonlocal s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        return s & 0xFFFFFFFF
    return nxt


def _font(size, mono=True):
    for path in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
                 "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()


def generate(w, h, host, specs, out, variant=0):
    rnd = _rng("%s-%d" % (host, variant))
    img = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(img)

    cell = max(14, w // 90)          # glyph cell / column width
    gf = _font(cell - 2)
    cols = w // cell
    rows = h // cell + 1

    # icon well: bottom-left quadrant kept dark & sparse so icons read clearly
    well_w = int(w * 0.42)
    well_top = int(h * 0.42)

    def in_well(px, py):
        return px < well_w and py > well_top

    # --- digital rain: each column a falling head with a fading green tail ---
    for cx in range(cols):
        x = cx * cell
        head = rnd() % (rows + 20) - 10          # head row (can start off-screen)
        tail = 6 + rnd() % 18
        bright = 160 + rnd() % 60
        for t in range(tail):
            ry = head - t
            if ry < 0 or ry > rows:
                continue
            y = ry * cell
            if in_well(x, y) and rnd() % 100 < 78:
                continue                          # thin the rain over the icon well
            g = int(bright * (1.0 - t / float(tail)))
            if g < 22:
                continue
            if t == 0:                            # head glyph = near-white
                col = (200, 255, 210)
            else:
                col = (0, g, int(g * 0.28))
            ch = GLYPHS[rnd() % len(GLYPHS)]
            d.text((x, y), ch, font=gf, fill=col)
        # a few static dim glyphs for depth
        for _ in range(rnd() % 3):
            ry = rnd() % rows
            y = ry * cell
            if in_well(x, y):
                continue
            d.text((x, y), GLYPHS[rnd() % len(GLYPHS)], font=gf, fill=(0, 40, 12))

    # --- terminal spec panel, top-right ---
    pad = max(16, w // 60)
    tf_h = _font(max(26, w // 34))
    tf_s = _font(max(15, w // 74))
    tf_t = _font(max(13, w // 88))
    lines = ["> whoami", "  " + host.upper()] + ["  " + s for s in specs]
    tw = max(d.textlength(l, font=tf_s) for l in (["  " + host.upper()] + ["  " + s for s in specs]))
    panel_w = int(tw + pad * 3)
    panel_h = int(len(lines) * (tf_s.size + 8) + tf_h.size + pad * 2)
    px0 = w - panel_w - pad
    py0 = pad

    # translucent dark panel with a green frame + glow
    panel = Image.new("RGBA", (panel_w, panel_h), (0, 12, 4, 205))
    img.paste(Image.new("RGB", (panel_w, panel_h), (0, 8, 3)), (px0, py0),
              panel.split()[3])
    d.rectangle([px0, py0, px0 + panel_w, py0 + panel_h], outline=(0, 160, 60), width=2)
    d.rectangle([px0 + 3, py0 + 3, px0 + panel_w - 3, py0 + panel_h - 3],
                outline=(0, 70, 26), width=1)
    # title bar
    d.rectangle([px0, py0, px0 + panel_w, py0 + tf_h.size + pad // 2], fill=(0, 40, 16))
    d.text((px0 + pad, py0 + 4), "root@%s:~#" % host.lower(), font=tf_t, fill=(0, 230, 90))

    y = py0 + tf_h.size + pad
    d.text((px0 + pad, y), "> whoami", font=tf_t, fill=(0, 150, 55)); y += tf_t.size + 6
    d.text((px0 + pad, y), host.upper(), font=tf_h, fill=(120, 255, 150)); y += tf_h.size + 8
    for s in specs:
        d.text((px0 + pad, y), s, font=tf_s, fill=(0, 220, 90)); y += tf_s.size + 8

    # blinking-cursor prompt at the bottom of the panel
    d.text((px0 + pad, py0 + panel_h - tf_s.size - pad // 2), "> _",
           font=tf_s, fill=(140, 255, 160))

    # subtle scanlines for the CRT/terminal vibe
    for sy in range(0, h, 3):
        d.line([(0, sy), (w, sy)], fill=(0, 6, 2))

    # faint corner label so the well reads as intentional
    d.text((14, h - tf_t.size - 12), "// SYSTEM ICONS", font=tf_t, fill=(0, 90, 34))

    img.save(out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--host", required=True)
    ap.add_argument("--spec", action="append", default=[])
    ap.add_argument("--variant", type=int, default=0)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    generate(a.w, a.h, a.host, a.spec, a.out, a.variant)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
