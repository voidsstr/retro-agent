#!/usr/bin/env python3
"""Is another agent already working this box?

WHY THIS EXISTS. Several agents drive the fleet at once and they invalidated
each other's tests repeatedly: one purged a title 11 minutes after another had
purged and was mid-verify, and a third launched Quake III on top of a connected
UT99 client. Every agent connects from 192.168.1.132, so the agent has no way
to tell us apart -- **the box's own log is the only record that two people are
here**, and nothing surfaces it.

So: read the last stretch of C:\\RETRO_AGENT\\agent.log, ignore the polling
chatter, and report what has actually been done to this machine recently.

    python3 scripts/fleet/box-owner.py 192.168.1.143
    python3 scripts/fleet/box-owner.py 192.168.1.143 --window 600 --json

Exit code is the useful part in a script: 0 = quiet, 1 = someone is here.

IT CANNOT TELL YOU FROM THEM. Your own commands appear in this log too, so a
"BUSY" verdict seconds after your own work is you. Read the commands, not just
the verdict -- that is why they are printed.
"""
import argparse
import asyncio
import json
import os
import re
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
LOG = r"C:\RETRO_AGENT\agent.log"

# The chat daemon long-polls forever; these say nothing about who is working.
NOISE = re.compile(r'CMD: "(PROMPT_WAIT|STATUS_WAIT|LOG_WAIT|PING|GAMESYNC STATUS'
                   r'|SYSINFO|WINLIST|PROCLIST|SCREENSHOT)\b', re.I)
# Commands that mean somebody is CHANGING this box, not just looking at it.
MUTATING = re.compile(r'CMD: "(GAMESYNC START|EXEC|EXECW|LAUNCH|UPLOAD|DELETE|MKDIR'
                      r'|FILECOPY|REGWRITE|REGDELETE|UICLICK|UIKEY|CLICKSHOT'
                      r'|REBOOT|RESTART|QUIT|DRVUPDATE|ONBOARD)\b', re.I)
STAMP = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\]")

# Anything here is a real workload, not a service. Deliberately a denylist of
# things we start, not an allowlist of Windows -- an allowlist goes stale.
GAMEISH = re.compile(r"\.(exe)$", re.I)
NOT_A_GAME = {
    "retro_agent.exe", "retro_chat.exe", "explorer.exe", "rotate_wall.exe",
    "arrange_icons.exe", "rundll32.exe", "svchost.exe", "csrss.exe",
    "winlogon.exe", "services.exe", "lsass.exe", "spoolsv.exe", "ctfmon.exe",
    "alg.exe", "wmiprvse.exe", "wuauclt.exe", "wscntfy.exe", "nvsvc32.exe",
    "daemon.exe", "ati2evxx.exe", "atieclxx.exe", "atiesrxx.exe", "ccc.exe",
    "mom.exe", "soundman.exe", "cthelper.exe", "wpabaln.exe", "conhost.exe",
    "lsm.exe", "dwm.exe", "taskhost.exe", "audiodg.exe", "msmpeng.exe",
    "msseces.exe", "windowssearch.exe", "searchindexer.exe", "conime.exe",
    "smss.exe", "hkcmd.exe", "igfxtray.exe", "igfxpers.exe", "igfxsrvc.exe",
    # Vendor tray junk and Windows extras seen on the fleet. Added as observed:
    # this list is a DENYLIST and will always leak a few, which is why the
    # command lines are printed rather than just the verdict.
    "msmsgs.exe", "core.exe", "adeck.exe", "bdaremote.exe", "raid_tool.exe",
    "hde.exe", "dumprep.exe", "dwwin.exe", "ssstars.scr", "logon.scr",
    "3dfxman.exe", "wmiadap.exe", "userinit.exe", "spupdsvc.exe",
}


def _secs(line):
    m = STAMP.match(line)
    return None if not m else int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


async def inspect(ip, window, tail):
    out = {"ip": ip, "reachable": False, "games": [], "modals": [],
           "recent": [], "mutating": 0, "verdict": "unknown"}
    c = RetroConnection(ip, 9898)
    await c.connect(SECRET, timeout=10.0)
    out["reachable"] = True
    try:
        _, d = await c.send_command('EXEC cmd /c type %s | find /i "CMD"' % LOG)
        lines = [l.rstrip() for l in d.decode("ascii", "replace").splitlines() if l.strip()]
        lines = [l for l in lines if not NOISE.search(l)][-tail:]

        newest = next((_secs(l) for l in reversed(lines) if _secs(l) is not None), None)
        for l in lines:
            t = _secs(l)
            # Only a same-day window; the log has no date, so a wrap is possible.
            if newest is not None and t is not None and 0 <= newest - t <= window:
                out["recent"].append(l)
                if MUTATING.search(l):
                    out["mutating"] += 1

        _, d = await c.send_command("PROCLIST")
        p = json.loads(d.decode("ascii", "replace"))
        procs = p if isinstance(p, list) else p.get("processes", [])
        out["games"] = sorted({
            n for n in (x.get("name", "").lower() for x in procs)
            if GAMEISH.search(n) and n not in NOT_A_GAME
        })

        _, d = await c.send_command("WINLIST")
        out["modals"] = [w.get("title", "")[:60]
                         for w in json.loads(d.decode("ascii", "replace")).get("windows", [])
                         if w.get("class") == "#32770"]
    finally:
        await c.close()

    if out["games"] or out["mutating"]:
        out["verdict"] = "BUSY"
    elif out["recent"]:
        out["verdict"] = "recent-activity"
    else:
        out["verdict"] = "quiet"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ip")
    ap.add_argument("--window", type=int, default=300,
                    help="seconds of log history to consider (default 300)")
    ap.add_argument("--tail", type=int, default=400,
                    help="how many CMD lines to scan back through")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    try:
        r = asyncio.run(inspect(a.ip, a.window, a.tail))
    except Exception as e:
        if a.json:
            print(json.dumps({"ip": a.ip, "reachable": False,
                              "error": type(e).__name__, "verdict": "unreachable"}))
        else:
            print("%s: UNREACHABLE (%s)" % (a.ip, type(e).__name__))
        return 2

    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print("%s: %s" % (r["ip"], r["verdict"]))
        if r["games"]:
            print("  RUNNING: %s" % ", ".join(r["games"]))
        if r["modals"]:
            print("  MODALS (these block `start`): %s" % "; ".join(r["modals"]))
        if r["recent"]:
            print("  last %d command(s) in the window, %d of them mutating:"
                  % (len(r["recent"]), r["mutating"]))
            for l in r["recent"][-12:]:
                print("    %s" % l[:150])
        if r["verdict"] != "quiet":
            print("  NOTE: your own commands appear here too. Read the lines "
                  "above before concluding someone else owns this box.")
    return 1 if r["verdict"] == "BUSY" else 0


if __name__ == "__main__":
    raise SystemExit(main())
