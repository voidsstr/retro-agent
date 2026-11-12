"""Reviewer check: byte-level, relocation-aware diff of the patched ui.qvm
against pak8's OWN vm/ui.qvm (extracted read-only from the staged pak8.pk3).

q3asm's map names functions and globals but NO local labels ($N: string
literals, function-local statics), so a literal address cannot be resolved by
nearest symbol. The literal pool is mapped instead by its unchanged PREFIX
(files linked before ui_video: same address) and SUFFIX (files after it:
shifted by the pool's growth); the window between is ui_video's own strings.
Rule: every instruction outside ui_video's functions and every data word
outside ui_video's data must be identical, or an address whose new value is
exactly the relocated old one."""
import bisect, io, os, struct, sys, zipfile
P = "/home/voidsstr/development/retro-agent/.claude/worktrees/lcd1080-patches/provisioning/patches/q3-baseq3-ui"
sys.path.insert(0, P)
import apply
B = os.path.expanduser("~/.retro-fleet/patch-out/q3-baseq3-ui/_build")
pak8 = zipfile.ZipFile("/mnt/retro-share/Files/Games-Library/Quake3-TeamArena/baseq3/pak8.pk3").read("vm/ui.qvm")
assert open(B + "/stock/code/q3_ui/vm/ui.qvm", "rb").read() == pak8, "stock build != pak8"
patched = zipfile.ZipFile(io.BytesIO(open(os.path.expanduser(
    "~/.retro-fleet/patch-out/q3-baseq3-ui/baseq3/zz-fleetres-ui.pk3"), "rb").read())).read("vm/ui.qvm")

def load(qvm, mp):
    h = apply.qvm_header(qvm)
    base = {1: 0, 2: h["dataLength"], 3: h["dataLength"] + h["litLength"]}
    code, dat = {}, {}
    for line in open(mp).read().splitlines():
        p = line.split()
        if len(p) != 3:
            continue
        seg, a, name = int(p[0]), int(p[1], 16), p[2]
        if seg == 0:
            if 0 <= a < h["instructionCount"]:
                code[name] = a
        else:
            dat[name] = a + base[seg]
    d = qvm[h["dataOffset"]:h["dataOffset"] + h["dataLength"]]
    lit = qvm[h["dataOffset"] + h["dataLength"]:h["dataOffset"] + h["dataLength"] + h["litLength"]]
    return h, code, dat, d, lit

hs, cs, ds, sdat, slit = load(pak8, B + "/stock/code/q3_ui/vm/ui.map")
hp, cp, dp, pdat, plit = load(patched, B + "/patched/code/q3_ui/vm/ui.map")
pre = 0
while pre < min(len(slit), len(plit)) and slit[pre] == plit[pre]:
    pre += 1
suf = 0
while suf < min(len(slit), len(plit)) - pre and slit[-1 - suf] == plit[-1 - suf]:
    suf += 1
lit_delta = len(plit) - len(slit)
print("literal pool: stock %d B, patched %d B (+%d); common prefix %d B, common suffix %d B; "
      "changed window stock [%d,%d) = ui_video's strings" % (len(slit), len(plit), lit_delta, pre, suf,
                                                              pre, len(slit) - suf))
print("  window, stock  : %r" % [s for s in slit[pre:len(slit) - suf].split(b"\0") if s][:8])
print("  window, patched: %r" % [s for s in plit[pre:len(plit) - suf].split(b"\0") if s][:12])

video = set(apply.procs_in_asm(open(B + "/stock/code/q3_ui/vm/ui_video.asm").read())) | \
        set(apply.procs_in_asm(open(B + "/patched/code/q3_ui/vm/ui_video.asm").read()))
cs_sorted = sorted((a, n) for n, a in cs.items()); ckeys = [a for a, _ in cs_sorted]
ds_sorted = sorted((a, n) for n, a in ds.items()); dkeys = [a for a, _ in ds_sorted]
DL, LL = hs["dataLength"], hs["litLength"]

def relocate(v, space):
    if space == "code":
        # a code address: a function START follows its function wherever it
        # went (a call INTO ui_video from another file); a label INSIDE a
        # function (jump target, switch table) keeps its offset - but only in
        # a function outside ui_video, whose body is unchanged
        i = bisect.bisect_right(ckeys, v) - 1
        if i < 0 or cs_sorted[i][1] not in cp:
            return None
        name, start = cs_sorted[i][1], cs_sorted[i][0]
        if v == start:
            return cp[name]
        return None if name in video else cp[name] + v - start
    if DL <= v < DL + LL:                                  # literal pool
        o = v - DL
        if o < pre:
            return v
        if o >= LL - suf:
            return v + lit_delta
        return None                                        # ui_video's own strings
    i = bisect.bisect_right(dkeys, v) - 1                  # data / bss: named globals
    if i < 0 or ds_sorted[i][1] not in dp:
        return None
    return dp[ds_sorted[i][1]] + v - ds_sorted[i][0]

si, pi = apply.qvm_decode(pak8), apply.qvm_decode(patched)
checked = relocated = 0
bad = []
for k, (start, name) in enumerate(cs_sorted):
    if name in video:
        continue
    end = cs_sorted[k + 1][0] if k + 1 < len(cs_sorted) else len(si)
    ps = cp[name]
    for off in range(end - start):
        (op, arg), (op2, arg2) = si[start + off], pi[ps + off]
        checked += 1
        if op != op2:
            bad.append((name, off, "opcode")); continue
        if arg == arg2:
            continue
        if op in apply.BRANCHES:
            ok = arg - start == arg2 - ps
        elif op == apply.OP["CONST"]:
            nxt = si[start + off + 1][0]
            ok = relocate(arg, "code" if nxt in (apply.OP["CALL"], apply.OP["JUMP"]) else "data") == arg2
        else:
            ok = False
        if ok:
            relocated += 1
        else:
            bad.append((name, off, "%s %d -> %d" % (apply.OP_NAMES[op], arg, arg2)))
print("code: %d instructions outside ui_video compared, %d operands differ only by relocation, "
      "UNEXPLAINED %d" % (checked, relocated, len(bad)))
for b in bad[:20]:
    print("   ", b)

# data segment, word by word, outside ui_video's own data (its first global to the next file's)
v_lo = min(ds[n] for n in ("driverinfo_artlist", "s_drivers", "s_ivo_templates"))
v_hi = ds["randSeed"]
assert dp["randSeed"] == v_hi and len(sdat) == len(pdat)
same = reloc = 0
dbad = []
for j in range(0, len(sdat), 4):
    if v_lo <= j < v_hi:
        continue
    v, v2 = struct.unpack_from("<i", sdat, j)[0], struct.unpack_from("<i", pdat, j)[0]
    if v == v2:
        same += 1
    elif relocate(v, "data") == v2 or relocate(v, "code") == v2:
        reloc += 1
    else:
        dbad.append((j, v, v2))
print("data: %d words outside ui_video's data [0x%x,0x%x): %d identical, %d relocated pointers, "
      "UNEXPLAINED %d" % (same + reloc + len(dbad), v_lo, v_hi, same, reloc, len(dbad)))
for b in dbad[:10]:
    print("    data word @0x%x %d -> %d" % b)
