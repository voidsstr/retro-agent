#!/usr/bin/env python3
"""Write ONE file to the NAS share and prove it landed - smbclient + vault creds.

WHY THIS EXISTS. The read-write gvfs mount only exists while someone is logged
into the desktop, so a headless session has nowhere to publish a proven library
fix. `smbclient -A` works headless (CLAUDE.md "Publishing builds to the share"),
but every write to this NAS has to be verified FILE BY FILE: gvfs has silently
dropped writes here, and a batch report ("copied 10, failed 0") has already
certified a run that destroyed three launchers.

So each call writes one file and then reads it back through /mnt/retro-share -
a DIFFERENT mount from the one that wrote it - and compares md5. It also checks
the write time: GAMESYNC skips a file whose size AND mtime match what a box
already has, so a same-size edit stamped with a stale time would silently never
reach a box (smbclient on this NAS once stamped Oct 2007; measured correct on
2026-09-29, but it is checked every time rather than trusted).

    python3 scripts/fleet/sharewrite.py put <local file> "Files/Games-Library/<Title>/<file>"
    python3 scripts/fleet/sharewrite.py rm  "Files/Games-Library/<Title>/<file>"
    python3 scripts/fleet/sharewrite.py put ... --dry-run

Exit 0 only when the post-condition holds (md5 match / file gone). The NAS
credentials come from Azure Key Vault (fleet-nas-192-168-1-122-user/-password)
into a 0600 temp file that is deleted on exit; they are never put in argv.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time

SERVER = "//192.168.1.122/files"
MNT = "/mnt/retro-share"
HERE = os.path.dirname(os.path.abspath(__file__))
KEYVAULT = os.path.join(HERE, "keyvault.py")
# /mnt is a CIFS mount with an attribute cache: a fresh write can take a few
# seconds to show there. Poll this long before calling it a failure.
VERIFY_WAIT_S = 30
# A write time further than this from now is reported as a failure: GAMESYNC's
# resume test is size AND mtime, so a stale stamp can hide an edit forever.
MTIME_SLACK_S = 15 * 60


def share_parts(rel):
    """'Files/Games-Library/X/y.bat' -> (['Files','Games-Library','X'], 'y.bat')."""
    rel = rel.replace("\\", "/").strip("/")
    if not rel or ".." in rel.split("/"):
        raise ValueError("share path must be relative to the share root, no '..': %r" % rel)
    parts = rel.split("/")
    return parts[:-1], parts[-1]


def smb_quote(s):
    # smbclient's -c parser splits on ';' and whitespace; double quotes group.
    if '"' in s or ";" in s:
        raise ValueError("unsupported character in share path: %r" % s)
    return '"%s"' % s


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Auth:
    def __enter__(self):
        def get(name):
            r = subprocess.run([sys.executable, KEYVAULT, "get", name],
                               capture_output=True, text=True)
            if r.returncode != 0 or not r.stdout.strip():
                raise SystemExit("keyvault: could not read %s" % name)
            return r.stdout.strip()
        user = get("fleet-nas-192-168-1-122-user")
        pw = get("fleet-nas-192-168-1-122-password")
        fd, self.path = tempfile.mkstemp(prefix="nasauth-")
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write("username=%s\npassword=%s\n" % (user, pw))
        return self.path

    def __exit__(self, *a):
        try:
            os.unlink(self.path)
        except OSError:
            pass


def smb(auth, commands):
    r = subprocess.run(["smbclient", SERVER, "-A", auth, "-c", commands],
                       capture_output=True, text=True)
    out = (r.stdout + r.stderr)
    # never echo anything that could carry the password
    out = "\n".join(l for l in out.splitlines() if "password" not in l.lower())
    return r.returncode, out


def wait_md5(mnt_path, size, want, wait_s=VERIFY_WAIT_S):
    """Poll the /mnt copy until it reads back as `want`; return what it read."""
    got = None
    deadline = time.time() + wait_s
    while True:
        try:
            if os.path.getsize(mnt_path) == size:
                got = md5_of(mnt_path)
                if got == want:
                    return got
        except OSError:
            pass
        if time.time() >= deadline:
            return got
        time.sleep(2)


def verify_landed(mnt_path, size, want, log=print):
    """The post-condition of a write, read through /mnt: bytes AND a fresh mtime.
    Returns 0 or a non-zero code (3 = wrong bytes, 4 = stale write time)."""
    got = wait_md5(mnt_path, size, want)
    if got != want:
        log("FAILED: %s reads back as %s (want %s)" % (mnt_path, got, want))
        return 3
    # GAMESYNC compares size AND mtime, so a stale stamp can hide an edit forever
    age = time.time() - os.path.getmtime(mnt_path)
    if abs(age) > MTIME_SLACK_S:
        log("FAILED: %s has write time %s (%.0f s from now) - GAMESYNC may skip it"
            % (mnt_path, time.ctime(os.path.getmtime(mnt_path)), age))
        return 4
    return 0


def put(local, dest, auth, dry_run=False, log=print):
    """Write ONE local file to share path `dest` and prove it landed.

    `auth` is the path of an smbclient auth file (an open Auth()). Returns 0
    only when the md5 read back through /mnt matches and the write time is
    fresh; the caller must treat anything else as the end of the run."""
    if not os.path.isfile(local):
        log("FAILED: no such local file: %s" % local)
        return 1
    dirs, name = share_parts(dest)
    want = md5_of(local)
    size = os.path.getsize(local)
    mnt_path = os.path.join(MNT, *dirs, name)
    log("put %s (%d B, md5 %s) -> %s" % (local, size, want, "/".join(dirs + [name])))
    if dry_run:
        log("dry-run: nothing written")
        return 0
    # create each parent (a collision on an existing one is fine)
    path = ""
    mk = []
    for d in dirs:
        path = d if not path else path + "/" + d
        mk.append("mkdir %s" % smb_quote(path))
    cd = "cd %s" % smb_quote("/".join(dirs)) if dirs else ""
    seq = "; ".join(mk + ([cd] if cd else []) +
                    ["put %s %s" % (smb_quote(local), smb_quote(name)),
                     "allinfo %s" % smb_quote(name)])
    rc, out = smb(auth, seq)
    put_ok = "putting file" in out and "NT_STATUS_ACCESS_DENIED" not in out
    if not put_ok:
        log(out)
        log("FAILED: smbclient did not report the put")
        return 2
    rc = verify_landed(mnt_path, size, want, log)
    if rc == 0:
        log("OK: md5 verified through %s, mtime %s"
            % (MNT, time.ctime(os.path.getmtime(mnt_path))))
    return rc


def rm(dest, auth, dry_run=False, log=print):
    """Delete ONE share file; 0 only once /mnt no longer shows it."""
    dirs, name = share_parts(dest)
    mnt_path = os.path.join(MNT, *dirs, name)
    log("rm %s" % "/".join(dirs + [name]))
    if dry_run:
        log("dry-run: nothing deleted")
        return 0
    cd = "cd %s; " % smb_quote("/".join(dirs)) if dirs else ""
    rc, out = smb(auth, cd + "del %s" % smb_quote(name))
    return _wait_gone(mnt_path, out, log)


def rmdir(dest, auth, dry_run=False, log=print):
    """Remove ONE empty share directory; 0 only once /mnt no longer shows it."""
    dirs, name = share_parts(dest)
    mnt_path = os.path.join(MNT, *dirs, name)
    log("rmdir %s" % "/".join(dirs + [name]))
    if dry_run:
        log("dry-run: nothing removed")
        return 0
    rc, out = smb(auth, "rmdir %s" % smb_quote("/".join(dirs + [name])))
    return _wait_gone(mnt_path, out, log)


def _wait_gone(mnt_path, out, log):
    deadline = time.time() + VERIFY_WAIT_S
    while time.time() < deadline:
        if not os.path.lexists(mnt_path):
            log("OK: gone (verified through %s)" % MNT)
            return 0
        time.sleep(2)
    log(out)
    log("FAILED: %s still exists" % mnt_path)
    return 3


def cmd_put(a):
    if not os.path.isfile(a.local):
        raise SystemExit("no such local file: %s" % a.local)
    if a.dry_run:
        return put(a.local, a.dest, None, dry_run=True)
    with Auth() as auth:
        return put(a.local, a.dest, auth)


def cmd_rm(a):
    if a.dry_run:
        return rm(a.dest, None, dry_run=True)
    with Auth() as auth:
        return rm(a.dest, auth)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("put")
    p.add_argument("local")
    p.add_argument("dest", help="path relative to \\\\192.168.1.122\\files")
    p.add_argument("--dry-run", action="store_true")
    r = sub.add_parser("rm")
    r.add_argument("dest")
    r.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    return cmd_put(a) if a.cmd == "put" else cmd_rm(a)


if __name__ == "__main__":
    sys.exit(main())
