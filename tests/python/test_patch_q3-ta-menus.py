"""Quake III Team Arena menus: Video Mode gets an r_mode -1 "Native" entry that
shows the box's own r_customwidth/r_customheight, and the Quality presets
(which wrote r_mode 4/3) are removed.
provisioning/patches/q3-ta-menus/apply.py builds missionpack/zz-fleetres-menus.pk3.

The pure tests run without the share. They cover:
  - the edit logic, on a synthetic menu of our own;
  - a tokenizer/parser that mimics botlib + ui_shared, including the traps it
    exists to catch: adjacent string concatenation, a brace inside a script,
    an unknown keyword, and '-1' lexing as '-' then 1;
  - the painted-label and one-click semantics of the multi control, before
    and after the fix;
  - the layout against the real glyph advances;
  - the pk3's reproducibility and its search-order win.

The share tests verify the staged originals and the pinned output md5s. They
also run the validator over every stock TA menu to show that it raises no
false positives. They SKIP LOUDLY when the share is absent.
"""
import importlib.util
import io
import json
import os
import shutil
import types
import zipfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
APPLY = os.path.join(REPO, "provisioning", "patches", "q3-ta-menus", "apply.py")

_spec = importlib.util.spec_from_file_location("q3ta_apply", APPLY)
ap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ap)

LIB = ap.LIBRARY
SHARE = os.path.isdir(os.path.join(LIB, ap.TITLE))

# Built 2026-09-29 from pak0.pk3 e8ba9e3b...; a change here means the output changed.
PATCHED_MD5 = {"ui/system.menu": "4cd05af14011932c330157015f687b01",
               "ui/ingame_system.menu": "b3466223d6cd10eeba56d09a47bb24b9"}
PK3_MD5 = "627013444d74562c7ed28ed9caa895f2"

# Glyph advances (xSkip) of the characters the Video Mode row paints, read from
# missionpack/pak0.pk3 fonts/fontImage_12.dat (smallFont, glyphScale 4.0) and
# fontImage_16.dat (font, glyphScale 3.0).
XSKIP = {
    "small": dict(zip("0123456789x*NativeV", [7, 6, 7, 7, 7, 7, 8, 6, 7, 8, 7, 4, 8, 7, 5, 4, 7, 7, 7])),
    "text": dict(zip("0123456789x*NativeV", [10, 7, 9, 9, 9, 10, 10, 7, 10, 10, 7, 6, 10, 9, 6, 5, 9, 9, 8])),
}
GLYPHSCALE = {"small": 4.0, "text": 3.0, "big": 2.4}


def fake_fonts():
    out = {}
    for k in ("small", "text", "big"):
        xs = [0] * 256
        for c, v in XSKIP.get(k, XSKIP["text"]).items():
            xs[ord(c)] = v
        out[k] = {"xskip": xs, "glyphscale": GLYPHSCALE[k]}
    return out


# --- a synthetic menu of our own, shaped like the Team Arena System page -----
def synth_menu(sep="x"):
    lab = lambda w, h: '"%d%s%d" %d' % (w, sep, h, 0)  # noqa: E731
    modes = " ".join('"%d%s%d" %d' % (w, sep, h, i) for i, (w, h) in
                     enumerate([(320, 240), (400, 300), (512, 384), (640, 480)]))
    del lab
    t = ('#include "ui/menudef.h"\r\n\r\n{\r\n\\\\ TEST MENU \\\\\r\n\r\n'
         'menuDef {\r\n\tname "t"\r\n\tvisible 0\r\n\trect 0 0 300 300\r\n'
         '\tonOpen { hide grpSystem ; show graphics }\r\n\r\n'
         '\titemDef {\r\n\t\tname graphics\r\n\t\tgroup grpSystem\r\n\t\ttext "Quality:"\r\n'
         '\t\ttype ITEM_TYPE_MULTI\r\n\t\tcvar "ui_glCustom"\r\n'
         '\t\tcvarFloatList { "Best" 0 "Worst" 3 }\r\n\t\trect 0 50 256 20\r\n'
         '\t\tvisible 0\r\n\t\taction { uiScript update "ui_glCustom" }\r\n\t}\r\n\r\n'
         '\titemDef {\r\n\t\tname graphics\r\n\t\tgroup grpSystem\r\n'
         '\t\ttype ITEM_TYPE_MULTI\r\n\t\ttext "Video Mode:"\r\n\t\tcvar "r_mode"\r\n'
         '\t\tcvarFloatList { ' + modes + ' }\r\n'
         '\t\trect 0 110 256 20\r\n\t\ttextalign ITEM_ALIGN_RIGHT\r\n\t\ttextalignx 133\r\n'
         '\t\tvisible 0\r\n\t\taction { uiScript glCustom }\r\n\t}\r\n\r\n'
         '\titemDef {\r\n\t\tname graphics\r\n\t\ttext "Apply"\r\n\t\ttype 1\r\n'
         '\t\trect 101 295 75 20\r\n\t\tvisible 0\r\n\t\taction { exec "vid_restart" }\r\n\t}\r\n'
         '}\r\n}\r\n')
    return t.encode("ascii")


def synth_spec(data, sep="x"):
    """The spec apply.py would pin for this synthetic file."""
    base = dict(ap.MEMBERS["ui/ingame_system.menu"])
    anchor = ('cvarFloatList { "320%s240" 0 ' % sep).encode()
    q = data.find(b'cvar "ui_glCustom"')
    qs = data.rfind(b"\r\n", 0, data.rfind(b"itemDef", 0, q)) + 2
    qe = data.find(b"\t}\r\n", q) + 4
    v = data.find(b'cvar "r_mode"')
    ve = data.find(b"\t}\r\n", v) + 4
    base.update(size=len(data), md5=ap.md5_bytes(data), list_anchor=anchor,
                list_off=data.find(anchor), quality_off=qs, quality_len=qe - qs,
                quality_md5=ap.md5_bytes(data[qs:qe]), vmode_end=ve, sep=sep)
    return base


# ---------------------------------------------------------------------------
# edits
# ---------------------------------------------------------------------------
def test_native_goes_first_in_both_lists():
    for m, spec in ap.MEMBERS.items():
        new = ap.new_list_anchor(spec)
        assert new.startswith(b'cvarFloatList { "Native" -1 "320'), m
        assert new.endswith(spec["list_anchor"][len(b"cvarFloatList { "):])


def test_synthetic_patch_passes_the_audit_and_the_original_fails_it():
    data = synth_menu()
    spec = synth_spec(data)
    new = ap.patch_member(data, spec)
    orig_probs = ap.audit_menu(ap.parse_menu_file(data.decode()))
    assert any("first r_mode entry" in p for p in orig_probs)
    assert any("still writes r_mode 1" in p for p in orig_probs)
    assert any("Quality" in p for p in orig_probs)
    assert ap.audit_menu(ap.parse_menu_file(new.decode())) == []
    assert ap.structure_problems(data.decode(), new.decode()) == []
    # CRLF throughout, no lone LF introduced, ASCII only
    assert new.count(b"\n") == new.count(b"\r\n")
    new.decode("ascii")


def test_patch_refuses_a_moved_or_changed_original():
    data = synth_menu()
    spec = synth_spec(data)
    with pytest.raises(ValueError):
        ap.locate_edits(b" " + data, spec)          # every offset moved
    bad = dict(spec, quality_md5="0" * 32)
    with pytest.raises(ValueError):
        ap.locate_edits(data, bad)
    with pytest.raises(ValueError):
        ap.locate_edits(data.replace(b'"320x240" 0', b'"320x240" 9'), spec)


def test_overlay_items_are_type_text_decorations_bound_to_the_cvars():
    for m, spec in ap.MEMBERS.items():
        o = ap.overlay_items(spec).decode()
        assert o.count("itemDef {") == 3
        assert o.count("decoration") == 3 and o.count("visible 0") == 3
        assert o.count("type ITEM_TYPE_TEXT") == 3
        assert 'cvar "r_customwidth"' in o and 'cvar "r_customheight"' in o
        assert 'text "%s"' % spec["sep"] in o
        # the gate separates its values with ';' - see the concatenation test
        assert o.count('showCvar { "-1" ; "-1.000000" }') == 3
        assert "(" not in o and ")" not in o


def test_output_name_has_no_parentheses_and_wins_the_search_order():
    assert "(" not in ap.OUT_PK3_NAME and ")" not in ap.OUT_PK3_NAME
    assert ap.wins_search_order(["pak0.pk3", "pak1.pk3", "pak2.pk3", "pak3.pk3"], ap.OUT_PK3_NAME)
    assert ap.wins_search_order(["PAK0.PK3", "PAK3.PK3"], ap.OUT_PK3_NAME)
    assert not ap.wins_search_order(["pak0.pk3"], "aa-fleetres-menus.pk3")
    assert not ap.wins_search_order(["zzz.pk3"], ap.OUT_PK3_NAME)


def test_search_order_upper_cases_like_fs_pathcmp():
    # FS_PathCmp compares a-z UPPER-cased: '_' (0x5F) sorts after every letter
    # there, but before 'a'..'z' in a lower-cased model - the old key got this
    # pair backwards.
    assert ap.wins_search_order(["zza.pk3"], "zz_menus.pk3")
    assert not ap.wins_search_order(["zz_menus.pk3"], "zza.pk3")
    assert ap.pk3_sort_key("a\\b:c.pk3") == "A/B/C.PK3"
    # a '_'-named neighbour WOULD outrank our real name ('-' 0x2D < '_' 0x5F),
    # which is exactly what check() must report if one is ever staged
    assert not ap.wins_search_order(["pak3.pk3", "zz_other.pk3"], ap.OUT_PK3_NAME)


# ---------------------------------------------------------------------------
# the tokenizer/parser traps
# ---------------------------------------------------------------------------
def test_adjacent_strings_concatenate_like_botlib():
    toks = ap.tokenize('showCvar { "-1" "-1.000000" }')
    assert ("string", "-1-1.000000") in [(t[0], t[1]) for t in toks]
    toks = ap.tokenize('showCvar { "-1" ; "-1.000000" }')
    assert [t[1] for t in toks if t[0] == "string"] == ["-1", "-1.000000"]


def test_a_gate_without_separators_never_matches():
    item = {"cvartest": ["r_mode"], "showcvar": [ap._p_script(ap._Stream(
        ap.tokenize('{ "-1" "-1.000000" }'), "t"))]}
    assert not ap.gate_shows(item, {"r_mode": "-1"})
    item["showcvar"] = [ap._p_script(ap._Stream(ap.tokenize('{ "-1" ; "-1.000000" }'), "t"))]
    assert ap.gate_shows(item, {"r_mode": "-1"})
    assert ap.gate_shows(item, {"r_mode": "-1.000000"})
    assert not ap.gate_shows(item, {"r_mode": "6"})
    hide = {"cvartest": ["g_gametype"], "hidecvar": ['"6" ; "7" ']}
    assert ap.gate_shows(hide, {"g_gametype": "4"}) and not ap.gate_shows(hide, {"g_gametype": "7"})


def test_minus_one_lexes_as_minus_then_number_and_parses_negative():
    toks = [(t[0], t[1]) for t in ap.tokenize('cvarFloatList { "Native" -1 }')]
    assert ("punct", "-") in toks and ("number", "1") in toks
    s = ap._Stream(ap.tokenize('{ "Native" -1 "a" 0 }'), "t")
    assert ap._p_floatlist(s) == [("Native", -1.0), ("a", 0.0)]


def _menu(body):
    return '{\r\nmenuDef {\r\n\tname "t"\r\n\titemDef {\r\n' + body + '\t}\r\n}\r\n}\r\n'


@pytest.mark.parametrize("body,msg", [
    ('\t\tcvarTst "r_mode"\r\n', "unknown itemDef keyword"),
    ('\t\taction { show a { x } }\r\n', "inside a script"),
    ('\t\ttype ITEM_TYPE_NOPE\r\n', "expected integer"),
    ('\t\trect 1 2 3\r\n\t\tvisible 0\r\n', "expected float"),
    ('\t\tcvarFloatList { "a" }\r\n', "expected float"),
    ('\t\ttext "unterminated\r\n', "newline inside string"),
    ('\t\ttext "bad \\q escape"\r\n', "unknown escape"),
])
def test_parser_rejects_what_ui_shared_would_misread(body, msg):
    with pytest.raises(ap.MenuError, match=msg):
        ap.parse_menu_file(_menu(body))


def test_an_extra_brace_in_an_item_is_caught():
    # the item closes early, so its remaining keywords become menu keywords
    with pytest.raises(ap.MenuError, match="unknown menuDef keyword"):
        ap.parse_menu_file(_menu('\t\tname a\r\n\t}\r\n\t\tcvar "r_mode"\r\n'))


def test_multi_cvar_limit():
    lst = " ".join('"m%d" %d' % (i, i) for i in range(31))
    ap.parse_menu_file(_menu('\t\tcvarFloatList { %s }\r\n' % lst))
    lst = " ".join('"m%d" %d' % (i, i) for i in range(32))
    with pytest.raises(ap.MenuError, match="MAX_MULTI_CVARS"):
        ap.parse_menu_file(_menu('\t\tcvarFloatList { %s }\r\n' % lst))


def test_brace_profile_is_relative():
    good = synth_menu().decode()
    assert ap.structure_problems(good, good.replace("}\r\n}\r\n", "}\r\n}\r\n}\r\n")) != []


# ---------------------------------------------------------------------------
# multi control semantics: the bug and the fix
# ---------------------------------------------------------------------------
def _vm(data):
    items = [it for m in ap.parse_menu_file(data.decode()) for it in m["items"]]
    return [it for it in items if ap.field(it, "cvar") == "r_mode"][0]


def test_multi_label_and_click_before_and_after():
    data = synth_menu()
    old, new = _vm(data), _vm(ap.patch_member(data, synth_spec(data)))
    # measured on .240 (54-B-condump-rmode.txt): blank label, one click = r_mode 1
    assert ap.multi_setting(old, -1.0) == "" and ap.multi_next(old, -1.0) == 1.0
    assert ap.multi_setting(new, -1.0) == "Native" and ap.multi_next(new, -1.0) == 0.0
    # cycling from the last entry comes back to Native
    assert ap.multi_next(new, 3.0) == -1.0


def test_script_values():
    assert ap.script_values('"-1" ; "-1.000000" ') == ["-1", "-1.000000"]
    assert ap.script_values('"6" ; "7" ') == ["6", "7"]


# ---------------------------------------------------------------------------
# layout
# ---------------------------------------------------------------------------
def test_layout_holds_every_realistic_mode():
    f = fake_fonts()
    for m, spec in ap.MEMBERS.items():
        r = ap.layout_report(f, spec)
        assert r["problems"] == [], (m, r)
        assert r["gap_widest_to_sep"] >= ap.MIN_GAP


def test_layout_catches_a_collision():
    f = fake_fonts()
    spec = dict(ap.MEMBERS["ui/ingame_system.menu"], x_sep=ap.MEMBERS["ui/ingame_system.menu"]["x_sep"] - 5)
    assert ap.layout_report(f, spec)["problems"]
    spec = dict(ap.MEMBERS["ui/system.menu"], x_w=270)
    assert ap.layout_report(f, spec)["problems"]


def test_text_width_picks_the_font_like_text_width():
    f = fake_fonts()
    assert ap.text_width(f, "1920", 0.25) == 28 * 4.0 * 0.25        # smallFont
    assert abs(ap.text_width(f, "1920", 0.333) - 36 * 3.0 * 0.333) < 1e-9  # font


def test_parse_font_layout():
    blob = bytearray(256 * 80 + 4 + 64)
    import struct
    struct.pack_into("<7i", blob, ord("7") * 80, 1, 2, 3, 4, 6, 5, 5)
    struct.pack_into("<f", blob, 256 * 80, 4.0)
    f = ap.parse_font(bytes(blob))
    assert f["xskip"][ord("7")] == 6 and f["glyphscale"] == 4.0
    with pytest.raises(ValueError):
        ap.parse_font(b"\0" * 100)


# ---------------------------------------------------------------------------
# pk3
# ---------------------------------------------------------------------------
def test_pk3_is_stored_deterministic_and_reads_back():
    members = {"ui/system.menu": b"a\r\n", "ui/ingame_system.menu": b"b\r\n"}
    a, b = ap.build_pk3(members), ap.build_pk3(dict(reversed(list(members.items()))))
    assert a == b
    with zipfile.ZipFile(io.BytesIO(a)) as z:
        assert z.testzip() is None
        for i in z.infolist():
            assert i.compress_type == zipfile.ZIP_STORED
            assert i.date_time == ap.ZIP_TIME
            assert not i.flag_bits & 0x8          # no data descriptor
        assert z.read("ui/system.menu") == b"a\r\n"


# ---------------------------------------------------------------------------
# --publish / --install-server (FUTURE use) against a FAKE share - the real
# sharewrite.py is never called: _put is replaced and MNT points at tmp_path.
# ---------------------------------------------------------------------------
class FakeShare:
    """Stands in for sharewrite.py put: records every call (in order, with the
    bytes it was handed) and, unless dry-run or told to fail, lands the file."""

    def __init__(self, root):
        self.root, self.calls, self.fail_on = root, [], None

    def put(self, local, share_path, dry_run):
        with open(local, "rb") as fh:
            self.calls.append((share_path, fh.read(), dry_run))
        if self.fail_on and self.fail_on in share_path:
            return 3
        if not dry_run:
            dest = os.path.join(self.root, *share_path.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(local, dest)
        return 0


PATCHED = b"PATCHED-PK3"


@pytest.fixture
def fake_publish(tmp_path, monkeypatch):
    share = tmp_path / "share"
    mp = share / "Files" / "Games-Library" / ap.TITLE / "missionpack"
    mp.mkdir(parents=True)
    out = tmp_path / "out"
    out.mkdir()
    pk3 = out / ap.OUT_PK3_NAME
    pk3.write_bytes(PATCHED)
    members = {"ui/system.menu": "a" * 32, "ui/ingame_system.menu": "b" * 32}
    (out / "manifest.json").write_text(json.dumps({"outputs": [{
        "share_path": ap.SHARE_OUT, "local_path": str(pk3), "md5": ap.md5_bytes(PATCHED),
        "size": len(PATCHED), "members": {k: {"md5": v} for k, v in members.items()}}]}))
    monkeypatch.setattr(ap, "MNT", str(share))
    monkeypatch.setattr(ap, "check", lambda library, full_md5=True, verbose=True: (
        {"ok": True, "members": {k: {"patched_md5": v} for k, v in members.items()}}, None))
    fs = FakeShare(str(share))
    monkeypatch.setattr(ap, "_put", fs.put)
    backup = share.joinpath(*ap.BACKUP_DIR.split("/")) / "missionpack" / ap.OUT_PK3_NAME
    return types.SimpleNamespace(share=share, mp=mp, out=str(out), pk3=pk3, fs=fs,
                                 lib=str(share / "Files" / "Games-Library"), backup=backup,
                                 members=members)


def test_publish_new_file_is_one_put_and_then_idempotent(fake_publish):
    f = fake_publish
    ap.cmd_publish(f.out, False, f.lib)
    assert [c[0] for c in f.fs.calls] == [ap.SHARE_OUT]          # no original -> no backup
    assert (f.mp / ap.OUT_PK3_NAME).read_bytes() == PATCHED
    ap.cmd_publish(f.out, False, f.lib)                          # share already has the md5
    assert len(f.fs.calls) == 1


def test_publish_backs_up_a_different_share_copy_before_replacing_it(fake_publish):
    f = fake_publish
    (f.mp / ap.OUT_PK3_NAME).write_bytes(b"OLDER")
    ap.cmd_publish(f.out, False, f.lib)
    assert [c[0] for c in f.fs.calls] == [
        ap.BACKUP_DIR + "/missionpack/" + ap.OUT_PK3_NAME, ap.SHARE_OUT]
    assert f.fs.calls[0][1] == b"OLDER"                          # the backup holds what was replaced
    assert f.backup.read_bytes() == b"OLDER"
    assert (f.mp / ap.OUT_PK3_NAME).read_bytes() == PATCHED


def test_publish_never_overwrites_an_existing_backup(fake_publish):
    f = fake_publish
    f.backup.parent.mkdir(parents=True)
    f.backup.write_bytes(b"FIRST-ORIGINAL")
    (f.mp / ap.OUT_PK3_NAME).write_bytes(b"OLDER")
    ap.cmd_publish(f.out, False, f.lib)
    assert [c[0] for c in f.fs.calls] == [ap.SHARE_OUT]
    assert f.backup.read_bytes() == b"FIRST-ORIGINAL"


def test_publish_stops_on_the_first_failure(fake_publish):
    f = fake_publish
    (f.mp / ap.OUT_PK3_NAME).write_bytes(b"OLDER")
    f.fs.fail_on = "originals-"
    with pytest.raises(SystemExit, match="backup .* failed"):
        ap.cmd_publish(f.out, False, f.lib)
    assert len(f.fs.calls) == 1                                  # the replacement was never tried
    assert (f.mp / ap.OUT_PK3_NAME).read_bytes() == b"OLDER"


def test_publish_refuses_when_the_share_is_not_visible(fake_publish):
    f = fake_publish
    shutil.rmtree(str(f.share / "Files"))
    with pytest.raises(SystemExit, match="not visible"):
        ap.cmd_publish(f.out, False, f.lib)
    assert f.fs.calls == []


def test_publish_refuses_a_stale_manifest_or_a_changed_original(fake_publish, monkeypatch):
    f = fake_publish
    monkeypatch.setattr(ap, "check", lambda *a, **k: (
        {"ok": True, "members": {m: {"patched_md5": "c" * 32} for m in f.members}}, None))
    with pytest.raises(SystemExit, match="run --build again"):
        ap.cmd_publish(f.out, False, f.lib)
    monkeypatch.setattr(ap, "check", lambda *a, **k: ({"ok": False, "members": {}}, None))
    with pytest.raises(SystemExit, match="no longer passes --check"):
        ap.cmd_publish(f.out, False, f.lib)
    assert f.fs.calls == []


def test_publish_refuses_a_local_pk3_changed_after_build(fake_publish):
    f = fake_publish
    f.pk3.write_bytes(b"TAMPERED")
    with pytest.raises(SystemExit, match="changed since --build"):
        ap.cmd_publish(f.out, False, f.lib)
    assert f.fs.calls == []


def test_publish_dry_run_writes_nothing_and_says_so(fake_publish, capsys):
    f = fake_publish
    ap.cmd_publish(f.out, True, f.lib)
    assert f.fs.calls and all(c[2] for c in f.fs.calls)          # --dry-run reaches sharewrite
    assert not (f.mp / ap.OUT_PK3_NAME).exists()
    out = capsys.readouterr().out
    assert "dry-run" in out and "published" not in out


def test_publish_finds_the_share_copy_case_insensitively(fake_publish):
    f = fake_publish
    f.mp.rename(f.mp.parent / "MissionPack")
    (f.mp.parent / "MissionPack" / ap.OUT_PK3_NAME.upper()).write_bytes(PATCHED)
    ap.cmd_publish(f.out, False, f.lib)
    assert f.fs.calls == []                                      # same md5 -> skipped, not re-put


def test_install_server_copies_verifies_and_is_idempotent(tmp_path, monkeypatch, capsys):
    out = tmp_path / "out"
    out.mkdir()
    pk3 = out / ap.OUT_PK3_NAME
    pk3.write_bytes(PATCHED)
    (out / "manifest.json").write_text(json.dumps({"outputs": [{
        "share_path": ap.SHARE_OUT, "local_path": str(pk3), "md5": ap.md5_bytes(PATCHED)}]}))
    srv = tmp_path / "srv"
    monkeypatch.setattr(ap, "SERVER_HOMEPATH", str(srv))
    with pytest.raises(SystemExit, match="q3ta-server installed"):
        ap.cmd_install_server(str(out), False)
    (srv / "missionpack").mkdir(parents=True)
    ap.cmd_install_server(str(out), True)
    assert not (srv / "missionpack" / ap.OUT_PK3_NAME).exists()  # dry-run
    ap.cmd_install_server(str(out), False)
    assert (srv / "missionpack" / ap.OUT_PK3_NAME).read_bytes() == PATCHED
    assert not list((srv / "missionpack").glob("*.tmp"))
    ap.cmd_install_server(str(out), False)
    text = capsys.readouterr().out
    assert "skip" in text and "systemctl --user restart q3ta-server" in text


# ---------------------------------------------------------------------------
# share-dependent
# ---------------------------------------------------------------------------
share = pytest.mark.skipif(not SHARE, reason="SHARE NOT MOUNTED (%s) - the staged-original, "
                           "anchor, output-md5 and stock-menu validator checks did NOT run" % LIB)


@share
def test_staged_originals_anchors_and_layout():
    r, _ = ap.check(LIB, full_md5=False, verbose=False)
    assert r["ok"], r
    for m, got in r["members"].items():
        assert got["owners"] == ["pak0.pk3"], m
        assert got["audit_patched"] == [], m
        assert got["patched_md5"] == PATCHED_MD5[m], m


@share
def test_build_reproduces_the_pinned_pk3(tmp_path):
    man = ap.cmd_build(LIB, str(tmp_path), full_md5=False)
    o = man["outputs"][0]
    assert o["md5"] == PK3_MD5
    assert o["share_path"] == "Files/Games-Library/Quake3-TeamArena/missionpack/zz-fleetres-menus.pk3"
    assert ap.md5_file(o["local_path"]) == PK3_MD5
    assert {k: v["md5"] for k, v in o["members"].items()} == PATCHED_MD5


@share
def test_real_fonts_match_the_embedded_metrics():
    _, src = ap.read_sources(LIB, full_md5=False)
    fonts = ap.fonts_from(src)
    for k in ("small", "text"):
        assert fonts[k]["glyphscale"] == GLYPHSCALE[k]
        for c, v in XSKIP[k].items():
            assert fonts[k]["xskip"][ord(c)] == v, (k, c)


@share
def test_validator_accepts_every_stock_team_arena_menu():
    """No false positives: id's own 53 menus (pak0 + the pak3 1.32 updates) parse."""
    mp = os.path.join(LIB, ap.TITLE, "missionpack")
    _, src = ap.read_sources(LIB, full_md5=False)
    defines = ap.parse_defines(src["ui/menudef.h"].decode("latin-1"))
    n = 0
    for pk in ("pak0.pk3", "pak3.pk3"):
        with zipfile.ZipFile(os.path.join(mp, pk)) as z:
            for name in z.namelist():
                if name.lower().startswith("ui/") and name.lower().endswith(".menu"):
                    ap.parse_menu_file(z.read(name).decode("latin-1"), defines, pk + "!" + name)
                    n += 1
    assert n >= 50
