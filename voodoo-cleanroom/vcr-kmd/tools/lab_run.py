#!/usr/bin/env python3
"""lab_run.py - run one of the self-checking lab programs on a box or test bed.

    lab_run.py gdilab 127.0.0.1 --port 19920 [-- --rounds 5]
    lab_run.py ddlab  192.168.1.124 -- blt --res 800x600 --bpp 16

Uploads out/<lab>.exe to C:\\vcr\\<lab>\\, runs it through `start /wait` (a
normal desktop window) under EXECW with --log, and prints the RESULT json
from the flushed log. Exit 1 on an error, a failed check ("fail" > 0) or any
"bad*" count > 0 - the labs report counts, never a bare pass.

A lab that takes the screen - ddlab flip/blt, d3dprobe --full, every glidelab
mode - is gated like its own runner before anything is uploaded: the mode it
will open is checked on the host against the monitor's EDID ranges
(mode_sweep.py's gate). ddlab and d3dprobe switch at refresh 0, so the
HIGHEST refresh the driver lists at --res x --bpp is checked; glidelab opens
--res at 16 bpp and --refresh (default 60). Under another driver that needs
--monitor-info, used once the box's registry says that monitor is the one on
it (or with --i-have-checked-the-monitor when it cannot say). A lab this
script does not know is refused: nothing says whether it switches. The EXECW
adds the pace gate's worst case for every switch (a busy switch lock, then
the floor) to --timeout, and a budget past the agent's clamp is refused.

A run that does not end civilly - the EXECW's timeout marker in the reply, no
reply at all, or no RESULT line (a crash skips the lab's exit hold) - is
cleaned up before this returns, and fails: PROCLIST, a <lab>.exe that was not
running before this launch killed BY PID through `vcrctl pace-kill` (paced,
the revert stamped), confirmed gone, and the pace. A lab that lost the focus
("focus_lost"), found the switch lock busy ('pace lock busy') or - glidelab -
opened another refresh than asked fails too.
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


# the options each lab reads with a value (its own argv loop); anything else
# that starts with '-' is a switch, anything that does not is the mode
VALUE_OPTS = {"--res", "--bpp", "--frames", "--pace", "--log", "--tests", "--refresh", "--cfg",
              "--layers", "--cycles", "--dll", "--origin", "--rounds"}
# the labs this runner knows, and their C defaults (ddlab.c / d3dprobe.c /
# glidelab.c): what a run that names no --res / --bpp / --refresh opens
LABS = {"gdilab": {}, "ddlab": {"mode": "caps"}, "d3dprobe": {"mode": "caps"},
        "glidelab": {"mode": "fill", "--refresh": "60", "--cycles": "2"}}


def lab_argv(lab, rest):
    """The mode and options the lab will read from `rest`, as its own argv
    loop does, over its defaults."""
    o = {"mode": None, "--res": "640x480", "--bpp": "16", "flags": set()}
    o.update(LABS.get(lab, {}))
    i = 0
    while i < len(rest):
        x = rest[i]
        if x in VALUE_OPTS and i + 1 < len(rest):
            o[x] = rest[i + 1]
            i += 2
            continue
        if x.startswith("-"):
            o["flags"].add(x)
        else:
            o["mode"] = x
        i += 1
    return o


def lab_plan(lab, rest):
    """(switches, gate specs, fixed modes, None) for one run, or (.., why it
    may not run). Specs are driver_picks'; fixed modes are gated as named."""
    if lab not in LABS:
        return 0, [], [], (f"{lab} is not a lab this runner knows (gdilab, ddlab, d3dprobe, "
                           "glidelab) - nothing says whether it switches the display mode, so "
                           "it is not run blind")
    o = lab_argv(lab, rest)
    try:
        w, h = (int(v) for v in o["--res"].lower().split("x"))
        bpp = int(o["--bpp"])
    except ValueError:
        return 0, [], [], f"--res {o['--res']} / --bpp {o['--bpp']} is not WxH / a depth"
    if lab == "ddlab" and o["mode"] != "caps":
        return 2, [(w, h, bpp, False)], [], None
    if lab == "d3dprobe" and "--full" in o["flags"] and o["mode"] != "caps":
        return 2, [(w, h, bpp, False)], [], None
    if lab == "glidelab":
        import glidelab_run as gr    # only here: it pulls in the bench runner
        try:
            hz, cycles = int(o["--refresh"]), int(o["--cycles"])
        except ValueError:
            return 0, [], [], "--refresh / --cycles is not a number"
        why = gr.bad_refresh(hz)
        if why:
            return 0, [], [], why
        if cycles > gr.MAX_CYCLES:
            return 0, [], [], f"--cycles {cycles}: at most {gr.MAX_CYCLES} (each is two re-syncs)"
        return (gr.glide_switches(o["mode"], cycles), [],
                gr.glide_modes([o["--res"]], hz), None)
    return 0, [], [], None


def verdict(res):
    if "error" in res or res.get("fail"):
        return 1
    return 1 if any(v for k, v in res.items() if k.startswith("bad") and isinstance(v, int)) else 0


def refused(a, why):
    """A run that may not start: the banner, and a RESULT-shaped line the
    battery records (rc 2 - nothing was uploaded or switched)."""
    ms.refuse(why)
    print(json.dumps({"lab": a.lab, "error": f"REFUSED: {why}", "refused": True}))
    return 2


async def main_async(a, rest):
    box = lambda cmd, t: call_st(a, cmd, t)  # noqa: E731
    switches, specs, fixed, why = lab_plan(a.lab, rest)
    if why:
        return refused(a, why)
    budget = ms.execw_budget(switches, a.timeout)
    why = ms.budget_refusal(budget, f"{a.lab} with --timeout {a.timeout}")
    if why:
        return refused(a, why)
    if getattr(a, "test_bed", False) and not ms.is_loopback(a.host):
        return refused(a, f"--test-bed is for the VM test bed behind a loopback port; {a.host} "
                          "is a real box with a real monitor")
    want_hz = None
    if (specs or fixed) and getattr(a, "test_bed", False):
        # the QEMU bed's Bochs VGA has no EDID (mon_src 0) and no tube to
        # protect; it is still paced by vcr_pace.h on the box
        print("plan: test bed - no monitor gate (loopback only)", flush=True)
        if fixed:
            want_hz = int(fixed[0].split("@")[1])
    elif specs or fixed:
        saved = getattr(a, "monitor_info", None)
        checked = getattr(a, "i_have_checked_the_monitor", False)
        if specs:
            rng, calc, modes, why = await ms.fullscreen_gate(box, a.tool, specs, saved, checked)
        else:
            modes = fixed
            rng, calc, why = await ms.gate_on_box(box, a.tool, modes, saved, checked)
            want_hz = int(modes[0].split("@")[1])     # glidelab must open exactly this
        if why:
            return refused(a, why)
        print(f"plan: {a.lab} at {modes[0]}  {ms.rates(calc, modes[0])}, {switches} switches"
              f"\n  monitor {ms.describe(rng)}", flush=True)
    d = rf"C:\vcr\{a.lab}"
    await call(a, rf"MKDIR {d}")
    await call(a, rf"UPLOAD {d}\{a.lab}.exe", (KMD / "out" / f"{a.lab}.exe").read_bytes())
    log = rf"{d}\{a.lab}.log"
    await call(a, rf'EXEC cmd /c del /f /q "{log}"')
    args = " ".join(rest + ["--log", log])
    # a <lab>.exe already running is not this run's: a cleanup never kills
    # it (None - PROCLIST did not answer - kills nothing, and says so)
    image = f"{a.lab}.exe"
    spare = await ms.pids_of(box, [image])
    trouble = marker = None
    try:
        reply = await call(a, f'EXECW {budget} cmd /c start "{a.lab}" /wait '
                           f'"{d}\\{a.lab}.exe" {args}', timeout=budget + ms.HOST_SLACK_S)
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
                res = {"lab": a.lab, "error": "unparsable RESULT line", "raw": ln}
    if res is None:
        res = {"lab": a.lab, "error": "no RESULT line", "log_tail": text.splitlines()[-8:]}
    if trouble:
        res["error"] = trouble + (f" (and: {res['error']})" if "error" in res else "")
        res["timed_out"] = True
        if marker:
            res["execw"] = marker     # the agent's own words: silicon_battery keys on them
    halt = ms.lab_halt(res, text, want_hz=want_hz)
    if halt:
        # the screen or the pacing was not the lab's alone, or Glide opened
        # a mode the gate never checked: a failure, and the battery stops
        # switching on it
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
    return verdict(res)


def main():
    argv = sys.argv[1:]
    rest = []
    if "--" in argv:
        i = argv.index("--")
        argv, rest = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("lab", help="gdilab | ddlab | d3dprobe | glidelab (another is refused: "
                    "nothing says whether it switches the display mode)")
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--timeout", type=int, default=240,
                    help="seconds of WORK; the EXECW adds the pace gate's worst case per switch")
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe",
                    help="vcrctl.exe on the box (info and modes for the gate; pace-kill and "
                         "pace-mark after a run that did not end civilly)")
    ap.add_argument("--test-bed", action="store_true",
                    help="the QEMU VM test bed (loopback host only): no EDID, no monitor gate")
    ap.add_argument("--monitor-info", help="a fullscreen lab under another driver: a `vcrctl "
                    "info` saved from OUR driver on this box (used only when the registry says "
                    "that monitor is the one on the box)")
    ap.add_argument(ms.CHECKED_FLAG, action="store_true",
                    help="with --monitor-info: go on when the box's registry cannot confirm the "
                         "monitor - only after looking at it (a different one is still refused)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    sys.exit(asyncio.run(main_async(a, rest)))


if __name__ == "__main__":
    main()
