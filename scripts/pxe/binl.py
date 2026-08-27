"""BINL NetCard Query responder - the piece that makes RIS text-mode setup work.

THE DISCOVERY THIS IMPLEMENTS. When SETUPLDR is booted over the network it does
NOT pick its network driver from the image. Not from txtsetup.sif, not from
[HardwareIdsDatabase], not from the INFs - it asks the SERVER, over BINL on UDP
4011, and the server tells it which .sys to load and what to call the service.

That is why injecting drivers into I386 and registering them in txtsetup.sif
changed nothing: the image was never consulted. Setup sends a NetCard Query
carrying the adapter's PCI IDs, waits for four timeouts, and prints

    "The operating system image you selected does not contain the necessary
     drivers for your network adapter."

which names the image and thereby sends you looking in exactly the wrong place.
It really means "the server did not answer my NCQ".

Established by disassembling SETUPLDR (the TFTP'd `ntldr`): the message is ID
0x237B in its message table, pushed at exactly one call site, which is the
error path of the BINL exchange at 0x329DD1. Setup only takes this path when
booted from net(0), and then it is unconditional - a PXE-booted setupldr always
requires a BINL responder. Three further independent investigations agreed.

WHY OUR SERVER WAS SILENT: the 4011 listener began with

    if len(data) < 240 or data[0] != 1: continue

which is a perfectly reasonable DHCP sanity check, and the NCQ is 77 bytes
beginning 0x81. It was dropped before anything was logged.

WIRE FORMAT (offsets are from the start of the datagram)

  request  '\x81NCQ'
    0x00  4  magic b'\x81NCQ'
    0x04  4  0x30
    0x08  4  status slot
    0x0C  4  0
    0x10 16  system GUID (zero if the ROM had none)
    0x20  4  NIC type: 2 = PCI. Anything else fails earlier, with a different
             message about an older boot ROM.
    0x24  2  PCI vendor id
    0x26  2  PCI device id
    0x28  4  base class / sub class / prog-if / revision
    0x2C  2  bus:dev:func
    0x30  2  subsystem vendor id
    0x32  2  subsystem device id
    0x34  2  length of the path that follows
    0x36  n  '\' + ANSI UNC of the install share + NUL

  reply    '\x82NCR' (success) - '\x82NCE' is the explicit failure form, and
           anything else is treated as no reply at all
    0x00  4  magic b'\x82NCR'
    0x04  4  total length, must be >= 0x24
    0x08  4  status - MUST be 0; any other value produces the same fatal
             message as no reply, which is worth remembering when debugging
    0x0C  4  0
    0x10  4  offset of string 1: the device's PnP HARDWARE ID, <= 63 wchars.
             NOT a file name. The kernel wcscmp's this - case-sensitively -
             against each entry of the adapter's HardwareID and CompatibleID
             multi-sz. Only on a match does it write Service/ClassGUID/Driver
             into the card's Enum key, and only then does PnP start the
             miniport. Get this wrong and everything else still looks right:
             the driver loads, the stack loads, and twenty seconds later the
             machine bugchecks 0xBB because the IP device never got an interface.
    0x14  4  offset of string 2: the driver FILE name, <= 23 wchars. This is
             the one setup down-converts to ANSI and loads, as a bare filename
             from the boot source - no [SourceDisksFiles] lookup, so the .sys
             simply has to be present flat in I386.
    0x18  4  offset of string 3: service name, <= 23 wchars. Setup appends it
             to \\Registry\\Machine\\System\\CurrentControlSet\\Services\\ to form
             the boot driver's RegistryPath.
    0x1C  4  length of the registry blob (0 is legal and tolerated)
    0x20  4  offset of that blob. Its values are written to
             CCS\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\0000,
             not to the service key. Missing ones fall back to miniport
             defaults, so the blob is optional - but sending it is closer to
             what a real RIS server does.
  All three strings are UTF-16LE and NUL-terminated. Offsets are from the start
  of the reply and must be >= 0x24.
"""
import json
import os
import struct

NCQ_MAGIC = b'\x81NCQ'
NCR_MAGIC = b'\x82NCR'
NCE_MAGIC = b'\x82NCE'

NCQ_MIN_LEN = 0x36
HDR_LEN = 0x24


class NicQuery(object):
    __slots__ = ('nic_type', 'ven', 'dev', 'subven', 'subdev', 'rev',
                 'bus_dev_func', 'guid', 'path')

    @property
    def exact_key(self):
        """INF SUBSYS_ is written device-then-vendor: SUBSYS_002E8086."""
        return '%04X&%04X&%04X%04X' % (self.ven, self.dev,
                                       self.subdev, self.subven)

    @property
    def generic_key(self):
        return '%04X&%04X' % (self.ven, self.dev)

    @property
    def hardware_id(self):
        """The PnP id to send in NCR string 1.

        Deliberately the SHORT form. pci.sys always generates
        PCI\\VEN_xxxx&DEV_xxxx among the device's ids, whereas the
        subsystem/revision forms depend on what the card's config space
        actually yields - echoing a subsystem we guessed wrong fails the
        case-sensitive compare and looks exactly like no reply at all.
        Uppercase hex is mandatory for the same reason.
        """
        return 'PCI\\VEN_%04X&DEV_%04X' % (self.ven, self.dev)

    def __str__(self):
        return ('PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X'
                % (self.ven, self.dev, self.subdev, self.subven, self.rev))


def parse_ncq(data):
    """Parse a NetCard Query, or return None if this is not one."""
    if len(data) < NCQ_MIN_LEN or data[0:4] != NCQ_MAGIC:
        return None
    q = NicQuery()
    q.nic_type = struct.unpack_from('<I', data, 0x20)[0]
    q.ven, q.dev = struct.unpack_from('<HH', data, 0x24)
    q.rev = data[0x2B]
    q.bus_dev_func = struct.unpack_from('<H', data, 0x2C)[0]
    q.subven, q.subdev = struct.unpack_from('<HH', data, 0x30)
    q.guid = data[0x10:0x20]
    path = ''
    if len(data) > 0x36:
        raw = data[0x36:]
        end = raw.find(b'\x00')
        path = raw[:end if end >= 0 else len(raw)].decode('latin-1')
    q.path = path
    return q


def _wstr(s):
    return s.encode('utf-16-le') + b'\x00\x00'


def build_blob(values):
    """Serialise registry values for the netboot card's class instance key.

    Format, walked byte-by-byte by the kernel:

        <ValueName> NUL <TypeCode> NUL <ValueData> NUL   (repeated)
        NUL                                              (terminator)

    all ANSI. TypeCode is one character: '1' means REG_DWORD and the data is
    decimal ASCII, '2' means REG_SZ. Names are capped at 31 characters and data
    at 127 by the kernel's fixed conversion buffers, so anything longer is
    silently truncated - refuse instead.
    """
    out = bytearray()
    for name, kind, data in values:
        if kind not in ('1', '2'):
            raise ValueError('blob type must be "1" (dword) or "2" (sz)')
        text = str(data)
        if len(name) > 31:
            raise ValueError('blob value name over 31 chars: %r' % name)
        if len(text) > 127:
            raise ValueError('blob value data over 127 chars: %r' % text)
        out += name.encode('latin-1') + b'\x00'
        out += kind.encode('ascii') + b'\x00'
        out += text.encode('latin-1') + b'\x00'
    out += b'\x00'
    return bytes(out)


# What a real RIS server derives from the NIC's INF. These are the values that
# matter to a miniport being started; everything else falls back to a default.
DEFAULT_BLOB = build_blob([
    ('BusType', '1', 5),            # PCI
    ('Characteristics', '1', 132),  # NCF_PHYSICAL | NCF_HAS_UI
])


def build_ncr(hardware_id, sys_file, service, blob=DEFAULT_BLOB):
    """Build a successful NetCard Reply.

    hardware_id must be the PnP id the kernel will see for this device, spelled
    exactly as pci.sys formats it - uppercase, e.g. PCI\\VEN_8086&DEV_100E. The
    comparison is a case-sensitive wcscmp against the adapter's HardwareID and
    CompatibleID lists, and a mismatch fails silently twenty seconds later.
    """
    s1 = _wstr(hardware_id)       # PnP hardware id, <= 63 wchars
    s2 = _wstr(sys_file)          # what setup loads, <= 23 wchars
    s3 = _wstr(service)           # service name, <= 23 wchars
    if len(s1) > 0x80:
        raise ValueError('hardware id too long for setupldr: %r' % hardware_id)
    if len(s2) > 0x30 or len(s3) > 0x30:
        raise ValueError('BINL string too long for setupldr buffers: '
                         '%r / %r' % (sys_file, service))
    blob = blob or b''
    off1 = HDR_LEN
    off2 = off1 + len(s1)
    off3 = off2 + len(s2)
    off_blob = off3 + len(s3)
    end = off_blob + len(blob)
    # setupldr rejects a string-1 offset at or beyond 0xFDC, so the whole reply
    # has to stay small; the blob is the only part that can grow.
    if off1 >= 0xFDC or end > 4000:
        raise ValueError('NCR reply too large (%d bytes)' % end)
    return (NCR_MAGIC
            + struct.pack('<IIIIIIII', end, 0, 0, off1, off2, off3,
                          len(blob), off_blob if blob else 0)
            + s1 + s2 + s3 + blob)


def build_nce(status=0xC0000001):
    """Explicit 'no driver' reply. Setup shows the same fatal message as for a
    timeout, but answering is still better than silence: it fails in one round
    trip instead of four, and it tells the operator we saw the query."""
    return NCE_MAGIC + struct.pack('<IIIIIIII', HDR_LEN, status, 0, 0, 0, 0, 0, 0)


class NicDb(object):
    """PCI id -> (driver file, service name), built by build-nicdb.py."""

    def __init__(self, path):
        self.path = path
        self.exact = {}
        self.generic = {}
        self.mtime = 0
        self.load()

    def load(self):
        try:
            st = os.stat(self.path)
        except OSError:
            return False
        try:
            with open(self.path, encoding='ascii') as fh:
                db = json.load(fh)
        except (OSError, ValueError):
            return False
        self.exact = db.get('exact', {})
        self.generic = db.get('generic', {})
        self.mtime = st.st_mtime
        return True

    def reload_if_changed(self):
        """Pick up a rebuilt database without a restart - the map changes
        whenever drivers are injected, and that must not require bouncing a
        server a machine may be mid-install against."""
        try:
            if os.stat(self.path).st_mtime != self.mtime:
                return self.load()
        except OSError:
            pass
        return False

    def lookup(self, q):
        """Most specific match first: subsystem, then plain vendor/device."""
        e = self.exact.get(q.exact_key)
        if e:
            return e, 'exact'
        e = self.generic.get(q.generic_key)
        if e:
            return e, 'generic'
        return None, None

    def __len__(self):
        return len(self.generic)
