#!/usr/bin/env python3
"""Report which fleet boxes carry which Halo key - WITHOUT printing a key.

WHY THIS EXISTS
---------------
Halo PC allows ONE SIMULTANEOUS PLAYER PER CD KEY, and the host rejects the
second machine with the same generic "Your CD Key is invalid" that a genuinely
bad key produces (measured 2026-08-31 - see scripts/halo/assign_keys.py).  So
"two boxes share a key" is a live fault that presents as a licensing error, and
it is invisible unless somebody looks.

assign_keys.py refuses to CREATE a duplicate.  This is the other half: it reads
back what is actually on each machine, so a box that was re-imaged, restored
from a backup, or given a staged install.reg carrying the library's single key
is caught before someone spends an afternoon on "the key is wrong".

WHAT IT PRINTS
--------------
A fingerprint per box - the first 10 hex of SHA-256 over the DigitalProductID
blob - never a key and never the blob.  Two boxes with the same fingerprint
have the same key.  That is the whole report.

    python3 scripts/halo/audit_keys.py --boxes 192.168.1.123,192.168.1.145

With no --boxes it audits every box in the fleet roster that answers.  A box
that is switched off is reported as UNREACHABLE, never as "no key" - the fleet
is powered on demand and those two must never render the same.
"""
import argparse
import asyncio
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)

HALO_KEY = "SOFTWARE\\Microsoft\\Microsoft Games\\Halo"
DEFAULT_BOXES = ["192.168.1.123", "192.168.1.124", "192.168.1.133",
                 "192.168.1.143", "192.168.1.145", "192.168.1.171",
                 "192.168.1.240", "192.168.1.243", "192.168.1.246"]


async def read_box(ip, secret, timeout):
    """Return (state, fingerprint, installed, detail).

    state is one of ok / no-key / no-halo / unreachable.  `installed` is True
    only when halo.exe is actually on the box - a key and an install are
    separate facts, and conflating them turns a harmless leftover registry
    value into a reported fault (see the duplicate handling in main_async)."""
    from client.retro_protocol import RetroConnection
    c = RetroConnection(ip, 9898)
    try:
        await c.connect(secret, timeout=timeout)
    except Exception as e:
        return "unreachable", None, False, type(e).__name__
    try:
        try:
            probe = await c.command_text(
                "EXEC cmd /c if exist C:\\Games\\Halo\\halo.exe "
                "(echo HALO-PRESENT) else (echo HALO-ABSENT)", timeout=25.0)
            installed = "HALO-PRESENT" in probe
        except Exception:
            installed = False
        out = await c.command_text("REGREAD HKLM " + HALO_KEY, timeout=25.0)
        try:
            values = json.loads(out).get("values", [])
        except ValueError:
            # the key is absent -> the agent answers with an error, not JSON
            return "no-halo", None, installed, out.strip()[:60]
        for v in values:
            if v.get("name", "").lower() == "digitalproductid":
                hexs = re.sub(r"[^0-9a-fA-F]", "", str(v.get("data", ""))).lower()
                if not hexs:
                    return "no-key", None, installed, "DigitalProductID is empty"
                fp = hashlib.sha256(hexs.encode()).hexdigest()[:10]
                return "ok", fp, installed, "%d bytes" % (len(hexs) // 2)
        return ("no-key", None, installed,
                "%d value(s), no DigitalProductID" % len(values))
    except Exception as e:
        return "unreachable", None, False, type(e).__name__
    finally:
        try:
            await c.close()      # graceful: an abrupt close crashes Win98
        except Exception:
            pass


def classify_duplicates(by_fp):
    """Split shared keys into ones that MATTER and ones that are only stale.

    `by_fp` maps fingerprint -> [(ip, halo_installed), ...].

    A duplicate only matters between boxes that can actually be in a game
    together. A leftover DigitalProductID on a box with no Halo installed is
    stale registry state - most often a fleet-wide install.reg push from before
    keys were assigned per box - and reporting it as a licensing fault is the
    same class of mistake as calling a measurement artefact a defect. Measured
    2026-09-01: .133 and .143 share a key and neither has halo.exe (the gate
    refuses the title there anyway - no SSE2), so nothing is broken.
    """
    live, stale = {}, {}
    for fp, entries in by_fp.items():
        if len(entries) < 2:
            continue
        playing = [ip for ip, inst in entries if inst]
        (live if len(playing) > 1 else stale)[fp] = entries
    return live, stale


async def main_async(a):
    boxes = ([b.strip() for b in a.boxes.split(",") if b.strip()]
             if a.boxes else DEFAULT_BOXES)
    results = await asyncio.gather(
        *(read_box(ip, a.secret, a.timeout) for ip in boxes))

    rows = list(zip(boxes, results))
    width = max(len(b) for b in boxes)
    by_fp = {}
    for ip, (state, fp, installed, _detail) in rows:
        if state == "ok":
            by_fp.setdefault(fp, []).append((ip, installed))

    print("%-*s  %-11s  %-10s  %-9s  %s"
          % (width, "box", "state", "key fp", "halo.exe", "detail"))
    print("-" * (width + 46))
    for ip, (state, fp, installed, detail) in rows:
        mark = "-" if state == "unreachable" else ("yes" if installed else "no")
        print("%-*s  %-11s  %-10s  %-9s  %s"
              % (width, ip, state, fp or "-", mark, detail))

    # A duplicate only MATTERS between boxes that can actually be in a game.
    # A leftover DigitalProductID on a box with no Halo installed is stale
    # registry state, not a licensing fault - reporting it as one is the same
    # class of mistake as calling a measurement artefact a defect.
    live_dupes, stale_dupes = classify_duplicates(by_fp)
    print()

    rc = 0
    if live_dupes:
        print("DUPLICATE KEYS ON BOXES THAT HAVE HALO - these CANNOT be in the "
              "same game:")
        for fp, entries in sorted(live_dupes.items()):
            print("  %s  %s" % (fp, ", ".join(ip for ip, inst in entries if inst)))
        print("\nFix with:  python3 scripts/halo/assign_keys.py --keys-file "
              "<file> --boxes "
              + ",".join(ip for e in live_dupes.values() for ip, inst in e if inst))
        rc = 1

    if stale_dupes:
        print("Shared key, but NOT a fault today - Halo is not installed on these,"
              " so no game can be affected. It is LEFTOVER registry state, most"
              " likely a fleet-wide install.reg push from before keys were"
              " assigned per box:")
        for fp, entries in sorted(stale_dupes.items()):
            print("  %s  %s" % (fp, ", ".join(
                "%s(%s)" % (ip, "halo" if inst else "no halo") for ip, inst in entries)))
        print("  Leave it, or clear the value if the box is being tidied:")
        print("    REGDELETE HKLM \"%s\" DigitalProductID" % HALO_KEY)
        print("  It only becomes a fault if Halo is ever deployed to two of them.")

    n = len(by_fp)
    total = sum(len(v) for v in by_fp.values())
    playing = sum(1 for _ip, (st, _f, inst, _d) in rows if st == "ok" and inst)
    if not live_dupes:
        print("\nNo duplicate among boxes that have Halo. %d box(es) carry a key "
              "(%d distinct); %d of them actually have halo.exe installed."
              % (total, n, playing))
    unreachable = [ip for ip, (st, _f, _i, _d) in rows if st == "unreachable"]
    if unreachable:
        print("NOT AUDITED (switched off, which is normal on this fleet - this is "
              "NOT 'no key'): " + ", ".join(unreachable))
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boxes", help="comma-separated IPs (default: the roster)")
    ap.add_argument("--secret", default="retro-agent-secret")
    ap.add_argument("--timeout", type=float, default=8.0)
    a = ap.parse_args()
    raise SystemExit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
