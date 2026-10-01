#!/usr/bin/env python3
"""ipxsrv.py - a DOSBox-compatible IPX-over-UDP server that LOGS what it routes.

DOSBox's IPXNET tunnel (and DALI, the real-DOS IPX driver that joins one) speak
a tiny protocol: every UDP datagram carries a whole IPX header + payload, and a
node address is the client's IPv4 address + UDP port AS THE SERVER SEES IT.

  registration  dest socket 2, dest node 0.0.0.0  -> server acks with the
                client's own address in dest (that is how a client learns it)
  broadcast     dest node ff:ff:ff:ff:ff:ff        -> every OTHER client
  unicast       dest node = ip:port of a client     -> that client

This is the same routing as DOSBox 0.74 src/hardware/ipxserver.cpp. It exists
for diagnosis: a DOSBox `IPXNET STARTSERVER` on a fleet box says nothing about
what it forwards, so when two machines do not see each other's games there is
no way to tell "never registered" from "registered, packets dropped" from "the
game never broadcast". This prints all three.

    python3 scripts/fleet/ipxsrv.py [--port 213] [--log FILE] [--quiet-payload]

Port 213 is DOSBox's default but is privileged on Linux; DOSBox and DALI both
take a port argument (IPXNET CONNECT <ip> <port>, DALI <ip> <port>).
"""
import argparse
import socket
import struct
import sys
import time

HDR = 30
REG_SOCKET = 2


def node_of(addr):
    ip, port = addr
    return socket.inet_aton(ip) + struct.pack('>H', port)


def fmt_node(n):
    if n == b'\xff' * 6:
        return 'BROADCAST'
    if n == b'\0' * 6:
        return '0'
    return '%s:%d' % (socket.inet_ntoa(n[:4]), struct.unpack('>H', n[4:])[0])


def parse(pkt):
    if len(pkt) < HDR:
        return None
    _cs, length, _tc, ptype = struct.unpack('>HHBB', pkt[:6])
    dnet, dnode, dsock = pkt[6:10], pkt[10:16], struct.unpack('>H', pkt[16:18])[0]
    snet, snode, ssock = pkt[18:22], pkt[22:28], struct.unpack('>H', pkt[28:30])[0]
    return dict(length=length, ptype=ptype, dnet=dnet, dnode=dnode, dsock=dsock,
                snet=snet, snode=snode, ssock=ssock)


def ack(client_addr, server_node):
    """The registration reply: dest = the client's address as we see it."""
    return (struct.pack('>HHBB', 0xFFFF, HDR, 0, 0)
            + struct.pack('>I', 0) + node_of(client_addr) + struct.pack('>H', REG_SOCKET)
            + struct.pack('>I', 1) + server_node + struct.pack('>H', REG_SOCKET))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=213)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--log', default=None)
    ap.add_argument('--quiet-payload', action='store_true',
                    help='log one line per packet without the payload hex')
    a = ap.parse_args()
    out = open(a.log, 'a', buffering=1) if a.log else sys.stdout

    def log(msg):
        out.write('%s %s\n' % (time.strftime('%H:%M:%S'), msg))

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((a.bind, a.port))
    server_node = node_of((a.bind if a.bind != '0.0.0.0' else '127.0.0.1', a.port))
    clients = []                        # list of (ip, port) in registration order
    counts = {}
    log('ipxsrv listening on %s:%d' % (a.bind, a.port))
    while True:
        pkt, addr = s.recvfrom(2048)
        h = parse(pkt)
        if h is None:
            log('%s:%d short datagram (%d bytes) ignored' % (addr[0], addr[1], len(pkt)))
            continue
        if h['dsock'] == REG_SOCKET and h['dnode'][:4] == b'\0\0\0\0':
            if addr not in clients:
                clients.append(addr)
                log('REGISTER %s:%d (client %d)' % (addr[0], addr[1], len(clients)))
            else:
                log('re-register %s:%d' % addr)
            s.sendto(ack(addr, server_node), addr)
            continue
        if addr not in clients:
            log('DROP from unregistered %s:%d' % addr)
            continue
        key = (addr, h['dsock'], h['dnode'] == b'\xff' * 6)
        counts[key] = counts.get(key, 0) + 1
        n = counts[key]
        sent = 0
        if h['dnode'] == b'\xff' * 6:
            for c in clients:
                if c != addr:
                    s.sendto(pkt, c)
                    sent += 1
        else:
            want = (socket.inet_ntoa(h['dnode'][:4]), struct.unpack('>H', h['dnode'][4:])[0])
            for c in clients:
                if c == want:
                    s.sendto(pkt, c)
                    sent += 1
        # Log the first few of each flow and then every 100th, so a running
        # game does not flood the log but a stalled one is obvious.
        if n <= 5 or n % 100 == 0:
            log('%s:%d -> %s sock %04x->%04x type %d len %d forwarded to %d (#%d)%s' % (
                addr[0], addr[1], fmt_node(h['dnode']), h['ssock'], h['dsock'], h['ptype'],
                len(pkt), sent, n,
                '' if a.quiet_payload else ' ' + pkt[HDR:HDR + 24].hex()))


if __name__ == '__main__':
    main()
