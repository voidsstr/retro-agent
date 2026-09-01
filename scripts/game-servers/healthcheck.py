#!/usr/bin/env python3
"""Health-check every fleet game server on this host (192.168.1.132).

Each engine family needs its OWN query packet -- probing them all with Quake's
`getstatus` produces false "down" reports (Q2, UT and Tribes 2 all ignore it).
"""
import socket, re, struct, sys

HOST = "192.168.1.132"

def ask(port, payload, timeout=3.0, host=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(timeout)
    try:
        s.sendto(payload, (host or HOST, port)); return s.recv(16384)
    except Exception:
        return None
    finally:
        s.close()

def a2s(port):                      # GoldSrc: CS 1.6, The Specialists
    r = ask(port, b"\xff\xff\xff\xffTSource Engine Query\x00")
    if r and r[4:5] == b'A':        # anti-reflection challenge -> echo it back
        r = ask(port, b"\xff\xff\xff\xffTSource Engine Query\x00" + r[5:9])
    if not r: return None
    p = r[6:].split(b'\x00')
    return "%s | map=%s" % (p[0].decode('latin-1'), p[1].decode('latin-1'))

def q3(port):                       # Quake 3 engine: Q3A, OpenArena
    r = ask(port, b"\xff\xff\xff\xffgetstatus\n")
    if not r: return None
    t = r[4:].decode('latin-1', 'replace')
    n = re.search(r'\\sv_hostname\\([^\\]+)', t); m = re.search(r'\\mapname\\([^\\]+)', t)
    return "%s | map=%s" % (n.group(1) if n else '?', m.group(1) if m else '?')

def q2(port):                       # Quake 2 uses `status`, NOT `getstatus`
    r = ask(port, b"\xff\xff\xff\xffstatus\n")
    if not r: return None
    t = r[4:].decode('latin-1', 'replace')
    n = re.search(r'\\hostname\\([^\\]+)', t); m = re.search(r'\\mapname\\([^\\]+)', t)
    return "%s | map=%s" % (n.group(1) if n else '?', m.group(1) if m else '?')

def qw(port):                       # QuakeWorld mvdsv
    r = ask(port, b"\xff\xff\xff\xffstatus\n")
    if not r: return None
    t = r[4:].decode('latin-1', 'replace')
    n = re.search(r'\\hostname\\([^\\]+)', t); m = re.search(r'\\map\\([^\\]+)', t)
    return "%s | map=%s" % (n.group(1) if n else '?', m.group(1) if m else '?')

def ut(port):                       # GameSpy query = game port + 1
    """UT99, UT2004 -- and Serious Sam, which speaks the same packet.

    Serious Sam names the level `mapname`; the UT family names it `maptitle`.
    Reading only `maptitle` reported the Serious Sam servers as `map=?` while
    they were happily hosting.
    """
    r = ask(port, b"\\status\\")
    if not r: return None
    t = r.decode('latin-1', 'replace')
    n = re.search(r'\\hostname\\([^\\]+)', t)
    m = (re.search(r'\\maptitle\\([^\\]+)', t) or
         re.search(r'\\mapname\\([^\\]+)', t))
    return "%s | map=%s" % (n.group(1) if n else '?', m.group(1) if m else '?')

def unreal227(port):
    r"""Unreal 227 (Unreal Gold) -- `\info\`, NOT `\status\`.

    UT99 and UT2004 answer `\status\` with hostname and maptitle; Unreal 227's
    UdpServerQuery answers the SAME packet with only the *basic* block --
    `\gamename\unreal\gamever\227k\...` and nothing else. Probing it the UT
    way therefore yields a reply with no hostname and no map, which the shared
    `ut()` renders as "? | map=?": a server that is fully healthy, reported as
    nameless. The hostname/map live in the `\info\` response.
    """
    r = ask(port, b"\\info\\")
    if not r: return None
    t = r.decode('latin-1', 'replace')
    n = re.search(r'\\hostname\\([^\\]+)', t); m = re.search(r'\\mapname\\([^\\]+)', t)
    if not n: return None
    return "%s | map=%s" % (n.group(1), m.group(1) if m else '?')

def nq(port, game=b"QUAKE"):        # NetQuake control protocol: Quake 1, Hexen II
    """Quake 1 / Hexen II answer NEITHER `getstatus` NOR `status` -- they speak
    the Quake CONTROL protocol on the game port and drop the other two without
    a word, so probing them the Quake III way reports a healthy host as DOWN.

    Request [0x80|len : u32 BE][0x02]["QUAKE"\0][3];
    reply   [0x80|len][0x83][address\0][hostname\0][level\0][cur][max][proto].
    A Hexen II host answers only to b"HEXENII"; the wrong game string looks
    exactly like a dead box.
    """
    body = bytes([0x02]) + game + b"\x00" + bytes([3])
    r = ask(port, struct.pack(">I", 0x80000000 | (len(body) + 4)) + body)
    if not r or len(r) < 6 or r[4] != 0x83:
        return None
    f = r[5:].split(b"\x00", 3)
    if len(f) < 4:
        return None
    cur = f[3][0] if len(f[3]) > 0 else "?"
    mx = f[3][1] if len(f[3]) > 1 else "?"
    return "%s | map=%s | %s/%s" % (f[1].decode('latin-1'), f[2].decode('latin-1'), cur, mx)

def d3(port):                       # Descent 3: TCP accept, not a text query
    """Descent 3 answers no text query, but the dedicated server binds TCP 2092
    only once a mission is loaded -- so an accepted connection separates
    "hosting" from "process alive but serving nothing"."""
    s = socket.socket(); s.settimeout(3.0)
    try:
        s.connect((HOST, port)); return "Descent 3 accepting on TCP %d" % port
    except Exception:
        return None
    finally:
        s.close()

def udp_bound(port):                # Far Cry: LOCAL-ONLY liveness
    """Far Cry's LAN packet is proprietary and the server answers nothing we can
    spell, so this checks the post-condition instead: FarCry_WinSV.exe binds UDP
    49001 ONLY once a map is loaded. Works only ON this host -- which is where
    healthcheck.py runs."""
    import subprocess
    try:
        out = subprocess.run(["ss", "-lnuH"], capture_output=True, text=True,
                             timeout=3).stdout
    except Exception:
        return None
    return ("bound on UDP %d (map loaded)" % port) if re.search(r"[:\s]%d\s" % port, out) else None

def d3bfg(port):                    # DOOM 3: id Tech 4 connectionless getInfo
    r"""id Tech 4 speaks NEITHER `getstatus` NOR `\status\`.

    Its connectionless messages are `short 0xFFFF`, a NUL-terminated command
    string, then a long -- so a Quake III `getstatus` is dropped in silence and
    a healthy DOOM 3 server reads as dead.

    The reply layout, and the reason a naive split does not work: after
    `\xff\xff` + "infoResponse\0" come the echoed CHALLENGE (4 bytes) and the
    PROTOCOL (4 bytes) -- eight raw bytes that contain NULs of their own. Only
    then do the NUL-separated key/value pairs start, at offset 23. Splitting
    from byte 0 puts the parser one field out of phase and every value lands
    against the wrong key, which reads as "the server answered but has no
    name".
    """
    challenge = 0x1234ABCD
    r = ask(port, struct.pack("<H", 0xFFFF) + b"getInfo\x00" + struct.pack("<i", challenge))
    if not r or not r.startswith(b"\xff\xffinfoResponse\x00"):
        return None
    if struct.unpack("<i", r[15:19])[0] != challenge:
        return None                 # not an answer to OUR query
    fields = r[23:].split(b"\x00")
    kv = {}
    for i in range(0, len(fields) - 1, 2):
        k = fields[i].decode("latin-1", "replace")
        if not k:
            break                   # empty key terminates the dict
        kv[k] = fields[i + 1].decode("latin-1", "replace")
    if "si_name" not in kv:
        return None
    return "%s | map=%s" % (kv["si_name"], kv.get("si_map", "?"))

def t2(port):                       # Tribes 2 speaks the Torque binary query
    r = ask(port, bytes([0x0E, 0, 0, 0, 0, 0]), timeout=4.0)
    return ("Tribes 2 responded (%d bytes)" % len(r)) if r else None

CHECKS = [
    ("cs16-server",        "CS 1.6",              27018, a2s),
    ("  \\_ a2s proxy",    "CS 1.6 browser",      27015, a2s),
    ("cs16-noblood",       "CS 1.6 No Blood",     27019, a2s),
    ("  \\_ a2s proxy",    "No Blood browser",    27016, a2s),
    ("specialists-server", "The Specialists",     27017, a2s),
    ("hldm-server",        "Half-Life DM",        27021, a2s),
    ("  \\_ a2s proxy",    "Half-Life DM browser",27020, a2s),
    ("quake3-server",      "Quake III Arena",     27961, q3),
    ("q3ta-server",        "Quake III Team Arena",27962, q3),
    ("jka-server",         "Jedi Academy (JKA)",  29070, q3),
    ("sof2-server",        "Soldier of Fortune II",20100, q3),
    ("openarena-server",   "OpenArena",           27960, q3),
    # RTCW, added 2026-09-01. ioRTCW 1.51c serves com_protocol 61 AND
    # com_legacyprotocol 60 -- 60 is what retail 1.41 (the staged tree) speaks,
    # and it is 60 that the getinfo reply advertises, so the retail LAN browser
    # lists it. :27963 is deliberate: the Q3 engine's LAN scan broadcasts to
    # 27960-27963 only, so a port outside that range would never self-announce.
    ("rtcw-server",        "Return to Castle Wolfenstein", 27963, q3),
    ("quake2-server",      "Quake 2",             27910, q2),
    ("quake1-server",      "Quake 1 (NetQuake)",  26000, nq),
    ("quakeworld-server",  "QuakeWorld",          27502, qw),
    ("ut99-server",        "UT99 (query 7798)",    7798, ut),
    ("ut2004-server",      "UT2004 (query 7787)",  7787, ut),
    ("unrealgold-server",  "Unreal Gold (query 7808)", 7808, unreal227),
    # Deus Ex answers `\info\` like Unreal 227, NOT `\status\`. Query port
    # is the game port + 1 (7790 -> 7791); probing 7776/7777 times out and
    # reads as "no server at all".
    ("deusex-server",      "Deus Ex (query 7791)", 7791, unreal227),
    # Serious Sam speaks GameSpy on game port + 1, and names the level
    # `mapname` rather than the UT family's `maptitle`.
    ("ssam-tfe-server",    "Serious Sam TFE (q 25601)", 25601, ut),
    ("ssam-tse-server",    "Serious Sam TSE (q 25611)", 25611, ut),
    ("tribes2-server",     "Tribes 2",            28000, t2),
    # Wine-in-docker Windows servers. They were live on this host and absent
    # from THIS list, so the "one-shot check of every server" quietly checked
    # 18 of 20 -- an outage on either would have been invisible here.
    ("doom3-server",       "DOOM 3",              27666, d3bfg),
    ("descent3-server",    "Descent 3 (TCP 2092)", 2092, d3),
    ("farcry-server",      "Far Cry (local only)",49001, udp_bound),
]

fail = 0
print("Fleet game servers on %s\n" % HOST)
for unit, label, port, fn in CHECKS:
    res = fn(port)
    if res is None:
        fail += 1
        print("  [DOWN] %-20s %-22s :%-6d NO RESPONSE" % (unit, label, port))
    else:
        print("  [ OK ] %-20s %-22s :%-6d %s" % (unit, label, port, res))
print("\n%d/%d responding" % (len(CHECKS) - fail, len(CHECKS)))
sys.exit(1 if fail else 0)
