#!/usr/bin/env python3
"""Publish WHOLE-LIBRARY verdict files for every live fleet box, and prove it.

One publisher owns <library>/_gamegate/<profile_hash>.txt, and it covers every
title. This script is that publisher. A per-title pass must never write those
files - see README.md; on 2026-08-30 one did, leaving a single-row file on seven
of eight boxes that was well formed enough that nothing noticed for hours, and
taking nine ollama adjudications with it.

Two things here are deliberate and easy to "simplify" wrongly:

  * IT WRITES STRAIGHT TO THE SHARE (UPLOAD to the Z: path), never a local temp
    plus `copy`. `copy` propagates the SOURCE timestamp, so a file staged on a
    box lands stamped with THAT BOX's clock - .124 is two hours fast. The
    agent's own write is CreateFile+WriteFile, so the file server stamps it.

  * IT VERIFIES THE POST-CONDITION. Publishing is not "the copy returned 0", it
    is "the file on the share carries one row per title". That check is the
    whole point: the failure this script exists to prevent produced a perfectly
    valid file, so only the ROW COUNT distinguishes success from disaster.

Usage:
    python3 scripts/gamegate/publish_all.py [ip ...]     (default: all known)
    python3 scripts/gamegate/publish_all.py --verify-only
"""
import argparse
import asyncio
import datetime
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "scripts"))
sys.path.insert(0, REPO)

from gamegate import rules, cache as cache_mod, library as library_mod, llm as llm_mod
from gamegate.gamegate import get_profile_async, plan
from client.retro_protocol import RetroConnection

SECRET = "retro-agent-secret"
# Any box with the share mapped can write it; the file lands on the SERVER.
# The box used to WRITE the verdict files onto the share.
#
# This was hardcoded to .124, which meant the publisher could not run whenever
# that one machine was off - and this fleet is powered on demand, so that is
# the normal state, not an edge case. Measured 2026-08-31: publishing verdicts
# for .240's new GPU failed with "Connect call failed ('192.168.1.124', 9898)"
# while .240 itself, .133, .143 and .246 were all up and answering.
#
# Now: an override, then the first box that actually answers. The writer only
# needs a mapped share and a working agent; it does not have to be any
# particular machine.
WRITER = os.getenv("RETRO_GAMEGATE_WRITER", "")
WRITER_CANDIDATES = ["192.168.1.124", "192.168.1.123", "192.168.1.133",
                     "192.168.1.143", "192.168.1.240", "192.168.1.246"]


async def _pick_writer():
    """First candidate that can actually WRITE, not merely answer.

    An answering agent is not a usable writer: .123 replies to PING perfectly
    and has no Z: mapping at all, so the first version of this picked it and
    the publish failed with "Cannot create file: error 3" (path not found).
    Verify the post-condition - can this box SEE the target directory - rather
    than the return value. A stale or missing share mapping is common here;
    CLAUDE.md records boxes answering `net use` with "no entries in the list"
    and with an "Unavailable" drive.
    """
    if WRITER:
        return WRITER
    for ip in WRITER_CANDIDATES:
        try:
            c = RetroConnection(ip, 9898)
            await c.connect(SECRET, timeout=8.0)
            try:
                r = await c.command_text(
                    'EXEC cmd /c if exist "%s" (echo YES) else (echo NO)'
                    % SHARE_DIR, timeout=20.0)
            finally:
                await c.close()
            if "YES" in r.upper():
                return ip
        except Exception:
            continue
    raise SystemExit(
        "no reachable fleet box can SEE %s. The publisher needs one machine "
        "with the share mapped - an agent that merely answers is not enough. "
        "Set RETRO_GAMEGATE_WRITER to override the candidate list." % SHARE_DIR)
SHARE_DIR = "Z:\\Files\\Games-Library\\_gamegate"
FLEET = ["192.168.1.123", "192.168.1.124", "192.168.1.133", "192.168.1.143",
         "192.168.1.145", "192.168.1.171", "192.168.1.240", "192.168.1.246"]


async def _write(conn, name, text):
    """Straight to the share, so the server stamps the mtime."""
    data = text.encode("ascii", "replace").replace(b"\n", b"\r\n")
    st, rp = await conn.send_command(f"UPLOAD {SHARE_DIR}\\{name}",
                                     binary_payload=data)
    return st == 0, rp.decode("ascii", "replace")[:60]


async def _rows_on_share(conn, name):
    """Count the verdict rows actually present in the published file."""
    st, rp = await conn.send_command(
        f'EXEC cmd /c find /v /c "#" < "{SHARE_DIR}\\{name}"')
    txt = rp.decode("ascii", "replace")
    for tok in txt.split():
        if tok.strip().isdigit():
            return int(tok.strip())
    return -1


async def main_async(ips, verify_only):
    titles = library_mod.load_library(None)
    print(f"library: {len(titles)} titles")
    cache = cache_mod.Cache()
    judge = llm_mod.Judge()
    model = getattr(judge, "model", "") or ""
    now = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")

    writer = await _pick_writer()
    print("writer: %s" % writer)
    conn = RetroConnection(writer, 9898)
    await conn.connect(SECRET, timeout=20.0)
    await conn.send_command(
        f'EXEC cmd /c if not exist "{SHARE_DIR}" mkdir "{SHARE_DIR}"')

    bad = []
    for ip in ips:
        prof = None
        last = None
        for attempt in range(6):
            try:
                prof, _d = await get_profile_async(ip, cache)
                break
            except (OSError, asyncio.TimeoutError) as e:
                # Only a genuinely network-shaped failure is worth retrying.
                last = e
                if attempt < 5:
                    await asyncio.sleep(15)
            except Exception as e:
                # Anything else is a BUG, not a powered-off box. Say so and
                # stop -- retrying it six times over 90 seconds and then
                # printing "UNREACHABLE" is how this stayed hidden: an
                # asyncio.run()-inside-a-loop RuntimeError wore the costume of
                # a machine that was simply switched off, on a fleet that is
                # deliberately switched off most of the time.
                bad.append(f"{ip}: {type(e).__name__}: {e}")
                print(f"{ip:16s} ERROR ({type(e).__name__}: {e}) - not a "
                      f"reachability problem, not retried")
                last = e
                break
        if prof is None:
            if not any(b.startswith(f"{ip}:") for b in bad):
                print(f"{ip:16s} UNREACHABLE ({type(last).__name__ if last else 'no reply'})"
                      f" - not published (its file is left alone)")
            continue

        if not verify_only:
            rows = plan(prof, titles, cache, judge, model,
                        use_llm=True, refresh=False)
            text = rules.format_verdict_file(
                prof, [(t.name, d) for t, d, _s, _x in rows], model, now)
            ok, msg = await _write(conn, f"{prof.profile_hash}.txt", text)
            if not ok:
                bad.append(f"{ip}: write failed ({msg})")
                continue

        # THE POST-CONDITION. A valid one-row file is the failure mode, so the
        # count is the only thing that tells success from disaster.
        got = await _rows_on_share(conn, f"{prof.profile_hash}.txt")
        state = "OK" if got == len(titles) else "*** WRONG ***"
        print(f"{ip:16s} {prof.profile_hash}  {got:3d}/{len(titles)} rows  {state}")
        if got != len(titles):
            bad.append(f"{ip}: {got} rows on the share, expected {len(titles)}")

    await conn.close()
    cache.close()

    if bad:
        print("\nFAILED:")
        for b in bad:
            print("  " + b)
        return 1
    print("\nevery published file covers the whole library")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip", nargs="*", default=None)
    ap.add_argument("--verify-only", action="store_true",
                    help="check the row counts on the share, write nothing")
    a = ap.parse_args()
    return asyncio.run(main_async(a.ip or FLEET, a.verify_only))


if __name__ == "__main__":
    sys.exit(main())
