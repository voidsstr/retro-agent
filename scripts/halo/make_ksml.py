#!/usr/bin/env python3
r"""Generate Halo's missing 1080-line Keystone layout files.

WHY.  Halo picks its in-game overlay layout by VERTICAL resolution: it opens
`content\<height>log.ksml` and `content\<height>editbox.ksml`.  Bungie shipped
480, 576, 600, 720, 768, 864, 900, 960, 1024 and 1200 -- 1920x1080 did not
exist as a PC mode in 2003, so there is no 1080 pair.  Launch Halo at 1080 and
Keystone paints a grey panel over the main menu:

    404 Error!
    There was an error trying to open the KSML file:
    C:\Games\Halo\content\1080log.ksml
    File Not Found!

Four of the fleet's eight boxes are 1920x1080 panels and the staged launcher
picks the panel's native mode, so this hits half the fleet.  The fix belongs in
the staged tree, not on one box.

HOW.  The 1200 pair is scaled: vertically by 1080/1200, and horizontally by
1920/1600 (the 1200 layout assumes a 1600-wide screen; ours is 1920 wide, and
the shipped 900 layout confirms the width field is the real screen width, not
4/3 of the height).  The editbox background PNG is scaled to match, because
every shipped editboxNNNN.png is exactly as wide as its ksml's width field.

    python3 make_ksml.py "/path/to/Games-Library/Halo"
"""
import os
import sys

# from 1200log.ksml / 1200editbox.ksml, scaled: y x 0.9, x x 1.2
LOG = ('<ksml>\r\n  <head/>\r\n<body>\r\n'
       '  <listbox left="5" top="657" width="1920" height="216" id="oListbox" \r\n'
       '           textbackground="#00000000" outerborder="#00000000" \r\n'
       '           innerborder="#00000000" shadow="#00000000" highlight="#00000000" \r\n'
       '          textcolor="#ffffffff" font="Arial Narrow-16-bold"\t\r\n'
       '/>\r\n</body>\r\n</ksml>\r\n')

EDITBOX = ('<ksml>\r\n  <head/>\r\n<body>\r\n\r\n'
           '  <editbox width="1272" left="264" top="1044" background="gallery\\editbox1080.png"\r\n'
           '\tid="oEditbox" ime="false" tabindex="0" maxlength="40"\r\n'
           '\tfont="Arial Narrow-16-bold" color="#ffffffff"\r\n'
           '\t/>          \r\n'
           '  <label left="0" top="1048" width="264" text="Prompt" id="oPrompt" \r\n'
           '\tfont="Arial Narrow-16-bold" color="#ffffffff"\r\n'
           '\t/>    \r\n</body>\r\n</ksml>\r\n')


def write_utf16(path, text):
    """Keystone reads these as UTF-16LE with a BOM - match the shipped files."""
    with open(path, "wb") as f:
        f.write(b"\xff\xfe" + text.encode("utf-16-le"))


def main(tree):
    content = os.path.join(tree, "CONTENT")
    if not os.path.isdir(content):
        content = os.path.join(tree, "content")
    gallery = os.path.join(content, "GALLERY")
    if not os.path.isdir(gallery):
        gallery = os.path.join(content, "gallery")
    if not os.path.isdir(gallery):
        sys.exit("no CONTENT/GALLERY under %s" % tree)

    write_utf16(os.path.join(content, "1080log.ksml"), LOG)
    write_utf16(os.path.join(content, "1080editbox.ksml"), EDITBOX)

    from PIL import Image
    src = os.path.join(gallery, "editbox1200.png")
    im = Image.open(src).convert("RGBA").resize((1272, 32), Image.LANCZOS)
    im.save(os.path.join(gallery, "editbox1080.png"), optimize=True)

    for n in ("1080log.ksml", "1080editbox.ksml"):
        print("wrote", os.path.join(content, n))
    print("wrote", os.path.join(gallery, "editbox1080.png"), "1272x32")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
