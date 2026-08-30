"""The agent must not execute a CMOV before `main` on a Pentium 1.

WHY. A Windows 98 Pentium-1 (Compaq Deskpro 2000 class) is joining the fleet.
`CMOV` is a Pentium **Pro** instruction; a genuine P5 raises `0xC000001D`
ILLEGAL_INSTRUCTION on the first one. If that happens inside CRT startup the
agent dies **before `main`**, so it never opens a socket, never writes a line
of its own log, and the box looks like it simply failed to boot the agent --
the single hardest failure on this fleet to diagnose remotely.

`agent/Makefile` already builds with `-march=i586`, which clears our own code.
Two CMOVs survive anyway, from prebuilt mingw runtime objects that `-march`
cannot reach. Measured on v1.77.2, they are in exactly two functions:

    __GetPEImageBase
    _mark_section_writable

and BOTH are on the startup path:

    ___tmainCRTStartup
      -> __pei386_runtime_relocator
           -> _mark_section_writable   (CMOV)
           -> __GetPEImageBase         (CMOV)

**They are nevertheless unreachable, and the reason is the thing this test
guards.** The relocator's first real act is to measure its own work list:

    mov  $0x4338dc,%eax          ; __RUNTIME_PSEUDO_RELOC_LIST_END__
    sub  $0x4338dc,%eax          ; - __RUNTIME_PSEUDO_RELOC_LIST__   => 0
    cmp  $0x7,%eax
    jle  <epilogue>              ; 0 <= 7, taken -- returns immediately

The agent links with no pseudo-relocations at all, so the linker resolves both
bounds to the *same address*, the subtraction is a compile-time zero, and the
function returns before either CMOV-bearing callee is reached. The only thing
called ahead of that point is `___mingw_GetSectionCount`, which contains no
CMOV and does not call `__GetPEImageBase`.

**So the safety is a property of the LINK, not of the source**, and that is
exactly why counting CMOVs is not enough: the count stays at 2 while the
binary becomes unsafe. Acquiring even one pseudo-relocation -- a differently
linked import, a new library, a dropped `-static` -- makes both instructions
live, with no source change and no change in the count. That is a silent
regression of precisely the shape this project keeps paying for, on the one
machine nobody can easily test.

This test therefore asserts the load-bearing condition (an EMPTY pseudo-reloc
list) as well as the count, and it does so against the BUILT artifact, because
neither fact is visible in the source.
"""
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(REPO, "agent", "retro_agent.exe")

NM = "i686-w64-mingw32-nm"
OBJDUMP = "i686-w64-mingw32-objdump"

# The two mingw runtime functions that legitimately carry a CMOV. Named rather
# than counted, so a CMOV appearing anywhere ELSE is a hard failure even if the
# total happens to stay the same.
KNOWN_CMOV_FUNCS = {"__GetPEImageBase", "_mark_section_writable"}


def _need(tool):
    if not shutil.which(tool):
        pytest.skip("%s not installed - cannot inspect the built agent" % tool)


def _need_exe():
    if not os.path.exists(EXE):
        # Loud, not silent: an unbuilt agent means this invariant went unchecked.
        pytest.skip("agent/retro_agent.exe not built - P5 safety NOT verified; "
                    "run `make -C agent` to check it")


# The SHIPPED agent is linked with -s, so it carries no symbols: objdump then
# attributes every instruction to the section `.text` and neither the
# pseudo-reloc bounds nor the CMOV's enclosing function can be read at all.
# Skipping there would make this file assert nothing about the binary we
# actually ship.
#
# So relink the REAL objects, minus the strip, into a temp file. Same compiler,
# same objects, same libraries, same link -- only the symbol table differs, and
# that is precisely what is needed. Building from source would be a different
# (and slower) thing to have verified.
_LINK_LIBS = ["-lws2_32", "-ladvapi32", "-lsetupapi", "-lgdi32",
              "-luser32", "-lkernel32", "-lwinmm"]


def _unstripped(tmp_path):
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
    out = str(tmp_path / "agent-sym.exe")
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


def _run(*cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return p.stdout


def test_the_pseudo_reloc_list_is_empty(tmp_path):
    """The load-bearing fact. If this list is non-empty the two CMOVs go live.

    This is the assertion that a CMOV *count* cannot make: the count stays at
    2 either way.
    """
    _need_exe()
    _need(NM)
    out = _run(NM, _unstripped(tmp_path))
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


def test_no_cmov_outside_the_two_known_runtime_functions(tmp_path):
    """Our own code must stay CMOV-free; -march=i586 is what keeps it so."""
    _need_exe()
    _need(OBJDUMP)
    dis = _run(OBJDUMP, "-d", _unstripped(tmp_path))

    fn = None
    offenders = set()
    fn_re = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
    for line in dis.splitlines():
        m = fn_re.match(line)
        if m:
            fn = m.group(1)
        elif "\tcmov" in line:
            offenders.add(fn or "<unknown>")

    assert offenders != {".text"}, (
        "the relinked binary still has no symbols - the relink lost -g/symbols "
        "and this check is measuring nothing")

    unexpected = offenders - KNOWN_CMOV_FUNCS
    assert not unexpected, (
        "CMOV found in %s.\n\n"
        "Only %s may contain one: they are prebuilt mingw runtime code that "
        "-march=i586 cannot reach, and they are unreachable at run time "
        "because the pseudo-reloc list is empty. A CMOV anywhere else is OUR "
        "code and will fault on a Pentium 1."
        % (sorted(unexpected), sorted(KNOWN_CMOV_FUNCS))
    )


def test_the_agent_uses_no_sse_at_all():
    """A Pentium 1 has no MMX (unless P55C), no SSE, no SSE2.

    Guarded separately from CMOV because it has a different cause: SSE would
    come from a compiler flag rather than from prebuilt runtime objects, and
    the fix is different too.
    """
    _need_exe()
    _need(OBJDUMP)
    dis = _run(OBJDUMP, "-d", "-M", "intel", EXE)
    sse = re.findall(
        r"\b(movaps|movups|movss|movsd|mulps|addps|subps|divps|"
        r"movdqa|movdqu|cvtsi2sd|cvttsd2si|mulsd|addsd|subsd|punpcklqdq)\b",
        dis)
    assert not sse, (
        "the agent contains %d SSE/SSE2 instruction(s) (%s...). A Pentium 1 "
        "has neither and faults on the first one. Check agent/Makefile still "
        "passes -march=i586." % (len(sse), sorted(set(sse))[:4])
    )


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
