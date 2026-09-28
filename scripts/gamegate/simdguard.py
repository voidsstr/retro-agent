#!/usr/bin/env python3
"""Is a binary's SSE2 (or SSE) a FLOOR, or a runtime-dispatched fast path?

    python3 scripts/gamegate/simdguard.py <pe> [<pe> ...] [--class sse2|sse] [--json] [-v]

exit 0  every way into SIMD code is behind a CPU-feature decision (or there is
        no SIMD at all) - NOT a floor
exit 1  at least one UNGUARDED way in: ordinary code calls SIMD code directly -
        a floor, declare it in requires.json
exit 2  could not analyse (missing pefile/capstone, not a PE)

WHY THIS EXISTS. isascan.py counts instructions, and SCHEMA.md says a count is
"the start of the answer, never the end". Halo 1.10 is what happens when the
start is taken for the end: halo.exe carries 501 SSE2 instructions (scalar
double mulsd/addsd/movlpd, the look of an /arch:SSE2 build) and 19 cpuid sites,
the conservative reading won, and requires.json refused Halo on every box
without SSE2 (.124's Athlon XP, a Pentium III). Traced by hand (2026-09-28)
every one of those 501 sits behind a CPU decision:

  * the MSVC 7.1 CRT's SSE2 libm (pow/exp/log/sin/cos...) - each entry point
    opens with `cmp dword ptr [_sse2_mathfcns], 0 / je <x87 path>`, and that
    flag is written only next to a `cpuid` that tested EDX bit 26;
  * D3DX9's per-CPU function tables - the SSE2 table is filled only after
    IsProcessorFeaturePresent(PF_3DNOW) said no and a cpuid helper said SSE2;
  * one-instruction SEH probes ("does the OS save XMM state?").

So this answers the question isascan cannot: for each region of SIMD code, HOW
DOES CONTROL GET IN? Every external way in is classified:

  flag_gate     the path passes a `cmp [G], 0 / jcc` on a global G that is
                written beside a cpuid (the CRT _sse2_mathfcns shape)
  cpu_decision  the way in is taken within a few dozen instructions after a
                cpuid, a call to a cpuid helper, or IsProcessorFeaturePresent
  dispatch_only the calling code is itself reached only through guarded ways
                (a per-CPU table filler, a wrapper stored in such a table)
  seh_probe     a data pointer into the body of a <=2-instruction SIMD region:
                an SEH scope table of an OS-support probe
  ignored       linear-sweep artefacts: jump-table data decoded as code, ASCII
                or a mid-instruction value that happens to look like an
                address, a branch that lands past the SIMD in its block
  unreferenced  the caller is code nothing reaches: no call, jump, pointer,
                export or entry point names it (statically linked library
                code the linker kept - Keystone.dll has three such routines)
  UNGUARDED     everything else - a direct call from ordinary code, a
                function pointer (vtable, callback) that no CPU decision chose,
                an export, the image entry point

It is deliberately conservative in the UNGUARDED direction: a vtable pointer to
SIMD code counts against the title, so a C++ build compiled /arch:SSE2 still
reads as a floor (UT99 469e's Core.dll is the regression control). A SIMD
region it finds no way into at all is listed as `no_entry` (with -v) - usually
the inside of a routine entered elsewhere, never proof of anything. It is a
disassembly heuristic, not a proof: READ THE WAYS it prints (-v) before
changing a requires.json in either direction.

The analysis core (analyse()) takes plain instruction tuples, so the rules are
unit-tested without a PE: tests/python/test_simdguard_halo.py.
"""
import bisect
import collections
import json
import re
import struct
import sys

# --------------------------------------------------------------------------
# instruction classes
# --------------------------------------------------------------------------
_DOUBLE_FP = set("""addpd addsd subpd subsd mulpd mulsd divpd divsd sqrtpd sqrtsd
maxpd maxsd minpd minsd andpd andnpd orpd xorpd cmppd cmpsd comisd ucomisd movapd
movupd movhpd movlpd movsd movmskpd shufpd unpckhpd unpcklpd cmpeqsd cmpltsd
cmplesd cmpunordsd cmpneqsd cmpnltsd cmpnlesd cmpordsd cmpeqpd cmpltpd cmplepd
cmpunordpd cmpneqpd cmpnltpd cmpnlepd cmpordpd""".split())
_CONV2 = set("""cvtdq2pd cvtdq2ps cvtpd2dq cvtpd2pi cvtpd2ps cvtpi2pd cvtps2dq
cvtps2pd cvtsd2si cvtsd2ss cvtsi2sd cvtss2sd cvttpd2dq cvttpd2pi cvttps2dq
cvttsd2si""".split())
_INT2 = set("""movdqa movdqu movq2dq movdq2q maskmovdqu movntdq movntpd movnti
lfence mfence clflush pshufd pshufhw pshuflw pslldq psrldq punpckhqdq punpcklqdq
paddq psubq pmuludq""".split())
_SSE1_XMM = set("""addps addss subps subss mulps mulss divps divss sqrtps sqrtss
rsqrtps rsqrtss rcpps rcpss maxps maxss minps minss andps andnps orps xorps cmpps
cmpss comiss ucomiss movaps movups movhps movlps movhlps movlhps movss movmskps
shufps unpckhps unpcklps cvtpi2ps cvtps2pi cvtsi2ss cvtss2si cvttps2pi cvttss2si
movntps ldmxcsr stmxcsr""".split())
_SSE1_MMX = set("""pshufw pmaxsw pminsw pmaxub pminub pavgb pavgw pmovmskb
pmulhuw psadbw pextrw pinsrw maskmovq movntq sfence prefetchnta prefetcht0
prefetcht1 prefetcht2""".split())


def simd_class(mnemonic, op_str):
    """'sse2' | 'sse' | None for one decoded instruction (Intel syntax).
    The MMX/SSE2 shared integer mnemonics are told apart by their OPERANDS:
    pxor mm0 is MMX, pxor xmm0 is SSE2. movsd/cmpsd without an xmm operand are
    the string instructions."""
    m = mnemonic
    xmm = "xmm" in op_str
    if m in _DOUBLE_FP:
        return "sse2" if xmm else None
    if m in _CONV2 or m in _INT2:
        return "sse2"
    if m.startswith("p") and xmm:
        return "sse2"
    if m in ("movd", "movq") and xmm:
        return "sse2"
    if m in _SSE1_XMM or m in _SSE1_MMX:
        return "sse"
    return None


# --------------------------------------------------------------------------
# analysis core - pure logic over instruction tuples
# --------------------------------------------------------------------------
_HEX = re.compile(r"0x[0-9a-f]+")
_JCC = re.compile(r"^j(?!mp$)[a-z]+$")
_FLAG_CMP = re.compile(r"^dword ptr \[(0x[0-9a-f]+)\], 0$")
_FLAG_WRITE = re.compile(r"^dword ptr \[(0x[0-9a-f]+)\], ")
_JUMPTABLE = re.compile(r"^dword ptr \[e[a-z]{2}\*4 \+ 0x[0-9a-f]+\]$")

WINDOW = 48          # instructions a cpu decision may precede the way in by
OUTSIDE = ("data", "entry", "export")   # ways in with no source instruction
MAX_DEPTH = 8        # callers walked back for dispatch_only


class Image:
    """What the analysis needs from a PE, without the PE.

    ins        [(addr, size, mnemonic, op_str)] in address order (code only)
    data       [(va, dword_value, raw4)] every aligned dword of the non-code
               sections (for pointers stored in tables)
    ipfp_slots IAT slots of kernel32!IsProcessorFeaturePresent
    roots      [(va, kind)] ways in that no instruction or pointer shows: the
               entry point, TLS callbacks, exports ('entry' / 'export').
               These are called from OUTSIDE, so they are never guarded.
    code       [(va, bytes)] the raw executable sections, to find the switch
               jump tables MSVC puts in .text (optional)
    """

    def __init__(self, ins, data=(), ipfp_slots=(), roots=(), code=()):
        self.roots = list(roots)
        self.code = list(code)
        self.ins = sorted(ins)
        self.addrs = [i[0] for i in self.ins]
        self.index = {a: k for k, a in enumerate(self.addrs)}
        self.data = list(data)
        self.ipfp_slots = set(ipfp_slots)
        if self.ins:
            self.lo = self.ins[0][0]
            self.hi = self.ins[-1][0] + self.ins[-1][1]
        else:
            self.lo = self.hi = 0


def _blocks(img):
    """Split code at padding: a new block starts after ret/int3."""
    starts = [img.ins[0][0]] if img.ins else []
    for k in range(1, len(img.ins)):
        if img.ins[k - 1][2] in ("ret", "int3") and img.ins[k][2] != "int3":
            starts.append(img.ins[k][0])
    return starts


def analyse(img, cls="sse2", debug=None):
    """Return a report dict; report['unguarded'] is the list that makes a floor."""
    ins = img.ins
    if not ins:
        return {"class": cls, "count": 0, "regions": 0, "entries": {},
                "unguarded": [], "no_entry": [], "ways": [], "cpuid_sites": 0,
                "verdict": "none"}
    starts = _blocks(img)

    def block_of(a):
        return starts[bisect.bisect_right(starts, a) - 1]

    def block_end(b):
        j = bisect.bisect_right(starts, b)
        return starts[j] if j < len(starts) else img.hi

    # --- switch tables MSVC keeps in .text are DATA, not SIMD -----------
    # `jmp dword ptr [reg*4 + T]` names a table of code addresses at T; the
    # `movzx reg, byte ptr [reg + I]` before it names a byte index table.
    # Linear sweep decodes both as instructions - Keystone.dll's "movups
    # [edx], xmm4" is the address 0x1022110f - so SIMD found there is dropped.
    def code_byte(va):
        for base_va, blob in img.code:
            if base_va <= va < base_va + len(blob):
                return blob[va - base_va]
        return None

    def code_dword(va):
        bs = [code_byte(va + i) for i in range(4)]
        return None if None in bs else bs[0] | bs[1] << 8 | bs[2] << 16 | bs[3] << 24

    table_ranges = []
    if img.code:
        tstarts = set()
        for a, _s, m, o in ins:
            mm = _JUMPTABLE.match(o or "")
            if m == "jmp" and mm:
                tstarts.add(int(_HEX.findall(o)[-1], 16))
        idx_starts = set()
        for a, _s, m, o in ins:
            if m == "movzx" and "byte ptr [" in (o or ""):
                for h in _HEX.findall(o):
                    v = int(h, 16)
                    if img.lo <= v < img.hi:
                        idx_starts.add(v)
        bounds = sorted(tstarts | idx_starts | set(starts))
        for t in tstarts:
            e = t
            while True:
                v = code_dword(e)
                if v is None or not (img.lo <= v < img.hi):
                    break
                e += 4
            if e > t:
                table_ranges.append((t, e))
        for t in idx_starts:
            j = bisect.bisect_right(bounds, t)
            nxt = bounds[j] if j < len(bounds) else t + 256
            e = t
            while e < min(nxt, t + 256):
                c = code_byte(e)
                if c is None or c == 0xCC:
                    break
                e += 1
            if e > t:
                table_ranges.append((t, e))
        table_ranges.sort()

    def in_table(a):
        j = bisect.bisect_right(table_ranges, (a, float("inf"))) - 1
        return j >= 0 and table_ranges[j][0] <= a < table_ranges[j][1]

    # --- SIMD blocks ----------------------------------------------------
    simd_at = set()
    simd_blocks = collections.Counter()
    for a, _sz, m, o in ins:
        if simd_class(m, o) == cls and not in_table(a):
            simd_at.add(a)
            simd_blocks[block_of(a)] += 1
    cpuid_at = [a for a, _s, m, _o in ins if m == "cpuid"]

    # --- functions that detect the CPU (cpuid inside, or call one) --------
    # A function's body runs from its start to the next int3 padding (MSVC
    # pads between functions); prefix sums keep this linear.
    n = len(ins)
    next_pad = [n] * (n + 1)
    for k in range(n - 1, -1, -1):
        next_pad[k] = k if ins[k][2] == "int3" else next_pad[k + 1]

    def body_end(k):
        return next_pad[k + 1] if k + 1 <= n else n

    def prefix(flags):
        p = [0] * (n + 1)
        for k in range(n):
            p[k + 1] = p[k] + flags[k]
        return p

    call_to = [None] * n
    call_targets = set()
    for k, (a, _s, m, o) in enumerate(ins):
        if m == "call" and _HEX.fullmatch(o or ""):
            call_to[k] = int(o, 16)
            call_targets.add(call_to[k])
    cp = prefix([1 if i[2] == "cpuid" else 0 for i in ins])
    detect = set()
    for f in call_targets:
        k = img.index.get(f)
        if k is not None and cp[body_end(k)] - cp[k] > 0:
            detect.add(f)
    for _ in range(2):              # callers of detectors, two levels up
        dp = prefix([1 if call_to[k] in detect else 0 for k in range(n)])
        more = set()
        for f in call_targets - detect:
            k = img.index.get(f)
            if k is not None and dp[body_end(k)] - dp[k] > 0:
                more.add(f)
        if not more:
            break
        detect |= more

    def is_cpu_decision_ins(m, o):
        if m == "cpuid":
            return True
        if m == "call" and _HEX.fullmatch(o or "") and int(o, 16) in detect:
            return True
        for h in _HEX.findall(o or ""):
            if int(h, 16) in img.ipfp_slots:
                return True
        return False

    # globals written next to a cpu decision = CPU feature flags
    cpu_flags = set()
    for k, (a, _s, m, o) in enumerate(ins):
        if not is_cpu_decision_ins(m, o):
            continue
        b = block_of(a)
        for j in range(k, min(k + 80, len(ins))):
            aj, _sj, mj, oj = ins[j]
            if block_of(aj) != b:
                break
            if mj in ("mov", "and", "or") and _FLAG_WRITE.match(oj or ""):
                cpu_flags.add(_FLAG_WRITE.match(oj).group(1))

    def gate_before(site):
        """flag_gate / cpu_decision evidence for code at `site`, or None."""
        k = img.index.get(site)
        if k is None:
            return None
        b = block_of(site)
        seen_jcc = False
        for j in range(k - 1, max(-1, k - WINDOW - 1), -1):
            aj, _sj, mj, oj = ins[j]
            if aj < b:
                break
            if _JCC.match(mj):
                seen_jcc = True
            if mj == "cmp" and seen_jcc:
                mm = _FLAG_CMP.match(oj or "")
                if mm and mm.group(1) in cpu_flags:
                    return "flag_gate"
            if is_cpu_decision_ins(mj, oj) and seen_jcc:
                return "cpu_decision"   # a CPU decision, then a branch on it
        return None

    def target_opens_with_gate(t):
        """the way in lands ON a `cmp [flag],0 / jcc` and the SIMD is past it"""
        k = img.index.get(t)
        if k is None or k + 1 >= len(ins):
            return False
        m0, o0 = ins[k][2], ins[k][3]
        mm = _FLAG_CMP.match(o0 or "")
        return (m0 == "cmp" and mm is not None and mm.group(1) in cpu_flags
                and _JCC.match(ins[k + 1][2]) is not None)

    reach_memo = {}

    def reach_from(t):
        """Instruction addresses control entering at t can reach inside t's own
        block: fall-through and direct branches that stay in the block (a
        branch out is some other block's way in, judged there); stops at ret
        and at indirect jumps. None when t is not an instruction boundary."""
        if t in reach_memo:
            return reach_memo[t]
        if img.index.get(t) is None:
            reach_memo[t] = None
            return None
        b = block_of(t)
        end = block_end(b)
        seen = set()
        todo = [img.index[t]]
        while todo:
            k = todo.pop()
            while k < len(ins) and ins[k][0] not in seen:
                aj, _s, mj, oj = ins[k]
                if aj >= end or aj < b:
                    break
                seen.add(aj)
                if mj == "ret":
                    break
                if (mj == "jmp" or _JCC.match(mj)) and _HEX.fullmatch(oj or ""):
                    tk = img.index.get(int(oj, 16))
                    if tk is not None:
                        todo.append(tk)
                    if mj == "jmp":
                        break
                elif mj == "jmp":
                    break           # indirect: not followed
                k += 1
        reach_memo[t] = seen
        return seen

    def reaches_simd(t):
        """Can control entering at t reach a SIMD instruction of its block?
        Drops ways that only touch the x87 tail of a block whose SIMD sits
        elsewhere in it."""
        r = reach_from(t)
        return True if r is None else any(a in simd_at for a in r)

    def reaches(t, site):
        r = reach_from(t)
        return True if r is None else site in r

    # --- every reference into the image, by target block ---------------
    refs = collections.defaultdict(list)     # block -> [(target, src, kind, text)]
    for a, _s, m, o in ins:
        for h in _HEX.findall(o or ""):
            v = int(h, 16)
            if not (img.lo <= v < img.hi):
                continue
            if _JUMPTABLE.match(o or "") or (m == "movzx" and "byte ptr [" in o):
                kind = "jumptable"
            elif (m == "call" or m == "jmp" or _JCC.match(m)) and o == h:
                kind = "branch"
            else:
                kind = "pointer"
            refs[block_of(v)].append((v, a, kind, "%s %s" % (m, o)))
    for va, v, raw in img.data:
        if img.lo <= v < img.hi:
            refs[block_of(v)].append((v, va, "data", raw))
    for va, kind in img.roots:
        if img.lo <= va < img.hi:
            refs[block_of(va)].append((va, va, kind, kind))

    def ways_into(b):
        """references into block b from outside it (data pointers, exports and
        the entry point always count - they have no source block)"""
        return [r for r in refs.get(b, ())
                if r[2] in OUTSIDE or block_of(r[1]) != b]

    def ways_to_site(site):
        """the ways into site's block that can actually reach site - a block
        split only at ret/int3 can hold the tail of one function and the whole
        of the next, and a way into the first says nothing about the second"""
        return [r for r in ways_into(block_of(site)) if reaches(r[0], site)]

    def direct_way(t, src, kind, text, into):
        """Classify one way into `into` without looking further back. Returns
        (verdict, why); verdict 'NEEDS' means 'guarded only if the calling
        block is itself reached only under a CPU decision'."""
        is_simd = into in simd_blocks
        if kind == "jumptable":
            return ("ignored", "jump table")
        if kind in ("entry", "export"):
            if is_simd and not reaches_simd(t):
                return ("ignored", "lands past the SIMD")
            if target_opens_with_gate(t):
                return ("flag_gate", "lands on the flag test")
            return ("UNGUARDED", "called from outside the image (%s)" % kind)
        if kind == "data":
            raw = text
            if isinstance(raw, (bytes, bytearray)) and all(
                    0x20 <= c < 0x7f or c == 0 for c in raw) and any(raw):
                return ("ignored", "ascii")
            if img.index.get(t) is None:
                # a function pointer points at an instruction; a dword that
                # lands mid-instruction is a number that looks like an address
                return ("ignored", "not an instruction boundary")
            if is_simd and not reaches_simd(t):
                return ("ignored", "lands past the SIMD")
            if is_simd and t != into and simd_blocks[into] <= 2:
                return ("seh_probe", "pointer into a probe body")
            return ("UNGUARDED", "data pointer")
        if is_simd and not reaches_simd(t):
            return ("ignored", "lands past the SIMD")
        if target_opens_with_gate(t):
            return ("flag_gate", "lands on the flag test")
        g = gate_before(src)
        if g:
            return (g, "")
        if block_of(src) in simd_blocks:
            return ("internal", "")
        return ("NEEDS", src)

    # --- is the calling code reached only under a CPU decision? ---------
    # Nodes are SITES (the instruction that makes a way into SIMD code, then
    # the instructions making ways into THAT code, ...), walked back at most
    # MAX_DEPTH callers. Solved as fixed points:
    #   dead          every way to it comes from dead code (or there is none)
    #   G (greatest)  no way to it is UNGUARDED (a cycle alone does not taint)
    #   R (least)     some chain of ways reaches a real CPU decision
    # dispatch_only = in G and in R and not dead, so code that only calls
    # itself, or a chain that ran past the depth limit, is never "guarded".
    GUARDS = ("flag_gate", "cpu_decision", "seh_probe")

    def solve(roots):
        """-> (dispatched, dead) sets of sites. 'dead': no call, jump,
        pointer, export or entry point leads to it - statically linked code
        the linker kept anyway (Keystone.dll carries three such D3DX SSE2
        routines, and a call from them is no way in at all)."""
        depth = {r: 0 for r in roots}
        todo = list(roots)
        graph = {}
        while todo:
            site = todo.pop()
            res = []
            into = block_of(site)
            for (t, src, kind, text) in ways_to_site(site):
                v, why = direct_way(t, src, kind, text, into)
                if v == "NEEDS":
                    if depth[site] >= MAX_DEPTH:
                        res.append(("bad", None))
                        continue
                    if why not in depth:
                        depth[why] = depth[site] + 1
                        todo.append(why)
                    res.append(("dep", why))
                elif v == "UNGUARDED":
                    res.append(("bad", None))
                elif v in GUARDS:
                    res.append(("root", None))
                else:
                    res.append(("neutral", None))
            graph[site] = res
        dead = {b for b, res in graph.items() if all(k == "dep" for k, _ in res)}
        changed = True
        while changed:
            changed = False
            for b in list(dead):
                if any(d not in dead for _k, d in graph[b]):
                    dead.discard(b)
                    changed = True
        for b, res in graph.items():         # a dead caller neither taints nor guards
            graph[b] = [("neutral", None) if (k == "dep" and d in dead) else (k, d)
                        for k, d in res]
        if debug is not None:
            debug["graph"] = graph
            debug["depth"] = depth
            debug["dead"] = dead
        good = {b for b, res in graph.items()
                if res and not any(k == "bad" for k, _ in res)}
        changed = True
        while changed:                                   # greatest fixed point
            changed = False
            for b in list(good):
                if any(k == "dep" and d not in good for k, d in graph[b]):
                    good.discard(b)
                    changed = True
        reach = {b for b, res in graph.items() if any(k == "root" for k, _ in res)}
        changed = True
        while changed:                                   # least fixed point
            changed = False
            for b, res in graph.items():
                if b not in reach and any(k == "dep" and d in reach for k, d in res):
                    reach.add(b)
                    changed = True
        return (good & reach) - dead, dead

    pending = set()
    for b in simd_blocks:
        for (t, src, kind, text) in ways_into(b):
            if block_of(src) in simd_blocks and kind not in OUTSIDE:
                continue
            v, why = direct_way(t, src, kind, text, b)
            if v == "NEEDS":
                pending.add(why)
    dispatched_sites, dead_sites = solve(pending)

    def classify_way(t, src, kind, text, into):
        v, why = direct_way(t, src, kind, text, into)
        if v != "NEEDS":
            return (v, why)
        if why in dispatched_sites:
            return ("dispatch_only", "every way to %#x is guarded" % why)
        if why in dead_sites:
            return ("unreferenced", "nothing leads to %#x" % why)
        return ("UNGUARDED", "")

    entries = collections.Counter()
    unguarded = []
    no_entry = []
    all_ways = []
    for b in sorted(simd_blocks):
        ways = [r for r in refs.get(b, ())
                if r[2] in OUTSIDE or block_of(r[1]) not in simd_blocks]
        if not ways:
            no_entry.append(b)
            continue
        for (t, src, kind, text) in ways:
            verdict, why = classify_way(t, src, kind, text, b)
            entries[verdict] += 1
            way = {"region": b, "target": t, "from": src, "kind": kind,
                   "text": text if isinstance(text, str) else repr(text),
                   "verdict": verdict, "why": why}
            all_ways.append(way)
            if verdict == "UNGUARDED":
                unguarded.append(way)
    return {
        "class": cls,
        "count": sum(simd_blocks.values()),
        "regions": len(simd_blocks),
        "cpuid_sites": len(cpuid_at),
        "entries": dict(entries),
        "unguarded": unguarded,
        "no_entry": no_entry,
        "ways": all_ways,
        "verdict": ("none" if not simd_blocks
                    else "floor" if unguarded else "dispatched"),
    }


# --------------------------------------------------------------------------
# PE loading
# --------------------------------------------------------------------------
def load_image(path):
    import capstone          # noqa: deferred so the core imports without them
    import pefile
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    ins, data, code = [], [], []
    for s in pe.sections:
        blob = s.get_data()
        va = base + s.VirtualAddress
        if s.Characteristics & 0x20000000:            # IMAGE_SCN_MEM_EXECUTE
            code.append((va, blob))
            for i in md.disasm(blob, va):
                ins.append((i.address, i.size, i.mnemonic, i.op_str))
        elif s.Characteristics & 0x02000000 or s.Name.rstrip(b"\0") == b".rsrc":
            continue      # .reloc (discardable) and resources hold no pointers
        else:
            for off in range(0, len(blob) - 3, 4):
                raw = blob[off:off + 4]
                data.append((va + off, struct.unpack("<I", raw)[0], raw))
    slots = []
    for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) or []:
        for imp in e.imports:
            if imp.name == b"IsProcessorFeaturePresent":
                slots.append(imp.address)
    roots = [(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, "entry")]
    exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    for sym in (exp.symbols if exp else ()):
        if sym.address:
            roots.append((base + sym.address, "export"))
    tls = getattr(pe, "DIRECTORY_ENTRY_TLS", None)
    if tls is not None and tls.struct.AddressOfCallBacks:
        cb = tls.struct.AddressOfCallBacks
        for k in range(64):
            try:
                v = pe.get_dword_at_rva(cb - base + 4 * k)
            except Exception:
                break
            if not v:
                break
            roots.append((v, "entry"))
    return Image(ins, data, slots, roots, code)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pe", nargs="+")
    ap.add_argument("--class", dest="cls", default="sse2", choices=("sse2", "sse"))
    ap.add_argument("--json", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    rc = 0
    out = {}
    for p in a.pe:
        try:
            rep = analyse(load_image(p), a.cls)
        except ImportError as exc:
            print("simdguard: needs pefile and capstone (%s)" % exc, file=sys.stderr)
            return 2
        except Exception as exc:                      # not a PE, unreadable
            print("simdguard: %s: %s" % (p, exc), file=sys.stderr)
            rc = max(rc, 2)
            continue
        out[p] = rep
        if rep["verdict"] == "floor":
            rc = max(rc, 1)
        if not a.json:
            print("%s: %s %d instr in %d region(s), %d cpuid site(s) -> %s"
                  % (p, a.cls, rep["count"], rep["regions"], rep["cpuid_sites"],
                     rep["verdict"].upper()))
            print("    ways in: %s" % ", ".join(
                "%s=%d" % kv for kv in sorted(rep["entries"].items())) or "none")
            if a.verbose:
                for w in rep["ways"]:
                    if w["verdict"] != "UNGUARDED":
                        print("    %-13s %#x <- %#x [%s] %s %s" % (
                            w["verdict"], w["target"], w["from"], w["kind"],
                            w["text"], w["why"]))
            for u in rep["unguarded"][: (None if a.verbose else 12)]:
                print("    UNGUARDED %#x <- %#x [%s] %s %s" % (
                    u["target"], u["from"], u["kind"], u["text"], u["why"]))
            if rep["no_entry"] and a.verbose:
                print("    no way in found (not proof of anything): %s" % ", ".join(
                    "%#x" % b for b in rep["no_entry"]))
    if a.json:
        print(json.dumps(out, indent=1, default=str))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
