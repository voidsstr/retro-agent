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


def cmd_put(a):
    if not os.path.isfile(a.local):
        raise SystemExit("no such local file: %s" % a.local)
    dirs, name = share_parts(a.dest)
    want = md5_of(a.local)
    size = os.path.getsize(a.local)
    mnt_path = os.path.join(MNT, *dirs, name)
    print("put %s (%d B, md5 %s) -> %s" % (a.local, size, want, "/".join(dirs + [name])))
    if a.dry_run:
        print("dry-run: nothing written")
        return 0
    with Auth() as auth:
        # create each parent (a collision on an existing one is fine)
        path = ""
        mk = []
        for d in dirs:
            path = d if not path else path + "/" + d
            mk.append("mkdir %s" % smb_quote(path))
        cd = "cd %s" % smb_quote("/".join(dirs)) if dirs else ""
        seq = "; ".join(mk + ([cd] if cd else []) +
                        ["put %s %s" % (smb_quote(a.local), smb_quote(name)),
                         "allinfo %s" % smb_quote(name)])
        rc, out = smb(auth, seq)
    put_ok = "putting file" in out and "NT_STATUS_ACCESS_DENIED" not in out
    if not put_ok:
        print(out)
        print("FAILED: smbclient did not report the put")
        return 2
    # post-condition 1: the bytes, read back through the OTHER mount
    got = None
    deadline = time.time() + VERIFY_WAIT_S
    while time.time() < deadline:
        try:
            if os.path.getsize(mnt_path) == size:
                got = md5_of(mnt_path)
                if got == want:
                    break
        except OSError:
            pass
        time.sleep(2)
    if got != want:
        print("FAILED: %s reads back as %s (want %s)" % (mnt_path, got, want))
        return 3
    # post-condition 2: a fresh write time (GAMESYNC compares size AND mtime)
    age = time.time() - os.path.getmtime(mnt_path)
    if abs(age) > MTIME_SLACK_S:
        print("FAILED: %s has write time %s (%.0f s from now) - GAMESYNC may skip it"
              % (mnt_path, time.ctime(os.path.getmtime(mnt_path)), age))
        return 4
    print("OK: md5 verified through %s, mtime %s" % (MNT, time.ctime(os.path.getmtime(mnt_path))))
    return 0


def cmd_rm(a):
    dirs, name = share_parts(a.dest)
    mnt_path = os.path.join(MNT, *dirs, name)
    print("rm %s" % "/".join(dirs + [name]))
    if a.dry_run:
        print("dry-run: nothing deleted")
        return 0
    with Auth() as auth:
        cd = "cd %s; " % smb_quote("/".join(dirs)) if dirs else ""
        rc, out = smb(auth, cd + "del %s" % smb_quote(name))
    deadline = time.time() + VERIFY_WAIT_S
    while time.time() < deadline:
        if not os.path.exists(mnt_path):
            print("OK: gone (verified through %s)" % MNT)
            return 0
        time.sleep(2)
    print(out)
    print("FAILED: %s still exists" % mnt_path)
    return 3


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
