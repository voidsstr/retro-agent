import re, sys
def parse(path):
    lines = open(path).read().splitlines()
    # string literal / label definitions: "LABELV $123" followed by "byte 1 N" lines (lit)
    procs, cur, lits, seg = {}, None, {}, None
    labeldef = None
    for l in lines:
        t = l.strip()
        if not t or t.startswith(';') or t.startswith('line ') or t.startswith('file '):
            continue
        if t in ('code', 'data', 'lit', 'bss'):
            seg = t; labeldef = None
            continue
        if t.startswith('LABELV $') and seg == 'lit':
            labeldef = t.split()[1]; lits[labeldef] = []; continue
        if labeldef and t.startswith('byte 1 '):
            lits[labeldef].append(int(t.split()[2])); continue
        labeldef = None if seg != 'lit' else labeldef
        if t.startswith('proc '):
            cur = t.split()[1]; procs[cur] = [t]; continue
        if t.startswith('endproc '):
            procs[cur].append(t); cur = None; continue
        if cur:
            procs[cur].append(t)
    return procs, {k: bytes(v).split(b'\0')[0] for k, v in lits.items()}

def norm(body, lits):
    m, out = {}, []
    for t in body:
        def sub(mo):
            name = mo.group(0)
            if name in lits:
                return 'LIT<%r>' % lits[name]
            if name not in m:
                m[name] = '$L%d' % len(m)
            return m[name]
        out.append(re.sub(r'\$\d+', sub, t))
    return out

sp, sl = parse(sys.argv[1]); pp, pl = parse(sys.argv[2])
same, diff = [], []
for name in sorted(set(sp) | set(pp)):
    a = norm(sp.get(name, []), sl); b = norm(pp.get(name, []), pl)
    (same if a == b and a else diff).append(name)
print('identical in ui_video.asm:', same)
print('changed / new / removed  :', diff)
print('only in stock :', sorted(set(sp) - set(pp)))
print('only in patched:', sorted(set(pp) - set(sp)))
print('lit strings removed:', sorted(set(sl.values()) - set(pl.values())))
print('lit strings added  :', sorted(set(pl.values()) - set(sl.values())))
