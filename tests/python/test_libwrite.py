"""scripts/fleet/libwrite.py - which backend a library generator's writes use,
and where each write lands. No network: sharewrite's put/rm are injected.

Why it matters: the headless dev host only has /mnt/retro-share, which is
mounted READ-ONLY. A generator that writes there with open() fails; one that
batches its writes and checks at the end has already lost files by the time it
looks (CLAUDE.md "WRITING TO THE SHARE"). So every write must go through ONE
verified smbclient put, and the first failure must end the run.
"""
import importlib.util
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
FLEET = os.path.join(REPO, "scripts", "fleet")


def _load(name, fn):
    spec = importlib.util.spec_from_file_location(name, os.path.join(FLEET, fn))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


lw = _load("libwrite", "libwrite.py")
LIB_MNT = "/mnt/retro-share/Files/Games-Library"
LIB_GVFS = lw.GVFS + "/Files/Games-Library"


# --- backend selection ------------------------------------------------------

def test_the_read_only_mnt_library_publishes_over_smb():
    assert lw.backend_for(LIB_MNT) == "smb"
    assert lw.backend_for(LIB_MNT + "/Quake3/") == "smb"
    assert lw.backend_for("/mnt/retro-share") == "smb"


def test_gvfs_writes_locally_unless_smb_is_forced():
    assert lw.backend_for(LIB_GVFS) == "local"
    assert lw.backend_for(LIB_GVFS, via_smb=True) == "smb"


def test_a_local_tree_writes_locally(tmp_path):
    assert lw.backend_for(str(tmp_path)) == "local"
    assert lw.writer_for(str(tmp_path)).kind == "local"


def test_via_smb_on_a_path_not_on_the_share_is_an_error_not_a_local_write(tmp_path):
    with pytest.raises(lw.LibWriteError):
        lw.backend_for(str(tmp_path), via_smb=True)


def test_a_sibling_with_the_same_prefix_is_not_the_share():
    # '/mnt/retro-share-old' starts with '/mnt/retro-share' as a STRING
    assert lw.share_rel("/mnt/retro-share-old/Files/x") is None
    assert lw.backend_for("/mnt/retro-share-old/Files") == "local"


# --- path mapping -------------------------------------------------------------

def test_mnt_and_gvfs_paths_map_to_the_same_share_path():
    a = lw.share_rel(LIB_MNT + "/Quake3/Play Quake III.bat")
    b = lw.share_rel(LIB_GVFS + "/Quake3/Play Quake III.bat")
    assert a == b == "Files/Games-Library/Quake3/Play Quake III.bat"
    assert lw.mnt_path(LIB_GVFS + "/Quake3/x.cfg") == LIB_MNT + "/Quake3/x.cfg"
    assert lw.share_rel("/home/x/y") is None
    assert lw.share_rel(LIB_MNT + "/A/../B/c.txt") == "Files/Games-Library/B/c.txt"


# --- the SMB writer, with sharewrite faked ---------------------------------------

class FakeAuth:
    entered = exited = 0

    def __enter__(self):
        FakeAuth.entered += 1
        return "/fake/auth"

    def __exit__(self, *a):
        FakeAuth.exited += 1


def _smbwriter(calls, fail_at=None):
    def put(local, rel, auth, log=print):
        with open(local, "rb") as fh:
            calls.append(("put", rel, fh.read(), auth))
        return 3 if fail_at is not None and len(calls) == fail_at else 0

    def rm(rel, auth, log=print):
        calls.append(("rm", rel, None, auth))
        return 0

    FakeAuth.entered = FakeAuth.exited = 0
    return lw.SmbWriter(log=lambda m: None, put=put, rm=rm, auth=FakeAuth)


def test_each_write_is_one_put_of_exactly_those_bytes_to_the_mapped_path(tmp_path):
    calls = []
    w = _smbwriter(calls)
    src = tmp_path / "FLEETRES.EXE"
    src.write_bytes(b"MZ\x90\x00payload")
    with w:
        w.write_bytes(LIB_MNT + "/Quake3/FLEETRES.BAT", b"@echo off\r\n")
        w.copy_file(str(src), LIB_MNT + "/Quake3/FLEETRES.EXE")
        w.remove(LIB_MNT + "/Quake3/stale.cfg")
    assert calls == [
        ("put", "Files/Games-Library/Quake3/FLEETRES.BAT", b"@echo off\r\n", "/fake/auth"),
        ("put", "Files/Games-Library/Quake3/FLEETRES.EXE", b"MZ\x90\x00payload", "/fake/auth"),
        ("rm", "Files/Games-Library/Quake3/stale.cfg", None, "/fake/auth"),
    ]
    # the vault is read once per run, and the auth file is removed at the end
    assert FakeAuth.entered == 1 and FakeAuth.exited == 1
    assert w.written == ["Files/Games-Library/Quake3/FLEETRES.BAT",
                         "Files/Games-Library/Quake3/FLEETRES.EXE"]
    assert not os.path.exists(w._tmp), "the temp staging dir must be cleaned up"


def test_the_first_failed_file_stops_the_run(tmp_path):
    calls = []
    w = _smbwriter(calls, fail_at=2)
    with w:
        w.write_bytes(LIB_MNT + "/T/a.bat", b"a")
        with pytest.raises(lw.LibWriteError) as e:
            w.write_bytes(LIB_MNT + "/T/b.bat", b"b")
    assert "Files/Games-Library/T/b.bat" in str(e.value)
    assert "1 file(s) published before it" in str(e.value)
    assert w.written == ["Files/Games-Library/T/a.bat"]


def test_check_and_dry_run_never_reach_the_vault():
    calls = []
    w = _smbwriter(calls)
    w.close()
    assert FakeAuth.entered == 0 and calls == []


def test_a_path_off_the_share_is_refused_before_anything_is_written(tmp_path):
    calls = []
    w = _smbwriter(calls)
    with w, pytest.raises(lw.LibWriteError):
        w.write_bytes(str(tmp_path / "x.bat"), b"x")
    assert calls == [] and FakeAuth.entered == 0


def test_local_writer_writes_and_removes(tmp_path):
    w = lw.LocalWriter(log=lambda m: None)
    p = tmp_path / "T" / "a.bat"
    w.write_bytes(str(p), b"abc")
    assert p.read_bytes() == b"abc"
    w.remove(str(p))
    assert not p.exists()


# --- the generators use it -------------------------------------------------------

GENERATORS = ("stage-fleetres.py", "make-mount-launcher.py",
              "stage-serioussam.py", "stage-dosnative.py")


@pytest.mark.parametrize("fn", GENERATORS)
def test_every_library_generator_writes_through_libwrite(fn):
    src = open(os.path.join(FLEET, fn), encoding="utf-8").read()
    assert "import libwrite" in src and "libwrite.add_arguments(ap)" in src, fn
    # no direct write path left beside it: open(..., 'wb'/'w'), copyfile, remove
    code = "\n".join(l for l in src.splitlines() if not l.lstrip().startswith("#"))
    for pat in (r"open\([^)]*['\"]wb?['\"]", r"shutil\.copy", r"os\.remove\(",
                r"os\.unlink\("):
        assert not re.search(pat, code), "%s still writes directly: %s" % (fn, pat)


def test_stage_fleetres_runner_picks_the_backend_from_the_library(tmp_path):
    sf = _load("stage_fleetres", "stage-fleetres.py")
    assert sf.Runner(str(tmp_path), dry=False, check=False).writer.kind == "local"
    assert sf.Runner(LIB_MNT, dry=False, check=True).writer.kind == "smb"


def test_stage_fleetres_writes_only_through_its_writer(tmp_path):
    sf = _load("stage_fleetres", "stage-fleetres.py")

    class Rec:
        kind = "rec"

        def __init__(self):
            self.got = []

        def write_bytes(self, path, data):
            self.got.append((path, data))

        def copy_file(self, src, dst):
            self.got.append((dst, open(src, "rb").read()))

    rec = Rec()
    r = sf.Runner(str(tmp_path), dry=False, check=False, writer=rec)
    tdir = tmp_path / "T"
    tdir.mkdir()
    r.payload(str(tdir), "T")
    names = sorted(os.path.basename(p) for p, _ in rec.got)
    assert names == ["FLEETRES.BAT", "FLEETRES.EXE"]
    assert not any(tdir.iterdir()), "nothing may be written behind the writer's back"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
