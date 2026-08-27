#!/usr/bin/env python3
"""The PXE server must be able to pin its sockets to one interface.

WHY THIS MATTERS. The proxyDHCP OFFER is a broadcast to 255.255.255.255, and
for a socket bound to INADDR_ANY the kernel chooses the egress interface from
the route table - which means the default route, regardless of which link the
DISCOVER arrived on. On a host with more than one link on the fleet LAN, every
client on the non-default link silently never receives an offer. The boot side
looks perfect in the log (the OFFER is recorded as sent) and the machine just
sits there, which is the most expensive kind of failure to diagnose.

That is exactly what happened testing against a VM: a macvtap guest cannot
reach its parent interface's own address, so the server had to answer from a
macvlan sibling instead, and answering required pinning the socket to it.

The guard has to FAIL OPEN. SO_BINDTODEVICE needs CAP_NET_RAW; a server that
refused to start when it could not pin would be strictly worse than one
answering on every interface, so a failure is logged and tolerated.
"""
import importlib.util
import os
import socket
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, os.pardir, 'scripts', 'pxe', 'pxe_server.py')

spec = importlib.util.spec_from_file_location('pxe_server_bd', SERVER)
pxe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pxe)
pxe.LOGFILE = None

FAILS = []


def check(name, cond):
    print(('  PASS  ' if cond else '  FAIL  ') + name)
    if not cond:
        FAILS.append(name)


def main():
    print('== the option exists and defaults to "all interfaces" ==')
    check("DEFAULT_CONFIG carries a bind_device key",
          'bind_device' in pxe.DEFAULT_CONFIG)
    check("it defaults to empty, i.e. unchanged behaviour",
          pxe.DEFAULT_CONFIG.get('bind_device') == '')

    print('== an empty device is a no-op, not an error ==')
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        check('bind_to_device(sock, "") returns True',
              pxe.bind_to_device(s, '') is True)
        check('bind_to_device(sock, None) returns True',
              pxe.bind_to_device(s, None) is True)
    finally:
        s.close()

    print('== a bad device is tolerated, never fatal ==')
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        raised = False
        result = None
        try:
            result = pxe.bind_to_device(s, 'no-such-iface-xyzzy')
        except Exception:
            raised = True
        check('an unknown interface does not raise', not raised)
        check('an unknown interface reports failure rather than success',
              result is False)
        # The socket must still be usable - the point of failing open.
        usable = True
        try:
            s.bind(('', 0))
        except OSError:
            usable = False
        check('the socket is still bindable after a failed pin', usable)
    finally:
        s.close()

    print('== the server actually applies it to both listeners ==')
    src = open(SERVER, encoding='utf-8').read()
    # Strip comments so a mention inside an explanatory comment cannot satisfy
    # the assertion - a check that can be satisfied by its own documentation
    # has caught nobody.
    code = '\n'.join(l.split('#', 1)[0] for l in src.splitlines())
    check('the DHCP socket is pinned before bind',
          code.count('bind_to_device(sock') >= 2)
    check('pinning happens before bind, not after',
          code.index('bind_to_device(sock') < code.index("sock.bind(('', self.port))"))

    print()
    if FAILS:
        print(f'{len(FAILS)} FAILED: ' + ', '.join(FAILS))
        return 1
    print('pxe bind_device: all checks passed')
    return 0


def test_pxe_bind_device():
    """pytest entry point - without it the file is collected and reports zero
    tests, which reads exactly like passing."""
    assert main() == 0, 'bind_device assertions failed: ' + ', '.join(FAILS)


if __name__ == '__main__':
    sys.exit(main())
