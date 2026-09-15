#!/usr/bin/env python3
"""
v56k_sweep.py - the full chip x AA sweep, ONE CONFIG PER BOOT.

Why it reboots between configurations
-------------------------------------
Writing SSTH3_SLI_AA_CONFIGURATION repeatedly within a single boot wedges the
AmigaMerlin display driver, on the second or third write. That is the measured
behaviour on both boxes the Voodoo 5 6000 has lived in, and it is NOT a
property of any particular configuration - cfg 5, the default, hung as a third
change and works perfectly as a first:

    .191  cfg 0 ok -> cfg 5 ok                 2 changes, fine
    .191  cfg 0 ok -> cfg 1 ok -> cfg 2 HUNG   3rd change
    .191  cfg 5 ok -> cfg 8 HUNG               2nd change
    .124  cfg 0 ok -> cfg 1 ok -> cfg 5 HUNG   3rd change
    .124  (after reboot) cfg 5 ok              1st, fine

Once the board is wedged every later cell fails for that reason and gets
blamed on its own config, which is how six false "wedges the driver" verdicts
were produced in one screening run. A reboot is the only reliable reset, so
each configuration gets a clean boot and exactly one topology write.

A RESOLUTION change is not a topology change - five resolutions at one config
run back to back cleanly - so the inner loop is free.

What it refuses to do
---------------------
- It will not reboot a box that `safe-reboot.py` refuses (unactivated XP does
  not come back: logon is blocked, the Run-key agent never starts, and there is
  no remote path in). No --ignore-activation here; that flag means "a human is
  standing at the machine", which is not true of an unattended sweep.
- It stops on the first wedge or dead agent rather than producing rows that
  describe broken hardware.

Usage:
    python3 v56k_sweep.py --host 192.168.1.124
    python3 v56k_sweep.py --host 192.168.1.124 --configs 5,6,7 --outdir DIR
"""

import argparse
import asyncio
import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
PROBE = r"C:\RETRO_AGENT\glideprobe.exe"
LABELS = {0: "1chip-noaa", 1: "1chip-2xaa", 2: "2chip-noaa", 3: "2chip-2xaa",
          4: "2chip-4xaa", 5: "4chip-noaa", 6: "4chip-2xaa", 7: "4chip-4xaa",
          8: "4chip-8xaa"}


def log(m):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {m}", flush=True)


async def _cmd(ip, command, timeout=90):
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=20.0)
    try:
        st, d = await c.send_command(command, timeout=timeout)
        return st, d.decode("ascii", errors="replace")
    finally:
        await c.close()


async def glide_key(ip):
    """The AA value lives under the instance of the adapter driving the desktop.
    It is \\0001 on .124 and was \\0000 on .191 - never hardcode it."""
    st, out = await _cmd(ip, "HWPROFILE", 60)
    prof = json.loads(out)
    inst = "0000"
    for v in prof.get("video_cards", []):
        if v.get("attached_to_desktop"):
            inst = v.get("instance", "0000")
    return (r"SYSTEM\CurrentControlSet\Control\Class"
            r"\{4D36E968-E325-11CE-BFC1-08002BE10318}"
            rf"\{inst}\Settings\Glide"), inst


async def set_aa(ip, key, cfg):
    """Write the topology, then read it back - REGWRITE answers OK for a write
    that created a subkey instead of setting the value."""
    await _cmd(ip, f"REGWRITE HKLM {key} SSTH3_SLI_AA_CONFIGURATION REG_SZ {cfg}")
    st, out = await _cmd(ip, f"REGREAD HKLM {key}")
    for v in json.loads(out).get("values", []):
        if v["name"].upper() == "SSTH3_SLI_AA_CONFIGURATION":
            return str(v["data"]).strip() == str(cfg), str(v["data"]).strip()
    return False, None


async def wait_agent(ip, timeout=900):
    """A completed PING, never a bare connect: a dead agent leaves 9897 bound
    and accepting while answering nothing."""
    await asyncio.sleep(25)
    end = time.time() + timeout
    while time.time() < end:
        try:
            st, out = await _cmd(ip, "PING", 20)
            if "PONG" in out.upper():
                return True
        except Exception:
            pass
        await asyncio.sleep(15)
    return False


async def board_ok(ip):
    """Can Glide still initialise? --noopen stops before touching the board."""
    try:
        st, out = await _cmd(
            ip, f"EXECW 90 {PROBE} --res 640x480 --noopen "
                r"--dll C:\WINDOWS\system32\glide3x.dll "
                r"--log C:\RETRO_AGENT\probe-sweep.log", 140)
    except Exception:
        return None
    if "RESULT: probe-ok-noopen" in out:
        return True
    if "timed out" in out or "grGlideInit" in out:
        return False
    return None


def reboot(ip):
    r = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "fleet" / "safe-reboot.py"), ip],
        capture_output=True, text=True, timeout=300)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def run_bench(ip, cfg, resolutions, depths, outdir, max_run):
    r = subprocess.run(
        [sys.executable, str(HERE / "v56k_bench.py"), "--host", ip,
         "--titles", "quake3", "--resolutions", resolutions, "--depths", depths,
         "--configs", str(cfg), "--allow-hazards",
         "--outdir", str(outdir), "--max-run", str(max_run)],
        capture_output=True, text=True, timeout=7200)
    return r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--configs", default="5,0,1,6,7,2,3,4,8",
                    help="order matters only for how much you learn before a "
                         "failure; each gets its own boot regardless")
    ap.add_argument("--resolutions",
                    default="640x480,800x600,1024x768,1280x960,1600x1200")
    ap.add_argument("--depths", default="16")
    ap.add_argument("--max-run", type=float, default=600.0,
                    help="high AA at 1600x1200 is genuinely slow; give it room")
    ap.add_argument("--outdir", default=None)
    a = ap.parse_args()
    cfgs = [int(x) for x in a.configs.split(",")]
    outdir = Path(a.outdir) if a.outdir else (HERE / "results" / f"v56k_sweep_{a.host}")
    outdir.mkdir(parents=True, exist_ok=True)

    log(f"full sweep on {a.host}: {len(cfgs)} config(s), one boot each")
    log(f"resolutions: {a.resolutions}")
    log(f"results -> {outdir / 'results.csv'}")

    key, inst = asyncio.run(glide_key(a.host))
    log(f"AA key: display-class instance {inst}")

    done, failed = [], []
    for n, cfg in enumerate(cfgs, 1):
        log("")
        log(f"===== [{n}/{len(cfgs)}] cfg {cfg} ({LABELS.get(cfg,'?')}) =====")

        ok, got = asyncio.run(set_aa(a.host, key, cfg))
        if not ok:
            log(f"  AA write did not stick (read back {got!r}) - skipping")
            failed.append((cfg, "aa-write-failed"))
            continue
        log(f"  AA set to {cfg} and read back")

        log("  rebooting for a clean single topology write ...")
        rok, rout = reboot(a.host)
        if not rok:
            log("  safe-reboot REFUSED - stopping the sweep:")
            for line in rout.splitlines()[:6]:
                log(f"    {line}")
            failed.append((cfg, "reboot-refused"))
            break
        if not asyncio.run(wait_agent(a.host)):
            log("  the box did not come back - stopping.")
            failed.append((cfg, "no-agent-after-reboot"))
            break
        log("  agent back")

        h = asyncio.run(board_ok(a.host))
        if h is False:
            log("  BOARD WEDGED before any measurement - stopping.")
            failed.append((cfg, "board-wedged-on-boot"))
            break
        log(f"  board health: {'ok' if h else 'unknown'}")

        rc, out = run_bench(a.host, cfg, a.resolutions, a.depths, outdir, a.max_run)
        for line in out.splitlines():
            if "->" in line or "!!" in line or "WEDGED" in line or "EXCLUDED" in line:
                log(f"    {line.strip()}")
        if rc == 0:
            done.append(cfg)
        else:
            log(f"  bench exited {rc} for cfg {cfg}")
            failed.append((cfg, f"bench-rc-{rc}"))
            if rc in (3, 4):        # dead agent / wedged board
                log("  stopping the sweep - the box needs attention.")
                break

    log("")
    log(f"sweep finished: {len(done)} config(s) measured {done}")
    if failed:
        log(f"not measured: {failed}")
    log(f"results: {outdir / 'results.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
