"""Static helper: linear-sweep disassembly of a PE's executable sections into a pickle cache."""
import sys, site
sys.path.append(site.getusersitepackages())
import pefile, capstone, pickle
path, cache = sys.argv[1], sys.argv[2]
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
insns = []
for s in pe.sections:
    if not (s.Characteristics & 0x20000000):
        continue
    code = s.get_data()
    va = base + s.VirtualAddress
    off = 0
    n = len(code)
    while off < n:
        last = off
        for (addr, size, mn, ops) in md.disasm_lite(code[off:], va + off):
            insns.append((addr, size, mn, ops))
            last = addr - va + size
        off = last if last > off else off + 1
secs = [(s.Name.rstrip(b'\0').decode('latin1'), base + s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
pickle.dump({'base': base, 'insns': insns, 'sections': secs}, open(cache, 'wb'))
print(path.split('/')[-1], len(insns), 'instructions, base', hex(base), secs)
