#!/usr/bin/env python3
"""
Minimal PXE stack for the retro fleet: proxyDHCP + TFTP, pure stdlib.

Runs natively on Windows (whitebeast, 192.168.1.249) - NOT in WSL, because WSL2
is NAT'd and never sees the fleet's broadcast domain.

proxyDHCP means we do NOT hand out IP addresses: the LAN's existing DHCP server
keeps doing that, and we only answer the PXE-specific part (next-server + boot
file name).  That makes it safe to leave running on a shared network.

Usage:  python pxe_server.py [--config pxe_config.json]
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time

MAGIC_COOKIE = b'\x63\x82\x53\x63'

DHCP_DISCOVER, DHCP_OFFER, DHCP_REQUEST, DHCP_ACK, DHCP_INFORM = 1, 2, 3, 5, 8


def log(msg):
    ts = time.strftime('%H:%M:%S')
    line = f'[{ts}] {msg}'
    print(line, flush=True)
    if LOGFILE:
        try:
            with open(LOGFILE, 'a', encoding='ascii', errors='replace') as fh:
                fh.write(line + '\n')
        except OSError:
            pass


LOGFILE = None


# --------------------------------------------------------------------------
# DHCP packet helpers
# --------------------------------------------------------------------------

def parse_options(data):
    """Return {code: bytes} from the option area of a DHCP packet."""
    opts = {}
    if len(data) < 240 or data[236:240] != MAGIC_COOKIE:
        return opts
    i = 240
    while i < len(data):
        code = data[i]
        if code == 255:
            break
        if code == 0:
            i += 1
            continue
        if i + 1 >= len(data):
            break
        ln = data[i + 1]
        opts[code] = data[i + 2:i + 2 + ln]
        i += 2 + ln
    return opts


def opt(code, payload):
    if isinstance(payload, str):
        payload = payload.encode('ascii')
    return bytes([code, len(payload)]) + payload


class Request:
    def __init__(self, data, addr):
        self.data = data
        self.addr = addr
        self.op = data[0]
        self.xid = data[4:8]
        self.secs = data[8:10]
        self.flags = data[10:12]
        self.ciaddr = data[12:16]
        self.giaddr = data[24:28]
        self.chaddr = data[28:44]
        self.opts = parse_options(data)
        self.msgtype = self.opts.get(53, b'\x00')[0]
        self.vendor = self.opts.get(60, b'')
        self.arch = struct.unpack('!H', self.opts[93])[0] if len(self.opts.get(93, b'')) == 2 else 0

    @property
    def mac(self):
        return ':'.join('%02x' % b for b in self.chaddr[:6])

    @property
    def is_pxe(self):
        return self.vendor.startswith(b'PXEClient')


# --------------------------------------------------------------------------
# serve-once hold
# --------------------------------------------------------------------------

def mac_for_ip(ip):
    """MAC of a same-segment host, from the kernel ARP cache.

    TFTP only ever learns the client's IP, but the hold has to be keyed on
    something stable across the reboot - and the whole point is that the IP may
    change. The client has just DHCPed and talked to us, so it is in the cache.
    """
    try:
        with open('/proc/net/arp', encoding='ascii') as fh:
            next(fh, None)
            for line in fh:
                f = line.split()
                if len(f) >= 4 and f[0] == ip and f[3] != '00:00:00:00:00:00':
                    return f[3].lower()
    except OSError:
        pass
    return None


class BootHold:
    """Serve a machine once, then stand aside until released.

    WHY THIS EXISTS. A proxyDHCP server cannot tell a bare machine from one
    that has just finished installing - both send the same DISCOVER. So if
    network boot stays ahead of the disk in the BIOS order, the machine PXE
    boots again the moment text-mode setup reboots it, setup starts over, and
    with AutoPartition/Repartition in winnt.sif it REPARTITIONS AND REFORMATS
    the disk it was halfway through installing. The install can never finish,
    and nothing in the log looks like an error: it reads as a tidy, identical
    boot sequence every few minutes. Observed on the Gateway 550
    (00:d0:b7:40:96:a9) on 2026-08-27, looping every 5.5 minutes.

    A hold is armed when the machine successfully DOWNLOADS the boot file, not
    when it is offered one. Offers get retried several times within a single
    boot - the Intel Boot Agent here sends three - so arming on the offer would
    block the very boot it was meant to allow. A completed download means the
    ROM is committed and setup is starting.

    Retries inside one boot are unaffected: by the time the download finishes,
    the ROM already has the offer it needs.
    """

    def __init__(self, path, hold_seconds):
        self.path = path
        self.hold = int(hold_seconds or 0)
        self.lock = threading.Lock()
        self.served = {}
        if self.hold:
            self._load()

    def _load(self):
        try:
            with open(self.path, encoding='ascii') as fh:
                data = json.load(fh)
            if isinstance(data, dict):
                self.served = {k.lower(): float(v) for k, v in data.items()}
        except (OSError, ValueError):
            self.served = {}

    def _save(self):
        try:
            tmp = self.path + '.tmp'
            with open(tmp, 'w', encoding='ascii') as fh:
                json.dump(self.served, fh)
            os.replace(tmp, self.path)
        except OSError as exc:
            log(f'boot-hold: cannot write {self.path}: {exc}')

    def held(self, mac):
        """True if this MAC should NOT be offered a boot file right now."""
        if not self.hold or not mac:
            return False
        with self.lock:
            ts = self.served.get(mac.lower())
        return ts is not None and (time.time() - ts) < self.hold

    def arm(self, mac):
        if not self.hold or not mac:
            return
        with self.lock:
            self.served[mac.lower()] = time.time()
            self._save()
        log(f'boot-hold: armed for {mac} - not offering again for {self.hold}s '
            f'(release with --release {mac})')

    def release(self, mac):
        with self.lock:
            if mac == 'all':
                n = len(self.served)
                self.served = {}
            else:
                n = 1 if self.served.pop(mac.lower(), None) is not None else 0
            self._save()
        return n

    def listing(self):
        now = time.time()
        with self.lock:
            return sorted((m, max(0, int(self.hold - (now - t))))
                          for m, t in self.served.items())


# --------------------------------------------------------------------------
# proxyDHCP
# --------------------------------------------------------------------------

class ProxyDHCP(threading.Thread):
    daemon = True

    def __init__(self, cfg, port):
        super().__init__(name=f'proxydhcp:{port}')
        self.cfg = cfg
        self.port = port

    def bootfile(self, req):
        """Pick the NBP for the client's architecture (option 93)."""
        arches = self.cfg.get('bootfile_by_arch', {})
        return arches.get(str(req.arch)) or self.cfg['bootfile']

    def build_reply(self, req, msgtype, bootfile):
        server_ip = socket.inet_aton(self.cfg['server_ip'])
        pkt = bytearray(240)
        pkt[0] = 2                      # BOOTREPLY
        pkt[1] = 1                      # ethernet
        pkt[2] = 6
        pkt[3] = 0
        pkt[4:8] = req.xid
        pkt[8:10] = b'\x00\x00'
        pkt[10:12] = req.flags
        pkt[12:16] = req.ciaddr
        pkt[16:20] = b'\x00\x00\x00\x00'   # yiaddr: proxyDHCP never assigns
        pkt[20:24] = server_ip             # siaddr = TFTP server
        pkt[24:28] = req.giaddr
        pkt[28:44] = req.chaddr
        sname = self.cfg['server_ip'].encode('ascii')[:63]
        pkt[44:44 + len(sname)] = sname
        bf = bootfile.encode('ascii')[:127]
        pkt[108:108 + len(bf)] = bf
        pkt[236:240] = MAGIC_COOKIE

        # PXE vendor options - option 6 is PXE_DISCOVERY_CONTROL, a BITFIELD:
        #   0x01  do not do broadcast boot-server discovery
        #   0x02  do not do multicast boot-server discovery
        #   0x04  accept ONLY boot servers named in PXE_BOOT_SERVERS (option 8)
        #   0x08  the boot file is in this offer - download it, skip discovery
        #
        # This used to send 0x07, described in the comment as "no prompt". That
        # is not what 0x04 means. 0x04 tells the ROM to accept only servers
        # from a list we never send, so the Gateway 440BX (Intel Boot Agent)
        # went looking for that list, found nothing, and stopped with
        # "bad or missing server discovery list" - 2026-08-26.
        #
        # 0x0B is what was meant all along: suppress both discovery cycles AND
        # set bit 3, which is the bit that actually says "just use the boot
        # file from this offer". No list needed, because no discovery happens.
        pxe = opt(6, b'\x0b') + b'\xff'

        options = b''
        options += opt(53, bytes([msgtype]))
        options += opt(54, server_ip)
        options += opt(60, b'PXEClient')
        if 97 in req.opts:
            options += opt(97, req.opts[97])
        options += opt(43, pxe)
        options += opt(66, self.cfg['server_ip'])
        options += opt(67, bootfile + '\x00')
        options += b'\xff'
        return bytes(pkt) + options

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(('', self.port))
        log(f'proxyDHCP listening on UDP {self.port}')
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except OSError as exc:
                log(f'proxyDHCP recv error: {exc}')
                continue
            if len(data) < 240 or data[0] != 1:
                continue
            try:
                req = Request(data, addr)
            except Exception as exc:            # malformed packet
                log(f'proxyDHCP parse error from {addr}: {exc}')
                continue
            if not req.is_pxe:
                continue
            if req.msgtype == DHCP_DISCOVER:
                reply_type = DHCP_OFFER
            elif req.msgtype in (DHCP_REQUEST, DHCP_INFORM):
                # ONLY on 4011. Answering a DHCPREQUEST on port 67 is what
                # stalled the Gateway 440BX (Intel Boot Agent, 00:d0:b7:...)
                # on 2026-08-26: the client broadcasts its REQUEST and gets
                # TWO acks - the router's, carrying the real lease, and ours,
                # carrying yiaddr 0.0.0.0 because a proxy never assigns. When
                # ours is the one it keeps, the machine has NO IP, and a PXE
                # client with no address cannot open a TFTP session. It waits,
                # times out, and starts the whole DISCOVER again - which in the
                # log looks like OFFER, OFFER, ACK, silence, retry, forever,
                # with the boot side apparently working perfectly.
                #
                # The spec puts this exchange on 4011 for exactly this reason:
                # there it is unicast to the client, cannot race the real DHCP
                # server, and cannot be mistaken for the address assignment.
                if self.port != 4011:
                    continue
                reply_type = DHCP_ACK
            else:
                continue
            hold = self.cfg.get('_hold')
            if hold is not None and hold.held(req.mac):
                # Already installed from us once. Staying quiet is what lets the
                # machine fall through to its own disk instead of reinstalling.
                log(f'proxyDHCP HOLD -> {req.mac} (already served; '
                    f'--release {req.mac} to reinstall)')
                continue
            bootfile = self.bootfile(req)
            reply = self.build_reply(req, reply_type, bootfile)
            dest = self.dest_for(req, addr)
            try:
                sock.sendto(reply, dest)
                log(f'proxyDHCP {"OFFER" if reply_type == DHCP_OFFER else "ACK"} '
                    f'-> {req.mac} arch={req.arch} via {dest[0]}:{dest[1]} file={bootfile}')
            except OSError as exc:
                log(f'proxyDHCP send error to {dest}: {exc}')

    def dest_for(self, req, addr):
        if self.port == 4011:                      # unicast follow-up request
            return addr
        if req.giaddr != b'\x00\x00\x00\x00':      # relayed
            return (socket.inet_ntoa(req.giaddr), 67)
        if req.ciaddr != b'\x00\x00\x00\x00':
            return (socket.inet_ntoa(req.ciaddr), 68)
        return ('255.255.255.255', 68)


# --------------------------------------------------------------------------
# TFTP (RFC 1350 + RFC 2347/2348/2349 options)
# --------------------------------------------------------------------------

OP_RRQ, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR, OP_OACK = 1, 2, 3, 4, 5, 6


class TFTPServer(threading.Thread):
    daemon = True

    def __init__(self, cfg):
        super().__init__(name='tftp')
        self.root = os.path.abspath(cfg['tftp_root'])
        self.cfg = cfg

    def resolve(self, name):
        """Map a TFTP filename onto a real path, case-insensitively.

        PXE ROMs and NTLDR send backslashes and mixed case; Windows is
        case-insensitive anyway, but keep this portable and reject escapes.
        """
        name = name.replace('\\', '/').lstrip('/')
        if not name:
            return None
        parts = []
        for part in name.split('/'):
            if part in ('', '.'):
                continue
            if part == '..':
                return None
            parts.append(part)
        path = os.path.join(self.root, *parts)
        if os.path.isfile(path):
            return path
        # case-insensitive walk
        cur = self.root
        for part in parts:
            try:
                entries = os.listdir(cur)
            except OSError:
                return None
            match = next((e for e in entries if e.lower() == part.lower()), None)
            if match is None:
                return None
            cur = os.path.join(cur, match)
        return cur if os.path.isfile(cur) else None

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('', 69))
        log(f'TFTP listening on UDP 69, root={self.root}')
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except OSError as exc:
                log(f'tftp recv error: {exc}')
                continue
            if len(data) < 4 or struct.unpack('!H', data[:2])[0] != OP_RRQ:
                continue
            threading.Thread(target=self.serve, args=(data, addr), daemon=True).start()

    def serve(self, data, addr):
        fields = data[2:].split(b'\x00')
        filename = fields[0].decode('latin-1')
        options = {}
        rest = [f.decode('latin-1') for f in fields[2:] if f != b'']
        for i in range(0, len(rest) - 1, 2):
            options[rest[i].lower()] = rest[i + 1]

        path = self.resolve(filename)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(5)
        if path is None:
            log(f'tftp {addr[0]} MISS {filename}')
            sock.sendto(struct.pack('!HH', OP_ERROR, 1) + b'File not found\x00', addr)
            sock.close()
            return
        size = os.path.getsize(path)
        log(f'tftp {addr[0]} GET {filename} ({size} bytes) opts={options or "-"}')

        blksize = 512
        oack = {}
        if 'blksize' in options:
            blksize = max(8, min(int(options['blksize']), 1428))
            oack['blksize'] = str(blksize)
        if 'tsize' in options:
            oack['tsize'] = str(size)
        if 'timeout' in options:
            oack['timeout'] = options['timeout']

        try:
            with open(path, 'rb') as fh:
                if oack:
                    payload = b''
                    for k, v in oack.items():
                        payload += k.encode() + b'\x00' + v.encode() + b'\x00'
                    if not self.send_and_wait(sock, struct.pack('!H', OP_OACK) + payload, addr, 0):
                        log(f'tftp {addr[0]} no ACK for OACK, aborting {filename}')
                        return
                block = 1
                while True:
                    chunk = fh.read(blksize)
                    pkt = struct.pack('!HH', OP_DATA, block & 0xFFFF) + chunk
                    if not self.send_and_wait(sock, pkt, addr, block & 0xFFFF):
                        log(f'tftp {addr[0]} timeout on block {block} of {filename}')
                        return
                    if len(chunk) < blksize:
                        break
                    block += 1
            log(f'tftp {addr[0]} DONE {filename}')
            # Arm only on the boot file itself. Setup fetches a dozen other
            # files over TFTP; any of them would be the wrong trigger.
            hold = self.cfg.get('_hold')
            if hold is not None and os.path.basename(
                    filename.replace('\\', '/')).lower() == str(
                    self.cfg.get('bootfile', '')).lower():
                hold.arm(mac_for_ip(addr[0]))
        except OSError as exc:
            log(f'tftp {addr[0]} error on {filename}: {exc}')
        finally:
            sock.close()

    @staticmethod
    def send_and_wait(sock, packet, addr, expect_block, retries=5):
        for _ in range(retries):
            sock.sendto(packet, addr)
            try:
                while True:
                    data, raddr = sock.recvfrom(1024)
                    if raddr[0] != addr[0]:
                        continue
                    if len(data) >= 4:
                        op, blk = struct.unpack('!HH', data[:4])
                        if op == OP_ERROR:
                            return False
                        if op == OP_ACK and blk == expect_block:
                            return True
            except socket.timeout:
                continue
        return False


# --------------------------------------------------------------------------

def detect_lan_ip():
    """This host's address on the route out to the fleet.

    Auto-detected rather than configured, because a hardcoded fleet IP is a
    thing that has already gone stale here more than once - and getting it
    wrong is silent: the PXE client is handed a next-server it cannot reach and
    just sits at "TFTP." until it times out. No traffic is sent; connect() on a
    UDP socket only picks the route.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('192.168.1.1', 9))
        return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'
    finally:
        s.close()


if os.name == 'nt':
    _ROOT = r'C:\development\pxe'
else:
    # /srv is where a served payload belongs on a Linux host, and it keeps the
    # TFTP root off the repo tree - the XP boot files are licensed content and
    # are deliberately not in git.
    _ROOT = '/srv/retro-pxe'

DEFAULT_CONFIG = {
    # 'auto' resolves through detect_lan_ip() at startup. An explicit address
    # still wins, for a host with several interfaces on the fleet LAN.
    'server_ip': 'auto',
    'tftp_root': os.path.join(_ROOT, 'tftp'),
    'bootfile': 'startrom.n12',
    'bootfile_by_arch': {},
    'logfile': os.path.join(_ROOT, 'pxe_server.log'),
    # Once a machine has taken the boot file, stand aside for this long so
    # it can boot the disk it just installed. 0 disables the hold and
    # restores the old always-offer behaviour. See BootHold.
    'boot_hold_seconds': 3600,
    'state_file': os.path.join(_ROOT, 'pxe_state.json'),
}


def main():
    global LOGFILE
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                     'pxe_config.json'))
    ap.add_argument('--release', metavar='MAC',
                    help="drop the serve-once hold on a MAC (or 'all') so it "
                         "can install again, then exit")
    ap.add_argument('--list-holds', action='store_true',
                    help='show which machines are being held, and for how long')
    args = ap.parse_args()

    cfg = dict(DEFAULT_CONFIG)
    if os.path.isfile(args.config):
        with open(args.config, encoding='ascii') as fh:
            cfg.update(json.load(fh))
    LOGFILE = cfg.get('logfile')
    hold = BootHold(cfg.get('state_file'), cfg.get('boot_hold_seconds', 0))
    cfg['_hold'] = hold

    if args.release:
        n = hold.release(args.release)
        print(f'released {n} hold(s)')
        return 0
    if args.list_holds:
        rows = hold.listing()
        if not rows:
            print('no machines held')
        for mac, left in rows:
            print(f'{mac}  {left}s remaining' if left else f'{mac}  expired')
        return 0

    if str(cfg.get('server_ip', '')).lower() in ('', 'auto'):
        cfg['server_ip'] = detect_lan_ip()

    log('=' * 60)
    log(f'PXE server starting: proxyDHCP + TFTP on {cfg["server_ip"]}')
    log(f'boot file: {cfg["bootfile"]}   tftp root: {cfg["tftp_root"]}')
    if not os.path.isdir(cfg['tftp_root']):
        log(f'FATAL: tftp root {cfg["tftp_root"]} does not exist')
        return 1

    threads = [TFTPServer(cfg), ProxyDHCP(cfg, 67), ProxyDHCP(cfg, 4011)]
    for t in threads:
        t.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        log('shutting down')
    return 0


if __name__ == '__main__':
    sys.exit(main())
