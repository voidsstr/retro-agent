import sys, struct, re
sys.path.insert(0, '.')
from xref import Img

def find_global_after_call(lines):
    # after the first 'call', track eax->reg moves, then first store to [0x...]
    seen_call = False
    holders = {'eax'}
    for l in lines:
        mn = l.split()[1]
        ops = l.split(None, 2)[2] if len(l.split(None, 2)) > 2 else ''
        if not seen_call:
            if mn == 'call':
                seen_call = True
            continue
        m = re.match(r'(\w+), (\w+)$', ops)
        if mn == 'mov' and m and m.group(2) in holders:
            holders.add(m.group(1))
            continue
        m2 = re.match(r'dword ptr \[0x([0-9a-f]+)\], (\w+)', ops)
        if mn == 'mov' and m2 and m2.group(2) in holders:
            return int(m2.group(1), 16)
        if mn == 'call':
            return None
    return None

def analyze(path, cvar='r_displayRefresh', ctx=10):
    im = Img(path)
    print('#####', path, 'base', hex(im.base))
    svas = im.find_string(cvar)
    print('string VA(s):', [hex(v) for v in svas])
    globals_ = set()
    for sva in svas:
        for r in im.refs_imm(sva):
            ins = im.insn_containing(r)
            if not ins: continue
            print(' ref to string at', hex(ins.address), ins.mnemonic, ins.op_str)
            lines = im.dis(ins.address, 80)
            for l in lines[:12]:
                print('   ', l)
            g = find_global_after_call(lines[:20])
            if g: globals_.add(g)
    for g in sorted(globals_):
        print(' global cvar ptr at', hex(g))
        for r in im.refs_imm(g):
            ins = im.insn_containing(r)
            if not ins: continue
            print('  use at', hex(ins.address), ins.mnemonic, ins.op_str)
            for l in im.dis(ins.address, 64)[:ctx]:
                print('     ', l)
    return im, globals_

if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)
