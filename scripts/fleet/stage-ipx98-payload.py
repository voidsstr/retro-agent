#!/usr/bin/env python3
"""Publish the Windows 98 SE IPX/SPX payload that IPXSETUP installs on Win9x boxes.

IPXSETUP (agent 1.97.0, agent/src/ipx9x.c) copies NWLINK.VXD and WSIPX.VXD into
a Win98 SE box's SYSTEM folder from

    \\\\192.168.1.122\\files\\Utility\\Retro Automation\\ipx\\win98se\\

(HKLM\\Software\\RetroAgent\\IpxPayloadPath overrides it). Neither file is on a
fleet box today, and the Win98 build VM has none either: they live only in the
Windows 98 SE cabinets - nwlink.vxd in NET9.CAB, wsipx.vxd in NET10.CAB
(LAYOUT.INF; R3_IPX_install.md section 1.1). This script

  1. extracts both from the Windows 98 SE ISO on the share (read-only: 7z pulls
     the cabinets out, cabextract opens them - the set spans, so the neighbours
     come too);
  2. checks each against the manifest COMPILED INTO THE AGENT
     (agent/shared/ipxplan.h ipx9x_payload[] - size, CRC-32 and md5), so the
     share can never offer a build the agent would refuse;
  3. writes each through scripts/fleet/sharewrite.py - ONE put per file, read
     back through /mnt (md5 and a fresh mtime) before the next - and
     MANIFEST.TXT LAST: the agent treats the folder as reachable only once the
     manifest is there, so a half-published folder is never used.

The destination is under Utility\\, NOT Games-Library\\, so no autodeploy resync
is triggered (retro-autodeploy watches the library root only).

    python3 scripts/fleet/stage-ipx98-payload.py            # extract, verify, publish
    python3 scripts/fleet/stage-ipx98-payload.py --check    # is the share current? (read-only)
    python3 scripts/fleet/stage-ipx98-payload.py --dry-run  # extract + verify, write nothing

Exit 0 only when every file on the share is the manifest's build.
"""
import argparse
import hashlib
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, "agent", "shared", "ipxplan.h")
ISO = "/mnt/retro-share/DiskImages/Windows 98 SE/faXcooL_win_98_se_bootable.iso"
MNT = "/mnt/retro-share"
DEST = "Utility/Retro Automation/ipx/win98se"
MANIFEST = "MANIFEST.TXT"
# Which cabinet each file is in (Win98 SE LAYOUT.INF: nwlink.vxd=9, wsipx.vxd=10),
# plus the neighbours a spanned file may continue into.
CAB_OF = {"NWLINK.VXD": "NET9.CAB", "WSIPX.VXD": "NET10.CAB"}
CAB_SET = ("NET8.CAB", "NET9.CAB", "NET10.CAB")


def manifest_from_header(path=HEADER):
    """[(name, size, crc32, md5)] - ipx9x_payload[] exactly as the agent compiles it."""
    text = open(path).read()
    block = text[text.index("static const ipx_file_t ipx9x_payload[] = {"):]
    block = block[:block.index("};")]
    rows = re.findall(r'\{\s*"([^"]+)",\s*(\d+)UL,\s*0x([0-9A-Fa-f]+)UL,\s*"([0-9a-f]{32})"\s*\}', block)
    if not rows:
        raise SystemExit("could not read ipx9x_payload[] from %s" % path)
    return [(n, int(s), int(c, 16), m) for n, s, c, m in rows]


def check_dest(dest):
    """Never the game library: a new top-level directory there resyncs the fleet."""
    d = dest.replace("\\", "/").strip("/").lower()
    if d.startswith("files/games-library") or "games-library" in d.split("/"):
        raise SystemExit("REFUSED: %s is inside Games-Library - a write there can trigger a "
                         "fleet-wide GAMESYNC (retro-autodeploy)" % dest)
    return dest


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def crc_of(path):
    with open(path, "rb") as f:
        return zlib.crc32(f.read()) & 0xFFFFFFFF


def verify(path, size, crc, md5):
    """None, or why the file is not the manifest's build."""
    if not os.path.isfile(path):
        return "missing"
    got = (os.path.getsize(path), crc_of(path), md5_of(path))
    if got != (size, crc, md5):
        return "is %d B crc %08X md5 %s, want %d B crc %08X md5 %s" % (got + (size, crc, md5))
    return None


def manifest_text(files):
    lines = [
        "; IPX/SPX payload for IPXSETUP on Windows 98 SE (agent 1.97.0, agent/src/ipx9x.c).",
        "; Extracted from the Windows 98 SE CD (nwlink.vxd NET9.CAB, wsipx.vxd NET10.CAB)",
        "; by scripts/fleet/stage-ipx98-payload.py. The agent copies these into SYSTEM",
        "; only if size and CRC-32 match the manifest compiled into it (ipxplan.h).",
        "; name size crc32 md5",
    ]
    for n, s, c, m in files:
        lines.append("%s %d %08X %s" % (n, s, c, m))
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def extract(work, files, log=print):
    """Extract each payload file into work/out; return {name: path}."""
    if not os.path.isfile(ISO):
        raise SystemExit("the Windows 98 SE ISO is not readable: %s" % ISO)
    for tool in ("7z", "cabextract"):
        if not shutil.which(tool):
            raise SystemExit("%s is not installed" % tool)
    cabs = os.path.join(work, "cabs")
    out = os.path.join(work, "out")
    os.makedirs(cabs)
    os.makedirs(out)
    r = subprocess.run(["7z", "e", "-y", "-o" + cabs, ISO] + ["win98/" + c for c in CAB_SET],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("7z could not extract the cabinets:\n" + r.stdout[-800:] + r.stderr[-800:])
    got = {}
    for name, _s, _c, _m in files:
        cab = os.path.join(cabs, CAB_OF[name])
        r = subprocess.run(["cabextract", "-q", "-F", name.lower(), "-d", out, cab],
                           capture_output=True, text=True)
        cands = [f for f in os.listdir(out) if f.lower() == name.lower()]
        if r.returncode != 0 or not cands:
            raise SystemExit("cabextract found no %s in %s:\n%s" % (name, CAB_OF[name], r.stderr[-600:]))
        src = os.path.join(out, cands[0])
        dst = os.path.join(out, name + ".ok")
        os.rename(src, dst)
        got[name] = dst
        log("extracted %s from %s (%d B)" % (name, CAB_OF[name], os.path.getsize(dst)))
    return got


def load_sharewrite():
    spec = importlib.util.spec_from_file_location("sharewrite", os.path.join(HERE, "sharewrite.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def share_state(files, dest=DEST, mnt=MNT):
    """{name: None (current) | why} for every file, MANIFEST.TXT included."""
    base = os.path.join(mnt, *dest.split("/"))
    state = {}
    for n, s, c, m in files:
        state[n] = verify(os.path.join(base, n), s, c, m)
    want = manifest_text(files)
    mpath = os.path.join(base, MANIFEST)
    if not os.path.isfile(mpath):
        state[MANIFEST] = "missing"
    elif open(mpath, "rb").read() != want:
        state[MANIFEST] = "differs from the manifest this script would write"
    else:
        state[MANIFEST] = None
    return state


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="read-only: is the share current?")
    ap.add_argument("--dry-run", action="store_true", help="extract and verify, write nothing")
    a = ap.parse_args(argv)
    check_dest(DEST)
    files = manifest_from_header()

    if a.check:
        if not os.path.isdir(MNT):
            print("SKIPPED: %s is not mounted - the share was NOT checked" % MNT)
            return 2
        st = share_state(files)
        for n, why in st.items():
            print("%-12s %s" % (n, "current" if why is None else "NOT CURRENT: " + why))
        return 0 if all(v is None for v in st.values()) else 1

    work = tempfile.mkdtemp(prefix="ipx98-")
    try:
        got = extract(work, files)
        for n, s, c, m in files:
            why = verify(got[n], s, c, m)
            if why:
                print("REFUSED: %s from the ISO %s - the agent would refuse it too" % (n, why))
                return 1
            print("verified %s %d B crc %08X md5 %s" % (n, s, c, m))
        mlocal = os.path.join(work, MANIFEST)
        with open(mlocal, "wb") as f:
            f.write(manifest_text(files))
        if a.dry_run:
            print("dry-run: nothing written to the share")
            return 0
        st = share_state(files)
        sw = load_sharewrite()
        with sw.Auth() as auth:
            # the VxDs first, MANIFEST.TXT LAST - one put, one read-back, each
            for n, _s, _c, _m in files:
                if st[n] is None:
                    print("%s already current on the share - not rewritten" % n)
                    continue
                rc = sw.put(got[n], "%s/%s" % (DEST, n), auth)
                if rc != 0:
                    print("*** PUBLISH FAILED *** %s (sharewrite rc %d) - stopping" % (n, rc))
                    return 1
            if st[MANIFEST] is None:
                print("%s already current on the share - not rewritten" % MANIFEST)
            else:
                rc = sw.put(mlocal, "%s/%s" % (DEST, MANIFEST), auth)
                if rc != 0:
                    print("*** PUBLISH FAILED *** %s (sharewrite rc %d)" % (MANIFEST, rc))
                    return 1
        st = share_state(files)
        bad = {n: w for n, w in st.items() if w is not None}
        if bad:
            print("*** NOT CURRENT AFTER PUBLISH *** %s" % bad)
            return 1
        print("OK: %s holds %s, verified through %s"
              % (DEST, ", ".join([n for n, *_ in files] + [MANIFEST]), MNT))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
