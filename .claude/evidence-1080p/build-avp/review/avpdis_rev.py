import struct, sys, capstone
exe = open("/mnt/retro-share/Files/Games-Library/AliensVsPredator/avp.exe", "rb").read()
pe = struct.unpack_from("<I", exe, 0x3c)[0]
nsec = struct.unpack_from("<H", exe, pe + 6)[0]
opt = struct.unpack_from("<H", exe, pe + 20)[0]
base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + 40 * i
    name = exe[o:o+8].rstrip(b"\0").decode()
    vsz, va, rsz, raw = struct.unpack_from("<IIII", exe, o + 8)
    secs.append((name, base + va, vsz, rsz, raw))
def off(v):
    for n, va, vsz, rsz, raw in secs:
        if va <= v < va + max(vsz, rsz): return raw + (v - va)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def dis(v, n):
    o = off(v)
    for ins in md.disasm(exe[o:o+n], v):
        print("  %08x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
def cstr(v):
    o = off(v); return exe[o:exe.index(b"\0", o)]
if __name__ == "__main__":
    for a in sys.argv[1:]:
        v, n = a.split(":")
        print("##", v); dis(int(v, 16), int(n))
