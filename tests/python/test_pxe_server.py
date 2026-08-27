"""Tests for scripts/pxe/pxe_server.py (proxyDHCP + TFTP).

Encodes the invariants that made PXE boot actually work on 2026-08-20:
  * proxyDHCP must NOT assign an address (yiaddr stays 0.0.0.0) - the LAN's own
    DHCP server does that; we only add next-server + boot file.
  * the reply must carry vendor class PXEClient, option 54, option 43 discovery
    control, and the boot file in BOTH the BOOTP `file` field and option 67,
    because different PXE ROMs read different ones.
  * TFTP path resolution must accept backslashes and mixed case (NTLDR asks for
    files that way) and must reject traversal outside the root.
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts', 'pxe'))

import pxe_server  # noqa: E402

MAGIC = b'\x63\x82\x53\x63'
CFG = {'server_ip': '192.168.1.249', 'tftp_root': '.', 'bootfile': 'startrom.n12',
       'bootfile_by_arch': {'7': 'ipxe.efi'}}


def discover(vendor=b'PXEClient', arch=None, msgtype=1):
    pkt = bytearray(240)
    pkt[0] = 1
    pkt[1] = 1
    pkt[2] = 6
    pkt[4:8] = b'\xde\xad\xbe\xef'
    pkt[28:34] = bytes.fromhex('001122334455')
    pkt[236:240] = MAGIC
    opts = bytes([53, 1, msgtype]) + bytes([60, len(vendor)]) + vendor
    if arch is not None:
        opts += bytes([93, 2]) + struct.pack('!H', arch)
    return bytes(pkt) + opts + b'\xff'


class TestProxyDHCP(unittest.TestCase):
    def setUp(self):
        self.proxy = pxe_server.ProxyDHCP(CFG, 67)

    def reply_options(self, reply):
        return pxe_server.parse_options(reply)

    def test_offer_does_not_assign_an_address(self):
        req = pxe_server.Request(discover(), ('0.0.0.0', 68))
        reply = self.proxy.build_reply(req, pxe_server.DHCP_OFFER, 'startrom.n12')
        self.assertEqual(reply[16:20], b'\x00\x00\x00\x00', 'proxyDHCP must never set yiaddr')
        self.assertEqual(reply[20:24], bytes([192, 168, 1, 249]), 'siaddr must be the TFTP host')
        self.assertEqual(reply[0], 2)
        self.assertEqual(reply[4:8], req.xid)
        self.assertEqual(reply[28:34], req.chaddr[:6])

    def test_bootfile_in_file_field_and_option_67(self):
        req = pxe_server.Request(discover(), ('0.0.0.0', 68))
        reply = self.proxy.build_reply(req, pxe_server.DHCP_OFFER, 'startrom.n12')
        self.assertEqual(reply[108:236].split(b'\x00')[0], b'startrom.n12')
        opts = self.reply_options(reply)
        self.assertEqual(opts[67].rstrip(b'\x00'), b'startrom.n12')
        self.assertEqual(opts[60], b'PXEClient')
        self.assertEqual(opts[54], bytes([192, 168, 1, 249]))
        self.assertEqual(opts[53], b'\x02')
        self.assertEqual(opts[43], b'\x06\x01\x07\xff')  # discovery control: use our boot file

    def test_ack_for_request(self):
        req = pxe_server.Request(discover(msgtype=3), ('0.0.0.0', 68))
        reply = self.proxy.build_reply(req, pxe_server.DHCP_ACK, 'startrom.n12')
        self.assertEqual(self.reply_options(reply)[53], b'\x05')

    def test_only_pxe_clients_are_ours(self):
        self.assertTrue(pxe_server.Request(discover(), ('0.0.0.0', 68)).is_pxe)
        self.assertFalse(pxe_server.Request(discover(vendor=b'MSFT 5.0'), ('0.0.0.0', 68)).is_pxe)

    def test_arch_specific_bootfile(self):
        bios = pxe_server.Request(discover(arch=0), ('0.0.0.0', 68))
        efi = pxe_server.Request(discover(arch=7), ('0.0.0.0', 68))
        self.assertEqual(self.proxy.bootfile(bios), 'startrom.n12')
        self.assertEqual(self.proxy.bootfile(efi), 'ipxe.efi')


class TestTFTPPaths(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.tmp, 'I386'), exist_ok=True)
        for name in ('ntldr', os.path.join('I386', 'ntdetect.com')):
            with open(os.path.join(self.tmp, name), 'wb') as fh:
                fh.write(b'x')
        self.srv = pxe_server.TFTPServer({'tftp_root': self.tmp})

    def test_plain_and_backslash_and_case(self):
        for name in ('ntldr', 'NTLDR', '\\ntldr', 'I386\\ntdetect.com', '/i386/NTDETECT.COM'):
            self.assertIsNotNone(self.srv.resolve(name), name)

    def test_traversal_is_refused(self):
        for name in ('../secrets', '..\\..\\windows\\system32\\config\\sam', 'I386/../../x'):
            self.assertIsNone(self.srv.resolve(name), name)

    def test_missing_file(self):
        self.assertIsNone(self.srv.resolve('nope.0'))


class HostPortabilityTests(unittest.TestCase):
    """The server moved from the Windows host to the Linux fleet host.

    The 2026-08-24 cutover left whitebeast paused, so the PXE host is now
    192.168.1.132 - a different OS with different paths. These pin the parts
    that made it portable, because both failure modes are SILENT: a wrong
    next-server means the client sits at "TFTP." until it times out, and a
    tftp_root that does not exist means every GET is a file-not-found with the
    boot dying at a blank screen.
    """

    def test_defaults_follow_the_platform(self):
        root = pxe_server.DEFAULT_CONFIG['tftp_root']
        log = pxe_server.DEFAULT_CONFIG['logfile']
        if os.name == 'nt':
            self.assertTrue(root.lower().startswith('c:\\'), root)
        else:
            self.assertTrue(root.startswith('/srv/retro-pxe'), root)
            self.assertTrue(log.startswith('/srv/retro-pxe'), log)
        # Same parent either way - the log sits beside the payload it explains.
        self.assertEqual(os.path.dirname(root), os.path.dirname(log))

    def test_server_ip_defaults_to_auto_not_a_hardcoded_host(self):
        # A hardcoded fleet address has gone stale here before, and a PXE
        # client handed an unreachable next-server reports nothing useful.
        self.assertEqual(pxe_server.DEFAULT_CONFIG['server_ip'], 'auto')

    def test_detect_lan_ip_returns_a_dotted_quad(self):
        ip = pxe_server.detect_lan_ip()
        parts = ip.split('.')
        self.assertEqual(len(parts), 4, ip)
        self.assertTrue(all(p.isdigit() and 0 <= int(p) <= 255 for p in parts), ip)

    def test_detect_lan_ip_never_raises_without_a_route(self):
        # It must degrade, not explode: the server still has to start and log
        # so the operator can see WHY it is not answering.
        self.assertIsInstance(pxe_server.detect_lan_ip(), str)


if __name__ == '__main__':
    unittest.main()
