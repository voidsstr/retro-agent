"""DOOM 3 1.3 zz_fleetvideo.pk4 - the 'Native' entry in the Screen Size menu.

provisioning/patches/doom3/apply.py builds base/zz_fleetvideo.pk4 from the
staged pak007.pk4: copies of guis/mainmenu.gui and guis/mpmain.gui with ONLY
the r_mode choiceDef's choices/values lines changed, so the menu can express
r_mode -1 (the per-box r_customWidth/Height the launcher sets).

The pure-logic tests run anywhere. The share tests read the staged original
and SKIP LOUDLY when the library is not mounted.
"""
import importlib.util
import io
import os
import sys
import zipfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
APPLY = os.path.join(REPO, "provisioning", "patches", "doom3", "apply.py")

spec = importlib.util.spec_from_file_location("doom3_apply", APPLY)
d3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d3)

LIB_TITLE = os.path.join(d3.LIBRARY, d3.TITLE)
share = pytest.mark.skipif(
    not os.path.isdir(LIB_TITLE),
    reason="SKIPPED LOUDLY: %s is not mounted - the staged-original checks did not run" % LIB_TITLE)


def _synthetic(spec_, filler_lines=40):
    """A GUI-shaped document with the anchors on the spec's line numbers."""
    eol = spec_["eol"]
    first = min(e["line"] for e in spec_["edits"])
    lines = [b"// filler %d" % i for i in range(1, first - 2)]
    # choiceDef header two lines above the first anchor, then the rect line
    lines.append(b"\t\t\tchoiceDef OS2Primary {")
    lines.append(b"\t\t\t\trect\t\t210, 1, 100, 14")
    assert len(lines) == first - 1
    for e in sorted(spec_["edits"], key=lambda e: e["line"]):
        assert len(lines) == e["line"] - 1
        lines.append(e["old"])
    lines += [b'\t\t\t\tcvar\t\t"r_mode"', b"\t\t\t\tchoiceType\t1", b"\t\t\t}"]
    lines += [b"// tail %d" % i for i in range(filler_lines)]
    return eol.join(lines) + eol


# ------------------------------------------------------------------ pure logic

def test_patched_menu_maps_native_to_minus_one_for_both_guis():
    for s in d3.MEMBERS:
        c = [e for e in s["edits"] if b"choices" in e["old"]][0]
        v = [e for e in s["edits"] if b"values" in e["old"]][0]
        pairs = d3.choice_pairs(c["new"], v["new"])
        assert pairs[-1] == ("Native", "-1"), s["name"]
        # the six stock modes are untouched and still in r_vidModes order
        assert pairs[:-1] == [("640x480", "3"), ("800x600", "4"), ("1024x768", "5"),
                              ("1152x864", "6"), ("1280x1024", "7"), ("1600x1200", "8")]


def test_old_lines_had_no_native_entry():
    """The bug: stock values stop at 8, so r_mode -1 matches nothing and the
    control shows index 0 ('640x480')."""
    for s in d3.MEMBERS:
        v = [e for e in s["edits"] if b"values" in e["old"]][0]
        q = v["old"].split(b'"')[1].decode()
        assert "-1" not in d3.parse_choice_list(q)
        assert d3.parse_choice_list(q) == ["3", "4", "5", "6", "7", "8"]


def test_stock_mode_labels_equal_str_04222():
    assert d3.STOCK_MODES == "640x480;800x600;1024x768;1152x864;1280x1024;1600x1200"


def test_patch_member_changes_only_the_two_lines_on_synthetic_input():
    for s in d3.MEMBERS:
        old = _synthetic(s)
        new = d3.patch_member(old, s)
        assert d3.verify_patched(old, new, s) == sorted(e["line"] for e in s["edits"])
        assert new.count(s["eol"]) == old.count(s["eol"])      # line endings preserved
        for e in s["edits"]:
            assert e["new"] in new and e["old"] not in new


def test_anchor_outside_the_r_mode_choicedef_is_refused():
    s = d3.MEMBERS[0]
    doc = _synthetic(s).replace(b"choiceDef OS2Primary", b"choiceDef OS9Primary")
    with pytest.raises(d3.PatchError, match="not inside"):
        d3.patch_member(doc, s)
    doc = _synthetic(s).replace(b'"r_mode"', b'"s_volume_dB"')
    with pytest.raises(d3.PatchError, match="not inside"):
        d3.patch_member(doc, s)


def test_duplicate_or_missing_anchor_is_refused():
    s = d3.MEMBERS[0]
    doc = _synthetic(s)
    e = s["edits"][0]
    with pytest.raises(d3.PatchError, match="occurs 2 times"):
        d3.patch_member(doc + e["old"] + s["eol"], s)
    with pytest.raises(d3.PatchError, match="occurs 0 times"):
        d3.patch_member(doc.replace(e["old"], b"\t\t\t\tchoices\t\t\"#str_09999\""), s)


def test_anchor_on_the_wrong_line_is_refused():
    s = d3.MEMBERS[0]
    with pytest.raises(d3.PatchError, match="expected line"):
        d3.patch_member(b"// one extra line\n" + _synthetic(s), s)


def test_already_patched_input_is_refused_not_double_patched():
    s = d3.MEMBERS[0]
    once = d3.patch_member(_synthetic(s), s)
    with pytest.raises(d3.PatchError):
        d3.patch_member(once, s)


def test_verify_patched_catches_an_extra_change():
    s = d3.MEMBERS[0]
    old = _synthetic(s)
    new = d3.patch_member(old, s).replace(b"// tail 3", b"// tail X")
    with pytest.raises(d3.PatchError, match="changed lines"):
        d3.verify_patched(old, new, s)


def test_precedence_zz_sorts_after_every_stock_pak_either_case():
    stock = ["game00.pk4", "game01.pk4", "game02.pk4", "game03.pk4",
             "pak000.pk4", "pak001.pk4", "pak002.pk4", "pak003.pk4",
             "pak004.pk4", "pak005.pk4", "pak006.pk4", "pak007.pk4"]
    assert d3.overrides_all("zz_fleetvideo.pk4", stock + ["zz_fleetvideo.pk4"])
    # a name that sorts before pak007 would lose to pak007's copy of the GUI
    assert not d3.overrides_all("fleetvideo.pk4", stock)
    # an upper-case stock name must not change the answer (Windows keeps case)
    assert d3.overrides_all("zz_fleetvideo.pk4", ["PAK007.PK4", "Pak008.pk4"])
    assert not d3.overrides_all("zz_fleetvideo.pk4", ["zzz_other.pk4"])


def test_pk4_is_deterministic_and_reads_back():
    members = [("guis/a.gui", b"x" * 1000), ("guis/b.gui", b"y\r\n" * 50)]
    a = d3.build_pk4(members)
    b = d3.build_pk4(members)
    assert a == b
    with zipfile.ZipFile(io.BytesIO(a)) as z:
        assert z.testzip() is None
        assert z.namelist() == ["guis/a.gui", "guis/b.gui"]
        assert all(i.compress_type == zipfile.ZIP_DEFLATED for i in z.infolist())
        assert z.read("guis/b.gui") == b"y\r\n" * 50


def test_output_name_and_share_path():
    assert d3.PK4_REL == "base/zz_fleetvideo.pk4"
    assert "(" not in d3.PK4_NAME and ")" not in d3.PK4_NAME


# ----------------------------------------------------------------- the share

@share
def test_check_passes_against_the_staged_original(capsys):
    assert d3.main(["--check"]) == 0
    out = capsys.readouterr().out
    assert "Native=-1" in out
    assert "zz_fleetvideo.pk4 sorts after all" in out


@share
def test_build_output_differs_from_stock_only_on_the_edited_lines(tmp_path):
    assert d3.main(["--build", str(tmp_path)]) == 0
    pk4 = tmp_path / d3.TITLE / "base" / d3.PK4_NAME
    src = os.path.join(LIB_TITLE, d3.SOURCE_PAK["rel"])
    with zipfile.ZipFile(pk4) as z, zipfile.ZipFile(src) as stock:
        assert sorted(z.namelist()) == sorted(s["name"] for s in d3.MEMBERS)
        for s in d3.MEMBERS:
            old = stock.read(s["name"])
            new = z.read(s["name"])
            assert d3.changed_lines(old, new) == sorted(e["line"] for e in s["edits"])
            assert b"Native" in new and b"3;4;5;6;7;8;-1" in new
    import json
    m = json.loads((tmp_path / "manifest.json").read_text())
    o = m["outputs"][0]
    assert o["share_path"] == "Files/Games-Library/Doom3/base/zz_fleetvideo.pk4"
    assert o["md5"] == d3.md5_file(str(pk4))
    assert m["originals"][0]["md5"] == d3.SOURCE_PAK["md5"]


@share
def test_the_stock_gui_the_game_shows_today_is_pak007s():
    """The patch is rebased on pak007; if a later stock pak ever carries these
    GUIs the patch would silently revert newer content."""
    for s in d3.MEMBERS:
        owners = d3.gui_owners(d3.LIBRARY, s["name"])
        assert owners and owners[-1] == "pak007.pk4", owners


# ------------------------------------------------ reviewer additions 2026-09-29
# Two properties the patch depends on, pinned against mirrors of id's GPL code
# (neo/framework/FileSystem.cpp, neo/ui/ChoiceWindow.cpp):
#   1. the pk4 is PURE_NEVER, so no pure server - including the PURE listen
#      server "Host DOOM 3 - LAN.bat" runs (si_pure defaults to 1) - can put it
#      on its list, and it stays active while connected to one;
#   2. the menu selects "Native" for r_mode -1 where the stock one showed index 0.

GUIS = [s["name"] for s in d3.MEMBERS]


def test_pk4_is_pure_never_so_no_pure_server_can_require_it():
    assert all(d3.pure_excluded(n) for n in GUIS)
    assert d3.pak_pure_status(d3.PK4_NAME, GUIS) == "never"
    assert d3.require_pure_never(d3.PK4_NAME, GUIS) == "never"
    # UpdatePureServerChecksums: never listed, even though mpmain.gui IS read at map load
    assert d3.enters_pure_list("never", referenced=True) is False


def test_one_non_gui_member_makes_it_pure_neutral_and_the_build_refuses():
    """A readme next to the GUIs would put this pk4 on a Host LAN game's pure
    list (mpmain.gui is referenced at map load) and lock out every box without it."""
    names = GUIS + ["readme.txt"]
    assert d3.pak_pure_status(d3.PK4_NAME, names) == "neutral"
    assert d3.enters_pure_list("neutral", referenced=True) is True
    with pytest.raises(d3.PatchError, match="PURE_NEUTRAL"):
        d3.require_pure_never(d3.PK4_NAME, names)


def test_pure_status_mirror_follows_id_filesystem_rules():
    ex = d3.pure_excluded
    assert ex("guis/mainmenu.gui") and ex("GUIS/MPMAIN.GUI") and ex("guis\\x.gui")
    assert ex("guis/") and ex("strings/english.lang") and ex("guis/a.pd") and ex("x/a.pda")
    assert ex("sound/VO/x.ogg") and ex("sound/vo/y.wav")
    assert not ex("sound/weapons/x.ogg") and not ex("maps/game/mp/d3dm1.map")
    assert not ex("default.cfg") and not ex(".gui")          # l > extLen, like excludeExtension
    # GetPackStatus: all excluded -> never; else pak* -> always; else neutral
    assert d3.pak_pure_status("zz_x.pk4", []) == "never"
    assert d3.pak_pure_status("pak007.pk4", ["guis/mainmenu.gui", "def/x.def"]) == "always"
    assert d3.pak_pure_status("PAK000.pk4", ["def/x.def"]) == "always"
    assert d3.pak_pure_status("zz_x.pk4", ["def/x.def"]) == "neutral"


def test_pure_never_pak_is_searched_on_a_pure_server_and_a_neutral_one_is_not():
    """The builder's first report said a client-only pk4 is dropped on a pure
    server. That is true for PURE_NEUTRAL (old claim) and false for this pk4."""
    assert d3.searched_on_pure_server("never", on_server_list=False) is True
    assert d3.searched_on_pure_server("neutral", on_server_list=False) is False
    assert d3.searched_on_pure_server("neutral", on_server_list=True) is True


def _values_of(spec, which):
    e = [e for e in spec["edits"] if b"values" in e["old"]][0]
    return d3.engine_values(d3.quoted_field(e[which]))


def test_engine_selects_native_for_minus_one_where_stock_showed_640x480():
    labels_new = d3.parse_choice_list(d3.STOCK_MODES + ";" + d3.NATIVE_LABEL)
    labels_old = d3.parse_choice_list(d3.STOCK_MODES)
    for s in d3.MEMBERS:
        new, old = _values_of(s, "new"), _values_of(s, "old")
        assert new == ["3", "4", "5", "6", "7", "8", "-1"]          # negNum joins "-" "1"
        assert old == ["3", "4", "5", "6", "7", "8"]
        assert labels_new[d3.engine_current_choice("-1", new)] == "Native"        # fixed
        assert labels_old[d3.engine_current_choice("-1", old)] == "640x480"       # old bug
        # the stock modes still select themselves
        assert labels_new[d3.engine_current_choice("5", new)] == "1024x768"
        assert labels_new[d3.engine_current_choice("8", new)] == "1600x1200"


def test_engine_value_mirror_reproduces_the_stock_invert_mouse_control():
    """The stock m_pitch choiceDef (values "0.022;-0.022", choiceType 1) is the
    reference that negative values work in the shipped 1.3 menu."""
    v = d3.engine_values("0.022;-0.022")
    assert v == ["0.022", "-0.022"]
    assert d3.engine_current_choice("-0.022", v) == 1
    assert d3.engine_current_choice("0.022", v) == 0


def test_value_lexer_refuses_what_it_does_not_model():
    for bad in ("3;--1", "3;-=1", "1x2", "a;b", '3;"4"'):
        with pytest.raises(d3.PatchError):
            d3.engine_values(bad)


def test_launcher_premise_detects_a_launcher_that_drops_r_mode_minus_one():
    good = (b'start "" DOOM3.exe +set r_mode -1 +set r_customWidth %FR_W% '
            b'+set r_customHeight %FR_H% +set r_fullscreen 1\r\n')
    assert d3.launcher_premise(good) == []
    assert d3.launcher_premise(good.upper()) == []                     # cmd is case-blind
    assert d3.launcher_premise(good.replace(b"r_mode -1", b"r_mode 5")) == ["+set r_mode -1"]
    assert "+set r_customWidth %FR_W%" in d3.launcher_premise(good.replace(b"r_customWidth", b"r_customwidthX"))


def _fake_output(tmp_path, members=(("guis/mainmenu.gui", b"x"), ("guis/mpmain.gui", b"y"))):
    blob = d3.build_pk4(list(members))
    local = tmp_path / "Doom3" / "base" / d3.PK4_NAME
    local.parent.mkdir(parents=True)
    local.write_bytes(blob)
    o = {"share_path": "Files/Games-Library/Doom3/base/" + d3.PK4_NAME,
         "local_path": str(local), "md5": d3.md5_bytes(blob), "size": len(blob)}
    import json
    (tmp_path / "manifest.json").write_text(json.dumps({"outputs": [o]}))
    return o


def test_install_server_copies_nothing_for_a_pure_never_pk4(tmp_path, monkeypatch, capsys):
    _fake_output(tmp_path)
    srv = tmp_path / "srv"
    srv.mkdir()
    monkeypatch.setattr(d3, "SERVER_BASE", str(srv))
    assert d3.cmd_install_server(str(tmp_path), dry_run=False) == 0
    assert list(srv.iterdir()) == []
    out = capsys.readouterr().out
    assert "NOT NEEDED" in out and "systemctl" not in out


def test_publish_flow_backs_up_first_is_idempotent_and_stops_on_failure(tmp_path, monkeypatch):
    """--publish's control flow with everything that touches the world stubbed:
    no sharewrite subprocess, no /mnt read."""
    o = _fake_output(tmp_path)
    target = o["share_path"]
    backup = "Files/Games-Library/_patches/Doom3/%s/base/%s" % (d3.BACKUP_DIR, d3.PK4_NAME)

    def no_subprocess(*a, **k):
        raise AssertionError("the flow test must not run sharewrite.py")
    monkeypatch.setattr(d3.subprocess, "call", no_subprocess)
    monkeypatch.setattr(d3, "read_source", lambda lib: None)

    def run(state, fail_on=None, land=True):
        calls = []

        def fake_put(local, share_path, dry_run):
            calls.append((local, share_path))
            if share_path == fail_on:
                return 1
            if land:
                state[share_path] = (d3.md5_file(local) if os.path.isfile(local)
                                     else state.get(target))
            return 0
        monkeypatch.setattr(d3, "_share_md5", lambda sp: state.get(sp))
        monkeypatch.setattr(d3, "_put", fake_put)
        return d3.cmd_publish("/nonexistent", str(tmp_path), dry_run=False), calls

    # new file: one put, no backup
    rc, calls = run({})
    assert rc == 0 and [c[1] for c in calls] == [target]
    # already published: nothing to do
    rc, calls = run({target: o["md5"]})
    assert rc == 0 and calls == []
    # a different file sits there: it is backed up BEFORE it is replaced
    rc, calls = run({target: "0" * 32})
    assert rc == 0 and [c[1] for c in calls] == [backup, target]
    assert calls[0][0] == d3._share_abs(target)
    # the backup fails: stop, and the original is never replaced
    rc, calls = run({target: "0" * 32}, fail_on=backup)
    assert rc == 1 and [c[1] for c in calls] == [backup]
    # the put "succeeds" but the md5 does not read back: a failure, not a success
    rc, calls = run({}, land=False)
    assert rc == 1


@share
def test_literal_labels_equal_the_stock_language_string():
    with zipfile.ZipFile(os.path.join(LIB_TITLE, d3.SOURCE_PAK["rel"])) as z:
        lang = z.read("strings/english.lang").decode("latin-1")
    assert d3._lang_string(lang, "#str_04222") == d3.STOCK_MODES
    assert d3._lang_string(lang, "#str_02153") == "Screen Size"


@share
def test_built_pk4_on_disk_is_pure_never(tmp_path):
    assert d3.main(["--build", str(tmp_path)]) == 0
    with zipfile.ZipFile(tmp_path / d3.TITLE / "base" / d3.PK4_NAME) as z:
        assert d3.pak_pure_status(d3.PK4_NAME, z.namelist()) == "never"
    import json
    m = json.loads((tmp_path / "manifest.json").read_text())
    assert m["outputs"][0]["pure_status"] == "PURE_NEVER"


@share
def test_staged_launchers_still_pass_r_mode_minus_one():
    for bat in d3.LAUNCHERS:
        with open(d3._ci_path(LIB_TITLE, bat), "rb") as f:
            assert d3.launcher_premise(f.read()) == [], bat
