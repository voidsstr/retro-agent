#!/usr/bin/env python3
"""Run the GAME'S OWN mode-list code (EnumDisplayModes callbacks, 64 cap, MotS's
pass-2 filter, the CRT qsort and the game's comparator) under unicorn, on a
measured enumeration sequence, and print the list the game would store.

usage: emu_modelist.py df2|mots <fixture> [pitch-mode]
  pitch-mode: 'natural' (lPitch = w*bpp/8, default) or 'aligned32'
"""
import re, struct, sys
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EAX

LIB = "/mnt/retro-share/Files/Games-Library"
GAMES = {
    "df2":  dict(exe=LIB + "/JediKnightDF2/JK.EXE", p1=0x426030, p2=0x425e30, counter=0x55b3fc,
                 array=0x866cc0, qsort=0x513060, cmp=0x4249d0),
    "mots": dict(exe=LIB + "/JediKnightMotS/JKM.EXE", p1=0x429160, p2=0x428f60, counter=0x5b6bac,
                 array=0x948b60, qsort=0x56c510, cmp=0x427b00),
}
SENTINEL = 0x0DEAD000
STACK = 0x00200000


def load(path):
    pe = pefile.PE(path)
    data = open(path, "rb").read()
    base = pe.OPTIONAL_HEADER.ImageBase
    size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(base, size)
    mu.mem_write(base, data[:pe.OPTIONAL_HEADER.SizeOfHeaders])
    for s in pe.sections:
        raw = data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
        mu.mem_write(base + s.VirtualAddress, raw[:max(s.Misc_VirtualSize, len(raw))])
    mu.mem_map(STACK, 0x100000)
    mu.mem_map(SENTINEL, 0x1000)
    mu.mem_write(SENTINEL, b"\xf4" * 16)          # hlt
    mu.mem_map(0x01000000, 0x10000)                # scratch for the DDSURFACEDESC
    return mu


def call(mu, fn, args):
    sp = STACK + 0x100000 - 0x100
    frame = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", a & 0xffffffff) for a in args)
    sp -= len(frame)
    mu.mem_write(sp, frame)
    mu.reg_write(UC_X86_REG_ESP, sp)
    mu.emu_start(fn, SENTINEL, count=5_000_000)
    return mu.reg_read(UC_X86_REG_EAX)


def ddsd(w, h, pf, bpp, pitch):
    b = bytearray(0x6c)
    struct.pack_into("<IIIIi", b, 0, 0x6c, 0x100f, h, w, pitch)
    if bpp == 16:
        masks = (0xf800, 0x07e0, 0x001f)
    elif bpp in (24, 32):
        masks = (0xff0000, 0xff00, 0xff)
    else:
        masks = (0, 0, 0)
    struct.pack_into("<IIIIIIII", b, 0x48, 0x20, pf, 0, bpp, masks[0], masks[1], masks[2], 0)
    return bytes(b)


def read_fixture(path):
    p1, p2 = [], []
    for line in open(path):
        m = re.match(r"\s*(\d+)\s+(X?)\s+(\d+)x(\d+)\s+(\d+)bpp pf=0x([0-9a-f]+)", line)
        if not m:
            continue
        rec = (int(m.group(3)), int(m.group(4)), int(m.group(6), 16), int(m.group(5)))
        (p1 if m.group(2) else p2).append(rec)
    return p1, p2


def run(game, p1, p2, pitch_mode="natural"):
    g = GAMES[game]
    mu = load(g["exe"])
    mu.mem_write(g["counter"], struct.pack("<I", 0))
    for pas, seq in ((1, p1), (2, p2)):
        for (w, h, pf, bpp) in seq:
            pitch = w * bpp // 8
            if pitch_mode == "aligned32":
                pitch = (pitch + 31) & ~31
            mu.mem_write(0x01000000, ddsd(w, h, pf, bpp, pitch))
            call(mu, g["p1"] if pas == 1 else g["p2"], [0x01000000, 0])
    n = struct.unpack("<I", mu.mem_read(g["counter"], 4))[0]
    before = [struct.unpack_from("<IIII", mu.mem_read(g["array"] + i * 0x54, 0x24), 0) for i in range(n)]
    call(mu, g["qsort"], [g["array"], n, 0x54, g["cmp"]])
    out = []
    for i in range(n):
        rec = mu.mem_read(g["array"] + i * 0x54, 0x54)
        modex, _asp, w, h = struct.unpack_from("<IIII", rec, 0)
        bpp = struct.unpack_from("<I", rec, 0x20)[0]
        out.append((modex, w, h, bpp))
    return n, out


def index_of(lst, w=1920, h=1080, bpp=16):
    for i, (mx, ww, hh, b) in enumerate(lst):
        if not mx and ww == w and hh == h and b == bpp:
            return i
    return -1


if __name__ == "__main__":
    game, fx = sys.argv[1], sys.argv[2]
    pm = sys.argv[3] if len(sys.argv) > 3 else "natural"
    p1, p2 = read_fixture(fx)
    if game == "mots":
        # MotS's pass 1 runs at coop 0x11: it sees the NORMAL list's 320-wide
        # modes, not the ModeX ones the DF2 fixture recorded under 0x51.
        p1 = [m for m in p2 if m[0] == 320]
    n, lst = run(game, p1, p2, pm)
    for i, (mx, w, h, b) in enumerate(lst):
        print("%3d %s %4dx%-4d %2dbpp" % (i, "X" if mx else " ", w, h, b))
    print("RESULT game=%s stored=%d idx1080x16=%d" % (game, n, index_of(lst)))
