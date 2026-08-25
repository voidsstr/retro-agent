#!/usr/bin/env python3
"""Report whether each game server is actually VISIBLE to the public.

healthcheck.py answers "is it running on the LAN". This answers the different
and harder question: "can a stranger on the internet find it?" -- by asking the
game's real master server whether our public IP is in its list.

A master only lists a server it can successfully probe back, so "NOT LISTED"
almost always means inbound UDP is not reaching us (no NAT port-forward),
NOT that the server is down.
"""
import socket, struct, sys, urllib.request, re

TIMEOUT = 6

def public_ip():
    for url in ("https://api.ipify.org", "https://ifconfig.me"):
        try:
            return urllib.request.urlopen(url, timeout=8).read().decode().strip()
        except Exception:
            continue
    return None

# ---------- dpmaster family (Quake III engine) ----------
def dpmaster(master, port, proto):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(TIMEOUT)
    try:
        s.sendto(b"\xff\xff\xff\xffgetservers %d empty full" % proto, (master, port))
        data = b""
        while True:
            try: data += s.recv(65535)
            except socket.timeout: break
        out = []
        for chunk in data.split(b"getserversResponse")[1:]:
            i = 0
            while i + 7 <= len(chunk):
                if chunk[i:i+1] != b"\\": i += 1; continue
                ip = ".".join(str(b) for b in chunk[i+1:i+5])
                pt = struct.unpack(">H", chunk[i+5:i+7])[0]
                if ip != "0.0.0.0": out.append("%s:%d" % (ip, pt))
                i += 7
        return out
    except Exception as e:
        return ["ERR:%s" % e]
    finally:
        s.close()

# ---------- Quake 2 master (GameSpy-ish, port 27900) ----------
def q2master(master, port=27900):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(TIMEOUT)
    try:
        s.sendto(b"query", (master, port))
        data = b""
        while True:
            try: data += s.recv(65535)
            except socket.timeout: break
        body = data.split(b"servers ")[-1] if b"servers " in data else data
        out = []
        for i in range(0, len(body) - 5, 6):
            ip = ".".join(str(b) for b in body[i:i+4])
            pt = struct.unpack(">H", body[i+4:i+6])[0]
            if ip != "0.0.0.0" and pt: out.append("%s:%d" % (ip, pt))
        return out
    except Exception as e:
        return ["ERR:%s" % e]
    finally:
        s.close()

# ---------- QuakeWorld master (port 27000, 'c' query) ----------
def qwmaster(master, port=27000):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(TIMEOUT)
    try:
        s.sendto(b"c\n", (master, port))
        data = b""
        while True:
            try: data += s.recv(65535)
            except socket.timeout: break
        body = data[6:] if len(data) > 6 else b""
        out = []
        for i in range(0, len(body) - 5, 6):
            ip = ".".join(str(b) for b in body[i:i+4])
            pt = struct.unpack(">H", body[i+4:i+6])[0]
            if ip != "0.0.0.0" and pt: out.append("%s:%d" % (ip, pt))
        return out
    except Exception as e:
        return ["ERR:%s" % e]
    finally:
        s.close()

# ---------- web listings (UT / Tribes 2) ----------
def weblist(url):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        return urllib.request.urlopen(req, timeout=12).read().decode("utf-8", "replace")
    except Exception as e:
        return "ERR:%s" % e

def main():
    ip = public_ip()
    if not ip:
        print("could not determine public IP"); return 2
    print("Public IP: %s\n" % ip)
    print("  %-22s %-8s %s" % ("SERVER", "PORT", "PUBLIC MASTER VISIBILITY"))
    print("  " + "-" * 74)

    rows, listed = [], 0

    checks = [
        ("Quake III Arena", 27961, lambda: dpmaster("dpmaster.deathmask.net", 27950, 68)
                                            + dpmaster("master.ioquake3.org", 27950, 68)),
        ("OpenArena",       27960, lambda: dpmaster("dpmaster.deathmask.net", 27950, 71)),
        ("Quake 2",         27910, lambda: q2master("master.quakeservers.net")
                                            + q2master("master.q2servers.com")),
        ("QuakeWorld",      27502, lambda: qwmaster("master.quakeworld.nu")
                                            + qwmaster("master.quakeservers.net")),
    ]
    for name, port, fn in checks:
        lst = fn()
        errs = [x for x in lst if x.startswith("ERR:")]
        hits = [x for x in lst if x.startswith(ip + ":")]
        if hits:
            listed += 1
            status = "LISTED as %s" % ", ".join(hits)
        elif len(lst) == len(errs):
            status = "master unreachable (%s)" % (errs[0][:40] if errs else "?")
        else:
            status = "NOT LISTED  (master saw %d servers, none ours)" % (len(lst) - len(errs))
        rows.append((name, port, status))

    # UT99 / UT2004 uplink to GameSpy-protocol masters (333networks, openspy).
    # Those have no clean machine-readable list -- the web browser is paginated
    # and its search does not filter -- so DO NOT claim a negative here. Report
    # it as unverified with the URL rather than print a confident wrong answer.
    for name, port in (("UT99", 7797), ("UT2004", 7777)):
        rows.append((name, port,
                     "UNVERIFIED - check https://master.333networks.com/s for %s" % ip))

    t2 = weblist("http://master.tribesnext.com/list")
    if t2.startswith("ERR:"):
        rows.append(("Tribes 2", 28000, "master unreachable (%s)" % t2[4:44]))
    elif ip in t2:
        listed += 1; rows.append(("Tribes 2", 28000, "LISTED on tribesnext"))
    else:
        rows.append(("Tribes 2", 28000, "NOT LISTED on tribesnext"))

    # LAN-only by design
    for name, port in (("CS 1.6 vanilla", 27018), ("CS 1.6 no blood", 27019),
                       ("The Specialists", 27017)):
        rows.append((name, port, "LAN-ONLY by design (sv_lan 1; needs sv_lan 0 + Steam GSLT)"))

    for name, port, status in rows:
        mark = "OK  " if status.startswith("LISTED") else "-- "
        print("  %s %-22s %-8s %s" % (mark, name, port, status))

    print("\n%d server(s) visible on a public master." % listed)
    if listed == 0:
        print("\nZero public visibility. Masters only list servers they can probe back,")
        print("so this points at inbound UDP being blocked -- i.e. no NAT port-forward")
        print("on the gateway. See scripts/game-servers/README.md (Publishing section).")
    return 0 if listed else 1

if __name__ == "__main__":
    sys.exit(main())
