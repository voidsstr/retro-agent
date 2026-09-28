#!/usr/bin/env python3
"""Build and publish the driver-store index the agent's DRIVERS UPDATE reads.

The store is the image's driver tree ($OEM$\\$1\\D on the share - the same
tree a fresh box gets as C:\\D). A box whose C:\\D was reclaimed, or that was
never PXE-imaged, can still be served from it, but it must not scan 3,700 INFs
over SMB. So:

  1. scripts/fleet/drvindex.c - the agent's OWN matcher (agent/shared/
     drvmatch.h) and 3dfx rule (drvsafe.h), compiled for the host - lists
     every (bucket, id, dir\\inf, DriverVer) the store serves;
  2. this script groups the lines by bucket (agent/shared/drvstore.h) into
     DRVINDEX\\<bucket>.TXT plus DRVINDEX\\MANIFEST.TXT, OUTSIDE the $OEM$
     tree so imaging is untouched;
  3. publishes them file by file, verifying each through /mnt before the next
     (gvfs and smbclient both have silently lost files on this NAS).

    python3 scripts/fleet/driverstore.py build            # into a local dir
    python3 scripts/fleet/driverstore.py publish          # build + publish
    python3 scripts/fleet/driverstore.py check            # is the share current?
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MNT = Path("/mnt/retro-share")
STORE_REL = "Files/OS/XPSP3-FLEET/$OEM$/$1/D"
INDEX_REL = "Files/OS/XPSP3-FLEET/DRVINDEX"
KEYVAULT = ROOT / "scripts" / "fleet" / "keyvault.py"


def build_tool(out_dir):
    exe = Path(out_dir) / "drvindex"
    cc = os.environ.get("CC", "gcc")
    subprocess.run([cc, "-O2", "-w", "-o", str(exe), str(ROOT / "scripts" / "fleet" / "drvindex.c")], check=True)
    return exe


def group(lines):
    """bucket -> sorted unique lines 'ID\\tDIR\\INF\\tVER'; plus the 3dfx skips."""
    buckets, skipped = defaultdict(set), []
    for ln in lines:
        if not ln.strip():
            continue
        if ln.startswith("#skip3dfx"):
            skipped.append(ln.split("\t", 1)[1])
            continue
        parts = ln.split("\t")
        if len(parts) != 4:
            continue
        bucket, hwid, inf, ver = parts
        buckets[bucket].add(f"{hwid}\t{inf}\t{ver}")
    return {b: sorted(v) for b, v in buckets.items()}, sorted(skipped)


def build(out_dir):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = build_tool(out_dir)
    res = subprocess.run([str(exe), str(MNT / STORE_REL)], check=True, capture_output=True)
    buckets, skipped = group(res.stdout.decode("latin-1").splitlines())
    files = {}
    for b, lines in buckets.items():
        files[f"{b}.TXT"] = ("\r\n".join(lines) + "\r\n").encode("latin-1")
    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    manifest = [f"# driver-store index built from {STORE_REL} by drvindex.c at {commit}",
                "# bucket<TAB>lines   (agent/shared/drvstore.h names the bucket)"]
    manifest += [f"{b}\t{len(lines)}" for b, lines in sorted(buckets.items())]
    manifest += [f"#skip3dfx\t{s}" for s in skipped]
    files["MANIFEST.TXT"] = ("\r\n".join(manifest) + "\r\n").encode("latin-1")
    for name, data in files.items():
        (out_dir / name).write_bytes(data)
    return files


def _auth_file():
    fd, path = tempfile.mkstemp(prefix="nasauth.")
    os.close(fd)
    os.chmod(path, 0o600)
    user = subprocess.run([sys.executable, str(KEYVAULT), "get", "fleet-nas-192-168-1-122-user"],
                          capture_output=True, text=True, check=True).stdout.strip()
    pw = subprocess.run([sys.executable, str(KEYVAULT), "get", "fleet-nas-192-168-1-122-password"],
                        capture_output=True, text=True, check=True).stdout.strip()
    Path(path).write_text(f"username = {user}\npassword = {pw}\n")
    return path


def publish(files, local_dir):
    auth = _auth_file()
    bad = []
    try:
        remote_dir = INDEX_REL.replace("/", "\\")
        subprocess.run(["smbclient", "//192.168.1.122/files", "-A", auth, "-m", "SMB3",
                        "-c", f'mkdir "{remote_dir}"'], capture_output=True)
        order = sorted(n for n in files if n != "MANIFEST.TXT") + ["MANIFEST.TXT"]   # manifest last
        for name in order:
            want = hashlib.md5(files[name]).hexdigest()
            ok = False
            for _ in range(3):
                subprocess.run(["smbclient", "//192.168.1.122/files", "-A", auth, "-m", "SMB3", "-c",
                                f'cd "{remote_dir}"; put "{Path(local_dir) / name}" "{name}"'],
                               capture_output=True)
                try:
                    got = hashlib.md5((MNT / INDEX_REL / name).read_bytes()).hexdigest()
                except OSError:
                    got = None
                if got == want:
                    ok = True
                    break
            print(("OK   " if ok else "FAIL ") + name)
            if not ok:
                bad.append(name)
    finally:
        os.unlink(auth)
    return bad


def check(files):
    stale = []
    for name, data in files.items():
        p = MNT / INDEX_REL / name
        if not p.exists() or p.read_bytes() != data:
            stale.append(name)
    return stale


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["build", "publish", "check"])
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    out = a.out or tempfile.mkdtemp(prefix="drvindex.")
    files = build(out)
    total = sum(d.count(b"\n") for n, d in files.items() if n != "MANIFEST.TXT")
    print(f"{len(files) - 1} buckets, {total} lines, in {out}")
    if a.action == "publish":
        bad = publish(files, out)
        print("published" if not bad else f"FAILED: {bad}")
        return 1 if bad else 0
    if a.action == "check":
        stale = check(files)
        print("share is current" if not stale else f"STALE: {stale}")
        return 1 if stale else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
