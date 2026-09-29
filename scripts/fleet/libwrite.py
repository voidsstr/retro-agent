#!/usr/bin/env python3
"""Where a library generator's WRITES go - one file at a time, each one proven.

WHY THIS EXISTS. The generators that own staged launchers and configs
(stage-fleetres.py, make-mount-launcher.py, stage-serioussam.py,
stage-dosnative.py) used to write with a plain open()/shutil.copyfile() into
whatever library path they were given. That only works through the gvfs
mount, which exists only while someone is logged into the desktop. Headless,
the only mount is /mnt/retro-share, and it is READ-ONLY: every generator there
could check the library and could not fix it.

So a generator asks this module for a writer:

    w = libwrite.writer_for(library_root, via_smb=args.via_smb)
    w.write_bytes(path, data)      # path is the /mnt path the generator READ
    w.copy_file(src, path)         # binary payloads too (FLEETRES.EXE)
    w.remove(path)

  * root under /mnt/retro-share (or --via-smb) -> SmbWriter: each file goes
    through scripts/fleet/sharewrite.py (`smbclient put` with the vaulted NAS
    credentials) and is read back through /mnt - md5 AND a fresh write time -
    before the next file is touched. The FIRST failure raises LibWriteError
    and the generator stops: a batch that reports at the end has already
    destroyed files by the time it looks (CLAUDE.md "WRITING TO THE SHARE").
  * root on the gvfs mount -> LocalWriter, but still verified file by file
    through /mnt, for the same reason: gvfs has silently dropped writes here.
  * anything else (a test's tmp_path, a local copy) -> LocalWriter, plain.

Reads keep using the path the generator already has (/mnt): a writer only
changes where bytes are WRITTEN.

    python3 scripts/fleet/libwrite.py selftest   # write + delete a scratch file
"""
import argparse
import hashlib
import importlib.util
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location(
    "sharewrite", os.path.join(HERE, "sharewrite.py"))
sharewrite = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sharewrite)

MNT = sharewrite.MNT
GVFS = ("/run/user/1000/gvfs/smb-share:server=192.168.1.122,"
        "share=files,user=voidsstr")
LIBRARY_REL = "Files/Games-Library"
SELFTEST_REL = LIBRARY_REL + "/_patches/_sharewrite_selftest"


class LibWriteError(RuntimeError):
    """A write that did not land. The run must stop at the first one."""


def _under(path, root):
    p = os.path.normpath(os.path.abspath(path))
    r = os.path.normpath(root)
    if p == r:
        return ""
    if p.startswith(r + os.sep):
        return p[len(r) + 1:].replace(os.sep, "/")
    return None


def share_rel(path):
    """The share-relative path ('Files/Games-Library/X/y.bat') of a path under
    /mnt/retro-share or the gvfs mount, or None when it is on neither."""
    for root in (MNT, GVFS):
        rel = _under(path, root)
        if rel is not None:
            return rel
    return None


def mnt_path(path):
    """The read-only /mnt path that shows the same share file, or None."""
    rel = share_rel(path)
    if rel is None:
        return None
    return os.path.join(MNT, *rel.split("/")) if rel else MNT


def backend_for(root, via_smb=False):
    """'smb' or 'local' - decided by WHERE the library is, never by a guess.

    /mnt/retro-share is mounted read-only (fstab `ro`), so a write there can
    only go over SMB. --via-smb forces it for any path on the share (e.g. the
    gvfs one); for a path that is NOT on the share it is an error rather than
    a silent local write - the operator asked for the share and would not get
    it."""
    rel = share_rel(root)
    if via_smb:
        if rel is None:
            raise LibWriteError(
                "--via-smb: %s is not a path on the share (%s or %s)"
                % (root, MNT, GVFS))
        return "smb"
    if _under(root, MNT) is not None:
        return "smb"
    return "local"


def _md5_bytes(data):
    return hashlib.md5(data).hexdigest()


class _Writer:
    kind = "?"

    def __init__(self, log=None):
        self.log = log or (lambda m: print(m, flush=True))
        self.written = []           # share/local paths, in order
        self.removed = []

    def write_text(self, path, text, encoding="latin1"):
        self.write_bytes(path, text.encode(encoding))

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class LocalWriter(_Writer):
    """A plain filesystem write. When the path is on the share (gvfs), each
    file is ALSO read back through /mnt before the next is written."""
    kind = "local"

    def __init__(self, log=None, verify=None):
        super().__init__(log)
        self.verify = verify    # None = verify exactly when the path is on the share

    def _check(self, path, data):
        verify = self.verify if self.verify is not None else share_rel(path) is not None
        if not verify:
            return
        mp = mnt_path(path)
        if mp is None:
            return
        rc = sharewrite.verify_landed(mp, len(data), _md5_bytes(data), self.log)
        if rc:
            raise LibWriteError("%s did not land (read back through %s, code %d)"
                                % (path, MNT, rc))

    def write_bytes(self, path, data):
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
        self._check(path, data)
        self.written.append(path)

    def copy_file(self, src, dst):
        with open(src, "rb") as fh:
            self.write_bytes(dst, fh.read())

    def remove(self, path):
        os.remove(path)
        if share_rel(path) is not None and os.path.lexists(mnt_path(path)):
            # give the /mnt attribute cache the same grace a write gets
            deadline = time.time() + sharewrite.VERIFY_WAIT_S
            while os.path.lexists(mnt_path(path)) and time.time() < deadline:
                time.sleep(2)
            if os.path.lexists(mnt_path(path)):
                raise LibWriteError("%s still exists after the delete" % path)
        self.removed.append(path)

    def rmdir(self, path):
        os.rmdir(path)
        self.removed.append(path)


class SmbWriter(_Writer):
    """Every write is one sharewrite.put(), verified before the next one.

    `put`/`rm`/`rmdir`/`auth` are injectable so the selection and the
    stop-at-the-first-failure rule are testable with no network."""
    kind = "smb"

    def __init__(self, log=None, put=None, rm=None, rmdir=None, auth=None,
                 tmpdir=None):
        super().__init__(log)
        self._put = put or sharewrite.put
        self._rm = rm or sharewrite.rm
        self._rmdir = rmdir or sharewrite.rmdir
        self._auth_factory = auth or sharewrite.Auth
        self._auth_cm = None
        self._auth = None
        self._tmp = tempfile.mkdtemp(prefix="libwrite-", dir=tmpdir)

    def _get_auth(self):
        # Lazily: a --check or --dry-run never reaches the vault.
        if self._auth is None:
            try:
                self._auth_cm = self._auth_factory()
                self._auth = self._auth_cm.__enter__()
            except SystemExit as e:           # keyvault failure in sharewrite.Auth
                raise LibWriteError("NAS credentials unavailable: %s" % e)
        return self._auth

    @staticmethod
    def _rel(path):
        rel = share_rel(path)
        if not rel:
            raise LibWriteError("%s is not a file on the share - refusing to "
                                "guess where it should go" % path)
        return rel

    def _put_local(self, local, path):
        rel = self._rel(path)
        rc = self._put(local, rel, self._get_auth(), log=self.log)
        if rc != 0:
            raise LibWriteError("PUBLISH FAILED at %s (sharewrite code %d) - "
                                "stopped; %d file(s) published before it"
                                % (rel, rc, len(self.written)))
        self.written.append(rel)

    def write_bytes(self, path, data):
        self._rel(path)                     # refuse before touching disk
        fd, tmp = tempfile.mkstemp(dir=self._tmp)
        try:
            with os.fdopen(fd, "wb") as fh:
                fh.write(data)
            self._put_local(tmp, path)
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass

    def copy_file(self, src, dst):
        self._put_local(src, dst)

    def remove(self, path):
        rel = self._rel(path)
        rc = self._rm(rel, self._get_auth(), log=self.log)
        if rc != 0:
            raise LibWriteError("DELETE FAILED at %s (sharewrite code %d)" % (rel, rc))
        self.removed.append(rel)

    def rmdir(self, path):
        rel = self._rel(path)
        rc = self._rmdir(rel, self._get_auth(), log=self.log)
        if rc != 0:
            raise LibWriteError("RMDIR FAILED at %s (sharewrite code %d)" % (rel, rc))
        self.removed.append(rel)

    def close(self):
        if self._auth_cm is not None:
            self._auth_cm.__exit__(None, None, None)
            self._auth_cm = self._auth = None
        shutil.rmtree(self._tmp, ignore_errors=True)


def writer_for(root, via_smb=False, log=None):
    """The writer for a library rooted at `root` (see backend_for)."""
    if backend_for(root, via_smb) == "smb":
        return SmbWriter(log=log)
    return LocalWriter(log=log)


def add_arguments(ap):
    """The one flag every generator shares."""
    ap.add_argument("--via-smb", action="store_true",
                    help="publish every write through scripts/fleet/sharewrite.py "
                         "(smbclient + md5 read-back through /mnt). Automatic when "
                         "the library is the read-only /mnt/retro-share mount.")


def failure_banner(e, writer=None):
    n = len(writer.written) if writer is not None else 0
    bar = "=" * 72
    return ("%s\nPUBLISH FAILED - the run stopped at the first file that did not "
            "land.\n%s\n%d file(s) were published and verified before it. The "
            "library may be half-updated:\nre-run this generator once the share "
            "is reachable, then run\n  python3 scripts/validate-staged-library.py"
            "\n%s" % (bar, e, n, bar))


def selftest(root=None):
    """Write a scratch file under _patches/_sharewrite_selftest/, verify it,
    delete it and its directory, verify both are gone - through the SAME
    writer the generators use for the /mnt library."""
    root = root or os.path.join(MNT, *LIBRARY_REL.split("/"))
    d = os.path.join(MNT, *SELFTEST_REL.split("/"))
    if backend_for(root) != "smb":
        raise LibWriteError("selftest expects the /mnt library (smb backend)")
    path = os.path.join(d, "selftest-%d.txt" % os.getpid())
    body = ("libwrite selftest %s pid %d\r\n" %
            (time.strftime("%Y-%m-%d %H:%M:%S"), os.getpid())).encode("ascii")
    with writer_for(root) as w:
        w.write_bytes(path, body)
        with open(path, "rb") as fh:
            assert fh.read() == body, "read-back through /mnt differs"
        print("selftest: wrote and read back %s (%d B)" % (path, len(body)))
        w.remove(path)
        w.rmdir(d)
    if os.path.lexists(path) or os.path.lexists(d):
        raise LibWriteError("selftest: %s still present" % d)
    print("selftest: OK - written, verified, deleted, directory gone")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest")
    b = sub.add_parser("backend", help="print which backend a path gets")
    b.add_argument("root")
    add_arguments(b)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "selftest":
            return selftest()
        print(backend_for(a.root, a.via_smb))
        return 0
    except LibWriteError as e:
        print(failure_banner(e), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
