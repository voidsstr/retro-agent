#!/usr/bin/env python3
"""d3dprobe_run.py - run d3dprobe.exe (tools/d3dprobe.c) on a box or a test bed.

    d3dprobe_run.py 127.0.0.1 --port 19920 caps
    d3dprobe_run.py 127.0.0.1 --port 19920 render [--full --res 800x600 --bpp 16]
    d3dprobe_run.py 192.168.1.124 perf --full --res 800x600 --frames 300

Uploads out/d3dprobe.exe to C:\\vcr\\d3dprobe\\, runs it through `start /wait`
(a normal desktop window - a device needs one) under EXECW, and prints the
RESULT json from the flushed log. Exit 1 when a check failed or no RESULT.

`render` and `perf` with --full create a fullscreen device at --res x --bpp
with the default refresh - the driver / XP picks the rate - so before
anything is uploaded the mode is checked on the host against the monitor's
EDID ranges at the HIGHEST refresh the driver lists there (mode_sweep.py's
gate: `vcrctl info` and `vcrctl modes`; under another driver --monitor-info,
used once the box's registry says that monitor is the one on it, or with
--i-have-checked-the-monitor when it cannot say). The EXECW adds the pace
gate's worst case for the two switches (a busy switch lock, then the floor)
to --timeout, and a budget past the agent's clamp is refused.

A run that does not end civilly - the EXECW's timeout marker in the reply, no
reply at all, or no RESULT line (a crash skips the lab's exit hold) - is
cleaned up before this returns, and fails: PROCLIST, a d3dprobe.exe that was
not running before this launch killed BY PID through `vcrctl pace-kill`
(paced, the revert stamped), confirmed gone, and the pace. A lab that lost
the focus ("focus_lost") or found the switch lock busy ('pace lock busy')
fails too.
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
sys.path.insert(0, str(KMD.parents[1]))
sys.path.insert(0, str(HERE))
from client.retro_protocol import RetroConnection  # noqa: E402
import mode_sweep as ms  # noqa: E402  (the cleanup every tool that launches a lab shares)

DIR = r"C:\vcr\d3dprobe"


async def call(a, cmd, payload=None, timeout=60):
    c = RetroConnection(a.host, a.port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        if payload is not None:
            st, d = await c.send_command(cmd, binary_payload=payload, timeout=timeout)
        else:
            st, d = await c.send_command(cmd, timeout=timeout)
        return d
    finally:
        await c.close()


async def call_st(a, cmd, timeout=60):
    """(status, text) on a fresh connection - mode_sweep.reap()'s call."""
    c = RetroConnection(a.host, a.port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        st, d = await c.send_command(cmd, timeout=timeout)
        return st, d.decode("latin1", "replace")
    finally:
        await c.close()


def refused(a, why):
    """A run that may not start: the banner, and a RESULT-shaped line the
    battery records (rc 2 - nothing was uploaded or switched)."""
    ms.refuse(why)
    print(json.dumps({"mode": a.mode, "error": f"REFUSED: {why}", "refused": True}))
    return 2


async def main_async(a):
    box = lambda cmd, t: call_st(a, cmd, t)  # noqa: E731
    # caps creates no device; render / perf go fullscreen only with --full
    full = bool(a.full) and a.mode != "caps"
    budget = ms.execw_budget(2 if full else 0, a.timeout)
    why = ms.budget_refusal(budget, f"d3dprobe {a.mode} with --timeout {a.timeout}")
    if why:
        return refused(a, why)
    if getattr(a, "test_bed", False) and not ms.is_loopback(a.host):
        return refused(a, f"--test-bed is for the VM test bed behind a loopback port; {a.host} "
                          "is a real box with a real monitor")
    if full and getattr(a, "test_bed", False):
        # the QEMU bed's Bochs VGA has no EDID (mon_src 0) and no tube to
        # protect; it is still paced by vcr_pace.h on the box
        print("plan: test bed - no monitor gate (loopback only)", flush=True)
    elif full:
        try:
            w, h = (int(v) for v in a.res.lower().split("x"))
        except ValueError:
            return refused(a, f"--res {a.res} is not WxH")
        rng, calc, modes, why = await ms.fullscreen_gate(
            box, a.tool, [(w, h, a.bpp, False)], getattr(a, "monitor_info", None),
            getattr(a, "i_have_checked_the_monitor", False))
        if why:
            return refused(a, why)
        print(f"plan: d3dprobe {a.mode} fullscreen at {modes[0]} (the highest refresh the driver "
              f"lists there: the default refresh lets it pick)  {ms.rates(calc, modes[0])}"
              f"\n  monitor {ms.describe(rng)}", flush=True)
    await call(a, rf"MKDIR {DIR}")
    await call(a, rf"UPLOAD {DIR}\d3dprobe.exe", (KMD / "out" / "d3dprobe.exe").read_bytes())
    log = rf"{DIR}\{a.mode}.log"
    await call(a, rf'EXEC cmd /c del /f /q "{log}"')
    args = f"{a.mode} --res {a.res} --bpp {a.bpp} --frames {a.frames} --log {log}"
    if a.full:
        args += " --full"
    if a.novsync:
        args += " --novsync"
    if a.tests:
        args += f" --tests {a.tests}"
    # a d3dprobe.exe already running is not this run's: a cleanup never kills
    # it (None - PROCLIST did not answer - kills nothing, and says so)
    image = "d3dprobe.exe"
    spare = await ms.pids_of(box, [image])
    trouble = marker = None
    try:
        reply = await call(a, f'EXECW {budget} cmd /c start "d3dprobe" /wait '
                           f'"{DIR}\\d3dprobe.exe" {args}', timeout=budget + ms.HOST_SLACK_S)
        reply = reply.decode("latin1", "replace")
        marker = next((ln.strip() for ln in reply.splitlines() if ms.EXECW_TIMED_OUT in ln), None)
        if marker:
            trouble = (f"the agent's EXECW timed out after {budget} s and tried to kill "
                       "the tree (best effort - the lab may still hold its mode)")
    except Exception as e:  # the host gave up, or the connection dropped
        trouble = f"EXECW did not answer: {type(e).__name__}: {e}"
    try:
        text = (await call(a, f"DOWNLOAD {log}")).decode("latin1", "replace")
    except Exception as e:
        text = f"(log not read: {type(e).__name__}: {e})"
    res = None
    for ln in text.splitlines():
        if ln.startswith("RESULT "):
            try:
                res = json.loads(ln[7:])
            except ValueError:  # a garbled RESULT is not a pass
                res = {"mode": a.mode, "error": "unparsable RESULT line", "raw": ln}
    if res is None:
        res = {"mode": a.mode, "error": "no RESULT line", "log_tail": text.splitlines()[-8:]}
    if trouble:
        res["error"] = trouble + (f" (and: {res['error']})" if "error" in res else "")
        res["timed_out"] = True
        if marker:
            res["execw"] = marker     # the agent's own words: silicon_battery keys on them
    halt = ms.lab_halt(res, text)
    if halt:
        # the screen or the pacing was not the lab's alone: a failure, and
        # the battery stops switching on it
        res["halt"] = halt
        res["error"] = halt + (f" (and: {res['error']})" if "error" in res else "")
    if trouble or "RESULT line" in str(res.get("error", "")):
        # it timed out, or died without a RESULT (a crash skips the exit
        # hold, so XP reverted its mode unstamped): nothing else switches
        # until it is gone and the box's pace file says "just now"
        res["cleanup"] = await ms.reap(box, [image], a.tool, ms.PACE_FLOOR_S, spare=spare)
    print(json.dumps(res))
    if a.verbose:
        print(text)
    return 0 if "error" not in res and not res.get("fail") else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("mode", choices=("caps", "render", "perf"))
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--bpp", type=int, default=16)
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--novsync", action="store_true")
    ap.add_argument("--tests", default="")
    ap.add_argument("--timeout", type=int, default=180,
                    help="seconds of WORK; the EXECW adds the pace gate's worst case per switch")
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe",
                    help="vcrctl.exe on the box (info and modes for the gate; pace-kill and "
                         "pace-mark after a run that did not end civilly)")
    ap.add_argument("--test-bed", action="store_true",
                    help="the QEMU VM test bed (loopback host only): no EDID, no monitor gate")
    ap.add_argument("--monitor-info", help="--full under another driver: a `vcrctl info` saved "
                    "from OUR driver on this box (used only when the registry says that monitor "
                    "is the one on the box)")
    ap.add_argument(ms.CHECKED_FLAG, action="store_true",
                    help="with --monitor-info: go on when the box's registry cannot confirm the "
                         "monitor - only after looking at it (a different one is still refused)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
