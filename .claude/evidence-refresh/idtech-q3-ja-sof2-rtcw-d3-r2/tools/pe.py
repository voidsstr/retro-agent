#!/usr/bin/env python3
"""Small static-analysis helper for 32-bit PE files (capstone + pefile).

  pe.py FILE info
  pe.py FILE str NEEDLE              every VA where the ASCII string occurs (case-insensitive)
  pe.py FILE xref VA                 code locations that use VA as an imm32 or disp32
  pe.py FILE dis VA [N] [BACK]       disassemble N instructions from VA (BACK bytes before)
  pe.py FILE iat NAME                IAT slot VA of an imported function (case-insensitive)
  pe.py FILE callsiat NAME           every 'call/jmp dword ptr [IAT]' to that import
"""
import sys, struct, re
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


class PE:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path, fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = open(path, 'rb').read()
        self.secs = []
        for s in self.pe.sections:
            va = self.base + s.VirtualAddress
            self.secs.append((s.Name.rstrip(b'\0').decode('latin-1'), va,
                              max(s.Misc_VirtualSize, s.SizeOfRawData),
                              s.PointerToRawData, s.SizeOfRawData, s.Characteristics))
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = False

    def off2va(self, off):
        for n, va, vs, ro, rs, ch in self.secs:
            if ro <= off < ro + rs:
                return va + (off - ro)
        return None

    def va2off(self, va):
        for n, sva, vs, ro, rs, ch in self.secs:
            if sva <= va < sva + rs:
                return ro + (va - sva)
        return None

    def code_secs(self):
        return [s for s in self.secs if s[5] & 0x20000000]   # IMAGE_SCN_MEM_EXECUTE

    def imports(self):
        out = {}
        if hasattr(self.pe, 'DIRECTORY_ENTRY_IMPORT'):
            for e in self.pe.DIRECTORY_ENTRY_IMPORT:
                for imp in e.imports:
                    if imp.name:
                        out[(e.dll.decode('latin-1').lower(), imp.name.decode('latin-1'))] = imp.address
        return out

    def iat(self, name):
        name = name.lower()
        return [(d, n, a) for (d, n), a in self.imports().items() if n.lower() == name]

    def find_str(self, needle):
        res = []
        nb = needle.encode('latin-1')
        for m in re.finditer(re.escape(nb), self.data, re.I):
            va = self.off2va(m.start())
            if va is not None:
                # print the whole NUL-terminated string it sits in
                s = m.start()
                while s > 0 and self.data[s - 1] not in (0,):
                    s -= 1
                e = self.data.index(b'\0', m.start())
                res.append((self.off2va(s), va, self.data[s:e]))
        return res

    def xref(self, va):
        pat = struct.pack('<I', va)
        res = []
        for n, sva, vs, ro, rs, ch in self.code_secs():
            blob = self.data[ro:ro + rs]
            for m in re.finditer(re.escape(pat), blob):
                res.append(sva + m.start())
        return res

    def dis(self, va, n=30, back=0):
        start = va - back
        off = self.va2off(start)
        code = self.data[off:off + n * 16 + back]
        out = []
        for i in self.md.disasm(code, start):
            out.append('%08x: %-8s %s' % (i.address, i.mnemonic, i.op_str))
            if len(out) >= n + (back // 2 if back else 0):
                break
        return out

    def insn_at_containing(self, ref, maxback=12, target=None):
        """Find the instruction that STARTS at ref-k, whose bytes include ref and
        whose operands name the target value; the LONGEST such decode wins
        (a short misaligned decode like 'and eax, imm32' loses to 'jmp [imm32]')."""
        best = None
        for k in range(1, maxback):
            s = ref - k
            off = self.va2off(s)
            if off is None:
                continue
            for i in self.md.disasm(self.data[off:off + 16], s):
                if i.address == s and s + i.size >= ref + 4:
                    if target is None or ('%#x' % target) in i.op_str:
                        if best is None or k > best[0]:
                            best = (k, i)
                break
        return best[1] if best else None


def main():
    f, cmd = sys.argv[1], sys.argv[2]
    p = PE(f)
    if cmd == 'info':
        print('image base %#x' % p.base)
        for s in p.secs:
            print('  %-8s va=%#x vsize=%#x raw=%#x rsize=%#x ch=%#x' % s)
    elif cmd == 'str':
        for s, va, txt in p.find_str(sys.argv[3]):
            print('%#x (match %#x): %r' % (s, va, txt[:200]))
    elif cmd == 'xref':
        va = int(sys.argv[3], 16)
        for r in p.xref(va):
            i = p.insn_at_containing(r, target=va)
            print('%#x  %s' % (r, ('%08x: %s %s' % (i.address, i.mnemonic, i.op_str)) if i else '?'))
    elif cmd == 'dis':
        va = int(sys.argv[3], 16)
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 30
        back = int(sys.argv[5]) if len(sys.argv) > 5 else 0
        print('\n'.join(p.dis(va, n, back)))
    elif cmd == 'iat':
        for d, n, a in p.iat(sys.argv[3]):
            print('%s!%s IAT %#x' % (d, n, a))
    elif cmd == 'callsiat':
        for d, n, a in p.iat(sys.argv[3]):
            print('== %s!%s IAT %#x' % (d, n, a))
            for r in p.xref(a):
                i = p.insn_at_containing(r, target=a)
                if i:
                    print('   %08x: %s %s' % (i.address, i.mnemonic, i.op_str))
    elif cmd == 'relcalls':
        pass
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()


def rel_calls(p, target):
    """Every E8/E9 rel32 whose destination is target."""
    out = []
    for n, sva, vs, ro, rs, ch in p.code_secs():
        blob = p.data[ro:ro + rs]
        for m in re.finditer(b'[\xe8\xe9]', blob):
            i = m.start()
            if i + 5 > len(blob):
                continue
            rel = struct.unpack_from('<i', blob, i + 1)[0]
            src = sva + i
            if (src + 5 + rel) & 0xffffffff == target:
                out.append(src)
    return out


if __name__ == '__main__' and len(sys.argv) > 2 and sys.argv[2] == 'relcalls':
    _p = PE(sys.argv[1])
    for _s in rel_calls(_p, int(sys.argv[3], 16)):
        print('%#x' % _s)
