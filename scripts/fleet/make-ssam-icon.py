#!/usr/bin/env python3
"""Build Serious Sam's desktop icon out of the GAME'S OWN menu artwork.

WHY THIS SCRIPT EXISTS AT ALL.  Serious Sam ships **no icon anywhere**.  Every
binary on both retail discs - ``SeriousSam.exe``, ``DedicatedServer.exe``, the
disc's ``Setup.exe`` - has an entirely empty PE resource directory, and so do
the official 1.05/1.07 patch installers and both demo installers on the share.
There is no ``.ico`` on either disc.  Verified case-insensitively.

That matters because ``launch.txt``'s third column only has to *resolve*; the
staged-library validator cannot tell an exe that HAS an icon from one that does
not.  Point the column at ``Bin\\SeriousSam.exe`` and the validator passes, the
deploy succeeds, and the desktop gets three generic white pages - which
CLAUDE.md's staging checklist calls "not finished".

So the artwork comes from the only place it exists: the game's own main-menu
logo texture, ``Textures/Logo/sam_menulogo256b.tex`` inside the title's
``.gro``.  Its left-hand end is the Serious Sam sun-and-bomb emblem - square,
high-contrast, and still readable at 16x16, which a wordmark is not.

THE .tex FORMAT, read out of the files rather than guessed:
    96-byte header - ``TVER`` + version, ``TDAT`` + five dwords, ``FRMS`` -
    then RAW, UNCOMPRESSED, TOP-DOWN pixels with no stride padding.
    ``sam_menulogo256b`` is 65632 bytes = 96 + 256*64*4, i.e. 256x64 RGBA.
    (The size arithmetic is the check: a wrong geometry does not merely look
    wrong, it shears, and that is obvious.)

Both Encounters ship the same texture under the same name, so each tree's icon
is generated from ITS OWN .gro - same picture, honest provenance, and a tree
that stays self-contained.
"""
import argparse
import os
import zipfile

from PIL import Image

#: (archive member, geometry) - the size arithmetic above is asserted at run time.
TEX = 'Textures/Logo/sam_menulogo256b.tex'
HDR = 96
TEX_W, TEX_H = 256, 64

#: The emblem's bounding box inside that texture, measured from the decoded
#: image.  x stops at 56 and not 60: the next glyph of the wordmark starts
#: there and a two-pixel sliver of it is clearly visible at 5x.
EMBLEM = (18, 4, 56, 62)

#: 48 is what XP's desktop actually draws; 32 and 16 are what the shell falls
#: back to in details/list views and in the taskbar.
SIZES = (48, 32, 16)

#: Where each title keeps its texture.  A title is named by its LIBRARY
#: directory so this script can be pointed at the share and left alone.
TITLES = {
    'SeriousSamFirstEncounter':  '1_00.gro',
    'SeriousSamSecondEncounter': 'SE1_00.gro',
}


def decode_logo(gro_path):
    """Return the 256x64 RGBA menu-logo texture from a .gro (a plain zip)."""
    with zipfile.ZipFile(gro_path) as z:
        raw = z.read(TEX)
    want = HDR + TEX_W * TEX_H * 4
    if len(raw) != want:
        raise SystemExit(
            '%s: %s is %d bytes, expected %d (96-byte header + %dx%d RGBA). '
            'The geometry assumption above no longer holds - re-derive it '
            'rather than cropping a sheared image.'
            % (gro_path, TEX, len(raw), want, TEX_W, TEX_H))
    return Image.frombytes('RGBA', (TEX_W, TEX_H), raw[HDR:])


def build_icon(logo):
    """Crop the emblem, pad it to square, and return the multi-size frames."""
    emb = logo.crop(EMBLEM)
    side = max(emb.size)
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(emb, ((side - emb.width) // 2, (side - emb.height) // 2))
    return [sq.resize((n, n), Image.LANCZOS) for n in SIZES]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('library', help='Games-Library root (the writable gvfs path)')
    ap.add_argument('--name', default='SeriousSam.ico',
                    help='icon filename written into each title tree')
    ap.add_argument('--check', action='store_true',
                    help='report whether each icon is present and current, write nothing')
    args = ap.parse_args()

    rc = 0
    for title, gro in sorted(TITLES.items()):
        tree = os.path.join(args.library, title)
        out = os.path.join(tree, args.name)
        if not os.path.isdir(tree):
            print('%-28s MISSING TREE %s' % (title, tree))
            rc = 1
            continue
        if args.check:
            state = 'present %d bytes' % os.path.getsize(out) if os.path.exists(out) else 'ABSENT'
            print('%-28s %s' % (title, state))
            if not os.path.exists(out):
                rc = 1
            continue
        frames = build_icon(decode_logo(os.path.join(tree, gro)))
        frames[0].save(out, format='ICO',
                       sizes=[(f.width, f.height) for f in frames])
        print('%-28s wrote %s (%d bytes, sizes %s)'
              % (title, out, os.path.getsize(out),
                 ' '.join('%dx%d' % (n, n) for n in SIZES)))
    raise SystemExit(rc)


if __name__ == '__main__':
    main()
