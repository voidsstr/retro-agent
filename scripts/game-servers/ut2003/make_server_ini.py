#!/usr/bin/env python3
r"""Derive the UT2003 server's UT2003Server.ini from the STAGED client ini.

The server runs the staged tree's own System\UCC.exe (build 2225) over the
staged tree's own packages, because a UE2 client joins only a server of the
same build: the native browser reply carries a net version (121 = UT2003
2225, 128 = UT2004 3369) and the package GUIDs must match. So the server's
config is derived from the staged `System\UT2003.ini` rather than kept as a
second hand-maintained copy that can drift from it. What changes:

  * `[URL] Port=7757`  -- UT2004 holds 7777 / 7778 / 7787 (game, native
    query = game+1, GameSpy query = game+10) and every UE2 server needs all
    three. 7757 / 7758 / 7767 are free, and clear of UT99 (7797-7806, 8777),
    Unreal Gold (7807/7808, beacon 7775) and Deus Ex (7790/7791).
  * `[IpDrv.MasterServerUplink]` -- absent from the staged ini (IpDrv.u's
    defaults apply), so it is written out: no master uplink and no stats.
    This is a LAN server; the client finds it through the LAN responder on
    LANServerPort (10777), which is independent of DoUplink.
  * `[Engine.AccessControl] AdminPassword` -- the fleet's rcon convention.
  * `[Engine.GameReplicationInfo] ServerName/ShortName` -- named like the
    UT2004 server ("NSC Retro Fleet Arena").
  * `[Engine.GameInfo] bEnableStatLogging=False` -- no stats nag.
  * `[XGame.xDeathMatch]` -- MinPlayers=6 (bots fill to six, like UT2004),
    TimeLimit=15.
  * `[XInterface.MapListDeathMatch]` -- a rotation over every stock DM map
    the staged tree ships (checked against the tree by the installer).

    python3 make_server_ini.py <staged UT2003.ini> <out UT2003Server.ini>
"""

import re
import sys

GAME_PORT = 7757
SERVER_NAME = "NSC Retro Fleet Arena (UT2003)"
SHORT_NAME = "NSC UT2003"
ADMIN_PASSWORD = "retroadmin"
MIN_PLAYERS = 6
TIME_LIMIT = 15

# Every DM-*.ut2 in the staged Maps/ (2225 retail + the patch maps).
DM_ROTATION = [
    "DM-Antalus", "DM-Asbestos", "DM-Compressed", "DM-Curse3", "DM-Flux2",
    "DM-Gael", "DM-Inferno", "DM-Insidious", "DM-Leviathan", "DM-Oceanic",
    "DM-Phobos2", "DM-Plunge", "DM-Serpentine", "DM-TokaraForest",
    "DM-TrainingDay",
]

UPLINK = [
    ("DoUplink", "False"),
    ("UplinkToGamespy", "False"),
    ("SendStats", "False"),
    ("ServerBehindNAT", "True"),
    ("DoLANBroadcast", "True"),
]


def _sections(lines):
    """[(name or None, [lines])] preserving order and every original line."""
    out = [(None, [])]
    for line in lines:
        m = re.match(r"^\[([^\]]+)\]\s*$", line)
        if m:
            out.append((m.group(1), [line]))
        else:
            out[-1][1].append(line)
    return out


def _set_key(body, key, value):
    """Set `key=value` in a section body (first line is the header)."""
    pat = re.compile(r"^%s=" % re.escape(key), re.IGNORECASE)
    for i, line in enumerate(body):
        if pat.match(line):
            body[i] = f"{key}={value}"
            return body
    at = len(body)
    while at > 1 and not body[at - 1].strip():
        at -= 1
    body.insert(at, f"{key}={value}")
    return body


def _section(secs, name):
    for n, body in secs:
        if n is not None and n.lower() == name.lower():
            return body
    body = ["", f"[{name}]"]
    secs.append((name, body))
    return body


def server_ini(text):
    """The staged client ini in, the server ini out. Raises on a surprise."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.replace("\r\n", "\n").split("\n")
    trailing = lines and lines[-1] == ""
    if trailing:
        lines = lines[:-1]
    secs = _sections(lines)
    names = [n for n, _ in secs]
    for need in ("URL", "Engine.GameEngine", "IpDrv.MasterServerLink"):
        if need not in names:
            raise ValueError(f"staged ini has no [{need}] section")

    _set_key(_section(secs, "URL"), "Port", str(GAME_PORT))

    engine = _section(secs, "Engine.GameEngine")
    actors = [l.split("=", 1)[1].strip() for l in engine
              if l.lower().startswith("serveractors=")]
    if "IpDrv.MasterServerUplink" not in actors:
        raise ValueError("staged ini has no ServerActors=IpDrv.MasterServerUplink;"
                         " it carries the LAN responder - the server would be"
                         " invisible in the LAN browser")

    link = _section(secs, "IpDrv.MasterServerLink")
    lan = [l for l in link if l.lower().startswith("lanserverport=")]
    if lan != ["LANServerPort=10777"]:
        raise ValueError("staged client LANServerPort is %r, not 10777 - the "
                         "server must listen where the clients broadcast" % lan)

    uplink = _section(secs, "IpDrv.MasterServerUplink")
    for k, v in UPLINK:
        _set_key(uplink, k, v)

    _set_key(_section(secs, "Engine.AccessControl"), "AdminPassword",
             ADMIN_PASSWORD)
    gri = _section(secs, "Engine.GameReplicationInfo")
    _set_key(gri, "ServerName", SERVER_NAME)
    _set_key(gri, "ShortName", SHORT_NAME)
    _set_key(_section(secs, "Engine.GameInfo"), "bEnableStatLogging", "False")

    dm = _section(secs, "XGame.xDeathMatch")
    _set_key(dm, "MinPlayers", str(MIN_PLAYERS))
    _set_key(dm, "TimeLimit", str(TIME_LIMIT))

    # A map list is a repeated key, so it is rebuilt rather than _set_key'd.
    for i, (n, body) in enumerate(secs):
        if n == "XInterface.MapListDeathMatch":
            blank = [l for l in body[1:] if not l.strip()]
            secs[i] = (n, [body[0]] + ["Maps=%s" % m for m in DM_ROTATION]
                       + blank)
            break
    else:
        secs.append(("XInterface.MapListDeathMatch",
                     ["", "[XInterface.MapListDeathMatch]"]
                     + ["Maps=%s" % m for m in DM_ROTATION]))

    out = [l for _, body in secs for l in body]
    return newline.join(out) + (newline if trailing else "")


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    with open(argv[1], "r", encoding="latin-1", newline="") as fh:
        text = fh.read()
    with open(argv[2], "w", encoding="latin-1", newline="") as fh:
        fh.write(server_ini(text))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
