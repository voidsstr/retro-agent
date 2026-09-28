"""The agent must execute no instruction a genuine Pentium 1 (P54C) lacks.

WHY. `.243` is a Compaq Deskpro 2000 -- a Pentium P54C on Windows 98 SE. Every
instruction family introduced with the Pentium Pro or later raises
`0xC000001D` STATUS_ILLEGAL_INSTRUCTION there. Inside CRT startup that kills
the agent before `main`, so it never logs a line and the box simply looks
agent-less -- the hardest failure on this fleet to diagnose remotely. Inside a
command handler it kills that command:

    [3] Exception 0xC000001D processing command

which is exactly what every `UIDRAG` did on `.243` until 2026-09-28.

THE UIDRAG MISS, AND WHY THIS FILE NOW SCANS FOR EVERY FAMILY. This test used
to look for CMOV only. `handle_uidrag()` (agent/src/input.c) called `sqrt()`,
and the `sqrt` that links is NOT the box's msvcrt.dll -- it is mingw's STATIC
`_sqrt` out of `libmsvcrt.a` (`lib32_libmsvcrt_common_a-sqrt.o`), prebuilt for
i686, whose path for every positive argument is

    fld1 ; fxch %st(1) ; fucomi %st(1),%st ; ... ; fsqrt

FUCOMI is as Pentium-Pro-only as CMOV, and the CMOV-only scan passed right
over it. `-march=i586` cannot reach a prebuilt runtime object, so the compiler
flag was never going to catch it either. The fix computes the step count with
integer arithmetic (agent/shared/dragsteps.h, tests/native/test_dragsteps.c),
and this file now scans the BUILT binary for every P6-or-later family:

    CMOVcc, FCMOVcc, FCOMI/FCOMIP/FUCOMI/FUCOMIP (P6), multi-byte/hint NOP
    0F 18-1F (P6), SYSENTER/SYSEXIT, FXSAVE/FXRSTOR (P-II), RDPMC,
    MMX (any %mm register, EMMS -- the P54C has none; only the P55C does),
    SSE/SSE2/SSE3 (any %xmm register, the MXCSR/fence/prefetch/non-temporal
    forms, the xmm->GPR conversions, FISTTP), and anything newer.

Measured on the pre-fix build (1.89.1 source), the WHOLE binary held exactly
three such instructions: `fucomi` in `_sqrt` (live -- UIDRAG) and one `cmove`
each in `__GetPEImageBase` and `_mark_section_writable` (dead -- see below).
After the fix, `_sqrt` is not linked at all and only the two dead CMOVs remain.

THE TWO ALLOWED CMOVs ARE DEAD, AND THE REASON IS A PROPERTY OF THE LINK.

    ___tmainCRTStartup
      -> __pei386_runtime_relocator
           -> _mark_section_writable   (CMOV)
           -> __GetPEImageBase         (CMOV)

The relocator's first real act is to measure its own work list:

    mov  $LIST_END,%eax ; sub $LIST,%eax ; cmp $0x7,%eax ; jle <epilogue>

The agent links with no pseudo-relocations at all, so both bounds are the same
address, the subtraction is a compile-time zero, and the function returns
before either CMOV-bearing callee is reached. Acquiring even one
pseudo-relocation -- a differently linked import, a new library, a dropped
`-static` -- makes both live with no change in any count, so the empty list is
asserted separately, against the built artifact.

The allowance is NARROW: a (function, family) pair with a maximum count. A
CMOV anywhere else, a second CMOV in one of those two functions, or any other
family in them, fails.
"""
import collections
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(REPO, "agent", "retro_agent.exe")
INPUT_C = os.path.join(REPO, "agent", "src", "input.c")

NM = "i686-w64-mingw32-nm"
OBJDUMP = "i686-w64-mingw32-objdump"

# --------------------------------------------------------------------------
# the scanner
# --------------------------------------------------------------------------

# Families matched on the MNEMONIC (after prefixes), anchored, AT&T spelling as
# objdump prints it (size suffixes allowed where objdump adds them).
#   (family, full-match regex, why a P54C faults on it)
MNEMONIC_FAMILIES = [
    ("cmov",   r"cmov[a-z]+",            "CMOVcc is Pentium Pro"),
    ("fcmov",  r"fcmov[a-z]+",           "FCMOVcc is Pentium Pro"),
    ("fcomi",  r"fu?comip?",             "FCOMI/FCOMIP/FUCOMI/FUCOMIP are Pentium Pro"),
    ("mmx",    r"f?emms",                "MMX: the P54C has none (P55C and later)"),
    ("fxsr",   r"fxsave\w*|fxrstor\w*",  "FXSAVE/FXRSTOR are Pentium II"),
    ("sysenter", r"sysenter|sysexit",    "SYSENTER/SYSEXIT are Pentium II"),
    ("rdpmc",  r"rdpmc",                 "RDPMC is Pentium Pro / Pentium MMX"),
    ("sse",    r"ldmxcsr|stmxcsr|[slm]fence|clflush\w*|movnti\w*|prefetch\w*"
               r"|cvtt?s[sd]2si\w*|fisttp\w*|monitor|mwait|maskmovq",
                                         "SSE/SSE2/SSE3 (no xmm operand needed to be one)"),
    ("endbr",  r"endbr(32|64)",          "ENDBR sits in the 0F 1E hint-NOP space, #UD before P6"),
    # NOT tzcnt: it is F3 0F BC, i.e. `rep bsf`, and every pre-BMI CPU runs it
    # AS BSF (the REP prefix is ignored). gcc emits it on purpose for ctz,
    # where the two agree for any non-zero input. lzcnt (F3 0F BD) also
    # decodes as BSR there, but with a DIFFERENT answer, so it stays flagged.
    ("post-p6", r"popcnt\w*|lzcnt\w*|movbe\w*|crc32\w*|rdrand|rdseed"
                r"|rdtscp|xgetbv|xsave\w*|xrstor\w*|adcx|adox|andn|bextr|blsi"
                r"|blsmsk|blsr|bzhi|mulx|pdep|pext|rorx|sarx|shlx|shrx"
                r"|cmpxchg16b|vzero\w*",  "post-Pentium-Pro extensions"),
]
_MNEM_RES = [(fam, re.compile(r"(?:%s)\Z" % rx), why)
             for fam, rx, why in MNEMONIC_FAMILIES]

# Families matched on a REGISTER anywhere in the operands.
REGISTER_FAMILIES = [
    ("mmx",  re.compile(r"%mm[0-7]\b"),        "MMX register - absent on the P54C"),
    ("sse",  re.compile(r"%xmm\d+\b"),         "SSE register"),
    ("avx",  re.compile(r"%[yz]mm\d+\b|%k[0-7]\b"), "AVX register"),
]

# Plain 0x90 `nop` has no operand. `nop`/`nopw`/`nopl` WITH an operand is the
# 0F 18-1F multi-byte / hint-NOP encoding, which is Pentium Pro: a P5 faults
# on it. gas pads with it for -march=i686 objects, so it is the likeliest
# family to arrive in a prebuilt runtime object after CMOV.
_HINT_NOP = re.compile(r"nop[lw]?\Z")

_PREFIXES = {"lock", "rep", "repz", "repnz", "repe", "repne", "cs", "ds", "es",
             "ss", "fs", "gs", "data16", "data32", "addr16", "addr32", "bnd",
             "notrack", "xacquire", "xrelease", "rex"}

_FN_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:")
_INSN_RE = re.compile(r"^\s*([0-9a-f]+):\t[^\t]*\t(.*)$")

Hit = collections.namedtuple("Hit", "func addr family insn")


def scan(disassembly):
    """Every P6-or-later instruction in `objdump -d` (AT&T) output."""
    hits = []
    fn = "<none>"
    for line in disassembly.splitlines():
        m = _FN_RE.match(line)
        if m:
            fn = m.group(2)
            continue
        m = _INSN_RE.match(line)
        if not m:
            continue
        addr, insn = m.group(1), m.group(2).strip()
        toks = insn.split(None, 1)
        while toks and toks[0] in _PREFIXES:
            toks = toks[1].split(None, 1) if len(toks) > 1 else []
        if not toks:
            continue
        mnem = toks[0]
        operands = toks[1] if len(toks) > 1 else ""
        fam = None
        for f, rx, _why in _MNEM_RES:
            if rx.match(mnem):
                fam = f
                break
        if fam is None and _HINT_NOP.match(mnem) and operands.strip():
            fam = "hint-nop"
        if fam is None:
            for f, rx, _why in REGISTER_FAMILIES:
                if rx.search(operands):
                    fam = f
                    break
        if fam is not None:
            hits.append(Hit(fn, addr, fam, insn))
    return hits


# The ONLY P6 instructions the agent may carry: (function, family) -> max count.
# Both are prebuilt mingw pseudo-relocator helpers, dead because the
# pseudo-reloc list is empty (asserted below).
ALLOWED = {
    ("__GetPEImageBase", "cmov"): 1,
    ("_mark_section_writable", "cmov"): 1,
}


def disallowed(hits):
    """Hits beyond ALLOWED, per (function, family)."""
    seen = collections.Counter()
    bad = []
    for h in hits:
        key = (h.func, h.family)
        seen[key] += 1
        if seen[key] > ALLOWED.get(key, 0):
            bad.append(h)
    return bad


def _fmt(hits):
    return "\n".join("    %-32s %-9s 0x%s  %s" % (h.func, h.family, h.addr, h.insn)
                     for h in hits)


# --------------------------------------------------------------------------
# the scanner, on the real pre-fix disassembly (the old-buggy half)
# --------------------------------------------------------------------------

# Verbatim `objdump -d` of the pre-fix build (1.89.1 source, relinked with
# symbols): the UIDRAG call site, the static _sqrt it reached, and one of the
# two allowed CMOVs.
PRE_FIX_DISASSEMBLY = """\
00422fce <_handle_uidrag>:
  423117:\tdd 1c 24             \tfstpl  (%esp)
  42311a:\te8 71 4a 01 00       \tcall   437b90 <_sqrt>

00437790 <__GetPEImageBase>:
  4377be:\t0f 44 c2             \tcmove  %edx,%eax

00437b90 <_sqrt>:
  437b96:\tdd 45 08             \tfldl   0x8(%ebp)
  437b99:\td9 e5                \tfxam
  437b9b:\t9b df e0             \tfstsw  %ax
  437bce:\td9 e8                \tfld1
  437bd0:\td9 c9                \tfxch   %st(1)
  437bd2:\tdb e9                \tfucomi %st(1),%st
  437bd4:\tdd d9                \tfstp   %st(1)
  437bd6:\t7a 06                \tjp     437bde <_sqrt+0x4e>
  437bde:\td9 fa                \tfsqrt
  437be2:\t8d b6 00 00 00 00    \tlea    0x0(%esi),%esi
"""


def test_the_scanner_catches_the_uidrag_fucomi_the_cmov_scan_missed():
    """Old-buggy half: the pre-fix binary FAILS this scan, on _sqrt's FUCOMI."""
    bad = disallowed(scan(PRE_FIX_DISASSEMBLY))
    assert [(h.func, h.family, h.addr) for h in bad] == [("_sqrt", "fcomi", "437bd2")]
    # ...and the check this file used to make (CMOV only, two functions
    # allowed) found nothing wrong with the same disassembly:
    fn, old_offenders = None, set()
    for line in PRE_FIX_DISASSEMBLY.splitlines():
        m = _FN_RE.match(line)
        if m:
            fn = m.group(2)
        elif "\tcmov" in line:
            old_offenders.add(fn)
    assert old_offenders - {"__GetPEImageBase", "_mark_section_writable"} == set()


@pytest.mark.parametrize("insn,family", [
    ("cmovne %ecx,%eax", "cmov"),
    ("cmovgl 0x4(%esp),%eax", "cmov"),
    ("fcmovbe %st(1),%st", "fcmov"),
    ("fcomi  %st(1),%st", "fcomi"),
    ("fcomip %st(1),%st", "fcomi"),
    ("fucomi %st(1),%st", "fcomi"),
    ("fucomip %st(1),%st", "fcomi"),
    ("nopl   0x0(%eax)", "hint-nop"),
    ("nopw   0x0(%eax,%eax,1)", "hint-nop"),
    ("cs nopw 0x0(%eax,%eax,1)", "hint-nop"),
    ("data16 cs nopw 0x0(%eax,%eax,1)", "hint-nop"),
    ("emms", "mmx"),
    ("movq   %mm0,(%eax)", "mmx"),
    ("paddb  %mm1,%mm0", "mmx"),
    ("movaps %xmm0,(%esp)", "sse"),
    ("cvttsd2si 0x8(%esp),%eax", "sse"),
    ("ldmxcsr 0x4(%esp)", "sse"),
    ("sfence", "sse"),
    ("prefetchnta (%eax)", "sse"),
    ("movnti %eax,(%edx)", "sse"),
    ("fisttpl 0x8(%esp)", "sse"),
    ("fxsave (%eax)", "fxsr"),
    ("sysenter", "sysenter"),
    ("rdpmc", "rdpmc"),
    ("endbr32", "endbr"),
    ("popcnt %eax,%edx", "post-p6"),
    ("vmovdqu %ymm0,(%eax)", "avx"),
])
def test_the_scanner_flags_every_family(insn, family):
    dis = "00401000 <_f>:\n  401000:\t00 00 \t%s\n" % insn
    assert [(h.func, h.family) for h in scan(dis)] == [("_f", family)]


@pytest.mark.parametrize("insn", [
    # every one of these is a P5 (or older) instruction and must NOT be flagged
    "nop", "xchg   %ax,%ax", "pause", "lea    0x0(%esi,%eiz,1),%esi",
    "fcom   %st(1)", "fcoml  0x8(%esp)", "fcomp  %st(1)", "fcompp",
    "fucom  %st(1)", "fucomp %st(1)", "fucompp", "fnstsw %ax", "sahf",
    "fsqrt", "fxam", "fistpl 0x38(%esp)", "fldcw  0x4c(%esp)",
    "cpuid", "rdtsc", "lock cmpxchg8b (%esi)", "lock cmpxchg %edx,0x45a034",
    "bswap  %eax", "xadd   %eax,(%edx)", "rep stos %eax,%es:(%edi)",
    "ud2", "in     $0x71,%al", "out    %al,$0x70",
    "tzcnt  %ebx,%ecx",   # rep bsf: runs as BSF on a P5 (see MNEMONIC_FAMILIES)
    "mov    %eax,0x4(%esp)", "movzbl (%eax),%edx", "imul   %eax,%eax",
])
def test_the_scanner_passes_p5_instructions(insn):
    dis = "00401000 <_f>:\n  401000:\t00 00 \t%s\n" % insn
    assert scan(dis) == []


# --------------------------------------------------------------------------
# the BUILT agent
# --------------------------------------------------------------------------

def _need(tool):
    if not shutil.which(tool):
        pytest.skip("%s not installed - cannot inspect the built agent" % tool)


def _need_exe():
    if not os.path.exists(EXE):
        # Loud, not silent: an unbuilt agent means this invariant went unchecked.
        pytest.skip("agent/retro_agent.exe not built - P5 safety NOT verified; "
                    "run `make -C agent` to check it")


def _run(*cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return p.stdout


# The SHIPPED agent is linked with -s, so it carries no symbols: objdump then
# attributes every instruction to `.text`, and neither the pseudo-reloc bounds
# nor an instruction's enclosing function can be read. So relink the REAL
# objects, minus the strip, into a temp file -- same compiler, objects,
# libraries and link; only the symbol table differs. The shipped binary is
# scanned as well (below) and must carry the same hits, family for family.
_LINK_LIBS = ["-lws2_32", "-ladvapi32", "-lsetupapi", "-lgdi32",
              "-luser32", "-lkernel32", "-lwinmm"]


@pytest.fixture(scope="module")
def relinked(tmp_path_factory):
    _need_exe()
    objdir = os.path.join(REPO, "agent", "obj")
    if not os.path.isdir(objdir):
        pytest.skip("agent/obj not present - run `make -C agent` first; "
                    "P5 safety NOT verified")
    objs = sorted(os.path.join(objdir, f) for f in os.listdir(objdir)
                  if f.endswith(".o"))
    if not objs:
        pytest.skip("no object files in agent/obj - P5 safety NOT verified")
    cc = "i686-w64-mingw32-gcc"
    _need(cc)
    out = str(tmp_path_factory.mktemp("p5") / "agent-sym.exe")
    p = subprocess.run(
        [cc, "-static", "-Wl,--subsystem,console",
         "-Wl,--major-os-version,4", "-Wl,--minor-os-version,0",
         "-L" + os.path.join(REPO, "agent", "lib"), "-o", out]
        + objs + _LINK_LIBS,
        capture_output=True, text=True, timeout=600)
    if p.returncode != 0 or not os.path.exists(out):
        pytest.skip("could not relink an unstripped agent (%s) - P5 safety "
                    "NOT verified" % (p.stderr.strip().splitlines() or [""])[-1:])
    return out


def test_the_pseudo_reloc_list_is_empty(relinked):
    """The load-bearing fact. If this list is non-empty the two CMOVs go live.

    This is the assertion that a CMOV *count* cannot make: the count stays at
    2 either way.
    """
    _need(NM)
    out = _run(NM, relinked)
    start = end = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr, sym = parts[0], parts[-1]
        if sym in ("__RUNTIME_PSEUDO_RELOC_LIST__", "___RUNTIME_PSEUDO_RELOC_LIST__"):
            start = addr
        elif sym in ("__RUNTIME_PSEUDO_RELOC_LIST_END__",
                     "___RUNTIME_PSEUDO_RELOC_LIST_END__"):
            end = addr
    if start is None or end is None:
        pytest.skip("no pseudo-reloc list symbols (stripped build?) - "
                    "P5 reachability NOT verified")
    assert start == end, (
        "the runtime pseudo-relocation list is NOT empty (start=%s end=%s).\n\n"
        "That makes __pei386_runtime_relocator do real work at startup, which "
        "calls _mark_section_writable and __GetPEImageBase -- both of which "
        "execute a CMOV. CMOV is Pentium Pro; a genuine Pentium 1 faults with "
        "0xC000001D BEFORE main, so the agent dies without logging anything and "
        "the box appears to have no agent at all.\n\n"
        "The CMOV count will not have changed. Find what introduced a "
        "pseudo-relocation (a new import, a lost -static) rather than relaxing "
        "this test." % (start, end)
    )


def test_no_p6_only_instruction_outside_the_allowlist(relinked):
    """Every P6-or-later instruction in the whole linked agent, by function."""
    _need(OBJDUMP)
    dis = _run(OBJDUMP, "-d", relinked)
    assert "<_handle_uidrag>:" in dis, (
        "the relinked binary has no function symbols - this check would be "
        "measuring nothing")
    hits = scan(dis)
    bad = disallowed(hits)
    assert not bad, (
        "%d instruction(s) a Pentium P54C cannot execute (0xC000001D "
        "ILLEGAL_INSTRUCTION on .243), ALL offenders listed:\n%s\n\n"
        "Our own code is built -march=i586 and should never emit these; a hit "
        "is usually a PREBUILT runtime object the flag cannot reach (UIDRAG's "
        "sqrt() pulled mingw's static _sqrt and its FUCOMI). Stop calling the "
        "function that links it, as dragsteps.h did. Only allowlist a hit, by "
        "function and family, if it provably cannot run on a 9x/P5 box."
        % (len(bad), _fmt(bad)))


def test_the_shipped_exe_carries_no_more_than_the_allowlist():
    """The stripped binary we SHIP, scanned directly (no symbols to attribute
    with, so it is held to the allowlist's per-family totals)."""
    _need_exe()
    _need(OBJDUMP)
    hits = scan(_run(OBJDUMP, "-d", EXE))
    allowed_per_family = collections.Counter()
    for (_fn, fam), n in ALLOWED.items():
        allowed_per_family[fam] += n
    per_family = collections.Counter(h.family for h in hits)
    over = {f: n for f, n in per_family.items() if n > allowed_per_family[f]}
    assert not over, (
        "agent/retro_agent.exe carries P6-or-later instructions beyond the "
        "allowlist: %s\n%s" % (over, _fmt(hits)))


def test_uidrag_no_longer_links_the_static_sqrt(relinked):
    """The specific regression: UIDRAG computes its steps without sqrt()."""
    _need(NM)
    syms = {line.split()[-1] for line in _run(NM, relinked).splitlines()
            if line.split()}
    assert "_sqrt" not in syms, (
        "mingw's static _sqrt is linked again - its FUCOMI faults on a "
        "Pentium 1 (UIDRAG on .243, 2026-09-28). Who calls sqrt()?")
    src = open(INPUT_C, encoding="utf-8", errors="replace").read()
    code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    assert not re.search(r"\bsqrt\s*\(", code), "input.c calls sqrt() again"
    assert "#include <math.h>" not in code, "input.c includes <math.h> again"
    assert "drag_steps(dx, dy)" in code, (
        "handle_uidrag must take its step count from shared/dragsteps.h "
        "(integer-only; pinned by tests/native/test_dragsteps.c)")


def test_the_makefile_still_targets_i586():
    """If this flag goes, our own code starts emitting CMOV again."""
    mk = os.path.join(REPO, "agent", "Makefile")
    text = open(mk, encoding="utf-8", errors="replace").read()
    assert "-march=i586" in text, (
        "agent/Makefile no longer builds for i586. gcc's default i686 baseline "
        "emits CMOV throughout our own code, which faults on a Pentium 1 -- "
        "and the two-function allowance in this file assumes only prebuilt "
        "runtime objects can carry one."
    )
