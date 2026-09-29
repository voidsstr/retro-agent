"""ue1 patch: Unreal Engine 1 video menus must be able to list 1920x1080.

provisioning/patches/ue1/apply.py lifts the D3DDrv GetRes 16-entry cut
(83 FF 10 -> 83 FF 7F) in Unreal Gold 226b, Deus Ex 1112fm and (optionally)
UT99 436, and Deus Ex's own 16-token script cut in DEUSEX.U (IntConstByte
0x10 -> 0x28). On .195 (20 modes) and .240 (24 modes) the 16-entry cut hid
1920x1080 from the menu, and on Deus Ex, Esc/Enter/OK on Settings > Display
then SetRes'd the box down to entry 0 (640x480 / 640x400).

Pure logic runs without the share (synthetic PE and UE1 package). The checks
against the real staged files SKIP LOUDLY when the library is not mounted -
a silent skip would let a changed staged file go unnoticed.
"""
import hashlib
import importlib.util
import json
import os
import struct
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
APPLY = os.path.join(REPO, "provisioning", "patches", "ue1", "apply.py")
LIB = "/mnt/retro-share/Files/Games-Library"
DX_SERVER_U = "/home/voidsstr/deusex-server/System/DEUSEX.U"


def load():
    spec = importlib.util.spec_from_file_location("ue1_apply", APPLY)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


A = load()


# ---------------------------------------------------------------- the table
def by_rel(rel):
    return [p for p in A.PATCHES if p["rel"] == rel][0]


def test_table_names_exactly_the_four_files_and_offsets():
    got = {p["rel"]: [(e["offset"], e["before"], e["after"]) for e in p["edits"]] for p in A.PATCHES}
    assert got == {
        "UnrealGold/System/D3DDrv.dll": [(0x64F5, "83ff10", "83ff7f")],
        "DeusEx/SYSTEM/D3DDRV.DLL": [(0x62CE, "83ff10", "83ff7f")],
        "DeusEx/SYSTEM/DEUSEX.U": [(0x4607CE, "10", "28")],
        "UnrealTournament436/System/D3DDrv.dll": [(0x659F, "83ff10", "83ff7f")],
    }


def test_originals_recorded_as_measured_2026_09_29():
    assert by_rel("UnrealGold/System/D3DDrv.dll")["md5"] == "f924e209e7bc23b90b6f0360174683d3"
    assert by_rel("DeusEx/SYSTEM/D3DDRV.DLL")["md5"] == "6179c83ce1d7516908e5442ebfb4d154"
    assert by_rel("DeusEx/SYSTEM/DEUSEX.U")["md5"] == "d343da03a6d311ee412dfae4b52ff975"
    assert by_rel("UnrealTournament436/System/D3DDrv.dll")["md5"] == "dd6e3692f8ead5e1df88716024bc25d1"
    assert by_rel("DeusEx/SYSTEM/DEUSEX.U")["size"] == 5386972


def test_every_file_has_a_distinct_recorded_patched_md5():
    for p in A.PATCHES:
        assert p["patched_md5"] and len(p["patched_md5"]) == 32
        assert p["patched_md5"] != p["md5"]


def test_required_flags_ut436_is_the_only_optional_edit():
    assert {p["rel"] for p in A.PATCHES if not p["required"]} == {"UnrealTournament436/System/D3DDrv.dll"}


def test_new_caps_old_16_new_127_and_40():
    # cmp edi,imm8 is sign-extended: 0x7f is the largest one-byte cap, and
    # it must still be a positive compare (the old value was 16).
    for p in A.PATCHES:
        for e in p["edits"]:
            if p["kind"] == "pe":
                assert bytes.fromhex(e["before"]) == b"\x83\xff\x10"   # cmp edi,16
                assert bytes.fromhex(e["after"]) == b"\x83\xff\x7f"    # cmp edi,127
                assert struct.unpack("b", bytes.fromhex(e["after"])[2:])[0] == 127
            else:
                assert int(e["before"], 16) == 16      # ArrayCount(Resolutions) (unused local)
                assert int(e["after"], 16) == 40       # ArrayCount(enumText): no write past [39]


def test_old_value_would_cut_1080p_on_195_and_240_new_does_not():
    """The measured distinct 16-bpp mode counts (DirectDraw probe, 1080p last)."""
    counts = {".123": 15, ".145": 16, ".195": 20, ".240": 24}
    for box, n in counts.items():
        listed_old = min(n, 16) >= n           # 1920x1080 is entry n (last)
        listed_new = min(n, 127) >= n
        assert listed_new, box
        assert listed_old == (box in (".123", ".145")), box


# ---------------------------------------------------------------- transforms
def test_apply_edits_changes_only_the_edit_bytes_and_keeps_length():
    data = bytes(range(256)) * 4
    edits = [{"offset": 10, "before": data[10:13].hex(), "after": "aabbcc"}]
    out = A.apply_edits(data, edits)
    assert len(out) == len(data)
    assert out[10:13] == b"\xaa\xbb\xcc"
    assert A.diff_offsets(data, out) == [10, 11, 12]


def test_apply_edits_refuses_unexpected_bytes():
    with pytest.raises(A.PatchError):
        A.apply_edits(b"\x00" * 16, [{"offset": 4, "before": "83ff10", "after": "83ff7f"}])


def test_apply_edits_refuses_a_length_change():
    with pytest.raises(A.PatchError):
        A.apply_edits(b"\x83\xff\x10\x00", [{"offset": 0, "before": "83ff10", "after": "83ff"}])


def test_edit_state_original_patched_unknown():
    e = [{"offset": 2, "before": "83ff10", "after": "83ff7f"}]
    assert A.edit_state(b"\0\0\x83\xff\x10", e) == "original"
    assert A.edit_state(b"\0\0\x83\xff\x7f", e) == "patched"
    assert A.edit_state(b"\0\0\x83\xff\x20", e) == "unknown"


def test_compact_index_roundtrip_and_the_bytecode_local():
    for v in (0, 1, 63, 64, 6091, 21422, 12872, 5386972, -1, -6091):
        enc = A.write_compact(v)
        assert A.read_compact(enc, 0) == (v, len(enc))
    # the loop test's LocalVariable operand '4b 5f' is object ref 6091
    assert A.read_compact(bytes.fromhex("4b5f"), 0) == (6091, 2)


def test_resolve_ci_finds_a_windows_path_in_any_case(tmp_path):
    (tmp_path / "DeusEx" / "SYSTEM").mkdir(parents=True)
    (tmp_path / "DeusEx" / "SYSTEM" / "D3DDRV.DLL").write_bytes(b"x")
    assert A.resolve_ci(str(tmp_path), "deusex/System/d3ddrv.dll").endswith("SYSTEM/D3DDRV.DLL")
    with pytest.raises(A.PatchError):
        A.resolve_ci(str(tmp_path), "DeusEx/SYSTEM/nope.dll")


# ---------------------------------------------------------------- synthetic PE
def make_pe(body_at_raw, raw_text=0x400, va_text=0x1000, size=0x1000, checksum=0,
            text_chars=0x60000020, base=0x10000000):
    b = bytearray(raw_text + size)
    b[0:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", b, 0x3C, pe)
    b[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HH", b, pe + 4, 0x14C, 1)          # machine, 1 section
    struct.pack_into("<H", b, pe + 20, 0xE0)              # optional header size
    opt = pe + 24
    struct.pack_into("<H", b, opt, 0x10B)
    struct.pack_into("<I", b, opt + 28, base)
    struct.pack_into("<I", b, opt + 64, checksum)
    sec = opt + 0xE0
    b[sec:sec + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", b, sec + 8, size, va_text, size, raw_text)
    struct.pack_into("<I", b, sec + 36, text_chars)
    for off, data in body_at_raw:
        b[off:off + len(data)] = data
    return bytes(b)


def pe_spec(off, va):
    return {"rel": "T/System/D3DDrv.dll", "title": "T", "kind": "pe", "required": True,
            "edits": [{"offset": off, "before": "83ff10", "after": "83ff7f",
                       "context": A.CAP_CONTEXT.hex(), "va": va, "what": "test"}]}


def test_pe_structure_accepts_original_and_patched():
    off = 0x400 + 0x4F5
    data = make_pe([(off - 8, A.CAP_CONTEXT)])
    spec = pe_spec(off, 0x10001000 + 0x4F5)
    rep = []
    A.verify_structure(spec, data, rep)
    assert any(".text" in r for r in rep)
    A.verify_structure(spec, A.apply_edits(data, spec["edits"]), [])


def test_pe_structure_refuses_a_checksummed_or_non_code_target():
    off = 0x400 + 0x4F5
    spec = pe_spec(off, 0x10001000 + 0x4F5)
    with pytest.raises(A.PatchError):
        A.verify_structure(spec, make_pe([(off - 8, A.CAP_CONTEXT)], checksum=0x1234), [])
    with pytest.raises(A.PatchError):
        A.verify_structure(spec, make_pe([(off - 8, A.CAP_CONTEXT)], text_chars=0x40000040), [])


def test_pe_structure_refuses_wrong_context():
    off = 0x400 + 0x4F5
    ctx = bytearray(A.CAP_CONTEXT)
    ctx[11] = 0x75  # jne instead of jge after the cmp
    with pytest.raises(A.PatchError):
        A.verify_structure(pe_spec(off, 0x10001000 + 0x4F5), make_pe([(off - 8, bytes(ctx))]), [])


# ---------------------------------------------------------------- synthetic UE1 package
PATTERN = bytes.fromhex("074b0196004b5f2c1016")
GUID = "6bcadecaffb7b34e92b50c1f8eccec21"


def make_pkg(pattern_count=1, guid=GUID):
    names = ["None", "MenuChoice_Resolution", "GetScreenResolutions", "Function", "Class"]
    fn_body = b"\x0b" * 20 + PATTERN + b"\x0b" * 10 + (PATTERN if pattern_count > 1 else b"") + b"\x53"
    hdr_len = 56 + 8
    name_tab = b"".join(A.write_compact(len(n) + 1) + n.encode() + b"\0" + struct.pack("<I", 0) for n in names)
    no = hdr_len
    body_off = no + len(name_tab)
    cls_body = b"\x00" * 8
    fn_off = body_off + len(cls_body)
    eo = fn_off + len(fn_body)
    exports = (A.write_compact(0) + A.write_compact(0) + struct.pack("<i", 0) + A.write_compact(1)
               + struct.pack("<I", 0) + A.write_compact(len(cls_body)) + A.write_compact(body_off)
               + A.write_compact(0) + A.write_compact(0) + struct.pack("<i", 1) + A.write_compact(2)
               + struct.pack("<I", 0) + A.write_compact(len(fn_body)) + A.write_compact(fn_off))
    io = eo + len(exports)
    hdr = struct.pack("<IHHIIIIIII", A.UE_TAG, 68, 0, 1, len(names), no, 2, eo, 0, io)
    hdr += bytes.fromhex(guid) + struct.pack("<I", 1) + struct.pack("<II", 2, len(names))
    data = hdr + name_tab + cls_body + fn_body + exports
    return data, fn_off + 20, len(names)


def pkg_spec(pat_off, nnames, guid=GUID):
    return {"rel": "DeusEx/SYSTEM/DEUSEX.U", "title": "DeusEx", "kind": "ue1pkg", "required": True,
            "guid": guid, "generations": [[2, nnames]],
            "function": ("MenuChoice_Resolution", "GetScreenResolutions"),
            "edits": [{"offset": pat_off + 8, "before": "10", "after": "28",
                       "pattern": PATTERN.hex(), "pattern_offset": pat_off, "what": "test"}]}


def test_package_parser_finds_the_function_export():
    data, pat_off, n = make_pkg()
    idx, ex = A.ue_find_export(data, "MenuChoice_Resolution", "GetScreenResolutions")
    assert idx == 1 and ex["serial_offset"] <= pat_off < ex["serial_offset"] + ex["serial_size"]
    assert A.ue_header(data)["guid"] == GUID


def test_package_edit_keeps_guid_generations_and_size():
    data, pat_off, n = make_pkg()
    spec = pkg_spec(pat_off, n)
    A.verify_structure(spec, data, [])
    out = A.apply_edits(data, spec["edits"])
    A.verify_structure(spec, out, [])
    assert len(out) == len(data)
    assert A.ue_header(out) == A.ue_header(data)
    assert A.diff_offsets(data, out) == [pat_off + 8]
    assert out[pat_off:pat_off + 10] == bytes.fromhex("074b0196004b5f2c2816")


def test_package_refuses_an_ambiguous_loop_pattern():
    data, pat_off, n = make_pkg(pattern_count=2)
    with pytest.raises(A.PatchError):
        A.verify_structure(pkg_spec(pat_off, n), data, [])


def test_package_refuses_a_different_guid():
    data, pat_off, n = make_pkg(guid="00" * 16)
    with pytest.raises(A.PatchError):
        A.verify_structure(pkg_spec(pat_off, n), data, [])


# ---------------------------------------------------------------- build + publish (dry run), synthetic library
def test_build_then_publish_dry_run_on_a_synthetic_library(tmp_path, monkeypatch, capfd):
    mnt = tmp_path / "mnt"
    lib = mnt / "Files" / "Games-Library"
    off = 0x400 + 0x4F5
    pe = make_pe([(off - 8, A.CAP_CONTEXT)])
    pkg, pat_off, n = make_pkg()
    (lib / "T" / "System").mkdir(parents=True)
    (lib / "DeusEx" / "SYSTEM").mkdir(parents=True)
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(pe)
    (lib / "DeusEx" / "SYSTEM" / "DEUSEX.U").write_bytes(pkg)
    s1 = pe_spec(off, 0x10001000 + 0x4F5)
    s1.update(size=len(pe), md5=hashlib.md5(pe).hexdigest(),
              patched_md5=hashlib.md5(A.apply_edits(pe, s1["edits"])).hexdigest())
    s2 = pkg_spec(pat_off, n)
    s2.update(size=len(pkg), md5=hashlib.md5(pkg).hexdigest(),
              patched_md5=hashlib.md5(A.apply_edits(pkg, s2["edits"])).hexdigest())
    monkeypatch.setattr(A, "PATCHES", [s1, s2])
    monkeypatch.setattr(A, "MNT", str(mnt))
    out = tmp_path / "out"
    assert A.main(["--check"]) == 0
    assert A.main(["--build", str(out)]) == 0
    man = json.load(open(out / "manifest.json"))
    assert [o["share_path"] for o in man["outputs"]] == [
        "Files/Games-Library/T/System/D3DDrv.dll", "Files/Games-Library/DeusEx/SYSTEM/DEUSEX.U"]
    for o in man["outputs"]:
        assert hashlib.md5(open(o["local_path"], "rb").read()).hexdigest() == o["md5"]
        assert o["backup_share_path"].startswith("Files/Games-Library/_patches/")
        assert "/originals-2026-09-29/" in o["backup_share_path"]
    capfd.readouterr()
    assert A.main(["--publish", str(out), "--dry-run"]) == 0
    printed = capfd.readouterr().out
    # backup is put BEFORE the patched file, for each file
    first = printed.index("_patches/T/originals-2026-09-29/System/D3DDrv.dll")
    second = printed.index("put %s Files/Games-Library/T/System/D3DDrv.dll" % (out / "patched" / "T" / "System" / "D3DDrv.dll"))
    assert first < second
    assert "dry-run: nothing written" in printed
    # a staged file already carrying the patched bytes is skipped (idempotent)
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(A.apply_edits(pe, s1["edits"]))
    capfd.readouterr()
    assert A.main(["--publish", str(out), "--dry-run"]) == 0
    assert "already patched on the share - skip" in capfd.readouterr().out
    # an unknown staged file stops the publish before anything is written
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(pe[:-1] + b"\x01")
    assert A.main(["--publish", str(out), "--dry-run"]) == 3


def test_install_server_changes_nothing(tmp_path, monkeypatch, capsys):
    """Read-only: it installs nothing, and it checks the server package identity
    the patched clients will present (absent -> says NOT VERIFIED, rc 0)."""
    monkeypatch.setattr(A, "DX_SERVER_ROOT", str(tmp_path / "no-server-here"))
    assert A.main(["--install-server"]) == 0
    assert "NOT VERIFIED" in capsys.readouterr().out
    assert os.listdir(tmp_path) == []          # nothing was created


# ---------------------------------------------------------------- review additions (2026-09-29)
def synth_library(tmp_path, monkeypatch):
    """A two-file synthetic library (one PE, one UE1 package) wired into A."""
    mnt = tmp_path / "mnt"
    lib = mnt / "Files" / "Games-Library"
    off = 0x400 + 0x4F5
    pe = make_pe([(off - 8, A.CAP_CONTEXT)])
    pkg, pat_off, n = make_pkg()
    (lib / "T" / "System").mkdir(parents=True)
    (lib / "DeusEx" / "SYSTEM").mkdir(parents=True)
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(pe)
    (lib / "DeusEx" / "SYSTEM" / "DEUSEX.U").write_bytes(pkg)
    s1 = pe_spec(off, 0x10001000 + 0x4F5)
    s1.update(size=len(pe), md5=hashlib.md5(pe).hexdigest(),
              patched_md5=hashlib.md5(A.apply_edits(pe, s1["edits"])).hexdigest())
    s2 = pkg_spec(pat_off, n)
    s2.update(size=len(pkg), md5=hashlib.md5(pkg).hexdigest(),
              patched_md5=hashlib.md5(A.apply_edits(pkg, s2["edits"])).hexdigest())
    monkeypatch.setattr(A, "PATCHES", [s1, s2])
    monkeypatch.setattr(A, "MNT", str(mnt))
    return lib, pe, pkg, s1, s2


def tree_md5s(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            if f != "manifest.json" and not f.startswith("PATCH-NOTES"):   # both carry a build time
                p = os.path.join(d, f)
                out[os.path.relpath(p, root)] = hashlib.md5(open(p, "rb").read()).hexdigest()
    return out


def test_revert_edits_is_the_exact_inverse_of_apply_edits():
    data = b"\x00" * 6 + b"\x83\xff\x10" + b"\x00" * 7
    e = [{"offset": 6, "before": "83ff10", "after": "83ff7f"}]
    patched = A.apply_edits(data, e)
    assert A.revert_edits(patched, e) == data
    with pytest.raises(A.PatchError):          # an ORIGINAL cannot be "reverted"
        A.revert_edits(data, e)


def test_build_and_check_still_work_after_publish(tmp_path, monkeypatch):
    """Once --publish has run, the share holds the PATCHED files. --build must
    still reproduce byte-identical outputs (the original is rebuilt from the
    published file and checked against the recorded md5), and --check passes."""
    lib, pe, pkg, s1, s2 = synth_library(tmp_path, monkeypatch)
    assert A.main(["--build", str(tmp_path / "before")]) == 0
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(A.apply_edits(pe, s1["edits"]))
    (lib / "DeusEx" / "SYSTEM" / "DEUSEX.U").write_bytes(A.apply_edits(pkg, s2["edits"]))
    assert A.main(["--check"]) == 0
    assert A.main(["--build", str(tmp_path / "after")]) == 0
    assert tree_md5s(tmp_path / "before") == tree_md5s(tmp_path / "after")
    # a staged file that is neither original nor patched is still refused
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(pe[:-1] + b"\x01")
    with pytest.raises(SystemExit):
        A.main(["--build", str(tmp_path / "bad")])


def test_build_refuses_an_outdir_inside_a_git_work_tree(tmp_path, monkeypatch):
    """Patched game binaries are copyrighted: never write them where git can add them."""
    synth_library(tmp_path, monkeypatch)
    repo = tmp_path / "repo"
    repo.mkdir()
    r = subprocess.run(["git", "init", "-q", str(repo)], capture_output=True)
    if r.returncode != 0:
        pytest.skip("SKIPPED LOUDLY: git is unavailable - the git-work-tree guard was NOT tested")
    with pytest.raises(SystemExit) as ex:
        A.main(["--build", str(repo / "patch-out")])
    assert "git work tree" in str(ex.value)
    assert not (repo / "patch-out").exists()
    assert A.git_toplevel(str(tmp_path / "outside")) is None


def test_publish_refuses_a_manifest_that_lacks_a_selected_file(tmp_path, monkeypatch, capfd):
    """A narrower --build must not turn into a silent partial publish."""
    synth_library(tmp_path, monkeypatch)
    out = tmp_path / "out"
    assert A.main(["--build", str(out), "--titles", "T"]) == 0      # DeusEx not built
    capfd.readouterr()
    assert A.main(["--publish", str(out), "--dry-run"]) == 2
    printed = capfd.readouterr().out
    assert "has no build for Files/Games-Library/DeusEx/SYSTEM/DEUSEX.U" in printed
    assert "sharewrite.py put" not in printed                       # nothing was attempted
    assert A.main(["--publish", str(out), "--dry-run", "--titles", "T"]) == 0


def test_publish_backs_up_the_original_even_when_the_share_is_already_patched(tmp_path, monkeypatch, capfd):
    lib, pe, pkg, s1, s2 = synth_library(tmp_path, monkeypatch)
    out = tmp_path / "out"
    assert A.main(["--build", str(out)]) == 0
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(A.apply_edits(pe, s1["edits"]))
    capfd.readouterr()
    assert A.main(["--publish", str(out), "--dry-run", "--titles", "T"]) == 0
    printed = capfd.readouterr().out
    backup = printed.index("Files/Games-Library/_patches/T/originals-2026-09-29/System/D3DDrv.dll")
    skip = printed.index("already patched on the share - skip")
    assert backup < skip
    assert "put %s Files/Games-Library/T/System/D3DDrv.dll" % (out / "patched" / "T" / "System" / "D3DDrv.dll") \
        not in printed                                               # the patched file is not re-put


def test_publish_stops_on_an_unknown_share_copy_before_any_write_and_says_how_to_recover(tmp_path, monkeypatch, capfd):
    lib, pe, pkg, s1, s2 = synth_library(tmp_path, monkeypatch)
    out = tmp_path / "out"
    assert A.main(["--build", str(out)]) == 0
    (lib / "T" / "System" / "D3DDrv.dll").write_bytes(pe[:100])     # e.g. a truncated failed put
    capfd.readouterr()
    assert A.main(["--publish", str(out), "--dry-run"]) == 3
    printed = capfd.readouterr().out
    assert "sharewrite.py put" not in printed
    assert "the verified original is %s" % (out / "originals" / "T" / "System" / "D3DDrv.dll") in printed


def test_install_server_compares_the_server_package_identity(tmp_path, monkeypatch, capsys):
    lib, pe, pkg, s1, s2 = synth_library(tmp_path, monkeypatch)
    srv = tmp_path / "srv"
    (srv / "SYSTEM").mkdir(parents=True)                             # case differs on purpose
    (srv / "SYSTEM" / "DeusEx.u").write_bytes(pkg)
    monkeypatch.setattr(A, "DX_SERVER_ROOT", str(srv))
    assert A.main(["--install-server"]) == 0
    assert "the recorded ORIGINAL" in capsys.readouterr().out
    other, _, _ = make_pkg(guid="11" * 16)                             # a different package identity
    (srv / "SYSTEM" / "DeusEx.u").write_bytes(other)
    assert A.main(["--install-server"]) == 1
    assert "version mismatch" in capsys.readouterr().out
    assert (srv / "SYSTEM" / "DeusEx.u").read_bytes() == other        # read-only: untouched


# ---------------------------------------------------------------- the real staged files
share = pytest.mark.skipif(not os.path.isdir(LIB),
                           reason="SKIPPED LOUDLY: %s is not mounted - the staged UE1 files "
                                  "were NOT verified" % LIB)


@share
def test_staged_files_are_the_recorded_originals_or_already_patched():
    for p in A.PATCHES:
        path = A.resolve_ci(LIB, p["rel"])
        data = open(path, "rb").read()
        state = A.classify(p, data)
        assert state in ("original", "patched"), (p["rel"], hashlib.md5(data).hexdigest())
        A.verify_structure(p, data, [])
        if state == "original":
            assert hashlib.md5(A.apply_edits(data, p["edits"])).hexdigest() == p["patched_md5"]


@share
def test_check_passes_on_the_share(capsys):
    assert A.main(["--check"]) == 0
    assert "all files verified" in capsys.readouterr().out


@share
def test_patched_deusex_u_matches_the_fleet_servers_package_identity():
    """Joining deusex-server compares package GUID + generation; the edit keeps both."""
    p = by_rel("DeusEx/SYSTEM/DEUSEX.U")
    data = open(A.resolve_ci(LIB, p["rel"]), "rb").read()
    if A.classify(p, data) == "original":
        data = A.apply_edits(data, p["edits"])
    h = A.ue_header(data)
    assert h["guid"] == p["guid"] and h["generations"] == p["generations"]
    if not os.path.isfile(DX_SERVER_U):
        pytest.skip("SKIPPED LOUDLY: %s absent - server identity NOT compared" % DX_SERVER_U)
    srv = A.ue_header(open(DX_SERVER_U, "rb").read())
    assert (srv["guid"], srv["generations"], srv["name_count"], srv["export_count"], srv["import_count"]) == \
           (h["guid"], h["generations"], h["name_count"], h["export_count"], h["import_count"])
