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


# --- the SMB writer, with the NAS faked -------------------------------------------

class FakeAuth:
    entered = exited = 0

    def __enter__(self):
        FakeAuth.entered += 1
        return "/fake/auth"

    def __exit__(self, *a):
        FakeAuth.exited += 1


class FakeShare:
    """The NAS as a writer sees it: the files it holds (by share path), the
    staged titles, and a put that can be made to fail on its n-th call while
    leaving the file as it was ('keep'), half-written ('torn'), already
    carrying the new bytes ('landed' - a stale write time) or gone ('absent')."""

    LIB = os.path.join(lw.MNT, "Files", "Games-Library")

    def __init__(self, titles=("Quake3", "T"), files=None, fail=None, rm_rc=0):
        self.titles = set(titles)
        self.files = dict(files or {})
        self.fail = dict(fail or {})
        self.rm_rc = rm_rc
        self.calls = []

    def put(self, local, rel, auth, log=print):
        with open(local, "rb") as fh:
            data = fh.read()
        self.calls.append(("put", rel, data, auth))
        how = self.fail.get(sum(1 for c in self.calls if c[0] == "put"))
        if how is None:
            self.files[rel] = data
            return 0
        if how == "torn":
            self.files[rel] = data[: len(data) // 2] + b"\0TORN"
        elif how == "landed":
            self.files[rel] = data
        elif how == "absent":
            self.files.pop(rel, None)
        return 4 if how == "landed" else 3

    def rm(self, rel, auth, log=print):
        self.calls.append(("rm", rel, None, auth))
        if self.rm_rc == 0:
            self.files.pop(rel, None)
        return self.rm_rc

    def read(self, rel):
        return self.files.get(rel)

    def isdir(self, path):
        return (os.path.dirname(path) == self.LIB
                and os.path.basename(path) in self.titles)


def _smbwriter(share, tmp_path=None):
    FakeAuth.entered = FakeAuth.exited = 0
    rescue = str(tmp_path / "rescue") if tmp_path is not None else None
    return lw.SmbWriter(log=lambda m: None, put=share.put, rm=share.rm,
                        auth=FakeAuth, read=share.read, isdir=share.isdir,
                        rescue_root=rescue, settle_s=0)


def test_each_write_is_one_put_of_exactly_those_bytes_to_the_mapped_path(tmp_path):
    share = FakeShare(files={"Files/Games-Library/Quake3/stale.cfg": b"old"})
    w = _smbwriter(share, tmp_path)
    src = tmp_path / "FLEETRES.EXE"
    src.write_bytes(b"MZ\x90\x00payload")
    with w:
        w.write_bytes(LIB_MNT + "/Quake3/FLEETRES.BAT", b"@echo off\r\n")
        w.copy_file(str(src), LIB_MNT + "/Quake3/FLEETRES.EXE")
        w.remove(LIB_MNT + "/Quake3/stale.cfg")
        staging = w._tmp
    assert share.calls == [
        ("put", "Files/Games-Library/Quake3/FLEETRES.BAT", b"@echo off\r\n", "/fake/auth"),
        ("put", "Files/Games-Library/Quake3/FLEETRES.EXE", b"MZ\x90\x00payload", "/fake/auth"),
        ("rm", "Files/Games-Library/Quake3/stale.cfg", None, "/fake/auth"),
    ]
    # the vault is read once per run, and the auth file is removed at the end
    assert FakeAuth.entered == 1 and FakeAuth.exited == 1
    assert w.written == ["Files/Games-Library/Quake3/FLEETRES.BAT",
                         "Files/Games-Library/Quake3/FLEETRES.EXE"]
    assert staging and not os.path.exists(staging), \
        "the temp staging dir must be cleaned up"


def test_the_first_failed_file_stops_the_run(tmp_path):
    share = FakeShare(fail={2: "keep"})
    w = _smbwriter(share, tmp_path)
    with w:
        w.write_bytes(LIB_MNT + "/T/a.bat", b"a")
        with pytest.raises(lw.LibWriteError) as e:
            w.write_bytes(LIB_MNT + "/T/b.bat", b"b")
    assert "Files/Games-Library/T/b.bat" in str(e.value)
    assert "1 file(s) published before it" in str(e.value)
    assert "Nothing was left on the share" in str(e.value)
    assert w.written == ["Files/Games-Library/T/a.bat"]


def test_check_and_dry_run_never_reach_the_vault_or_make_a_temp_dir():
    share = FakeShare()
    w = _smbwriter(share)
    assert w._tmp is None, "a writer that writes nothing must create nothing"
    w.close()
    assert FakeAuth.entered == 0 and share.calls == [] and w._tmp is None


def test_a_path_off_the_share_is_refused_before_anything_is_written(tmp_path):
    share = FakeShare()
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError):
        w.write_bytes(str(tmp_path / "x.bat"), b"x")
    assert share.calls == [] and FakeAuth.entered == 0


# --- what a generator may touch: retro-autodeploy acts on the title set --------

def test_a_new_title_directory_is_refused_before_anything_is_sent(tmp_path):
    """A mistyped title in a path would make sharewrite's mkdir create a NEW
    top-level directory - a new title - and retro-autodeploy then runs
    GAMESYNC RESET + START on every box that answers."""
    share = FakeShare(titles=("Quake3-TeamArena",))
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/Quake3TeamArena/Play Quake III.bat", b"x")
    assert "NEW title directory" in str(e.value)
    assert "retro-autodeploy" in str(e.value)
    assert share.calls == [] and FakeAuth.entered == 0


@pytest.mark.parametrize("path", [
    LIB_MNT + "/_deploy_generation.txt",          # the fleet-wide deploy signal
    LIB_MNT + "/_priority.txt",
    LIB_MNT + "/Quake3",                          # a title directory itself
    "/mnt/retro-share/Utility/Retro Automation/retro_agent.exe.ver",
    "/mnt/retro-share/Files/Games/DOSFILL/x.zip",
])
def test_the_library_root_and_anything_outside_the_library_are_refused(tmp_path, path):
    share = FakeShare()
    w = _smbwriter(share, tmp_path)
    with w:
        for op in (lambda: w.write_bytes(path, b"x"), lambda: w.remove(path),
                   lambda: w.rmdir(path)):
            with pytest.raises(lw.LibWriteError):
                op()
    assert share.calls == [] and FakeAuth.entered == 0


def test_underscore_support_dirs_are_not_titles_and_stay_writable(tmp_path):
    share = FakeShare(titles=())
    w = _smbwriter(share, tmp_path)
    with w:
        w.write_bytes(lw.MNT + "/" + lw.SELFTEST_REL + "/x.txt", b"x")
    assert share.files == {lw.SELFTEST_REL + "/x.txt": b"x"}


def test_the_place_rules_compare_the_library_prefix_without_case():
    assert lw.library_place("files/games-library/Quake3/a.bat") == ("Quake3", "a.bat")
    assert lw.library_place("Files/Games-LibraryX/Quake3/a.bat") is None
    assert lw.library_place("Files/Games-Library/Quake3") == ("Quake3", "")


# --- a write that does not land never leaves a torn file ----------------------------

OLD = b"@echo off\r\nrem the previous launcher\r\n"
NEW = b"@echo off\r\nrem the regenerated launcher, longer\r\n"
REL = "Files/Games-Library/T/Play T.bat"


def test_identical_bytes_are_not_rewritten(tmp_path):
    share = FakeShare(files={REL: NEW})
    w = _smbwriter(share, tmp_path)
    with w:
        assert w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW) is False
    assert share.calls == [] and w.written == [] and w.unchanged == [REL]
    assert FakeAuth.entered == 0, "an unchanged file needs no credentials"


def test_a_torn_overwrite_is_put_back_to_the_previous_version(tmp_path):
    share = FakeShare(files={REL: OLD}, fail={1: "torn"})
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert share.files[REL] == OLD, "the previous version must be back"
    assert [c[2] for c in share.calls] == [NEW, OLD]
    assert "put back and verified" in str(e.value)
    saved = [os.path.join(dp, f) for dp, _, fs in os.walk(tmp_path / "rescue") for f in fs]
    assert len(saved) == 1 and open(saved[0], "rb").read() == OLD
    assert saved[0] in str(e.value), "the kept copy must be named"


def test_an_overwrite_that_never_happened_is_left_alone(tmp_path):
    share = FakeShare(files={REL: OLD}, fail={1: "keep"})
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert "previous version is intact" in str(e.value)
    assert len(share.calls) == 1 and share.files[REL] == OLD
    assert not (tmp_path / "rescue").exists()


def test_new_bytes_with_a_stale_write_time_are_not_rolled_back(tmp_path):
    share = FakeShare(files={REL: OLD}, fail={1: "landed"})
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert "new content IS on the share" in str(e.value)
    assert len(share.calls) == 1 and share.files[REL] == NEW


def test_a_partial_new_file_is_deleted_again(tmp_path):
    share = FakeShare(fail={1: "torn"})
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert REL not in share.files
    assert share.calls[-1] == ("rm", REL, None, "/fake/auth")
    assert "partial NEW file was deleted" in str(e.value)


def test_a_partial_new_file_that_cannot_be_deleted_is_named_loudly(tmp_path):
    share = FakeShare(fail={1: "torn"}, rm_rc=3)
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert "PARTIAL NEW FILE IS LEFT ON THE SHARE" in str(e.value)


def test_when_the_previous_version_cannot_go_back_it_is_kept_and_named(tmp_path):
    share = FakeShare(files={REL: OLD}, fail={1: "torn", 2: "torn"})
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    msg = str(e.value)
    assert "COULD NOT PUT THE PREVIOUS VERSION BACK" in msg
    assert "sharewrite.py put" in msg and REL in msg
    saved = [os.path.join(dp, f) for dp, _, fs in os.walk(tmp_path / "rescue") for f in fs]
    assert len(saved) == 1 and open(saved[0], "rb").read() == OLD
    assert saved[0] in msg


def test_a_put_that_raises_is_repaired_like_one_that_failed(tmp_path):
    """sharewrite.put can raise (smbclient missing, a path it will not quote,
    a file that vanished mid-check) - possibly after the NAS was touched. That
    must end in the same repair and banner, never a bare traceback."""
    share = FakeShare(files={REL: OLD})
    real_put = share.put

    def exploding_put(local, rel, auth, log=print):
        if not share.calls:
            share.calls.append(("put", rel, b"", auth))
            share.files[rel] = b"TORN"
            raise RuntimeError("smbclient went away")
        return real_put(local, rel, auth, log)
    share.put = exploding_put
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert share.files[REL] == OLD
    assert "RuntimeError: smbclient went away" in str(e.value)
    assert "put back and verified" in str(e.value)


def test_a_rescue_dir_that_cannot_be_written_still_restores(tmp_path, monkeypatch):
    import tempfile
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))  # the fallback copy
    blocker = tmp_path / "not-a-dir"
    blocker.write_bytes(b"")
    share = FakeShare(files={REL: OLD}, fail={1: "torn"})
    w = lw.SmbWriter(log=lambda m: None, put=share.put, rm=share.rm,
                     auth=FakeAuth, read=share.read, isdir=share.isdir,
                     rescue_root=str(blocker / "rescue"), settle_s=0)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert share.files[REL] == OLD
    assert "could not be kept under" in str(e.value)


def test_the_failure_path_waits_for_the_mount_before_judging_it():
    """/mnt is actimeo=1,closetimeo=1: reading it the instant a put failed can
    show the file as it was a moment ago and call a torn file 'intact'."""
    assert lw.RECOVER_SETTLE_S >= 2
    assert lw.SmbWriter().settle_s == lw.RECOVER_SETTLE_S
    assert lw.LocalWriter().settle_s == lw.RECOVER_SETTLE_S


def test_a_previous_version_that_cannot_be_read_blocks_the_write(tmp_path):
    share = FakeShare(files={REL: OLD})

    def unreadable(rel):
        raise OSError(5, "Input/output error")
    share.read = unreadable
    w = _smbwriter(share, tmp_path)
    with w, pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_MNT + "/T/Play T.bat", NEW)
    assert "refusing to overwrite" in str(e.value)
    assert share.calls == []


def test_the_gvfs_writer_repairs_a_dropped_write_the_same_way(tmp_path, monkeypatch):
    """gvfs has left a destination MISSING after a failed copy here (CLAUDE.md
    'WRITING TO THE SHARE'). The local writer on a share path reads the
    previous version first and puts it back through the same path."""
    files = {REL: OLD}
    monkeypatch.setattr(lw, "read_share", lambda rel: files.get(rel))
    writes = []

    def raw_write(self, path, data):
        writes.append(data)
        if len(writes) == 1:
            files.pop(REL, None)            # the drop: destination gone
            raise OSError(5, "Input/output error")
        files[REL] = data

    monkeypatch.setattr(lw.LocalWriter, "_raw_write", raw_write)
    monkeypatch.setattr(lw.LocalWriter, "_landed",
                        lambda self, path, data: 0 if files.get(REL) == data else 3)
    w = lw.LocalWriter(log=lambda m: None, isdir=FakeShare().isdir,
                       rescue_root=str(tmp_path / "rescue"), settle_s=0)
    with pytest.raises(lw.LibWriteError) as e:
        w.write_bytes(LIB_GVFS + "/T/Play T.bat", NEW)
    assert files[REL] == OLD and writes == [NEW, OLD]
    assert "put back and verified" in str(e.value)


def test_local_writer_writes_and_removes(tmp_path):
    w = lw.LocalWriter(log=lambda m: None)
    p = tmp_path / "T" / "a.bat"
    w.write_bytes(str(p), b"abc")
    assert p.read_bytes() == b"abc"
    w.remove(str(p))
    assert not p.exists()


def test_local_writer_reports_an_os_error_as_a_publish_failure(tmp_path):
    (tmp_path / "file").write_bytes(b"")
    w = lw.LocalWriter(log=lambda m: None)
    with pytest.raises(lw.LibWriteError):
        w.write_bytes(str(tmp_path / "file" / "under-a-file.bat"), b"x")


# --- the generators use it -------------------------------------------------------

GENERATORS = ("stage-fleetres.py", "make-mount-launcher.py",
              "stage-serioussam.py", "stage-dosnative.py")


@pytest.mark.parametrize("fn", GENERATORS)
def test_every_library_generator_writes_through_libwrite(fn):
    src = open(os.path.join(FLEET, fn), encoding="utf-8").read()
    assert "import libwrite" in src and "libwrite.add_arguments(ap)" in src, fn
    # no direct write path left beside it: open(..., 'wb'/'w'), copyfile, remove
    code = "\n".join(l for l in src.splitlines() if not l.lstrip().startswith("#"))
    for pat in (r"open\([^)]*['\"](?:w|a|x|r\+)b?\+?['\"]",
                r"shutil\.(?:copy|move|rmtree)", r"os\.(?:remove|unlink|rename|"
                r"replace|rmdir|makedirs|mkdir)\(", r"\.write_text\("):
        assert not re.search(pat, code), "%s still writes directly: %s" % (fn, pat)


def test_stage_fleetres_runner_picks_the_backend_from_the_library(tmp_path):
    sf = _load("stage_fleetres", "stage-fleetres.py")
    assert sf.Runner(str(tmp_path), dry=False, check=False).writer.kind == "local"
    smb = sf.Runner(LIB_MNT, dry=False, check=True).writer
    # a --check run constructs this writer and never writes: it must leave no
    # temp dir behind (each run used to leak one /tmp/libwrite-* directory)
    assert smb.kind == "smb" and smb._tmp is None


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


def _run(fn, *args):
    import subprocess
    return subprocess.run([sys.executable, os.path.join(FLEET, fn)] + list(args),
                          capture_output=True, text=True)


def test_via_smb_off_the_share_is_a_usage_error_not_a_publish_failure(tmp_path):
    spec = os.path.join(REPO, "provisioning", "discmount", "specs", "BF1942.json")
    r = _run("make-mount-launcher.py", "--spec", spec,
             "--out", str(tmp_path / "Play X.bat"), "--via-smb")
    assert r.returncode == 2 and "not a path on the share" in r.stderr
    assert "PUBLISH FAILED" not in r.stderr and not (tmp_path / "Play X.bat").exists()
    r = _run("stage-serioussam.py", str(tmp_path), "--via-smb")
    assert r.returncode == 2 and "not a path on the share" in r.stderr
    assert "Traceback" not in r.stderr


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
