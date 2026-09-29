#!/usr/bin/env python3
"""run_modetest.py - the per-box plan for tools/modetest/modetest.exe, as code.

    run_modetest.py plan [--box 192.168.1.124]   print the plan; touches nothing
    run_modetest.py parse <modetest.log>         RESULT/VERDICT lines -> JSON
    run_modetest.py preflight <ip>               READ-ONLY: is the box ready?
    run_modetest.py go <ip> --step N --approved-by <name> [--v5-lane-released]
                                                 run ONE step - this SWITCHES
                                                 the box's display modes

WHY A PLAN IN CODE. modetest measures what a fullscreen mode change that names
no refresh lands on, per OS + driver (the question behind "every Quake-era
title at the monitor's best refresh at its own resolution"). It switches modes
on real monitors, two of them CRTs, on boxes other sessions are using. So the
exact modes, rates, pacing and launch route per box are written down here once,
checked by `preflight` against what the box reports right now, and `go` refuses
anything outside them.

STATUS 2026-09-29: BUILT, NOT RUN ON ANY BOX. `plan` and `parse` are exercised
by tests; `go` has not met hardware - its first run must be watched.

Every go:
  * refuses Windows 8+ (.249) and any box not in PLAN;
  * refuses .124 without --v5-lane-released (the Voodoo 5 6000 lane owns it);
  * runs `preflight` first and refuses on any failure: hostname, live mode ==
    persisted mode, no known game running, agent new enough on Win9x;
  * starts modetest the way the box needs (Win9x: LAUNCH only - EXEC runs the
    child in the single-threaded agent's own console; a DirectDraw/D3D step:
    LAUNCH, it needs the foreground; otherwise EXECW with a budget);
  * polls the log by DOWNLOAD (never EXEC on Win9x) until its own run's "exit"
    line, then reads DISPLAYCFG get and compares it with the persisted mode;
  * saves everything under .claude/evidence-refresh/modetest/<box>/<stamp>/.
"""
import argparse
import asyncio
import json
import os
import re
import sys
import time
import uuid

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

EXE_LOCAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modetest.exe")
REMOTE_DIR = r"C:\RETRO_AGENT\modetest"
REMOTE_EXE = REMOTE_DIR + r"\modetest.exe"
REMOTE_LOG = REMOTE_DIR + r"\modetest.log"
EVIDENCE = os.path.join(REPO, ".claude", "evidence-refresh", "modetest")
SECRET = "retro-agent-secret"

# Processes that mean "a game or a mode-switching tool owns the screen now".
BUSY = {"quake3.exe", "ioquake3.x86.exe", "quake2.exe", "glquake.exe", "winquake.exe",
        "hl.exe", "jasp.exe", "jamp.exe", "sof2.exe", "sof2mp.exe", "wolfsp.exe",
        "wolfmp.exe", "unrealtournament.exe", "unreal.exe", "ut2004.exe", "deusex.exe",
        "halo.exe", "doom3.exe", "farcry.exe", "bf1942.exe", "dosbox.exe", "sin.exe",
        "sof.exe", "glh2.exe", "main.exe", "vcrctl.exe", "ddlab.exe", "d3dprobe.exe",
        "glidelab.exe", "modetest.exe", "refreshkeep.exe", "setrefresh.exe"}

# ----------------------------------------------------------------------------
# THE PLAN. Numbers are what GAMERES / DISPLAYCFG / HWPROFILE reported on
# 2026-09-29 00:26-00:38 (.claude/evidence-refresh/*/boxes). A step names the
# modetest arguments, how many mode switches it makes (modetest_logic.h
# mt_plan_switches), and whether it needs the foreground (DirectDraw / D3D).
# ----------------------------------------------------------------------------
PLAN = {
    "192.168.1.123": {
        "host": "NSC-B20C188E96D", "os": "xp",
        "stack": "XP SP3, ATI Radeon HD 3850 AGP, Catalyst 8.970.100.0 (13.4 legacy)",
        "monitor": "DELL P2312H analog LCD, EDID vmax 76",
        "persisted": (1920, 1080, 32, 60),
        "gate": "the 1080p workflow owns this screen until it says it is done",
        "steps": [
            {"args": "-list", "switches": 0, "fg": False,
             "why": "raw list incl. 0/1 Hz entries, EDID, timed desktop refresh"},
            {"args": "1280 960 32 -a -b 75", "switches": 5, "fg": False,
             "why": "the 4:3 target (Q2/SiN/SoF/Q1/WON HL): no-rate lands on? and "
                    "does it KEEP a 75 Hz pre-switch? (listed, <= vmax 76)"},
            {"args": "1280 960 32 -c -cb 75", "switches": 7, "fg": True,
             "why": "DirectDraw SetDisplayMode(...,0), cold and after a pre-switch"},
            {"args": "1280 960 32 -d", "switches": 3, "fg": True,
             "why": "Direct3D 9 FullScreen_RefreshRateInHz 0"},
        ],
    },
    "192.168.1.145": {
        "host": "DELL", "os": "xp",
        "stack": "XP SP3, GeForce 8400GS, NVIDIA 340.52 (6.14.13.4052)",
        "monitor": "DELL E2414H analog LCD, EDID vmax 76",
        "persisted": (1920, 1080, 32, 60),
        "gate": "the 1080p workflow owns this screen until it says it is done",
        "steps": [
            {"args": "-list", "switches": 0, "fg": False, "why": "as .123"},
            {"args": "1280 960 32 -a -b 75", "switches": 5, "fg": False,
             "why": "the NVIDIA answer for the same question as .123 - the only "
                    "prior measurement is ForceWare 71.89 (60 Hz, 2026-08-25)"},
            {"args": "1280 960 32 -c -cb 75", "switches": 7, "fg": True, "why": "as .123"},
            {"args": "1280 960 32 -d", "switches": 3, "fg": True, "why": "as .123"},
        ],
    },
    "192.168.1.240": {
        "host": "USER-41EA3B3330", "os": "xp",
        "stack": "XP SP3, Radeon X1600 (HWPROFILE active adapter), Catalyst 8.593 (10.2)",
        "monitor": "DELL E2313H digital LCD, EDID vmax 76",
        "persisted": (1920, 1080, 32, 60),
        "gate": "another session was diagnosing this display at 00:10 - wait for it",
        "steps": [
            {"args": "-list", "switches": 0, "fg": False, "why": "as .123"},
            {"args": "1280 960 32 -a -b 75", "switches": 5, "fg": False,
             "why": "a second ATI generation (10.2 vs 13.4) - DALRULE_APPLYPREVIOUS"
                    "REFRESHRATE exists in both miniports, unset"},
        ],
    },
    "192.168.1.195": {
        "host": "ADMIN-PC", "os": "win7",
        "stack": "Windows 7, Radeon HD 5450, AMD 15.200.1062.1004",
        "monitor": "HP 2511 digital LCD - every listed mode is 60 Hz",
        "persisted": (1920, 1080, 32, 60),
        "gate": "the 1080p workflow owns this screen; live was 640x480 at 00:26",
        "steps": [
            {"args": "-list", "switches": 0, "fg": False,
             "why": "confirm no mode lists above 60 - then 60 IS the best everywhere "
                    "and nothing needs a mechanism on this box"},
        ],
    },
    "192.168.1.124": {
        "host": "NSC-C543575F526", "os": "xp", "v5": True,
        "stack": "XP SP3, Voodoo 5 6000 on vcr-kmd 0.1.0.0 (ours)",
        "monitor": "HP P1120 21in CRT, EDID vmax 160",
        "persisted": (1280, 1024, 32, 85),
        "gate": "the V5 6000 benchmark lane owns this box - --v5-lane-released",
        "pace": 8,
        "steps": [
            {"args": "-list", "switches": 0, "fg": False, "why": "vcr-kmd's full list"},
            {"args": "1024 768 32 -a -b 85", "switches": 5, "fg": False,
             "why": "win32k's pick among vcr-kmd's 60/70/75/85 for a no-rate "
                    "request, and whether a pre-switched 85 survives it"},
            {"args": "800 600 32 -a", "switches": 2, "fg": False,
             "why": "who picks the rate for a no-rate request on vcr-kmd. In the XP build "
                    "VM (2026-09-29, MS Cirrus driver) win32k gave 60 at every size that "
                    "lists 60 - not the lowest (56 at 800x600), not the first listed, not "
                    "the registry's 75 or the current 85 - and the request FAILED at a size "
                    "listing no 60. If win32k hands the driver 60, vcr-kmd sets 60 here; if "
                    "it hands 0/1, vcr-kmd's own pick_refresh takes the LOWEST listed rate: "
                    "56 (DMT 800x600@56). The flight recorder's DrvEnablePDEV line shows "
                    "which. Needs 800x600x32@56 in step 0's -list (EDID-filtered)"},
            {"args": "1280 960 32 -c", "switches": 3, "fg": True,
             "why": "the DirectDraw rate-0 path on vcr-kmd's own DDraw HAL"},
        ],
    },
    "192.168.1.243": {
        "host": "N5R5L9", "os": "win98",
        "stack": "Windows 98 SE, Cirrus Logic 5436 (cirrusmm.drv), Voodoo 2 pass-through",
        "monitor": "DELL E773c CRT, NO EDID read; Display\\0000\\DEFAULT RefreshRate=\"75\"",
        "persisted": (1024, 768, 8, 75),
        "gate": "single-threaded Win9x agent: LAUNCH + DOWNLOAD only; agent >= 1.90.1",
        "pace": 8,
        "steps": [
            {"args": "-list", "switches": 0, "fg": False,
             "why": "Win9x reports 0 Hz: the vblank timing is the only reading"},
            {"args": "800 600 8 -a", "switches": 2, "fg": False,
             "why": "does a no-rate request land on the global RefreshRate (75)?"},
            {"args": "640 480 8 -a", "switches": 2, "fg": False, "why": "the same at 640x480"},
        ],
    },
}
NEVER = {"192.168.1.249": "Windows 11 (WHITEBEAST) - not a managed retro box"}


HOLD_S = 4

# What modetest may still spend AFTER its own watchdog fires, and the host must
# therefore wait out before anything kills it (review 2026-09-29):
#   45 s   MT_TAKEOVER_MS - the watchdog waits for a switch in flight
#   75 s   mt_restore: 3 tries x (VCR_PACE_LOCK_MS 20 s + 5 s) when the pace
#          lock is busy
#   30 s   the explicit-DEVMODE retry (a floor + the switch)
#   28 s   the exit hold: VCR_PACE_LOCK_MS 20 s + an 8 s floor
# The old margin (60 s past the watchdog) was shorter than the give-back path
# itself, so EXECW could tree-kill modetest in the middle of it - an UNPACED
# revert on a CRT, the one thing the pace gate exists to prevent.
GIVEBACK_WORST_S = 200


def tool_cap(b, s):
    """modetest's own watchdog budget (modetest.c mt_main: 60 s + (switches + 2)
    x (pace + hold + 4) s)."""
    return 60 + (s["switches"] + 2) * (b.get("pace", 6) + HOLD_S + 4)


def step_budget(b, s):
    """Seconds the host allows a step: the tool's watchdog plus everything the
    tool may still do after it fires. EXECW (clamped to 900 s by the agent)
    must never be what ends modetest."""
    return tool_cap(b, s) + GIVEBACK_WORST_S


def step_args(box, step, tag):
    s = PLAN[box]["steps"][step]
    extra = ""
    if s["switches"]:
        extra = " -pace %d -hold %d" % (PLAN[box].get("pace", 6), HOLD_S)
    return "%s%s -tag %s" % (s["args"], extra, tag)


def print_plan(only=None):
    total = 0
    for ip, b in PLAN.items():
        if only and ip != only:
            continue
        w, h, bpp, hz = b["persisted"]
        print("%s  %s  [%s]" % (ip, b["host"], b["os"]))
        print("    stack    : %s" % b["stack"])
        print("    monitor  : %s" % b["monitor"])
        print("    persisted: %dx%dx%d@%d" % (w, h, bpp, hz))
        print("    gate     : %s" % b["gate"])
        for i, s in enumerate(b["steps"]):
            route = ("LAUNCH" if (b["os"] == "win98" or s["fg"]) else "EXECW")
            print("    step %d  : modetest %s   (%d switches, %s)" %
                  (i, step_args(ip, i, "<tag>"), s["switches"], route))
            print("              %s" % s["why"])
            total += s["switches"]
        print()
    for ip, why in NEVER.items():
        print("%s  NEVER: %s" % (ip, why))
    print("\nplanned mode switches in all: %d (each >= 5 s apart, restores included)" % total)


# ----------------------------------------------------------------------------
# parse
# ----------------------------------------------------------------------------
_RESULT = re.compile(r"RESULT (.*)$")
_KV = re.compile(r'(\w+)=("[^"]*"|\S+)')


def parse_log(text, tag=None):
    """RESULT and VERDICT lines of ONE run (the one carrying `tag`, else the
    last run in the file) as a dict."""
    runs = re.split(r"(?m)^\[[ \d]+ ms\] ==== modetest ", text)
    if len(runs) < 2:
        return {"runs": 0}
    body = runs[-1]
    if tag:
        for r in runs[1:]:
            if ("-tag %s" % tag) in r.split("\n", 1)[0]:
                body = r
                break
        else:
            return {"runs": len(runs) - 1, "error": "no run tagged %s" % tag}
    out = {"results": [], "verdicts": [], "refused": None, "exit": None,
           "restore_failed": "RESTORE FAILED" in body or "RESTORE NOT ATTEMPTED" in body}
    for line in body.splitlines():
        m = _RESULT.search(line)
        if m:
            out["results"].append({k: v.strip('"') for k, v in _KV.findall(m.group(1))})
            continue
        if "VERDICT:" in line:
            out["verdicts"].append(line.split("VERDICT:", 1)[1].strip())
        if "REFUSED:" in line:
            out["refused"] = line.split("REFUSED:", 1)[1].strip()
        m = re.search(r"\] exit (\d+) \(switches made: (\d+)\)", line)
        if m:
            out["exit"] = int(m.group(1))
            out["switches"] = int(m.group(2))
    return out


def screen_is_persisted(displaycfg_text, persisted):
    """Is DISPLAYCFG get's answer the box's persisted mode? The same rule as
    modetest_logic.h mt_live_is_persisted: W x H x BPP must match, and the rate
    too when BOTH are real (Win9x reports 0 for the live rate = unknown). An
    answer that does not parse is NOT a pass."""
    try:
        d = json.loads(displaycfg_text[displaycfg_text.index("{"):])
    except (ValueError, TypeError):
        return False
    w, h, bpp, hz = persisted
    if (d.get("width"), d.get("height"), d.get("bpp")) != (w, h, bpp):
        return False
    live = d.get("refresh", 0) or 0
    real = lambda x: 50 <= x < 200
    return not (real(live) and real(hz) and live != hz)


# ----------------------------------------------------------------------------
# preflight (read-only) and go
# ----------------------------------------------------------------------------
async def _cmd(c, cmd):
    st, d = await c.send_command(cmd)
    return st, d.decode("ascii", errors="replace")


async def preflight(ip, c=None):
    """Read-only checks. Returns (ok, [problems], facts)."""
    from client.retro_protocol import RetroConnection
    own = c is None
    if own:
        c = RetroConnection(ip, 9898)
        await c.connect(SECRET, timeout=20.0)
    problems, facts = [], {}
    try:
        b = PLAN[ip]
        st, hw = await _cmd(c, "HWPROFILE")
        j = json.loads(hw[hw.index("{"):])
        facts["host"], facts["agent"] = j.get("hostname"), j.get("agent_version")
        if facts["host"] != b["host"]:
            problems.append("hostname %s, the plan is for %s" % (facts["host"], b["host"]))
        if b["os"] == "win98":
            v = tuple(int(x) for x in re.findall(r"\d+", facts["agent"] or "0")[:3])
            if v < (1, 90, 1):
                problems.append("agent %s < 1.90.1 on Win9x (LAUNCH of an .exe must "
                                "skip command.com)" % facts["agent"])
        st, dc = await _cmd(c, "DISPLAYCFG get")
        d = json.loads(dc[dc.index("{"):])
        facts["live"] = d
        w, h, bpp, hz = b["persisted"]
        if (d.get("width"), d.get("height"), d.get("bpp")) != (w, h, bpp):
            problems.append("live %sx%sx%s is not the persisted %dx%dx%d - the screen is "
                            "in use" % (d.get("width"), d.get("height"), d.get("bpp"),
                                        w, h, bpp))
        live_hz = d.get("refresh", 0)
        if live_hz not in (0, 1) and live_hz != hz:
            problems.append("live %s Hz is not the persisted %d Hz" % (live_hz, hz))
        st, pl = await _cmd(c, "PROCLIST")
        names = {p.lower() for p in re.findall(r'"name"\s*:\s*"([^"]+)"', pl)}
        busy = sorted(names & BUSY)
        if busy:
            problems.append("running now: %s" % ", ".join(busy))
    finally:
        if own:
            await c.close()
    return not problems, problems, facts


async def go(ip, step, approved_by, v5_released):
    from client.retro_protocol import RetroConnection
    if ip in NEVER:
        sys.exit("REFUSED: %s - %s" % (ip, NEVER[ip]))
    if ip not in PLAN:
        sys.exit("REFUSED: %s is not in the plan" % ip)
    b = PLAN[ip]
    if step < 0 or step >= len(b["steps"]):
        sys.exit("REFUSED: %s has steps 0..%d" % (ip, len(b["steps"]) - 1))
    if b.get("v5") and not v5_released:
        sys.exit("REFUSED: %s belongs to the V5 6000 lane - pass --v5-lane-released "
                 "only when that session has said so" % ip)
    if not approved_by:
        sys.exit("REFUSED: --approved-by <who agreed to switch this box's modes>")
    if not os.path.isfile(EXE_LOCAL):
        sys.exit("REFUSED: build tools/modetest/modetest.exe first (build.sh)")
    tag = uuid.uuid4().hex[:10]
    s = b["steps"][step]
    args = step_args(ip, step, tag)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(EVIDENCE, ip.split(".")[-1], "%s_step%d" % (stamp, step))
    os.makedirs(outdir, exist_ok=True)
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=20.0)
    try:
        ok, problems, facts = await preflight(ip, c)
        with open(os.path.join(outdir, "preflight.json"), "w") as f:
            json.dump({"ok": ok, "problems": problems, "facts": facts}, f, indent=1)
        if not ok:
            print("REFUSED by preflight:\n  " + "\n  ".join(problems))
            return 2
        await _cmd(c, "MKDIR " + REMOTE_DIR)
        data = open(EXE_LOCAL, "rb").read()
        await c.send_command("UPLOAD " + REMOTE_EXE, binary_payload=data)
        st, dl = await _cmd(c, "DIRLIST " + REMOTE_DIR)
        if str(len(data)) not in dl:
            print("REFUSED: the uploaded exe does not read back at %d bytes" % len(data))
            return 2
        budget = step_budget(b, s)
        line = REMOTE_EXE + " " + args
        with open(os.path.join(outdir, "command.txt"), "w") as f:
            f.write("approved-by: %s\n%s\n" % (approved_by, line))
        if b["os"] == "win98" or s["fg"]:
            st, out = await _cmd(c, "LAUNCH " + line)
        else:
            # the agent answers EXECW only when modetest has exited: the CLIENT
            # must wait that long too. send_command's default is 60 s, and a
            # timeout there closed the connection and lost the whole step's
            # evidence (and the post-check below) on any step over a minute.
            raw_st, raw = await c.send_command("EXECW %d %s" % (budget, line),
                                               timeout=budget + 60)
            st, out = raw_st, raw.decode("ascii", errors="replace")
        with open(os.path.join(outdir, "start.txt"), "w") as f:
            f.write("status %s\n%s\n" % (st, out))
        deadline = time.time() + budget + 30
        text, res = "", {}
        while time.time() < deadline:
            await asyncio.sleep(10)
            try:
                st, raw = await c.send_command("DOWNLOAD " + REMOTE_LOG)
                text = raw.decode("ascii", errors="replace")
            except Exception:
                continue
            res = parse_log(text, tag)
            if res.get("exit") is not None:
                break
        st, dc = await _cmd(c, "DISPLAYCFG get")
        with open(os.path.join(outdir, "modetest.log"), "w") as f:
            f.write(text)
        res["displaycfg_after"] = dc
        # THE POST-CONDITION, from the agent - not the tool's own word for it.
        res["screen_after_ok"] = screen_is_persisted(dc, b["persisted"])
        with open(os.path.join(outdir, "result.json"), "w") as f:
            json.dump(res, f, indent=1)
        print(json.dumps(res, indent=1))
        if not res["screen_after_ok"]:
            print("!!! THE SCREEN IS NOT AT ITS PERSISTED MODE %dx%dx%d@%d AFTER THE STEP "
                  "(DISPLAYCFG: %s) - look at the screen, then LAUNCH `%s -restore`"
                  % (b["persisted"] + (dc.strip(), REMOTE_EXE)))
            return 3
        if res.get("exit") is None:
            print("!!! NO EXIT LINE within the budget - look at the screen, then "
                  "`modetest -restore` (LAUNCH) if it is not the persisted mode")
            return 3
        return res["exit"]
    finally:
        await c.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--box")
    p = sub.add_parser("parse")
    p.add_argument("log")
    p.add_argument("--tag")
    p = sub.add_parser("preflight")
    p.add_argument("ip")
    p = sub.add_parser("go")
    p.add_argument("ip")
    p.add_argument("--step", type=int, required=True)
    p.add_argument("--approved-by", required=True)
    p.add_argument("--v5-lane-released", action="store_true")
    a = ap.parse_args()
    if a.cmd == "plan":
        print_plan(a.box)
        return 0
    if a.cmd == "parse":
        print(json.dumps(parse_log(open(a.log, errors="replace").read(), a.tag), indent=1))
        return 0
    if a.cmd == "preflight":
        if a.ip not in PLAN:
            sys.exit("%s is not in the plan%s" % (a.ip, " - " + NEVER[a.ip] if a.ip in NEVER else ""))
        ok, problems, facts = asyncio.run(preflight(a.ip))
        print(json.dumps({"ok": ok, "problems": problems, "facts": facts}, indent=1))
        return 0 if ok else 2
    return asyncio.run(go(a.ip, a.step, a.approved_by, a.v5_lane_released))


if __name__ == "__main__":
    sys.exit(main())
