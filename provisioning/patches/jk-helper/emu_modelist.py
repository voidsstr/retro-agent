#!/usr/bin/env python3
"""Run the GAME'S OWN mode-list code on a mode sequence and print what it stores.

WHY THIS EXISTS. JKMODE.EXE writes `displayMode`, an index into a list the game
builds with its own code: two EnumDisplayModes callbacks sharing a 64-entry
counter, a high-resolution 8-bpp filter in MotS only, and then the CRT qsort
with the game's comparator. jklogic.h re-implements all of that, and one wrong
detail turns a 1920x1080 16-bpp index into a 32-bpp mode or a read past the end
of the list. So this runs the REAL code - both callbacks, the qsort (JK
0x513060 / MotS 0x56c510) and the comparator - from the staged JK.EXE / JKM.EXE
under unicorn on a measured sequence, and tests/python/test_patch_jk-helper.py
holds jklogic.h to its output. It found the sort and the MotS filter that the
first version of the helper missed (2026-09-29).

Only pure code is executed (the callbacks call nothing but a log2 helper; the
qsort calls the comparator and its swap), so no Windows API is emulated. The
game binary is read from the share at run time and nothing of it is stored.

    python3 emu_modelist.py df2|mots <fixture> [natural|aligned32]

Needs `pip install unicorn pefile` (a venv is fine). The fixture format is
fixtures/ddenum_<box>_df2_sequence.txt: one enumerated mode per line, `X` for
the ModeX pass. MotS's pass 1 (coop 0x11) is taken as the normal list's 320-wide
modes. `natural` reports lPitch = width * bpp / 8; `aligned32` rounds it up to
32 bytes - MotS's filter tests the reported value, so both are worth running.
"""
import os
import re
import struct
import sys

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP

LIBRARY = "/mnt/retro-share/Files/Games-Library"
GAMES = {
    # pass-1 / pass-2 callbacks, the shared counter, the static mode array,
    # the CRT qsort and the comparator - see jklogic.h for what each does
    "df2":  dict(exe="JediKnightDF2/JK.EXE", p1=0x426030, p2=0x425e30, counter=0x55b3fc,
                 array=0x866cc0, qsort=0x513060, cmp=0x4249d0),
    "mots": dict(exe="JediKnightMotS/JKM.EXE", p1=0x429160, p2=0x428f60, counter=0x5b6bac,
                 array=0x948b60, qsort=0x56c510, cmp=0x427b00),
}
REC = 0x54            # one stored mode
SENTINEL = 0x0DEAD000
STACK = 0x00200000
SCRATCH = 0x01000000


def _load(path):
    pe = pefile.PE(path)
    data = open(path, "rb").read()
    base = pe.OPTIONAL_HEADER.ImageBase
    size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(base, size)
    mu.mem_write(base, data[:pe.OPTIONAL_HEADER.SizeOfHeaders])
    for s in pe.sections:
        mu.mem_write(base + s.VirtualAddress, data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData])
    mu.mem_map(STACK, 0x100000)
    mu.mem_map(SENTINEL, 0x1000)
    mu.mem_write(SENTINEL, b"\xf4" * 16)                     # hlt: where every call returns to
    mu.mem_map(SCRATCH, 0x10000)
    return mu


def _call(mu, fn, args):
    sp = STACK + 0x100000 - 0x100
    frame = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", a & 0xffffffff) for a in args)
    sp -= len(frame)
    mu.mem_write(sp, frame)
    mu.reg_write(UC_X86_REG_ESP, sp)
    mu.emu_start(fn, SENTINEL, count=5_000_000)
    return mu.reg_read(UC_X86_REG_EAX)


def _ddsd(w, h, pf, bpp, pitch):
    """A DDSURFACEDESC as EnumDisplayModes hands it over (only what the callbacks read)."""
    b = bytearray(0x6c)
    struct.pack_into("<IIIIi", b, 0, 0x6c, 0x100f, h, w, pitch)
    masks = {16: (0xf800, 0x07e0, 0x001f), 24: (0xff0000, 0xff00, 0xff), 32: (0xff0000, 0xff00, 0xff)}.get(bpp, (0, 0, 0))
    struct.pack_into("<IIIIIIII", b, 0x48, 0x20, pf, 0, bpp, masks[0], masks[1], masks[2], 0)
    return bytes(b)


def pitch_of(w, bpp, model="natural"):
    p = w * bpp // 8
    return (p + 31) & ~31 if model == "aligned32" else p


def read_fixture(path):
    """(pass-1 modes, pass-2 modes) as (w, h, pf flags, bpp) from a ddenum sequence."""
    p1, p2 = [], []
    for line in open(path):
        m = re.match(r"\s*(\d+)\s+(X?)\s+(\d+)x(\d+)\s+(\d+)bpp pf=0x([0-9a-f]+)", line)
        if m:
            rec = (int(m.group(3)), int(m.group(4)), int(m.group(6), 16), int(m.group(5)))
            (p1 if m.group(2) else p2).append(rec)
    return p1, p2


def run(game, p1, p2, pitch_model="natural", library=LIBRARY):
    """(count, [(modex, w, h, bpp)...]) - the list the game stores, in its order."""
    g = GAMES[game]
    mu = _load(os.path.join(library, g["exe"]))
    mu.mem_write(g["counter"], struct.pack("<I", 0))          # 0x425b9d: the counter is reset per device
    for pas, seq in ((1, p1), (2, p2)):
        for (w, h, pf, bpp) in seq:
            mu.mem_write(SCRATCH, _ddsd(w, h, pf, bpp, pitch_of(w, bpp, pitch_model)))
            _call(mu, g["p1"] if pas == 1 else g["p2"], [SCRATCH, 0])
    n = struct.unpack("<I", mu.mem_read(g["counter"], 4))[0]
    _call(mu, g["qsort"], [g["array"], n, REC, g["cmp"]])     # JK 0x422797 / MotS 0x4258da
    out = []
    for i in range(n):
        rec = bytes(mu.mem_read(g["array"] + i * REC, REC))
        modex, _aspect, w, h = struct.unpack_from("<IIII", rec, 0)
        out.append((modex, w, h, struct.unpack_from("<i", rec, 0x20)[0]))
    return n, out


def index_of(lst, w=1920, h=1080, bpp=16):
    for i, (mx, ww, hh, b) in enumerate(lst):
        if not mx and ww == w and hh == h and b == bpp:
            return i
    return -1


def main(argv):
    if len(argv) < 3 or argv[1] not in GAMES:
        print(__doc__)
        return 1
    game, fx = argv[1], argv[2]
    model = argv[3] if len(argv) > 3 else "natural"
    p1, p2 = read_fixture(fx)
    if game == "mots":
        p1 = [m for m in p2 if m[0] == 320]
    n, lst = run(game, p1, p2, model)
    for i, (mx, w, h, b) in enumerate(lst):
        print("%3d %s %4dx%-4d %2dbpp" % (i, "X" if mx else " ", w, h, b))
    print("RESULT game=%s stored=%d idx1080x16=%d" % (game, n, index_of(lst)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
