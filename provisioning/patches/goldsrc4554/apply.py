#!/usr/bin/env python3
"""GoldSrc build 4554 client: skip the client.dll hash drop for non-Valve mods.

Why (measured 2026-11-23, see README.md): the staged CounterStrike16 tree's
engine (hw.dll / sw.dll, No-Steam build 4554, protocol 48) compares, for every
mod EXCEPT valve/cstrike/cstrike_beta/czero/tfc/dod/ricochet/dmc, the MD5 of its
own cl_dlls\\client.dll with the 16 bytes the server sends in svc_serverinfo,
and disconnects itself on a mismatch ("Your .dll [cl_dlls\\client.dll] differs
from the server's.", server log: "Client sent 'drop'"). The fleet's TS server
is ReHLDS (anniversary HLDS refuses ts_i386.so), and ReHLDS never hashes
client.dll - strace of a run with a client connecting shows no open of it - so
the client is compared against an empty hash and The Specialists can never be
joined, although the client and server client.dll are byte-identical.

The edit: the `je` that skips the drop when memcmp() returned 0 becomes an
unconditional `jmp` (0x74 -> 0xEB, one byte per file). The Valve mods never
reach that code (the strcmp chain above it jumps past it), so CS 1.6 and HLDM
are unaffected; verified in the build VM on 2026-11-23 (CS joins :27015, TS
joins :27017 and spawns).

Modes (exactly one):
  --check            verify the staged originals (size, md5, the exact bytes
                     around each edit); report. Accepts already-patched files.
  --build [OUTDIR]   write patched copies + manifest.json
                     (default ~/.retro-fleet/patch-out/goldsrc4554)
  --publish [OUTDIR] back up each original to
                     Files/Games-Library/_patches/CounterStrike16/originals-2026-11-23/
                     then put each patched file with scripts/fleet/sharewrite.py,
                     one at a time, stopping on the first failure. Idempotent:
                     a file already carrying the patched md5 is skipped.

Game binaries are copyrighted: nothing this script writes may be committed.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
LIB = "/mnt/retro-share/Files/Games-Library"
TITLE = "CounterStrike16"
BACKUP_REL = "_patches/%s/originals-2026-11-23" % TITLE
DEFAULT_OUT = os.path.expanduser("~/.retro-fleet/patch-out/goldsrc4554")

# file -> original size/md5, patched md5, offset of the je opcode, and the
# exact original bytes from the memcmp call to the push of the message.
EDITS = {
    "hw.dll": {
        "size": 1840440,
        "md5": "4903e8e80f2a00d3ab2886f0f209a6d6",
        "patched_md5": "bdebc8a36deb13b205fa5059587d2b37",
        "ctx_off": 0x281B8,
        "ctx": "e8d3e40000 83c40c 85c0 7449 a13497d202 85c0 752e a1749ad202 85c0 7525 8d542404 52 6870c2e601",
        "je_off": 0x281C2,
    },
    "sw.dll": {
        "size": 1672504,
        "md5": "05c1e82726cd5ab6518864f77947527c",
        "patched_md5": "dce2ba0978550854454a008984b405a1",
        "ctx_off": 0x27F67,
        "ctx": "e874db0000 83c40c 85c0 7447 a1548e6502 85c0 752d a194916502 85c0 7524 8d55c0 52 6838f2e401",
        "je_off": 0x27F71,
    },
}
JE, JMP = 0x74, 0xEB


class PatchError(Exception):
    pass


def md5(b):
    return hashlib.md5(b).hexdigest()


def ctx_bytes(spec):
    return bytes.fromhex(spec["ctx"].replace(" ", ""))


def patch(name, data):
    """Return (patched bytes, state) for one original; state is 'original' or
    'patched'. Raises PatchError for anything that is neither."""
    spec = EDITS[name]
    if len(data) != spec["size"]:
        raise PatchError("%s: size %d, expected %d" % (name, len(data), spec["size"]))
    h = md5(data)
    if h == spec["patched_md5"]:
        return bytes(data), "patched"
    if h != spec["md5"]:
        raise PatchError("%s: md5 %s is neither the original nor the patched build" % (name, h))
    ctx = ctx_bytes(spec)
    o = spec["ctx_off"]
    if data[o:o + len(ctx)] != ctx:
        raise PatchError("%s: bytes at 0x%x are not the measured ones" % (name, o))
    j = spec["je_off"]
    if data[j] != JE or not (o <= j < o + len(ctx)):
        raise PatchError("%s: no je at 0x%x" % (name, j))
    out = bytearray(data)
    out[j] = JMP
    if md5(out) != spec["patched_md5"]:
        raise PatchError("%s: patched md5 %s != pinned %s" % (name, md5(out), spec["patched_md5"]))
    return bytes(out), "original"


def read_lib(name):
    with open(os.path.join(LIB, TITLE, name), "rb") as f:
        return f.read()


def cmd_check():
    rc = 0
    for name, spec in EDITS.items():
        try:
            _, state = patch(name, read_lib(name))
            print("%s: %s (je @0x%x)" % (name, state, spec["je_off"]))
        except PatchError as e:
            print("FAIL", e)
            rc = 1
    return rc


def cmd_build(outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = {}
    for name in EDITS:
        orig = read_lib(name)
        out, state = patch(name, orig)
        if state == "patched":
            raise PatchError("%s on the share is already patched - nothing to build from" % name)
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(out)
        with open(os.path.join(outdir, name + ".orig"), "wb") as f:
            f.write(orig)
        manifest[name] = {"from": EDITS[name]["md5"], "to": md5(out), "je_off": EDITS[name]["je_off"]}
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print("built", outdir, manifest)
    return 0


def sharewrite_put(local, rel):
    r = subprocess.run([sys.executable, os.path.join(REPO, "scripts", "fleet", "sharewrite.py"),
                        "put", local, "Files/Games-Library/" + rel])
    if r.returncode != 0:
        raise PatchError("PUBLISH FAILED putting %s" % rel)


def cmd_publish(outdir):
    if not os.path.isdir(os.path.join(LIB, TITLE)):
        raise PatchError("%s is not mounted" % LIB)
    for name, spec in EDITS.items():
        live = read_lib(name)
        _, state = patch(name, live)
        if state == "patched":
            print("%s: already patched - skipped" % name)
            continue
        bak = os.path.join(LIB, *BACKUP_REL.split("/"), name)
        if not (os.path.isfile(bak) and md5(open(bak, "rb").read()) == spec["md5"]):
            with tempfile.TemporaryDirectory() as td:
                p = os.path.join(td, name)
                with open(p, "wb") as f:
                    f.write(live)
                sharewrite_put(p, "%s/%s" % (BACKUP_REL, name))
        built = os.path.join(outdir, name)
        if md5(open(built, "rb").read()) != spec["patched_md5"]:
            raise PatchError("%s: build output is stale - run --build" % built)
        sharewrite_put(built, "%s/%s" % (TITLE, name))
        if md5(read_lib(name)) != spec["patched_md5"]:
            raise PatchError("%s: share copy does not carry the patched md5 after the put" % name)
        print("%s: published" % name)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--build", nargs="?", const=DEFAULT_OUT)
    g.add_argument("--publish", nargs="?", const=DEFAULT_OUT)
    a = ap.parse_args(argv)
    try:
        if a.check:
            return cmd_check()
        if a.build:
            return cmd_build(a.build)
        return cmd_publish(a.publish)
    except PatchError as e:
        print("ERROR:", e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
