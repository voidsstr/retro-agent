#!/usr/bin/env python3
"""pxe_selftest.py - prove the PXE server would actually boot a machine.

Runs the same two exchanges a 440BX/P3/P4 boot ROM performs, against the live
server, and checks the fields that decide whether the target boots or sits at a
blank screen:

  1. DHCP DISCOVER with vendor class PXEClient -> we must get an OFFER whose
     yiaddr is 0.0.0.0 (we are a PROXY - the router still does addressing),
     with next-server pointing at this host and the boot file in BOTH the BOOTP
     `file` field and option 67, because different ROMs read different ones.
  2. TFTP GET of every file the boot chain asks for, including the two naming
     quirks NTLDR actually uses - a leading backslash and upper case.

Why this exists: every failure in this path is silent from the client end. The
machine shows "DHCP." then "TFTP." and stops, with no message naming what went
wrong. Running this from the host answers it in two seconds.

  sudo python3 pxe_selftest.py            # needs root ONLY for the DHCP half
  python3 pxe_selftest.py --tftp-only     # no privileges needed

Root is required because the proxy broadcasts its OFFER to 255.255.255.255:68,
so the test has to listen on port 68 exactly as a real client does. An
ephemeral port never sees the reply - which looks like "no answer" and is the
single most misleading way to test this.
"""
import argparse
import hashlib
import os
import socket
import struct
import sys
import uuid

MAGIC = b'\x63\x82\x53\x63'
BOOT_FILES = ['startrom.n12', 'ntldr', 'ntdetect.com', 'winnt.sif']
# NTLDR asks for files with a leading backslash and in mixed case; the TFTP
# path resolver has to accept both or the boot dies after startrom loads.
QUIRK_NAMES = ['\\ntldr', 'NTDETECT.COM']

ok_count = 0
fail_count = 0


def ok(msg):
    global ok_count
    ok_count += 1
    print(f'  PASS  {msg}')


def bad(msg):
    global fail_count
    fail_count += 1
    print(f'  FAIL  {msg}')


# --------------------------------------------------------------- proxyDHCP

def dhcp_probe(server, timeout=5.0):
    """Return the decoded OFFER, or None."""
    def opt(code, payload):
        return bytes([code, len(payload)]) + payload

    xid = 0x13572468
    pkt = struct.pack('!BBBBIHH', 1, 1, 6, 0, xid, 0, 0x8000)
    pkt += b'\x00' * 16
    pkt += bytes.fromhex('001122334455') + b'\x00' * 10
    pkt += b'\x00' * 64 + b'\x00' * 128 + MAGIC
    pkt += opt(53, b'\x01')                               # DISCOVER
    pkt += opt(93, struct.pack('!H', 0))                  # arch: BIOS x86
    pkt += opt(97, b'\x00' + uuid.uuid4().bytes)
    pkt += opt(60, b'PXEClient:Arch:00000:UNDI:002001')   # the trigger
    pkt += b'\xff'

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except OSError:
        pass
    s.settimeout(timeout)
    try:
        s.bind(('', 68))
    except PermissionError:
        s.close()
        return 'noroot'
    try:
        s.sendto(pkt, (server, 67))
        data, _ = s.recvfrom(2048)
    except socket.timeout:
        return None
    finally:
        s.close()

    out = {
        'yiaddr': socket.inet_ntoa(data[16:20]),
        'siaddr': socket.inet_ntoa(data[20:24]),
        'file': data[108:236].split(b'\x00')[0].decode(),
        'opts': {},
    }
    i = data.index(MAGIC) + 4
    while i < len(data) and data[i] != 0xFF:
        if data[i] == 0:
            i += 1
            continue
        code, ln = data[i], data[i + 1]
        out['opts'][code] = data[i + 2:i + 2 + ln]
        i += 2 + ln
    return out


# -------------------------------------------------------------------- TFTP

def tftp_get(server, name, blksize=None, timeout=5.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    req = b'\x00\x01' + name.encode() + b'\x00octet\x00'
    if blksize:
        req += b'blksize\x00' + str(blksize).encode() + b'\x00'
    s.sendto(req, (server, 69))
    data, expect, bs = b'', 1, blksize or 512
    try:
        while True:
            pkt, addr = s.recvfrom(bs + 64)
            op = struct.unpack('!H', pkt[:2])[0]
            if op == 5:
                return None, pkt[4:].split(b'\x00')[0].decode()
            if op == 6:
                # Honour the NEGOTIATED size, not the one we asked for. The
                # server clamps to 1428 to stay inside an Ethernet frame, and a
                # client that keeps its own figure reads the first block as a
                # short block and stops one block in - which looks exactly like
                # a truncated file.
                fields = pkt[2:].split(b'\x00')
                for k, v in zip(fields[0::2], fields[1::2]):
                    if k.lower() == b'blksize':
                        bs = int(v)
                s.sendto(struct.pack('!HH', 4, 0), addr)
                continue
            blk = struct.unpack('!H', pkt[2:4])[0]
            if blk == expect:
                data += pkt[4:]
                s.sendto(struct.pack('!HH', 4, blk), addr)
                expect = (expect + 1) & 0xFFFF
                if len(pkt) - 4 < bs:
                    return data, None
    except socket.timeout:
        return (data or None), 'timeout'
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default=None, help='PXE host (default: this host)')
    ap.add_argument('--tftp-only', action='store_true')
    args = ap.parse_args()

    server = args.server
    if not server:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import pxe_server
        server = pxe_server.detect_lan_ip()
    print(f'PXE self-test against {server}\n')

    if not args.tftp_only:
        print('proxyDHCP (what the boot ROM asks first)')
        r = dhcp_probe(server)
        if r == 'noroot':
            print('  SKIP  need root to bind port 68 - re-run with sudo, '
                  'or use --tftp-only')
        elif r is None:
            bad('no OFFER - the ROM would sit at "DHCP." and time out')
        else:
            (ok if r['yiaddr'] == '0.0.0.0' else bad)(
                f'yiaddr is {r["yiaddr"]} (must be 0.0.0.0 - we are a PROXY, '
                f'the router does addressing)')
            (ok if r['siaddr'] == server else bad)(
                f'next-server is {r["siaddr"]}')
            (ok if r['file'] else bad)(f'BOOTP file field is {r["file"]!r}')
            (ok if r['opts'].get(67) else bad)(
                f'option 67 boot file is {r["opts"].get(67, b"")!r}')
            (ok if r['opts'].get(53) == b'\x02' else bad)('message type is OFFER')
            (ok if r['opts'].get(60, b'').startswith(b'PXEClient') else bad)(
                'vendor class is PXEClient')
        print()

    print('TFTP (what it fetches next)')
    for name in BOOT_FILES:
        d, err = tftp_get(server, name)
        if d is None:
            bad(f'{name}: {err}')
        else:
            ok(f'{name}: {len(d)} bytes  md5={hashlib.md5(d).hexdigest()[:12]}')

    print()
    print('TFTP naming quirks (NTLDR asks like this)')
    for name in QUIRK_NAMES:
        d, err = tftp_get(server, name)
        (ok if d else bad)(f'{name!r}: '
                           + (f'{len(d)} bytes' if d else str(err)))

    print()
    print('TFTP blksize negotiation (most ROMs ask for one)')
    plain, _ = tftp_get(server, 'startrom.n12')
    big, err = tftp_get(server, 'startrom.n12', blksize=1456)
    if big and plain and big == plain:
        ok(f'negotiated transfer matches the 512-byte one ({len(big)} bytes)')
    else:
        bad(f'blksize transfer differs: {len(big or b"")} vs {len(plain or b"")} ({err})')

    print(f'\n{ok_count} passed, {fail_count} failed')
    return 1 if fail_count else 0


if __name__ == '__main__':
    sys.exit(main())
