#!/usr/bin/env python3
"""sli_golden_sweep.py - live multi-chip captures for several SLI/AA configs.

One sli_golden.py capture per config, and - with --reboot, which is the
default - ONE CLEAN BOOT PER CONFIG. That is the vendor driver's ground rule
(docs/v56k-benchmark-plan.md: a second topology write in one boot wedges
AmigaMerlin), and a golden taken any other way is a golden of a state the
vendor never runs in. Our driver sets SLI up per Glide session, so
--no-reboot is how to test that claim, not how to take a reference.

    # the vendor's goldens (AmigaMerlin installed):
    sli_golden_sweep.py 192.168.1.124 --label amigamerlin-3.1-r11 --cfgs 1,2,3,4,6,7,8
    # ours, then the diff per config:
    sli_golden_sweep.py 192.168.1.124 --label vcrkmd-sli --cfgs 1,2,3,4,6,7,8 \
        --compare amigamerlin-3.1-r11

Each config: write SSTH3_SLI_AA_CONFIGURATION (the bench runner's own
mechanism, read back), safe-reboot (PXE hold armed), wait for the agent, run
sli_golden.py. A config whose capture fails is recorded and the sweep goes on;
the box is rebooted before the next one anyway.
"""
import argparse
import asyncio
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
REPO = KMD.parents[1]
sys.path.insert(0, str(REPO / "scripts" / "benchmarks"))
sys.path.insert(0, str(HERE))
import v56k_bench as vb     # noqa: E402
import deploy_box as db     # noqa: E402


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


async def set_cfg(ip, cfg):
    box = vb.Box(ip)
    inst, _ = await vb.find_display_instance(box)
    await vb.apply_aa_config(box, vb.GLIDE_KEY_TMPL.format(inst=inst), cfg)


async def main_async(a):
    rows = []
    agent = db.Agent(a.host)
    for cfg in a.cfgs:
        log(f"===== cfg {cfg} =====")
        try:
            await set_cfg(a.host, cfg)
        except Exception as e:
            log(f"  could not write cfg {cfg}: {e}")
            rows.append((cfg, "cfg-write-failed"))
            continue
        if a.reboot:
            if not db.safe_reboot(a.host):
                log("  safe-reboot REFUSED - stopping")
                rows.append((cfg, "reboot-refused"))
                break
            took = await db.wait_back(agent)
            if took is None:
                log("  box did not come back - stopping")
                rows.append((cfg, "not-back"))
                break
            log(f"  back after {took:.0f}s")
        r = subprocess.run([sys.executable, str(HERE / "sli_golden.py"), a.host,
                            "--label", a.label, "--cfg", str(cfg), "--w", str(a.w),
                            "--h", str(a.h), "--depth", str(a.depth),
                            "--settle", str(a.settle), "--samples", "2"],
                           capture_output=True, text=True, timeout=900)
        out = (r.stdout + r.stderr).strip()
        log("  " + out.replace("\n", "\n  ")[-600:])
        golden = KMD / "golden" / f"sli_{a.label}_cfg{cfg}_{a.host}.json"
        if r.returncode or not golden.exists():
            rows.append((cfg, f"capture-failed rc={r.returncode}"))
            continue
        res = "captured"
        if a.compare:
            ref = KMD / "golden" / f"sli_{a.compare}_cfg{cfg}_{a.host}.json"
            if ref.exists():
                c = subprocess.run([sys.executable, str(HERE / "sli_compare.py"), str(ref),
                                    str(golden)], capture_output=True, text=True)
                n = c.stdout.strip().splitlines()[-1] if c.stdout.strip() else "?"
                (KMD / "evidence" / "sli_compare").mkdir(parents=True, exist_ok=True)
                (KMD / "evidence" / "sli_compare" / f"{a.label}_vs_{a.compare}_cfg{cfg}.txt"
                 ).write_text(c.stdout)
                res += f", vs {a.compare}: {n}"
            else:
                res += f", no {a.compare} golden for cfg {cfg}"
        rows.append((cfg, res))
    log("")
    for cfg, what in rows:
        log(f"  cfg {cfg}: {what}")
    return 0 if all(w.startswith("captured") for _, w in rows) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--label", required=True)
    ap.add_argument("--cfgs", default="1,2,3,4,6,7,8",
                    type=lambda s: [int(x) for x in s.split(",") if x.strip()])
    ap.add_argument("--no-reboot", dest="reboot", action="store_false")
    ap.add_argument("--compare", help="label of the reference goldens to diff against")
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=480)
    ap.add_argument("--depth", type=int, default=16)
    ap.add_argument("--settle", type=int, default=20)
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
