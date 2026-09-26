#!/usr/bin/env python3
"""glidelab_sweep.py - glidelab's fill + bands tests across SLI/AA configs.

    # a kernel driver's fill rate and band correctness per config, one clean
    # boot each (the vendor driver's rule), results appended as JSON lines;
    # the vendor driver cannot be asked for the EDID, so --monitor-info:
    glidelab_sweep.py 192.168.1.124 --label amigamerlin-3.1-r11 --cfgs 0,5,7,8 \\
        --res 1024x768 --monitor-info ours-info.json
    # ours sets SLI up per Glide session, so it can be swept in one boot:
    glidelab_sweep.py 192.168.1.124 --label vcrkmd --cfgs 0,5,7,8 --res 1024x768 --no-reboot

Per config: fill (flat and blended) and bands at each --res, refresh pinned
(--refresh, default 60 - compare kernels only at the same refresh). Output:
evidence/glidelab/<label>.jsonl, one RESULT per line with label and cfg.

EVERY GLIDE SESSION IS TWO MONITOR RE-SYNCS (grSstWinOpen into the mode,
grSstWinClose back out) and a config is three sessions per --res: the
default --cfgs x --res is 24 sessions, 48 re-syncs - the kind of burst that
clicked .124's 1998 CRT ~250 times on 2026-09-26. So:
  - every --res at --refresh is checked on the host against the monitor's
    EDID ranges (mode_sweep.py's gate: the driver's own mode math, no slack
    at the top) before the first session and again after every reboot;
    under the vendor driver that needs --monitor-info, a `vcrctl info` saved
    from OUR driver on the same box and monitor, used only once the box's
    registry says that monitor is the one on it (when it cannot say:
    --i-have-checked-the-monitor);
  - --pace seconds (default 5, never under 3) before every session; glidelab
    holds each mode for the floor itself as well (tools/vcr_pace.h);
  - more than --max-resyncs planned re-syncs (default 24) is refused without
    --allow-many. Reboots come on top: each is its own POST and boot re-syncs;
  - a --refresh glidelab.c has no GR_REFRESH code for is refused (glidelab
    would open at 60 Hz, silently - a mode the gate never checked);
  - every session's EXECW covers the pace gate's worst case for its open
    and close on top of --timeout (glidelab_run.session_budget); a budget
    past the agent's clamp is refused before anything runs;
  - a session that TIMES OUT or WEDGES (the EXECW's timeout, no answer, a
    glidelab.exe that outlives its run and cannot be confirmed gone) ends
    the SWEEP, not just its cell, once glidelab_run's cleanup has run
    (a survivor pace-killed by PID, the stamp, the pace): a board that just
    hung a session is not handed the next one. So does a session that lost
    the focus, found the switch lock busy (something else is switching the
    box) or opened a refresh other than asked.
"""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
sys.path.insert(0, str(HERE))
import deploy_box as db      # noqa: E402
import glidelab_run as gr    # noqa: E402
import mode_sweep as ms      # noqa: E402  (the monitor gate every switching tool shares)
import sli_golden_sweep as sw  # noqa: E402

# one Glide session each, per config and --res: the plan and the loop read
# this same tuple, so the count printed is the count the monitor gets
RUNS = (("fill", False), ("fill", True), ("bands", False))


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


class Args:
    pass


async def gate(a, agent, box, modes):
    """(ranges, calc, None) when every mode may be switched to, else (.., why).
    Asked again after each reboot: the driver reads the EDID at boot, and a
    DDC read that failed there leaves it on the envelope or the default (a
    range that is not this monitor's) - and a monitor swapped during the
    reboot is caught by the registry check on --monitor-info."""
    live, _ = await agent.vcrctl("info", a.tool)
    return await ms.gate_on_box(box.cmd, a.tool, modes, a.monitor_info,
                                a.i_have_checked_the_monitor, live=live)


async def main_async(a):
    out = KMD / "evidence" / "glidelab"
    out.mkdir(parents=True, exist_ok=True)
    dest = out / f"{a.label}.jsonl"
    pace = max(a.pace, 3.0)
    sessions = len(a.cfgs) * len(a.res) * len(RUNS)
    if 2 * sessions > a.max_resyncs and not a.allow_many:
        ms.refuse(f"{len(a.cfgs)} configs x {len(a.res)} res x {len(RUNS)} runs = {sessions} Glide "
                  f"sessions, {2 * sessions} monitor re-syncs; the limit is {a.max_resyncs} "
                  "(fewer --cfgs / --res, or --allow-many)")
        return 2
    why = ms.budget_refusal(gr.session_budget("fill", 1, a.timeout),
                            f"a glidelab session with --timeout {a.timeout}")
    if why:
        ms.refuse(why)
        return 2
    modes = gr.glide_modes(a.res, a.refresh)
    agent = db.Agent(a.host)
    box = gr.vb.Box(a.host)
    rng, calc, why = await gate(a, agent, box, modes)
    if why:
        ms.refuse(why)
        return 2
    log(f"plan: {sessions} Glide sessions, {2 * sessions} monitor re-syncs, {pace:.0f} s before "
        f"each" + (f"; plus {len(a.cfgs)} reboot(s)" if a.reboot else ""))
    log(f"  monitor {ms.describe(rng)}")
    for m in modes:
        log(f"  {m:>18}  {ms.rates(calc, m)}")
    await box.text(rf"MKDIR {gr.DIR}")
    await box.upload(rf"{gr.DIR}\glidelab.exe", (KMD / "out" / "glidelab.exe").read_bytes())
    for cfg in a.cfgs:
        log(f"===== cfg {cfg} =====")
        await sw.set_cfg(a.host, cfg)
        if a.reboot:
            if not db.safe_reboot(a.host) or await db.wait_back(agent) is None:
                log("  reboot failed - stopping")
                return 2
            rng, calc, why = await gate(a, agent, box, modes)
            if why:
                ms.banner("STOPPED - no further Glide session", why)
                return 2
        for res in a.res:
            for mode, blend in RUNS:
                await asyncio.sleep(pace)
                r_args = Args()
                r_args.res, r_args.refresh, r_args.cfg = res, a.refresh, cfg
                r_args.frames, r_args.layers, r_args.cycles = a.frames, a.layers, 1
                r_args.blend, r_args.origin, r_args.glide = blend, None, a.glide
                r_args.timeout, r_args.tool, r_args.pace = a.timeout, a.tool, pace
                r_args.spare = None
                try:
                    r = await gr.run_mode(box, r_args, mode)
                except Exception as e:
                    # run_mode handles the EXECW itself, so this is the agent
                    # failing around it - and its cleanup never ran: run it
                    # here (it tolerates a silent agent) before stopping. It
                    # kills only what was not running before the session (the
                    # list run_mode took; none taken yet - it kills nothing)
                    r = {"mode": mode, "error": f"{type(e).__name__}: {e}", "wedged": True,
                         "cleanup": await ms.reap(box.cmd, gr.IMAGES, a.tool, pace,
                                                  spare=r_args.spare)}
                r.update({"label": a.label, "cfg": cfg, "res": res, "refresh": a.refresh,
                          "taken": time.strftime("%Y-%m-%dT%H:%M:%S")})
                log(f"  {json.dumps(r)}")
                with open(dest, "a") as f:
                    f.write(json.dumps(r) + "\n")
                if r.get("timed_out") or r.get("wedged") or r.get("halt"):
                    ms.banner("SWEEP STOPPED - no further Glide session",
                              f"cfg {cfg} {res} {mode}: "
                              + ("timed out" if r.get("timed_out") else "wedged"
                                 if r.get("wedged") else r["halt"])
                              + " (cleaned up: " + json.dumps(r.get("cleanup"))[:200] + ")")
                    return 3
                if "error" in r and not await agent.alive():
                    log("  agent gone - stopping")
                    return 3
    log(f"-> {dest}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--label", required=True)
    ap.add_argument("--cfgs", default="0,5,7,8",
                    type=lambda s: [int(x) for x in s.split(",") if x.strip()])
    ap.add_argument("--res", default="1024x768,1600x1200", type=lambda s: s.split(","))
    ap.add_argument("--refresh", type=int, default=60,
                    help=f"one of {', '.join(map(str, gr.GLIDE_HZ))} (glidelab.c's table)")
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--layers", type=int, default=8)
    ap.add_argument("--glide", default=gr.OUR_GLIDE)
    ap.add_argument("--timeout", type=int, default=180,
                    help="seconds of WORK per session; its EXECW adds the pace gate's worst case "
                         "for the open and the close")
    ap.add_argument("--no-reboot", dest="reboot", action="store_false")
    ap.add_argument("--pace", type=float, default=5.0, help="seconds before every session (min 3)")
    ap.add_argument("--max-resyncs", type=int, default=24,
                    help="most planned monitor re-syncs (2 per Glide session) without --allow-many")
    ap.add_argument("--allow-many", action="store_true",
                    help="more than --max-resyncs (NOT on a CRT you care about)")
    ap.add_argument("--monitor-info", help="a `vcrctl info` saved from OUR driver on this box: "
                    "the monitor's EDID ranges when the installed driver is not ours (used "
                    "only when the registry says that monitor is the one on the box)")
    ap.add_argument(ms.CHECKED_FLAG, action="store_true",
                    help="with --monitor-info: go on when the box's registry cannot confirm the "
                         "monitor - only after looking at it (a different one is still refused)")
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe")
    a = ap.parse_args()
    why = gr.bad_refresh(a.refresh)
    if why:
        ap.error(why)
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
