"""provisioning/patches/idtech2/apply.py - 1920x1080 as id Tech 2 mode 9.

Quake II 3.20, SiN 1.11 and Soldier of Fortune have a FIXED ten-entry mode
table ending at 1600x1200; the in-game menu indexes it and gl_mode IS the
index. The patch turns entry 9 into 1920x1080 and relabels every menu that
shows it (quake2.exe's own label array, SiN's main.mnu shipped LOOSE because
SiN searches loose files first, SoF's m_video.rmf shipped as base\\pak2.pak
because SoF searches paks first). Measured 2026-09-29, evidence in
.claude/evidence-1080p/_results/titles-verified.json.

Pure logic runs anywhere. The share-dependent half SKIPS LOUDLY when
/mnt/retro-share/Files/Games-Library is not mounted.

The module is loaded under its OWN name ("idtech2_apply"), never as a bare
`import apply` with a sys.path insert: several patch builders ship an apply.py
and tests/run_all.sh runs one pytest process, so a bare import hands whichever
test runs second the FIRST test's module out of sys.modules (measured
2026-09-29 by the descent3 builder against this file).
"""
import importlib.util
import io
import os
import shutil
import struct
import sys
import warnings

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
APPLY = os.path.join(HERE, "..", "..", "provisioning", "patches", "idtech2", "apply.py")
_spec = importlib.util.spec_from_file_location("idtech2_apply", APPLY)
P = importlib.util.module_from_spec(_spec)
sys.modules["idtech2_apply"] = P
_spec.loader.exec_module(P)

LIB = P.LIBRARY


def test_loaded_under_its_own_name_not_as_a_bare_apply_module():
    assert P.__name__ == "idtech2_apply"
    assert sys.modules.get("apply") is not P


def spec(title):
    return next(s for s in P.EXES if s["title"] == title)


# ---------------------------------------------------------------------------
# the offsets are the measured ones
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("title,w,h,s,extra", [
    ("Quake2Complete", 0x54464, 0x54468, 0x54470, 0x54720),
    ("SiNGold", 0x944e4, 0x944e8, 0x944f0, None),
    ("SoldierOfFortune", 0x1c6e9c, 0x1c6ea0, 0x1c6ea8, None),
])
def test_edit_offsets_are_the_verified_ones(title, w, h, s, extra):
    ed = P.exe_edits(spec(title))
    offs = [e[0] for e in ed]
    assert offs[:3] == [w, h, s]
    assert ed[0][1] == struct.pack("<I", 1600) and ed[0][2] == struct.pack("<I", 1920)
    assert ed[1][1] == struct.pack("<I", 1200) and ed[1][2] == struct.pack("<I", 1080)
    assert ed[2][1] == b"Mode 9: 1600x1200\0" and ed[2][2] == b"Mode 9: 1920x1080\0"
    if extra:
        assert offs[3] == extra and ed[3][1] == b"[1600 1200]\0" and ed[3][2] == b"[1920 1080]\0"
    else:
        assert len(ed) == 3


def test_quake2win9x_is_not_patched():
    """Its exe is byte-identical to Quake2Complete's and serves the Win9x lane."""
    assert all(s["title"] != "Quake2Win9x" for s in P.EXES)
    assert ("Quake2Win9x", "quake2.exe", spec("Quake2Complete")["md5"]) in P.KEEP_STOCK


# ---------------------------------------------------------------------------
# byte patching on a synthetic id Tech 2 table
# ---------------------------------------------------------------------------

MODES = [(320, 240), (400, 300), (512, 384), (640, 480), (800, 600),
         (960, 720), (1024, 768), (1152, 864), (1280, 960), (1600, 1200)]


def synthetic_exe(table=0x100, strings=0x300):
    """A buffer holding a vid_modes table whose desc 'pointers' are file offsets."""
    buf = bytearray(0x600)
    off = strings
    for i, (w, h) in enumerate(MODES):
        s = ("Mode %d: %dx%d" % (i, w, h)).encode() + b"\0"
        buf[off:off + len(s)] = s
        struct.pack_into("<IiiI", buf, table + 16 * i, off, w, h, i)
        off += len(s)
    desc9 = struct.unpack_from("<I", buf, table + 16 * 9)[0]
    return bytes(buf), desc9


def test_apply_edits_changes_entry9_and_nothing_else():
    data, desc9 = synthetic_exe()
    ed = P.vid_mode_edits(0x100, desc9)
    out = P.apply_edits(data, ed)
    ident = lambda va: va  # noqa: E731
    before = P.read_vid_table(data, 0x100, ident)
    after = P.read_vid_table(out, 0x100, ident)
    assert (before[9]["w"], before[9]["h"], before[9]["desc"]) == (1600, 1200, "Mode 9: 1600x1200")
    assert (after[9]["w"], after[9]["h"], after[9]["desc"]) == (1920, 1080, "Mode 9: 1920x1080")
    assert before[:9] == after[:9]
    assert len(out) == len(data)
    changed = [i for i in range(len(data)) if data[i] != out[i]]
    assert len(changed) <= 4 + 4 + 18
    assert P.edits_applied(out, ed) and not P.edits_applied(data, ed)


def test_apply_edits_refuses_a_file_it_was_not_measured_on():
    data, desc9 = synthetic_exe()
    ed = P.vid_mode_edits(0x100, desc9)
    wrong = bytearray(data)
    struct.pack_into("<i", wrong, 0x100 + 16 * 9 + 4, 2048)   # someone else's table
    with pytest.raises(P.PatchError):
        P.apply_edits(bytes(wrong), ed)
    # and re-applying to an already patched file is refused, not doubled
    with pytest.raises(P.PatchError):
        P.apply_edits(P.apply_edits(data, ed), ed)


def test_apply_edits_refuses_a_length_change():
    with pytest.raises(P.PatchError):
        P.apply_edits(b"abcdef", [(0, b"ab", b"abc", "grow")])


def test_ptr_array_reads_null_terminated_labels():
    buf = bytearray(0x100)
    buf[0x80:0x8c] = b"[1600 1200]\0"
    struct.pack_into("<II", buf, 0x10, 0x80, 0)
    assert P.read_ptr_array(bytes(buf), 0x10, lambda va: va, 2) == [(0x80, "[1600 1200]"), None]


# ---------------------------------------------------------------------------
# SiN main.mnu
# ---------------------------------------------------------------------------

def sin_block(numitems, trail=b""):
    items = b"".join(b'" %d x %d"' % wh + trail + b"\r\n" for wh in MODES)
    return (b'listitem con_vidmode 3 "fc 0.7 string \\"Video Mode: \\""\r\n'
            b"numitems %d\r\n" % numitems + items +
            b'selitem "fc 1 1 1 1"\r\nhelpdata "x" 5\r\nenditem\r\n')


def test_sin_menu_base_relabels_only_the_tenth_item():
    other = b'listitem con_other 0 "x"\r\nnumitems 1\r\n" 1600 x 1200"\r\nenditem\r\n'
    text = other + sin_block(10) + other
    out = P.sin_mnu_relabel(text, 10)
    assert b'" 1920 x 1080"\r\n' in out
    assert out.count(b'" 1600 x 1200"') == 2           # the other lists are untouched
    assert out.startswith(other) and out.endswith(other)
    assert len(out) == len(text)


def test_sin_menu_2015_gets_its_tenth_item_back():
    """Wages of SiN listed ten strings under 'numitems 9', so it stopped at 1280 x 960."""
    text = sin_block(9, trail=b" ")
    out = P.sin_mnu_relabel(text, 9)
    assert b"numitems 10\r\n" in out and b"numitems 9\r\n" not in out
    assert b'" 1920 x 1080" \r\n' in out                 # trailing blank and CRLF kept
    assert len(out) == len(text) + 1


def test_sin_menu_refuses_an_unexpected_block():
    with pytest.raises(P.PatchError):
        P.sin_mnu_relabel(sin_block(10), 9)             # numitems not as measured
    with pytest.raises(P.PatchError):
        P.sin_mnu_relabel(sin_block(10).replace(b"1600 x 1200", b"1920 x 1080"), 10)
    with pytest.raises(P.PatchError):
        P.sin_mnu_relabel(b"no menu here", 10)
    with pytest.raises(P.PatchError):
        P.sin_mnu_relabel(sin_block(10) + sin_block(10), 10)


# ---------------------------------------------------------------------------
# SoF m_video.rmf and pak2.pak
# ---------------------------------------------------------------------------

SOF_SAMPLE = (b'<list "^MENU_VIDEO_DRIVER^" match "opengl32,3dfxvgl" cvar menu_driver><hbr>\r\n'
              b'<list "640x480,800x600,960x720,1024x768,1152x864,1280x960,1600x1200" '
              b'match "3,4,5,6,7,8,9" atext "^MENU_VIDEO_RESOLUTION^ : " cvar menu_mode '
              b'key mouse1 "set menu_settings_changed 1"><hbr>\r\n')


def test_sof_menu_relabel_keeps_the_mode_mapping():
    out = P.sof_rmf_relabel(SOF_SAMPLE)
    assert b'1280x960,1920x1080" match "3,4,5,6,7,8,9"' in out
    assert b"1600x1200" not in out
    assert len(out) == len(SOF_SAMPLE)
    with pytest.raises(P.PatchError):
        P.sof_rmf_relabel(out)


def test_pak_round_trip():
    pak = P.build_pak([("menus/m_video.rmf", b"hello"), ("a/b.cfg", b"xyz")])
    magic, d = P.pak_dir(pak)
    assert magic == b"PACK" and set(d) == {"menus/m_video.rmf", "a/b.cfg"}
    f = io.BytesIO(pak)
    assert P.pak_read(f, "MENUS/M_VIDEO.RMF") == b"hello"
    assert P.pak_read(f, "a/b.cfg") == b"xyz"
    assert len(pak) == 12 + 8 + 2 * 64


def test_spak_directory_is_128_byte_entries():
    data = b"menu-bytes"
    ent = b"menus/main.mnu".ljust(120, b"\0") + struct.pack("<ii", 12, len(data))
    spak = b"SPAK" + struct.pack("<ii", 12 + len(data), 128) + data + ent
    assert P.pak_read(io.BytesIO(spak), "menus/main.mnu") == data
    with pytest.raises(P.PatchError):
        P.pak_dir(b"ZIPX" + b"\0" * 8)


def test_sof_pak_output_holds_only_the_menu():
    sp = next(d for d in P.DERIVED if d["kind"] == "sof-pak")
    out = P.derive(sp, SOF_SAMPLE)
    _m, d = P.pak_dir(out)
    assert list(d) == ["menus/m_video.rmf"]
    assert P.SOF_NEW_LIST in P.pak_read(io.BytesIO(out), "menus/m_video.rmf")
    assert sp["rel"] == "base/pak2.pak"      # a pak - loose files lose to pak0 in SoF


def test_sin_menus_ship_loose_in_every_game_dir_a_client_reaches():
    """base\\ and 2015\\ have shortcuts; ctf\\ is reached by joining a
    ds_sinctf.bat server, which switches the client into the server's game dir
    - where ctf\\pak1.sin's own main.mnu would still say 1600 x 1200 for what
    the patched sin.exe now sets as 1920x1080."""
    rels = sorted(d["rel"] for d in P.DERIVED if d["title"] == "SiNGold")
    assert rels == ["2015/menus/main.mnu", "base/menus/main.mnu", "ctf/menus/main.mnu"]
    ctf = next(d for d in P.DERIVED if d["rel"] == "ctf/menus/main.mnu")
    assert ctf["pak"] == "ctf/pak1.sin", "pak1 is the ctf copy the engine loads (pak0 has one too)"
    assert all(d["paks"] == (32, "sin") for d in P.DERIVED if d["title"] == "SiNGold")
    assert next(d for d in P.DERIVED if d["kind"] == "sof-pak")["paks"] == (10, "pak")


def write_pak(path, files, magic=b"PACK"):
    esz = P.PAK_FORMATS[magic]
    body, ents = bytearray(), []
    for name, data in files:
        ents.append((name.encode(), 12 + len(body), len(data)))
        body += data
    d = b"".join(n.ljust(esz - 8, b"\0") + struct.pack("<ii", p, l) for n, p, l in ents)
    with open(path, "wb") as f:
        f.write(magic + struct.pack("<ii", 12 + len(body), len(d)) + bytes(body) + d)


def test_pak_winner_is_the_highest_pak_carrying_the_entry(tmp_path):
    """Both FS_AddGameDirectory variants PREPEND each pak as it is opened, so
    the highest-numbered pak with the entry is the one the engine reads - the
    relabel must start from THAT copy or it silently reverts a newer pak."""
    write_pak(tmp_path / "pak0.sin", [("menus/main.mnu", b"a")], b"SPAK")
    write_pak(tmp_path / "pak1.sin", [("maps/x.bsp", b"b")], b"SPAK")
    write_pak(tmp_path / "PAK4.SIN", [("MENUS/MAIN.MNU", b"c")], b"SPAK")   # case-insensitive
    assert P.pak_winner(str(tmp_path), "menus/main.mnu", 32, "sin") == "PAK4.SIN"
    assert P.pak_winner(str(tmp_path), "menus/main.mnu", 32, "sin", skip=(4,)) == "pak0.sin"
    assert P.pak_winner(str(tmp_path), "menus/none.mnu", 32, "sin") is None
    # the engine's loop bound is part of the answer: SoF opens pak0-9 only
    write_pak(tmp_path / "pak12.pak", [("menus/m_video.rmf", b"d")])
    write_pak(tmp_path / "pak0.pak", [("menus/m_video.rmf", b"e")])
    assert P.pak_winner(str(tmp_path), "menus/m_video.rmf", 10, "pak") == "pak0.pak"


def test_partial_copy_recognises_only_a_truncated_known_file():
    whole = b"0123456789"
    assert P.is_partial_copy(b"", whole)
    assert P.is_partial_copy(b"0123", whole)
    assert not P.is_partial_copy(whole, whole)            # complete is not partial
    assert not P.is_partial_copy(b"0124", whole)          # diverges: somebody else's bytes
    assert not P.is_partial_copy(whole + b"x", whole)
    assert P.is_partial_copy(b"01", b"zz", whole)


def test_classify_exe_tells_stock_patched_and_foreign_apart():
    data, desc9 = synthetic_exe()
    spec = {"title": "T", "rel": "t.exe", "size": len(data), "md5": P.md5(data),
            "table": 0x100, "desc9": desc9, "extra": []}
    patched = P.apply_edits(data, P.exe_edits(spec))
    assert P.classify_exe(spec, data) == ("stock", data)
    assert P.classify_exe(spec, patched) == ("patched", data)
    other = bytearray(patched)
    other[0] ^= 1
    assert P.classify_exe(spec, bytes(other)) == (None, None)
    assert P.classify_exe(spec, patched[:-1]) == (None, None)
    assert P.classify_exe(spec, None) == (None, None)


def test_publish_refuses_a_library_that_is_not_the_share(tmp_path, monkeypatch):
    """sharewrite.py always writes \\\\192.168.1.122\\files and verifies
    through /mnt/retro-share; originals read from anywhere else would be
    backed up and 'verified' against a different tree."""
    monkeypatch.setattr(P, "sharewrite", lambda *a: pytest.fail("sharewrite must not run"))
    assert P.main(["--publish", str(tmp_path), "--library", str(tmp_path)]) == 2
    assert P.main(["--publish", str(tmp_path), "--library", str(tmp_path), "--dry-run"]) == 2


# ---------------------------------------------------------------------------
# FR_Q2WIDE - the launcher/GAMERES value (reference rule)
# ---------------------------------------------------------------------------

def offers(*modes):
    """mode_offered()/gr_mode_offered(): a list of fewer than four modes is
    treated as NO list (everything offered), exactly as both writers do."""
    if len(modes) < 4:
        return lambda w, h: True
    return lambda w, h: (w, h) in modes


# The driver mode lists the boxes answered GAMERES with on 2026-09-29
# (.claude/evidence-1080p/g6-*/gameres_1{23,45}_2026-09-29.json, gameres_240_*,
# q3-195/99-gameres-final.json; .124 from evidence-refresh/*/boxes/124__GAMERES.txt).
MODES_123 = [(640, 480), (720, 480), (800, 600), (848, 480), (1024, 768), (1152, 864),
             (1280, 720), (1280, 768), (1280, 800), (1280, 960), (1280, 1024), (1360, 768),
             (1440, 900), (1680, 1050), (1920, 1080)]
MODES_145 = [(640, 480), (720, 480), (720, 576), (800, 600), (1024, 768), (1152, 864),
             (1280, 720), (1280, 768), (1280, 800), (1280, 960), (1280, 1024), (1360, 768),
             (1600, 900), (1600, 1024), (1680, 1050), (1920, 1080)]
MODES_240 = [(320, 200), (320, 240), (400, 300), (512, 384), (640, 400), (640, 432),
             (640, 480), (720, 480), (720, 576), (800, 480), (800, 600), (848, 480),
             (1024, 768), (1152, 864), (1280, 720), (1280, 768), (1280, 800), (1280, 960),
             (1280, 1024), (1360, 768), (1360, 1024), (1440, 900), (1776, 1000), (1920, 1080)]
MODES_195 = [(640, 480), (720, 480), (720, 576), (800, 600), (1024, 768), (1152, 648),
             (1280, 720), (1280, 768), (1280, 800), (1280, 960), (1280, 1024), (1360, 768),
             (1360, 1024), (1366, 768), (1400, 1050), (1440, 900), (1600, 900), (1680, 1050),
             (1776, 1000), (1920, 1080)]
LCD_1080 = MODES_123
P1120_124 = [(320, 240), (400, 300), (512, 384), (640, 480), (800, 600), (1024, 768),
             (1152, 864), (1280, 960), (1280, 1024), (1600, 1200), (720, 576), (960, 720),
             (320, 200), (640, 400), (720, 480), (1600, 1024), (1280, 720), (1280, 800),
             (1360, 768), (1440, 900), (1680, 1050)]      # no 1920x1080 on the CRT


@pytest.mark.parametrize("box,tgt,t43,modes,q2mode,want", [
    (".123", (1920, 1080), (1280, 960), MODES_123, 8, 9),
    (".145", (1920, 1080), (1280, 960), MODES_145, 8, 9),
    (".195", (1920, 1080), (1280, 960), MODES_195, 8, 9),
    (".240", (1920, 1080), (1280, 960), MODES_240, 8, 9),
    (".124", (1280, 960), (1280, 960), P1120_124, 8, 8),     # HP P1120 CRT, measured
    # .143: no EDID -> 4:3 tube; persisted 1280x1024 -> 1280x960 (inferred, not
    # measured). Either a short list (the GeForce 6800 enumeration quirk: all
    # offered) or a normal one gives 8, never 9.
    (".143", (1280, 960), (1280, 960), [(1024, 768)], 8, 8),
    (".143", (1280, 960), (1280, 960), [(800, 600), (1024, 768), (1280, 960), (1280, 1024)], 8, 8),
])
def test_q2wide_per_box(box, tgt, t43, modes, q2mode, want):
    off = offers(*modes)
    assert P.q2_mode_for(t43[0], t43[1], off) == q2mode, "FR_Q2MODE as measured"
    assert P.fr_q2wide(tgt[0], tgt[1], t43[0], t43[1], off) == want
    assert P.q2wide_res(want) == ((1920, 1080) if want == 9 else P.Q2TAB[want])


def test_q2wide_never_hands_a_patched_exe_9_for_1600x1200():
    """On a patched exe mode 9 is 1920x1080: a 4:3 box whose FR_Q2MODE is 9
    (.124's P1120 if its desktop were raised to 1600x1200) must get 8."""
    off = offers(*P1120_124)
    assert P.q2_mode_for(1600, 1200, off) == 9
    assert P.fr_q2wide(1600, 1200, 1600, 1200, off) == 8


def test_q2wide_capped_branch_reselects_rather_than_clamping():
    """Capping the TABLE at entry 8 re-runs the selector; clamping the INDEX
    (the first draft's min(FR_Q2MODE, 8)) would ask a 1600x1200 tube that does
    not list 1280x960 for 1280x960 anyway, and id Tech 2 answers a refused
    fullscreen mode by opening a WINDOW at that size."""
    tube = [(640, 480), (800, 600), (1024, 768), (1152, 864), (1600, 1200)]
    off = offers(*tube)
    assert P.q2_mode_for(1600, 1200, off) == 9
    assert min(P.q2_mode_for(1600, 1200, off), 8) == 8                  # not listed
    assert P.fr_q2wide(1600, 1200, 1600, 1200, off) == 7                # 1152x864 is


def test_q2wide_equals_q2mode_wherever_q2mode_is_not_9():
    """The non-16:9 branch must be FR_Q2MODE itself on every target where
    FR_Q2MODE is not 9 - so the CRT boxes see no change at all."""
    lists = [LCD_1080, P1120_124, [(1024, 768)], [(640, 480), (800, 600), (1024, 768),
             (1280, 1024)], [(800, 600), (1024, 768), (1152, 864), (1280, 960)]]
    for modes in lists:
        off = offers(*modes)
        for t43 in ((640, 480), (800, 600), (1024, 768), (1152, 864), (1280, 960), (1600, 1200)):
            q2 = P.q2_mode_for(t43[0], t43[1], off)
            for tgt in (t43, (1280, 1024), (1920, 1200)):     # never 16:9 here
                got = P.fr_q2wide(tgt[0], tgt[1], t43[0], t43[1], off)
                assert got != 9
                if q2 != 9:
                    assert got == q2, (modes, t43, tgt)


def test_q2wide_falls_back_when_1080p_is_not_offered_or_capped():
    lcd_no_1080 = [m for m in LCD_1080 if m != (1920, 1080)]
    assert P.fr_q2wide(1920, 1080, 1280, 960, offers(*lcd_no_1080)) == 8
    # ResCap 1280x720 on a 1080p panel: 16:9 but under 1920x1080
    assert P.fr_q2wide(1280, 720, 800, 600, offers(*LCD_1080)) == 4
    assert P.fr_q2wide(1366, 768, 1024, 768, offers(*LCD_1080)) == 6   # small 16:9 panel
    assert P.fr_q2wide(1920, 1200, 1600, 1200, offers((1920, 1080), (1920, 1200),
                                                       (1600, 1200), (1280, 960))) == 8  # 16:10
    assert P.fr_q2wide(2560, 1440, 1280, 960, offers((1920, 1080), (2560, 1440),
                                                      (1280, 960), (1024, 768))) == 9   # 16:9, larger


# ---------------------------------------------------------------------------
# against the staged library (skips loudly when the share is absent)
# ---------------------------------------------------------------------------

EXPECTED_OUTPUT_MD5 = {
    ("Quake2Complete", "quake2.exe"): "d4fe128806b9ac7899c8ac6e4d6c367d",
    ("SiNGold", "sin.exe"): "55d69eb8584e633b16445adb126f8d47",
    ("SoldierOfFortune", "SoF.exe"): "b4ed42272887b16a8ecdb92c919a507d",
    ("SiNGold", "base/menus/main.mnu"): "886cdbf4cee22fc649b43aa0f42867e4",
    ("SiNGold", "2015/menus/main.mnu"): "dce920d214d1e1aa0445578066c2eaeb",
    ("SiNGold", "ctf/menus/main.mnu"): "e639508cdb2d45e0068f9af25cf3f0dd",
    ("SoldierOfFortune", "base/pak2.pak"): "d2d96ee679c039596c35c3fe9404ef41",
}


def need_share():
    if not os.path.isdir(LIB):
        msg = ("SKIPPED LOUDLY: staged library %s is not mounted - the id Tech 2 "
               "patch was NOT verified against the real originals" % LIB)
        print(msg, file=sys.stderr)
        warnings.warn(msg)          # shows in the default -q summary, unlike the reason
        pytest.skip(msg)


@pytest.fixture(scope="module")
def work():
    need_share()
    return {(w["title"], w["rel"]): w for w in P.load_originals(LIB)}


def test_share_outputs_are_reproducible(work):
    assert {k: P.md5(w["data"]) for k, w in work.items()} == EXPECTED_OUTPUT_MD5


def test_share_exes_patch_exactly_the_edited_bytes(work):
    for s in P.EXES:
        w = work[(s["title"], s["rel"])]
        assert w["bytes_changed"] <= sum(len(e[1]) for e in P.exe_edits(s))
        assert len(w["data"]) == s["size"]


def test_share_quake2win9x_exe_is_still_stock(work):
    p = P.ci_path(LIB, "Quake2Win9x/quake2.exe")
    if p is None:
        pytest.skip("Quake2Win9x not staged (case-insensitive)")
    assert P.md5(open(p, "rb").read()) == "57dd2cf4ba176f3e6ae72fccc94c38b4"


def test_share_is_in_the_deployed_state():
    """Published 2026-09-29: every exe on the share is our patched md5, its
    backup under _patches/ is the pinned original, and every new file is our
    output - and --check reports exactly that, not a failure."""
    need_share()
    for s in P.EXES:
        live = P.ci_path(LIB, "%s/%s" % (s["title"], s["rel"]))
        assert P.md5(open(live, "rb").read()) == EXPECTED_OUTPUT_MD5[(s["title"], s["rel"])]
        b = P.ci_path(LIB, P.backup_rel_of(s["title"], s["rel"]))
        assert b, "no backup of %s/%s" % (s["title"], s["rel"])
        bdata = open(b, "rb").read()
        assert len(bdata) == s["size"] and P.md5(bdata) == s["md5"]
    for d in P.DERIVED:
        live = P.ci_path(LIB, "%s/%s" % (d["title"], d["rel"]))
        assert live and P.md5(open(live, "rb").read()) == EXPECTED_OUTPUT_MD5[(d["title"], d["rel"])]
    states = {(w["title"], w["rel"]): w["state"] for w in P.load_originals(LIB)}
    assert all(states[(s["title"], s["rel"])] == "ALREADY PATCHED on the share; backup verified"
               for s in P.EXES)
    assert all(states[(d["title"], d["rel"])] == "ALREADY PUBLISHED on the share" for d in P.DERIVED)
    assert P.cmd_check(LIB) == 0


# ---------------------------------------------------------------------------
# the publish mechanics, on a synthetic PRE-deploy library of links to the
# share's files (nothing is written to the share). The share now holds the
# patched exes, so each exe slot gets its STOCK original - the verified backup
# under _patches/ - and the new files are left out, as they were before deploy.
# ---------------------------------------------------------------------------

SYNTH_LINKS = ["Quake2Complete/quake2.exe", "SiNGold/sin.exe", "SoldierOfFortune/SoF.exe",
               "SoldierOfFortune/base/pak0.pak", "SoldierOfFortune/base/pak1.pak",
               "SiNGold/2015/pak0.sin", "SiNGold/ctf/pak0.sin", "SiNGold/ctf/pak1.sin"] + \
              ["SiNGold/base/pak%d.sin" % i for i in range(6)]


def stock_source(rel):
    """The share file for `rel`, or - for an exe the share holds patched - its
    backup, which must be the pinned original (never a guess)."""
    src = P.ci_path(LIB, rel)
    assert src, "%s missing from the share (case-insensitive)" % rel
    s = next((s for s in P.EXES if "%s/%s" % (s["title"], s["rel"]) == rel), None)
    if s is None or P.md5(open(src, "rb").read()) == s["md5"]:
        return src
    b = P.ci_path(LIB, P.backup_rel_of(s["title"], s["rel"]))
    assert b, "%s is not stock on the share and has no backup" % rel
    assert P.md5(open(b, "rb").read()) == s["md5"], "%s: backup is not the pinned original" % b
    return b


@pytest.fixture
def synth(tmp_path):
    """tmp_path/lib mirrors what load_originals read BEFORE the deploy, as
    symlinks to the real share files (stock exes from their backups);
    quake2.exe is a private COPY so a test can damage it."""
    need_share()
    lib = tmp_path / "lib"
    for rel in SYNTH_LINKS:
        src = stock_source(rel)
        dst = lib / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if rel == "Quake2Complete/quake2.exe":
            shutil.copyfile(src, dst)
        else:
            os.symlink(src, dst)
    return lib


def q2(synth):
    return synth / "Quake2Complete" / "quake2.exe"


def backup(synth, data):
    b = synth / P.backup_rel_of("Quake2Complete", "quake2.exe")
    b.parent.mkdir(parents=True, exist_ok=True)
    b.write_bytes(data)


def test_synth_library_is_a_faithful_stand_in(synth):
    work = P.load_originals(str(synth))
    got = {(w["title"], w["rel"]): (P.md5(w["data"]), w["damaged"]) for w in work}
    assert got == {k: (v, None) for k, v in EXPECTED_OUTPUT_MD5.items()}
    # the pre-deploy state: stock exes, no new file yet
    assert {w["state"] for w in work} == {"stock (original)", "absent (new file)"}


def test_publish_plan_backs_up_each_replaced_original_first(synth, tmp_path, monkeypatch):
    """--publish is NOT run in the build phase: sharewrite is replaced by a
    recorder, so this only proves the ORDER and the backup paths (no write)."""
    out = str(tmp_path / "o")
    assert P.cmd_build(str(synth), out) == 0
    calls = []
    monkeypatch.setattr(P, "sharewrite", lambda local, dest, dry: calls.append((local, dest)) or 0)
    assert P.cmd_publish(str(synth), out, dry=True) == 0
    dests = [d for _l, d in calls]
    pre = "Files/Games-Library/"
    assert dests == [
        pre + "_patches/Quake2Complete/originals-2026-09-29/quake2.exe",
        pre + "Quake2Complete/quake2.exe",
        pre + "_patches/SiNGold/originals-2026-09-29/sin.exe",
        pre + "SiNGold/sin.exe",
        pre + "_patches/SoldierOfFortune/originals-2026-09-29/SoF.exe",
        pre + "SoldierOfFortune/SoF.exe",
        pre + "SiNGold/base/menus/main.mnu",
        pre + "SiNGold/2015/menus/main.mnu",
        pre + "SiNGold/ctf/menus/main.mnu",
        pre + "SoldierOfFortune/base/pak2.pak",
    ]
    # each backup is the STAGED original, each put is our built output
    assert calls[0][0].startswith(str(synth)) and calls[1][0].startswith(out)
    for (local, _d), s in zip(calls[0:6:2], P.EXES):      # the backups are stock
        assert P.md5(open(local, "rb").read()) == s["md5"]


def test_publish_stops_on_the_first_failure(synth, tmp_path, monkeypatch):
    out = str(tmp_path / "o")
    assert P.cmd_build(str(synth), out) == 0
    calls = []
    monkeypatch.setattr(P, "sharewrite", lambda local, dest, dry: calls.append(dest) or 3)
    assert P.cmd_publish(str(synth), out, dry=True) == 2
    assert len(calls) == 1


@pytest.mark.parametrize("damage", ["missing", "truncated-output", "truncated-original"])
def test_a_half_published_exe_is_recovered_from_its_verified_backup(synth, damage):
    orig = q2(synth).read_bytes()
    out = P.apply_edits(orig, P.exe_edits(P.EXES[0]))
    backup(synth, orig)
    if damage == "missing":
        q2(synth).unlink()
    elif damage == "truncated-output":
        q2(synth).write_bytes(out[:0x54470])      # died inside the edited bytes
    else:
        q2(synth).write_bytes(orig[:4096])
    w = {(x["title"], x["rel"]): x for x in P.load_originals(str(synth))}[("Quake2Complete", "quake2.exe")]
    assert w["damaged"] and w["state"].startswith("DAMAGED")
    assert P.md5(w["data"]) == EXPECTED_OUTPUT_MD5[("Quake2Complete", "quake2.exe")]
    assert w["orig_path"] is None, "the backup is the original now - never re-back-up damage"
    assert P.cmd_check(str(synth)) == 2, "a damaged share must never read CHECK OK"


def test_a_deployed_exe_reports_its_backup_and_refuses_a_wrong_one(synth):
    """The deployed state: the share holds OUR patch. The original is rebuilt by
    reversing the edits (pinned md5); the backup must BE that original."""
    orig = q2(synth).read_bytes()
    out = P.apply_edits(orig, P.exe_edits(P.EXES[0]))
    q2(synth).write_bytes(out)
    load = lambda: {(x["title"], x["rel"]): x for x in  # noqa: E731
                    P.load_originals(str(synth))}[("Quake2Complete", "quake2.exe")]
    w = load()
    assert w["state"].startswith("ALREADY PATCHED on the share; NO backup")
    assert P.md5(w["data"]) == EXPECTED_OUTPUT_MD5[("Quake2Complete", "quake2.exe")]
    assert w["orig_path"] is None and not w["damaged"]
    backup(synth, orig)
    assert load()["state"] == "ALREADY PATCHED on the share; backup verified"
    backup(synth, out)                            # a backup that is not the original
    with pytest.raises(P.PatchError, match="backup"):
        load()


def test_a_half_published_exe_without_a_backup_is_refused(synth):
    q2(synth).unlink()
    with pytest.raises(P.PatchError):
        P.load_originals(str(synth))


def test_a_changed_exe_is_never_overwritten_even_with_a_backup(synth):
    orig = q2(synth).read_bytes()
    backup(synth, orig)
    other = bytearray(orig)
    other[0x100] ^= 0xff                          # somebody else's build, same size
    q2(synth).write_bytes(bytes(other))
    with pytest.raises(P.PatchError):
        P.load_originals(str(synth))


def test_a_truncated_new_file_is_put_again_and_a_foreign_one_refused(synth, tmp_path, monkeypatch):
    out = {(x["title"], x["rel"]): x for x in P.load_originals(str(synth))}
    menu = out[("SiNGold", "ctf/menus/main.mnu")]["data"]
    dst = synth / "SiNGold" / "ctf" / "menus" / "main.mnu"
    dst.parent.mkdir(parents=True)
    dst.write_bytes(menu[:1000])
    w = {(x["title"], x["rel"]): x for x in P.load_originals(str(synth))}[("SiNGold", "ctf/menus/main.mnu")]
    assert w["damaged"] and w["state"].startswith("DAMAGED")
    # publish (sharewrite stubbed) puts exactly the damaged file again - and,
    # in this synthetic library, the exes too, whose backups do not exist here
    assert P.cmd_build(str(synth), str(tmp_path / "o")) == 0
    calls = []
    monkeypatch.setattr(P, "sharewrite", lambda l, d, dry: calls.append(d) or 0)
    assert P.cmd_publish(str(synth), str(tmp_path / "o"), dry=True) == 0
    assert "Files/Games-Library/SiNGold/ctf/menus/main.mnu" in calls
    dst.write_bytes(b"somebody else's menu")
    with pytest.raises(P.PatchError):
        P.load_originals(str(synth))


@pytest.mark.parametrize("title,gamedir,name,magic", [
    ("SiNGold", "base", "pak6.sin", b"SPAK"),
    ("SiNGold", "ctf", "pak2.sin", b"SPAK"),
    ("SoldierOfFortune", "base", "pak3.pak", b"PACK"),
])
def test_a_newer_pak_carrying_the_menu_stops_the_build(synth, title, gamedir, name, magic):
    """If a later pak starts shipping the menu, the relabel must be re-derived
    from it - or the loose file / pak2 would silently revert that pak."""
    entry = "menus/m_video.rmf" if title == "SoldierOfFortune" else "menus/main.mnu"
    write_pak(synth / title / gamedir / name, [(entry, b"newer")], magic)
    with pytest.raises(P.PatchError, match="engine loads"):
        P.load_originals(str(synth))
