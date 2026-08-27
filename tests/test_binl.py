#!/usr/bin/env python3
"""The BINL NetCard Query responder - without it, no PXE install can work.

WHAT THIS GUARDS. A PXE-booted SETUPLDR does not choose its network driver from
the image. Not from txtsetup.sif, not from [HardwareIdsDatabase], not from the
INFs. It asks the SERVER, over BINL on UDP 4011, and the server answers with the
.sys filename and service name to load.

The failure it produces is uniquely misleading:

    "The operating system image you selected does not contain the necessary
     drivers for your network adapter."

That names the image, so it sends you looking in exactly the wrong place - and
it did, repeatedly, for a long time. It really means "the server did not answer
my NetCard Query". Days went into injecting drivers into I386 and registering
them in txtsetup.sif; the image was never consulted at any point.

The reason our server was silent is worth pinning down forever: the 4011
listener opened with `if len(data) < 240 or data[0] != 1: continue`, a perfectly
sound DHCP sanity check. The NCQ is 77 bytes beginning 0x81, so it was dropped
before anything was logged. These assertions exist so that guard can never
swallow it again.
"""
import importlib.util
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PXE = os.path.join(HERE, os.pardir, 'scripts', 'pxe')
sys.path.insert(0, PXE)

spec = importlib.util.spec_from_file_location('binl', os.path.join(PXE, 'binl.py'))
binl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(binl)

FAILS = []


def check(name, cond):
    print(('  PASS  ' if cond else '  FAIL  ') + name)
    if not cond:
        FAILS.append(name)


def make_ncq(ven, dev, subven=0, subdev=0, nic_type=2,
             path=br'\\192.168.1.122\files'):
    """Build an NCQ exactly as setupldr does."""
    return (binl.NCQ_MAGIC
            + struct.pack('<III', 0x30, 2, 0)
            + b'\x11' * 16                          # system GUID
            + struct.pack('<I', nic_type)
            + struct.pack('<HH', ven, dev)
            + bytes([2, 0, 0, 3])                   # class/sub/progif/rev
            + struct.pack('<H', 0x0018)             # bus:dev:func
            + b'\x00\x00'
            + struct.pack('<HH', subven, subdev)
            + struct.pack('<H', len(path) + 1) + path + b'\x00')


def wread(buf, off):
    out = []
    while off + 1 < len(buf) and buf[off:off + 2] != b'\x00\x00':
        out.append(buf[off:off + 2])
        off += 2
    return b''.join(out).decode('utf-16-le')


def main():
    print('== an NCQ is recognised and parsed ==')
    q = binl.parse_ncq(make_ncq(0x8086, 0x100E, 0x8086, 0x002E))
    check('a real NCQ parses', q is not None)
    check('vendor id is read correctly', q.ven == 0x8086)
    check('device id is read correctly', q.dev == 0x100E)
    check('the UNC path is recovered', q.path == r'\\192.168.1.122\files')
    check('nic_type 2 (PCI) is seen', q.nic_type == 2)

    print('== SUBSYS is keyed device-then-vendor, as INFs write it ==')
    # An INF writes SUBSYS_002E8086 for subsystem vendor 8086, device 002E.
    # Getting this backwards silently downgrades every exact match to generic,
    # which still works - so it would never be noticed without this assertion.
    check('exact_key matches the INF spelling',
          q.exact_key == '8086&100E&002E8086')
    check('generic_key is vendor&device', q.generic_key == '8086&100E')

    print('== non-NCQ traffic is not mistaken for a query ==')
    check('a DHCP packet is rejected', binl.parse_ncq(b'\x01' + b'\x00' * 300) is None)
    check('a short packet is rejected', binl.parse_ncq(b'\x81NCQ') is None)
    check('empty input is rejected', binl.parse_ncq(b'') is None)

    print('== the NCR reply matches what setupldr parses ==')
    r = binl.build_ncr('e1000325.sys', 'E1000')
    magic = r[:4]
    total, status, zero, o1, o2, o3, blen, boff = struct.unpack_from('<IIIIIIII', r, 4)
    check('magic is 0x82 NCR', magic == binl.NCR_MAGIC)
    check('the declared length is the real length', total == len(r))
    # Any non-zero status produces the SAME fatal message as no reply at all,
    # which would send the next person back to hunting phantom driver problems.
    check('status is 0 (anything else reads as failure)', status == 0)
    check('the header is at least 0x24 bytes', o1 >= 0x24)
    check('string offsets are inside the packet', max(o1, o2, o3) < len(r))
    check('string 1 is the driver file', wread(r, o1) == 'e1000325.sys')
    check('string 2 is the driver file (this is the one loaded)',
          wread(r, o2) == 'e1000325.sys')
    check('string 3 is the service name', wread(r, o3) == 'E1000')

    print('== setupldr buffer limits are enforced, not overrun ==')
    # s2/s3 land in 24-wchar buffers. Silently overflowing them corrupts the
    # loader rather than failing cleanly, so refuse instead.
    over = False
    try:
        binl.build_ncr('a' * 40 + '.sys', 'svc')
    except ValueError:
        over = True
    check('an over-long driver name is refused', over)
    over = False
    try:
        binl.build_ncr('ok.sys', 'S' * 40)
    except ValueError:
        over = True
    check('an over-long service name is refused', over)

    print('== the NCE failure reply is well formed ==')
    e = binl.build_nce()
    check('NCE carries its own magic', e[:4] == binl.NCE_MAGIC)
    check('NCE declares at least the header length',
          struct.unpack_from('<I', e, 4)[0] >= 0x24)

    print('== the driver database resolves the adapters that matter ==')
    tmp = tempfile.mkdtemp()
    dbpath = os.path.join(tmp, 'nicdb.json')
    with open(dbpath, 'w') as fh:
        json.dump({'exact': {'8086&100E&002E8086':
                             {'sys': 'exact.sys', 'service': 'EX'}},
                   'generic': {'8086&100E':
                               {'sys': 'generic.sys', 'service': 'GEN'}}}, fh)
    db = binl.NicDb(dbpath)
    e1, how1 = db.lookup(binl.parse_ncq(make_ncq(0x8086, 0x100E, 0x8086, 0x002E)))
    check('a subsystem match wins over the generic one',
          e1 and e1['sys'] == 'exact.sys' and how1 == 'exact')
    e2, how2 = db.lookup(binl.parse_ncq(make_ncq(0x8086, 0x100E, 0x1028, 0x0134)))
    check('an unlisted subsystem still matches on vendor/device',
          e2 and e2['sys'] == 'generic.sys' and how2 == 'generic')
    e3, _ = db.lookup(binl.parse_ncq(make_ncq(0xDEAD, 0xBEEF)))
    check('an unknown adapter returns nothing rather than a wrong driver',
          e3 is None)

    print('== a missing database degrades, it does not crash ==')
    empty = binl.NicDb(os.path.join(tmp, 'does-not-exist.json'))
    check('a missing file yields an empty db', len(empty) == 0)
    e4, _ = empty.lookup(binl.parse_ncq(make_ncq(0x8086, 0x100E)))
    check('lookups against an empty db return nothing', e4 is None)
    with open(os.path.join(tmp, 'bad.json'), 'w') as fh:
        fh.write('{ not json')
    check('a corrupt database does not raise',
          len(binl.NicDb(os.path.join(tmp, 'bad.json'))) == 0)

    print('== the server handles NCQ before the DHCP guard ==')
    src = open(os.path.join(PXE, 'pxe_server.py'), encoding='utf-8').read()
    code = '\n'.join(l.split('#', 1)[0] for l in src.splitlines())
    check('the NCQ branch exists', 'binl.NCQ_MAGIC' in code)
    check('NCQ is checked BEFORE the 240-byte DHCP guard',
          code.index('binl.NCQ_MAGIC') < code.index('len(data) < 240 or data[0] != 1'))
    check('the handler is defined', 'def handle_ncq' in code)

    print()
    if FAILS:
        print(f'{len(FAILS)} FAILED: ' + ', '.join(FAILS))
        return 1
    print('binl: all checks passed')
    return 0


def test_binl():
    """pytest entry point - without it this file is collected, contains no test
    function, and reports zero tests, which reads exactly like passing."""
    assert main() == 0, 'binl assertions failed: ' + ', '.join(FAILS)


if __name__ == '__main__':
    sys.exit(main())
