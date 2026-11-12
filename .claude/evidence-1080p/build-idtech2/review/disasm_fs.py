import pefile, capstone, sys
def dis(path, va_from, va_to, note):
    d = open(path, "rb").read()
    pe = pefile.PE(data=d, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    off = pe.get_offset_from_rva(va_from - base)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    print("=====", note)
    for ins in md.disasm(d[off:off + (va_to - va_from)], va_from):
        print("%08x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
L = "/mnt/retro-share/Files/Games-Library"
dis(L + "/SiNGold/sin.exe", 0x441ef0, 0x441fc2, "SiN FS_AddGameDirectory")
dis(L + "/SoldierOfFortune/SoF.exe", 0x200281f0, 0x200282b8, "SoF FS_AddGameDirectory")
