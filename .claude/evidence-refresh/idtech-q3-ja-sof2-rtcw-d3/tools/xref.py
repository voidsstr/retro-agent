import sys, struct, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.path = path
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = False
        self.secs = []
        for s in self.pe.sections:
            va = self.base + s.VirtualAddress
            data = s.get_data()
            self.secs.append((s.Name.rstrip(b'\0').decode(errors='replace'), va, data, s.Characteristics))
        self.imports = {}
        if hasattr(self.pe, 'DIRECTORY_ENTRY_IMPORT'):
            for e in self.pe.DIRECTORY_ENTRY_IMPORT:
                for i in e.imports:
                    if i.name:
                        self.imports[i.name.decode()] = i.address
    def code_secs(self):
        return [s for s in self.secs if s[3] & 0x20000000]
    def find_bytes(self, pat):
        out = []
        for name, va, data, ch in self.secs:
            i = data.find(pat)
            while i >= 0:
                out.append(va + i)
                i = data.find(pat, i + 1)
        return out
    def find_string(self, s):
        return self.find_bytes(s.encode() + b'\0')
    def read(self, va, n):
        for name, sva, data, ch in self.secs:
            if sva <= va < sva + len(data):
                off = va - sva
                return data[off:off+n]
        return b''
    def refs_imm(self, val):
        """all places the 4-byte little endian value appears inside code sections"""
        pat = struct.pack('<I', val)
        out = []
        for name, va, data, ch in self.code_secs():
            i = data.find(pat)
            while i >= 0:
                out.append(va + i)
                i = data.find(pat, i + 1)
        return out
    def dis(self, start, n, mark=None):
        code = self.read(start, n)
        lines = []
        for ins in self.md.disasm(code, start):
            m = '  <==' if mark is not None and ins.address <= mark < ins.address + ins.size else ''
            lines.append('%08x: %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, m))
        return lines
    def insn_containing(self, va, back=64):
        """try to find an instruction boundary that contains va by disassembling from va-back"""
        for b in range(back, 0, -1):
            code = self.read(va - b, b + 16)
            for ins in self.md.disasm(code, va - b):
                if ins.address <= va < ins.address + ins.size:
                    # check that the disasm stream is aligned: start points reached cleanly
                    return ins
                if ins.address > va:
                    break
        return None

def calls_to(im, target):
    out = []
    for name, va, data, ch in im.code_secs():
        i = data.find(b'\xe8')
        while i >= 0:
            if i + 5 <= len(data):
                rel = int.from_bytes(data[i+1:i+5], 'little', signed=True)
                if va + i + 5 + rel == target:
                    out.append(va + i)
            i = data.find(b'\xe8', i + 1)
    return out

def aligned_dis(im, site, before=0x100, after=0x80):
    """disassemble a window around site, choosing a start that lands exactly on site"""
    best = None
    for s in range(site - before, site - before + 16):
        addrs = [ins.address for ins in im.md.disasm(im.read(s, site - s + 16), s)]
        if site in addrs:
            best = s
            break
    if best is None:
        best = site
    return im.dis(best, site - best + after)
