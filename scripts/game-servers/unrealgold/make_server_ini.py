#!/usr/bin/env python3
r"""Derive the Unreal Gold 226 server's UnrealServer.ini from the STAGED client ini.

The server must run the staged tree's own packages -- that is the whole reason
it exists (a 227k server's UnrealI.u is a later package generation and the
staged 226 client refuses it: "Package 'UnrealI' version mismatch"). So the
server's config is derived from the staged `System\Unreal.ini` rather than
kept as a second hand-maintained copy that can drift from it. Only these
settings differ from what every fleet box receives:

  * `[URL] Port=7807`       -- 7777 is UT2004 on this host and 7797 is UT99.
                               UE1's UdpServerQuery binds the NEXT free port,
                               so the query port is 7808.
  * the `IpServer.UdpServerUplink` ServerActors are dropped -- they point at
    master0.gamespy.com and friends, which have been gone for two decades;
    this is a LAN server and the fleet finds it by the 7775 beacon or by
    address. UdpBeacon and UdpServerQuery stay: they ARE the discovery.
  * `[Engine.GameInfo] AdminPassword` -- the fleet's rcon convention.
  * `[Engine.GameReplicationInfo] ServerName/ShortName` -- the same name the
    227k server advertised, so the favourites and the status wall read the
    same.
  * bots (2026-10-01, the fleet convention of 3 bots on "Hard"):
    `[UnrealShare.DeathMatchGame] bMultiPlayerBots=True InitialBots=3` and
    `[UnrealShare.BotInfo] Difficulty=2`. Unreal 226's own DeathMatchGame
    (source text in UnrealShare.u) spawns InitialBots only when
    bMultiPlayerBots is set (or in single player), and they are a FIXED
    number -- 226 has no MinPlayers backfill, unlike UT99. BotInfo.Difficulty
    is 0 Easy, 1 Medium, 2 Hard, 3 Unreal. Bots are server-side pawns of the
    stock UnrealI classes, so a vanilla 226 client needs nothing new.

    python3 make_server_ini.py <staged Unreal.ini> <out UnrealServer.ini>
"""

import re
import sys

GAME_PORT = 7807
SERVER_NAME = "NSC Retro Fleet Arena (Unreal Gold)"
SHORT_NAME = "NSC Unreal"
ADMIN_PASSWORD = "retroadmin"
# Fleet bot convention: 3 bots, "Hard" -- Unreal's BotInfo.Difficulty scale is
# 0 Easy, 1 Medium, 2 Hard, 3 Unreal.
BOT_COUNT = 3
BOT_DIFFICULTY = 2


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
    # Insert after the last non-blank line so the blank separator stays put.
    at = len(body)
    while at > 1 and not body[at - 1].strip():
        at -= 1
    body.insert(at, f"{key}={value}")
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

    for need in ("URL", "Engine.GameEngine"):
        if need not in names:
            raise ValueError(f"staged ini has no [{need}] section")

    for i, (name, body) in enumerate(secs):
        if name == "URL":
            _set_key(body, "Port", str(GAME_PORT))
        elif name == "Engine.GameEngine":
            kept = [l for l in body
                    if not re.match(r"^ServerActors=IpServer\.UdpServerUplink\b",
                                    l, re.IGNORECASE)]
            actors = [l.split("=", 1)[1].split()[0] for l in kept
                      if l.lower().startswith("serveractors=")]
            for need in ("IpDrv.UdpBeacon", "IpServer.UdpServerQuery"):
                if need not in actors:
                    raise ValueError(f"staged ini has no ServerActors={need}; "
                                     "the server would be invisible")
            secs[i] = (name, kept)
        elif name == "Engine.GameInfo":
            _set_key(body, "AdminPassword", ADMIN_PASSWORD)

    if "Engine.GameInfo" not in names:
        secs.append(("Engine.GameInfo", ["", "[Engine.GameInfo]",
                                         f"AdminPassword={ADMIN_PASSWORD}"]))
    if "Engine.GameReplicationInfo" in names:
        for name, body in secs:
            if name == "Engine.GameReplicationInfo":
                _set_key(body, "ServerName", SERVER_NAME)
                _set_key(body, "ShortName", SHORT_NAME)
    else:
        secs.append(("Engine.GameReplicationInfo",
                     ["", "[Engine.GameReplicationInfo]",
                      f"ServerName={SERVER_NAME}", f"ShortName={SHORT_NAME}"]))

    for sec, key, value in (
            ("UnrealShare.DeathMatchGame", "bMultiPlayerBots", "True"),
            ("UnrealShare.DeathMatchGame", "InitialBots", str(BOT_COUNT)),
            ("UnrealShare.BotInfo", "Difficulty", str(BOT_DIFFICULTY))):
        for name, body in secs:
            if name == sec:
                _set_key(body, key, value)
                break
        else:
            secs.append((sec, ["", f"[{sec}]", f"{key}={value}"]))

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
