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

# A refused connect is instant and usually means a full listen backlog, not a
# dead agent -- so retry it several times with a real pause, not back-to-back.
_REFUSAL_RETRIES = 4
_REFUSAL_BACKOFF_S = 1.5

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
    # Seen misreported as games on the first live fleet sweep:
    "cmd.exe", "dllhost.exe", "nasnavi.exe", "nassche.exe", "smax4pnp.exe",
    "wmiprvse.exe", "mdm.exe", "jusched.exe", "issch.exe", "realsched.exe",
}


def _secs(line):
    m = STAMP.match(line)
    return None if not m else int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


class ConnectFailed(Exception):
    """Could not reach the agent at all -- distinct from a check that broke."""


async def _connect(ip, timeout):
    """Retry with a rising timeout. A LOADED BOX IS NOT A DEAD BOX.

    .171 has always answered slowly, and on the first live sweep .240 read
    "unreachable" at 10s while a plain PING succeeded seconds later -- it was
    simply busy running a game. Reporting a healthy machine as unreachable is
    worse than reporting nothing: it sends someone to diagnose a box that is
    fine, and it is exactly the false-negative this project keeps paying for.
    A REFUSAL AND A TIMEOUT ARE NOT THE SAME FAILURE, and treating them alike
    produced that same false negative a second time.  With six agents driving
    the fleet a box's listen backlog fills and XP answers RST, so
    ConnectionRefusedError comes back INSTANTLY and consumes none of the time
    budget.  Two back-to-back attempts therefore both landed within the same
    few milliseconds, hit the same full backlog, and called a healthy machine
    unreachable: .124 refused three connects in a row and then answered at once
    with ten hours of uptime.

    So a refusal earns more attempts AND a real sleep between them, while a
    timeout -- which has already spent its wait -- earns the rising timeout it
    always had.
    """
    last = None
    for t in (timeout, timeout * 2):
        for refusal in range(_REFUSAL_RETRIES):
            try:
                c = RetroConnection(ip, 9898)
                await c.connect(SECRET, timeout=t)
                return c
            except ConnectionRefusedError as e:
                last = e
                # Instant failure, so back off deliberately rather than
                # spinning every attempt away inside one millisecond.
                await asyncio.sleep(_REFUSAL_BACKOFF_S * (refusal + 1))
            except Exception as e:
                last = e
                break          # a timeout: go straight to the longer one
    raise ConnectFailed("no agent on %s:9898" % ip) from last


async def inspect(ip, window, tail, timeout):
    out = {"ip": ip, "reachable": False, "games": [], "modals": [],
           "recent": [], "mutating": 0, "verdict": "unknown"}
    c = await _connect(ip, timeout)
    out["reachable"] = True
    try:
        # DOWNLOAD the log and filter HERE, rather than `type ... | find` on the
        # box. That log is ~400 KB and grows; shelling out to read all of it
        # through cmd on a machine that is busy running a game routinely
        # outran the timeout, and because every failure in this function used
        # to surface as "UNREACHABLE", a slow log read looked exactly like a
        # dead box. DOWNLOAD is a binary transfer and costs a fraction of it.
        raw = await c.command_binary("DOWNLOAD %s" % LOG)
        lines = [l.rstrip() for l in raw.decode("ascii", "replace").splitlines()
                 if "CMD" in l]
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
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="initial connect timeout; retried at double this "
                         "before giving up (default 15). A loaded box answers "
                         "slowly and must not read as a dead one.")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    try:
        r = asyncio.run(inspect(a.ip, a.window, a.tail, a.timeout))
    except ConnectFailed as e:
        # Only a failure to CONNECT may be reported as unreachable. Anything
        # else is a check that broke on a box that is answering fine, and
        # conflating the two sends someone to diagnose a healthy machine.
        if a.json:
            print(json.dumps({"ip": a.ip, "reachable": False,
                              "error": type(e.__cause__ or e).__name__,
                              "verdict": "unreachable"}))
        else:
            print("%s: UNREACHABLE (%s)" % (a.ip, type(e.__cause__ or e).__name__))
        return 2
    except Exception as e:
        if a.json:
            print(json.dumps({"ip": a.ip, "reachable": True,
                              "error": "%s: %s" % (type(e).__name__, e),
                              "verdict": "check-failed"}))
        else:
            print("%s: CHECK FAILED (%s: %s)" % (a.ip, type(e).__name__, e))
            print("  The box ANSWERED -- this is the check breaking, not the "
                  "machine being down. Do not report it as an outage.")
        return 3

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
