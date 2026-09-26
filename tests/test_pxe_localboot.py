#!/usr/bin/env python3
"""A held machine listed in localboot_macs is TOLD to boot its disk, not ignored.

2026-09-26: with the stale second PXE server on whitebeast stopped, a Dell
Dimension 4600 whose XP text mode had finished was held by this server - and
sat on "DHCP" indefinitely. Its Intel Boot Agent cycled DHCP DISCOVERs and
never fell through to the disk the way other ROMs do when a held machine gets
silence. (While whitebeast answered, that answer had masked it.)

The PXE spec's explicit answer: option 43 with a boot menu (sub-option 9)
whose first item is type 0 = boot from local disk, and a menu prompt
(sub-option 10) with timeout 0 = select it immediately. No boot file.

Opt-in per MAC, because every held reboot on the fleet passes through this
path and silence is proven on the other ROMs. Pinned here:
  * the local-boot reply's structure (menu type 0, prompt timeout 0,
    discovery control 0x03, no boot file anywhere);
  * a held MAC in localboot_macs gets that reply from the running server;
  * a held MAC NOT in the list still gets silence;
  * an unheld MAC still gets the normal boot-file offer.

Run: python3 tests/test_pxe_localboot.py   (run_all.sh runs every test_pxe_*.py)
"""
import json
import os
import socket
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PXE = os.path.join(HERE, '..', 'scripts', 'pxe')
sys.path.insert(0, PXE)
import pxe_rogue                                      # noqa: E402
import pxe_server                                     # noqa: E402

fails = []
MAGIC = b'\x63\x82\x53\x63'


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def options(pkt):
    out, i = {}, pkt.index(MAGIC, 236) + 4
    while i < len(pkt) and pkt[i] != 0xFF:
        if pkt[i] == 0:
            i += 1
            continue
        out[pkt[i]] = pkt[i + 2:i + 2 + pkt[i + 1]]
        i += 2 + pkt[i + 1]
    return out


def suboptions(blob):
    out, i = {}, 0
    while i < len(blob) and blob[i] != 0xFF:
        out[blob[i]] = blob[i + 2:i + 2 + blob[i + 1]]
        i += 2 + blob[i + 1]
    return out


def request_for(mac_hex):
    pkt = bytearray(pxe_rogue.build_probe(mac=bytes.fromhex(mac_hex)))
    return pxe_server.Request(bytes(pkt), ('127.0.0.1', 12345))


def ask(port, mac_hex, timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(pxe_rogue.build_probe(mac=bytes.fromhex(mac_hex)), ('127.0.0.1', port))
    try:
        return s.recvfrom(2048)[0]
    except socket.timeout:
        return None
    finally:
        s.close()


def main():
    cfg = {'server_ip': '127.0.0.1', 'bootfile': 'startrom.n12', 'bind_device': '',
           'localboot_macs': ['02:00:5E:AA:00:01']}

    print('== the local-boot reply ==')
    srv = pxe_server.ProxyDHCP(cfg, 0)
    reply = srv.build_localboot_reply(request_for('02005eaa0001'), pxe_server.DHCP_ACK)
    o = options(reply)
    check('it is a BOOTREPLY with vendor class PXEClient',
          reply[0] == 2 and o.get(60) == b'PXEClient')
    sub = suboptions(o.get(43, b''))
    menu = sub.get(9, b'')
    check('boot menu present, first item type 0 (boot from local disk)',
          len(menu) >= 3 and struct.unpack('!H', menu[:2])[0] == 0)
    check('menu prompt timeout 0 (select it immediately, show nothing)',
          sub.get(10, b'\xff')[:1] == b'\x00')
    check('discovery control 0x03 (no server discovery)', sub.get(6) == b'\x03')
    check('no boot file: option 67 absent and the file field empty',
          67 not in o and reply[108:236].strip(b'\x00') == b'')
    check('localboot_macs is normalised to lower case',
          '02:00:5e:aa:00:01' in srv.localboot_macs)

    print('== the running server ==')
    with tempfile.TemporaryDirectory() as td:
        state = os.path.join(td, 'state.json')
        hold = pxe_server.BootHold(state, 3600, 0)   # production: retry_grace_seconds 0
        for mac in ('02:00:5e:aa:00:01', '02:00:5e:aa:00:02'):
            hold.arm(mac)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
        s.close()
        run_cfg = dict(cfg, _hold=hold)
        srv = pxe_server.ProxyDHCP(run_cfg, port)
        srv.start()
        time.sleep(0.3)
        srv.port = 4011                 # take the unicast 4011 reply path
        r = ask(port, '02005eaa0001')
        check('a held MAC in localboot_macs gets the local-boot answer',
              r is not None and 9 in suboptions(options(r).get(43, b'')))
        r = ask(port, '02005eaa0002', timeout=0.6)
        check('a held MAC not in the list still gets silence', r is None)
        r = ask(port, '02005eaa0003')
        check('an unheld MAC still gets the boot file',
              r is not None and options(r).get(67, b'').rstrip(b'\x00') == b'startrom.n12')

    print('== the shipped config ==')
    with open(os.path.join(PXE, 'pxe_config.json'), encoding='ascii') as fh:
        shipped = json.load(fh)
    macs = shipped.get('localboot_macs', [])
    check('every localboot_macs entry is a colon-separated MAC',
          all(len(m.split(':')) == 6 and all(len(x) == 2 for x in m.split(':'))
              for m in macs))
    check('the Dell Dimension 4600 (Intel Boot Agent) is listed',
          '00:0c:f1:d7:98:4a' in [m.lower() for m in macs])

    print(f'\npxe localboot: {"all checks passed" if not fails else str(len(fails)) + " FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
