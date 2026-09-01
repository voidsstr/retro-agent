#!/usr/bin/env python3
r"""Deploy newly staged titles to each box as it comes online.

WHY THIS EXISTS - IT IS NOT AUTOMATIC, AND EVERYONE ASSUMES IT IS
-----------------------------------------------------------------
The agent's GAMESYNC startup thread runs once per boot and its gate is a bare
marker check (agent/src/gamesync.c):

    if (gs_file_exists(GS_MARKER)) {
        log_msg(LOG_GS, "already provisioned (%s present) - idle", GS_MARKER);
        return 0;
    }

There is no library comparison and no title count in that condition. Once
`gamesync.done` exists the thread idles FOREVER, so **a title staged today
never reaches a box that was provisioned yesterday.** Rainbow Six landed on
.123 only because a human issued `GAMESYNC RESET` by hand.

That is a quiet failure: the library grows, `validate-staged-library.py` says
DEPLOYABLE, the boxes look healthy, and the new games are simply not there.

WHAT THIS DOES
--------------
Watches for boxes answering the protocol, and when the LIBRARY'S TITLE SET HAS
CHANGED since that box was last synced, issues `GAMESYNC RESET` + `GAMESYNC
START` and waits for it to finish.

WHY IT KEYS ON THE LIBRARY SET AND NOT ON "WHAT IS MISSING FROM THE BOX"
-----------------------------------------------------------------------
The obvious design - compare C:\Games against the library and sync if anything
is missing - LOOPS FOREVER. The capability gate legitimately refuses titles per
box: .143's CPU lacks SSE so Halo is refused there, .133 lacks SSE2, .240 is
refused Halo 2 on free disk space. Those titles are missing by DESIGN and will
never appear, so "missing" is not a signal that work is needed. Keying on a
change in the library's own title set asks the right question - "is there
anything here the box has not been offered yet?" - and settles.

State lives in ~/.retro-fleet/autodeploy.json, one entry per box.

SAFETY
------
* A box mid-GAMESYNC is left alone (state != idle/done -> skip this pass).
* A refused connection is retried, not treated as absence: the Win9x agents are
  single-threaded and refuse while busy, and calling that DOWN produces a fake
  power-cycle event. Only a completed protocol handshake counts as UP.
* NEVER reboots anything. GAMESYNC is a file copy plus a registry merge.
* --dry-run prints what it would do and changes nothing.
"""
import argparse
import asyncio
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)

STATE = os.path.expanduser("~/.retro-fleet/autodeploy.json")
LIBRARY = "/mnt/retro-share/Files/Games-Library"
SECRET = "retro-agent-secret"
BOXES = ["192.168.1.123", "192.168.1.124", "192.168.1.133", "192.168.1.143",
         "192.168.1.145", "192.168.1.171", "192.168.1.240", "192.168.1.243",
         "192.168.1.246"]
INTERVAL = 60.0
REFUSAL_RETRIES = 3
REFUSAL_BACKOFF = 4.0


def library_titles():
    """The staged titles, by directory name. `_`-prefixed dirs are not titles."""
    try:
        return sorted(d for d in os.listdir(LIBRARY)
                      if not d.startswith("_")
                      and os.path.isdir(os.path.join(LIBRARY, d)))
    except OSError:
        return []


def load_state():
    try:
        with open(STATE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_state(st):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    tmp = STATE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(st, f, indent=1, sort_keys=True)
    os.replace(tmp, STATE)          # atomic: a torn state file re-syncs the fleet


async def _connect(ip, timeout):
    from client.retro_protocol import RetroConnection
    refused = 0
    while True:
        c = RetroConnection(ip, 9898)
        try:
            await c.connect(SECRET, timeout=timeout)
            return c
        except ConnectionRefusedError:
            # contention, not death - the single-threaded agents refuse while busy
            refused += 1
            if refused > REFUSAL_RETRIES:
                return None
            await asyncio.sleep(REFUSAL_BACKOFF)
        except Exception:
            return None


async def probe(ip, timeout=8.0):
    """Return (alive, gamesync_state) - alive only on a completed handshake."""
    c = await _connect(ip, timeout)
    if c is None:
        return False, None
    try:
        r = await c.command_text("PING", timeout=timeout)
        if "PONG" not in r.upper():
            return False, None          # accepts sockets, answers nothing
        try:
            st = json.loads(await c.command_text("GAMESYNC STATUS", timeout=20))
            return True, st.get("state")
        except Exception:
            return True, None
    except Exception:
        return False, None
    finally:
        try:
            await c.close()             # graceful: an abrupt close crashes Win98
        except Exception:
            pass


async def sync(ip, timeout=20.0, wait=True):
    """RESET + START, then wait for the run to finish. Returns the final status."""
    c = await _connect(ip, timeout)
    if c is None:
        return None
    try:
        await c.command_text("GAMESYNC RESET", timeout=30)
        await c.command_text("GAMESYNC START", timeout=40)
    finally:
        try:
            await c.close()
        except Exception:
            pass
    if not wait:
        return {"state": "started"}
    for _ in range(360):                # a full library sync is slow over SMB1
        await asyncio.sleep(20)
        c = await _connect(ip, timeout)
        if c is None:
            continue
        try:
            st = json.loads(await c.command_text("GAMESYNC STATUS", timeout=25))
        except Exception:
            st = None
        finally:
            try:
                await c.close()
            except Exception:
                pass
        if st and st.get("state") in ("done", "error"):
            return st
    return {"state": "timeout"}


async def pass_once(a, titles, st):
    changed = False
    for ip in a.boxes:
        alive, gs = await probe(ip)
        if not alive:
            continue
        rec = st.get(ip) or {}
        if rec.get("titles") == titles:
            continue                    # box has already been offered this library
        if gs not in (None, "idle", "done"):
            print("  %s busy (%s) - leaving it alone" % (ip, gs), flush=True)
            continue
        new = [t for t in titles if t not in (rec.get("titles") or [])]
        print("  %s needs a sync: %d new title(s)%s"
              % (ip, len(new), (" - " + ", ".join(new[:6])) if new else ""), flush=True)
        if a.dry_run:
            continue
        res = await sync(ip, wait=not a.no_wait)
        ok = bool(res) and res.get("state") == "done" and not res.get("failed_files")
        print("    -> %s" % json.dumps(res or {"state": "unreachable"}), flush=True)
        if ok:
            st[ip] = {"titles": titles, "at": time.strftime("%Y-%m-%d %H:%M:%S")}
            changed = True
        else:
            # do NOT record a failed run as done, or the box never retries
            print("    NOT recorded - it will be retried next pass", flush=True)
    if changed:
        save_state(st)


async def main_async(a):
    st = load_state()
    while True:
        titles = library_titles()
        if not titles:
            print("library unreadable at %s - waiting" % LIBRARY, flush=True)
        else:
            await pass_once(a, titles, st)
        if a.once:
            return
        await asyncio.sleep(a.interval)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boxes", default=",".join(BOXES))
    ap.add_argument("--interval", type=float, default=INTERVAL)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--no-wait", action="store_true",
                    help="fire GAMESYNC and move on instead of waiting for done")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    a.boxes = [b.strip() for b in a.boxes.split(",") if b.strip()]
    asyncio.run(main_async(a))


if __name__ == "__main__":
    main()
