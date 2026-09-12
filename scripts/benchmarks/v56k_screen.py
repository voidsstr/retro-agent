#!/usr/bin/env python3
"""
v56k_screen.py - find out which SSTH3_SLI_AA_CONFIGURATION values this board
will actually accept, WITHOUT risking the machine on a game.

Why this is not just "run the benchmark and see"
------------------------------------------------
Two of the nine configurations the 3dfx Tools control panel offers on the
Voodoo 5 6000 at .191 wedge the display driver and take the retro agent down
with it - cfg 8 (4-chip 8x AA) and cfg 2 (2-chip SLI). Each discovery cost a
trip to the machine, because on XP nothing restarts the agent: the Run key
only fires at logon.

A console child is different. When glideprobe hung at grSstWinOpen, EXECW
tree-killed it and the agent survived every single time - eight runs, eight
recoveries. A fullscreen game holding an exclusive Glide context cannot be
recovered that way. So the cheap, safe way to learn which topologies are valid
is to ask Glide directly, in a killable process, and only then spend a
benchmark grid on the ones that answered.

It reports three outcomes and keeps them distinct:
    open      grSstWinOpen returned a context - safe to benchmark
    refused   grSstWinOpen returned 0 - the driver declined, cleanly
    hung      no RESULT line: the probe died inside Glide. NOT safe.

"refused" and "hung" are both failures to render, and conflating them would
lose the only thing that matters operationally: a clean refusal costs nothing,
a hang costs a walk to the machine.

Usage:
    python3 v56k_screen.py --host 192.168.1.191 [--res 640x480]
                           [--configs 0,1,2,3,4,5,6,7,8] [--dll <path>]
"""

import argparse
import asyncio
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from client.retro_protocol import RetroConnection, RetroProtocolError  # noqa: E402

SECRET = "retro-agent-secret"
GLIDE_KEY = (r"SYSTEM\CurrentControlSet\Control\Class"
             r"\{4D36E968-E325-11CE-BFC1-08002BE10318}\0000\Settings\Glide")
PROBE = r"C:\RETRO_AGENT\glideprobe.exe"

LABELS = {0: "1chip-noaa", 1: "1chip-2xaa", 2: "2chip-noaa", 3: "2chip-2xaa",
          4: "2chip-4xaa", 5: "4chip-noaa", 6: "4chip-2xaa", 7: "4chip-4xaa",
          8: "4chip-8xaa"}


async def agent_alive(ip, tries=3):
    """Protocol PING, not a connect: when the agent dies its alt listener on
    9897 stays bound and keeps accepting while answering nothing."""
    for _ in range(tries):
        try:
            c = RetroConnection(ip, 9898)
            await c.connect(SECRET, timeout=15.0)
            try:
                st, d = await c.send_command("PING", timeout=20)
                if b"PONG" in d:
                    return True
            finally:
                await c.close()
        except Exception:
            pass
        await asyncio.sleep(5)
    return False


async def screen_one(ip, cfg, res, dll, timeout_s):
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=20.0)
    try:
        async def cmd(s, t=120):
            st, d = await c.send_command(s, timeout=t)
            return st, d.decode("ascii", errors="replace")

        # Apply, then READ BACK - REGWRITE answers OK for a write that created
        # a subkey instead of setting the value.
        await cmd(f"REGWRITE HKLM {GLIDE_KEY} SSTH3_SLI_AA_CONFIGURATION REG_SZ {cfg}")
        st, out = await cmd(f"REGREAD HKLM {GLIDE_KEY}")
        got = None
        for v in json.loads(out).get("values", []):
            if v["name"].upper() == "SSTH3_SLI_AA_CONFIGURATION":
                got = str(v["data"]).strip()
        if got != str(cfg):
            return {"cfg": cfg, "verdict": "aa-apply-failed", "detail": f"read back {got!r}"}

        log = rf"C:\RETRO_AGENT\screen-cfg{cfg}.log"
        st, out = await cmd(
            f'EXECW {timeout_s} cmd /c {PROBE} --res {res} --aa {cfg} '
            f'--dll {dll} --log {log}', timeout_s + 45)
        return {"cfg": cfg, "out": out.replace("\r", "")}
    finally:
        await c.close()


def classify(out):
    if re.search(r"RESULT: ok\b", out):
        return "open", ""
    if re.search(r"RESULT: winopen-refused", out):
        return "refused", "driver declined the mode cleanly"
    if re.search(r"RESULT: probe-ok-noopen", out):
        return "noopen", "stopped before touching the board"
    m = re.search(r"step: (grSstWinOpen[^\n]*)", out)
    where = m.group(1)[:60] if m else "before grSstWinOpen"
    return "hung", f"died at: {where}"


async def amain(a):
    if not await agent_alive(a.host):
        print(f"{a.host}: agent is not answering a protocol PING - nothing to "
              f"screen. Restart the agent on the machine first.")
        return 2

    print(f"screening {a.host} at {a.res}, dll={a.dll}\n")
    print(f"{'cfg':<5}{'label':<13}{'boards':>7}{'chips':>7}{'verdict':>10}  detail")
    results = {}
    for cfg in a.configs:
        try:
            r = await screen_one(a.host, cfg, a.res, a.dll, a.timeout)
        except Exception as e:
            print(f"{cfg:<5}{LABELS.get(cfg,'?'):<13}{'-':>7}{'-':>7}"
                  f"{'ERROR':>10}  {type(e).__name__}: {e}")
            results[cfg] = "error"
            continue
        if "out" not in r:
            print(f"{cfg:<5}{LABELS.get(cfg,'?'):<13}{'-':>7}{'-':>7}"
                  f"{r['verdict']:>10}  {r['detail']}")
            results[cfg] = r["verdict"]
            continue
        out = r["out"]
        boards = (re.search(r"GR_NUM_BOARDS = (\d+)", out) or [None, "-"])[1]
        chips = (re.search(r"chips in use\) = (\d+)", out) or [None, "-"])[1]
        verdict, detail = classify(out)
        results[cfg] = verdict
        print(f"{cfg:<5}{LABELS.get(cfg,'?'):<13}{boards:>7}{chips:>7}"
              f"{verdict:>10}  {detail}")

        # A hang leaves the board in an unknown state; make sure the agent is
        # still there before trying the next one, and stop if it is not.
        if verdict == "hung" and not await agent_alive(a.host, tries=2):
            print(f"\n  !! the agent went down on cfg {cfg} - stopping. "
                  f"{len([c for c in a.configs if c > cfg])} config(s) "
                  f"not screened.")
            break

    ok = [c for c, v in results.items() if v == "open"]
    bad = [c for c, v in results.items() if v == "hung"]
    print(f"\nsafe to benchmark: {','.join(map(str, sorted(ok))) or '(none)'}")
    if bad:
        print(f"WEDGES THE DRIVER:  {','.join(map(str, sorted(bad)))}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--res", default="640x480")
    ap.add_argument("--configs", default="0,1,2,3,4,5,6,7,8")
    ap.add_argument("--dll", default=r"C:\WINDOWS\system32\glide3x.dll")
    ap.add_argument("--timeout", type=int, default=70,
                    help="EXECW seconds before the probe is tree-killed")
    a = ap.parse_args()
    a.configs = [int(x) for x in a.configs.split(",")]
    raise SystemExit(asyncio.run(amain(a)))


if __name__ == "__main__":
    main()
