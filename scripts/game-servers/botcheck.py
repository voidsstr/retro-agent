#!/usr/bin/env python3
"""Does every fleet game server that CAN have bots actually have them - now,
and after it next starts by itself?

WHY THIS EXISTS. "The server has bots" was true of each server on the day it
was installed and is not a property anything re-checks. Three ways it rots,
all of which have really happened on this host:

  * a bot setting applied LIVE over rcon and never written to a file. It
    survives a map change and dies at the next restart, so the gap only shows
    up after a reboot nobody connects to the bots being gone.
  * a server whose unit is `enabled` but whose --user manager never starts
    because `loginctl enable-linger` is off: every unit reads `enabled` and
    the host comes up with all of them dead (CLAUDE.md calls linger the single
    point of failure for seven duties).
  * someone reads a plain player count as the bot count. On FOUR engines here
    that is simply wrong - 3zb2's Quake 2 bots are game entities and never
    appear in `status`, FrikBot's are invisible to the server-info query, and
    the Unreal family reports bots and humans in one number.

So this asks each engine THE QUESTION IT CAN ANSWER, and separates four
states that a single "bots: 0" would flatten into one:

    populated     bots are in the server right now
    on_join       no bots now, BY DESIGN - this engine only fills when a
                  human arrives. Not a fault, and not something to "fix"
                  without changing the engine's own behaviour.
    none_possible the game cannot have server-side bots at all, with the
                  evidence recorded in the game-servers skill
    BROKEN        it should have bots and it does not -> exit 1

Run:  python3 scripts/game-servers/botcheck.py [--quiet] [--json]
"""

import argparse
import json
import os
import re
import socket
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")
RCON_PW = "retroadmin"

POPULATED, ON_JOIN, NONE_POSSIBLE, BROKEN, UNKNOWN = (
    "populated", "on_join", "none_possible", "BROKEN", "unknown")


# --- the probes, one per engine family --------------------------------------

def _udp(port, payload, timeout=3.0, host="127.0.0.1"):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        s.send(payload)
        out = b""
        while True:
            try:
                out += s.recv(65535)
            except socket.timeout:
                return out
    finally:
        s.close()


def rcon_q3(port, cmd):
    """Quake III family: one packet, no challenge."""
    return _udp(port, b"\xff\xff\xff\xffrcon %s %s" % (RCON_PW.encode(), cmd.encode())
                ).decode("latin-1", "replace")


def rcon_goldsrc(port, cmd):
    """GoldSrc: challenge, then the command."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        s.connect(("127.0.0.1", port))
        s.send(b"\xff\xff\xff\xffchallenge rcon\n")
        chal = s.recv(4096).split()[2].decode()
        s.send(b'\xff\xff\xff\xffrcon %s "%s" %s\n'
               % (chal.encode(), RCON_PW.encode(), cmd.encode()))
        out = b""
        while True:
            try:
                out += s.recv(65535)
            except socket.timeout:
                return out.decode("latin-1", "replace")
    finally:
        s.close()


def count_goldsrc_bots(port):
    """`status` lines whose uniqueid is literally BOT.

    NOT the player count: the A2S proxies on 27015/27016 relay gameplay too,
    so a real player there shows up as 127.0.0.1 and looks local.
    """
    txt = rcon_goldsrc(port, "status")
    return sum(1 for ln in txt.splitlines() if re.match(r"^#\s*\d+", ln) and "BOT" in ln)


def count_q3_bots(port):
    """Quake III family `status`: a player line with ping 0 is a bot."""
    txt = rcon_q3(port, "status")
    n = 0
    for ln in txt.splitlines():
        m = re.match(r"^\s*(\d+)\s+(-?\d+)\s+(\d+)\s+", ln)
        if m and int(m.group(3)) == 0:
            n += 1
    return n


def count_quake2_bots(logpath, names):
    """3zb2's bots are game ENTITIES, not network clients.

    They never appear in `status` or in the server-info query, so the only
    honest evidence is the server's own log saying they entered the game
    after the most recent start.
    """
    try:
        with open(logpath, "rb") as fh:
            tail = fh.read()[-200000:].decode("latin-1", "replace")
    except OSError:
        return None
    seen = set()
    for ln in tail.splitlines():
        for n in names:
            if ln.startswith(n + " entered the game"):
                seen.add(n)
    return len(seen)


def count_quake1_bots(port):
    """FrikBot X bots are invisible to the server-info query; DarkPlaces can
    be asked for the mod's own global instead."""
    txt = rcon_q3(port, "prvm_globalget server bot_count")
    m = re.search(r"(\d+)\s*$", txt.strip())
    return int(m.group(1)) if m else None


# --- what each server is supposed to be -------------------------------------
#
# `expect`:  populated  -> bots must be there with nobody on the server
#            on_join    -> bots only once a human joins (engine behaviour)
#            none       -> the game cannot do it
# `persist`: (path, regex) that must match, so the setting survives a restart.

SERVERS = [
    dict(unit="cs16-server", label="CS 1.6", expect="populated",
         probe=lambda: count_goldsrc_bots(27018), want=8,
         persist=(f"{HOME}/hlds-cs16/cstrike/addons/yapb/conf/yapb.cfg",
                  r'^yb_quota\s+"?\d')),
    dict(unit="cs16-noblood", label="CS 1.6 no-blood", expect="populated",
         probe=lambda: count_goldsrc_bots(27019), want=8,
         persist=(f"{HOME}/hlds-cs16-noblood/cstrike/addons/yapb/conf/yapb.cfg",
                  r'^yb_quota\s+"?\d')),
    dict(unit="css-server", label="CS: Source", expect="populated",
         probe=None, want=3,
         probe_note="Valve bots: the A2S bot field",
         persist=(f"{HOME}/css-server/cstrike/cfg/server.cfg", r"^\s*bot_quota\s+\d")),
    dict(unit="hldm-server", label="Half-Life DM", expect="populated",
         probe=lambda: count_goldsrc_bots(27021), want=3,
         persist=(f"{HOME}/hldm-server/valve/addons/jk_botti/jk_botti.cfg",
                  r"^\s*max_bots\s+\d")),
    dict(unit="quake3-server", label="Quake III", expect="populated",
         probe=lambda: count_q3_bots(27961), want=3,
         persist=(f"{HOME}/q3a-server/.q3a/baseq3/server.cfg", r"bot_minplayers")),
    dict(unit="openarena-server", label="OpenArena", expect="populated",
         probe=lambda: count_q3_bots(27960), want=3,
         persist=(f"{HOME}/q3-server/.openarena/baseoa/server.cfg", r"bot_minplayers")),
    dict(unit="q3ta-server", label="Q3 Team Arena", expect="populated",
         probe=lambda: count_q3_bots(27962), want=3,
         persist=(f"{HOME}/q3ta-server/.q3a/missionpack/server.cfg", r"bot_minplayers")),
    dict(unit="jka-server", label="Jedi Academy", expect="populated",
         probe=lambda: count_q3_bots(29070), want=3,
         persist=(f"{HOME}/jka-server/base/server.cfg", r"bot_minplayers")),
    dict(unit="rtcw-server", label="RTCW", expect="populated",
         probe=lambda: count_q3_bots(27963), want=8,
         # Omni-Bot, not the engine - RTCW MP has no bot files and no mp_*.aas.
         persist=(f"{HOME}/rtcw-server/omni-bot/rtcw/user/omni-bot.cfg",
                  r"^MinBots\s*=\s*\d")),
    dict(unit="quake2-server", label="Quake 2", expect="populated",
         probe=lambda: count_quake2_bots(f"{HOME}/q2-server/server.log",
                                         ["Evil Zeep", "Claw Finger", "Korn"]),
         want=3, probe_note="3zb2 bots are entities - log, not status",
         persist=(f"{HOME}/q2-server/3zb2/3ZBConfig.cfg", r"^\[fleet\]")),
    dict(unit="quake1-server", label="Quake (NetQuake)", expect="populated",
         probe=lambda: count_quake1_bots(26000), want=3,
         probe_note="FrikBot is invisible to the query - rcon prvm global",
         persist=(f"{HOME}/quake1-server/id1/server.cfg", None)),
    dict(unit="unrealgold-server", label="Unreal Gold", expect="populated",
         probe=None, want=3, probe_note="counted by gameservers.py (NumBots)",
         persist=(f"{HOME}/unrealgold-server/System/UnrealServer.ini",
                  r"^InitialBots=\d")),
    dict(unit="ut2003-server", label="UT2003", expect="populated",
         probe=None, want=6, probe_note="counted by gameservers.py (native query)",
         persist=None),

    # --- bots only once a human arrives. NOT a fault. ---
    dict(unit="quakeworld-server", label="QuakeWorld", expect="on_join",
         why="KTX frogbots fill only while a human is on (k_fb_autoremove_at)",
         persist=(f"{HOME}/qw-server/id1/server.cfg", r"k_fb_skill")),
    dict(unit="ut99-server", label="UT99", expect="on_join",
         why="Botpack adds bots to MinPlayers once a player is present",
         persist=(f"{HOME}/.utpg/System/UnrealTournament.ini", r"^MinPlayers=\d")),
    dict(unit="ut2004-server", label="UT2004", expect="on_join",
         why="?MinPlayers on the URL fills once a player is present",
         persist=None, unit_has=r"MinPlayers=\d"),
    dict(unit="tribes2-server", label="Tribes 2", expect="on_join", manager="docker",
         why="TribesNext encrypts the info response - no count is readable at all",
         persist=None),

    # --- cannot have bots, evidence in the game-servers skill ---
    dict(unit="specialists-server", label="Specialists", expect="none",
         why="TS 3.0's cmd_addbot prints 'Bots only in listen servers.'"),
    dict(unit="sof2-server", label="SoF II", expect="none",
         why="mp.pk3's botfiles/ is empty and no pk3 holds a single .aas"),
    dict(unit="deusex-server", label="Deus Ex", expect="none",
         why="its own source: 'No support done for this one'"),
    dict(unit="doom3-server", label="DOOM 3", expect="none",
         why="no bot string in gamex86.dll"),
    dict(unit="shogo-server", label="Shogo", expect="none", why="no bot string"),
    dict(unit="descent3-server", label="Descent 3", expect="none", why="no bot string"),
    dict(unit="farcry-server", label="Far Cry", expect="none", why="no bot string"),
    dict(unit="ssam-tfe-server", label="Serious Sam TFE", expect="none",
         why="co-op - the monsters are the enemy"),
    dict(unit="ssam-tse-server", label="Serious Sam TSE", expect="none",
         why="co-op - the monsters are the enemy"),
    dict(unit="carmageddon2-server", label="Carmageddon 2", expect="none",
         why="the GAME hosting a LAN lobby, not a dedicated server"),
]


# --- durability -------------------------------------------------------------

def linger_on():
    try:
        out = subprocess.run(["loginctl", "show-user", os.environ.get("USER", "voidsstr"),
                              "--property=Linger"], capture_output=True, text=True,
                             timeout=10).stdout
        return "Linger=yes" in out
    except Exception:
        return None


def unit_state(unit, manager="systemd"):
    if manager == "docker":
        try:
            out = subprocess.run(["docker", "inspect", "-f",
                                  "{{.State.Status}} {{.HostConfig.RestartPolicy.Name}}",
                                  unit], capture_output=True, text=True, timeout=10).stdout.split()
            return ("active" if out and out[0] == "running" else "down",
                    out[1] if len(out) > 1 and out[1] != "no" else None)
        except Exception:
            return ("unknown", None)
    def ask(what):
        return subprocess.run(["systemctl", "--user", what, unit],
                              capture_output=True, text=True, timeout=10).stdout.strip()
    return ask("is-active"), ask("is-enabled")


def persisted(spec):
    """Is the bot setting in a FILE the server reads at start?"""
    p = spec.get("persist")
    if p is None:
        if spec.get("unit_has"):
            try:
                out = subprocess.run(["systemctl", "--user", "cat", spec["unit"]],
                                     capture_output=True, text=True, timeout=10).stdout
                return bool(re.search(spec["unit_has"], out)), "unit ExecStart"
            except Exception:
                return None, "unit ExecStart"
        return None, None
    path, pat = p
    if not os.path.exists(path):
        return False, path
    if pat is None:
        return True, path
    with open(path, encoding="latin-1") as fh:
        return bool(re.search(pat, fh.read(), re.M)), path


_GS_BOTS = {}


def gameservers_bots():
    """Borrow gameservers.py's own per-engine probes.

    Three engines here (CS: Source's A2S bot field, Unreal Gold's and UT2003's
    native queries) already have a correct probe in that module. Re-deriving
    them would be a second place to get the Unreal family's "players includes
    bots" wrong, so ask it once and cache.
    """
    global _GS_BOTS
    if _GS_BOTS:
        return _GS_BOTS
    try:
        sys.path.insert(0, HERE)
        import gameservers
        for row in gameservers.collect()["servers"]:
            n = row.get("bots")
            if n is None and row.get("players") is not None and row.get("humans") == 0:
                n = row["players"]          # nobody on it: every player IS a bot
            _GS_BOTS[row["unit"]] = n
    except Exception:
        _GS_BOTS = {"__failed__": True}
    return _GS_BOTS


def check(spec):
    out = dict(unit=spec["unit"], label=spec["label"], expect=spec["expect"],
               want=spec.get("want"), why=spec.get("why"),
               note=spec.get("probe_note"))
    state, enabled = unit_state(spec["unit"], spec.get("manager", "systemd"))
    out["unit_state"], out["enabled"] = state, enabled

    ok_persist, where = persisted(spec)
    out["persisted"], out["persist_path"] = ok_persist, where

    if spec["expect"] == "none":
        out["verdict"] = NONE_POSSIBLE
        return out
    if spec["expect"] == "on_join":
        out["verdict"] = ON_JOIN if ok_persist is not False else BROKEN
        return out

    if state != "active":
        out["verdict"] = UNKNOWN          # a server that is down is a DIFFERENT
        out["bots"] = None                # problem from one that lost its bots
        return out

    n = None
    if spec.get("probe"):
        try:
            n = spec["probe"]()
        except Exception as exc:
            out["error"] = str(exc)
    if n is None:
        n = gameservers_bots().get(spec["unit"])
        if n is not None:
            out["note"] = (out.get("note") or "") + " [via gameservers.py]"
    out["bots"] = n

    if n is None:
        # could not ask -> unknown, never "no bots". Those are different calls
        # to action and only one of them is a fault.
        out["verdict"] = UNKNOWN
    elif n > 0:
        out["verdict"] = POPULATED if ok_persist is not False else BROKEN
        if ok_persist is False:
            out["why"] = "bots are up but the setting is not in any file - " \
                         "they will NOT come back after a restart"
    else:
        out["verdict"] = BROKEN
        out["why"] = "expected bots and found none"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", action="store_true", help="only problems")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    rows = [check(s) for s in SERVERS]
    ling = linger_on()
    broken = [r for r in rows if r["verdict"] == BROKEN]
    unknown = [r for r in rows if r["verdict"] == UNKNOWN]

    if args.json:
        print(json.dumps({"linger": ling, "servers": rows}, indent=2))
        return 1 if broken or ling is False else 0

    if not args.quiet:
        print("Fleet game-server bots\n")
        # Linger first: without it NOTHING below starts after a reboot, and
        # every unit still reads `enabled`.
        print("  linger (--user units start at boot): %s" %
              {True: "yes", False: "NO - nothing starts until someone logs in!",
               None: "unknown"}[ling])
        print()
        for r in rows:
            if r["verdict"] == NONE_POSSIBLE:
                continue
            mark = {POPULATED: " ok ", ON_JOIN: "join", BROKEN: "FAIL",
                    UNKNOWN: " ?  "}[r["verdict"]]
            n = r.get("bots")
            cnt = "-" if n is None else str(n)
            want = "" if r.get("want") is None else "/%s" % r["want"]
            print("  [%s] %-18s %-6s %-8s %s" % (
                mark, r["label"], cnt + want, r["unit_state"],
                r.get("note") or r.get("why") or ""))
        none = [r["label"] for r in rows if r["verdict"] == NONE_POSSIBLE]
        print("\n  no bots possible (%d): %s" % (len(none), ", ".join(none)))

    if broken:
        print("\n  %d SERVER(S) SHOULD HAVE BOTS AND DO NOT:" % len(broken))
        for r in broken:
            print("    %-18s %s" % (r["label"], r.get("why")))
    if unknown and not args.quiet:
        print("\n  %d could not be asked (down, or no probe here) - "
              "unknown is NOT the same as no bots" % len(unknown))
    if not broken and ling is not False and not args.quiet:
        print("\n  OK - every bot-capable server is populated or fills on join,")
        print("  every setting is in a file, and linger is on.")
    return 1 if broken or ling is False else 0


if __name__ == "__main__":
    sys.exit(main())
