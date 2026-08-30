#!/usr/bin/env python3
r"""Did every host duty come back after the reboot -- and will it come back next time?

WHY THIS EXISTS. The dev host (192.168.1.132) carries the services the whole
fleet depends on, spread across THREE managers: `systemctl --user`, system
units, and docker. After a reboot on 2026-08-30 the question "is everything
back?" took a dozen ad-hoc commands and a careful reading of three different
status vocabularies. This makes it one command.

IT ASKS TWO SEPARATE QUESTIONS, because they have different answers:

  * **Is it running NOW?**  -- and not merely "active": a unit can be active
    while the thing it supervises is wedged, so where a duty has an observable
    output (a game server's query reply, the dashboard's state.json, ollama's
    API, the brain's heartbeat) this PROBES THAT OUTPUT.  Verify the
    post-condition, not the return value.
  * **Will it come back after the NEXT boot?**  -- a service started by hand is
    invisible until the reboot that loses it.  That means `enabled`, and for
    `--user` units it ALSO means the user has **linger** on, or none of them
    start until somebody logs in.  Linger is the single point of failure for
    seven of these duties and nothing else reports it.

THREE STATES, NEVER TWO.  "Not installed", "cannot ask", and "it died" are
different calls to action and only the last is a fault.  `claude-csbot` and the
`rtcw`/`mohaa` servers have never existed on this host; rendering their absence
as an outage would put a permanent red light on the board and train everyone to
ignore it.

    python3 scripts/fleet/host-duties.py            # full report
    python3 scripts/fleet/host-duties.py --quiet    # only problems
    python3 scripts/fleet/host-duties.py --json     # for tooling

Exit 0 = every duty is up and will survive a reboot.  Exit 1 = something is
down or would not come back.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# --- the duties -------------------------------------------------------------
# (unit, manager, human name, why it matters)
USER_UNITS = [
    ("retro-chat-daemon",       "LAN bridge that claims retro agents"),
    ("retro-chat-brain",        "Claude Agent SDK processor answering chat prompts"),
    ("retro-gameindex",         "favourites agent - keeps in-game server lists live"),
    ("retro-gameservers-watch", "watchdog - restarts game servers that die"),
    ("retro-dosgames-http",     "HTTP bridge for the DOS game catalog"),
]
GAME_UNITS = [
    ("cs16-server", "CS 1.6"), ("cs16-noblood", "CS 1.6 no-blood"),
    ("specialists-server", "The Specialists"), ("quake3-server", "Quake III"),
    ("openarena-server", "OpenArena"), ("quake2-server", "Quake 2"),
    ("quakeworld-server", "QuakeWorld"), ("ut99-server", "UT99"),
    ("ut2004-server", "UT2004"),
    ("a2s-proxy-cs16", "CS 1.6 browser proxy"),
    ("a2s-proxy-cs16-public", "no-blood browser proxy"),
]
SYSTEM_UNITS = [
    ("retro-pxe",                 "proxyDHCP + TFTP for network-installing the fleet"),
    ("retro-dashboard-collector", "gathers everything into /run/retro-dashboard/state.json"),
    ("ollama",                    "the 5090 inference engine the capability gate calls"),
]
# Tribes 2 needs a 2001 userland, so it is a container, not a unit.  Anything
# enumerating the game servers via systemd alone silently drops it.
DOCKER = [("tribes2-server", "Tribes 2 (needs a 2001 userland)")]

# Named here so their ABSENCE is reported as "never installed", not as an
# outage.  All three are referenced in docs but have never run on this host.
NEVER_INSTALLED_HERE = {"claude-csbot", "rtcw-server", "mohaa-server"}


def _run(cmd, timeout=15):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout or "").strip(), (p.stderr or "").strip()
    except FileNotFoundError:
        return 127, "", "no such binary"
    except subprocess.TimeoutExpired:
        return 124, "", "timed out"


def _user_env():
    """`systemctl --user` under root queries ROOT's manager, which holds none of
    these -- every service then reads "not found", which is indistinguishable
    from every service having died.  Point it at uid 1000's bus explicitly."""
    env = dict(os.environ)
    if os.geteuid() == 0:
        env.setdefault("XDG_RUNTIME_DIR", "/run/user/1000")
    return env


def _systemctl(user, *args):
    cmd = ["systemctl"] + (["--user"] if user else []) + list(args)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=15,
                           env=_user_env() if user else None)
        return (p.stdout or "").strip() or (p.stderr or "").strip()
    except Exception as e:                                   # noqa: BLE001
        return "ERROR:%s" % type(e).__name__


def check_unit(unit, user, name, why):
    active = _systemctl(user, "is-active", unit)
    enabled = _systemctl(user, "is-enabled", unit)
    row = {"unit": unit, "name": name, "why": why,
           "manager": "systemd --user" if user else "systemd (system)",
           "active": active, "enabled": enabled}
    if enabled in ("not-found",) and active in ("inactive", "unknown", "not-found"):
        row["state"] = "absent" if unit in NEVER_INSTALLED_HERE else "missing"
    elif active != "active":
        row["state"] = "down"
    elif enabled not in ("enabled", "enabled-runtime", "static",
                         "generated", "indirect", "alias"):
        # Running, but started by hand: invisible until the reboot that loses it.
        row["state"] = "wont-survive-reboot"
    else:
        row["state"] = "ok"
    return row


def check_docker(name, why):
    row = {"unit": name, "name": name, "why": why, "manager": "docker"}
    if not shutil.which("docker"):
        row.update(state="unknown", active="?", enabled="?",
                   detail="docker not installed")
        return row
    rc, out, _ = _run(["docker", "inspect", "-f",
                       "{{.State.Running}} {{.HostConfig.RestartPolicy.Name}}", name])
    if rc != 0:
        row.update(state="missing", active="not-found", enabled="not-found")
        return row
    running, policy = (out.split() + ["", ""])[:2]
    row["active"] = "active" if running == "true" else "inactive"
    row["enabled"] = policy
    # A container with restart=no is the docker equivalent of a disabled unit.
    if running != "true":
        row["state"] = "down"
    elif policy in ("no", "", "none"):
        row["state"] = "wont-survive-reboot"
    else:
        row["state"] = "ok"
    return row


def check_linger():
    """Without linger, NO --user unit starts until somebody logs in.

    That is the single point of failure for seven duties here, and it is
    completely silent: everything reads `enabled`, and the box still comes up
    with the whole fleet bridge dead.
    """
    user = os.environ.get("SUDO_USER") or os.environ.get("USER") or "voidsstr"
    out = _systemctl(False, "show-user", user) if False else ""
    rc, out, _ = _run(["loginctl", "show-user", user, "-p", "Linger"])
    val = out.split("=", 1)[1].strip() if "=" in out else "?"
    return {"user": user, "linger": val,
            "state": "ok" if val == "yes" else ("unknown" if val == "?" else "down")}


def probe_outputs():
    """Post-conditions.  An `active` unit is a promise; these are the evidence."""
    out = []

    # Game servers: the only honest check is the game's own query protocol, and
    # it differs per engine -- a single getstatus sweep reports false outages.
    hc = os.path.join(REPO, "scripts", "game-servers", "healthcheck.py")
    if os.path.exists(hc):
        rc, so, se = _run([sys.executable, hc], timeout=180)
        line = ""
        for ln in (so or "").splitlines():
            if "responding" in ln:
                line = ln.strip()
        ok = rc == 0 and "/" in line and line.split("/")[0].strip().isdigit() \
            and line.split("/")[0].strip() == line.split("/")[1].split()[0]
        out.append({"probe": "game servers answer their own query protocol",
                    "detail": line or "healthcheck produced no summary",
                    "state": "ok" if ok else "down"})
    else:
        out.append({"probe": "game servers", "detail": "healthcheck.py not present",
                    "state": "unknown"})

    # Dashboard: the collector is only doing its job if the file is FRESH.
    p = "/run/retro-dashboard/state.json"
    try:
        age = time.time() - os.path.getmtime(p)
        out.append({"probe": "dashboard state.json is fresh",
                    "detail": "%s, %.0fs old" % (p, age),
                    "state": "ok" if age < 300 else "down"})
    except OSError as e:
        out.append({"probe": "dashboard state.json", "detail": str(e), "state": "down"})

    # ollama: the capability gate calls this, so a dead API is a real outage
    # even though nothing on the fleet notices immediately.
    rc, so, se = _run(["curl", "-s", "--max-time", "8",
                       "http://localhost:11434/api/tags"], timeout=15)
    models = []
    if rc == 0 and so:
        try:
            models = [m.get("name") for m in json.loads(so).get("models", [])]
        except ValueError:
            pass
    out.append({"probe": "ollama API answers on :11434",
                "detail": ("models: " + ", ".join(m for m in models if m)) if models
                          else "no response",
                "state": "ok" if models else "down"})

    # The chat brain writes a heartbeat; the daemon claiming zero agents is NOT
    # a fault (the fleet is powered on demand), so judge the BRAIN, not a count.
    hb = "/tmp/retro-chat/processor.heartbeat"
    try:
        age = time.time() - os.path.getmtime(hb)
        out.append({"probe": "chat brain heartbeat",
                    "detail": "%.0fs old" % age,
                    "state": "ok" if age < 300 else "down"})
    except OSError:
        out.append({"probe": "chat brain heartbeat",
                    "detail": "no heartbeat file at %s" % hb, "state": "down"})
    return out


SYMBOL = {"ok": "  OK  ", "down": " DOWN ", "missing": "MISSING",
          "absent": "  --  ", "unknown": "  ??  ",
          "wont-survive-reboot": "REBOOT"}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", action="store_true", help="only show problems")
    ap.add_argument("--json", action="store_true", dest="as_json")
    a = ap.parse_args()

    groups = [
        ("fleet services (systemd --user)",
         [check_unit(u, True, u, w) for u, w in USER_UNITS]),
        ("game servers (systemd --user)",
         [check_unit(u, True, u, w) for u, w in GAME_UNITS]
         + [check_docker(u, w) for u, w in DOCKER]),
        ("host services (system units)",
         [check_unit(u, False, u, w) for u, w in SYSTEM_UNITS]),
    ]
    linger = check_linger()
    probes = probe_outputs()

    # A duty that is down, missing, or would not come back is a fault.
    # "absent" (never installed here) and "unknown" (could not ask) are not.
    faults = [r for _, rows in groups for r in rows
              if r["state"] in ("down", "missing", "wont-survive-reboot")]
    faults += [p for p in probes if p["state"] == "down"]
    if linger["state"] == "down":
        faults.append({"unit": "linger", "state": "down"})

    if a.as_json:
        print(json.dumps({"groups": [{"group": g, "rows": r} for g, r in groups],
                          "linger": linger, "probes": probes,
                          "faults": len(faults)}, indent=2))
        return 1 if faults else 0

    print("Host duties on this dev host (192.168.1.132)\n")
    for title, rows in groups:
        shown = [r for r in rows if not a.quiet or r["state"] != "ok"]
        if not shown:
            continue
        print("  %s" % title)
        for r in shown:
            print("    [%s] %-24s %-18s %s" %
                  (SYMBOL.get(r["state"], r["state"]), r["unit"],
                   "%s/%s" % (r.get("active"), r.get("enabled")), r["why"]))
            if r["state"] == "absent":
                print("             never installed on this host - not a fault")
            if r["state"] == "wont-survive-reboot":
                print("             RUNNING BUT NOT ENABLED - it will not come back")
        print()

    if not a.quiet or linger["state"] != "ok":
        print("  will --user services start at boot?")
        print("    [%s] linger for %s = %s" %
              (SYMBOL.get(linger["state"], "?"), linger["user"], linger["linger"]))
        if linger["state"] != "ok":
            print("             WITHOUT LINGER NO --user UNIT STARTS UNTIL A LOGIN.")
            print("             Fix: loginctl enable-linger %s" % linger["user"])
        print()

    print("  post-conditions (is it actually working, not merely active?)")
    for p in probes:
        if a.quiet and p["state"] == "ok":
            continue
        print("    [%s] %-42s %s" % (SYMBOL.get(p["state"], "?"),
                                     p["probe"], p["detail"]))
    print()

    if faults:
        print("  %d FAULT(S) - the host is not fully back." % len(faults))
        return 1
    print("  ALL HOST DUTIES UP, and every one is set to return after a reboot.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
