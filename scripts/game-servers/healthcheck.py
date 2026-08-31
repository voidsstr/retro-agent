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

def ut(port):                       # UT99/UT2004 GameSpy query = game port + 1
    r = ask(port, b"\\status\\")
    if not r: return None
    t = r.decode('latin-1', 'replace')
    n = re.search(r'\\hostname\\([^\\]+)', t); m = re.search(r'\\maptitle\\([^\\]+)', t)
    return "%s | map=%s" % (n.group(1) if n else '?', m.group(1) if m else '?')

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

def t2(port):                       # Tribes 2 speaks the Torque binary query
    r = ask(port, bytes([0x0E, 0, 0, 0, 0, 0]), timeout=4.0)
    return ("Tribes 2 responded (%d bytes)" % len(r)) if r else None

CHECKS = [
    ("cs16-server",        "CS 1.6",              27018, a2s),
    ("  \\_ a2s proxy",    "CS 1.6 browser",      27015, a2s),
    ("cs16-noblood",       "CS 1.6 No Blood",     27019, a2s),
    ("  \\_ a2s proxy",    "No Blood browser",    27016, a2s),
    ("specialists-server", "The Specialists",     27017, a2s),
    ("quake3-server",      "Quake III Arena",     27961, q3),
    ("q3ta-server",        "Quake III Team Arena",27962, q3),
    ("jka-server",         "Jedi Academy (JKA)",  29070, q3),
    ("sof2-server",        "Soldier of Fortune II",20100, q3),
    ("openarena-server",   "OpenArena",           27960, q3),
    ("quake2-server",      "Quake 2",             27910, q2),
    ("quake1-server",      "Quake 1 (NetQuake)",  26000, nq),
    ("quakeworld-server",  "QuakeWorld",          27502, qw),
    ("ut99-server",        "UT99 (query 7798)",    7798, ut),
    ("ut2004-server",      "UT2004 (query 7787)",  7787, ut),
    ("tribes2-server",     "Tribes 2",            28000, t2),
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
