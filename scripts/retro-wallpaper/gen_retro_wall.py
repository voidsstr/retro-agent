#!/usr/bin/env python3
"""Generate the fleet wallpaper: a green-phosphor CRT with a real icon bay.

THE POINT OF THE DESIGN. A desktop wallpaper on these machines has one job that
matters more than looking good: the icons have to be FINDABLE. The previous
wallpaper reserved a dark "well" in the bottom-left while arrange_icons.exe
parked icons in the bottom-right, so the art and the icons fought each other and
you hunted for shortcuts against a busy background.

So the geometry is defined ONCE here, in ICON_BAY, and exported to JSON for the
arranger to consume. The wallpaper draws a visible cell for every icon slot -
which means the grid reads as a grid even before Windows has drawn anything into
it, and an icon always sits inside a box rather than floating over artwork.

THE LOOK. Green phosphor on black, matching the fleet's Classic + green system
colours (see apply_hacker_theme in retrowall.c). The art is vector/wireframe
rather than pixel art on purpose: monochrome green vector graphics are what
Battlezone, Asteroids, Tempest and Star Wars actually looked like, so the
aesthetic is period-correct rather than a modern idea of "retro". A perspective
grid floor recedes to a horizon with wireframe mountains, an arcade cabinet
stands at the right, and the whole thing sits under scanlines and a CRT vignette.

Everything is drawn, not sourced - no assets to lose, and it regenerates at any
resolution the fleet turns out to need.
"""
import argparse
import json
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

# ---------------------------------------------------------------- palette
# A P1-phosphor green, plus the dimmer steps used for structure. Kept few and
# named so the art and the icon bay cannot drift apart tonally.
BLACK      = (0, 0, 0)
BAY_BG     = (4, 10, 4)
GRID_FAINT = (0, 40, 0)
GRID_DIM   = (0, 70, 0)
GREEN_DIM  = (0, 110, 0)
GREEN      = (0, 190, 0)
GREEN_HOT  = (120, 255, 140)

FONT_DIRS = ['/usr/share/fonts/truetype/dejavu', '/usr/share/fonts/truetype/noto']


def font(size, bold=False):
    names = (['DejaVuSansMono-Bold.ttf', 'NotoSansMono-Bold.ttf'] if bold
             else ['DejaVuSansMono.ttf', 'NotoSansMono-Regular.ttf'])
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.isfile(p):
                try:
                    return ImageFont.truetype(p, size)
                except OSError:
                    pass
    return ImageFont.load_default()


DEFAULT_LIBRARY = '/mnt/retro-share/Files/Games-Library'


def count_staged_shortcuts(library):
    """How many desktop icons the fleet will actually have.

    One per DATA line of each title's launch.txt (a title can ship several -
    Red Alert 2 lists the game and Yuri's Revenge), plus the handful the agent
    makes itself. Directories starting with '_' are support, not games.

    Returns 0 when the share is not mounted, which is not an error here: the
    caller falls back to the base bay and says so.
    """
    AGENT_OWN = 3          # Retro Agent, Retro Chat, and the share shortcut
    try:
        titles = sorted(os.listdir(library))
    except OSError:
        return 0
    n = 0
    for t in titles:
        if t.startswith('_'):
            continue
        try:
            with open(os.path.join(library, t, 'launch.txt'), 'rb') as fh:
                body = fh.read(1023).decode('ascii', 'replace')
        except OSError:
            continue
        for line in body.splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                n += 1
    return n + AGENT_OWN if n else 0


def icon_bay(w, h, n_icons=None):
    """Where the icons live. THE one definition - the arranger reads this.

    Windows puts icons at the top-left by default and fights attempts to move
    them elsewhere, so the bay is top-left too: with the grain rather than
    against it. Cell size matches XP's default icon spacing closely enough that
    a user dragging an icon lands in a cell.

    n_icons WIDENS THE DRAWN PANEL TO MATCH THE ARRANGER. The agent
    (gamesync.c:gs_arrange_cols) widens on overflow rather than packing
    downward, because at 1024x768 the base bay is 4x8 = 32 slots against a
    library that is now 76 shortcuts, and rows past the 8th land below the
    bottom of the screen where they cannot be clicked at all.

    Until now only the ARRANGER widened; this function always drew a bay at a
    fixed ~34% of the screen width. So the extra columns spilled outside the
    drawn "GAME LIBRARY" frame onto the bare wallpaper - observed on .143.
    Passing the real shortcut count makes the art and the layout agree.

    Leave n_icons None to get the base panel (what a caller wants when it does
    not know the count). The widening rule below MUST stay identical to
    gs_arrange_cols(); tests/python/test_icon_bay_matches_agent.py pins them.
    """
    cell_w, cell_h = 76, 80
    margin_x = max(18, int(w * 0.018))
    margin_y = max(18, int(h * 0.030))
    header_h = 34
    # As many columns as fit in ~34% of the width, and rows in the usable height.
    cols = max(2, int((w * 0.34) // cell_w))
    rows = max(3, int((h - margin_y - header_h - 24) // cell_h))

    if n_icons is not None and n_icons > cols * rows:
        need = (n_icons + rows - 1) // rows      # cols to fit in `rows` rows
        maxcols = max(1, (w - margin_x) // cell_w)   # what the screen allows
        cols = max(cols, min(need, maxcols))     # never narrow the bay

    return {
        'x': margin_x, 'y': margin_y + header_h,
        'cell_w': cell_w, 'cell_h': cell_h,
        'cols': cols, 'rows': rows,
        'header_y': margin_y,
        'width': cols * cell_w, 'height': rows * cell_h,
    }


def draw_bay(d, bay, w, h):
    """The icon bay: a panel with one visible cell per icon slot."""
    x, y = bay['x'], bay['y']
    bw, bh = bay['width'], bay['height']

    d.rectangle([x - 10, bay['header_y'] - 8, x + bw + 10, y + bh + 10],
                fill=BAY_BG, outline=GRID_DIM)
    # Corner ticks - a cheap way to make a plain rectangle look like hardware.
    for cx, cy, dx, dy in ((x - 10, bay['header_y'] - 8, 1, 1),
                           (x + bw + 10, bay['header_y'] - 8, -1, 1),
                           (x - 10, y + bh + 10, 1, -1),
                           (x + bw + 10, y + bh + 10, -1, -1)):
        d.line([cx, cy, cx + 16 * dx, cy], fill=GREEN_DIM)
        d.line([cx, cy, cx, cy + 16 * dy], fill=GREEN_DIM)

    f = font(15, bold=True)
    d.text((x, bay['header_y'] - 2), 'GAME LIBRARY', font=f, fill=GREEN)
    d.line([x, bay['header_y'] + 22, x + bw, bay['header_y'] + 22], fill=GREEN_DIM)

    # One filled slot per icon. The first version drew corner ticks, which
    # looked fine in isolation and turned into a field of plus-signs on screen -
    # ticks from four adjacent cells meet in the gaps. A faint filled panel with
    # a thin border reads unambiguously as "an icon goes here" and, being
    # slightly lighter than the backdrop, it lifts the icon off the background
    # instead of competing with it.
    for r in range(bay['rows']):
        for c in range(bay['cols']):
            cx = x + c * bay['cell_w'] + 3
            cy = y + r * bay['cell_h'] + 3
            cw, ch = bay['cell_w'] - 9, bay['cell_h'] - 9
            d.rectangle([cx, cy, cx + cw, cy + ch], fill=(6, 14, 6),
                        outline=GRID_FAINT)
            # A tick in the top-left of each slot: gives the eye a reading order
            # down the column even when the bay is empty.
            d.line([cx + 2, cy + 2, cx + 9, cy + 2], fill=GRID_DIM)
            d.line([cx + 2, cy + 2, cx + 2, cy + 9], fill=GRID_DIM)


def draw_grid_floor(d, w, h, horizon):
    """Perspective floor receding to the horizon - the vector-3D look."""
    vp_x = int(w * 0.66)
    # Verticals converging on the vanishing point.
    for i in range(-28, 29):
        bx = vp_x + i * int(w * 0.075)
        d.line([bx, h, vp_x, horizon], fill=GRID_FAINT if i % 2 else GRID_DIM)
    # Horizontals, spaced so they bunch up toward the horizon.
    y = h
    step = int(h * 0.075)
    while y > horizon + 2:
        d.line([0, y, w, y], fill=GRID_DIM if step > 6 else GRID_FAINT)
        y -= step
        step = max(3, int(step * 0.72))


def draw_mountains(d, w, horizon, seed):
    """Wireframe ridges - Battlezone's horizon."""
    rnd = random.Random(seed)
    for layer, (amp, col) in enumerate(((0.10, GRID_DIM), (0.16, GREEN_DIM))):
        pts, x = [], 0
        base = horizon - layer * 6
        while x <= w:
            pts.append((x, base - int(rnd.random() * amp * horizon)))
            x += int(w * (0.035 + rnd.random() * 0.04))
        pts.append((w, base))
        d.line(pts, fill=col, width=1)


def draw_cabinet(d, cx, floor_y, s):
    """An arcade cabinet, drawn from its FOOT so it stands on the grid floor.

    The first attempt positioned it by its top-left corner and it floated above
    the horizon like a packing crate. Cabinets have three things that make them
    read instantly: a marquee that overhangs, a screen set back behind a bezel,
    and a control panel that juts forward. All three are here.
    """
    W = int(64 * s)                 # body width
    H = int(112 * s)                # body height
    dx, dy = int(15 * s), int(-11 * s)
    x, y = cx - W // 2, floor_y - H

    # Body, with the side face receding.
    d.polygon([(x + W, y), (x + W + dx, y + dy),
               (x + W + dx, floor_y + dy), (x + W, floor_y)],
              fill=(0, 8, 0), outline=GREEN_DIM)
    d.polygon([(x, y), (x + W, y), (x + W, floor_y), (x, floor_y)],
              fill=BLACK, outline=GREEN)

    # Marquee: overhangs the body, brightest thing on the cabinet.
    mh = int(16 * s)
    d.polygon([(x - int(3 * s), y - mh), (x + W + int(3 * s), y - mh),
               (x + W + int(3 * s), y), (x - int(3 * s), y)],
              fill=(0, 26, 0), outline=GREEN_HOT)
    d.polygon([(x + W + int(3 * s), y - mh), (x + W + int(3 * s) + dx, y - mh + dy),
               (x + W + int(3 * s) + dx, y + dy), (x + W + int(3 * s), y)],
              outline=GREEN_DIM)

    # Screen behind a bezel.
    bx0, by0 = x + int(7 * s), y + int(9 * s)
    bx1, by1 = x + W - int(7 * s), y + int(52 * s)
    d.rectangle([bx0, by0, bx1, by1], outline=GREEN_DIM)
    d.rectangle([bx0 + 3, by0 + 3, bx1 - 3, by1 - 3], fill=(0, 14, 0),
                outline=GREEN_HOT)
    # A game in progress: invaders above, ship below, shot between.
    for i in range(4):
        ix = bx0 + int(9 * s) + i * int(11 * s)
        d.rectangle([ix, by0 + int(9 * s), ix + int(6 * s), by0 + int(13 * s)],
                    fill=GREEN)
    shipx = (bx0 + bx1) // 2
    d.rectangle([shipx - int(6 * s), by1 - int(10 * s),
                 shipx + int(6 * s), by1 - int(6 * s)], fill=GREEN_HOT)
    d.line([shipx, by1 - int(11 * s), shipx, by0 + int(20 * s)], fill=GREEN_HOT)

    # Control panel: juts forward, so it gets its own slab.
    px0, py0 = x - int(4 * s), y + int(58 * s)
    px1, py1 = x + W + int(4 * s), y + int(70 * s)
    d.polygon([(px0, py0), (px1, py0), (px1, py1), (px0, py1)],
              fill=(0, 10, 0), outline=GREEN)
    d.ellipse([px0 + int(11 * s), py0 + int(3 * s),
               px0 + int(20 * s), py0 + int(9 * s)], outline=GREEN_HOT)
    for b in range(3):
        bx = px0 + int(30 * s) + b * int(10 * s)
        d.ellipse([bx, py0 + int(4 * s), bx + int(6 * s), py0 + int(9 * s)],
                  outline=GREEN)

    # Coin door and a shadow so it sits ON the floor rather than in front of it.
    d.rectangle([x + int(24 * s), y + int(84 * s),
                 x + int(40 * s), y + int(92 * s)], outline=GREEN_DIM)
    d.line([x - int(2 * s), floor_y, x + W + dx, floor_y], fill=GREEN_HOT)


def draw_highscores(d, x, y, w, rows):
    """A high-score table. Fills the dead space the first draft had, and it is
    the other instantly-legible arcade signifier besides the cabinet."""
    f_h = font(max(11, w // 26), bold=True)
    f_r = font(max(11, w // 28))
    d.text((x, y), 'HIGH SCORES', font=f_h, fill=GREEN_HOT)
    d.line([x, y + 22, x + w, y + 22], fill=GREEN_DIM)
    for i, (initials, score) in enumerate(rows):
        ry = y + 32 + i * 20
        d.text((x, ry), f'{i + 1}', font=f_r, fill=GREEN_DIM)
        d.text((x + 26, ry), initials, font=f_r, fill=GREEN)
        sw = d.textbbox((0, 0), score, font=f_r)
        d.text((x + w - (sw[2] - sw[0]), ry), score, font=f_r, fill=GREEN)


def draw_hud(d, w, h, bay, label):
    """A scoreboard strip - period-correct chrome, and a place for the machine
    name so a screen identifies itself across a room."""
    f_small = font(13)
    f_big = font(20, bold=True)
    y = h - 34
    d.line([0, y - 12, w, y - 12], fill=GRID_DIM)
    d.text((bay['x'], y), '1UP', font=f_small, fill=GREEN_HOT)
    d.text((bay['x'] + 44, y), 'READY', font=f_small, fill=GREEN)
    right = f'{label}'
    bb = d.textbbox((0, 0), right, font=f_big)
    d.text((w - (bb[2] - bb[0]) - 24, y - 6), right, font=f_big, fill=GREEN)


def draw_title(d, w, h, bay):
    x = bay['x'] + bay['width'] + int(w * 0.06)
    y = int(h * 0.13)
    d.text((x, y), 'RETRO', font=font(int(h * 0.085), bold=True), fill=GREEN_HOT)
    d.text((x, y + int(h * 0.085)), 'FLEET', font=font(int(h * 0.085), bold=True),
           fill=GREEN)
    d.text((x + 4, y + int(h * 0.185)), 'INSERT COIN TO CONTINUE',
           font=font(int(h * 0.021)), fill=GREEN_DIM)


def scanlines_and_glow(img, w, h):
    """Scanlines plus a bloom pass. The bloom is what makes it read as phosphor
    rather than as flat clip-art; without it the greens look like plastic."""
    glow = img.filter(ImageFilter.GaussianBlur(radius=max(2, w // 500)))
    img = Image.blend(img, glow, 0.34)

    d = ImageDraw.Draw(img, 'RGBA')
    for y in range(0, h, 3):
        d.line([0, y, w, y], fill=(0, 0, 0, 54))

    # Vignette: darken toward the corners so the centre carries the eye.
    vig = Image.new('L', (w, h), 0)
    vd = ImageDraw.Draw(vig)
    vd.ellipse([-int(w * 0.22), -int(h * 0.30),
                int(w * 1.22), int(h * 1.30)], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(radius=max(24, w // 22)))
    return Image.composite(img, Image.new('RGB', (w, h), BLACK), vig)


def generate(w, h, label, seed=7, n_icons=None):
    img = Image.new('RGB', (w, h), BLACK)
    d = ImageDraw.Draw(img)
    bay = icon_bay(w, h, n_icons)

    horizon = int(h * 0.62)
    draw_grid_floor(d, w, h, horizon)
    draw_mountains(d, w, horizon, seed)
    draw_title(d, w, h, bay)
    # Stand the cabinet ON the floor, a little in front of the horizon so the
    # grid runs under it and the perspective reads.
    cab_s = h / 300.0
    draw_cabinet(d, int(w * 0.80), horizon + int(h * 0.115), cab_s)
    draw_highscores(d, bay['x'] + bay['width'] + int(w * 0.06),
                    int(h * 0.44), int(w * 0.20),
                    [('AAA', '999990'), ('CPU', '874200'), ('P-3', '651100'),
                     ('440', '498050'), ('SB1', '332700')])

    img = scanlines_and_glow(img, w, h)

    # The bay goes on AFTER the CRT pass so the icon area stays clean and high
    # contrast - the effects are for the art, not for the part you have to read.
    d = ImageDraw.Draw(img)
    draw_bay(d, bay, w, h)
    draw_hud(d, w, h, bay, label)
    return img, bay


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--label', default='RETRO FLEET')
    ap.add_argument('--sizes', default='800x600,1024x768,1280x1024,1600x1200,'
                                       '1280x800,1440x900,1920x1080')
    ap.add_argument('--icons', type=int, default=None,
                    help='expected desktop shortcut count. The bay is DRAWN '
                         'wide enough to hold this many, matching the widening '
                         'the agent does at arrange time. Omit and it is '
                         'counted from the staged library; pass 0 to force the '
                         'base panel.')
    ap.add_argument('--library', default=DEFAULT_LIBRARY,
                    help='staged library, used to count shortcuts for --icons')
    a = ap.parse_args()

    n_icons = a.icons
    if n_icons is None:
        n_icons = count_staged_shortcuts(a.library)
        if n_icons:
            print(f'  counted {n_icons} shortcuts in {a.library}')
        else:
            print(f'  library not readable ({a.library}) - drawing the base '
                  f'bay. Pass --icons N if the fleet has more shortcuts than '
                  f'it holds, or the art will not match the layout.')
    n_icons = n_icons or None
    os.makedirs(a.out, exist_ok=True)

    geom = {}
    for spec in a.sizes.split(','):
        w, h = (int(v) for v in spec.lower().split('x'))
        img, bay = generate(w, h, a.label, n_icons=n_icons)
        # BMP: XP sets a BMP wallpaper without involving any image decoder, which
        # is one less thing to fail on a fresh install.
        path = os.path.join(a.out, f'retrowall_{w}x{h}.bmp')
        img.save(path, 'BMP')
        geom[f'{w}x{h}'] = bay
        print(f'  {w}x{h}  {os.path.getsize(path) // 1024} KB  '
              f'bay {bay["cols"]}x{bay["rows"]} cells at ({bay["x"]},{bay["y"]})')

    # The arranger reads this so the two can never disagree about where icons go.
    with open(os.path.join(a.out, 'icon_bay.json'), 'w') as fh:
        json.dump(geom, fh, indent=1, sort_keys=True)
    print(f'  geometry -> {os.path.join(a.out, "icon_bay.json")}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
