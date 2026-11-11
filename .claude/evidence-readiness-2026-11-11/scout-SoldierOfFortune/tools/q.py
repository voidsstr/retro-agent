"""q.py <pe> <cache> str <text> [ctx]   - xrefs to a NUL-terminated string
   q.py <pe> <cache> imm <hex> [ctx]    - instructions whose operands contain the immediate
   q.py <pe> <cache> at <hexva> [n]     - disassembly listing from an address
   q.py <pe> <cache> call <hexva> [ctx] - callers of a function address"""
import sys, pickle, re, bisect
pe_path, cache, mode, arg = sys.argv[1:5]
ctx = int(sys.argv[5]) if len(sys.argv) > 5 else 6
C = pickle.load(open(cache, 'rb'))
base, insns, secs = C['base'], C['insns'], C['sections']
addrs = [i[0] for i in insns]
data = open(pe_path, 'rb').read()
def off2va(o):
    for (n, va, vs, ro, rs) in secs:
        if ro <= o < ro + rs:
            return va + (o - ro)
def va2off(v):
    for (n, va, vs, ro, rs) in secs:
        if va <= v < va + rs:
            return ro + (v - va)
def show(idx, before, after):
    for j in range(max(0, idx - before), min(len(insns), idx + after + 1)):
        a, s, m, o = insns[j]
        mark = '>>' if j == idx else '  '
        extra = ''
        for h in re.findall(r'0x([0-9a-f]{8})', o):
            v = int(h, 16); fo = va2off(v)
            if fo is not None and secs[0][1] + secs[0][2] <= v:  # not in .text
                raw = data[fo:fo + 60].split(b'\0')[0]
                if raw and all(32 <= c < 127 for c in raw[:20]):
                    extra = '   ; "' + raw.decode('latin1')[:60] + '"'
        print(f'{mark} {a:08x}: {m} {o}{extra}')
    print('-' * 60)
def find_refs(v):
    pat = re.compile(r'0x0*%x\b' % v)
    return [k for k, i in enumerate(insns) if pat.search(i[3])]
if mode == 'str':
    needle = arg.encode('latin1')
    for m in re.finditer(re.escape(needle), data):
        o = m.start()
        if o > 0 and data[o - 1] != 0:
            continue
        if data[o + len(needle)] != 0:
            continue
        v = off2va(o)
        if v is None: continue
        refs = find_refs(v)
        print(f'string {arg!r} at va {v:08x}, {len(refs)} refs')
        for k in refs:
            show(k, ctx, ctx)
elif mode == 'imm':
    v = int(arg, 16)
    refs = find_refs(v)
    print(len(refs), 'refs to', hex(v))
    for k in refs[:60]:
        show(k, ctx, ctx)
elif mode == 'at':
    v = int(arg, 16)
    k = bisect.bisect_left(addrs, v)
    show(k, 0, ctx)
elif mode == 'call':
    v = int(arg, 16)
    pat = re.compile(r'^0x0*%x$' % v)
    refs = [k for k, i in enumerate(insns) if i[2] in ('call', 'jmp') and pat.match(i[3])]
    print(len(refs), 'callers of', hex(v))
    for k in refs[:80]:
        show(k, ctx, 2)
