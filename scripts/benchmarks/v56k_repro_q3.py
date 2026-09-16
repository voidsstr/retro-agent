#!/usr/bin/env python3
"""
v56k_repro_q3.py - reproduce the cell that kills the agent, with the flight
recorder already running, and recover the evidence afterwards.

THE FAILURE
-----------
On .124 (V5 6000, AmigaMerlin 3.1-R11), a cfg-5 sweep measured Quake III
cleanly at 1600x1200 16/32 and 1280x960 16/32 and then **the agent died during
1024x768 16-bit** - 9898 refused, 9897 still bound, 445/139 open: the process is
gone, the box is fine. It has happened three times now and costs ~20 minutes of
watchdog recovery each time, so this script is built to get ONE reproduction and
keep everything it can.

WHY THE RING IS THE RIGHT INSTRUMENT
------------------------------------
`fxscan2 ring` is launched DETACHED and fflushes after every line, so when the
agent dies the file on disk still holds the last per-chip register state before
it went. Nothing else here survives: the bench runner's own capture path needs a
live agent, which is exactly what is missing. The escapes it rides are not
exclusive-gated, so it samples right through a live fullscreen Glide session -
the one path the display driver cannot see at all, because it releases the card
on DrvAssertMode(DISABLE).

THE HYPOTHESIS IT TESTS
-----------------------
The failure landed on the FIFTH mode change of the boot, and this board's known
weakness is mode programming across the four chips: the W2K miniport copies only
five registers master->slave and `vidScreenSize` is NOT among them. If the ring
shows the slaves failing to take the new geometry on that mode set - or a CRTC
heartbeat that stops advancing - that is the fault, and it is in the driver's
mode path rather than in anything Quake III does.

    python3 v56k_repro_q3.py --host 192.168.1.124 [--res 1024x768] [--depth 16]
"""
import argparse
import asyncio
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))
import v56k_diag  # noqa: E402
from v56k_bench import Box, Quake3  # noqa: E402


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


async def alive(ip, timeout=6):
    from client.retro_protocol import RetroConnection
    try:
        c = RetroConnection(ip, 9898)
        await c.connect("retro-agent-secret", timeout=timeout)
        await c.send_command("PING", timeout=timeout)
        await c.close()
        return True
    except Exception:
        return False


async def wait_back(ip, minutes=45):
    deadline = time.time() + minutes * 60
    while time.time() < deadline:
        if await alive(ip):
            return True
        await asyncio.sleep(20)
    return False


async def main_async(a):
    box = Box(a.host)
    outdir = Path(a.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    w, h = (int(x) for x in a.res.split("x"))

    if not await alive(a.host):
        log("agent is down; waiting for it to come back before starting")
        if not await wait_back(a.host):
            log("agent never came back - it needs a person at the box")
            return 2

    log("clearing Dr Watson so any crash is unambiguously THIS run")
    await v56k_diag.watson_clear(box)
    log("suppressing crash dialogs (a dialog behind fullscreen reads as a wedge)")
    log(f"  {await v56k_diag.errors_quiet(box)}")

    log("per-chip state BEFORE the run")
    try:
        (outdir / "before-dump.txt").write_text(await v56k_diag.dump(box))
    except Exception as e:
        log(f"  dump failed: {e}")

    # The recorder must be sampling BEFORE the mode switch - that is the part
    # that matters, and once the game is up there is nothing left to see.
    log(f"arming the ring for {a.secs}s @ {a.interval}ms (detached; survives the agent)")
    await v56k_diag.ring_start(box, a.secs, a.interval)
    await asyncio.sleep(5)

    t = Quake3()
    log(f"launching Quake III {a.res} {a.depth}-bit - the cell that kills the agent")
    await t.prepare(box, w, h, a.depth, {})
    await t.start(box)

    died_at = None
    deadline = time.time() + a.secs
    while time.time() < deadline:
        await asyncio.sleep(10)
        if not await alive(a.host):
            died_at = time.time()
            log("*** AGENT DIED - this is the reproduction. The ring is still on disk. ***")
            break
    if died_at is None:
        log("the cell did NOT kill the agent this time")
        try:
            await box.exec_(f'cmd /c taskkill /f /im "{t.proc}"', timeout=30)
        except Exception:
            pass

    if died_at:
        log("waiting for the watchdog to bring the agent back (it has taken ~20 min)")
        if not await wait_back(a.host):
            log("agent never came back - the ring is on the box at C:\\fxring.txt")
            return 2
        log("agent back")

    log("recovering the evidence")
    p, n = await v56k_diag.ring_fetch(box, outdir, "repro-fxring.txt")
    if p:
        log(f"  ring -> {p} ({n} B)")
        s = v56k_diag.ring_summary(p)
        log(f"  CRTC stalled: {s['stalled']}")
        log(f"  registers that moved: {len(s['changed'])}")
        for k, v in list(s["changed"].items())[:14]:
            log(f"    {k}: {v[-1][1]} -> {v[-1][2]}")
        for hb in s["heartbeats"]:
            log(f"    {hb}")
    else:
        log("  no ring file (fxscan2 may not have started)")
    got = await v56k_diag.watson_fetch(box, outdir)
    if "drwtsn32.log" in got:
        d = v56k_diag.watson_decode(outdir / "drwtsn32.log")
        for r in d["records"]:
            log(f"  CRASH app={r['app']} exception={r['exception']}")
        log(f"  fault function: {d['fault_function']}")
        for fr in d["frames"]:
            log(f"    {fr}")
    else:
        log("  no Dr Watson record"
            + (" - the agent died without a user-mode crash being logged" if died_at
               else " - correct: nothing crashed this run"))
    try:
        (outdir / "after-dump.txt").write_text(await v56k_diag.dump(box))
    except Exception as e:
        log(f"  after-dump failed: {e}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--res", default="1024x768")
    ap.add_argument("--depth", type=int, default=16)
    ap.add_argument("--secs", type=int, default=240)
    ap.add_argument("--interval", type=int, default=100)
    ap.add_argument("--outdir", default=str(HERE / "results" / "v56k_repro_192.168.1.124"))
    return asyncio.run(main_async(ap.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
