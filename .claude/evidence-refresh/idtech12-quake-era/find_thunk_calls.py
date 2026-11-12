#!/usr/bin/env python3
"""Find `call <thunk>` sites where thunk = `jmp [IAT slot]` for an imported
function, by scanning for E8 rel32 whose target is the thunk. Prints the
preceding instructions (decoded backwards from a safe window). Read-only."""
import sys, struct, pefile, capstone

def main(path, needle, back=40):
    p = pefile.PE(path)
    base = p.OPTIONAL_HEADER.ImageBase
    data = open(path, 'rb').read()
    slots = {}
    for e in getattr(p, 'DIRECTORY_ENTRY_IMPORT', []):
        for i in e.imports:
            if i.name and needle.encode() in i.name:
                slots[i.address] = i.name.decode()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    thunks = {}
    for slot, nm in slots.items():
        pat = b'\xff\x25' + struct.pack('<I', slot)
        k = data.find(pat)
        while k >= 0:
            thunks[base + p.get_rva_from_offset(k)] = nm
            k = data.find(pat, k + 1)
        # direct call [slot]
        pat2 = b'\xff\x15' + struct.pack('<I', slot)
        k = data.find(pat2)
        while k >= 0:
            thunks.setdefault(('direct', base + p.get_rva_from_offset(k)), nm)
            k = data.find(pat2, k + 1)
    print('thunks:', {hex(a) if isinstance(a, int) else (a[0], hex(a[1])): n for a, n in thunks.items()})
    text = [s for s in p.sections if s.Characteristics & 0x20000000]
    sites = []
    for s in text:
        sd = s.get_data()
        sva = base + s.VirtualAddress
        for k in range(len(sd) - 5):
            if sd[k] == 0xE8:
                rel = struct.unpack('<i', sd[k+1:k+5])[0]
                tgt = sva + k + 5 + rel
                if tgt in thunks:
                    sites.append((sva + k, thunks[tgt], s))
    for a, nm, s in thunks.items() if False else []:
        pass
    for key, nm in thunks.items():
        if isinstance(key, tuple):
            sites.append((key[1], nm, None))
    for va, nm, s in sites:
        print('==== call %s at 0x%x' % (nm, va))
        start = va - back * 3
        off = p.get_offset_from_rva(start - base)
        blob = data[off: off + (va - start) + 6]
        # try successive start alignments until one lands exactly on the call
        for shift in range(0, 16):
            ins = list(md.disasm(blob[shift:], start + shift))
            if any(x.address == va for x in ins):
                for x in ins:
                    if x.address > va: break
                    print('   0x%x: %s %s' % (x.address, x.mnemonic, x.op_str))
                break

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 40)
