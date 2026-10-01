"""carmageddon2-server - the Carmageddon 2 LAN host on 192.168.1.132.

Carmageddon 2 has no dedicated server, so the "server" is the game itself
(Carma2_SW.exe under Wine in docker) driven into a hosting lobby, tunnelled by
the staged tree's own IPXWrapper over UDP 54792. These tests pin the parts
that fail SILENTLY:

  * the probe speaks the game's real join handshake (read out of the binary),
    and counts only a reply from THIS host - not any box that happens to host;
  * the bind shim pins exactly IPXWrapper's port to the wired NIC, and nothing
    else (without it, broadcasts left over Wi-Fi from 192.168.1.129);
  * the IPXWrapper registry the run script writes disables the "wildcard"
    interface (Wi-Fi, docker bridges, tailscale) and makes the wired NIC primary;
  * the frame classifier ignores the lobby title, which BLINKS.

See scripts/game-servers/carmageddon2/README.md.
"""
import json
import os
import socket
import struct
import subprocess
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRV = os.path.join(REPO, 'scripts', 'game-servers')
C2 = os.path.join(SRV, 'carmageddon2')
sys.path.insert(0, SRV)
sys.path.insert(0, C2)

import gameservers  # noqa: E402

LIB_EXE = '/mnt/retro-share/Files/Games-Library/Carmageddon2/Carma2_SW.exe'


# ---------------------------------------------------------------- the probe

def test_query_packet_is_an_ipxwrapper_frame_carrying_car2msg1():
    pkt = gameservers.carma2_query_packet(node=b'\x02\x00\x00\x00\x00\x07')
    (ptype, dnet, dnode, dsock, snet, snode, ssock, size) = struct.unpack(
        '>B4s6sH4s6sHH', pkt[:27])
    assert ptype == 0
    assert dnode == b'\xff' * 6, 'discovery is an IPX broadcast'
    assert dnet == snet == b'\0\0\0\1', 'IPXWrapper interfaces default to net 1'
    assert dsock == ssock == 0x2FFE == 12286, 'Carma2 binds IPX socket 12286'
    assert size == len(pkt) - 27, 'IPXWrapper drops a frame whose size field lies'
    assert pkt[27:] == b'XXXXCAR2MSG1\x00'


@pytest.mark.skipif(not os.path.exists(LIB_EXE),
                    reason='SKIPPED LOUDLY: the share is not mounted, so the probe '
                           'string cannot be checked against the real binary')
def test_probe_strings_are_the_ones_the_binary_builds():
    """The handshake was read out of Carma2_SW.exe (sprintf "XXXX%s%0.1d"
    with "CAR2MSG", 1 to ask, 2 to answer). If a re-staged build changed
    them the probe would say "down" about a healthy host."""
    data = open(LIB_EXE, 'rb').read()
    assert b'XXXX%s%0.1d\x00' in data
    assert b'CAR2MSG\x00' in data


class _FakeSock:
    """Replays canned (data, addr) datagrams; records what was sent."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.sent = []
        self.opts = {}

    def settimeout(self, t):
        pass

    def setsockopt(self, level, opt, val):
        self.opts[opt] = val

    def bind(self, addr):
        pass

    def sendto(self, data, addr):
        self.sent.append((data, addr))

    def recvfrom(self, n):
        if not self.replies:
            raise socket.timeout()
        return self.replies.pop(0)

    def close(self):
        pass


def _reply(msg=b'XXXXCAR2MSG2\x00'):
    return struct.pack('>B4s6sH4s6sHH', 0, b'\0\0\0\1', b'\2\0\0\0\0\1', 0x2FFE,
                       b'\0\0\0\1', b'\xd0\xad\x08\xdc\x0e\x92', 0x2FFE, len(msg)) + msg


def _probe(monkeypatch, tmp_path, replies, state=None):
    fake = _FakeSock(replies)
    monkeypatch.setattr(gameservers.socket, 'socket', lambda *a, **k: fake)
    monkeypatch.setattr(gameservers, '_iface_of',
                        lambda ip: ('enp129s0', '192.168.1.255'))
    monkeypatch.setattr(gameservers, 'HOST', '192.168.1.132')
    monkeypatch.setattr(gameservers, 'C2_HOST_IP', '192.168.1.132')
    sf = tmp_path / 'state.json'
    if state is not None:
        sf.write_text(json.dumps(state))
    monkeypatch.setattr(gameservers, 'C2_STATE', str(sf))
    return gameservers.probe_carma2(54792, timeout=0.5), fake


def test_probe_broadcasts_out_of_the_hosts_own_nic(monkeypatch, tmp_path):
    res, fake = _probe(monkeypatch, tmp_path, [(_reply(), ('192.168.1.132', 54792))])
    assert res and res['name'] == 'Carmageddon 2 LAN host'
    (data, addr), = fake.sent
    assert addr == ('192.168.1.255', 54792), 'a unicast to ourselves lands on lo'
    assert fake.opts[socket.SO_BINDTODEVICE] == b'enp129s0\0'
    assert fake.opts[socket.SO_BROADCAST] == 1


def test_a_reply_from_another_box_is_not_this_server(monkeypatch, tmp_path):
    """A fleet box hosting its own Carmageddon 2 game answers the same
    broadcast. Counting it would report our server up while it is dead."""
    res, _ = _probe(monkeypatch, tmp_path, [(_reply(), ('192.168.1.123', 54792))])
    assert res is None


def test_anything_but_a_host_reply_is_not_hosting(monkeypatch, tmp_path):
    """Our own MSG1 can come back to us; it is not an answer."""
    res, _ = _probe(monkeypatch, tmp_path,
                    [(_reply(b'XXXXCAR2MSG1\x00'), ('192.168.1.132', 54792))])
    assert res is None


def test_lobby_count_excludes_the_hosts_own_car(monkeypatch, tmp_path):
    import time
    res, _ = _probe(monkeypatch, tmp_path, [(_reply(), ('192.168.1.132', 54792))],
                    state={'state': 'lobby', 'players': 3, 'updated': int(time.time())})
    assert res['players'] == 2 and res['map'] == 'lobby'


def test_a_stale_state_file_is_not_reported(monkeypatch, tmp_path):
    res, _ = _probe(monkeypatch, tmp_path, [(_reply(), ('192.168.1.132', 54792))],
                    state={'state': 'lobby', 'players': 3, 'updated': 1})
    assert 'players' not in res and res['map'] is None


def test_the_watchdogs_loopback_host_still_probes_the_lan_nic(monkeypatch, tmp_path):
    """retro-gameservers-watch runs with RETRO_GAMESERVER_HOST=127.0.0.1. The
    first version broadcast from HOST's NIC (lo - none) and wanted the reply
    from 127.0.0.1, so a healthy lobby read as mute and was restarted."""
    seen = {}
    def iface(ip):
        seen['ip'] = ip
        return ('enp129s0', '192.168.1.255')
    fake = _FakeSock([(_reply(), ('192.168.1.132', 54792))])
    monkeypatch.setattr(gameservers.socket, 'socket', lambda *a, **k: fake)
    monkeypatch.setattr(gameservers, '_iface_of', iface)
    monkeypatch.setattr(gameservers, 'HOST', '127.0.0.1')
    monkeypatch.setattr(gameservers, 'C2_HOST_IP', '192.168.1.132')
    monkeypatch.setattr(gameservers, 'C2_STATE', str(tmp_path / 'none.json'))
    assert gameservers.probe_carma2(54792, timeout=0.5)
    assert seen['ip'] == '192.168.1.132'


def test_probe_is_local_only(monkeypatch):
    assert gameservers.probe_carma2(54792, host='192.168.1.50') is None


def test_registered_with_the_watchdog_and_the_healthcheck():
    by_unit = {s['unit']: s for s in gameservers.SERVERS}
    row = by_unit['carmageddon2-server']
    assert row['probe'] == 'carma2' and row['port'] == 54792
    assert row['probe'] in gameservers.PROBES
    # Driving the intro into the lobby takes ~70 s; host.py gives up at 240.
    assert row['slow_start_sec'] >= 180
    hc = open(os.path.join(SRV, 'healthcheck.py')).read()
    assert '"carmageddon2-server"' in hc


# ---------------------------------------------------------------- the bind shim

def _build_shim(tmp_path):
    so = tmp_path / 'bindiface.so'
    r = subprocess.run(['gcc', '-O2', '-shared', '-fPIC', '-o', str(so),
                        os.path.join(C2, 'bindiface.c')], capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip('SKIPPED LOUDLY: no gcc to build bindiface.so: ' + r.stderr[:200])
    return so


def test_shim_needs_no_glibc_newer_than_the_containers(tmp_path):
    """retro-wine:bookworm has glibc 2.36. A shim built against a newer symbol
    version (atoi -> __isoc23_strtol@GLIBC_2.38 did exactly this) fails to
    preload, and the socket silently stays on every NIC."""
    so = _build_shim(tmp_path)
    out = subprocess.run(['objdump', '-T', str(so)], capture_output=True, text=True).stdout
    import re
    vers = [tuple(int(x) for x in v.split('.')) for v in re.findall(r'GLIBC_(\d+\.\d+)', out)]
    assert vers and max(vers) <= (2, 36), max(vers)


def test_shim_pins_only_the_ipxwrapper_port(tmp_path):
    so = _build_shim(tmp_path)
    log = tmp_path / 'bind.log'
    code = r'''
import socket
SO_BINDTODEVICE = 25
def bound(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    dev = s.getsockopt(socket.SOL_SOCKET, SO_BINDTODEVICE, 16).split(b"\0")[0]
    s.close()
    return dev.decode()
import sys
p = int(sys.argv[1])
print(bound(p) + "|" + bound(0))
'''
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    env = dict(os.environ, LD_PRELOAD=str(so), C2_BIND_IFACE='lo',
               C2_BIND_PORT=str(port), C2_BIND_LOG=str(log))
    r = subprocess.run([sys.executable, '-c', code, str(port)], env=env,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    pinned, other = r.stdout.strip().split('|')
    assert pinned == 'lo', 'the IPXWrapper port must be pinned to the NIC'
    assert other == '', 'every other socket must be left alone'
    assert 'SO_BINDTODEVICE lo: OK' in log.read_text(), 'the pin must be logged'


# ---------------------------------------------------------------- IPXWrapper config

def test_run_script_disables_the_wildcard_interface():
    """IPXWrapper's default "wildcard" interface spans every NIC - Wi-Fi, the
    docker bridges, tailscale (whose all-zero MAC IS the wildcard key) - and
    its broadcasts went out over Wi-Fi from .129."""
    host_ip = '192.168.1.132'
    have = subprocess.run(['ip', '-o', '-4', 'addr', 'show'], capture_output=True,
                          text=True).stdout
    if (' %s/' % host_ip) not in have:
        pytest.skip('SKIPPED LOUDLY: this is not the dev host (%s absent)' % host_ip)
    r = subprocess.run(['bash', os.path.join(C2, 'run-carmageddon2-server.sh'),
                        '--print-reg'], capture_output=True, text=True,
                       env=dict(os.environ, HOST_IP=host_ip))
    assert r.returncode == 0, r.stderr
    reg = r.stdout.replace('\r', '')
    assert reg.startswith('REGEDIT4')
    wc = reg.split('[HKEY_CURRENT_USER\\Software\\IPXWrapper\\00:00:00:00:00:00]')[1]
    assert wc.lstrip().startswith('"enabled"=dword:00000000')
    primary = reg.split('"primary"=hex:')[1].split('\n')[0]
    mac = primary.replace(',', ':').upper()
    assert ('[HKEY_CURRENT_USER\\Software\\IPXWrapper\\%s]\n"enabled"=dword:00000001'
            % mac) in reg
    assert reg.count('"enabled"=dword:00000001') == 1, 'exactly one NIC enabled'


# ---------------------------------------------------------------- the frame reader

def test_lobby_reference_avoids_the_blinking_title():
    """"LOADING STATUS" (y 15..47) blinks: a reference that included it
    matched every other frame and the driver flapped lobby/unknown."""
    refs = json.load(open(os.path.join(C2, 'frame_refs.json')))
    assert set(refs) == {'start', 'netmenu', 'lobby'}
    for r in refs['lobby']:
        x0, y0, x1, y1 = r['box']
        assert y0 >= 48, r['box']


def _xwd(w, h, pixels, directcolor_lut=None):
    """A minimal xwd: 24-bit packed ZPixmap like this Xvfb produces under Wine."""
    ncolors = 256 if directcolor_lut else 0
    name = b'x\0'
    hsize = 100 + len(name)
    bpl = w * 3 + 1      # padded stride, as seen
    hdr = [hsize, 7, 2, 24, w, h, 0, 0, 32, 0, 8, 24, bpl,
           5 if directcolor_lut else 4, 0xff0000, 0xff00, 0xff, 8, 256, ncolors,
           w, h, 0, 0, 0]
    out = struct.pack('>25I', *hdr) + name
    for i in range(ncolors):
        v = directcolor_lut(i)
        out += struct.pack('>IHHHBB', i, v << 8, v << 8, v << 8, 7, 0)
    for y in range(h):
        row = b''
        for x in range(w):
            r, g, b = pixels(x, y)
            row += bytes((b, g, r))
        out += row + b'\0'
    return out


def test_frame_reads_24bit_packed_with_a_padded_stride():
    import host
    raw = _xwd(4, 3, lambda x, y: (x * 10, y * 20, 7))
    f = host.Frame(raw)
    assert f.rgb(3, 2) == (30, 40, 7)
    assert f.rgb(0, 1) == (0, 20, 7)


def test_frame_applies_the_directcolor_colormap():
    """Under Wine's 8-bit palette emulation the root visual is DirectColor:
    the pixel bytes are colormap INDICES. Ignored, every frame is false-
    coloured and no reference ever matches."""
    import host
    raw = _xwd(2, 1, lambda x, y: (10, 20, 30), directcolor_lut=lambda i: 255 - i)
    f = host.Frame(raw)
    assert f.rgb(1, 0) == (245, 235, 225)
