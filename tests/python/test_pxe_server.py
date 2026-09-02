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
        # sub-option 6 = discovery control. 0x0b = no broadcast discovery
        # (0x01) + no multicast discovery (0x02) + "the boot file is in this
        # offer, use it" (0x08). It was 0x07, whose 0x04 bit points the ROM at
        # a PXE_BOOT_SERVERS list we never send - see DiscoveryControlTests.
        self.assertEqual(opts[43], b'\x06\x01\x0b\xff')

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


class DiscoveryControlTests(unittest.TestCase):
    """Option 43 sub-option 6 is a BITFIELD, and the wrong bit stops the boot.

        0x01  no broadcast boot-server discovery
        0x02  no multicast boot-server discovery
        0x04  accept ONLY boot servers listed in PXE_BOOT_SERVERS (option 8)
        0x08  the boot file is in this offer - download it, skip discovery

    It shipped as 0x07 with a comment calling 0x04 "no prompt". It is not. It
    points the ROM at a boot-server list we never send, and the Gateway 440BX
    stopped with "bad or missing server discovery list" - the ROM was looking
    for a list that by construction could not exist.
    """

    def _disco(self):
        proxy = pxe_server.ProxyDHCP(CFG, 67)
        req = pxe_server.Request(discover(), ('0.0.0.0', 68))
        reply = proxy.build_reply(req, pxe_server.DHCP_OFFER, 'startrom.n12')
        vendor = pxe_server.parse_options(reply)[43]
        j = 0
        while j < len(vendor) and vendor[j] != 0xFF:
            sub, slen = vendor[j], vendor[j + 1]
            if sub == 6:
                return vendor[j + 2]
            j += 2 + slen
        return None

    def test_bit3_is_set_so_the_rom_uses_the_offered_boot_file(self):
        d = self._disco()
        self.assertIsNotNone(d, 'no discovery-control sub-option in option 43')
        self.assertTrue(d & 0x08,
                        f'0x{d:02x}: bit 3 unset, so the ROM runs a boot-server '
                        f'discovery cycle instead of just fetching the file')

    def test_bit2_is_not_set_while_we_send_no_boot_server_list(self):
        d = self._disco()
        self.assertFalse(d & 0x04,
                         f'0x{d:02x}: bit 2 tells the ROM to accept only servers '
                         f'from PXE_BOOT_SERVERS, which we never send - that is '
                         f'"bad or missing server discovery list"')

    def test_both_discovery_cycles_are_suppressed(self):
        d = self._disco()
        self.assertTrue(d & 0x01 and d & 0x02, f'0x{d:02x}')


class ProxyMustNotAckOnPort67Tests(unittest.TestCase):
    """Port 67 answers DISCOVER only. Port 4011 is where an ACK belongs.

    A proxyDHCP never assigns an address, so its reply carries yiaddr 0.0.0.0.
    If it ACKs a broadcast DHCPREQUEST on port 67, the client receives TWO acks
    - the real server's with the lease, and ours with no address - and when
    ours is the one it keeps, the machine has no IP. A PXE client with no IP
    cannot open a TFTP session, so it waits, times out, and restarts DISCOVER.

    That is what stalled the Gateway 440BX (Intel Boot Agent, 00:d0:b7:...) on
    2026-08-26, and it is a nasty one to read: the server log shows OFFER,
    OFFER, ACK on a loop and looks like it is working perfectly, while the
    target never fetches a byte.
    """

    @staticmethod
    def _handled(port, msgtype):
        """Mirror the dispatch decision in ProxyDHCP.run()."""
        if msgtype == pxe_server.DHCP_DISCOVER:
            return 'OFFER'
        if msgtype in (pxe_server.DHCP_REQUEST, pxe_server.DHCP_INFORM):
            return 'ACK' if port == 4011 else None
        return None

    def test_discover_is_answered_on_both_ports(self):
        for port in (67, 4011):
            self.assertEqual(self._handled(port, pxe_server.DHCP_DISCOVER), 'OFFER')

    def test_request_is_ignored_on_67_and_acked_on_4011(self):
        for msg in (pxe_server.DHCP_REQUEST, pxe_server.DHCP_INFORM):
            self.assertIsNone(self._handled(67, msg),
                              'an ACK on 67 races the real DHCP server and can '
                              'leave the client with no address')
            self.assertEqual(self._handled(4011, msg), 'ACK')

    def test_the_source_actually_carries_the_port_guard(self):
        # The mirror above is only meaningful if the real dispatch has it.
        src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', 'scripts', 'pxe',
                                'pxe_server.py')).read()
        self.assertIn('if self.port != 4011:', src,
                      'port-67 ACK guard is missing from ProxyDHCP.run()')


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


class TestNeverOffer(unittest.TestCase):
    """The timed hold ALWAYS expires, and these boxes boot from the network
    first - so an expired hold silently reimages a finished machine. That is
    what happened on 2026-09-02. never_offer is the belt that does not expire.
    """

    def _state(self, served_ago):
        import json, tempfile, time
        fd, path = tempfile.mkstemp(suffix='.json')
        with os.fdopen(fd, 'w') as fh:
            json.dump({'aa:bb:cc:dd:ee:ff': time.time() - served_ago}, fh)
        self.addCleanup(os.unlink, path)
        return path

    def test_blocked_mac_stays_held_after_the_timed_hold_expires(self):
        hold = pxe_server.BootHold(self._state(999999), 21600, 0,
                                   ['aa:bb:cc:dd:ee:ff'])
        self.assertTrue(hold.held('aa:bb:cc:dd:ee:ff'),
                        'a blocked MAC must never be offered a boot file')

    def test_block_match_is_case_insensitive(self):
        hold = pxe_server.BootHold(self._state(999999), 21600, 0,
                                   ['aa:bb:cc:dd:ee:ff'])
        self.assertTrue(hold.held('AA:BB:CC:DD:EE:FF'))

    def test_other_macs_are_unaffected_by_the_blocklist(self):
        hold = pxe_server.BootHold(self._state(999999), 21600, 0,
                                   ['aa:bb:cc:dd:ee:ff'])
        self.assertFalse(hold.held('00:11:22:33:44:55'))

    def test_without_a_block_an_expired_hold_releases(self):
        hold = pxe_server.BootHold(self._state(999999), 21600, 0, [])
        self.assertFalse(hold.held('aa:bb:cc:dd:ee:ff'),
                         'this is the hazard never_offer exists to cover')
