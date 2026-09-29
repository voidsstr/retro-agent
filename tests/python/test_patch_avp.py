"""Aliens versus Predator Gold: the InstallShield de-obfuscation patch.

provisioning/patches/avp/apply.py undoes the InstallShield 5 file obfuscation
that was left on 154 files of the staged tree - the reason avp.exe has never
reached its menu on this fleet (LOGFILE.TXT: 'Menus\\IntroFont.rim ... not
found', 'ASSERTION FAILED! pSurface alt_tab.cpp 198', no 'Loaded FastFile:').

The pure-logic tests pin the transform (against the literal unshield loop),
the file table (against the independent NakedAVP md5 list), the structure
checks that stand in for a reference on the nine FMVs, the publish order and
its stop-on-first-failure contract, and the AvP_Video.cfg byte layout. They
need no share. The share-side tests SKIP LOUDLY when the library is absent.
"""
import hashlib
import importlib.util
import io
import os
import random
import shutil
import struct

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PATCH_DIR = os.path.join(REPO, "provisioning", "patches", "avp")
LIB = "/mnt/retro-share/Files/Games-Library"

# Loaded by path under a unique name: every patch directory has an apply.py.
_spec = importlib.util.spec_from_file_location("avp_patch_apply_under_test",
                                               os.path.join(PATCH_DIR, "apply.py"))
ap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ap)


def md5(b):
    return hashlib.md5(b).hexdigest()


# --------------------------------------------------------------------------
# The transform.
# --------------------------------------------------------------------------

@pytest.mark.parametrize("length", [0, 1, 70, 71, 72, 1000, 4099])
@pytest.mark.parametrize("seed", [0, 1, 70, 71, 200])
def test_fast_path_equals_the_literal_unshield_loop(length, seed):
    rnd = random.Random(length * 1000 + seed)
    data = bytes(rnd.randrange(256) for _ in range(length))
    assert ap.deobfuscate(data, seed) == ap.deobfuscate_reference(data, seed)


def test_obfuscate_is_the_exact_inverse():
    rnd = random.Random(7)
    data = bytes(rnd.randrange(256) for _ in range(5000))
    for seed in (0, 3, 71):
        assert ap.deobfuscate(ap.obfuscate(data, seed), seed) == data
        assert ap.obfuscate(ap.deobfuscate(data, seed), seed) == data


def _libunshield():
    """unshield's OWN unshield_deobfuscate(), exported by libunshield.so.1.

    The two tests above compare apply.py against apply.py: the fast path and the
    "literal loop" share one author, so a shared misreading of unshield would
    pass both. This one asks the real library, so a wrong transform fails here
    even without the share (where the NakedAVP md5s would catch it).
    """
    import ctypes
    import ctypes.util
    name = ctypes.util.find_library("unshield")
    if not name:
        pytest.skip("SKIPPED LOUDLY: libunshield is not installed (apt install libunshield1) - "
                    "the transform was NOT cross-checked against unshield's own function")
    fn = ctypes.CDLL(name).unshield_deobfuscate
    fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint)]
    fn.restype = None
    return ctypes, fn


@pytest.mark.parametrize("length,seed", [(1, 0), (70, 0), (71, 0), (72, 0), (4099, 0), (5000, 3), (300, 70)])
def test_transform_equals_unshields_own_function(length, seed):
    ctypes, fn = _libunshield()
    data = bytes(random.Random(length * 7 + seed).randrange(256) for _ in range(length))
    buf = ctypes.create_string_buffer(data, length)
    s = ctypes.c_uint(seed)
    fn(buf, length, ctypes.byref(s))
    assert buf.raw[:length] == ap.deobfuscate(data, seed)
    assert s.value == seed + length              # unshield advances the seed once per byte


# Raw heads read off the staged files on 2026-09-29 (static-avp-data-check.txt,
# static-avp-installshield-deobfuscation.txt) and what they must decode to.
KNOWN_HEADS = [
    ("fastfile/Tex1.FFL", "9cc8f4e8c5c1cdc9a5f1fdf946e9ede9", bytes.fromhex("5246464c0000000014000000d8020000")),
    ("fastfile/ffinfo.txt", "c8185818646c703c44e43034302424fc", b"Graphics\\Common;"),
    ("avp_huds/alien.rif", "9cccc4cc8cece43564dbf9f99547e9e9", bytes.fromhex("5245424352494631647a010004970100")),
    ("FMVs/AlienIntro.bik", "dcfce070e53abcc911e5fdf966fbede9", bytes.fromhex("42494b6608f6540029030000e07e0000")),
    ("FMVs/IntroSound.smk", "98ece001f5c1cdc9e5f1fdf9c295ede9", bytes.fromhex("534d4b320400000004000000b9030000")),
]


@pytest.mark.parametrize("path,raw,plain", KNOWN_HEADS)
def test_known_staged_heads_decode_to_their_formats(path, raw, plain):
    assert ap.deobfuscate(bytes.fromhex(raw)) == plain


def test_a_plain_fastfile_is_destroyed_by_the_transform():
    """Why the table must never list Snd*.FFL: they are ALREADY plain RFFL.

    Snd2.FFL's staged head is 'RFFL'; run through the transform it is noise.
    Leaving a file alone is as much a part of this patch as changing one.
    """
    snd2 = bytes.fromhex("5246464c000000000d0000002c020000")
    assert snd2.startswith(b"RFFL")
    assert not ap.deobfuscate(snd2).startswith(b"RFFL")


# --------------------------------------------------------------------------
# The table.
# --------------------------------------------------------------------------

@pytest.fixture(scope="module")
def table():
    return ap.load_table()


@pytest.fixture(scope="module")
def reference():
    return ap.load_reference()


def test_reference_list_is_the_201_file_gold_list(reference):
    assert len(reference) == 201
    assert reference["fastfile/tex1.ffl"] == "d9ecc8666917d167fc0f151287d3546a"
    assert reference["language.txt"] == "10564fea944ef6680191ecc89a4616d5"


def test_table_counts_by_directory(table):
    deob = [r for r in table if r.action == "deobfuscate"]
    keep = [r for r in table if r.action == "keep"]
    assert len(deob) == 154 and len(keep) == 56
    by = {}
    for r in deob:
        top = r.path.split("/")[0]
        by[top] = by.get(top, 0) + 1
    assert by == {"avp_huds": 23, "avp_rifs": 63, "fastfile": 59, "FMVs": 9}
    ff = sorted(r.path.lower() for r in deob if r.path.startswith("fastfile/"))
    assert ff == sorted(["fastfile/ffinfo.txt"] + ["fastfile/tex%d.ffl" % i for i in range(1, 59)])
    fmv = [r.path for r in deob if r.path.startswith("FMVs/")]
    assert sum(p.endswith(".bik") for p in fmv) == 8 and "FMVs/IntroSound.smk" in fmv


def test_table_never_touches_the_files_that_are_already_plain(table):
    for r in table:
        low = r.path.lower()
        if r.action != "deobfuscate":
            continue
        assert not low.startswith("fastfile/snd"), r.path
        assert not low.endswith(("common.ffl", ".dat", "language.txt")), r.path
        assert not low.startswith(("shape_rifs/", "hsound/", "lsound/", "rsound/", "english/",
                                   "mplayer/", "mpconfig/")), r.path
        assert not low.startswith("fmvs/message"), r.path     # the 53 message*.smk are plain


def test_table_agrees_with_the_independent_nakedavp_list(table, reference):
    covered = set()
    for r in table:
        low = r.path.lower()
        if r.verify == "nakedavp":
            assert reference[low] == r.reference_md5, r.path
            covered.add(low)
            if r.action == "deobfuscate":
                assert r.patched_md5 == r.reference_md5, r.path
            else:
                assert r.original_md5 == r.reference_md5 and r.patched_md5 == "-", r.path
        else:
            assert r.action == "deobfuscate" and r.path.startswith("FMVs/"), r.path
            assert r.reference_md5 == "-" and r.verify == r.kind, r.path
    assert covered == set(reference), "every NakedAVP file is either de-obfuscated or kept"
    assert sum(r.action == "deobfuscate" and r.verify == "nakedavp" for r in table) == 145


def test_table_rows_are_well_formed(table):
    seen = set()
    for r in table:
        assert r.path.lower() not in seen, "duplicate (case-insensitive): %s" % r.path
        seen.add(r.path.lower())
        assert r.size > 0 and len(r.original_md5) == 32 and len(r.raw_head16) == 32
        if r.action == "deobfuscate":
            assert len(r.patched_md5) == 32 and r.patched_md5 != r.original_md5


EXPECTED_MAGIC = {"rif": b"REBCRIF1", "rffl": b"RFFL", "bink": b"BIK", "smk": b"SMK2",
                  "ffinfo": b"Graphics\\Common;"}


def test_every_staged_head_in_the_table_decodes_to_its_own_format(table):
    """Without the share: the recorded raw bytes ARE obfuscated copies of what the row claims."""
    for r in table:
        head = bytes.fromhex(r.raw_head16)
        if r.action == "deobfuscate":
            assert ap.deobfuscate(head).startswith(EXPECTED_MAGIC[r.kind]), r.path
        elif r.path.lower().startswith("fastfile/snd"):
            assert head.startswith(b"RFFL"), r.path          # plain as staged


def test_publish_flips_ffinfo_last(table):
    order = ap.publish_order([r for r in table if r.action == "deobfuscate"])
    assert order[-1].path == "fastfile/ffinfo.txt"
    assert len(order) == 154


def _sharewrite_module():
    spec = importlib.util.spec_from_file_location(
        "sharewrite_for_avp_test", os.path.join(REPO, "scripts", "fleet", "sharewrite.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_every_publish_destination_is_one_sharewrite_accepts(table):
    """--publish hands each path to sharewrite.py, which REFUSES '"' and ';'.

    Found at publish time, that refusal would stop the run part-way through a
    141 MB write. Two files have spaces ('pred ship fury.RIF'); those are fine,
    and this pins that the publisher's own quoting agrees.
    """
    sw = _sharewrite_module()
    n = 0
    for r in table:
        if r.action != "deobfuscate":
            continue
        assert "\\" not in r.path and not r.path.startswith("/"), r.path
        for dest in (ap.TITLE_REL + "/" + r.path, ap.BACKUP_REL + "/" + r.path):
            dirs, name = sw.share_parts(dest)             # raises on '..' or an empty path
            for part in dirs + [name]:
                sw.smb_quote(part)                       # raises on '"' or ';'
            n += 1
    assert n == 2 * 154


# --------------------------------------------------------------------------
# Structure checks (the FMVs' only proof) on synthetic files.
# --------------------------------------------------------------------------

def iff(tag, body):
    return tag + struct.pack(">I", len(body)) + body


def make_rffl(entries):
    index, blob = b"", b""
    for name, payload in entries:
        rec = struct.pack("<II", len(blob), len(payload)) + name.encode("latin-1") + b"\0"
        rec += b"\0" * ((-len(rec)) % 4)
        index += rec
        blob += payload
    return b"RFFL" + struct.pack("<IIII", 0, len(entries), len(index), len(blob)) + index + blob


def make_bink(frames=3, tracks=1):
    w, h = 64, 48
    head_len = 44 + 12 * tracks
    table_len = 4 * (frames + 1)
    bodies = []
    for i in range(frames):
        body = b""
        for _t in range(tracks):
            audio = bytes([i]) * (8 + i)
            body += struct.pack("<I", len(audio)) + audio
        body += b"\x5a" * (20 + 3 * i)
        bodies.append(body)
    pos, p = [], head_len + table_len
    for b in bodies:
        pos.append(p)
        p += len(b)
    pos.append(p)
    size = p
    hdr = b"BIKf" + struct.pack("<IIIIIIIIII", size - 8, frames, max(len(b) for b in bodies), frames,
                                w, h, 15, 1, 0, tracks)
    hdr += struct.pack("<I", 4096) * tracks + struct.pack("<HH", 22050, 0) * tracks + struct.pack("<I", 0) * tracks
    table = struct.pack("<%dI" % (frames + 1), *([pos[0] | 1] + pos[1:]))
    return hdr + table + b"".join(bodies)


def make_smk(frames=2):
    trees = b"\x00" * 16
    bodies = []
    for i in range(frames):
        audio = struct.pack("<I", 12) + b"\x11" * 8            # chunk size includes itself
        video = b"\x22" * (16 + 4 * i)
        bodies.append(audio + video)
    hdr = b"SMK2" + struct.pack("<IIIiI", 32, 24, frames, 100, 0)
    hdr += struct.pack("<7I", 4096, 0, 0, 0, 0, 0, 0)
    hdr += struct.pack("<5I", len(trees), 0, 0, 0, 0)
    hdr += struct.pack("<7I", 0x40005622, 0, 0, 0, 0, 0, 0) + struct.pack("<I", 0)
    assert len(hdr) == 104
    sizes = struct.pack("<%dI" % frames, *[len(b) for b in bodies])
    flags = bytes([2] * frames)                                 # audio track 0 in every frame
    return hdr + sizes + flags + trees + b"".join(bodies)


def test_rffl_parser_and_iff_check():
    font = iff(b"LIST", b"RIMdata" * 5)
    data = make_rffl([("graphics\\Menus\\IntroFont.RIM", font), ("graphics\\Menus\\a.RIM", iff(b"FORM", b"xy"))])
    ents = ap.rffl_entries(data)
    assert [e[0] for e in ents] == ["graphics\\Menus\\IntroFont.RIM", "graphics\\Menus\\a.RIM"]
    assert data[ents[0][1]:ents[0][1] + ents[0][2]] == font
    assert "2 files" in ap.rffl_check(data)
    with pytest.raises(ValueError):
        ap.rffl_entries(data[:-1])                               # sizes no longer add up
    bad = make_rffl([("x.RIM", b"LIST" + struct.pack(">I", 99) + b"short")])
    with pytest.raises(ValueError):
        ap.rffl_check(bad)                                       # not a whole IFF chunk
    with pytest.raises(ValueError):
        ap.structure_check("rffl", ap.obfuscate(data))           # still obfuscated


def test_bink_check_walks_every_frame():
    good = make_bink()
    assert "3 frames" in ap.bink_check(good)
    with pytest.raises(ValueError):
        ap.bink_check(good[:-1])                                 # header size != file
    broken = bytearray(good)
    first = struct.unpack_from("<I", good, 56)[0] & ~1
    struct.pack_into("<I", broken, first, 10 ** 6)               # frame 0's audio size
    with pytest.raises(ValueError):
        ap.bink_check(bytes(broken))
    with pytest.raises(ValueError):
        ap.structure_check("bink", ap.obfuscate(good))


def test_smk_check_walks_every_frame():
    good = make_smk()
    assert "2 frames" in ap.smk_check(good)
    with pytest.raises(ValueError):
        ap.smk_check(good + b"\0\0\0\0")                         # size table disagrees
    broken = bytearray(good)
    start = 104 + 5 * 2 + 16
    struct.pack_into("<I", broken, start, 999)                   # audio chunk past its frame
    with pytest.raises(ValueError):
        ap.smk_check(bytes(broken))


def test_ffinfo_parser():
    txt = b"Graphics\\Common;Tex1.FFL\r\ngraphics\\Menus;Tex37.FFL\r\nsound\\Env\\X;Snd2.FFL\r\n"
    assert ap.ffinfo_entries(txt)[1] == ("graphics\\Menus", "Tex37.FFL")
    with pytest.raises(ValueError):
        ap.ffinfo_entries(b"Graphics\\Common Tex1.FFL\r\n")
    with pytest.raises(ValueError):
        ap.ffinfo_entries(b"Graphics\\Common;readme.txt\r\n")


# --------------------------------------------------------------------------
# AvP_Video.cfg - the layout a per-box launcher would write.
# --------------------------------------------------------------------------

def test_avp_video_cfg_layout():
    data = ap.avp_video_cfg(1920, 1080, 32)
    assert data == bytes(16) + bytes.fromhex("00000000" "80070000" "38040000" "20000000")
    assert len(data) == ap.AVP_VIDEO_CFG_SIZE == 32
    p = ap.parse_avp_video_cfg(data)
    assert (p["width"], p["height"], p["bpp"], p["primary_display"]) == (1920, 1080, 32, True)


@pytest.mark.parametrize("w,h,bpp", [(320, 200, 16), (640, 350, 16), (1920, 1080, 8), (1920, 1080, 15)])
def test_avp_video_cfg_refuses_a_mode_the_game_could_never_list(w, h, bpp):
    """A mode avp.exe's own filter drops leaves its mode index at -1 (0x559810)."""
    with pytest.raises(ValueError):
        ap.avp_video_cfg(w, h, bpp)


# --------------------------------------------------------------------------
# check / build / publish on a synthetic share tree.
# --------------------------------------------------------------------------

def _synthetic(tmp_path):
    """A fake share root holding a tiny AvP tree, and the table rows for it."""
    share = tmp_path / "share"
    root = share.joinpath(*ap.TITLE_REL.split("/"))
    plain = {
        "fastfile/ffinfo.txt": b"graphics\\Menus;Tex37.FFL\r\nsound\\Env\\X;Snd2.FFL\r\n",
        "fastfile/Tex37.FFL": make_rffl([("graphics\\Menus\\IntroFont.RIM", iff(b"LIST", b"font" * 40))]),
        "avp_huds/alien.rif": b"REBCRIF1" + b"\x01\x02\x03\x04" * 30,
        "FMVs/logos.bik": make_bink(),
        "FMVs/IntroSound.smk": make_smk(),
    }
    keep = {"fastfile/Snd2.FFL": make_rffl([("s.wav", iff(b"RIFF", b"w" * 12))])}
    rows = []
    for rel, body in plain.items():
        staged = ap.obfuscate(body)
        f = root.joinpath(*rel.split("/"))
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(staged)
        kind = ap.file_kind(rel)
        rows.append(ap.Row(action="deobfuscate", path=rel, size=len(staged), original_md5=md5(staged),
                           patched_md5=md5(body), reference_md5="-" if kind in ("bink", "smk") else md5(body),
                           verify=kind if kind in ("bink", "smk") else "nakedavp",
                           raw_head16=staged[:16].hex()))
    for rel, body in keep.items():
        f = root.joinpath(*rel.split("/"))
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(body)
        rows.append(ap.Row(action="keep", path=rel, size=len(body), original_md5=md5(body), patched_md5="-",
                           reference_md5=md5(body), verify="nakedavp", raw_head16=body[:16].hex()))
    return str(share), root, rows, plain


def test_build_reproduces_every_file_and_resolves_the_menu_font(tmp_path):
    share, _root, rows, plain = _synthetic(tmp_path)
    out = tmp_path / "out"
    m = ap.run_build(str(out), share_root=share, rows=rows, out=io.StringIO())
    assert m["complete"] and not m["failures"]
    assert len(m["outputs"]) == 5
    for o in m["outputs"]:
        rel = o["share_path"][len(ap.TITLE_REL) + 1:]
        assert o["share_path"].startswith("Files/Games-Library/AliensVsPredator/")
        assert open(o["local_path"], "rb").read() == plain[rel]
        assert o["md5"] == md5(plain[rel]) and o["original_md5"] == md5(ap.obfuscate(plain[rel]))
    assert "IntroFont.RIM" in m["menu_asset_check"]


def test_build_refuses_a_changed_input(tmp_path):
    share, root, rows, _plain = _synthetic(tmp_path)
    f = root / "avp_huds" / "alien.rif"
    b = bytearray(f.read_bytes())
    b[40] ^= 0xFF
    f.write_bytes(bytes(b))
    m = ap.run_build(str(tmp_path / "out"), share_root=share, rows=rows, out=io.StringIO())
    assert not m["complete"]
    assert any("alien.rif" in x for x in m["failures"])


def test_build_from_a_part_published_share_takes_patched_files_as_they_are(tmp_path):
    share, root, rows, plain = _synthetic(tmp_path)
    (root / "FMVs" / "logos.bik").write_bytes(plain["FMVs/logos.bik"])
    m = ap.run_build(str(tmp_path / "out"), share_root=share, rows=rows, out=io.StringIO())
    assert m["complete"]
    states = dict((o["share_path"].rsplit("/", 1)[1], o["source_state"]) for o in m["outputs"])
    assert states["logos.bik"] == "patched" and states["alien.rif"] == "original"
    assert all(o["md5"] == md5(plain[o["share_path"][len(ap.TITLE_REL) + 1:]]) for o in m["outputs"])


def test_check_accepts_original_and_patched_and_fails_anything_else(tmp_path):
    share, root, rows, plain = _synthetic(tmp_path)
    assert ap.run_check(share, rows=rows, out=io.StringIO())["ok"]
    (root / "FMVs" / "logos.bik").write_bytes(plain["FMVs/logos.bik"])          # part-way through a publish
    buf = io.StringIO()
    r = ap.run_check(share, rows=rows, out=buf)
    assert r["ok"] and r["counts"]["patched"] == 1 and "part-way" in buf.getvalue()
    (root / "fastfile" / "Snd2.FFL").write_bytes(b"RFFL-changed")                # a file that must stay
    assert not ap.run_check(share, rows=rows, out=io.StringIO())["ok"]


def test_full_scan_flags_an_obfuscated_file_the_table_missed(tmp_path):
    share, root, rows, _plain = _synthetic(tmp_path)
    (root / "avp_rifs").mkdir()
    (root / "avp_rifs" / "missed.rif").write_bytes(ap.obfuscate(b"REBCRIF1" + b"\0" * 60))
    r = ap.run_check(share, rows=rows, full_scan=True, out=io.StringIO())
    assert not r["ok"]
    assert [h[0] for h in r["scan_hits"]] == ["avp_rifs/missed.rif"]


class FakeShare(object):
    """Stands in for sharewrite.py: 'puts' by copying into the fake share root."""

    def __init__(self, share, fail_on_call=None):
        self.share, self.calls, self.fail_on_call = share, [], fail_on_call

    def __call__(self, local, dest):
        self.calls.append(dest)
        if self.fail_on_call is not None and len(self.calls) == self.fail_on_call:
            return 3
        target = os.path.join(self.share, *dest.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(local, target)
        return 0


def test_publish_backs_up_first_flips_ffinfo_last_and_is_idempotent(tmp_path):
    share, root, rows, plain = _synthetic(tmp_path)
    out = str(tmp_path / "out")
    ap.run_build(out, share_root=share, rows=rows, out=io.StringIO())
    fake = FakeShare(share)
    assert ap.run_publish(out, share_root=share, rows=rows, runner=fake, out=io.StringIO()) == 0
    assert len(fake.calls) == 10                                 # 5 backups + 5 puts
    for i in range(0, 10, 2):
        assert fake.calls[i].startswith(ap.BACKUP_REL + "/")
        assert fake.calls[i + 1] == ap.TITLE_REL + "/" + fake.calls[i][len(ap.BACKUP_REL) + 1:]
    assert fake.calls[-1] == ap.TITLE_REL + "/fastfile/ffinfo.txt"
    for rel, body in plain.items():
        assert root.joinpath(*rel.split("/")).read_bytes() == body
        backup = os.path.join(share, *ap.BACKUP_REL.split("/"), *rel.split("/"))
        assert open(backup, "rb").read() == ap.obfuscate(body)
    assert ap.run_check(share, rows=rows, out=io.StringIO())["counts"]["patched"] == 5
    again = FakeShare(share)
    assert ap.run_publish(out, share_root=share, rows=rows, runner=again, out=io.StringIO()) == 0
    assert again.calls == []                                     # nothing left to do


def test_publish_stops_at_the_first_failure(tmp_path):
    share, _root, rows, _plain = _synthetic(tmp_path)
    out = str(tmp_path / "out")
    ap.run_build(out, share_root=share, rows=rows, out=io.StringIO())
    fake = FakeShare(share, fail_on_call=2)                      # the first real put fails
    assert ap.run_publish(out, share_root=share, rows=rows, runner=fake, out=io.StringIO()) == 3
    assert len(fake.calls) == 2


def test_publish_never_overwrites_a_different_backup(tmp_path):
    share, _root, rows, _plain = _synthetic(tmp_path)
    out = str(tmp_path / "out")
    ap.run_build(out, share_root=share, rows=rows, out=io.StringIO())
    first = ap.publish_order([r for r in rows if r.action == "deobfuscate"])[0]
    bpath = os.path.join(share, *ap.BACKUP_REL.split("/"), *first.path.split("/"))
    os.makedirs(os.path.dirname(bpath), exist_ok=True)
    open(bpath, "wb").write(b"someone else's backup")
    fake = FakeShare(share)
    assert ap.run_publish(out, share_root=share, rows=rows, runner=fake, out=io.StringIO()) == 3
    assert fake.calls == []


@pytest.mark.parametrize("damage", ["garbled", "missing"])
def test_publish_stops_on_a_share_copy_it_cannot_account_for(tmp_path, damage):
    """A failed put can leave a truncated or absent file (gvfs has done both here).

    A re-run must STOP on it and write nothing - not back up the damaged bytes
    as the 'original', and not overwrite it blind. README.md says how to recover.
    """
    share, root, rows, _plain = _synthetic(tmp_path)
    out = str(tmp_path / "out")
    ap.run_build(out, share_root=share, rows=rows, out=io.StringIO())
    first = ap.publish_order([r for r in rows if r.action == "deobfuscate"])[0]
    target = root.joinpath(*first.path.split("/"))
    if damage == "garbled":
        target.write_bytes(b"half-written by a failed put")
    else:
        target.unlink()
    fake = FakeShare(share)
    buf = io.StringIO()
    assert ap.run_publish(out, share_root=share, rows=rows, runner=fake, out=buf) == 3
    assert fake.calls == [] and "STOP %s" % first.path in buf.getvalue()


def test_publish_refuses_an_incomplete_build(tmp_path):
    share, root, rows, _plain = _synthetic(tmp_path)
    (root / "avp_huds" / "alien.rif").write_bytes(b"tampered")
    out = str(tmp_path / "out")
    ap.run_build(out, share_root=share, rows=rows, out=io.StringIO())
    fake = FakeShare(share)
    assert ap.run_publish(out, share_root=share, rows=rows, runner=fake, out=io.StringIO()) == 2
    assert fake.calls == []


def test_dry_run_writes_nothing(tmp_path):
    share, _root, rows, _plain = _synthetic(tmp_path)
    out = str(tmp_path / "out")
    ap.run_build(out, share_root=share, rows=rows, out=io.StringIO())
    fake = FakeShare(share)
    buf = io.StringIO()
    assert ap.run_publish(out, share_root=share, rows=rows, runner=fake, dry_run=True, out=buf) == 0
    assert fake.calls == [] and "would put" in buf.getvalue()


def test_install_server_is_not_applicable(capsys):
    assert ap.main(["--install-server"]) == 0
    assert "NOT APPLICABLE" in capsys.readouterr().out


# --------------------------------------------------------------------------
# The real share - SKIPS LOUDLY when the library is not mounted.
# --------------------------------------------------------------------------

def _need_share():
    if not os.path.isdir(os.path.join(LIB, ap.TITLE)):
        pytest.skip("SKIPPED LOUDLY: %s/%s is not mounted - the staged AvP inputs were NOT verified"
                    % (LIB, ap.TITLE))


def test_share_small_inputs_are_the_pinned_originals_or_already_patched(table):
    """Every table row up to 256 KiB (65 of 210 files, ~8 MB), read from the share.

    Accepts both states, so this stays green before and after the publish -
    and across a publish that stopped part-way. The whole table is
    `apply.py --check` (171 MB), too slow for the suite.
    """
    _need_share()
    rows = [r for r in table if r.size <= 256 * 1024]
    assert len(rows) >= 60 and any(r.action == "keep" for r in rows)
    r = ap.run_check(ap.SHARE_MNT, rows=rows, out=io.StringIO())
    assert r["ok"], r["problems"]


def test_share_menu_font_resolves_after_the_transform(table, tmp_path):
    """The asset every LCD box logged as missing is in the built Menus fast-file."""
    _need_share()
    root = ap.title_root(ap.SHARE_MNT)
    rows = dict((r.path, r) for r in table)
    for rel in ("fastfile/ffinfo.txt", "fastfile/Tex37.FFL"):
        data = open(os.path.join(root, *rel.split("/")), "rb").read()
        state, why = ap.file_state(rows[rel], data)
        assert state in (ap.ORIGINAL, ap.PATCHED), why
        plain = ap.deobfuscate(data) if state == ap.ORIGINAL else data
        assert md5(plain) == rows[rel].reference_md5
        dst = tmp_path.joinpath(*rel.split("/"))
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(plain)
    assert "graphics\\Menus\\IntroFont.RIM" in ap.menu_asset_check(str(tmp_path), root)
