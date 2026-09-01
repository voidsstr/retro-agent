"""masters.py — find live internet servers, per engine.

Two steps for every engine: ask a master server for a list of addresses, then
probe each address directly to find out whether it is actually alive and who
is on it. The probe is not optional. Master lists are mostly stale -- of ~940
Q3 addresses roughly 605 answer -- so a favorites list built from the master
alone is mostly dead entries, which is exactly the complaint that started
this.

Every engine here declares whether it HAS a working discovery path. Ones that
do not are reported as unsupported rather than silently returning an empty
list: "0 servers" and "we never looked" must not look the same to the caller.

Query packets follow scripts/game-servers/healthcheck.py, which already had
the per-engine formats right (Q2 uses `status`, not `getstatus`; UT answers
GameSpy on port+1; GoldSrc needs the A2S challenge echo).
"""
import concurrent.futures as cf
import os
import re
import socket
import struct
import time

DEFAULT_TIMEOUT = 2.5
PROBE_WORKERS = 96


# --- low-level UDP -----------------------------------------------------------

def _udp(host, port, payload, timeout=DEFAULT_TIMEOUT, reads=1):
    """Send one datagram, collect up to `reads` replies. Returns (data, rtt_ms)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    t0 = time.time()
    chunks = []
    try:
        s.sendto(payload, (host, port))
        for _ in range(reads):
            chunks.append(s.recv(65535))
    except (socket.timeout, OSError):
        pass
    finally:
        s.close()
    rtt = int((time.time() - t0) * 1000)
    return b"".join(chunks), rtt


def _infostring(text):
    """Parse Quake's \\key\\value\\key\\value info string."""
    parts = text.split("\\")
    out = {}
    for i in range(1, len(parts) - 1, 2):
        out[parts[i].lower()] = parts[i + 1]
    return out


def _split_status(data):
    """Return (info dict, player-line count) from any Quake-family status reply.

    The infostring is NOT reliably the first line. Q3 answers
    "\xff\xff\xff\xffstatusResponse\n\\key\\value...\n<players>", so taking
    line 0 yields the header and an empty dict -- which silently turned every
    single Q3 server into "no reply" (0 alive of 400) even though they all
    answered. Find the first line that actually carries a backslash instead,
    and count the non-empty lines after it as players.
    """
    text = data[4:].decode("latin-1", "replace") if len(data) > 4 else ""
    lines = text.split("\n")
    for idx, line in enumerate(lines):
        if "\\" in line:
            info = _infostring(line)
            if info:
                players = len([l for l in lines[idx + 1:] if l.strip()])
                return info, players
    return {}, 0


# --- Quake III family (q3, rtcw, et, mohaa, openarena) -----------------------

Q3_MASTERS = [
    ("master.ioquake3.org", 27950, b"\xff\xff\xff\xffgetservers 68 empty full"),
    ("master.quake3arena.com", 27950, b"\xff\xff\xff\xffgetservers 68 empty full"),
    ("dpmaster.deathmask.net", 27950,
     b"\xff\xff\xff\xffgetservers Quake3Arena 68 empty full"),
]
OA_MASTERS = [
    ("dpmaster.deathmask.net", 27950,
     b"\xff\xff\xff\xffgetservers OpenArena 71 empty full"),
]


def _parse_getservers(data):
    """getserversResponse: repeating '\\' + 4-byte IP + 2-byte BE port, ends '\\EOT'."""
    out = []
    i = data.find(b"getserversResponse")
    if i < 0:
        return out
    body = data[i + len(b"getserversResponse"):]
    j = 0
    while j < len(body) - 6:
        if body[j] != 0x5C:  # '\'
            j += 1
            continue
        if body[j + 1:j + 4] == b"EOT":
            break
        ip = ".".join(str(b) for b in body[j + 1:j + 5])
        port = struct.unpack(">H", body[j + 5:j + 7])[0]
        if port and not ip.startswith("0."):
            out.append(f"{ip}:{port}")
        j += 7
    return out


def _q3_probe(addr):
    host, port = addr.rsplit(":", 1)
    data, rtt = _udp(host, int(port), b"\xff\xff\xff\xffgetstatus\n")
    if not data:
        return None
    # Count player LINES rather than trusting a cvar: several builds publish
    # no player count at all, and the whole point is "servers with people on".
    info, players = _split_status(data)
    if not info:
        return None
    return {
        "addr": addr,
        "hostname": re.sub(r"\^.", "", info.get("sv_hostname", ""))[:120],
        "map": info.get("mapname", ""),
        "players": players,
        "maxplayers": int(info.get("sv_maxclients", "0") or 0),
        "ping_ms": rtt,
        "gamename": info.get("gamename", ""),
        "passworded": 1 if info.get("g_needpass", "0") not in ("0", "") else 0,
        "source": "q3master",
    }


# --- NetQuake / Hexen II -----------------------------------------------------
# NEITHER `getstatus` NOR `status`: the Quake CONTROL protocol on the game
# port. Both of the other two are dropped in silence, so probing a NetQuake
# server the Quake III way reports it down forever -- and our own servers are
# pinned into every box's favourites either way, which is exactly how a
# permanent false alarm gets normalised. Hence a real probe rather than None.
_NQ_CTL = 0x80000000
_NQ_CCREQ_SERVER_INFO = 0x02
_NQ_CCREP_SERVER_INFO = 0x83


def _nq_probe(addr, game=b"QUAKE"):
    host, port = addr.rsplit(":", 1)
    body = bytes([_NQ_CCREQ_SERVER_INFO]) + game + b"\x00" + bytes([3])
    packet = struct.pack(">I", _NQ_CTL | (len(body) + 4)) + body
    data, rtt = _udp(host, int(port), packet)
    if not data or len(data) < 6 or data[4] != _NQ_CCREP_SERVER_INFO:
        return None
    fields = data[5:].split(b"\x00", 3)
    if len(fields) < 4:
        return None
    _address, name, level, tail = fields
    return {
        "addr": addr,
        "hostname": name.decode("latin-1", "replace")[:120],
        "map": level.decode("latin-1", "replace"),
        "players": tail[0] if len(tail) > 0 else 0,
        "maxplayers": tail[1] if len(tail) > 1 else 0,
        "ping_ms": rtt,
        "gamename": "netquake",
        "passworded": 0,
        "source": "local",
    }


# --- Quake II ----------------------------------------------------------------
# master.q2servers.com wants a plain "query" on 27900 and often does not answer
# from a residential IP. Kept, but treated as best-effort.
Q2_MASTERS = [("master.q2servers.com", 27900, b"query"),
              ("q2master.hardcore.rocks", 27900, b"query")]


def _parse_q2_master(data):
    out = []
    # Reply is a header then packed 6-byte ip:port records.
    body = data.split(b"\n", 1)[-1] if b"\n" in data else data
    for i in range(0, len(body) - 5, 6):
        ip = ".".join(str(b) for b in body[i:i + 4])
        port = struct.unpack(">H", body[i + 4:i + 6])[0]
        if port and not ip.startswith("0."):
            out.append(f"{ip}:{port}")
    return out


def _q2_probe(addr):
    host, port = addr.rsplit(":", 1)
    data, rtt = _udp(host, int(port), b"\xff\xff\xff\xffstatus\n")
    if not data:
        return None
    info, players = _split_status(data)
    if not info:
        return None
    return {
        "addr": addr,
        "hostname": info.get("hostname", "")[:120],
        "map": info.get("mapname", ""),
        "players": players,
        "maxplayers": int(info.get("maxclients", "0") or 0),
        "ping_ms": rtt,
        "gamename": info.get("gamename", "baseq2"),
        "passworded": 1 if info.get("needpass", "0") not in ("0", "") else 0,
        "source": "q2master",
    }


# --- QuakeWorld --------------------------------------------------------------

QW_MASTERS = [("master.quakeworld.nu", 27000, b"c\n"),
              ("master.quakeservers.net", 27000, b"c\n")]


def _parse_qw_master(data):
    out = []
    i = data.find(b"\xff\xff\xff\xffd\n")
    body = data[i + 6:] if i >= 0 else data
    for j in range(0, len(body) - 5, 6):
        ip = ".".join(str(b) for b in body[j:j + 4])
        port = struct.unpack(">H", body[j + 4:j + 6])[0]
        if port and not ip.startswith("0."):
            out.append(f"{ip}:{port}")
    return out


def _qw_probe(addr):
    host, port = addr.rsplit(":", 1)
    data, rtt = _udp(host, int(port), b"\xff\xff\xff\xffstatus\n")
    if not data:
        return None
    info, players = _split_status(data)
    if not info:
        return None
    return {
        "addr": addr,
        "hostname": info.get("hostname", "")[:120],
        "map": info.get("map", ""),
        "players": players,
        "maxplayers": int(info.get("maxclients", "0") or 0),
        "ping_ms": rtt,
        "gamename": "qw",
        "passworded": 1 if info.get("needpass", "0") not in ("0", "") else 0,
        "source": "qwmaster",
    }


# --- GoldSrc (CS 1.6, HL, TFC, DoD, TS) --------------------------------------
# The Steam master speaks a request/continue protocol on 27011.

GOLDSRC_MASTER = ("hl2master.steampowered.com", 27011)


def _goldsrc_master_list(region=0xFF, filt=br"\gamedir\cstrike", limit=1500):
    out, last = [], "0.0.0.0:0"
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(DEFAULT_TIMEOUT)
    try:
        for _ in range(12):
            req = b"1" + bytes([region]) + last.encode() + b"\x00" + filt + b"\x00"
            s.sendto(req, GOLDSRC_MASTER)
            try:
                data = s.recv(65535)
            except (socket.timeout, OSError):
                break
            if len(data) < 6:
                break
            body = data[6:]
            batch = []
            for i in range(0, len(body) - 5, 6):
                ip = ".".join(str(b) for b in body[i:i + 4])
                port = struct.unpack(">H", body[i + 4:i + 6])[0]
                batch.append(f"{ip}:{port}")
            if not batch:
                break
            if batch[-1] == "0.0.0.0:0":
                out.extend(batch[:-1])
                break
            out.extend(batch)
            last = batch[-1]
            if len(out) >= limit:
                break
    finally:
        s.close()
    return out


def _a2s_probe(addr):
    host, port = addr.rsplit(":", 1)
    port = int(port)
    payload = b"\xff\xff\xff\xffTSource Engine Query\x00"
    data, rtt = _udp(host, port, payload)
    if data[4:5] == b"A":              # challenge -- echo it back
        data, rtt2 = _udp(host, port, payload + data[5:9])
        rtt += rtt2
    if not data or data[4:5] not in (b"I", b"m"):
        return None
    try:
        body = data[6:]
        parts = body.split(b"\x00")
        name, mapname, folder, game = (p.decode("latin-1", "replace")
                                       for p in parts[:4])
        rest = body.split(b"\x00", 4)[4]
        players, maxplayers = rest[2], rest[3]
    except (IndexError, ValueError):
        return None
    return {
        "addr": addr, "hostname": name[:120], "map": mapname,
        "players": players, "maxplayers": maxplayers, "ping_ms": rtt,
        "gamename": folder, "passworded": 0, "source": "steammaster",
    }


# --- Unreal engine 1 and 2 (UT99, Unreal Gold, Deus Ex, UT2004) --------------
#
# GameSpy died in 2014 and took the Unreal masters with it, so there is no list
# to ask. What there IS is a curated set of addresses -- and, crucially, a
# probe: `\status\` on the QUERY port, which is the game port + 1 by
# convention but not by rule (the fleet's own UT99 is 7797/7798, UT2004 is
# 7777/7787). Every seed is probed before it can reach anybody's favourites,
# so a dead entry in the list below costs nothing but a timeout.
#
# The reply carries `hostport`, which is the address a client actually
# connects to. That is what we store as `addr`; the query port is stored
# alongside it because the UT99 favourites format wants the query port and
# UT2004's wants both.

# Provenance: Games-Library/UnrealTournament/SERVERS-online.txt (community
# curation, refreshed 2026-04-19) plus the three servers the staged tree's own
# [UBrowser.UBrowserFavoritesFact] shipped with. Listed as GAME ports; the
# probe tries port+1 then the port itself, so either style works.
UNREAL_SEEDS = [
    "139.162.235.20:7777", "130.61.36.168:7777", "192.223.24.67:7777",
    "194.164.194.68:7787", "85.214.243.170:7777", "23.92.16.218:7777",
    "109.123.250.228:4444", "74.91.116.164:7777", "185.242.112.2:6677",
    "37.153.1.43:7777", "66.85.80.155:7797", "108.61.109.162:7777",
    "74.91.119.21:7777", "85.214.243.170:9000", "173.199.111.57:7777",
    "31.220.4.155:2250", "18.184.97.232:2250", "209.38.218.216:2250",
]


def _gamespy_status(host, port):
    r"""UT-family GameSpy query, with the ONE fallback Unreal 227 needs.

    UT99 and UT2004 answer `\status\` with hostname, maptitle and numplayers.
    **Unreal 227 (Unreal Gold) answers the same packet with only the *basic*
    block** -- `\gamename\unreal\gamever\227k\mingamever\224\location\0` -- so
    the caller's `"hostname" not in info` test rejects it and our own live
    Unreal Gold server on .132:7808 reads as down forever. Its hostname, map
    and player count are in the `\info\` response instead.

    So: ask `\status\`, and if the reply carries no hostname, ask `\info\` and
    merge. The extra packet only ever costs a round trip on a server that had
    already failed to answer the question.
    """
    data, rtt = _udp(host, port, b"\\status\\", reads=3)
    if not data:
        return None, rtt
    info = _infostring(data.decode("latin-1", "replace"))
    if info and "hostname" not in info:
        data2, rtt2 = _udp(host, port, b"\\info\\", reads=3)
        if data2:
            extra = _infostring(data2.decode("latin-1", "replace"))
            if extra.get("hostname"):
                merged = dict(info)
                merged.update(extra)
                return merged, (rtt2 if rtt2 is not None else rtt)
    return (info or None), rtt


def _gamespy_probe(addr, want_gamename=None, query_port=0):
    """Probe a UT-family server. `addr` may be the game port or the query port.

    Returns a row whose `addr` is the JOINABLE address (from the reply's own
    `hostport`) and whose `query_port` is where it answered -- the two are not
    the same and both are needed downstream.

    `query_port` short-circuits the guessing when the caller already knows it.
    That is not a nicety: the fleet's UT2004 answers on 7787 for game port
    7777, so trying port+1 and then the port itself finds nothing at all and
    our own live server reads as down.
    """
    host, port = addr.rsplit(":", 1)
    port = int(port)
    if query_port:
        info, rtt = _gamespy_status(host, query_port)
        qport = query_port
    else:
        info, rtt = _gamespy_status(host, port + 1)
        qport = port + 1
        if not info:
            info, rtt = _gamespy_status(host, port)
            qport = port
    if not info or "hostname" not in info:
        return None
    try:
        gameport = int(info.get("hostport") or (qport - 1))
    except ValueError:
        gameport = qport - 1
    gamename = info.get("gamename", "")
    if want_gamename and gamename.lower() != want_gamename:
        return None
    passworded = info.get("password", "0").strip().lower() not in ("0", "false", "")
    return {
        "addr": f"{host}:{gameport}",
        "query_port": qport,
        "hostname": " ".join(info.get("hostname", "").split())[:120],
        "map": info.get("maptitle") or info.get("mapname", ""),
        "players": int(info.get("numplayers", "0") or 0),
        "maxplayers": int(info.get("maxplayers", "0") or 0),
        "ping_ms": rtt,
        "gamename": gamename,
        "passworded": 1 if passworded else 0,
        "source": "seed",
    }


def _unreal_probe(addr):
    return _gamespy_probe(addr, want_gamename="ut")


def _ut2k4_probe(addr):
    return _gamespy_probe(addr, want_gamename="ut2004")


def _serioussam_probe(addr, query_port=0):
    """Serious Sam speaks GameSpy on the game port + 1, like the UT family.

    TFE reports `gamename\\serioussam` and TSE `gamename\\serioussamse` -- two
    different games on one host, which is exactly what `want_gamename` keeps
    apart, so the caller's declared name is used rather than a fixed one.
    """
    return _gamespy_probe(addr, query_port=query_port)


def _idtech4_probe(addr):
    r"""DOOM 3 -- id Tech 4's connectionless `getInfo`.

    NOT `getstatus` and NOT `\status\`: the message is `short 0xFFFF`, a
    NUL-terminated command, then a long. The reply is
    `\xff\xff` + "infoResponse\0" + the echoed challenge (4B) + protocol (4B)
    + NUL-separated key/value pairs, so the pairs start at offset 23; splitting
    from zero puts every value against the wrong key. The challenge is checked
    so a stray datagram cannot be mistaken for an answer.
    """
    host, port = addr.rsplit(":", 1)
    port = int(port)
    challenge = int.from_bytes(os.urandom(4), "little", signed=True)
    query = struct.pack("<H", 0xFFFF) + b"getInfo\x00" + struct.pack("<i", challenge)
    data, rtt = _udp(host, port, query, reads=3)
    if not data or not data.startswith(b"\xff\xffinfoResponse\x00") or len(data) < 23:
        return None
    if struct.unpack("<i", data[15:19])[0] != challenge:
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
    return {
        "addr": f"{host}:{port}", "query_port": port,
        "hostname": " ".join(kv["si_name"].split())[:120],
        "map": kv.get("si_map", ""),
        "players": 0, "maxplayers": int(kv.get("si_maxPlayers", "0") or 0),
        "ping_ms": rtt, "gamename": kv.get("gamename", "baseDOOM-1"),
        "passworded": 1 if kv.get("si_usepass", "0") not in ("0", "") else 0,
        "source": "local",
    }


# Engines whose probe needs to be told the query port rather than guess it.
_QUERY_PORT_ENGINES = {"unreal", "ut2k4", "serioussam", "lithtech"}


def probe_server(engine, addr, query_port=0, gamename=""):
    """Probe ONE known address, for the servers we already know we own.

    Separate from discover() on purpose: discovery answers "what is out
    there", this answers "is the thing we run still up", and the fleet's own
    servers must be verified rather than pinned on faith.
    """
    spec = ENGINES.get(engine) or {}
    probe = spec.get("probe")
    if probe is None:
        return None
    if query_port and engine in _QUERY_PORT_ENGINES:
        # The CALLER's declared gamename wins. Hardcoding "ut" for every
        # `unreal`-engine row was right while UT99 was the only one; Unreal
        # Gold reports `gamename\unreal` and was filtered out by its own
        # engine's probe.
        want = gamename.lower() or ("ut2004" if engine == "ut2k4" else "ut")
        if engine in ("serioussam", "lithtech") and not gamename:
            want = None          # TFE and TSE report different gamenames
        return _gamespy_probe(addr, want_gamename=want, query_port=query_port)
    return probe(addr)


# --- engine registry ---------------------------------------------------------

def _collect(masters, parser):
    seen = []
    for host, port, payload in masters:
        data, _ = _udp(host, port, payload, timeout=4.0, reads=8)
        if data:
            seen.extend(parser(data))
    return list(dict.fromkeys(seen))          # de-dup, keep order


ENGINES = {
    "q3":      dict(list=lambda: _collect(Q3_MASTERS, _parse_getservers),
                    probe=_q3_probe, supported=True),
    "q2":      dict(list=lambda: _collect(Q2_MASTERS, _parse_q2_master),
                    probe=_q2_probe, supported=True),
    "qw":      dict(list=lambda: _collect(QW_MASTERS, _parse_qw_master),
                    probe=_qw_probe, supported=True),
    # The Steam master is listed but NOT usable from here: every
    # *.steampowered.com master hostname fails DNS on this host (verified
    # 2026-08-25, while the Q3 and QuakeWorld masters resolve fine). Rather
    # than ship a discovery path that silently yields nothing, declare it
    # unsupported -- the LOCAL CS 1.6 / TS / DoD servers on .132 are still
    # pinned into favorites by the sync pass, which is the part that matters
    # on this LAN. _goldsrc_master_list/_a2s_probe are kept and tested so
    # this flips back on the day a reachable master exists.
    # The probe is wired even though discovery is not: it is what verifies our
    # OWN GoldSrc servers on .132 before they are pinned, instead of inventing
    # a row for a port nobody checked. Its PLAYER COUNT is not to be trusted --
    # A2S_INFO reported players=0 on :27015 while two real clients were playing,
    # because they arrive through the browser proxy and hlds logs them as the
    # host itself. Use the hlds log or rcon for population; A2S answers only
    # "this server is up, and this is its name and map".
    "goldsrc": dict(list=None, probe=_a2s_probe, supported=False,
                    why="Steam master hostnames do not resolve from this host"),
    # GameSpy is gone, so there is no master to ask -- but a CURATED SEED LIST
    # that is probed before anything is listed is a real discovery path, not a
    # guess, and `seeded` makes the log say so rather than implying a master
    # answered. UT2004 has no seed list because the only UT2004 server we can
    # reach is our own, which the sync pass pins directly.
    "unreal":  dict(list=lambda: list(UNREAL_SEEDS), probe=_unreal_probe,
                    supported=True, seeded=True),
    "ut2k4":   dict(list=None, probe=_ut2k4_probe, supported=False,
                    why="no live UT2004 master and no curated seed list; the "
                        "fleet's own server on .132 is pinned directly"),
    "t2":      dict(list=None, probe=None, supported=False,
                    why="TribesNext master not implemented"),
    # RTCW's own master is long dead, but the PROBE is wired now: it is what
    # verifies the fleet's rtcw-server on .132:27963 before the sync pass pins
    # it. RTCW is a Quake III engine, so `getstatus` is the right packet.
    "rtcw":    dict(list=None, probe=_q3_probe, supported=False,
                    why="wolfmaster.idsoftware.com is long dead; the fleet's "
                        "own server on .132:27963 is pinned directly"),
    # No master for either of these and no discovery -- the probes exist so our
    # OWN servers are verified before being pinned rather than asserted.
    "serioussam": dict(list=None, probe=_serioussam_probe, supported=False,
                    why="GameSpy is gone and Croteam's master with it; the "
                        "fleet's own TFE/TSE servers are pinned directly"),
    "lithtech": dict(list=None, probe=_gamespy_probe, supported=False,
                    why="Shogo's GameSpy master is gone; the fleet's own "
                        "server on .132:27888 is pinned directly"),
    "idtech4": dict(list=None, probe=_idtech4_probe, supported=False,
                    why="id's DOOM 3 master is long dead; the fleet's own "
                        "server on .132:27666 is pinned directly"),
    # No master exists for NetQuake, so there is no discovery -- but the probe
    # IS wired, because it is what verifies the fleet's own quake1-server on
    # :26000 before the sync pass pins it.
    "nq":      dict(list=None, probe=_nq_probe, supported=False,
                    why="NetQuake has no live master; the fleet's own server "
                        "on .132:26000 is pinned directly"),
}


def discover(engine, max_probe=900, workers=PROBE_WORKERS):
    """Return (rows, note). rows may be empty; note explains why if it is."""
    spec = ENGINES.get(engine)
    if spec is None:
        return [], f"unknown engine '{engine}'"
    if not spec["supported"]:
        return [], f"unsupported: {spec.get('why', 'no discovery path')}"

    if spec.get("list") is None:
        return [], "no discovery list for this engine (local servers are " \
                   "pinned by the sync pass)"
    addrs = spec["list"]()
    seeded = spec.get("seeded")
    if not addrs:
        return [], ("seed list is empty" if seeded else
                    "master returned no addresses (master down or filtered)")
    addrs = addrs[:max_probe]

    rows = []
    with cf.ThreadPoolExecutor(workers) as ex:
        for r in ex.map(spec["probe"], addrs):
            if r:
                rows.append(r)
    where = "seeded (no master)" if seeded else "listed"
    return rows, f"{len(rows)} alive of {len(addrs)} {where}"
