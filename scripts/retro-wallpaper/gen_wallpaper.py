#!/usr/bin/env python3
"""
Retro Dossier wallpaper generator.

Composes an "exciting, engaging" spec-sheet wallpaper for a retro PC:
  - Header with hostname + CPU-year / GPU-year badges
  - A horizontal strip of spec cards (CPU / GPU / RAM / OS / DISPLAY / STORAGE)
  - Two game panels: games from the CPU release year and the GPU release year
  - A full-width collage of historical-event tiles (image + gradient + caption)
    for the year the CPU was released

Renders at the machine's exact native resolution and writes a 24-bit BMP
(safe for plain Windows wallpaper on every OS from Win98 to XP) plus a PNG
preview.

Usage:
  python3 gen_wallpaper.py profiles/192.168.1.124.json
  python3 gen_wallpaper.py profiles/192.168.1.124.json --preview-only

Profile schema: see profiles/*.json and README / the retro-wallpaper skill.

Design rationale (UX for CRT + desktop-icon coexistence) is documented in the
retro-wallpaper skill (SKILL.md).
"""
import sys, os, json, hashlib, urllib.request, argparse
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache")
OUT = os.path.join(HERE, "out")

# Fraction of the content width reserved (bottom-right) as a blank icon well.
# Kept in sync with arrange_icons.exe, which moves the desktop icons into it.
ICON_WELL_FRAC = 0.36
os.makedirs(CACHE, exist_ok=True)
os.makedirs(OUT, exist_ok=True)

DEJAVU = "/usr/share/fonts/truetype/dejavu"
LIB = "/usr/share/fonts/truetype/liberation"


def _pick(*candidates):
    """First font file that exists. The Condensed DejaVu faces ship in a
    separate package (fonts-dejavu-extra) that is absent on plain Ubuntu, and
    PIL fails with a bare 'cannot open resource' when one is missing."""
    for p in candidates:
        if p and os.path.exists(p):
            return p
    raise SystemExit(
        "no usable font found; tried: %s\n"
        "install with: sudo apt-get install fonts-dejavu-core fonts-dejavu-extra"
        % ", ".join(str(c) for c in candidates))


F_BOLD = _pick(os.path.join(DEJAVU, "DejaVuSans-Bold.ttf"),
               os.path.join(LIB, "LiberationSans-Bold.ttf"))
F_REG = _pick(os.path.join(DEJAVU, "DejaVuSans.ttf"),
              os.path.join(LIB, "LiberationSans-Regular.ttf"))
# Condensed falls back to the regular face - slightly wider text, same layout.
F_COND = _pick(os.path.join(DEJAVU, "DejaVuSansCondensed.ttf"),
               os.path.join(LIB, "LiberationSansNarrow-Regular.ttf"), F_REG)
F_COND_B = _pick(os.path.join(DEJAVU, "DejaVuSansCondensed-Bold.ttf"),
                 os.path.join(LIB, "LiberationSansNarrow-Bold.ttf"), F_BOLD)
F_MONO = _pick(os.path.join(DEJAVU, "DejaVuSansMono.ttf"),
               os.path.join(LIB, "LiberationMono-Regular.ttf"))
F_MONO_B = _pick(os.path.join(DEJAVU, "DejaVuSansMono-Bold.ttf"),
                 os.path.join(LIB, "LiberationMono-Bold.ttf"))

_font_cache = {}
def font(path, size):
    key = (path, size)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(path, size)
    return _font_cache[key]


# ---------------------------------------------------------------- image utils
def fetch(url):
    """Download url to cache (keyed by hash) and return local path. Cached."""
    h = hashlib.sha1(url.encode()).hexdigest()[:16]
    ext = ".png" if url.lower().split("?")[0].endswith(".png") else ".jpg"
    dst = os.path.join(CACHE, h + ext)
    if os.path.exists(dst) and os.path.getsize(dst) > 1000:
        return dst
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (retro-wallpaper-bot; contact perrymb@gmail.com)"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    with open(dst, "wb") as f:
        f.write(data)
    return dst


def cover(img, w, h):
    """Resize+crop img to exactly w x h, preserving aspect (center crop)."""
    iw, ih = img.size
    scale = max(w / iw, h / ih)
    nw, nh = max(1, int(iw * scale + 0.5)), max(1, int(ih * scale + 0.5))
    img = img.resize((nw, nh), Image.LANCZOS)
    left = (nw - w) // 2
    top = (nh - h) // 2
    return img.crop((left, top, left + w, top + h))


def vgrad(w, h, top_rgb, bot_rgb):
    base = Image.new("RGB", (w, h))
    px = base.load()
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(top_rgb[0] + (bot_rgb[0] - top_rgb[0]) * t)
        g = int(top_rgb[1] + (bot_rgb[1] - top_rgb[1]) * t)
        b = int(top_rgb[2] + (bot_rgb[2] - top_rgb[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    return base


# ---------------------------------------------------------------- text utils
def measure(draw, text, fnt):
    b = draw.textbbox((0, 0), text, font=fnt)
    return b[2] - b[0], b[3] - b[1]


def wrap(draw, text, fnt, max_w):
    words = text.split()
    lines, cur = [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if measure(draw, trial, fnt)[0] <= max_w or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def text_shadow(draw, xy, text, fnt, fill, shadow=(0, 0, 0), off=2):
    x, y = xy
    draw.text((x + off, y + off), text, font=fnt, fill=shadow)
    draw.text((x, y), text, font=fnt, fill=fill)


def rrect(draw, box, radius, fill=None, outline=None, width=1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


# ---------------------------------------------------------------- main compose
def build(profile):
    W = profile["width"]
    H = profile["height"]
    accent = tuple(profile.get("accent", [80, 200, 255]))
    accent2 = tuple(profile.get("accent2", [255, 160, 40]))
    ink = (232, 238, 245)
    dim = (150, 160, 175)

    # scale factor relative to a 1024x768 baseline. Use the SMALLER of the
    # width/height ratios so a widescreen (16:9/16:10) target doesn't let the
    # fixed-height blocks (header, cards, game panels) grow tall enough to
    # starve the events grid at the bottom. On 4:3 this is identical to W/1024.
    S = min(W / 1024.0, H / 768.0)

    def sp(v):  # scale a pixel value
        return int(round(v * S))

    # ---- background: dark vertical gradient + faint accent glow + scanlines
    bg_top = (14, 16, 22)
    bg_bot = (8, 9, 13)
    img = vgrad(W, H, bg_top, bg_bot).convert("RGB")

    # subtle radial accent glow in the upper-right
    glow = Image.new("L", (W, H), 0)
    gd = ImageDraw.Draw(glow)
    gr = int(W * 0.55)
    gd.ellipse([W - gr, -gr // 2, W + gr // 3, gr], fill=90)
    glow = glow.filter(ImageFilter.GaussianBlur(int(W * 0.10)))
    tint = Image.new("RGB", (W, H), accent)
    img = Image.composite(Image.blend(img, tint, 0.25), img, glow)

    # faint horizontal scanlines for CRT vibe
    sl = ImageDraw.Draw(img, "RGBA")
    step = max(2, sp(3))
    for y in range(0, H, step):
        sl.line([(0, y), (W, y)], fill=(0, 0, 0, 26), width=1)

    draw = ImageDraw.Draw(img, "RGBA")

    M = sp(26)          # outer margin
    gap = sp(16)        # gap between blocks
    y = M

    # ==================================================== HEADER
    hostname = profile["hostname"]
    title = profile.get("title", "SYSTEM DOSSIER")
    f_host = font(F_MONO_B, sp(46))
    f_sub = font(F_COND_B, sp(21))
    # left accent bar
    hb_h = sp(64)
    draw.rectangle([M, y, M + sp(7), y + hb_h], fill=accent)
    tx = M + sp(20)
    text_shadow(draw, (tx, y - sp(4)), hostname, f_host, ink, off=sp(2))
    draw.text((tx + sp(2), y + sp(46)), title.upper(), font=f_sub, fill=accent)

    # year badges on the right
    badges = [("CPU  " + str(profile["cpu_year"]), accent),
              ("GPU  " + str(profile["gpu_year"]), accent2)]
    f_badge_k = font(F_COND_B, sp(15))
    f_badge_v = font(F_MONO_B, sp(34))
    bw, bh = sp(150), sp(66)
    bx = W - M - bw
    for label, col in badges:
        by = y
        rrect(draw, [bx, by, bx + bw, by + bh], sp(8),
              fill=(255, 255, 255, 12), outline=col, width=sp(2))
        yr = label.split()[-1]
        klabel = label.split()[0]
        draw.text((bx + sp(14), by + sp(9)), klabel, font=f_badge_k, fill=col)
        vw = measure(draw, yr, f_badge_v)[0]
        text_shadow(draw, (bx + bw - vw - sp(14), by + sp(24)), yr, f_badge_v, ink, off=sp(1))
        bx -= bw + gap
    y += hb_h + gap

    # thin divider
    draw.line([(M, y), (W - M, y)], fill=(255, 255, 255, 30), width=1)
    y += gap

    # ==================================================== SPEC CARDS
    specs = profile["specs"]  # list of [label, value]
    n = len(specs)
    card_h = sp(74)
    total_w = W - 2 * M
    # Reserve the right column (bottom-right) as a blank icon well. Spec cards +
    # header stay full width (they sit above the well); the game panels and the
    # events collage keep to content_w on the left, leaving the well clear.
    icon_w = int(round(ICON_WELL_FRAC * total_w))
    content_w = total_w - icon_w - gap
    cw = (total_w - (n - 1) * gap) // n
    f_sk = font(F_COND_B, sp(15))
    f_sv = font(F_COND_B, sp(19))
    f_sv_sm = font(F_COND_B, sp(16))
    cx = M
    for label, value in specs:
        rrect(draw, [cx, y, cx + cw, y + card_h], sp(9),
              fill=(255, 255, 255, 14), outline=(255, 255, 255, 34), width=1)
        draw.rectangle([cx, y, cx + sp(4), y + card_h], fill=accent)
        draw.text((cx + sp(14), y + sp(9)), label.upper(), font=f_sk, fill=accent)
        # fit value: wrap to <=2 lines
        vf = f_sv
        lines = wrap(draw, value, vf, cw - sp(24))
        if len(lines) > 2:
            vf = f_sv_sm
            lines = wrap(draw, value, vf, cw - sp(22))
        vy = y + sp(30)
        for ln in lines[:2]:
            draw.text((cx + sp(14), vy), ln, font=vf, fill=ink)
            vy += measure(draw, ln, vf)[1] + sp(4)
        cx += cw + gap
    y += card_h + gap

    # ==================================================== GAMES PANELS
    games_h = sp(196)
    panel_w = (content_w - gap) // 2
    panels = [
        ("GAMES OF " + str(profile["cpu_year"]) + "  -  CPU ERA",
         profile.get("cpu_label", ""), profile["games_cpu"], accent),
        ("GAMES OF " + str(profile["gpu_year"]) + "  -  GPU ERA",
         profile.get("gpu_label", ""), profile["games_gpu"], accent2),
    ]
    f_ph = font(F_COND_B, sp(18))
    f_psub = font(F_COND, sp(13))
    f_gt = font(F_COND_B, sp(16))
    f_gn = font(F_COND, sp(13))
    px = M
    for head, sub, games, col in panels:
        rrect(draw, [px, y, px + panel_w, y + games_h], sp(10),
              fill=(255, 255, 255, 10), outline=(255, 255, 255, 30), width=1)
        # header strip
        draw.rounded_rectangle([px, y, px + panel_w, y + sp(30)], radius=sp(10),
                               fill=(col[0], col[1], col[2], 46))
        draw.rectangle([px, y + sp(20), px + panel_w, y + sp(30)],
                       fill=(col[0], col[1], col[2], 46))
        draw.text((px + sp(14), y + sp(6)), head, font=f_ph, fill=col)
        hw = measure(draw, head, f_ph)[0]
        if sub:
            sw = measure(draw, sub, f_psub)[0]
            # only draw the right-aligned sub-label if it clears the header text
            if sp(14) + hw + sp(12) + sw + sp(12) <= panel_w:
                draw.text((px + panel_w - sw - sp(12), y + sp(10)), sub, font=f_psub, fill=dim)
        # list
        gy = y + sp(38)
        col_x = px + sp(16)
        line_h = sp(19)
        for g in games[:8]:
            draw.ellipse([col_x, gy + sp(5), col_x + sp(6), gy + sp(11)], fill=col)
            tstr = g["title"]
            draw.text((col_x + sp(14), gy - sp(1)), tstr, font=f_gt, fill=ink)
            tw = measure(draw, tstr, f_gt)[0]
            note = g.get("note", "")
            if note:
                nx = col_x + sp(14) + tw + sp(8)
                note_txt = "- " + note
                nw = measure(draw, note_txt, f_gn)[0]
                # only draw the note if it fits fully inside the panel (no overflow
                # into the icon well on narrow panels)
                if nx + nw <= px + panel_w - sp(10):
                    draw.text((nx, gy + sp(1)), note_txt, font=f_gn, fill=dim)
            gy += line_h
        px += panel_w + gap
    y += games_h + gap

    # ==================================================== EVENTS COLLAGE
    events = profile["events"][:6]
    # section label
    f_sech = font(F_COND_B, sp(18))
    sec = "  THIS IS THE WORLD OF " + str(profile["cpu_year"]) + "  "
    draw.text((M, y), sec.strip(), font=f_sech, fill=ink)
    shw = measure(draw, sec.strip(), f_sech)[0]

    # events keep to content_w (left); the reserved right column stays blank so
    # the desktop icons parked there (arrange_icons.exe) read cleanly.
    ev_w = content_w
    draw.line([(M + shw + sp(12), y + sp(12)), (M + ev_w, y + sp(12))],
              fill=(255, 255, 255, 30), width=1)
    y += sp(28)

    grid_h = H - M - y
    cols = 3
    rows = 2
    tgap = sp(12)
    tw = (ev_w - (cols - 1) * tgap) // cols
    th = (grid_h - (rows - 1) * tgap) // rows
    f_cap = font(F_COND_B, sp(15))
    f_cap_sm = font(F_COND_B, sp(13))
    f_date = font(F_MONO_B, sp(12))
    f_credit = font(F_COND, sp(9))

    for i, ev in enumerate(events):
        r, c = divmod(i, cols)
        tx = M + c * (tw + tgap)
        ty = y + r * (th + tgap)
        # image
        placed = False
        url = ev.get("image_url")
        if url:
            try:
                im = Image.open(fetch(url)).convert("RGB")
                im = cover(im, tw, th)
                # rounded mask
                mask = Image.new("L", (tw, th), 0)
                ImageDraw.Draw(mask).rounded_rectangle([0, 0, tw, th], radius=sp(10), fill=255)
                img.paste(im, (tx, ty), mask)
                placed = True
            except Exception as e:
                sys.stderr.write("  [warn] image failed %s: %s\n" % (url, e))
        if not placed:
            rrect(draw, [tx, ty, tx + tw, ty + th], sp(10), fill=(30, 34, 44))

        # bottom gradient scrim for caption legibility
        scrim_h = int(th * 0.62)
        scrim = Image.new("RGBA", (tw, scrim_h), (0, 0, 0, 0))
        sp_px = scrim.load()
        for yy in range(scrim_h):
            a = int(238 * (yy / (scrim_h - 1)) ** 1.35)
            for xx in range(tw):
                sp_px[xx, yy] = (0, 0, 0, a)
        smask = Image.new("L", (tw, th), 0)
        ImageDraw.Draw(smask).rounded_rectangle([0, 0, tw, th], radius=sp(10), fill=255)
        scrim_full = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
        scrim_full.paste(scrim, (0, th - scrim_h))
        img.paste(scrim_full, (tx, ty), Image.composite(
            scrim_full.split()[3], Image.new("L", (tw, th), 0), smask))
        draw = ImageDraw.Draw(img, "RGBA")

        # accent border
        rrect(draw, [tx, ty, tx + tw, ty + th], sp(10),
              outline=(accent[0], accent[1], accent[2], 200), width=sp(2))

        # date chip (top-left)
        date = ev.get("date", "")
        if date:
            dw = measure(draw, date, f_date)[0]
            rrect(draw, [tx + sp(8), ty + sp(8), tx + sp(8) + dw + sp(12), ty + sp(8) + sp(20)],
                  sp(5), fill=(accent[0], accent[1], accent[2], 220))
            draw.text((tx + sp(14), ty + sp(10)), date, font=f_date, fill=(10, 12, 16))

        # caption (bottom)
        cap = ev.get("caption", "")
        cf = f_cap
        lines = wrap(draw, cap, cf, tw - sp(24))
        if len(lines) > 3:
            cf = f_cap_sm
            lines = wrap(draw, cap, cf, tw - sp(22))
        lh = measure(draw, "Ag", cf)[1] + sp(4)
        cred = ev.get("credit", "")
        block_h = len(lines[:3]) * lh + (sp(12) if cred else 0)
        cy = ty + th - sp(12) - block_h
        for ln in lines[:3]:
            text_shadow(draw, (tx + sp(12), cy), ln, cf, ink, off=sp(1))
            cy += lh
        if cred:
            draw.text((tx + sp(12), ty + th - sp(13)), cred[:60], font=f_credit, fill=(170, 178, 190))

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--preview-only", action="store_true")
    args = ap.parse_args()

    with open(args.profile) as f:
        profile = json.load(f)

    img = build(profile)
    base = os.path.splitext(os.path.basename(args.profile))[0]
    png = os.path.join(OUT, base + ".png")
    img.save(png)
    print("PNG preview:", png)
    if not args.preview_only:
        bmp = os.path.join(OUT, base + ".bmp")
        img.save(bmp)  # 24-bit BMP
        print("BMP wallpaper:", bmp, "(%d x %d)" % img.size)


if __name__ == "__main__":
    main()
