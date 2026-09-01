#!/usr/bin/env python3
"""gameservers.py — what every fleet game server is doing, right now.

`healthcheck.py` answers "is it up" for a human at a terminal. This module
answers the same question for a *machine*, and answers three more the login
wall needs: how many people are on it, what map, and how long the server took
to reply.

Two facts are gathered per server and they are deliberately kept apart:

  * the **unit** state, from systemd — did the process die?
  * the **probe** result, from the game's own query protocol — is it answering?

A server can be `active` and mute (wedged, or mid map-change), and it can be
`inactive` for the honest reason that it was never installed on this host. A
watchdog that cannot tell those apart either restarts healthy servers or
ignores dead ones, so both are reported and neither is inferred from the other.

Each engine needs its OWN query packet. Probing them all with Quake's
`getstatus` reports Q2, UT and Tribes 2 as down when they are fine -- the
mistake this module exists to stop anyone making again.

    python3 gameservers.py            # table, like healthcheck.py
    python3 gameservers.py --json     # the blob the dashboard collector reads
"""

import json
import os
import re
import re
import socket
import struct
import subprocess
import sys
import time

# The host the servers run on. They are Linux dedicated servers on the dev box
# (192.168.1.132); whitebeast carries the Windows-only ones.
HOST = os.environ.get("RETRO_GAMESERVER_HOST", "192.168.1.132")

DEFAULT_TIMEOUT = 2.0

# systemd --user is where these units live, so a root caller has to reach into
# the owning user's manager rather than its own.
UNIT_USER = os.environ.get("RETRO_GAMESERVER_USER", "voidsstr")


# --------------------------------------------------------------------------
# query protocols — one per engine family
# --------------------------------------------------------------------------

def _ask(port, payload, timeout=DEFAULT_TIMEOUT, host=None):
    """Send one UDP query, return (reply_bytes, rtt_ms) or (None, None)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    started = time.monotonic()
    try:
        sock.sendto(payload, (host or HOST, port))
        data = sock.recv(65535)
        return data, round((time.monotonic() - started) * 1000, 1)
    except Exception:
        return None, None
    finally:
        sock.close()


def _infostring(text):
    """`\\key\\value\\key\\value` → dict. Empty keys dropped."""
    parts = text.split("\\")
    out = {}
    for i in range(1, len(parts) - 1, 2):
        if parts[i]:
            out[parts[i]] = parts[i + 1]
    return out


def probe_a2s(port, timeout=DEFAULT_TIMEOUT, host=None):
    """GoldSrc / Source A2S_INFO — CS 1.6, The Specialists, any HLDS mod.

    Modern HLDS answers the Source-style `I` reply, and since the 2020
    anti-reflection change it may first answer `A` with a four-byte challenge
    that has to be echoed back. Reply layout after the header byte and the
    protocol byte is four NUL-terminated strings (name, map, folder, game),
    then appid:u16, players:u8, maxplayers:u8, bots:u8 -- which is where the
    player count this panel exists to show actually lives.
    """
    query = b"\xff\xff\xff\xffTSource Engine Query\x00"
    data, rtt = _ask(port, query, timeout, host)
    if data and data[4:5] == b"A":
        data, rtt = _ask(port, query + data[5:9], timeout, host)
    if not data or data[4:5] != b"I":
        return None
    body = data[6:]  # skip \xff\xff\xff\xff, 'I', protocol byte
    fields = body.split(b"\x00", 4)
    if len(fields) < 5:
        return None
    name, mapname, _folder, _game, rest = fields
    out = {
        "name": name.decode("latin-1", "replace"),
        "map": mapname.decode("latin-1", "replace"),
        "rtt_ms": rtt,
    }
    if len(rest) >= 5:
        out["players"] = rest[2]
        out["max_players"] = rest[3]
        out["bots"] = rest[4]
    return out


_PLAYER_RE = re.compile(r'^\s*(-?\d+)\s+(-?\d+)\s+"(.*)"\s*$')


def _quake_family(port, payload, host_key, map_key, timeout, host, info_line=1):
    r"""Shared shape for Q3/Q2/QW: an infostring, then one line per player.

    `info_line` is not a nicety. Q3 and Q2 answer `<header>\n\key\value...`,
    so the infostring is line 1 -- reading line 0 yields the header and an
    empty dict, which is how an earlier version reported healthy servers as
    empty. QuakeWorld's mvdsv puts its `n` header and the infostring on the
    SAME line, so there line 0 is right and line 1 is a stray NUL.

    Player lines are `<score> <ping> "<name>"`, and **ping 0 means a bot** --
    the only way to tell a bot-filled arena from a busy one on an engine that,
    unlike GoldSrc, has no bot count of its own.
    """
    data, rtt = _ask(port, payload, timeout, host)
    if not data:
        return None
    lines = data[4:].decode("latin-1", "replace").split("\n")
    info = _infostring(lines[info_line]) if len(lines) > info_line else {}
    players = bots = 0
    for line in lines[info_line + 1:]:
        line = line.strip().strip("\x00").strip()
        if not line:
            continue
        players += 1
        match = _PLAYER_RE.match(line)
        if match and int(match.group(2)) == 0:
            bots += 1
    out = {
        "name": info.get(host_key, "?"),
        "map": info.get(map_key, "?"),
        "players": players,
        "bots": bots,
        "rtt_ms": rtt,
    }
    for key in ("sv_maxclients", "maxclients"):
        if key in info:
            try:
                out["max_players"] = int(info[key])
            except ValueError:
                pass
            break
    return out


def probe_q3(port, timeout=DEFAULT_TIMEOUT, host=None):
    """Quake III engine — Q3A, OpenArena, ET, JK2/JKA, SoF2, MOHAA."""
    return _quake_family(port, b"\xff\xff\xff\xffgetstatus\n",
                         "sv_hostname", "mapname", timeout, host)


def probe_q2(port, timeout=DEFAULT_TIMEOUT, host=None):
    """Quake 2 speaks `status`, NOT `getstatus`."""
    return _quake_family(port, b"\xff\xff\xff\xffstatus\n",
                         "hostname", "mapname", timeout, host)


def probe_qw(port, timeout=DEFAULT_TIMEOUT, host=None):
    """QuakeWorld (mvdsv): map key is `map`, and the infostring is on line 0."""
    return _quake_family(port, b"\xff\xff\xff\xffstatus\n",
                         "hostname", "map", timeout, host, info_line=0)


def probe_ut(port, timeout=DEFAULT_TIMEOUT, host=None):
    """UT99 / UT2004 GameSpy query — note the QUERY port is game port + 1.

    This one hands us the player count directly, so nothing is counted by
    hand: `numplayers` already excludes spectators the way the browser does.
    """
    data, rtt = _ask(port, b"\\status\\", timeout, host)
    if not data:
        return None
    info = _infostring(data.decode("latin-1", "replace"))
    out = {
        "name": info.get("hostname", "?"),
        "map": info.get("maptitle") or info.get("mapname", "?"),
        "rtt_ms": rtt,
    }
    for src, dst in (("numplayers", "players"), ("maxplayers", "max_players")):
        if src in info:
            try:
                out[dst] = int(info[src])
            except ValueError:
                pass
    return out


def probe_unreal227(port, timeout=DEFAULT_TIMEOUT, host=None):
    r"""Unreal 227 (Unreal Gold) — `\info\`, NOT the UT family's `\status\`.

    UT99 and UT2004 answer `\status\` with hostname, maptitle and numplayers.
    Unreal 227's UdpServerQuery answers that SAME packet with only the *basic*
    block — `\gamename\unreal\gamever\227k\mingamever\224\location\0` —
    no hostname, no map, no count. Reusing probe_ut here would therefore report
    a perfectly healthy server as `? | map=? | 0 players`, which is the exact
    class of mistake this module exists to stop: a probe that guesses.

    Unreal's DeathMatchGame runs no bots unless MultiplayerBots is set, and
    this server leaves it False, so `numplayers` is a human count.
    """
    data, rtt = _ask(port, b"\\info\\", timeout, host)
    if not data:
        return None
    info = _infostring(data.decode("latin-1", "replace"))
    if "hostname" not in info:
        return None
    out = {"name": info["hostname"],
           "map": info.get("mapname", "?"),
           "rtt_ms": rtt}
    for src, dst in (("numplayers", "players"), ("maxplayers", "max_players")):
        if src in info:
            try:
                out[dst] = int(info[src])
            except ValueError:
                pass
    return out


def probe_idtech4(port, timeout=DEFAULT_TIMEOUT, host=None):
    r"""DOOM 3 -- id Tech 4 connectionless `getInfo`.

    id Tech 4 answers NEITHER `getstatus` NOR `\status\`. Its out-of-band
    messages are `short 0xFFFF`, a NUL-terminated command, then a long, so
    every probe in this module except this one reports a healthy DOOM 3 server
    as down.

    The reply is `\xff\xff` + "infoResponse\0" + the echoed CHALLENGE (4 bytes)
    + the PROTOCOL (4 bytes) + NUL-separated key/value pairs. Those eight raw
    bytes contain NULs, so the pairs start at offset 23 -- splitting from zero
    puts every value against the wrong key. The challenge is checked so a
    stray datagram cannot be read as an answer.

    DOOM 3 multiplayer has no bots, so the count is always human.
    """
    challenge = int.from_bytes(os.urandom(4), "little", signed=True)
    query = struct.pack("<H", 0xFFFF) + b"getInfo\x00" + struct.pack("<i", challenge)
    data, rtt = _ask(port, query, timeout, host)
    if not data or not data.startswith(b"\xff\xffinfoResponse\x00"):
        return None
    if len(data) < 23 or struct.unpack("<i", data[15:19])[0] != challenge:
        return None
    fields = data[23:].split(b"\x00")
    kv = {}
    for i in range(0, len(fields) - 1, 2):
        k = fields[i].decode("latin-1", "replace")
        if not k:
            break
        kv[k] = fields[i + 1].decode("latin-1", "replace")
    if "si_name" not in kv:
        return None
    out = {"name": kv["si_name"], "map": kv.get("si_map", "?"), "rtt_ms": rtt}
    try:
        out["max_players"] = int(kv.get("si_maxPlayers", "0"))
    except ValueError:
        pass
    return out


# Torque request -> expected response type. Tribes 2 answers a ping (0x0E) with
# 0x10 and an info request (0x12) with 0x14, echoing the four key bytes back.
_T2_PING = 0x0E
_T2_PING_REPLY = 0x10


def probe_t2(port, timeout=4.0, host=None):
    """Tribes 2 speaks the Torque binary query. Liveness only, and that is not
    laziness: under TribesNext the info response body is **encrypted**, so the
    player count and map simply are not readable from off the box. (Verified
    by hand: 0x12 returns a well-formed 0x14 whose payload is ciphertext.)

    What we can do is make the liveness answer trustworthy. The reply echoes
    the request's four key bytes, so a random key proves the packet is an
    answer to OUR query rather than any UDP traffic that happened to arrive —
    which is all "did some bytes come back" ever established.
    """
    key = os.urandom(4)
    data, rtt = _ask(port, bytes([_T2_PING, 0]) + key, timeout, host)
    if not data or len(data) < 6:
        return None
    if data[0] != _T2_PING_REPLY or data[2:6] != key:
        return None
    return {"name": "Tribes 2", "map": None, "rtt_ms": rtt}


# NetQuake / Hexen II speak neither `getstatus` nor `status`: they answer the
# Quake CONTROL protocol on the game port, and ignore the other two in silence.
# That silence is the whole hazard -- probed the Quake III way, a perfectly
# healthy Quake 1 or Hexen II host reads as DOWN.
_NQ_CTL_FLAG = 0x80000000
_NQ_CCREQ_SERVER_INFO = 0x02
_NQ_CCREP_SERVER_INFO = 0x83
_NQ_NET_PROTOCOL_VERSION = 3


def probe_nq(port, timeout=DEFAULT_TIMEOUT, host=None, game=b"QUAKE"):
    r"""NetQuake control query — Quake 1 (26000) and Hexen II (26900).

    Request:  [0x80 | len : u32 big-endian][0x02]["QUAKE"\0][protocol byte]
    Reply:    [0x80 | len][0x83][address\0][hostname\0][level\0][cur][max][proto]

    `game` is NOT decoration: a Hexen II host answers only to b"HEXENII" and
    drops b"QUAKE" without a word, so the wrong string here is indistinguishable
    from a dead box.

    The reply carries the server's own player count, so nothing is counted by
    hand and there is no ping-0 bot heuristic to get wrong -- neither engine
    has bots.
    """
    body = bytes([_NQ_CCREQ_SERVER_INFO]) + game + b"\x00" + bytes([_NQ_NET_PROTOCOL_VERSION])
    packet = struct.pack(">I", _NQ_CTL_FLAG | (len(body) + 4)) + body
    data, rtt = _ask(port, packet, timeout, host)
    if not data or len(data) < 6 or data[4] != _NQ_CCREP_SERVER_INFO:
        return None
    rest = data[5:]
    fields = rest.split(b"\x00", 3)
    if len(fields) < 4:
        return None
    _address, name, level, tail = fields
    out = {
        "name": name.decode("latin-1", "replace"),
        "map": level.decode("latin-1", "replace"),
        "bots": 0,
        "rtt_ms": rtt,
    }
    if len(tail) >= 2:
        out["players"] = tail[0]
        out["max_players"] = tail[1]
    return out


def probe_hexen2(port, timeout=DEFAULT_TIMEOUT, host=None):
    """Hexen II: the same control protocol, with the game string it answers to."""
    return probe_nq(port, timeout, host, game=b"HEXENII")


def probe_sof2(port, timeout=DEFAULT_TIMEOUT, host=None):
    r"""Soldier of Fortune II — Quake III `getstatus`, with one difference that
    matters: its player lines carry THREE numbers before the name, not two
    (`0 5 0 "B240"`), so the shared `<score> <ping> "<name>"` bot heuristic does
    not apply and must not be allowed to guess. SoF2 multiplayer ships no bots
    at all, so the honest answer is a hard zero rather than a parse.
    """
    out = probe_q3(port, timeout, host)
    if out is not None:
        out["bots"] = 0
    return out

# --------------------------------------------------------------------------
# The two Wine-hosted servers (Descent 3, Far Cry).  Both are Windows binaries
# run on this host under Wine in a container - see ~/descent3-server/_run and
# ~/farcry-server/_run - because neither game ever shipped a Linux server.
# --------------------------------------------------------------------------

def probe_d3(port, timeout=DEFAULT_TIMEOUT, host=None):
    """Descent 3 - TCP connect to the game port.

    D3 answers no text query we know, but it LISTENS on TCP 2092 as well as
    UDP, and only while a mission is actually loaded: the dedicated server
    binds the socket after `Mission '<x>.mn3' loaded successfully`.  So an
    accepted TCP connection separates "hosting" from "process alive but not
    serving", which is the distinction this module exists for.
    """
    sock = socket.socket()
    sock.settimeout(timeout)
    started = time.monotonic()
    try:
        sock.connect((host or HOST, port))
        return {"name": "Descent 3", "map": None,
                "rtt_ms": round((time.monotonic() - started) * 1000, 1)}
    except Exception:
        return None
    finally:
        sock.close()


def probe_udp_bound(port, timeout=DEFAULT_TIMEOUT, host=None):
    """LOCAL-ONLY liveness: is anything bound to this UDP port on this host?

    Far Cry's LAN discovery packet is proprietary and undocumented, and the
    server answers nothing we can spell - `\\status\\` and a bare NUL both time
    out (measured 2026-08-31).  Rather than invent a probe that can only ever
    say "no reply", this checks the post-condition that actually distinguishes
    the failure we hit: FarCry_WinSV.exe binds UDP 49001 **only once a map is
    loaded**, so an unbound port is exactly the "console is up, nothing is
    hosting" state, and a bound one is a real server.

    It is honest about its limit: it can only be run ON the server host, so it
    returns None (reported as no reply) from anywhere else rather than
    pretending to know.
    """
    if host not in (None, HOST, "127.0.0.1", "localhost"):
        return None
    try:
        out = subprocess.run(["ss", "-lnuH"], capture_output=True, text=True,
                             timeout=timeout).stdout
    except Exception:
        return None
    if re.search(r"[:\s]%d\s" % port, out):
        return {"name": "Far Cry", "map": None, "rtt_ms": None}
    return None


PROBES = {
    "a2s": probe_a2s,
    "q3": probe_q3,
    "q2": probe_q2,
    "qw": probe_qw,
    "ut": probe_ut,
    "unreal227": probe_unreal227,
    "idtech4": probe_idtech4,
    "t2": probe_t2,
    "nq": probe_nq,
    "hexen2": probe_hexen2,
    "sof2": probe_sof2,

    "d3": probe_d3,
    "udp_bound": probe_udp_bound,
}


# --------------------------------------------------------------------------
# the server table
# --------------------------------------------------------------------------
#
# `port` is the QUERY port, which is not always the port players connect to:
# UT's is game+1, and the CS 1.6 units are queried on 27018/27019 while old
# clients browse the a2s proxies on 27015/27016. `join` is what a person types.

SERVERS = [
    {"unit": "cs16-server",        "label": "CS 1.6",          "engine": "goldsrc",
     "probe": "a2s", "port": 27018, "join": 27015},
    {"unit": "cs16-noblood",       "label": "CS 1.6 no-blood", "engine": "goldsrc",
     "probe": "a2s", "port": 27019, "join": 27016},
    {"unit": "specialists-server", "label": "Specialists",     "engine": "goldsrc",
     "probe": "a2s", "port": 27017, "join": 27017},
    # Half-Life Deathmatch. The QUERY port is 27021 and the JOIN port is
    # 27020 (the proxy), same split as the CS units. NOTE the staged
    # HalfLife1 tree CANNOT reach this - it is the WON build, protocol 45,
    # and this is protocol 48. The client that can is the CounterStrike16
    # tree's engine: hl.exe -game valve +connect 192.168.1.132:27020
    {"unit": "hldm-server",        "label": "Half-Life DM",   "engine": "goldsrc",
     "probe": "a2s", "port": 27021, "join": 27020},
    {"unit": "quake3-server",      "label": "Quake III",       "engine": "q3",
     "probe": "q3",  "port": 27961, "join": 27961},
    {"unit": "openarena-server",   "label": "OpenArena",       "engine": "q3",
     "probe": "q3",  "port": 27960, "join": 27960},
    # fs_game missionpack. A baseq3 client cannot join this and a missionpack
    # client cannot join 27961, so it is a separate server and not a map.
    {"unit": "q3ta-server",        "label": "Q3 Team Arena",   "engine": "q3",
     "probe": "q3",  "port": 27962, "join": 27962},
    {"unit": "jka-server",         "label": "Jedi Academy",    "engine": "q3",
     "probe": "q3",  "port": 29070, "join": 29070},
    {"unit": "sof2-server",        "label": "SoF II",          "engine": "q3",
     "probe": "sof2", "port": 20100, "join": 20100},
    # RTCW, added 2026-09-01. ioRTCW 1.51c speaks com_protocol 61 AND
    # com_legacyprotocol 60; 60 is retail 1.41, which is what the staged tree
    # ships, and the getinfo reply advertises 60, so the retail LAN browser
    # lists it. 27963 is not arbitrary: the Q3 engine's LAN scan broadcasts to
    # 27960-27963 ONLY, so a server outside that window never self-announces.
    # RTCW MP shipped no bots, so this count is always human.
    {"unit": "rtcw-server",        "label": "RTCW",            "engine": "q3",
     "probe": "q3",  "port": 27963, "join": 27963},
    {"unit": "quake2-server",      "label": "Quake 2",         "engine": "q2",
     "probe": "q2",  "port": 27910, "join": 27910},
    # NetQuake, not QuakeWorld. GLQUAKE.EXE cannot join 27502 and mvdsv cannot
    # serve it -- two protocols, so two servers, and the staged Quake 1 tree is
    # a NetQuake client.
    {"unit": "quake1-server",      "label": "Quake (NetQuake)", "engine": "nq",
     "probe": "nq",  "port": 26000, "join": 26000},
    {"unit": "quakeworld-server",  "label": "QuakeWorld",      "engine": "qw",
     "probe": "qw",  "port": 27502, "join": 27502},
    {"unit": "ut99-server",        "label": "UT99",            "engine": "unreal",
     "probe": "ut",  "port": 7798,  "join": 7797},
    {"unit": "ut2004-server",      "label": "UT2004",          "engine": "ut2k4",
     "probe": "ut",  "port": 7787,  "join": 7777},
    # Unreal Gold on OldUnreal 227k. NOT the UT probe -- see probe_unreal227.
    # 7807/7808 because 7777 is UT2004 and 7797 is UT99; the server advertises
    # mingamever 224, so the staged 227k client joins it.
    {"unit": "unrealgold-server",  "label": "Unreal Gold",     "engine": "unreal",
     "probe": "unreal227", "port": 7808, "join": 7807},
    # Deus Ex. Same `\info\` probe as Unreal 227 and the same +1 query port
    # (7790 -> 7791); probing 7776/7777 times out and reads as "no server".
    {"unit": "deusex-server",      "label": "Deus Ex",         "engine": "unreal",
     "probe": "unreal227", "port": 7791, "join": 7790},
    # DOOM 3. Windows DOOM3DED.exe under Wine, because dhewm3 is not network
    # compatible with retail 1.3 and the fleet's staged client IS retail 1.3.
    {"unit": "doom3-server",       "label": "DOOM 3",          "engine": "idtech4",
     "probe": "idtech4", "port": 27666, "join": 27666},
    # Serious Sam speaks GameSpy on game port + 1 and names the level
    # `mapname`, which probe_ut already falls back to. TSE is on 25610 rather
    # than 25601 because Serious Engine opens the port AND port+1.
    {"unit": "ssam-tfe-server",    "label": "Serious Sam TFE",  "engine": "serioussam",
     "probe": "ut",  "port": 25601, "join": 25600},
    {"unit": "ssam-tse-server",    "label": "Serious Sam TSE",  "engine": "serioussam",
     "probe": "ut",  "port": 25611, "join": 25610},
    # Docker, not systemd: Tribes 2 needs a 2001 userland. See docker_states().
    {"unit": "tribes2-server",     "label": "Tribes 2",        "engine": "t2",
     "probe": "t2",  "port": 28000, "join": 28000, "manager": "docker"},
    # Wine in Docker, not systemd-only: these two are Windows dedicated servers
    # with no Linux build, run on this host inside retro-wine:bookworm. The
    # UNIT is still systemd --user, so `manager` stays default; only the probe
    # is unusual.
    {"unit": "descent3-server",    "label": "Descent 3",       "engine": "d3",
     "probe": "d3",  "port": 2092,  "join": 2092},
    {"unit": "farcry-server",      "label": "Far Cry",         "engine": "farcry",
     "probe": "udp_bound", "port": 49001, "join": 49001},
]

# The a2s proxies are what make the CS servers visible in a 2003 LAN browser.
# They are not game servers, so they get their own short list rather than
# padding the fleet's up/total with things nobody joins directly.
PROXIES = [
    {"unit": "a2s-proxy-cs16",        "label": "a2s 27015", "port": 27015},
    {"unit": "a2s-proxy-cs16-public", "label": "a2s 27016", "port": 27016},
    {"unit": "a2s-proxy-hldm",        "label": "a2s 27020", "port": 27020},
]


# --------------------------------------------------------------------------
# systemd --user, from wherever we happen to be running
# --------------------------------------------------------------------------

def _user_systemctl_prefix():
    """Argv prefix that reaches the game servers' `systemd --user` manager.

    The watchdog runs as the owning user, so a bare `systemctl --user` works.
    The dashboard collector runs as root, where `--user` means root's own
    manager -- which has none of these units and would report every server
    "not installed". Root therefore goes in through the user's runtime dir.
    """
    try:
        import pwd
        uid = pwd.getpwnam(UNIT_USER).pw_uid
    except Exception:
        uid = None
    if os.geteuid() != 0 or uid is None or uid == os.geteuid():
        return ["systemctl", "--user"]
    return [
        "setpriv", "--reuid", str(uid), "--regid", str(uid), "--clear-groups",
        "env", f"XDG_RUNTIME_DIR=/run/user/{uid}",
        "systemctl", "--user",
    ]


_UNIT_PROPS = ("ActiveState", "SubState", "LoadState", "Result",
               "NRestarts", "ExecMainStartTimestampMonotonic")


def unit_states(units, timeout=8):
    """One `systemctl show` for every unit, because a subprocess per server is
    ten forks every cycle for data that arrives in a single call."""
    out = {u: {"state": "unknown"} for u in units}
    if not units:
        return out
    cmd = _user_systemctl_prefix() + ["show", "--no-pager",
                                      "-p", ",".join(_UNIT_PROPS)]
    cmd += [f"{u}.service" for u in units]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except Exception:
        return out
    blocks = [b for b in res.stdout.split("\n\n") if b.strip()]
    now_mono = _monotonic_usec()
    for unit, block in zip(units, blocks):
        props = {}
        for line in block.splitlines():
            if "=" in line:
                key, val = line.split("=", 1)
                props[key] = val
        # LoadState=not-found is "we never installed this", which must not read
        # as a crashed server -- see the module docstring.
        if props.get("LoadState") == "not-found":
            rec = {"state": "absent"}
        else:
            rec = {
                "state": props.get("ActiveState") or "unknown",
                "sub": props.get("SubState") or None,
                "result": props.get("Result") or None,
            }
            try:
                rec["restarts"] = int(props.get("NRestarts") or 0)
            except ValueError:
                pass
            try:
                started = int(props.get("ExecMainStartTimestampMonotonic") or 0)
                if started and now_mono:
                    rec["uptime_sec"] = max(0, (now_mono - started) // 1_000_000)
            except ValueError:
                pass
        out[unit] = rec
    return out


def _monotonic_usec():
    try:
        with open("/proc/uptime") as fh:
            return int(float(fh.read().split()[0]) * 1_000_000)
    except Exception:
        return 0


# --------------------------------------------------------------------------
# docker — the fleet's second process manager
# --------------------------------------------------------------------------
#
# Tribes 2 runs in a container, not a systemd unit, because it needs a 2001
# userland. Looking it up with `systemctl show` returns `not-found`, which this
# module reports as "never installed here" -- so a running game server was
# being left off the wall entirely, and an outage on it would have been
# invisible. A server's manager is therefore declared, not assumed.

_DOCKER_STATE = {
    "running": "active",
    "restarting": "activating",
    "paused": "inactive",
    "created": "inactive",
    "exited": "failed",
    "dead": "failed",
}


def docker_states(names, timeout=8):
    """`docker inspect` for a batch of containers, in systemd's vocabulary."""
    out = {n: {"state": "unknown"} for n in names}
    if not names:
        return out
    fmt = "{{.Name}}\t{{.State.Status}}\t{{.State.StartedAt}}\t{{.RestartCount}}"
    try:
        res = subprocess.run(["docker", "inspect", "--format", fmt] + list(names),
                             capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        # No docker at all. That is not "the container is missing" -- it is
        # "we could not look", and the two must not render the same.
        return {n: {"state": "unknown", "error": "docker not installed"}
                for n in names}
    except Exception:
        return out

    seen = set()
    for line in res.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 4:
            continue
        name = parts[0].lstrip("/")
        seen.add(name)
        rec = {"state": _DOCKER_STATE.get(parts[1], "unknown"), "sub": parts[1]}
        try:
            rec["restarts"] = int(parts[3])
        except ValueError:
            pass
        started = _parse_docker_time(parts[2])
        if started:
            rec["uptime_sec"] = max(0, int(time.time() - started))
        out[name] = rec

    # `docker inspect` fails per-name on stderr; anything it did not describe
    # and did not error on is genuinely absent.
    for name in names:
        if name not in seen and out[name]["state"] == "unknown":
            out[name] = {"state": "absent"}
    return out


def _parse_docker_time(text):
    """RFC3339 with nanoseconds, which datetime cannot parse before 3.11."""
    try:
        import datetime
        cleaned = re.sub(r"\.(\d{6})\d*", r".\1", (text or "").strip())
        cleaned = cleaned.replace("Z", "+00:00")
        return datetime.datetime.fromisoformat(cleaned).timestamp()
    except Exception:
        return None


def restart_container(name, timeout=60):
    try:
        res = subprocess.run(["docker", "restart", name],
                             capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "restart timed out"
    except Exception as exc:
        return False, str(exc)
    if res.returncode == 0:
        return True, "restarted"
    return False, (res.stderr or res.stdout or "").strip()[:200] or "restart failed"


def restart_unit(unit, timeout=30):
    """Restart one game server. Returns (ok, message)."""
    cmd = _user_systemctl_prefix() + ["restart", f"{unit}.service"]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "restart timed out"
    except Exception as exc:
        return False, str(exc)
    if res.returncode == 0:
        return True, "restarted"
    return False, (res.stderr or res.stdout or "").strip()[:200] or "restart failed"


def restart(spec):
    """Restart one server through whichever manager owns it."""
    if spec.get("manager") == "docker":
        return restart_container(spec["unit"])
    return restart_unit(spec["unit"])


def _all_states(specs):
    """Unit states for a mixed list, one call per manager rather than per row."""
    states = {}
    systemd = [s["unit"] for s in specs if s.get("manager", "systemd") == "systemd"]
    docker = [s["unit"] for s in specs if s.get("manager") == "docker"]
    if systemd:
        states.update(unit_states(systemd))
    if docker:
        states.update(docker_states(docker))
    return states


# --------------------------------------------------------------------------
# assembly
# --------------------------------------------------------------------------

def collect(servers=None, proxies=None, timeout=DEFAULT_TIMEOUT, host=None):
    """Probe every server and read every unit state. No side effects."""
    servers = SERVERS if servers is None else servers
    proxies = PROXIES if proxies is None else proxies

    states = _all_states(list(servers) + list(proxies))

    rows = []
    for spec in servers:
        unit = states.get(spec["unit"], {})
        rec = {
            "unit": spec["unit"],
            "label": spec["label"],
            "engine": spec["engine"],
            "manager": spec.get("manager", "systemd"),
            "port": spec.get("join", spec["port"]),
            "query_port": spec["port"],
            "unit_state": unit.get("state", "unknown"),
            "unit_sub": unit.get("sub"),
            "restarts": unit.get("restarts"),
            "uptime_sec": unit.get("uptime_sec"),
            "up": False,
            "players": None,
            "max_players": None,
            "bots": None,
            "map": None,
            "name": None,
            "ping_ms": None,
        }
        # Not installed is not a fault, and probing a port nothing listens on
        # just spends the timeout.
        # `absent` = never installed here. `unknown` = we could not ask the
        # manager (no docker binary, daemon down). Neither is a game-server
        # fault, and neither may be counted as one.
        if unit.get("state") in ("absent", "unknown"):
            rec["installed"] = False
            rec["unavailable"] = unit.get("error") or (
                "manager unreachable" if unit.get("state") == "unknown" else None)
            rows.append(rec)
            continue
        rec["installed"] = True
        try:
            info = PROBES[spec["probe"]](spec["port"], timeout, host)
        except Exception:
            info = None
        if info:
            rec.update({
                "up": True,
                "name": info.get("name"),
                "map": info.get("map"),
                "players": info.get("players"),
                "max_players": info.get("max_players"),
                "bots": info.get("bots"),
                "ping_ms": info.get("rtt_ms"),
            })
        rows.append(rec)

    prox = []
    for spec in proxies:
        unit = states.get(spec["unit"], {})
        info = None
        if unit.get("state") != "absent":
            try:
                info = probe_a2s(spec["port"], timeout, host)
            except Exception:
                info = None
        prox.append({
            "unit": spec["unit"],
            "label": spec["label"],
            "port": spec["port"],
            "unit_state": unit.get("state", "unknown"),
            "installed": unit.get("state") != "absent",
            "up": bool(info),
        })

    installed = [r for r in rows if r.get("installed")]
    return {
        "ts": time.time(),
        "host": host or HOST,
        "servers": rows,
        "proxies": prox,
        "up": sum(1 for r in installed if r["up"]),
        "total": len(installed),
        "players": sum(r["players"] or 0 for r in installed),
        "bots": sum(r["bots"] or 0 for r in installed),
        # Humans, as far as we can tell them apart: GoldSrc reports bots inside
        # `players`, so subtracting them is the only honest "someone is here".
        "humans": sum(max(0, (r["players"] or 0) - (r["bots"] or 0))
                      for r in installed),
        "down": [r["unit"] for r in installed if not r["up"]],
    }


def main():
    as_json = "--json" in sys.argv
    snap = collect()
    if as_json:
        json.dump(snap, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0
    print(f"Fleet game servers on {snap['host']}   "
          f"{snap['up']}/{snap['total']} up · {snap['players']} players "
          f"({snap['humans']} human)\n")
    for r in snap["servers"]:
        if not r.get("installed"):
            print(f"  --  {r['label']:<18} not installed")
            continue
        mark = "OK " if r["up"] else "DOWN"
        players = "-" if r["players"] is None else \
            f"{r['players']}/{r['max_players'] or '?'}"
        ping = "-" if r["ping_ms"] is None else f"{r['ping_ms']}ms"
        print(f"  {mark} {r['label']:<18} {r['unit_state']:<10} "
              f"{players:>6} {ping:>8}  {r['map'] or ''}")
    for p in snap["proxies"]:
        if p["installed"]:
            print(f"  {'OK ' if p['up'] else 'DOWN'} {p['label']:<18} {p['unit_state']}")
    return 0 if not snap["down"] else 1


if __name__ == "__main__":
    sys.exit(main())
