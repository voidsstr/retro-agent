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

EVERY CAPTURE SWITCHES THE MONITOR (Quake II goes fullscreen at --w x --h, at
the highest refresh the driver lists there). The game's mode is checked on
the host against the monitor's EDID ranges before the first config is
written, and sli_golden.py checks it again after every reboot; a refusal
there (rc 2 - the driver's limits or the monitor changed across the reboot)
ENDS the sweep. Under the vendor driver - which is what a golden is taken on
- that needs --monitor-info, a `vcrctl info` saved from OUR driver on this
box, used only once the box's registry says that monitor is the one on it
(or, when it cannot say, with --i-have-checked-the-monitor).
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
import mode_sweep as ms     # noqa: E402  (the monitor gate every switching tool shares)
import sli_golden as sg     # noqa: E402


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


async def set_cfg(ip, cfg):
    box = vb.Box(ip)
    inst, _ = await vb.find_display_instance(box)
    await vb.apply_aa_config(box, vb.GLIDE_KEY_TMPL.format(inst=inst), cfg)


def gate_args(a):
    """--monitor-info / --i-have-checked-the-monitor, passed on to every
    sli_golden.py run - it gates again after each reboot."""
    return ((["--monitor-info", a.monitor_info] if a.monitor_info else [])
            + ([ms.CHECKED_FLAG] if a.i_have_checked_the_monitor else []))


async def main_async(a):
    rows = []
    agent = db.Agent(a.host)
    # the gate before the first config is written or the box rebooted: a
    # sweep the monitor cannot take is refused while it has cost nothing
    box = vb.Box(a.host)
    await box.cmd(r"MKDIR C:\vcr")         # an existing one answers an error: fine
    await box.upload(sg.TOOL, (KMD / "out" / "vcrctl.exe").read_bytes())
    rng, calc, modes, why = await sg.gate(box, a)
    if why:
        ms.refuse(why)
        return 2
    log(f"the game's mode {modes[0]}  {ms.rates(calc, modes[0])}; monitor {ms.describe(rng)}")

    async def recovered():
        # A config that wedges the display driver takes the agent with it
        # (9898 refused, 9897 accepting, SMB up); box-guardian reboots it over
        # RPC after its grace. Wait for that rather than burning the rest of
        # the list against a dead agent (which the first version did).
        if await agent.alive():
            return True
        log("  agent DEAD - the config wedged the box; waiting for the guardian's reboot")
        took = await db.wait_back(agent, limit=a.recover_wait)
        log(f"  back after {took:.0f}s" if took else "  NOT back - stopping")
        return took is not None

    for cfg in a.cfgs:
        log(f"===== cfg {cfg} =====")
        if not await recovered():
            rows.append((cfg, "box-unreachable"))
            break
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
                            "--settle", str(a.settle), "--samples", "2"] + gate_args(a),
                           capture_output=True, text=True, timeout=900)
        out = (r.stdout + r.stderr).strip()
        log("  " + out.replace("\n", "\n  ")[-600:])
        if r.returncode == 2:
            # the gate refused after the reboot: the driver's limits or the
            # monitor are not what they were - no further config switches it
            ms.banner("SWEEP STOPPED - no further capture", f"cfg {cfg}: sli_golden.py's monitor "
                      "gate refused after the reboot (see above)")
            rows.append((cfg, "gate-refused"))
            break
        golden = KMD / "golden" / f"sli_{a.label}_cfg{cfg}_{a.host}.json"
        if r.returncode or not golden.exists():
            wedged = not await agent.alive()
            rows.append((cfg, f"capture-failed rc={r.returncode}" +
                         (" - WEDGED the box (agent died)" if wedged else "")))
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
    ap.add_argument("--recover-wait", type=int, default=1500,
                    help="seconds to wait for a wedged box to come back (guardian grace + boot)")
    ap.add_argument("--monitor-info", help="a `vcrctl info` saved from OUR driver on this box: "
                    "the monitor's EDID ranges when the installed driver is not ours (used "
                    "only when the registry says that monitor is the one on the box)")
    ap.add_argument(ms.CHECKED_FLAG, action="store_true",
                    help="with --monitor-info: go on when the box's registry cannot confirm the "
                         "monitor - only after looking at it (a different one is still refused)")
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
