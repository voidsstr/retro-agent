#!/usr/bin/env python3
"""Give each fleet box its OWN Halo CD key - one player per key.

WHY THIS EXISTS (measured 2026-08-31, not inferred)
---------------------------------------------------
Halo PC allows **one simultaneous player per CD key**, and it reports the
second machine's rejection as

    ATTENTION
    Your CD Key is invalid.

which is the same wording it uses for a key that is genuinely bad. That
collision cost real time: the error was read as "this key is wrong", the fleet's
key was replaced, and the problem moved rather than went away.

The experiment that settled it, on one server (.246:2302) and one key:

    .145 joins alone .......................... IN-GAME
    .240 joins while .145 is connected ........ "Your CD Key is invalid"
    .145 disconnected, .240 joins alone ....... IN-GAME

Same key, same box, same server. The only variable was whether another machine
was already using that key. **Halo has no "key already in use" string at all** -
searched every binary in the tree - so absence of that message is not evidence
that the check does not exist. The rejection comes from the server and reuses
the generic text.

So: to have N machines in one Halo game you need N DISTINCT keys. This script
takes a list of keys and assigns them one-to-one to boxes, building each
machine's DigitalProductID with make_dpid.py and verifying the value that
actually landed.

    python3 scripts/halo/assign_keys.py --keys-file keys.txt \
            --boxes 192.168.1.145,192.168.1.240,192.168.1.123

`keys.txt` is one 25-character key per line. It is read, used and never echoed;
nothing here prints a key, and the summary identifies each only by a short
fingerprint so two boxes can be compared without exposing either.

REFUSES to assign the same key to two boxes, because that is precisely the
configuration that produces the misleading error above.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)

MAKE_DPID = os.path.join(HERE, "make_dpid.py")


def fingerprint(s):
    return hashlib.sha256(s.strip().upper().replace("-", "").encode()).hexdigest()[:10]


def build_reg(key):
    """Return the REGEDIT4 stanza for one key. The key never reaches argv."""
    import tempfile
    fd, path = tempfile.mkstemp(prefix="halokey-", suffix=".txt")
    try:
        os.write(fd, key.strip().encode())
        os.close(fd)
        os.chmod(path, 0o600)
        r = subprocess.run([sys.executable, MAKE_DPID, "--key-file", path, "--reg"],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            raise SystemExit("make_dpid.py failed: %s" % r.stderr.strip()[:200])
        return r.stdout
    finally:
        try:
            with open(path, "wb") as f:          # overwrite before unlinking
                f.write(b"\0" * 64)
            os.unlink(path)
        except OSError:
            pass


async def apply(ip, reg_text, secret):
    from client.retro_protocol import RetroConnection
    c = RetroConnection(ip, 9898)
    await c.connect(secret, timeout=20.0)
    try:
        await c.send_command("UPLOAD C:\\halokey.reg",
                             binary_payload=reg_text.encode("latin-1"))
        await c.command_text("EXEC cmd /c regedit /s C:\\halokey.reg", timeout=30.0)
        await c.command_text("EXEC cmd /c del /q C:\\halokey.reg", timeout=20.0)
        # VERIFY THE POST-CONDITION, never the return value: read the blob back
        o = await c.command_text(
            "REGREAD HKLM SOFTWARE\\Microsoft\\Microsoft Games\\Halo", timeout=25.0)
        for v in json.loads(o).get("values", []):
            if v.get("name", "").lower() == "digitalproductid":
                h = re.sub(r"[^0-9a-fA-F]", "", str(v.get("data", ""))).lower()
                return len(h) // 2, hashlib.sha256(h.encode()).hexdigest()[:10]
        return None, None
    finally:
        try:
            await c.close()          # graceful: an abrupt close crashes Win98
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys-file", required=True,
                    help="one 25-character Halo key per line")
    ap.add_argument("--boxes", required=True, help="comma-separated IPs")
    ap.add_argument("--secret", default="retro-agent-secret")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    with open(a.keys_file) as f:
        keys = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    boxes = [b.strip() for b in a.boxes.split(",") if b.strip()]

    seen = {}
    for k in keys:
        fp = fingerprint(k)
        if fp in seen:
            raise SystemExit(
                "the key list contains a DUPLICATE (fingerprint %s). Two boxes "
                "sharing a key is exactly what produces the misleading "
                "'Your CD Key is invalid' on the second one - refusing." % fp)
        seen[fp] = k

    if len(keys) < len(boxes):
        print("NOTE: %d key(s) for %d box(es). Only the first %d boxes can be in "
              "a Halo game AT THE SAME TIME; the rest will be left alone rather "
              "than given a duplicate." % (len(keys), len(boxes), len(keys)),
              file=sys.stderr)
        boxes = boxes[:len(keys)]

    import asyncio
    for ip, key in zip(boxes, keys):
        reg = build_reg(key)
        if a.dry_run:
            print("  %-16s would get key %s" % (ip, fingerprint(key)))
            continue
        try:
            n, fp = asyncio.run(apply(ip, reg, a.secret))
        except Exception as e:
            print("  %-16s FAILED: %s" % (ip, str(e)[:60]))
            continue
        ok = n == 164
        print("  %-16s key %s -> DPID %s bytes  %s"
              % (ip, fingerprint(key), n, "OK" if ok else "*** WRONG SIZE ***"))


if __name__ == "__main__":
    main()
