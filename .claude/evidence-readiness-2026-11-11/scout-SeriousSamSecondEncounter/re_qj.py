import pefile, capstone, struct, sys
p = sys.argv[1]
pe = pefile.PE(p)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(p, 'rb').read()
def off2va(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return base + s.VirtualAddress + (off - s.PointerToRawData)
def va2off(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
strs = {}
for name in [b"+quickjoin\0", b"+connect\0", b"ETRSCommand line connection: '%s%s'", b"Command line connection"]:
    i = -1
    while True:
        i = data.find(name, i + 1)
        if i < 0: break
        strs.setdefault(name, []).append((i, off2va(i)))
for k, v in strs.items():
    print(k, [(hex(a), hex(b)) for a, b in v])
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
tva = base + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
# find code refs (push imm32 / mov reg, imm32) to the string VAs
targets = {}
for k, v in strs.items():
    for off, va in v:
        targets[va] = k
        # ETRS strings: the translatable text starts after 'ETRS'
        targets[va + 4] = k + b' (+4)'
refs = []
for i in range(len(code) - 4):
    imm = struct.unpack_from('<I', code, i)[0]
    if imm in targets:
        refs.append((tva + i, targets[imm]))
for va, k in refs:
    print('ref', hex(va), k)
