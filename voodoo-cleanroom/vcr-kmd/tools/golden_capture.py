#!/usr/bin/env python3
"""golden_capture.py - register dumps from a KNOWN-GOOD driver, per mode.

Uploads vcrctl.exe to a box, proves the HWCEXT mapping works through whatever
3dfx driver is installed (`vcrctl hwc`), then for each mode sets it through
GDI and dumps every IO register plus the CRTC (`vcrctl golden`). The result is
the reference our own mode-set math is compared against
(tools/golden_compare.py) BEFORE our driver programs the chip.

Read-only apart from the mode changes and the CRTC index writes; the box's
own mode is restored at the end.

    golden_capture.py 192.168.1.124 --label amigamerlin-3.1-r11 \\
        --monitor-info ours-info.json --modes 1024x768x16@85,1600x1200x32@70

EVERY CAPTURE IS TWO MONITOR RE-SYNCS: into the mode, and back when vcrctl
exits (XP reverts a CDS_FULLSCREEN mode with its process). The default sweep
is ~123 captures - the shape of the 2026-09-26 .124 run that clicked a 1998
CRT's relays ~250 times at two a second. So, as in mode_sweep.py:
  - every mode is checked on the host against the monitor's EDID ranges (the
    driver's own mode math, tools/modecalc.c, and its own rounding rule) before
    anything is loaded or switched. The vendor driver cannot be asked for the
    EDID, so a capture under it needs --monitor-info: `vcrctl info` saved
    from OUR driver on the same box and monitor - used only once the
    registry says the monitor on the box now is that one (without a readable
    registry, only with --i-have-checked-the-monitor);
  - --pace seconds (default 5, never under 3) between captures and before the
    restore; vcrctl holds each mode for the floor itself as well;
  - more than --max-live captures (default 12) is refused without
    --allow-many;
  - each capture runs under EXECW with a budget that covers the pace gate's
    worst case for both of its switches (a busy switch lock, then the
    floor) and a host timeout past it, so a hang comes back as the agent's
    answer, not as a host exception that skips the cleanup;
  - the FIRST signal, or any exception, ends the loop but does not abandon
    the capture in flight: `vcrctl golden` holds its mode for the floor and
    exits by itself, so it is waited for - bounded by its EXECW budget. Only
    a second signal, or that bound, leads to the cleanup: PROCLIST, a
    surviving vcrctl killed by PID through `vcrctl pace-kill` (never one
    that predates the capture), the stamp, the pace - then ONE restore,
    under EXECW, never a retry, and none when the desktop mode is back;
  - vcrprobe is unloaded in a finally, and a failed final restore is rc 1.
"""
import argparse
import asyncio
import json
import signal
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(HERE))
from client.retro_protocol import RetroConnection  # noqa: E402
import mode_sweep as ms  # noqa: E402  (the monitor gate every switching tool shares)

SECRET = "retro-agent-secret"
DEFAULT_DIR = r"C:\vcr"
# The register read, plus the VGA file through vcrprobe, plus start-up
GOLDEN_WORK_S = 20
# One `vcrctl golden`: a paced switch into the mode, the read, and the exit
# hold before XP reverts it - two waits on vcr_pace.h's gate, each of which
# can queue behind another tool's switch lock before its floor starts. The
# agent kills at this budget and SAYS so in its reply - but its tree kill
# reverts the mode unpaced, so the budget covers the gate's worst case. The
# host waits HOST_SLACK_S longer, so it hears that answer instead of timing
# out first. (With plain EXEC the host's 60 s equalled the agent's, a hang
# raised on the host first, and every cleanup was skipped.)
GOLDEN_EXECW_S = ms.execw_budget(2, GOLDEN_WORK_S)
HOST_SLACK_S = ms.HOST_SLACK_S
# EXEC (sc, and the commands that switch nothing) is killed by the agent at
# 60 s: the host outwaits that
EXEC_HOST_TIMEOUT = 90


class Interrupted(Exception):
    """A KeyboardInterrupt that reached a capture's own await - before the
    handlers below were installed, or raised by the transport - carried as
    an ordinary exception, so it takes the same civil way out instead of
    tearing the event loop down with a capture in flight."""


async def guarded(coro):
    try:
        return await coro
    except KeyboardInterrupt as e:
        raise Interrupted("KeyboardInterrupt") from e


class Interrupts:
    """SIGINT/SIGTERM while the box is capturing. A running `vcrctl golden`
    gives its mode back by itself within seconds - its exit hold, then XP's
    revert, paced and stamped - and a kill would skip exactly that. So:
      1st  (or any exception) no further capture; the one in flight is waited
           for, bounded by its EXECW budget, to exit on its own;
      2nd  stop waiting: a survivor is pace-killed by PID (ms.reap);
    then the pace and ONE restore. While that cleanup runs, further signals
    are noted, not obeyed - it is what gives the monitor its desktop mode."""

    def __init__(self):
        self.seen = []
        self.stopped = False             # a signal or an exception ended the loop
        self.interrupted = False         # ... a KeyboardInterrupt that reached an await
        self.first = asyncio.Event()
        self.second = asyncio.Event()
        self.cleaning = False
        self.loop = asyncio.get_running_loop()

    def __enter__(self):
        for s in (signal.SIGINT, signal.SIGTERM):
            self.loop.add_signal_handler(s, self.on_signal, s.name)
        return self

    def __exit__(self, *exc):
        for s in (signal.SIGINT, signal.SIGTERM):
            self.loop.remove_signal_handler(s)

    def stop(self):
        self.stopped = True
        self.first.set()

    def on_signal(self, name):
        self.seen.append(name)
        if self.cleaning:
            print(f"\n{name}: noted - the cleanup and the one restore finish first", flush=True)
        elif not self.stopped:
            print(f"\n{name}: no further capture; the one running is left to finish and give "
                  "its mode back by itself (again to stop waiting and pace-kill it)", flush=True)
            self.stop()
        else:
            print(f"\n{name}: no longer waiting - a surviving vcrctl is pace-killed by PID, "
                  "then the pace and at most one restore", flush=True)
            self.second.set()


async def wait_first(delay, event):
    """Sleep `delay`, or less when `event` is set first. -> True when it was."""
    if event.is_set():
        return True
    ev = asyncio.create_task(event.wait())
    sl = asyncio.create_task(asyncio.sleep(delay))
    try:
        await asyncio.wait({ev, sl}, return_when=asyncio.FIRST_COMPLETED)
    finally:
        ev.cancel()
        sl.cancel()
    return event.is_set()


async def wait_gone(call, spare, bound, intr):
    """A capture's answer was lost (a dropped connection, a host timeout, the
    agent's own timeout): its vcrctl may still be on the box, holding the
    mode it is about to give back. Look until no vcrctl of ours is left,
    bounded; a second signal cuts it short. -> True when none is left."""
    print(f"waiting up to {bound:.0f} s for the capture's vcrctl to leave on its own", flush=True)
    for _ in range(int(bound // ms.POLL_S) + 1):
        if await ms.pids_of(call, ms.VCRCTL_IMAGES, spare) == []:
            return True
        if await wait_first(ms.POLL_S, intr.second):
            return False
    return False


async def run(c, cmd, timeout=60):
    st, d = await c.send_command(cmd, timeout=timeout)
    return d.decode("ascii", "replace")


def fresh(a):
    """ms.reap()'s call on a NEW connection per command: after a timeout or a
    dropped reply the capture's own connection may still owe an answer."""
    async def call(cmd, timeout=60):
        c = RetroConnection(a.host, 9898)
        await c.connect(SECRET, timeout=20)
        try:
            st, d = await c.send_command(cmd, timeout=timeout)
            return st, d.decode("ascii", "replace")
        finally:
            await c.close()
    return call


def last_json(text):
    for line in reversed(text.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                pass
    return None


def pick_modes(listed, limit_per_size=None):
    """Every listed mode at 16 bpp, plus 8 and 32 bpp at each size's lowest
    and highest refresh - the axes the mode math varies along."""
    by_size = {}
    for m in listed:
        wh, rest = m.rsplit("x", 1)[0], m
        w, h, bpp_hz = m.split("x")
        bpp, hz = bpp_hz.split("@")
        by_size.setdefault((int(w), int(h)), []).append((int(bpp), int(hz)))
    out = []
    for (w, h), lst in sorted(by_size.items()):
        hzs = sorted({hz for bpp, hz in lst if hz > 1})
        for bpp, hz in sorted(set(lst)):
            if hz <= 1:
                continue
            if bpp == 16 or (bpp in (8, 32) and hz in (hzs[0], hzs[-1])):
                out.append(f"{w}x{h}x{bpp}@{hz}")
    return out


async def main_async(a):
    exe = Path(a.exe)
    c = RetroConnection(a.host, 9898)
    await c.connect(SECRET, timeout=20)
    result = {"host": a.host, "label": a.label, "taken": time.strftime("%Y-%m-%dT%H:%M:%S"),
              "captures": []}
    try:
        await run(c, f"MKDIR {a.dir}")
        data = exe.read_bytes()
        await c.send_command(rf"UPLOAD {a.dir}\vcrctl.exe", binary_payload=data, timeout=60)
        # Verify the post-condition: an UPLOAD to a drive that does not exist
        # (.124 lost its D: in a re-image) answers without error.
        listing = await run(c, f"DIRLIST {a.dir}")
        if f'"size":{len(data)}' not in listing.replace(" ", ""):
            print(f"FAIL: vcrctl.exe did not land in {a.dir}: {listing[:200]}")
            return 2
        tool = rf"{a.dir}\vcrctl.exe"
        modes = last_json(await run(c, f"EXEC {tool} modes"))
        if not modes:
            print("FAIL: vcrctl modes produced no JSON")
            return 2
        result["modes"] = modes
        original = modes.get("current") if modes else None
        print(f"{len(modes.get('modes', []))} modes listed, current {original}")
        todo = ([m.strip() for m in a.modes.split(",") if m.strip()] if a.modes
                else pick_modes(modes.get("modes", [])))
        if len(todo) > a.max_live and not a.allow_many:
            ms.refuse(f"{len(todo)} captures is {2 * len(todo)} monitor re-syncs; the limit is "
                      f"{a.max_live} captures (--allow-many to override)")
            return 2
        # the monitor gate, before a driver is loaded or a mode switched
        rng, calc, why = await ms.gate_on_box(fresh(a), tool, todo, a.monitor_info,
                                              a.i_have_checked_the_monitor)
        if why:
            ms.refuse(why)
            return 2
        where = "vcrctl info" if not a.monitor_info else f"--monitor-info {a.monitor_info}"
        pace = max(a.pace, 3.0)
        result["monitor_gate"] = dict(rng, source=where, pace_s=pace)
        print(f"plan: {len(todo)} captures, {pace:.0f} s apart: up to {2 * len(todo) + 1} monitor "
              f"re-syncs (into each mode and back as vcrctl exits, then the restore); each "
              f"capture under EXECW {GOLDEN_EXECW_S} s"
              f"\n  monitor {ms.describe(rng)}")
        for m in todo:
            print(f"  {m:>18}  {ms.rates(calc, m)}")
        probe_created = False
        failure = interrupted = None
        try:
            if a.probe:
                # vcrprobe.sys next to the installed driver: the VGA register
                # file (unreachable from user mode) and PCI config of every chip
                pdata = Path(a.probe).read_bytes()
                await c.send_command(rf"UPLOAD {a.dir}\vcrprobe.sys", binary_payload=pdata,
                                     timeout=60)
                probe_created = True     # from here on the finally unloads it
                await run(c, rf"EXEC sc create vcrprobe type= kernel start= demand binPath= {a.dir}\vcrprobe.sys")
                started = await run(c, "EXEC sc start vcrprobe")
                pv = last_json(await run(c, f"EXEC {tool} probe-vga"))
                probe_loaded = bool(pv and pv.get("ok"))
                print(f"vcrprobe: {'loaded' if probe_loaded else 'NOT loaded: ' + started[-200:]}")
                result["probe_vga_desktop"] = pv
                if probe_loaded:
                    result["pci"] = []
                    for bus, dev, fns in a.pci:
                        for fn in fns:
                            result["pci"].append(last_json(await run(c, f"EXEC {tool} probe-pci {bus} {dev} {fn}")))
            result["hwc"] = last_json(await run(c, f"EXEC {tool} hwc"))
            print("hwc:", json.dumps(result["hwc"]))
            # vcrctl PIDs already running belong to someone else: a cleanup
            # never kills them (None: PROCLIST did not answer - it cannot
            # tell, and then kills nothing)
            spare = await ms.pids_of(fresh(a), ms.VCRCTL_IMAGES)
            print(f"capturing {len(todo)} modes")
            with Interrupts() as intr:
                failure, lost = await capture_all(a, c, tool, todo, pace, result, intr)
                if lost and not intr.second.is_set():
                    # its answer never came: its vcrctl may still be finishing
                    await wait_gone(fresh(a), spare, GOLDEN_EXECW_S + HOST_SLACK_S, intr)
                intr.cleaning = True
                interrupted = intr.seen or intr.interrupted
                if failure:
                    ms.banner("CAPTURE STOPPED", f"{failure} - cleanup, then at most ONE "
                              "restore, no retry")
                    result["error"] = failure
                if lost:
                    # a survivor of ours is pace-killed by PID; what predates
                    # the capture is never touched
                    rep = await ms.reap(fresh(a), ms.VCRCTL_IMAGES, tool, pace, spare=spare)
                    result["cleanup"] = rep
                    if rep["gone"] and not rep["spared"]:
                        result["restore"] = await restore_if_needed(a, tool, original)
                    else:
                        result["restore"] = {"ok": False, "error": "not restored: a vcrctl is "
                                             "still running (a restore would fight it)",
                                             "cleanup": rep}
                else:
                    # every capture that ran answered: each vcrctl held its
                    # mode and gave it back stamped
                    await asyncio.sleep(pace)
                    result["restore"] = await restore_if_needed(a, tool, original)
            print("restore:", result["restore"])
            if not (result["restore"] and result["restore"].get("ok")):
                ms.banner("NOT RESTORED", "the final restore did not succeed - look at the box; "
                          "it was not retried")
        finally:
            # after the cleanup: a vcrctl still holding the probe device would
            # keep it from stopping. Each step on its own connection, and none
            # may stop the other or hide the capture's own error
            if probe_created:
                for cmd in ("EXEC sc stop vcrprobe", "EXEC sc delete vcrprobe"):
                    try:
                        _, txt = await fresh(a)(cmd, EXEC_HOST_TIMEOUT)
                        print(f"vcrprobe {cmd.split()[2]}:", txt.strip()[-80:])
                    except Exception as e:
                        print(f"vcrprobe {cmd.split()[2]} FAILED: {type(e).__name__}: {e}")
    finally:
        await c.close()
    outp = Path(a.out) if a.out else HERE.parent / "golden" / f"{a.label}_{a.host}.json"
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text(json.dumps(result, indent=1))
    print(f"-> {outp}")
    if interrupted:
        return 130
    restored = bool(result.get("restore") and result["restore"].get("ok"))
    return 0 if not failure and restored and all(x.get("ok") for x in result["captures"]) else 1


async def capture_all(a, c, tool, todo, pace, result, intr):
    """Every capture in `todo`, `pace` apart, until one fails, times out,
    raises, or a signal comes. -> (failure or None, lost): `lost` when the
    capture in flight was not heard to finish - it timed out, its answer was
    lost, or a second signal cut the wait - so a vcrctl of ours may still be
    on the box and the cleanup must look for it."""
    bound = GOLDEN_EXECW_S + HOST_SLACK_S
    for i, m in enumerate(todo):
        if intr.first.is_set():
            return f"interrupted before {m}", False
        if i and await wait_first(pace, intr.first):
            return f"interrupted before {m}", False
        w, h, rest = m.split("x")
        bpp, hz = rest.split("@")
        cap = asyncio.create_task(guarded(run(c, f"EXECW {GOLDEN_EXECW_S} {tool} golden {w} {h} "
                                                 f"{bpp} {hz}", timeout=bound)))
        first = asyncio.create_task(intr.first.wait())
        await asyncio.wait({cap, first}, return_when=asyncio.FIRST_COMPLETED)
        first.cancel()
        if not cap.done():
            # a signal while vcrctl golden runs: it holds the mode for the
            # floor and exits by itself - let it, bounded by its own budget
            print(f"  {m}: waiting up to {bound} s for this capture to finish on its own",
                  flush=True)
            second = asyncio.create_task(intr.second.wait())
            await asyncio.wait({cap, second}, timeout=bound, return_when=asyncio.FIRST_COMPLETED)
            second.cancel()
            if not cap.done():
                cap.cancel()
                try:
                    await cap
                except BaseException:
                    pass
                result["captures"].append({"cmd": "golden", "ok": False, "mode": m,
                                           "error": "not heard to finish"})
                return f"{m}: interrupted, and the capture was not heard to finish", True
        exc = cap.exception()
        if exc is not None:
            # a host timeout, a dropped connection, a KeyboardInterrupt that
            # reached the await: the answer is lost, the capture may not be
            if isinstance(exc, Interrupted):
                intr.interrupted = True
            intr.stop()
            result["captures"].append({"cmd": "golden", "ok": False, "mode": m,
                                       "error": f"{type(exc).__name__}: {exc}"})
            return f"{m}: {type(exc).__name__}: {exc}", True
        txt = cap.result()
        out = last_json(txt)
        if ms.EXECW_TIMED_OUT in txt:
            # the agent's tree kill is best effort (and unpaced): vcrctl may
            # still hold the mode. Not retried, and no further capture
            failure = (f"{m}: the agent's EXECW timed out after {GOLDEN_EXECW_S} s "
                       "(vcrctl hung; its tree kill is best effort)")
            result["captures"].append({"cmd": "golden", "ok": False, "mode": m,
                                       "error": failure, "partial": out})
            print(f"  {m}: TIMED OUT")
            return failure, True
        ok = bool(out and out.get("ok"))
        print(f"  {m}: {'ok' if ok else 'FAILED'} {'' if ok else out}")
        result["captures"].append(out or {"cmd": "golden", "ok": False, "mode": m})
        halt = ms.lab_halt(out or {})
        if halt:
            # vcrctl refused the switch - something else is switching the
            # box: no further capture asks it to
            return f"{m}: {halt}", False
        if intr.first.is_set():
            return f"interrupted after {m}", False
    return None, False


async def restore_if_needed(a, tool, original):
    """The one restore at the end: none when the box shows its desktop mode
    already - each capture's CDS_FULLSCREEN mode went back as its vcrctl
    exited, and a restore is one more switch - else ONE, under EXECW with a
    budget, on a fresh connection, never retried (a loop of them is a burst
    of re-syncs)."""
    call = fresh(a)
    now = (await ms.box_json(call, f"EXEC {tool} modes") or {}).get("current")
    if original and now == original:
        return {"ok": True, "skipped": f"{now} is the desktop mode already - no restore"}
    return await ms.restore_once(call, tool)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--label", required=True, help="which driver produced it")
    ap.add_argument("--modes", help="comma list WxHxBPP@HZ (default: a sweep - ~123 "
                    "captures, needs --allow-many)")
    ap.add_argument("--pace", type=float, default=5.0, help="seconds between captures (min 3)")
    ap.add_argument("--max-live", type=int, default=12, help="most captures without --allow-many")
    ap.add_argument("--allow-many", action="store_true",
                    help="more than --max-live captures (NOT on a CRT you care about)")
    ap.add_argument("--monitor-info", help="a `vcrctl info` saved from OUR driver on this box: "
                    "the monitor's EDID ranges when the installed driver is not ours (used "
                    "only when the registry says that monitor is the one on the box)")
    ap.add_argument(ms.CHECKED_FLAG, action="store_true",
                    help="with --monitor-info: go on when the box's registry cannot confirm the "
                         "monitor - only after looking at it (a different one is still refused)")
    ap.add_argument("--exe", default=str(HERE.parent / "out" / "vcrctl.exe"))
    ap.add_argument("--dir", default=DEFAULT_DIR)
    ap.add_argument("--out")
    ap.add_argument("--probe", help="vcrprobe.sys to load for the VGA file + PCI config")
    ap.add_argument("--pci", default="3:0:0123,2:0:0",
                    help="bus:dev:fns to dump with the probe (default: the V5 6000's "
                         "four chip functions on bus 3 and its HiNT bridge on bus 2)")
    a = ap.parse_args()
    a.pci = [(int(b), int(d), [int(f) for f in fns])
             for b, d, fns in (x.split(":") for x in a.pci.split(",") if x)]
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
