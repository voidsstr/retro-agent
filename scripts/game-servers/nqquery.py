#!/usr/bin/env python3
"""NetQuake / Hexen II LAN control-protocol query (CCREQ_SERVER_INFO).

A Quake 1 or Hexen II server -- dedicated on this host, or a LISTEN server on a
fleet box -- answers on its GAME port with the Quake control protocol. It does
NOT answer Quake III's `getstatus` and does NOT answer Quake II's `status`; it
drops both without a word, so probing it either of those ways reports a
perfectly live host as dead. That is the mistake this file exists to prevent.

    python3 nqquery.py 192.168.1.132 26000 QUAKE      # the fleet Quake 1 server
    python3 nqquery.py 192.168.1.123 26900 HEXENII    # a box hosting Hexen II

THE GAME STRING IS NOT DECORATION. A Hexen II host replies only to "HEXENII"
and ignores "QUAKE" -- which is again indistinguishable from a dead box.
"""
import socket, sys, struct

NET_HEADER_FLAG_CTL = 0x80000000
CCREQ_SERVER_INFO   = 0x02
CCREP_SERVER_INFO   = 0x83

def query(ip, port, game=b"QUAKE", version=3, timeout=3.0):
    body = bytes([CCREQ_SERVER_INFO]) + game + b"\x00" + bytes([version])
    pkt  = struct.pack(">I", NET_HEADER_FLAG_CTL | (len(body) + 4)) + body
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(pkt, (ip, port))
        data, _ = s.recvfrom(4096)
    except Exception:
        return None
    finally:
        s.close()
    if len(data) < 5:
        return None
    ctl = struct.unpack(">I", data[:4])[0]
    if (ctl & 0xFFFF0000) != NET_HEADER_FLAG_CTL >> 0 & 0xFFFF0000:
        pass
    if data[4] != CCREP_SERVER_INFO:
        return None
    rest = data[5:]
    parts = rest.split(b"\x00")
    addr  = parts[0].decode('latin-1')
    name  = parts[1].decode('latin-1')
    level = parts[2].decode('latin-1')
    tail  = rest[len(parts[0]) + len(parts[1]) + len(parts[2]) + 3:]
    cur   = tail[0] if len(tail) > 0 else -1
    mx    = tail[1] if len(tail) > 1 else -1
    proto = tail[2] if len(tail) > 2 else -1
    return dict(address=addr, hostname=name, level=level,
                players=cur, maxplayers=mx, protocol=proto)

if __name__ == "__main__":
    ip   = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 26000
    game = sys.argv[3].encode() if len(sys.argv) > 3 else b"QUAKE"
    r = query(ip, port, game)
    print(r if r else "NO REPLY")
