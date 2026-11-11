import pefile, capstone, sys
p, start, length = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
pe = pefile.PE(p); base = pe.OPTIONAL_HEADER.ImageBase; data = open(p,'rb').read()
def va2off(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
off = va2off(start)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for ins in md.disasm(data[off:off+length], start):
    print(f"{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}")
