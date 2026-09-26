#!/usr/bin/env python3
"""A second PXE server on the LAN must be FOUND, not remembered.

2026-09-25: a Dell Dimension 4600 was PXE-installed three times and text mode
finished every time. Each time it rebooted, this server correctly HELD it so it
would boot its disk - and a second copy of this very server, still scheduled on
whitebeast (192.168.1.249) from before the 2026-08-24 move, answered instead.
Its winnt.sif pointed at the retired XPSP3-PXE tree, so setupldr's TFTP read of
txtsetup.sif failed: "INF file txtsetup.sif is corrupt or missing, status 21".
The README said "do not run two PXE hosts on one LAN"; nothing checked it. And
the Windows copy could never arm a hold, because mac_for_ip() only read Linux's
/proc/net/arp.

Pinned here:
  * the probe finds a responder, ignores our own replies (siaddr == server_ip)
    and a silent host - including a copy of OUR ProxyDHCP class, i.e. exactly
    the whitebeast case;
  * the sweep is paced and waits long enough (a back-to-back burst was measured
    to miss whitebeast on the real LAN);
  * pxe_server.py probes at startup, keeps re-probing, and a Windows copy yields;
  * the Windows ARP lookup parses `arp -a`.

Run: python3 tests/test_pxe_rogue.py   (run_all.sh runs every test_pxe_*.py)
"""
import os
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PXE = os.path.join(HERE, '..', 'scripts', 'pxe')
sys.path.insert(0, PXE)
import pxe_rogue                                      # noqa: E402
import pxe_server                                     # noqa: E402

fails = []


def check(label, cond):
    print(f'  {"PASS" if cond else "FAIL"}  {label}')
    if not cond:
        fails.append(label)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    print('== probe packet ==')
    pkt = pxe_rogue.build_probe()
    check('it is a BOOTREQUEST carrying PXEClient', pkt[0] == 1 and b'PXEClient' in pkt)
    check('it uses a locally-administered MAC (no real box is touched)',
          pkt[28] & 0x02 == 0x02)
    check('PROBE_MAC_TEXT matches the MAC in the packet',
          ':'.join('%02x' % b for b in pkt[28:34]) == pxe_rogue.PROBE_MAC_TEXT)
    reply = bytearray(pkt)
    reply[0] = 2
    reply[20:24] = socket.inet_aton('192.168.1.249')
    check('a reply is parsed, with its next-server',
          pxe_rogue.parse_reply(bytes(reply)) == (True, '192.168.1.249'))
    reply[4] ^= 0xFF
    check('a reply to someone else\'s xid is ignored', pxe_rogue.parse_reply(bytes(reply))[0] is False)

    print('== our own ProxyDHCP, running as the "other" server ==')
    port = free_port()
    cfg = {'server_ip': '127.0.0.1', 'bootfile': 'startrom.n12', 'bind_device': ''}
    rogue = pxe_server.ProxyDHCP(cfg, port)
    rogue.start()
    time.sleep(0.3)                  # bound on `port`; now take the 4011 reply path
    rogue.port = 4011
    found = pxe_rogue.find_other_pxe_servers('192.168.1.132', hosts=['127.0.0.1'],
                                             port=port, timeout=1.0)
    check('a copy of this server elsewhere is FOUND', found == ['127.0.0.1'])
    found = pxe_rogue.find_other_pxe_servers('127.0.0.1', hosts=['127.0.0.1'],
                                             port=port, timeout=1.0)
    check('our own replies (siaddr == server_ip) are not a rogue', found == [])
    silent = free_port()
    found = pxe_rogue.find_other_pxe_servers('192.168.1.132', hosts=['127.0.0.1'],
                                             port=silent, timeout=0.5)
    check('a host with nothing on the port is not a rogue', found == [])

    print('== the sweep is paced and waits ==')
    import inspect
    sig = inspect.signature(pxe_rogue.find_other_pxe_servers)
    check('default wait >= 4 s (2 s missed whitebeast on the real LAN)',
          sig.parameters['timeout'].default >= 4.0)
    check('sends are paced and repeated',
          sig.parameters['pace'].default > 0 and sig.parameters['rounds'].default >= 2)

    print('== pxe_server.py uses it ==')
    src = open(os.path.join(PXE, 'pxe_server.py')).read()
    main_src = src[src.index('def main():'):]
    check('startup probes for another server before serving',
          main_src.index('find_other_pxe_servers') < main_src.index('ProxyDHCP(cfg, 67)'))
    check('and keeps re-probing (RogueWatch runs with the servers)',
          'RogueWatch(' in main_src[main_src.index('threads = ['):])
    check('a Windows copy yields to a server already answering',
          'FATAL: this copy yields' in main_src)
    check('mac_for_ip falls back to arp -a where /proc/net/arp does not exist',
          'FileNotFoundError' in src and 'mac_for_ip_windows' in src)

    print('== Windows ARP ==')
    arp = ('\nInterface: 192.168.1.249 --- 0x5\n'
           '  Internet Address      Physical Address      Type\n'
           '  192.168.1.194         00-0c-f1-d7-98-4a     dynamic\n'
           '  192.168.1.255         ff-ff-ff-ff-ff-ff     static\n')
    check('the Dell\'s MAC is read from arp -a',
          pxe_rogue.parse_windows_arp(arp, '192.168.1.194') == '00:0c:f1:d7:98:4a')
    check('an address not in the table gives None',
          pxe_rogue.parse_windows_arp(arp, '192.168.1.5') is None)

    print(f'\npxe rogue server: {"all checks passed" if not fails else str(len(fails)) + " FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
