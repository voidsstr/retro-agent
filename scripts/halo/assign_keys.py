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
sys.path.insert(0, HERE)
from boxkey import boxkey_path, dpid_fp_from_reg   # noqa: E402

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
    blob = reg_text.encode("latin-1")
    try:
        await c.send_command("UPLOAD C:\\halokey.reg", binary_payload=blob)
        await c.command_text("EXEC cmd /c regedit /s C:\\halokey.reg", timeout=30.0)
        await c.command_text("EXEC cmd /c del /q C:\\halokey.reg", timeout=20.0)
        # VERIFY THE POST-CONDITION, never the return value: read the blob back
        o = await c.command_text(
            "REGREAD HKLM SOFTWARE\\Microsoft\\Microsoft Games\\Halo", timeout=25.0)
        n = fp = None
        for v in json.loads(o).get("values", []):
            if v.get("name", "").lower() == "digitalproductid":
                h = re.sub(r"[^0-9a-fA-F]", "", str(v.get("data", ""))).lower()
                n, fp = len(h) // 2, hashlib.sha256(h.encode()).hexdigest()[:10]
        # The BOX-LOCAL copy the launcher re-applies at every start, because
        # the next GAMESYNC re-merges install.reg's single key over the value
        # just written (boxkey.py). Verified by reading the bytes back.
        local = False
        try:
            path = await boxkey_path(c)
            await c.send_command("MKDIR " + path.rsplit("\\", 1)[0])
            await c.send_command("UPLOAD " + path, binary_payload=blob)
            back = await c.command_binary("DOWNLOAD " + path, timeout=30.0)
            local = back == blob and dpid_fp_from_reg(back.decode("latin-1")) == fp
        except Exception:
            local = False
        return n, fp, local
    finally:
        try:
            await c.close()          # graceful: an abrupt close crashes Win98
        except Exception:
            pass


MAP_DEFAULT = os.path.join(HERE, "box-keys.txt")
LIBRARY_REG = "/mnt/retro-share/Files/Games-Library/Halo/install.reg"


def load_map(path):
    """{HOSTNAME: vault secret name}. A malformed line, or one secret named
    twice, is an ERROR: two boxes on one key is the failure this prevents."""
    out, seen = {}, {}
    with open(path) as f:
        for n, ln in enumerate(f, 1):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split("\t")
            if len(parts) != 2 or not parts[1].startswith("fleet-gamekey-halo-pc"):
                raise SystemExit("%s:%d: want <hostname> TAB <fleet-gamekey-halo-pc*>"
                                 % (path, n))
            host, name = parts[0].strip().upper(), parts[1].strip()
            if name in seen:
                raise SystemExit("%s:%d: %s is already %s's key - two boxes on "
                                 "one key cannot be in one game"
                                 % (path, n, name, seen[name]))
            seen[name] = host
            out[host] = name
    return out


def vault_get(name):
    """A key from the vault, kept in memory: never argv, never printed."""
    r = subprocess.run([sys.executable, os.path.join(REPO, "scripts", "fleet",
                                                     "keyvault.py"), "get", name],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not r.stdout.strip():
        raise SystemExit("cannot read %s from the vault" % name)
    return r.stdout.strip()


async def box_hostname(ip, secret):
    from client.retro_protocol import RetroConnection
    c = RetroConnection(ip, 9898)
    await c.connect(secret, timeout=20.0)
    try:
        out = await c.command_text("EXEC hostname", timeout=20.0)
        return out.strip().splitlines()[-1].strip().upper() if out.strip() else ""
    finally:
        try:
            await c.close()
        except Exception:
            pass


def main():
    import asyncio
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys-file", help="one 25-character Halo key per line")
    ap.add_argument("--map", nargs="?", const=MAP_DEFAULT,
                    help="assign each box the vaulted key box-keys.txt names "
                         "for its HOSTNAME (default file: %(const)s)")
    ap.add_argument("--boxes", required=True, help="comma-separated IPs")
    ap.add_argument("--secret", default="retro-agent-secret")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if bool(a.keys_file) == bool(a.map):
        ap.error("give exactly one of --keys-file or --map")

    boxes = [b.strip() for b in a.boxes.split(",") if b.strip()]
    if a.map:
        mp = load_map(a.map)
        keys, mapped = [], []
        for ip in boxes:
            try:
                host = asyncio.run(box_hostname(ip, a.secret))
            except Exception as e:
                print("  %-16s unreachable (%s) - left alone" % (ip, type(e).__name__))
                continue
            if host not in mp:
                print("  %-16s %s is not in %s - left alone" % (ip, host or "?", a.map))
                continue
            keys.append(vault_get(mp[host]))
            mapped.append(ip)
            print("  %-16s %s -> %s" % (ip, host, mp[host]))
        boxes = mapped
    else:
        with open(a.keys_file) as f:
            keys = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]

    # The library's own key is what every box WITHOUT a key of its own plays
    # on, so handing it to a box as "its own" shares it with all of those.
    try:
        lib_fp = dpid_fp_from_reg(open(LIBRARY_REG, "rb").read().decode("latin-1"))
    except OSError:
        lib_fp = None
    for k in keys:
        if lib_fp and dpid_fp_from_reg(build_reg(k)) == lib_fp:
            raise SystemExit("a key in the list IS the library's install.reg key "
                             "(fingerprint %s) - every unassigned box already "
                             "plays on it; refusing" % fingerprint(k))

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

    for ip, key in zip(boxes, keys):
        reg = build_reg(key)
        if a.dry_run:
            print("  %-16s would get key %s" % (ip, fingerprint(key)))
            continue
        try:
            n, fp, local = asyncio.run(apply(ip, reg, a.secret))
        except Exception as e:
            print("  %-16s FAILED: %s" % (ip, str(e)[:60]))
            continue
        ok = n == 164
        print("  %-16s key %s -> DPID %s bytes  %s  (audit fp %s)  box-local copy %s"
              % (ip, fingerprint(key), n, "OK" if ok else "*** WRONG SIZE ***",
                 fp, "OK" if local else
                 "*** MISSING - the next GAMESYNC reverts this box to the "
                 "library key ***"))


if __name__ == "__main__":
    main()
