"""provisioning/patches/idtech3-kin/apply.py - Jedi Academy, SoF2 and RTCW get
the panel's own mode (r_mode -1) into their in-game video menus, and SoF2 MP's
r_mode minimum is patched from 3.0f to -1.0f.

The pure logic is tested on synthetic input (no share, no game data): the exe
patch table, every menu transform on sample menus written in each engine's own
dialect, the pk3 search-order rule, the font parsers, and the publish /
install-server safety rules against temporary directories.

The share-dependent half re-runs --check and --build against the staged
originals and SKIPS LOUDLY when /mnt/retro-share is not mounted."""
import hashlib
import importlib.util
import io
import json
import os
import struct
import sys
import zipfile

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
APPLY = os.path.join(REPO, "provisioning", "patches", "idtech3-kin", "apply.py")


def _load():
    spec = importlib.util.spec_from_file_location("idtech3_kin_apply", APPLY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


A = _load()

# A flat font: every glyph advances 10 units. Positions are then easy to reason about.
FLAT = [10.0] * 256

# --- sample menus, one per dialect (shapes copied from the staged originals) --
JKA_MENU = (
    "{\r\n\tmenuDef\r\n\t{\r\n\t\tname\t\"setupMenu\"\r\n"
    "\t\titemDef \r\n\t\t{\r\n\t\t\tname\t\tgraphics\r\n\t\t\tgroup\t\tvideo\r\n"
    "\t\t\ttype\t\tITEM_TYPE_MULTI\r\n\t\t\tcvar\t\t\"ui_r_glCustom\"\r\n"
    "\t\t\tcvarFloatList { @MENUS_HIGH_QUALITY 0 @MENUS_CUSTOM 4 }\r\n"
    "\t\t\taction\r\n\t\t\t{\r\n\t\t\t\tplay \"sound/interface/button1.wav\" ; \r\n"
    "\t\t\t\tuiScript \t\tupdate \"ui_r_glCustom\" ;\r\n"
    "\t\t\t\tsetcvar ui_r_modified 1 ;\r\n\t\t\t}\r\n\t\t}\r\n"
    "\t\titemDef \r\n\t\t{\t\r\n\t\t\tname\t\tvideo_mode\r\n\t\t\tgroup\t\tvideo\r\n"
    "\t\t\ttype\t\tITEM_TYPE_MULTI\r\n\t\t\ttext\t\t@MENUS_VIDEO_MODE\r\n"
    "\t\t\tcvarFloatList\t\t{  @MENUS_640_X_480 3 @MENUS_800_X_600 4 @MENUS_2400_X_600 12 }\r\n"
    "\t\t\tcvar\t\t\"ui_r_mode\"\r\n\r\n\t\t\trect\t\t260 216 340 14\r\n"
    "\t\t\ttextalign\t\tITEM_ALIGN_RIGHT\r\n\t\t\ttextalignx\t\t174\r\n"
    "\t\t\ttextaligny\t\t0\r\n\t\t\tfont \t\t4\r\n\t\t\ttextscale \t\t1\r\n"
    "\t\t\tforecolor\t\t.615 .615 .956 1\r\n\t\t\tvisible\t\t0\r\n"
    "\t\t\taction\r\n\t\t\t{\r\n\t\t\t\tuiScript glCustom ;\r\n\t\t\t}\r\n\t\t}\r\n"
    "\t}\r\n}\r\n")

RTCW_MENU = (
    "#include \"ui/menudef.h\"\r\n{\r\nmenuDef {\r\n\tname \"system_menu\"\r\n"
    "itemDef {\r\n\tname graphics\r\n\tgroup grpSystem\r\n\ttext \"Quality:\"\r\n"
    "\ttype ITEM_TYPE_MULTI\r\n\tcvar \"ui_glCustom\"\r\n"
    "\tcvarFloatList { \"High Quality\" 0 \"Custom\" 4 }\r\n"
    "\taction { uiScript update \"ui_glCustom\" }\r\n\t}\r\n"
    "\titemDef {\r\n\t\tname graphics\r\n\t\tgroup grpSystem\r\n"
    "\t\ttype ITEM_TYPE_MULTI\r\n\t\ttext \"Video Mode:\"\r\n\t\tcvar \"r_mode\"\r\n"
    "\t\tcvarFloatList { \"640x480\" 3 \"800x600\" 4 \"856x480 wide screen\" 11 }\r\n"
    "//\t\t\"320x240\" 0 \"400x300\" 1 \"512x384\" 2\r\n"
    "\t\trect 82 85 290 12\r\n      \ttextalign ITEM_ALIGN_RIGHT\r\n"
    "      \ttextalignx 142\r\n      \ttextaligny 10\r\n\t\ttextscale .23 \r\n"
    "\t\tstyle WINDOW_STYLE_FILLED\r\n\t\tbackcolor 1 1 1 .07\r\n"
    "      \tforecolor 1 1 1 1\r\n      \tvisible 0 \r\n"
    "\t\taction { uiScript glCustom }\r\n    \t\t}\r\n}\r\n}\r\n")

SOF2_MENU = (
    "{\r\n\tmenuDef \r\n\t{\r\n\t\titemDef \r\n\t\t{\r\n"
    "\t\t\tname\t\tquality_multi\r\n\t\t\ttype\t\tITEM_TYPE_MULTI\r\n"
    "\t\t\tcvar\t\t\"ui_glCustom\"\r\n"
    "\t\t\tcvarFloatList \r\n\t\t\t{ \r\n\t\t\t\t\"Normal\"\t\t1 \r\n\t\t\t}\r\n"
    "\t\t\taction \r\n\t\t\t{ \r\n\t\t\t\tplay \"sound/misc/menus/select.wav\" ; \r\n"
    "\t\t\t\tuiScript update \"ui_glCustom\" \r\n\t\t\t}\r\n\t\t}\r\n"
    "\t\titemDef \r\n\t\t{\r\n\t\t\tname\t\tvideomode_multi\r\n"
    "\t\t\tstyle\t\tWINDOW_STYLE_FILLED\r\n\t\t\ttype\t\tITEM_TYPE_MULTI\t\r\n"
    "\t\t\ttext\t\t\"Video Mode:\"\r\n\t\t\tcvar\t\t\"r_mode\"\r\n"
    "\t\t\trect\t\t50 79 240 15\r\n\t\t\ttextalign\tITEM_ALIGN_RIGHT\r\n"
    "\t\t\ttextalignx\t125\r\n\t\t\ttextaligny\t0\r\n\t\t\ttextfont\t\"hud\"\r\n"
    "\t\t\ttextscale\t.43\r\n\t\t\tforecolor\t.12 .14 .08 1\r\n\t\t\tvisible\t\t1\r\n\r\n"
    "\t\t\tcvarFloatList \r\n\t\t\t{ \r\n\t\t\t\t\"640*480\" 3 \r\n"
    "\t\t\t\t\"1600*1200\" 9 \r\n\t\t\t}\r\n"
    "\t\t\taction \r\n\t\t\t{ \r\n\t\t\t\tuiScript glCustom \r\n\t\t\t}\r\n\t\t}\r\n"
    "\t}\r\n}\r\n")

JKA_SPEC = dict(A.JKA_MENU)
RTCW_SPEC = dict(A.RTCW_MENU)
SOF2_SPEC = dict(A.SOF2_MENU)


# ---------------------------------------------------------------------------
# SoF2 MP exe patch
# ---------------------------------------------------------------------------

def _fake_exe():
    buf = bytearray(os.urandom(0xBA700))
    off, ctx = A.SOF2MP_CONTEXT
    buf[off:off + len(ctx)] = ctx
    return bytes(buf)


def test_patch_bytes_are_the_floats_the_review_named():
    (off, old, new), = A.SOF2MP_PATCH
    assert off == 0xBA61A
    assert old == struct.pack("<f", 3.0)          # stock minimum
    assert new == struct.pack("<f", -1.0)         # r_mode -1 allowed
    # the context is push 10.0f / push 3.0f / push 0x2021 and the 3.0f operand
    # is exactly the patched dword
    coff, ctx = A.SOF2MP_CONTEXT
    assert ctx[0] == 0x68 and ctx[5] == 0x68 and ctx[10] == 0x68
    assert ctx[1:5] == struct.pack("<f", 10.0)
    assert coff + 6 == off and ctx[6:10] == old


def test_exe_patch_changes_exactly_two_bytes():
    data = _fake_exe()
    out = A.patch_exe_bytes(data)
    diff = [i for i in range(len(data)) if data[i] != out[i]]
    assert diff == [0xBA61C, 0xBA61D]
    assert out[0xBA61A:0xBA61E] == bytes.fromhex("000080bf")
    assert len(out) == len(data)


def test_exe_patch_refuses_a_foreign_binary():
    data = bytearray(_fake_exe())
    data[0xBA614] = 0x90                                  # context broken
    with pytest.raises(ValueError):
        A.patch_exe_bytes(bytes(data))


def test_exe_patch_refuses_an_already_patched_binary():
    once = A.patch_exe_bytes(_fake_exe())
    with pytest.raises(ValueError):                       # context now reads -1.0
        A.patch_exe_bytes(once)


# ---------------------------------------------------------------------------
# Menu transforms
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("text,spec,label", [
    (JKA_MENU, JKA_SPEC, "@MENUS_CUSTOM -1"),
    (RTCW_MENU, RTCW_SPEC, '"Custom" -1'),
    (SOF2_MENU, SOF2_SPEC, '"Custom" -1'),
])
def test_menu_gains_custom_entry_overlay_and_preset_reassert(text, spec, label):
    new, info = A.patch_q3_menu(text, spec, FLAT)
    assert label in new
    assert info["list_values"][-1] == -1.0
    assert info["list_values"].count(-1.0) == 1
    assert info["items_added"] == 3
    assert info["presets_reasserted"] >= 1
    # the overlay: read-only, shown only on -1, never takes the mouse
    assert new.count('showCvar\t{ "-1" }') == 3
    assert new.count('cvarTest\t"%s"' % spec["mode_cvar"]) == 3
    assert '"r_customwidth"' in new and '"r_customheight"' in new
    assert new.count("decoration") - text.count("decoration") == 3
    for m in __import__("re").finditer(r"rect\t(\d+) (\d+) 0 (\d+)", new):
        assert int(m.group(1)) > 0
    # the preset re-assert quotes -1 (a bare -1 is lexed as '-' '1')
    assert 'setcvar %s "-1"' % spec["mode_cvar"] in new
    assert "setcvar %s -1" % spec["mode_cvar"] not in new
    A.check_balanced(new)


@pytest.mark.parametrize("text,spec", [(JKA_MENU, JKA_SPEC), (RTCW_MENU, RTCW_SPEC),
                                       (SOF2_MENU, SOF2_SPEC)])
def test_crlf_is_kept(text, spec):
    new, _ = A.patch_q3_menu(text, spec, FLAT)
    assert "\n" not in new.replace("\r\n", "")


@pytest.mark.parametrize("text,spec", [(JKA_MENU, JKA_SPEC), (RTCW_MENU, RTCW_SPEC),
                                       (SOF2_MENU, SOF2_SPEC)])
def test_second_application_is_refused(text, spec):
    once, _ = A.patch_q3_menu(text, spec, FLAT)
    with pytest.raises(ValueError):
        A.patch_q3_menu(once, spec, FLAT)


def test_overlay_copies_the_rows_show_hide_handle():
    # RTCW shows a tab by NAME ("show graphics"), SoF2 recolours by name;
    # an overlay with another name would stay hidden / never highlight.
    new, _ = A.patch_q3_menu(RTCW_MENU, RTCW_SPEC, FLAT)
    blocks = [b for _, _, b in A.item_blocks(new) if "fleet:" in b]
    assert len(blocks) == 3
    for b in blocks:
        assert A.attr(b, "name") == "graphics"
        assert A.attr(b, "group") == "grpSystem"
        assert A.attr(b, "style") is None            # no background painted
        assert A.attr(b, "visible") == "0"


def test_multiline_list_keeps_its_closing_brace_line():
    new, _ = A.patch_q3_menu(SOF2_MENU, SOF2_SPEC, FLAT)
    assert '\t\t\t\t"1600*1200" 9 \r\n\t\t\t\t"Custom" -1\r\n\t\t\t}\r\n' in new


def test_single_line_list_gets_the_entry_before_its_brace():
    new = A.add_custom_entry(JKA_MENU, "ui_r_mode", "@MENUS_CUSTOM")
    assert "@MENUS_2400_X_600 12 @MENUS_CUSTOM -1 }" in new


def test_comment_after_the_list_does_not_confuse_the_parser():
    # RTCW's SP menus carry a // line of disabled modes after the list
    _, _, body = A.find_mode_item(RTCW_MENU, "r_mode")
    assert A.list_values(body) == [3.0, 4.0, 11.0]


def test_overlay_positions_follow_the_value():
    _, _, body = A.find_mode_item(JKA_MENU, "ui_r_mode")
    xs, rect = A.overlay_positions(body, FLAT, 1.0, "Custom")
    value_x = 260 + 174 + 8
    assert xs[0] >= value_x + 60                      # after "Custom" (6 x 10)
    assert xs[0] < xs[1] < xs[2]
    assert xs[1] - xs[0] >= 40                        # a 4-digit width fits
    assert rect[1] == 216


def test_left_aligned_mode_item_is_refused():
    bad = JKA_MENU.replace("ITEM_ALIGN_RIGHT", "ITEM_ALIGN_LEFT")
    with pytest.raises(ValueError):
        A.patch_q3_menu(bad, JKA_SPEC, FLAT)


def test_two_mode_lists_are_refused():
    s, e, body = A.find_mode_item(RTCW_MENU, "r_mode")
    doubled = RTCW_MENU[:e] + "\r\n" + body + RTCW_MENU[e:]
    with pytest.raises(ValueError):
        A.find_mode_item(doubled, "r_mode")


def test_preset_reassert_counts_every_preset_action():
    t = RTCW_MENU + '\r\naction { setcvar ui_glCustom 0; uiScript update "ui_glCustom" }\r\n'
    new, n = A.keep_custom_after_presets(t, "ui_glCustom", "r_mode")
    assert n == 2


def test_check_balanced_catches_a_broken_menu():
    with pytest.raises(ValueError):
        A.check_balanced("itemDef { name x { }")
    with pytest.raises(ValueError):
        A.check_balanced('itemDef { text "unterminated }')
    A.check_balanced('itemDef { text "a } b" // } \n }')


# --- SoF2 single player (RMF + Strip) ---------------------------------------

RMF = ('<list "&MENU_VIDEO_VIDMODES&" atext "&MENU_VIDEO_RESOLUTION& : " \r\n'
       'cvar menu_mode match "3,4,6,7,8,9" key mouse1 "set x 1"><hbr>\r\n')
STRIP = ('INDEX 21\r\n{\r\n   REFERENCE VIDMODES\r\n   NOTES "29 Mar 02"\r\n'
         '   TEXT_LANGUAGE1 "640x480,800x600,1024x768,1152x864,1280x1024,1600x1200"\r\n}\r\n'
         'INDEX 22\r\n{\r\n   REFERENCE TRANS_TEXT\r\n   TEXT_LANGUAGE1 "Alpha"\r\n}\r\n')


def test_rmf_match_gains_minus_one():
    out = A.patch_rmf_match(RMF)
    assert 'match "3,4,6,7,8,9,-1"' in out
    with pytest.raises(ValueError):
        A.patch_rmf_match(out)


def test_strip_labels_stay_one_per_match_value():
    out = A.patch_strip_vidmodes(STRIP)
    labels = out.split('TEXT_LANGUAGE1 "')[1].split('"')[0].split(",")
    values = A.patch_rmf_match(RMF).split('match "')[1].split('"')[0].split(",")
    assert len(labels) == len(values) == 7
    assert labels[-1] == "Custom" and values[-1] == "-1"
    assert 'TEXT_LANGUAGE1 "Alpha"' in out                # other entries untouched
    with pytest.raises(ValueError):
        A.patch_strip_vidmodes(out)


def test_strip_with_other_languages_is_refused():
    two = STRIP.replace('   NOTES', '   TEXT_LANGUAGE2 "x"\r\n   NOTES')
    with pytest.raises(ValueError):
        A.patch_strip_vidmodes(two)


# ---------------------------------------------------------------------------
# Search order, fonts, pk3
# ---------------------------------------------------------------------------

def test_the_last_name_case_insensitively_serves_a_member():
    names = ["assets0.pk3", "Assets1.pk3", "assets3.pk3"]
    lists = {"assets0.pk3": ["ui/x.menu"], "Assets1.pk3": ["UI/X.MENU"],
             "assets3.pk3": ["other"]}
    assert A.winning_pk3(names, lists, "ui/x.menu") == "Assets1.pk3"
    lists["assets3.pk3"].append("ui/X.menu")
    assert A.winning_pk3(names, lists, "ui/x.menu") == "assets3.pk3"
    assert A.winning_pk3(names, lists, "absent") is None


@pytest.mark.parametrize("stock", [
    ["assets0.pk3", "assets1.pk3", "assets2.pk3", "assets3.pk3"],
    ["maps.pk3", "models.pk3", "mp.pk3", "musicandsound.pk3", "skins.pk3",
     "textures0.pk3", "textures1234.pk3", "therest.pk3", "update101.pk3",
     "update102.pk3", "update103.pk3"],
    ["mp_bin.pk3", "mp_pak0.pk3", "mp_pak5.pk3", "mp_pakmaps6.pk3", "pak0.pk3",
     "sp_pak1.pk3", "sp_pak4.pk3"],
])
def test_zz_name_sorts_after_every_stock_pk3(stock):
    # the three staged game directories as listed on 2026-09-29
    assert sorted(stock + [A.PK3_NAME], key=A.pk3_sort_key)[-1] == A.PK3_NAME


def test_pk3_order_is_fs_pathcmp_not_lower_case():
    """All six engines sort pk3 names with FS_PathCmp, which UPPERCASES a-z
    (jamp 0x43A60B, jasp 0x41CD39, sof2mp 0x44DC22, SoF2 0x1003A478, WolfMP
    0x4234DB, WolfSP 0x4200A6). '_' is 0x5F and 'Z' 0x5A, so '_' sorts AFTER
    every letter - the reverse of a lower() sort. The first build used lower()
    and asserted that zz_fleet_video.pk3 beats Z_mod.pk3; in the engine
    Z_mod.pk3 wins."""
    names = ["zz_fleet_video.pk3", "Z_mod.pk3", "zzz.pk3", "assets3.pk3"]
    engine = sorted(names, key=A.pk3_sort_key)
    assert engine == ["assets3.pk3", "zzz.pk3", "zz_fleet_video.pk3", "Z_mod.pk3"]
    assert sorted(names, key=str.lower) != engine          # the old key was wrong
    assert A.winning_pk3(names, {n: ["ui/x.menu"] for n in names}, "ui/x.menu") == "Z_mod.pk3"
    # separators: '\\' and ':' read as '/'
    assert A.pk3_sort_key("a\\b") == A.pk3_sort_key("A/B") == A.pk3_sort_key("a:b")


@pytest.mark.parametrize("textfont,scale,points", [
    (None, 0.22, 12), (None, 0.23, 12), (None, 0.25, 12),   # <= ui_smallFont
    (None, 0.26, 16), (None, 0.39, 16),                     # the normal band
    ("UI_FONT_NORMAL", 0.25, 16), ("1", 0.10, 16),          # textfont wins
    ("UI_FONT_SMALL", 0.50, 12), ("3", 0.30, 12),
])
def test_rtcw_font_is_the_one_its_text_width_picks(textfont, scale, points):
    """uix86.dll 0x40002AD0 / ui_mp_x86.dll 0x40002D3E: font 0 chooses by scale
    against ui_smallFont 0.25 / ui_bigFont 0.4; 1 = textFont (16 pt); 3 =
    smallFont (12 pt). The SP system menus (textscale .22/.23, no textfont)
    therefore draw the value with fontImage_12, not fontImage_16."""
    assert A.rtcw_font_points(textfont, scale) == points


@pytest.mark.parametrize("textfont,scale", [
    (None, 0.41), (None, 0.40),      # 0.4 itself: bigFont in MP, textFont in SP
    ("UI_FONT_BIG", 0.2), ("2", 0.3), ("UI_FONT_HANDWRITING", 0.3)])
def test_rtcw_unmeasured_fonts_are_refused_not_guessed(textfont, scale):
    with pytest.raises(ValueError):
        A.rtcw_font_points(textfont, scale)


def test_resolve_font_follows_each_rtcw_menu():
    sp = A.resolve_font(RTCW_SPEC, RTCW_MENU)             # textscale .23, no textfont
    assert sp == ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_12.dat", "q3")
    mp_menu = RTCW_MENU.replace("\t\ttextscale .23 ", "\t\ttextfont UI_FONT_NORMAL\r\n\t\ttextscale .25 ")
    mp = A.resolve_font(RTCW_SPEC, mp_menu)
    assert mp[1] == "fonts/fontImage_16.dat"
    assert A.resolve_font(JKA_SPEC, JKA_MENU) == A.JKA_MENU["font"]   # fixed font


def _overlay_xs(new):
    out = []
    for _, _, b in A.item_blocks(new):
        if "fleet:" in b:
            out.append(int(A.attr(b, "rect").split()[0]))
    return out


@pytest.mark.parametrize("text,spec", [(JKA_MENU, JKA_SPEC), (RTCW_MENU, RTCW_SPEC),
                                       (SOF2_MENU, SOF2_SPEC)])
def test_overlay_never_overlaps_the_value_or_itself(text, spec):
    """The regression the review found: measured with the wrong font, the
    RTCW SP overlay started exactly where "Custom" ended and the 4-digit width
    ran over the "x". With a proportional font, every piece must clear the
    previous one."""
    adv = [10.0] * 256
    for c, w in (("C", 13.0), ("u", 11.0), ("s", 9.0), ("t", 6.0), ("o", 11.0),
                 ("m", 16.0), ("x", 9.0), ("1", 8.0), ("8", 12.0), ("0", 12.0)):
        adv[ord(c)] = w
    new, _ = A.patch_q3_menu(text, spec, adv)
    _, _, body = A.find_mode_item(new, spec["mode_cvar"])
    scale = float(A.attr(body, "textscale"))
    rect = [float(v) for v in A.attr(body, "rect").split()]
    value_end = rect[0] + float(A.attr(body, "textalignx")) + 8 + \
        A.text_width(spec["label_text"], adv, scale)
    xw, xx, xh = _overlay_xs(new)
    digits4 = 4 * max(adv[ord(d)] for d in "0123456789") * scale
    assert value_end < xw
    assert xw + digits4 <= xx + 1                       # rounding to whole units
    assert xx + A.text_width("x", adv, scale) <= xh + 1


def test_raven_fontdat_parser():
    data = bytearray(256 * 28 + 10)
    struct.pack_into("<hhhhiffff", data, ord("8") * 28, 9, 14, 6, 0, 12, 0, 0, 0, 0)
    adv = A.raven_advances(bytes(data))
    assert adv[ord("8")] == 6.0
    assert A.text_width("88", adv, 0.5) == 6.0


def test_q3_font_parser_applies_glyph_scale():
    data = bytearray(256 * 80 + 4 + 64)
    struct.pack_into("<7i", data, ord("x") * 80, 10, 8, 0, 8, 9, 16, 16)
    struct.pack_into("<f", data, 256 * 80, 3.0)
    adv = A.q3_advances(bytes(data))
    assert adv[ord("x")] == 27.0
    assert A.text_width("x", adv, 0.25) == 6.75


def test_pk3_build_is_deterministic_and_readable():
    one = A.build_pk3([("ui/a.menu", b"abc"), ("Strip/b.sp", b"def")])
    two = A.build_pk3([("ui/a.menu", b"abc"), ("Strip/b.sp", b"def")])
    assert one == two
    z = zipfile.ZipFile(io.BytesIO(one))
    assert z.namelist() == ["ui/a.menu", "Strip/b.sp"]
    assert all(i.compress_type == zipfile.ZIP_DEFLATED for i in z.infolist())
    assert z.read("Strip/b.sp") == b"def"


def test_launcher_changes_name_what_the_other_writers_must_move():
    blob = "\n".join(A.LAUNCHER_CHANGES)
    for token in ("IDTECH3_NO_CUSTOM_MODE", "com_recommendedSet 1", "idtech3-index",
                  "idtech3-index-nofov", "idtech3-custom-nofov", "r_customwidth",
                  "SoldierOfFortune2.json", "test_gameres_mirror",
                  "test_sof2_uses_a_mode_index_and_the_right_table",
                  "test_gameres.c", "patch_launcher", "AGENT RELEASE",
                  "Host RTCW - LAN.bat", "Join RTCW - LAN.bat", "GAMESYNC RESET"):
        assert token in blob, token


def test_launcher_changes_generate_no_parentheses():
    """CLAUDE.md: nothing this project generates may carry ( or ) - a value
    expanded inside an if ( ... ) block closes it. Every line the changes ask
    a launcher to echo or start is quoted in backticks; none may carry one."""
    import re
    for item in A.LAUNCHER_CHANGES:
        for quoted in re.findall(r"`([^`]*)`", item):
            if quoted.lstrip().startswith((">", "+set", "start", "rem", "echo")) \
                    or "fleetres.cfg" in quoted:
                assert "(" not in quoted and ")" not in quoted, quoted


# ---------------------------------------------------------------------------
# publish / install-server safety (temporary directories; the share is never used)
# ---------------------------------------------------------------------------

def _md5(b):
    return hashlib.md5(b).hexdigest()


def _manifest(tmp_path, share, outputs):
    out = tmp_path / "out"
    out.mkdir(exist_ok=True)
    rows = []
    for rel, data, orig in outputs:
        lp = out / rel.replace("/", "_")
        lp.write_bytes(data)
        row = {"title": rel.split("/")[0], "share_path": "Files/Games-Library/" + rel,
               "local_path": str(lp), "md5": _md5(data), "size": len(data),
               "original_md5": _md5(orig) if orig is not None else None,
               "new_file": orig is None}
        if orig is not None:
            row["backup_share_path"] = ("Files/Games-Library/_patches/%s/%s/%s"
                                        % (rel.split("/")[0], A.BACKUP_TAG,
                                           rel.split("/", 1)[1]))
        rows.append(row)
    (out / "manifest.json").write_text(json.dumps({"outputs": rows}))
    return str(out)


@pytest.fixture
def fake_share(tmp_path, monkeypatch):
    share = tmp_path / "share"
    (share / "Files" / "Games-Library").mkdir(parents=True)
    monkeypatch.setattr(A, "SHARE_ROOT", str(share))
    monkeypatch.setattr(A, "LIB", str(share / "Files" / "Games-Library"))
    puts = []

    def fake_put(local, dest):
        puts.append(dest)
        p = share / dest
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(open(local, "rb").read())
        return True
    monkeypatch.setattr(A, "sharewrite_put", fake_put)
    # the deploy-order gates have their own tests below; these ones are about
    # the write sequence, so they start ungated
    monkeypatch.setattr(A, "PUBLISH_GATES", {})
    return share, puts


SOF2_GATE = {"launchers": {"Play MP.bat": ["+set r_mode -1 "]},
             "exe_md5": ("SoldierOfFortune2/sof2mp.exe", hashlib.md5(b"PATCHED-EXE!").hexdigest())}


def test_publish_replaces_the_exe_before_the_pk3_that_needs_it(tmp_path, fake_share, monkeypatch):
    share, puts = fake_share
    monkeypatch.setattr(A, "PUBLISH_GATES", {"SoldierOfFortune2/base/zz_fleet_video.pk3": SOF2_GATE})
    exe = share / "Files/Games-Library/SoldierOfFortune2/sof2mp.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"ORIGINAL-EXE")
    (exe.parent / "Play MP.bat").write_bytes(b'start "" sof2mp.exe +set r_mode -1 +set cg_fov 90\r\n')
    # the manifest lists the pk3 FIRST - publish must still do the exe first
    out = _manifest(tmp_path, share, [("SoldierOfFortune2/base/zz_fleet_video.pk3", b"PK", None),
                                      ("SoldierOfFortune2/sof2mp.exe", b"PATCHED-EXE!", b"ORIGINAL-EXE")])
    A.publish(out)
    assert [p.split("/")[-1] for p in puts] == ["sof2mp.exe", "sof2mp.exe", "zz_fleet_video.pk3"]
    assert "_patches" in puts[0]


def test_publish_refuses_a_pk3_whose_launchers_still_pass_an_index(tmp_path, fake_share, monkeypatch):
    share, puts = fake_share
    gate = {"launchers": {"Play RTCW Multiplayer.bat": ["+set r_mode -1 ", "+set com_recommendedSet 1"]}}
    monkeypatch.setattr(A, "PUBLISH_GATES", {"ReturnToCastleWolfenstein/Main/zz_fleet_video.pk3": gate})
    t = share / "Files/Games-Library/ReturnToCastleWolfenstein"
    t.mkdir(parents=True)
    (t / "Play RTCW Multiplayer.bat").write_bytes(
        b'start "" WolfMP.exe +set r_mode %FR_Q3MODE% +set r_fullscreen 1\r\n')
    out = _manifest(tmp_path, share, [("ReturnToCastleWolfenstein/Main/zz_fleet_video.pk3", b"PK", None),
                                      ("JediAcademy/base/zz_fleet_video.pk3", b"JK", None)])
    with pytest.raises(SystemExit) as e:
        A.publish(out)
    assert "ReturnToCastleWolfenstein" in str(e.value)
    # the refused pk3 was not written; the independent one still was
    assert puts == ["Files/Games-Library/JediAcademy/base/zz_fleet_video.pk3"]
    # --force is the only way past it
    puts.clear()
    A.publish(out, force=True)
    assert puts == ["Files/Games-Library/ReturnToCastleWolfenstein/Main/zz_fleet_video.pk3"]


def test_the_gate_names_a_half_migrated_launcher(tmp_path, fake_share, monkeypatch):
    share, _ = fake_share
    monkeypatch.setattr(A, "PUBLISH_GATES", {"SoldierOfFortune2/base/zz_fleet_video.pk3": SOF2_GATE})
    t = share / "Files/Games-Library/SoldierOfFortune2"
    t.mkdir(parents=True)
    # -1 on the command line but the index still in the cfg it writes
    (t / "Play MP.bat").write_bytes(b'echo seta r_mode "%FR_Q3MODE%"\r\nsof2mp.exe +set r_mode -1 \r\n')
    (t / "sof2mp.exe").write_bytes(b"ORIGINAL-EXE")
    why = A.publish_gate("SoldierOfFortune2/base/zz_fleet_video.pk3")
    assert any("mode index" in w for w in why)
    assert any("not the patched" in w for w in why)
    # a dry run counts the exe this same run would already have written
    why = A.publish_gate("SoldierOfFortune2/base/zz_fleet_video.pk3",
                         {"SoldierOfFortune2/sof2mp.exe": SOF2_GATE["exe_md5"][1]})
    assert not any("not the patched" in w for w in why)


def test_real_gates_cover_every_pk3_and_the_minus_one_idiom():
    for title, plan in A.PLAN.items():
        rel = "%s/%s" % (plan["gamedir"], A.PK3_NAME)
        g = A.PUBLISH_GATES[rel]
        assert g["launchers"], rel
        for need in g["launchers"].values():
            assert "+set r_mode -1 " in need and 'echo seta r_mode "-1"' in need
    rtcw = A.PUBLISH_GATES["ReturnToCastleWolfenstein/Main/zz_fleet_video.pk3"]["launchers"]
    assert len(rtcw) == 4 and all("+set com_recommendedSet 1" in v for v in rtcw.values())
    assert A.PUBLISH_GATES["SoldierOfFortune2/base/zz_fleet_video.pk3"]["exe_md5"] == \
        (A.SOF2MP, A.SOF2MP_PATCHED_MD5)


def test_publish_backs_up_the_original_before_replacing_it(tmp_path, fake_share):
    share, puts = fake_share
    orig, new = b"ORIGINAL-EXE", b"PATCHED-EXE!"
    exe = share / "Files/Games-Library/SoldierOfFortune2/sof2mp.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(orig)
    out = _manifest(tmp_path, share, [("SoldierOfFortune2/sof2mp.exe", new, orig),
                                      ("SoldierOfFortune2/base/zz_fleet_video.pk3", b"PK", None)])
    A.publish(out)
    assert puts[0].endswith("_patches/SoldierOfFortune2/%s/sof2mp.exe" % A.BACKUP_TAG)
    assert puts[1] == "Files/Games-Library/SoldierOfFortune2/sof2mp.exe"
    assert (share / puts[0]).read_bytes() == orig
    assert exe.read_bytes() == new
    # idempotent: a second run writes nothing
    puts.clear()
    A.publish(out)
    assert puts == []


def test_publish_stops_on_an_unexpected_share_file(tmp_path, fake_share):
    share, puts = fake_share
    exe = share / "Files/Games-Library/SoldierOfFortune2/sof2mp.exe"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"SOMEONE ELSE'S EXE")
    out = _manifest(tmp_path, share, [("SoldierOfFortune2/sof2mp.exe", b"new", b"orig")])
    with pytest.raises(SystemExit):
        A.publish(out)
    assert puts == []


def test_publish_stops_on_the_first_failed_put(tmp_path, fake_share, monkeypatch):
    share, _ = fake_share
    calls = []
    monkeypatch.setattr(A, "sharewrite_put", lambda l, d: calls.append(d) or False)
    out = _manifest(tmp_path, share, [("JediAcademy/base/zz_fleet_video.pk3", b"a", None),
                                      ("SoldierOfFortune2/base/zz_fleet_video.pk3", b"b", None)])
    with pytest.raises(SystemExit):
        A.publish(out)
    assert len(calls) == 1


def test_install_server_skips_a_non_pure_server(tmp_path, monkeypatch, capsys):
    srv = tmp_path / "srv"
    srv.mkdir()
    (srv / "server.cfg").write_text('seta sv_pure 0\n')
    monkeypatch.setattr(A, "PLAN", {"JediAcademy": dict(A.PLAN["JediAcademy"],
                        server=("jka-server", str(srv), str(srv / "server.cfg")))})
    out = _manifest(tmp_path, None, [("JediAcademy/base/zz_fleet_video.pk3", b"pk3", None)])
    A.install_server(out)
    assert not (srv / A.PK3_NAME).exists()
    assert "sv_pure 0" in capsys.readouterr().out
    (srv / "server.cfg").write_text('seta sv_pure "1"\n')
    A.install_server(out)
    assert (srv / A.PK3_NAME).read_bytes() == b"pk3"


def test_server_purity_reads_the_last_setting(tmp_path):
    cfg = tmp_path / "server.cfg"
    cfg.write_text('set sv_pure 1\nseta sv_pure "0"\n')
    assert A.server_purity(("x", str(tmp_path), str(cfg))) == "0"
    cfg.write_text("// nothing\n")
    assert A.server_purity(("x", str(tmp_path), str(cfg))).startswith("unset")


# ---------------------------------------------------------------------------
# Against the staged originals (skips LOUDLY without the share)
# ---------------------------------------------------------------------------

SHARE_OK = os.path.isdir(A.LIB)


def _need_share():
    if not SHARE_OK:
        msg = ("SHARE NOT MOUNTED: %s is absent - the staged-original checks for "
               "idtech3-kin did NOT run" % A.LIB)
        print(msg, file=sys.stderr)
        pytest.skip(msg)


def test_staged_originals_pass_check():
    _need_share()
    rep = A.check()
    assert rep.failed == 0, "\n".join(rep.lines)


def test_build_reproduces_the_reviewed_outputs(tmp_path):
    _need_share()
    man, rep = A.build(str(tmp_path))
    got = {o["share_path"].split("Games-Library/", 1)[1]: o["md5"] for o in man["outputs"]}
    assert got == A.REVIEWED_BUILD
    exe = [o for o in man["outputs"] if o["share_path"].endswith("sof2mp.exe")][0]
    assert exe["original_md5"] == A.ORIGINALS[A.SOF2MP][1]
    assert exe["md5"] == A.SOF2MP_PATCHED_MD5


# The font each engine DRAWS the Video Mode value with, stated independently of
# apply.py's resolve_font() so a wrong rule there cannot pass its own check.
# JKA font 4 = small2Font "arialnb" (ui/jamp/main.menu, ui/hud.menu); SoF2
# textfont "hud"; RTCW from Text_Width (uix86.dll 0x40002AD0, ui_mp_x86.dll
# 0x40002D3E) + the assetGlobalDefs: SP menus = textscale .22/.23, no textfont
# -> smallFont 12 pt; MP menus = UI_FONT_NORMAL -> textFont 16 pt.
ENGINE_FONT = {
    "JediAcademy": ("JediAcademy/base/assets1.pk3", "fonts/arialnb.fontdat", "raven"),
    ("SoldierOfFortune2", "ui/setup_display.menu"):
        ("SoldierOfFortune2/base/therest.pk3", "fonts/hud.fontdat", "raven"),
    ("ReturnToCastleWolfenstein", "ui/system.menu"):
        ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_12.dat", "q3"),
    ("ReturnToCastleWolfenstein", "ui/ingame_system.menu"):
        ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_12.dat", "q3"),
    ("ReturnToCastleWolfenstein", "ui_mp/system.menu"):
        ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_16.dat", "q3"),
    ("ReturnToCastleWolfenstein", "ui_mp/ingame_system.menu"):
        ("ReturnToCastleWolfenstein/Main/pak0.pk3", "fonts/fontImage_16.dat", "q3"),
}


def test_built_overlays_clear_the_value_in_the_engines_own_font(tmp_path):
    """Against the staged originals: in every built menu, measured with the
    font the ENGINE draws with, "Custom" ends before the width starts, a
    4-digit width ends before the "x", and the "x" before the height. The first
    build failed this on both RTCW SP menus (width at 276 where "Custom" ends
    at 276.0, 4 digits to 307.7 over an "x" at 303)."""
    _need_share()
    man, _ = A.build(str(tmp_path))
    for title, plan in A.PLAN.items():
        z = zipfile.ZipFile(os.path.join(str(tmp_path), plan["gamedir"], A.PK3_NAME))
        for member, src, spec in plan["pk3"]:
            if spec["kind"] != "q3menu":
                continue
            font = ENGINE_FONT.get((title, member)) or ENGINE_FONT[title]
            assert A.resolve_font(spec, A.read_member(src, member).decode("latin-1")) == font
            adv = A.load_font(font)
            new = z.read(member).decode("latin-1")
            _, _, body = A.find_mode_item(new, spec["mode_cvar"])
            scale = float(A.attr(body, "textscale"))
            rect = [float(v) for v in A.attr(body, "rect").split()]
            value_end = rect[0] + float(A.attr(body, "textalignx")) + 8 + \
                A.text_width(spec["label_text"], adv, scale)
            xw, xx, xh = _overlay_xs(new)
            digits4 = 4 * max(adv[ord(d)] for d in "0123456789") * scale
            where = "%s!%s" % (title, member)
            assert value_end < xw, where
            assert xw + digits4 <= xx + 1, where
            assert xx + A.text_width("x", adv, scale) <= xh + 1, where
    # the exe is listed before the pk3 that depends on it
    order = [o["share_path"].rsplit("/", 1)[-1] for o in man["outputs"]
             if o["title"] == "SoldierOfFortune2"]
    assert order == ["sof2mp.exe", A.PK3_NAME]
