"""scripts/vm/win98/sweep.py - the Win98 build VM's launch sweep judges frames from 86Box's own display.

A sweep that calls a title 'renders' when nothing appeared would certify a
broken launcher - so the three verdicts are pinned: unchanged desktop, black,
and a real change. The emulated area excludes 86Box's own menu and status
bars, which differ between captures (the clock, the activity LEDs) and would
otherwise make an untouched desktop read as 'renders'.
"""
import importlib.util
import os

from PIL import Image, ImageDraw

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("sweep", os.path.join(REPO, "scripts", "vm", "win98", "sweep.py"))
sw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sw)


def _frame(fill, chrome="gray"):
    im = Image.new("RGB", (667, 579), fill)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 666, 55), fill=chrome)          # 86Box menu + toolbar
    d.rectangle((0, 553, 666, 578), fill=chrome)       # status bar
    return im


def test_an_untouched_desktop_is_not_a_render():
    base = sw.emulated(_frame("teal"))
    changed_chrome = _frame("teal", chrome="white")       # only 86Box's bars differ
    assert sw.judge(changed_chrome, base) == "desktop"


def test_black_and_a_real_change():
    base = sw.emulated(_frame("teal"))
    assert sw.judge(_frame("black"), base) == "black"
    game = _frame("teal")
    ImageDraw.Draw(game).rectangle((100, 100, 500, 400), fill="red")
    assert sw.judge(game, base) == "renders"


def test_shortcut_targets_parse():
    pif = bytearray(0x200)
    pif[0x24:0x24 + 30] = b"C:\\GAMES\\QUAKE1\\PLAYQU~4.BAT\0"
    assert sw.pif_target(bytes(pif)) == "C:\\GAMES\\QUAKE1\\PLAYQU~4.BAT"
    lnk = b"L\0\0\0junk C:\\Games\\Quake2Win9x\\Play Quake II.bat\0more"
    assert sw.lnk_target(lnk) == "C:\\Games\\Quake2Win9x\\Play Quake II.bat"
