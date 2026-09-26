#!/usr/bin/env python3
"""glidelab_sweep.py - glidelab's fill + bands tests across SLI/AA configs.

    # a kernel driver's fill rate and band correctness per config, one clean
    # boot each (the vendor driver's rule), results appended as JSON lines:
    glidelab_sweep.py 192.168.1.124 --label amigamerlin-3.1-r11 --cfgs 0,5,7,8
    # ours sets SLI up per Glide session, so it can be swept in one boot:
    glidelab_sweep.py 192.168.1.124 --label vcrkmd --cfgs 0,1,2,3,4,5,6,7,8 --no-reboot

Per config: fill (flat and blended) and bands at each --res, refresh pinned
(--refresh, default 60 - compare kernels only at the same refresh). Output:
evidence/glidelab/<label>.jsonl, one RESULT per line with label and cfg.
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
import sli_golden_sweep as sw  # noqa: E402


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


class Args:
    pass


async def main_async(a):
    out = KMD / "evidence" / "glidelab"
    out.mkdir(parents=True, exist_ok=True)
    dest = out / f"{a.label}.jsonl"
    agent = db.Agent(a.host)
    box = gr.vb.Box(a.host)
    await box.text(rf"MKDIR {gr.DIR}")
    await box.upload(rf"{gr.DIR}\glidelab.exe", (KMD / "out" / "glidelab.exe").read_bytes())
    for cfg in a.cfgs:
        log(f"===== cfg {cfg} =====")
        await sw.set_cfg(a.host, cfg)
        if a.reboot:
            if not db.safe_reboot(a.host) or await db.wait_back(agent) is None:
                log("  reboot failed - stopping")
                return 2
        for res in a.res:
            for mode, blend in (("fill", False), ("fill", True), ("bands", False)):
                r_args = Args()
                r_args.res, r_args.refresh, r_args.cfg = res, a.refresh, cfg
                r_args.frames, r_args.layers, r_args.cycles = a.frames, a.layers, 1
                r_args.blend, r_args.origin, r_args.glide = blend, None, a.glide
                r_args.timeout = a.timeout
                try:
                    r = await gr.run_mode(box, r_args, mode)
                except Exception as e:  # a wedge costs this cell, not the sweep
                    r = {"mode": mode, "error": f"{type(e).__name__}: {e}"}
                r.update({"label": a.label, "cfg": cfg, "res": res, "refresh": a.refresh,
                          "taken": time.strftime("%Y-%m-%dT%H:%M:%S")})
                log(f"  {json.dumps(r)}")
                with open(dest, "a") as f:
                    f.write(json.dumps(r) + "\n")
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
    ap.add_argument("--refresh", type=int, default=60)
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--layers", type=int, default=8)
    ap.add_argument("--glide", default=gr.OUR_GLIDE)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--no-reboot", dest="reboot", action="store_false")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
