#!/usr/bin/env python3
r"""Find any OTHER PXE boot server answering on this LAN - and look up MACs on Windows.

WHY THIS EXISTS. "Do not run two PXE hosts on one LAN" was a sentence in the
README, and a sentence stopped nothing. On 2026-09-25 a Dell Dimension 4600 was
PXE-installed three times; text mode finished every time and the box rebooted
to boot its disk. Our server held it - correctly silent - and a SECOND copy of
this very server, still scheduled on whitebeast (192.168.1.249) from before the
2026-08-24 move to the Linux host, answered instead. Its winnt.sif pointed at
the retired XPSP3-PXE tree, so setupldr's TFTP read of txtsetup.sif failed and
the screen said "INF file txtsetup.sif is corrupt or missing, status 21". Two
proxyDHCP servers answering one DISCOVER is a race; after our HOLD it is not a
race at all, the other one simply wins. Nothing on either host said so.

So the server now probes for another responder at startup and every few
minutes (pxe_server.py), and the self-test does the same.

HOW. A PXE boot server answers a unicast DHCPREQUEST carrying the PXEClient
vendor class on UDP 4011 by replying to the request's source address - so an
unprivileged socket hears it, and nothing but a PXE boot server listens there.
Replies whose next-server (siaddr) is OUR server_ip are ours (this host may
answer on several addresses) and are ignored.

The probe uses a locally-administered MAC (02:...), so no real machine's hold or
lease is touched by it.
"""
import ipaddress
import os
import re
import select
import socket
import struct
import subprocess
import time

MAGIC = b'\x63\x82\x53\x63'
PROBE_MAC = bytes.fromhex('02005eed0b07')
PROBE_MAC_TEXT = '02:00:5e:ed:0b:07'
PROBE_XID = 0x5058EB07


def build_probe(xid=PROBE_XID, mac=PROBE_MAC):
    """A PXE DHCPREQUEST, as a boot ROM sends it to port 4011."""
    def opt(code, payload):
        return bytes([code, len(payload)]) + payload
    pkt = struct.pack('!BBBBIHH', 1, 1, 6, 0, xid, 0, 0)
    pkt += b'\x00' * 16                              # ciaddr yiaddr siaddr giaddr
    pkt += mac + b'\x00' * 10                         # chaddr
    pkt += b'\x00' * 64 + b'\x00' * 128 + MAGIC       # sname, file
    pkt += opt(53, b'\x03')                           # DHCPREQUEST
    pkt += opt(93, struct.pack('!H', 0))              # arch: BIOS x86
    pkt += opt(60, b'PXEClient:Arch:00000:UNDI:002001')
    pkt += b'\xff'
    return pkt


def parse_reply(data, xid=PROBE_XID):
    """(ok, siaddr) for a BOOTP reply to our probe, else (False, None)."""
    if len(data) < 240 or data[0] != 2 or data[236:240] != MAGIC:
        return False, None
    if struct.unpack('!I', data[4:8])[0] != xid:
        return False, None
    return True, socket.inet_ntoa(data[20:24])


def lan_hosts(server_ip, prefix=24):
    """Every host address in server_ip's /prefix."""
    net = ipaddress.ip_network(f'{server_ip}/{prefix}', strict=False)
    return [str(h) for h in net.hosts()]


def find_other_pxe_servers(server_ip, hosts=None, port=4011, timeout=4.0,
                           rounds=2, pace=0.004):
    """IPs of PXE boot servers that are NOT this one. Never raises: a probe that
    cannot run returns None, which callers must report as 'could not check',
    never as 'no other server'.

    Sends are PACED and repeated. The first version fired 254 datagrams back to
    back and waited 2 s: a directed probe found whitebeast every time, and the
    sweep missed it - its log never even saw the packet, because a burst at
    hosts whose ARP entries are unresolved is dropped by the kernel before it
    reaches the wire. A detector that can miss the server it exists to find is
    worse than none, so the sweep is proven against a live responder in
    tests/test_pxe_rogue.py."""
    hosts = hosts if hosts is not None else lan_hosts(server_ip)
    found = {}
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('', 0))
    except OSError:
        return None
    try:
        pkt = build_probe()

        def drain(until):
            while True:
                left = until - time.monotonic()
                if left <= 0:
                    return
                # select, not the socket timeout: a fixed 0.2 s read timeout
                # turned every 4 ms pace step into 0.2 s - 100 s per sweep.
                if not select.select([s], [], [], left)[0]:
                    return
                try:
                    data, addr = s.recvfrom(2048)
                except OSError:
                    continue                          # ICMP port unreachable (Windows)
                ok, siaddr = parse_reply(data)
                if ok and siaddr != server_ip:        # not ourselves
                    found[addr[0]] = siaddr

        for _ in range(max(1, rounds)):
            for h in hosts:
                try:
                    s.sendto(pkt, (h, port))
                except OSError:
                    pass                              # unreachable: not a server
                drain(time.monotonic() + pace)        # pace the burst, keep reading
        drain(time.monotonic() + timeout)
    finally:
        s.close()
    return sorted(found)


# --------------------------------------------------------------------------
# MAC lookup - the serve-once hold is keyed on it
# --------------------------------------------------------------------------

_ARP_WIN = re.compile(r'^\s*(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F]{2}(?:-[0-9a-fA-F]{2}){5})\s')


def parse_windows_arp(text, ip):
    """MAC for ip from `arp -a` output (Windows: aa-bb-cc-dd-ee-ff)."""
    for line in text.splitlines():
        m = _ARP_WIN.match(line)
        if m and m.group(1) == ip and m.group(2) != 'ff-ff-ff-ff-ff-ff':
            return m.group(2).replace('-', ':').lower()
    return None


def mac_for_ip_windows(ip):
    """The hold's MAC lookup on a host with no /proc/net/arp. Without it the
    Windows copy of the server could never arm a hold, so it re-offered a boot
    file on every reboot - installed machines included."""
    try:
        out = subprocess.run(['arp', '-a', ip], capture_output=True, text=True,
                             timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return parse_windows_arp(out, ip)


if __name__ == '__main__':
    import sys
    me = sys.argv[1] if len(sys.argv) > 1 else '192.168.1.132'
    others = find_other_pxe_servers(me)
    if others is None:
        print('could not probe (socket error)')
        sys.exit(2)
    print('other PXE servers answering:', ', '.join(others) if others else 'none')
    sys.exit(1 if others else 0)
