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

Two rules every writer keeps on a share path:

  * (checked BEFORE anything is sent) A generator changes titles that are
    ALREADY STAGED. It never creates a new top-level library directory, never
    writes a file in the library root, and never writes outside
    Files/Games-Library. retro-autodeploy (a --user service, 90 s passes)
    answers a change in the library's TITLE SET - and a change of
    _deploy_generation.txt in the root - by running GAMESYNC RESET + START on
    every box that answers, so one mistyped title in a path would resync the
    whole fleet. `_`-prefixed support dirs (_patches, ...) are not titles and
    stay writable.
  * A file that is overwritten has its previous bytes read first. When the
    write does not land, the writer leaves the share holding either the new
    bytes or the previous ones - never a torn file: it puts the previous
    version back (verified), or deletes a partial NEW file, and keeps a copy
    of the previous version under ~/.retro-fleet/libwrite-rescue/ when it
    cannot. The error says which. Identical bytes are never rewritten: that
    would only bump the write time and make every box recopy the file.

    python3 scripts/fleet/libwrite.py selftest   # the whole round trip, live
"""
import argparse
import hashlib
import importlib.util
import os
import shlex
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
GENERATION_FILE = "_deploy_generation.txt"
# Durable (the session scratchpad is deleted at session end): where the
# previous version of a file goes when a failed write could not put it back.
RESCUE_ROOT = os.path.join(os.path.expanduser("~"), ".retro-fleet",
                           "libwrite-rescue")


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


# --- what a library generator may touch ---------------------------------------

def library_place(rel):
    """(top, sub) for a share path inside the staged library - `top` is the
    library-root entry it sits under (a title, or a `_` support directory),
    `sub` the rest - or None for a path outside the library. Compared
    case-insensitively: this is a Windows share."""
    pre = LIBRARY_REL + "/"
    if not rel.lower().startswith(pre.lower()):
        return None
    top, _, sub = rel[len(pre):].partition("/")
    return top, sub


def check_place(rel, isdir=os.path.isdir, allow_new_title=False):
    """Refuse a write/remove that no library generator may make. Raises
    LibWriteError BEFORE anything is sent; returns None when it is allowed.

    A new title directory, or anything in the library root, is exactly what
    retro-autodeploy answers with GAMESYNC RESET + START on every box that
    answers - and a generator only changes titles that are already staged."""
    place = library_place(rel)
    if place is None:
        raise LibWriteError(
            "%s is outside %s - a library generator writes only into the "
            "staged library" % (rel, LIBRARY_REL))
    top, sub = place
    if not top or not sub:
        raise LibWriteError(
            "%s is in the library ROOT (or IS a title directory), which no "
            "generator writes: %s there is the fleet-wide deploy signal "
            "(retro-autodeploy runs GAMESYNC RESET + START on every box that "
            "answers when it changes), and a new or removed top-level "
            "directory changes the title set, which does the same. Do it by "
            "hand, deliberately, if it is really meant"
            % (rel, GENERATION_FILE))
    if top.startswith("_") or allow_new_title:
        return
    if not isdir(os.path.join(MNT, *LIBRARY_REL.split("/"), top)):
        raise LibWriteError(
            "%s would create a NEW title directory %r. That changes the "
            "library's title set, and retro-autodeploy answers a new title by "
            "running GAMESYNC RESET + START on every box that answers. A "
            "generator only changes titles that are already staged - check "
            "the title name" % (rel, top))


def read_share(rel):
    """The bytes the share holds at `rel`, read through /mnt; None when there
    is no such file. Any other failure raises OSError - "could not read it" is
    not "it is absent"."""
    try:
        with open(os.path.join(MNT, *rel.split("/")), "rb") as fh:
            return fh.read()
    except FileNotFoundError:
        return None


def save_rescue(rel, data, root=None):
    """Keep `data` (the previous version of `rel`) on local disk; its path."""
    d = os.path.join(root or RESCUE_ROOT,
                     "%s-%d" % (time.strftime("%Y%m%d-%H%M%S"), os.getpid()))
    p = os.path.join(d, *rel.split("/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as fh:
        fh.write(data)
    return p


def _keep(rel, data, root):
    """save_rescue(), falling back to a temp file: (path or None, problem)."""
    try:
        return save_rescue(rel, data, root), ""
    except OSError as e:
        try:
            fd, p = tempfile.mkstemp(prefix="libwrite-rescue-")
            with os.fdopen(fd, "wb") as fh:
                fh.write(data)
            return p, " (it could not be kept under %s: %s)" % (root or RESCUE_ROOT, e)
        except OSError as e2:
            return None, " (no local copy could be kept at all: %s; %s)" % (e, e2)


def _restore_hint(saved, rel):
    return ("python3 scripts/fleet/sharewrite.py put %s %s"
            % (shlex.quote(saved), shlex.quote(rel)))


def _attempt(fn, *a):
    """Run a repair step; a step that RAISES is a step that failed - the
    failure path must never hide the original failure behind a traceback."""
    try:
        return fn(*a)
    except Exception:
        return 1


# /mnt is `actimeo=1,closetimeo=1`: after a failed write, give its caches this
# long before reading back what the share holds - never judge a stale view.
RECOVER_SETTLE_S = 3


def recover(rel, before, data, read, put_back, delete, rescue_root=None,
            log=print, settle_s=RECOVER_SETTLE_S):
    """A write of `data` to `rel` did not land. Leave the share holding the
    new bytes or the previous ones - never a torn file - and SAY which.

    read()          -> the bytes the share holds now, None when absent
    put_back(local) -> 0 once the file at `local` is on the share, verified
    delete()        -> 0 once `rel` is verifiably gone
    Returns one sentence for the error message."""
    if settle_s:
        time.sleep(settle_s)
    try:
        now = read()
    except OSError as e:
        if before is None:
            return ("The share could not be read afterwards (%s): check %s "
                    "by hand - it was a NEW file, so delete it if it is there "
                    "and incomplete." % (e, rel))
        saved, why = _keep(rel, before, rescue_root)
        if saved is None:
            return ("The share could not be read afterwards (%s), and the "
                    "previous version of %s could not be kept%s - check the "
                    "file by hand." % (e, rel, why))
        return ("The share could not be read afterwards (%s). The previous "
                "version is saved at %s%s - check %s and, if it is damaged, "
                "restore it with:  %s"
                % (e, saved, why, rel, _restore_hint(saved, rel)))
    if now == data:
        return ("Its new content IS on the share, byte for byte (read back "
                "after the failure) - what failed was the check; the log "
                "above says which (code 4 = a stale write time).")
    if before is None:
        if now is None:
            return "Nothing was left on the share."
        if _attempt(delete) == 0:
            return "The partial NEW file was deleted again (verified gone)."
        return ("A PARTIAL NEW FILE IS LEFT ON THE SHARE at %s - delete it by "
                "hand before any box syncs this title." % rel)
    if now == before:
        return "The previous version is intact on the share."
    saved, why = _keep(rel, before, rescue_root)
    if saved is None:
        return ("COULD NOT PUT THE PREVIOUS VERSION BACK - %s is DAMAGED on "
                "the share%s. Take the file from a box that synced it before."
                % (rel, why))
    log("restoring the previous version of %s from %s" % (rel, saved))
    if _attempt(put_back, saved) == 0:
        return ("The file was damaged and the previous version was put back "
                "and verified (a copy is kept at %s%s)." % (saved, why))
    return ("COULD NOT PUT THE PREVIOUS VERSION BACK - %s is DAMAGED on the "
            "share. The previous version is saved at %s%s; restore it with:  %s"
            % (rel, saved, why, _restore_hint(saved, rel)))


class _Writer:
    kind = "?"

    def __init__(self, log=None):
        self.log = log or (lambda m: print(m, flush=True))
        self.written = []           # share/local paths, in order
        self.removed = []
        self.unchanged = []         # identical bytes, deliberately not rewritten

    def write_text(self, path, text, encoding="latin1"):
        return self.write_bytes(path, text.encode(encoding))

    def _unchanged(self, rel):
        self.log("unchanged %s - not rewritten (a rewrite only bumps its "
                 "write time, and every box would recopy it)" % rel)
        self.unchanged.append(rel)
        return False

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class LocalWriter(_Writer):
    """A plain filesystem write. On a SHARE path (gvfs) it also keeps the
    rules above: the place is checked first, identical bytes are skipped, the
    previous version is read first, each file is read back through /mnt before
    the next is written, and a write that does not land is repaired or
    reported. A path off the share (a test's tmp_path) is written plainly."""
    kind = "local"

    def __init__(self, log=None, verify=None, isdir=None, rescue_root=None,
                 allow_new_title=False, settle_s=RECOVER_SETTLE_S):
        super().__init__(log)
        self.verify = verify    # None = verify exactly when the path is on the share
        self._isdir = isdir or os.path.isdir
        self.rescue_root = rescue_root
        self.allow_new_title = allow_new_title
        self.settle_s = settle_s

    def _share(self, path):
        rel = share_rel(path)
        if rel is None:
            return None
        if not rel:
            raise LibWriteError("%s is the share itself, not a file" % path)
        check_place(rel, self._isdir, self.allow_new_title)
        return rel

    def _raw_write(self, path, data):
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)

    def _landed(self, path, data):
        """0 once the /mnt copy reads back as `data` (md5 + a fresh mtime)."""
        mp = mnt_path(path)
        if mp is None:
            return 0
        return sharewrite.verify_landed(mp, len(data), _md5_bytes(data), self.log)

    def write_bytes(self, path, data):
        rel = self._share(path)
        if rel is None:
            try:
                self._raw_write(path, data)
            except OSError as e:
                raise LibWriteError("could not write %s: %s" % (path, e))
            self.written.append(path)
            return True
        try:
            before = read_share(rel)
        except OSError as e:
            raise LibWriteError("cannot read the current %s through %s (%s) - "
                                "refusing to overwrite a file whose previous "
                                "version could not be kept" % (rel, MNT, e))
        if before == data:
            return self._unchanged(rel)
        verify = self.verify if self.verify is not None else True
        try:
            self._raw_write(path, data)
            rc = self._landed(path, data) if verify else 0
            why = "read back through %s, code %d" % (MNT, rc)
        except OSError as e:
            rc, why = 1, str(e)
        if rc:
            def put_back(local):
                try:
                    with open(local, "rb") as fh:
                        old = fh.read()
                    self._raw_write(path, old)
                    return self._landed(path, old)
                except OSError:
                    return 1

            def delete():
                try:
                    os.remove(path)
                except OSError:
                    return 1
                return 0 if self._gone(path) else 1
            outcome = recover(rel, before, data, lambda: read_share(rel),
                              put_back, delete, self.rescue_root, self.log,
                              self.settle_s)
            raise LibWriteError("PUBLISH FAILED at %s (%s) - stopped; %d "
                                "file(s) published before it. %s"
                                % (rel, why, len(self.written), outcome))
        self.written.append(path)
        return True

    def copy_file(self, src, dst):
        with open(src, "rb") as fh:
            return self.write_bytes(dst, fh.read())

    @staticmethod
    def _gone(path):
        mp = mnt_path(path)
        if mp is None:
            return not os.path.lexists(path)
        # give the /mnt attribute cache the same grace a write gets
        deadline = time.time() + sharewrite.VERIFY_WAIT_S
        while os.path.lexists(mp) and time.time() < deadline:
            time.sleep(2)
        return not os.path.lexists(mp)

    def remove(self, path):
        self._share(path)
        try:
            os.remove(path)
        except OSError as e:
            raise LibWriteError("could not delete %s: %s" % (path, e))
        if not self._gone(path):
            raise LibWriteError("%s still exists after the delete" % path)
        self.removed.append(path)

    def rmdir(self, path):
        self._share(path)
        try:
            os.rmdir(path)
        except OSError as e:
            raise LibWriteError("could not remove %s: %s" % (path, e))
        self.removed.append(path)


class SmbWriter(_Writer):
    """Every write is one sharewrite.put(), verified before the next one.

    `put`/`rm`/`rmdir`/`auth`/`read`/`isdir` are injectable so the selection,
    the place rules, the repair of a failed write and the stop-at-the-first-
    failure rule are testable with no network."""
    kind = "smb"

    def __init__(self, log=None, put=None, rm=None, rmdir=None, auth=None,
                 tmpdir=None, read=None, isdir=None, rescue_root=None,
                 allow_new_title=False, settle_s=RECOVER_SETTLE_S):
        super().__init__(log)
        self._put = put or sharewrite.put
        self._rm = rm or sharewrite.rm
        self._rmdir = rmdir or sharewrite.rmdir
        self._auth_factory = auth or sharewrite.Auth
        self._auth_cm = None
        self._auth = None
        self._tmpdir = tmpdir
        self._tmp = None            # made on the first write: --check makes none
        self._read = read or read_share
        self._isdir = isdir or os.path.isdir
        self.rescue_root = rescue_root
        self.allow_new_title = allow_new_title
        self.settle_s = settle_s

    def _get_auth(self):
        # Lazily: a --check or --dry-run never reaches the vault.
        if self._auth is None:
            try:
                self._auth_cm = self._auth_factory()
                self._auth = self._auth_cm.__enter__()
            except SystemExit as e:           # keyvault failure in sharewrite.Auth
                raise LibWriteError("NAS credentials unavailable: %s" % e)
        return self._auth

    def _staging(self):
        if self._tmp is None:
            self._tmp = tempfile.mkdtemp(prefix="libwrite-", dir=self._tmpdir)
        return self._tmp

    def _rel(self, path):
        rel = share_rel(path)
        if not rel:
            raise LibWriteError("%s is not a file on the share - refusing to "
                                "guess where it should go" % path)
        check_place(rel, self._isdir, self.allow_new_title)
        return rel

    def _before(self, rel):
        try:
            return self._read(rel)
        except OSError as e:
            raise LibWriteError("cannot read the current %s through %s (%s) - "
                                "refusing to overwrite a file whose previous "
                                "version could not be kept" % (rel, MNT, e))

    def _publish(self, path, data, local=None):
        """Put `data` at `path` - from the file `local` when the bytes are
        already in one (a binary payload), else from a staging temp file."""
        rel = self._rel(path)               # refuse before touching anything
        before = self._before(rel)
        if before == data:
            return self._unchanged(rel)
        auth = self._get_auth()             # a vault failure: nothing was sent
        tmp = None
        err = ""
        try:
            if local is None:
                try:
                    fd, tmp = tempfile.mkstemp(dir=self._staging())
                    with os.fdopen(fd, "wb") as fh:
                        fh.write(data)
                except OSError as e:
                    raise LibWriteError("could not stage %s locally (%s) - "
                                        "nothing was sent" % (rel, e))
                local = tmp
            try:
                rc = self._put(local, rel, auth, log=self.log)
            except Exception as e:          # a put that raises is a put that failed
                rc, err = 1, " (%s: %s)" % (type(e).__name__, e)
        finally:
            if tmp:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
        if rc != 0:
            outcome = recover(
                rel, before, data, lambda: self._read(rel),
                lambda saved: self._put(saved, rel, auth, log=self.log),
                lambda: self._rm(rel, auth, log=self.log),
                self.rescue_root, self.log, self.settle_s)
            raise LibWriteError("PUBLISH FAILED at %s (sharewrite code %d%s) - "
                                "stopped; %d file(s) published before it. %s"
                                % (rel, rc, err, len(self.written), outcome))
        self.written.append(rel)
        return True

    def write_bytes(self, path, data):
        return self._publish(path, data)

    def copy_file(self, src, dst):
        with open(src, "rb") as fh:
            data = fh.read()
        return self._publish(dst, data, local=src)

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
        if self._tmp is not None:
            shutil.rmtree(self._tmp, ignore_errors=True)
            self._tmp = None


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
    """The round trip the generators depend on, live, under
    _patches/_sharewrite_selftest/ (a `_` directory: not a title, so neither
    GAMESYNC nor retro-autodeploy ever acts on it) - through the SAME writer
    the generators use for the /mnt library:

      1. the place rules refuse a NEW title directory and a library-root file
         before anything is sent (retro-autodeploy would resync the fleet);
      2. a new file lands, read back through /mnt;
      3. an OVERWRITE with the SAME size and different bytes lands - the case
         a stale CIFS cache or a size-only check would hide, and the one every
         launcher republish is;
      4. identical bytes are not rewritten (the write time does not move);
      5. the file and the directory are removed, verified gone."""
    root = root or os.path.join(MNT, *LIBRARY_REL.split("/"))
    d = os.path.join(MNT, *SELFTEST_REL.split("/"))
    if backend_for(root) != "smb":
        raise LibWriteError("selftest expects the /mnt library (smb backend)")
    pid = os.getpid()
    path = os.path.join(d, "selftest-%d.txt" % pid)
    v1 = ("libwrite selftest %s pid %d - version 1\r\n"
          % (time.strftime("%Y-%m-%d %H:%M:%S"), pid)).encode("ascii")
    v2 = v1.replace(b"version 1", b"version 2")
    assert len(v1) == len(v2) and v1 != v2
    bogus_title = os.path.join(root, "ZZ-libwrite-no-such-title-%d" % pid, "x.txt")
    root_file = os.path.join(root, GENERATION_FILE)
    with writer_for(root) as w:
        for what, p in (("a NEW title directory", bogus_title),
                        ("a file in the library root", root_file)):
            try:
                w.write_bytes(p, b"must never be written\r\n")
            except LibWriteError as e:
                print("selftest: refused %s before sending anything: %s"
                      % (what, str(e)[:110]))
            else:
                raise LibWriteError("selftest: %s was NOT refused" % what)
        if os.path.lexists(os.path.dirname(bogus_title)):
            raise LibWriteError("selftest: %s exists" % os.path.dirname(bogus_title))
        if w.written or w._auth is not None:
            raise LibWriteError("selftest: a refused write reached the share")

        w.write_bytes(path, v1)
        with open(path, "rb") as fh:
            if fh.read() != v1:
                raise LibWriteError("selftest: new file reads back differently")
        t1 = os.path.getmtime(path)
        print("selftest: new file written and read back (%d B)" % len(v1))

        time.sleep(2)               # a coarse clock must still show a newer stamp
        w.write_bytes(path, v2)
        with open(path, "rb") as fh:
            if fh.read() != v2:
                raise LibWriteError("selftest: the same-size overwrite reads "
                                    "back as the OLD bytes")
        t2 = os.path.getmtime(path)
        if t2 < t1:
            raise LibWriteError("selftest: overwrite moved the write time "
                                "backwards (%s -> %s)" % (time.ctime(t1), time.ctime(t2)))
        print("selftest: same-size overwrite landed (md5 verified), write "
              "time %s -> %s" % (time.ctime(t1), time.ctime(t2)))

        n = len(w.written)
        if w.write_bytes(path, v2) is not False or len(w.written) != n:
            raise LibWriteError("selftest: identical bytes were rewritten")
        if os.path.getmtime(path) != t2:
            raise LibWriteError("selftest: an identical write moved the write time")
        print("selftest: identical bytes left alone (write time unchanged)")

        w.remove(path)
        w.rmdir(d)
    if os.path.lexists(path) or os.path.lexists(d):
        raise LibWriteError("selftest: %s still present" % d)
    print("selftest: OK - refusals, new file, same-size overwrite, no-op "
          "rewrite, delete, directory gone")
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
