#!/usr/bin/env python3
"""ntfsprobe - list an NTFS volume on a disk a Win9x box cannot mount, READ-ONLY.

Written for .243 (2026-09-25): a second IDE disk (Seagate ST380013A, 80 GB,
secondary master) that the 1997 BIOS cannot address and Win98 cannot read.
`ide9x read` pulls raw sectors straight from the IDE ports; this parses them on
the host - boot sector, $MFT runlist, MFT records (with update-sequence fixups),
$INDEX_ROOT / $INDEX_ALLOCATION - to name what is on the volume before anyone
wipes it. Nothing here can write: the only box-side action is `ide9x read`.

    python3 scripts/fleet/win9x/ntfsprobe.py --host 192.168.1.243 --part-lba 63 \\
        --list / --list "/Documents and Settings" --cat /boot.ini

Sectors are cached under --cache so a re-run costs nothing on the box.
"""
import argparse
import asyncio
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

CHUNK = 128          # sectors per ide9x read (its maximum)


class BoxDisk:
    """Raw sector reader: ide9x on the box, cached per 128-sector chunk."""

    def __init__(self, conn, device, cache_dir):
        self.c, self.dev, self.cache = conn, device, cache_dir
        os.makedirs(cache_dir, exist_ok=True)
        self.n = 0

    async def _chunk(self, first):
        path = os.path.join(self.cache, "c%09d.bin" % first)
        if os.path.exists(path):
            data = open(path, "rb").read()
            if len(data) == CHUNK * 512:
                return data
        self.n += 1
        name = "NP%05d.BIN" % (self.n % 100000)
        await self.c.send_command("DELETE C:\\RETRO_AGENT\\" + name)
        st, d = await self.c.send_command("LAUNCH C:\\RETRO_AGENT\\IDE9X.EXE read %s %d %d %s"
                                          % (self.dev, first, CHUNK, name))
        for _ in range(120):
            await asyncio.sleep(1)
            st, d = await self.c.send_command("PROCLIST")
            if not any("IDE9X" in p["name"].upper() for p in json.loads(d)):
                break
        st, data = await self.c.send_command("DOWNLOAD C:\\RETRO_AGENT\\" + name)
        await self.c.send_command("DELETE C:\\RETRO_AGENT\\" + name)
        if st != 1 or len(data) != CHUNK * 512:
            st2, log = await self.c.send_command("DOWNLOAD C:\\RETRO_AGENT\\IDE9X.TXT")
            tail = log.decode("latin-1")[-400:]
            raise IOError("read of lba %d failed (%d bytes): %s" % (first, len(data), tail))
        open(path, "wb").write(data)
        return data

    async def read(self, lba, count):
        out = b""
        while count > 0:
            first = lba - lba % CHUNK
            data = await self._chunk(first)
            off = (lba - first) * 512
            take = min(count, CHUNK - (lba - first))
            out += data[off:off + take * 512]
            lba += take
            count -= take
        return out


def fixup(rec, sector=512):
    """Apply the NTFS update-sequence array; returns the corrected record."""
    rec = bytearray(rec)
    usa_off, usa_cnt = struct.unpack_from("<HH", rec, 4)
    usn = rec[usa_off:usa_off + 2]
    for i in range(1, usa_cnt):
        end = i * sector - 2
        if end + 2 > len(rec):
            break
        if rec[end:end + 2] != usn:
            raise ValueError("fixup mismatch at sector %d (torn or not a record)" % i)
        rec[end:end + 2] = rec[usa_off + 2 * i:usa_off + 2 * i + 2]
    return bytes(rec)


def runlist(data, off):
    """Decode a non-resident runlist -> [(lcn, length)], lcn None = sparse."""
    runs, lcn = [], 0
    while True:
        h = data[off]
        if h == 0:
            return runs
        ln, lo = h & 0xF, h >> 4
        length = int.from_bytes(data[off + 1:off + 1 + ln], "little")
        if lo:
            delta = int.from_bytes(data[off + 1 + ln:off + 1 + ln + lo], "little", signed=True)
            lcn += delta
            runs.append((lcn, length))
        else:
            runs.append((None, length))
        off += 1 + ln + lo


def attributes(rec):
    """Yield (type, name, resident, header_off, attr_bytes) from an MFT record."""
    off = struct.unpack_from("<H", rec, 0x14)[0]
    while off + 8 <= len(rec):
        atype, alen = struct.unpack_from("<II", rec, off)
        if atype == 0xFFFFFFFF or alen == 0:
            return
        a = rec[off:off + alen]
        nonres = a[8]
        nlen, noff = a[9], struct.unpack_from("<H", a, 10)[0]
        name = a[noff:noff + 2 * nlen].decode("utf-16-le", "replace") if nlen else ""
        yield atype, name, not nonres, a
        off += alen


def resident_value(a):
    vlen, voff = struct.unpack_from("<IH", a, 0x10)
    return a[voff:voff + vlen]


def filename_attr(v):
    parent = struct.unpack_from("<Q", v, 0)[0] & 0xFFFFFFFFFFFF
    real = struct.unpack_from("<Q", v, 0x30)[0]
    flags = struct.unpack_from("<I", v, 0x38)[0]
    nlen, ns = v[0x40], v[0x41]
    name = v[0x42:0x42 + 2 * nlen].decode("utf-16-le", "replace")
    return {"parent": parent, "size": real, "flags": flags, "ns": ns, "name": name}


class NTFS:
    def __init__(self, disk, part_lba):
        self.d, self.base = disk, part_lba

    async def open(self):
        bs = await self.d.read(self.base, 1)
        if bs[3:11] != b"NTFS    ":
            raise ValueError("not an NTFS boot sector at lba %d" % self.base)
        self.bps = struct.unpack_from("<H", bs, 0x0B)[0]
        self.spc = bs[0x0D]
        self.total = struct.unpack_from("<Q", bs, 0x28)[0]
        self.mft_lcn = struct.unpack_from("<Q", bs, 0x30)[0]
        self.mft2_lcn = struct.unpack_from("<Q", bs, 0x38)[0]
        c = struct.unpack_from("<b", bs, 0x40)[0]
        self.rec_size = (1 << -c) if c < 0 else c * self.spc * self.bps
        c = struct.unpack_from("<b", bs, 0x44)[0]
        self.idx_size = (1 << -c) if c < 0 else c * self.spc * self.bps
        self.serial = struct.unpack_from("<Q", bs, 0x48)[0]
        self.csize = self.spc * self.bps
        rec0 = fixup(await self.d.read(self.lcn_lba(self.mft_lcn), self.rec_size // 512), self.bps)
        self.mft_runs = None
        for atype, name, res, a in attributes(rec0):
            if atype == 0x80 and not res:
                self.mft_runs = runlist(a, struct.unpack_from("<H", a, 0x20)[0])
        if not self.mft_runs:
            raise ValueError("$MFT has no non-resident $DATA")
        return self

    def lcn_lba(self, lcn):
        return self.base + lcn * self.spc

    def vcn_lba(self, runs, vcn):
        v = 0
        for lcn, length in runs:
            if vcn < v + length:
                return None if lcn is None else self.lcn_lba(lcn + (vcn - v))
            v += length
        return None

    async def record(self, n):
        off = n * self.rec_size
        vcn, within = divmod(off, self.csize)
        lba = self.vcn_lba(self.mft_runs, vcn)
        if lba is None:
            raise ValueError("MFT record %d is outside $MFT" % n)
        raw = await self.d.read(lba + within // 512, self.rec_size // 512)
        if raw[:4] != b"FILE":
            raise ValueError("MFT record %d has no FILE signature" % n)
        return fixup(raw, self.bps)

    async def read_runs(self, runs, nbytes):
        out = b""
        for lcn, length in runs:
            if len(out) >= nbytes:
                break
            want = min(length * self.csize, nbytes - len(out))
            if lcn is None:
                out += b"\0" * want
            else:
                sectors = (want + 511) // 512
                out += (await self.d.read(self.lcn_lba(lcn), sectors))[:want]
        return out

    async def list_dir(self, recno):
        rec = await self.record(recno)
        entries, alloc_runs, alloc_size = [], None, 0
        for atype, name, res, a in attributes(rec):
            if atype == 0x90 and name == "$I30":
                v = resident_value(a)
                entries += self._index_entries(v, 0x10)
            if atype == 0xA0 and name == "$I30" and not res:
                alloc_runs = runlist(a, struct.unpack_from("<H", a, 0x20)[0])
                alloc_size = struct.unpack_from("<Q", a, 0x30)[0]
        if alloc_runs:
            data = await self.read_runs(alloc_runs, alloc_size)
            for i in range(0, len(data), self.idx_size):
                blk = data[i:i + self.idx_size]
                if blk[:4] != b"INDX":
                    continue
                blk = fixup(blk, self.bps)
                entries += self._index_entries(blk, 0x18)
        seen, out = set(), []
        for e in entries:
            if e["ns"] == 2 or e["ref"] in seen:           # skip DOS 8.3 aliases
                continue
            seen.add(e["ref"])
            out.append(e)
        return sorted(out, key=lambda e: (not e["dir"], e["name"].lower()))

    def _index_entries(self, blk, hdr):
        first, used = struct.unpack_from("<II", blk, hdr)
        off, end, out = hdr + first, hdr + used, []
        while off + 16 <= end:
            ref, elen, klen, flags = struct.unpack_from("<QHHI", blk, off)
            if flags & 2 or elen == 0:
                break
            if klen >= 0x42:
                fn = filename_attr(blk[off + 16:off + 16 + klen])
                out.append({"ref": ref & 0xFFFFFFFFFFFF, "name": fn["name"], "size": fn["size"],
                            "dir": bool(fn["flags"] & 0x10000000), "ns": fn["ns"]})
            off += elen
        return out

    async def lookup(self, path):
        recno = 5
        for part in [p for p in path.split("/") if p]:
            match = [e for e in await self.list_dir(recno) if e["name"].lower() == part.lower()]
            if not match:
                raise FileNotFoundError(path)
            recno = match[0]["ref"]
        return recno

    async def cat(self, path, limit=65536):
        rec = await self.record(await self.lookup(path))
        for atype, name, res, a in attributes(rec):
            if atype == 0x80 and name == "":
                if res:
                    return resident_value(a)[:limit]
                size = struct.unpack_from("<Q", a, 0x30)[0]
                runs = runlist(a, struct.unpack_from("<H", a, 0x20)[0])
                return (await self.read_runs(runs, min(size, limit)))
        return b""

    async def volume(self):
        rec = await self.record(3)
        out = {}
        for atype, name, res, a in attributes(rec):
            if atype == 0x60 and res:
                out["label"] = resident_value(a).decode("utf-16-le", "replace")
            if atype == 0x70 and res:
                v = resident_value(a)
                out["ntfs_version"] = "%d.%d" % (v[8], v[9])
                out["dirty"] = bool(struct.unpack_from("<H", v, 10)[0] & 1)
        return out


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=9898)
    ap.add_argument("--device", default="m", choices=("m", "s"))
    ap.add_argument("--part-lba", type=int, required=True)
    ap.add_argument("--cache", default=os.path.join(REPO, ".claude", "evidence-243", "disk2", "cache"))
    ap.add_argument("--list", action="append", default=[])
    ap.add_argument("--cat", action="append", default=[])
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    c = RetroConnection(a.host, a.port)
    await c.connect(os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret"), timeout=15.0)
    report = {}
    try:
        fs = await NTFS(BoxDisk(c, a.device, a.cache), a.part_lba).open()
        vol = await fs.volume()
        report["volume"] = dict(vol, cluster_bytes=fs.csize, total_gb=round(fs.total * 512 / 1e9, 2),
                                serial="%016X" % fs.serial, mft_lcn=fs.mft_lcn)
        print(json.dumps(report["volume"]))
        for p in a.list:
            ents = await fs.list_dir(await fs.lookup(p))
            report[p] = ents
            print("\n== %s (%d entries)" % (p, len(ents)))
            for e in ents:
                print("  %s %12s  %s" % ("<DIR>" if e["dir"] else "     ", "" if e["dir"] else e["size"], e["name"]))
        for p in a.cat:
            data = await fs.cat(p)
            report["cat:" + p] = data.decode("latin-1")
            print("\n== %s\n%s" % (p, data.decode("latin-1", "replace")))
    finally:
        await c.close()
    if a.json:
        json.dump(report, open(a.json, "w"), indent=1, default=str)


if __name__ == "__main__":
    asyncio.run(main())
