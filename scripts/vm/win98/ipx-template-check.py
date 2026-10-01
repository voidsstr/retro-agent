#!/usr/bin/env python3
"""Compare IPXSETUP's Win98 registry template with a Win98 disk image - OFFLINE.

IPXSETUP (agent 1.97.0) installs IPX/SPX on Windows 98 SE by writing a registry
template (agent/shared/ipxplan.h, "THE WINDOWS 98 SE REGISTRY TEMPLATE") - and
writes nothing until IpxSetup9xTemplateOk=1, which is set only once the
template matches a golden Network-applet install. This tool is that check: it
reads SYSTEM.DAT straight out of a raw 86Box / dd disk image (read-only - the
image is opened 'rb' and nothing is mounted), finds the install the applet made
(Enum\\Network\\NWLINK\\<I> -> NetTrans\\<K>, and the adapter whose Bindings
names it), renders the template with THOSE indices and compares value by value
in both directions:

    MISSING in image   the template would write a value the applet did not
    DIFFERS            same value, other data
    EXTRA in image     the applet wrote a value under the protocol's own keys
                       that the template lacks

    python3 scripts/vm/win98/ipx-template-check.py ~/retro-vm/86box/vm98/w98.img
    python3 scripts/vm/win98/ipx-template-check.py <image> --lba 63

Template part 2 (the queued WSCInstallProvider + RunOnce FirstBootCall) is only
observable in an image taken AFTER the applet's OK and BEFORE the next boot:
FirstBootCall runs the queue and empties it. Copy the image at that moment and
check the copy. Read a COPY (or a shut-down VM) - a running 86Box may not have
flushed SYSTEM.DAT. Exit 0 only when nothing observable differs.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
HEADER = os.path.join(REPO, "agent", "shared", "ipxplan.h")
PARTS = ("ipx9x_tmpl_stack", "ipx9x_tmpl_ws2", "ipx9x_tmpl_bind")
QUEUE_KEYS = ("Software\\Microsoft\\Windows\\CurrentVersion\\Setup\\NetSetup\\QueuedAPI",
              "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce")
OWN_KEYS = ("System\\CurrentControlSet\\Services\\Class\\NetTrans\\",
            "System\\CurrentControlSet\\Services\\VxD\\NWLINK", "Enum\\Network\\NWLINK")


# ---------------------------------------------------------------------------
# the template, straight from the C header
# ---------------------------------------------------------------------------
def _c_unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"'}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def template_parts(header=HEADER):
    """{part name: text} - the C string literals of each template array."""
    src = open(header, encoding="latin-1").read()
    out = {}
    for name in PARTS:
        i = src.index("static const char %s[] =" % name)
        j = src.index(";", i)
        body = re.sub(r"/\*.*?\*/", "", src[i:j], flags=re.S)
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        out[name] = "".join(_c_unescape(l) for l in lits)
    return out


def render(text, values):
    """%NAME% -> values[NAME] (ipx9x_render's rules: unknown names are an error)."""
    def sub(m):
        if m.group(1) not in values:
            raise KeyError("template placeholder %%%s%% has no value" % m.group(1))
        return values[m.group(1)]
    return re.sub(r"%([A-Za-z0-9_:]+)%", sub, text)


def parse_reg(text):
    """[(key, {name: (type, bytes)})] in file order, for the REGEDIT4 subset the
    template uses: "strings" (only \\\\ and \\" escapes) and hex: binary."""
    out, cur = [], None
    for line in text.splitlines():
        if not line or line.startswith(";") or line == "REGEDIT4":
            continue
        if line.startswith("["):
            cur = {}
            out.append((line[1:-1].split("\\", 1)[1], cur))
            continue
        m = re.match(r'^(@|"((?:[^"\\]|\\.)*)")=(.*)$', line)
        if not m:
            raise ValueError("unparsed template line: %r" % line)
        name = "" if m.group(1) == "@" else re.sub(r"\\(.)", r"\1", m.group(2))
        val = m.group(3)
        if val.startswith('"'):
            cur[name] = (1, re.sub(r"\\(.)", r"\1", val[1:-1]).encode("latin-1"))
        elif val.startswith("hex:"):
            cur[name] = (3, bytes(int(x, 16) for x in val[4:].split(",")))
        else:
            raise ValueError("unsupported value type: %r" % line)
    return out


# ---------------------------------------------------------------------------
# read-only FAT32 + Win9x SYSTEM.DAT (CREG) readers
# ---------------------------------------------------------------------------
class Fat32:
    def __init__(self, path, lba=None):
        self.f = open(path, "rb")                       # READ-ONLY
        if lba is None:
            lba = self._first_fat32_lba()
        self.base = lba * 512
        bs = self._rd(0, 512)
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        self.rsv = struct.unpack_from("<H", bs, 14)[0]
        nfats = bs[16]
        fatsz = struct.unpack_from("<I", bs, 36)[0]
        self.root = struct.unpack_from("<I", bs, 44)[0]
        self.data = self.rsv + nfats * fatsz
        self.cs = self.bps * self.spc

    def _first_fat32_lba(self):
        self.f.seek(0)
        mbr = self.f.read(512)
        for i in range(4):
            e = mbr[446 + 16 * i:446 + 16 * (i + 1)]
            if e[4] in (0x0B, 0x0C):
                return struct.unpack_from("<I", e, 8)[0]
        return 63

    def _rd(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def _next(self, c):
        return struct.unpack("<I", self._rd(self.rsv * self.bps + c * 4, 4))[0] & 0x0FFFFFFF

    def _chain(self, c):
        out = []
        while 2 <= c < 0x0FFFFFF8 and len(out) < 4000000:
            out.append(c)
            c = self._next(c)
        return out

    def _clus(self, c):
        return self._rd((self.data + (c - 2) * self.spc) * self.bps, self.cs)

    def _entries(self, c):
        raw = b"".join(self._clus(x) for x in self._chain(c))
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                lfn.insert(0, e[1:11] + e[14:26] + e[28:32])
                continue
            short = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            name = short + ("." + ext if ext else "")
            long = b"".join(lfn).decode("utf-16le", "ignore").split("\x00")[0] if lfn else None
            lfn = []
            clus = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            yield name, long, clus, struct.unpack_from("<I", e, 28)[0]

    def find(self, path):
        c, ent = self.root, None
        for part in [p for p in path.replace("/", "\\").split("\\") if p]:
            ent = next((x for x in self._entries(c)
                        if x[0].upper() == part.upper() or (x[1] and x[1].upper() == part.upper())), None)
            if not ent:
                return None
            c = ent[2]
        return ent

    def read(self, path):
        ent = self.find(path)
        if not ent:
            return None
        return b"".join(self._clus(x) for x in self._chain(ent[2]))[:ent[3]]


class Win9xReg:
    """SYSTEM.DAT: CREG header, RGKN key tree, RGDB data blocks."""

    def __init__(self, data):
        if data[:4] != b"CREG":
            raise ValueError("not a Win9x registry file")
        self.d = data
        rgdb, = struct.unpack_from("<I", data, 8)
        nblocks, = struct.unpack_from("<H", data, 16)
        self.rgkn = 0x20
        self.root, = struct.unpack_from("<I", data, self.rgkn + 8)
        self.blocks, off = {}, rgdb
        for i in range(nblocks):
            size, = struct.unpack_from("<I", data, off + 4)
            self.blocks[i] = (off, size)
            off += size

    def _dke(self, off):
        _a, _b, _c, _prev, child, nxt, ls, ms = struct.unpack_from("<IIIIIIHH", self.d, self.rgkn + off)
        return child, nxt, ls, ms

    def _dkh(self, ms, ls):
        boff, bsize = self.blocks[ms]
        p, end = boff + 0x20, boff + bsize
        while p < end:
            nextoff, nls, nms, _used, nlen, nvals, _x = struct.unpack_from("<IHHIHHI", self.d, p)
            if nextoff == 0:
                break
            if nls == ls and nms == ms:
                name = self.d[p + 20:p + 20 + nlen].decode("latin-1")
                vals, q = [], p + 20 + nlen
                for _ in range(nvals):
                    t, _x1, vnl, vdl = struct.unpack_from("<IIHH", self.d, q)
                    vals.append((self.d[q + 12:q + 12 + vnl].decode("latin-1"), t,
                                 self.d[q + 12 + vnl:q + 12 + vnl + vdl]))
                    q += 12 + vnl + vdl
                return name, vals
            p += nextoff
        return None

    def children(self, off):
        child, _n, _l, _m = self._dke(off)
        out = []
        while child != 0xFFFFFFFF:
            c2, nxt, ls, ms = self._dke(child)
            h = self._dkh(ms, ls)
            out.append((h[0] if h else "?", child))
            child = nxt
        return out

    def find(self, path):
        off = self.root
        for part in [p for p in path.split("\\") if p]:
            m = [c for n, c in self.children(off) if n.upper() == part.upper()]
            if not m:
                return None
            off = m[0]
        return off

    def values(self, off):
        _c, _n, ls, ms = self._dke(off)
        h = self._dkh(ms, ls)
        return {vn: (t, vd) for vn, t, vd in (h[1] if h else [])}


# ---------------------------------------------------------------------------
# the comparison
# ---------------------------------------------------------------------------
def find_install(reg):
    """(K, I, adapter enum path) of the NWLINK install in the image, or None."""
    nw = reg.find("Enum\\Network\\NWLINK")
    if nw is None:
        return None
    for inst, off in reg.children(nw):
        drv = reg.values(off).get("Driver", (1, b""))[1].rstrip(b"\0").decode("latin-1")
        if not drv.lower().startswith("nettrans\\"):
            continue
        want = ("NWLINK\\" + inst).upper()
        enum = reg.find("Enum")
        for bus, boff in reg.children(enum):
            if bus.upper() == "NETWORK":
                continue
            for dev, doff in reg.children(boff):
                for ins, ioff in reg.children(doff):
                    b = reg.find("Enum\\%s\\%s\\%s\\Bindings" % (bus, dev, ins))
                    if b is not None and any(n.upper() == want for n in reg.values(b)):
                        return drv.split("\\", 1)[1], inst, "%s\\%s\\%s" % (bus, dev, ins)
    return None


def compare(reg, parts, values):
    """Lines describing every difference, and how many there were."""
    lines, bad = [], 0
    for pname in PARTS:
        for key, vals in parse_reg(render(parts[pname], values)):
            off = reg.find(key)
            have = reg.values(off) if off is not None else {}
            # the queue is consumed at the first boot: unobservable when none of
            # ITS values is there (RunOnce may hold other programs' entries)
            if key.startswith(QUEUE_KEYS) and not (set(vals) & set(have)):
                lines.append("(not observable - the first boot already ran FirstBootCall) "
                             "[%s] %d value(s)" % (key, len(vals)))
                continue
            for name, (t, d) in vals.items():
                if name not in have:
                    lines.append("MISSING in image  [%s] %s" % (key, name or "@"))
                    bad += 1
                    continue
                ht, hd = have[name]
                if (t == 1 and hd.rstrip(b"\0") != d) or (t == 3 and hd != d):
                    lines.append("DIFFERS           [%s] %s: template %r, image %r"
                                 % (key, name or "@", d, hd))
                    bad += 1
            if key.startswith(OWN_KEYS):
                for name in have:
                    if name not in vals:
                        lines.append("EXTRA in image    [%s] %s=%r" % (key, name or "@", have[name][1]))
                        bad += 1
    return lines, bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="a raw Win98 disk image (86Box .img / dd)")
    ap.add_argument("--lba", type=int, default=None, help="FAT32 partition start (default: from the MBR)")
    a = ap.parse_args(argv)
    fs = Fat32(a.image, a.lba)
    data = fs.read("WINDOWS\\SYSTEM.DAT")
    if not data:
        print("no WINDOWS\\SYSTEM.DAT in %s" % a.image)
        return 2
    reg = Win9xReg(data)
    inst = find_install(reg)
    if not inst:
        print("no IPX/SPX install in this image (no Enum\\Network\\NWLINK bound to an adapter)")
        return 2
    k, i, nic = inst
    queued = reg.find(QUEUE_KEYS[0])
    q = "0"
    if queued is not None:
        items = [n for n, _o in reg.children(queued) if n.upper().startswith("ITEM")]
        if items:
            q = items[0][4:]
    values = {"K": k, "I": i, "Q": q, "FRAME": "1", "NIC": nic,
              "SYSDIR:Q": "C:\\\\WINDOWS\\\\SYSTEM"}
    print("image install: NetTrans\\%s, Enum\\Network\\NWLINK\\%s, bound to Enum\\%s" % (k, i, nic))
    lines, bad = compare(reg, template_parts(), values)
    for l in lines:
        print(l)
    print("%d difference(s) between the template and the image" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
