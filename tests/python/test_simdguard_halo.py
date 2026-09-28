"""Halo 1.10 does NOT need SSE2 - and the tool that says why.

FIX BEING GUARDED (2026-09-28). Games-Library/Halo/requires.json declared
cpu_features [mmx, sse, sse2] from an instruction COUNT: halo.exe 1.10 (md5
b7aa8f68829a33a9f0b87ec81425f5db, file version 01.00.10.0621) carries ~500 SSE2
instructions, scalar-double mulsd/addsd/movlpd among them, "the signature of an
/arch:SSE2 build". The gate therefore withheld Halo from every box without
SSE2 - .124's Athlon XP 2400+ (SSE, 3DNow!, no SSE2) is the one that mattered.

Traced by hand, every one of those instructions is behind a CPU decision:
  * the MSVC 7.1 CRT's SSE2 libm (pow/exp/log/...): each entry opens with
    `cmp dword ptr [_sse2_mathfcns], 0 / je <x87>` (halo.exe 0x623e40,
    0x6267f0, 0x628380, 0x6283c0, 0x628d00, 0x628d3c), and that flag is set at
    0x62a74b only after cpuid EDX bit 26 AND an SEH probe (0x62a65c);
  * D3DX9's per-CPU tables: the SSE2 table (filler 0x5bf9d3) is installed at
    0x592dc4 only after IsProcessorFeaturePresent(PF_3DNOW) said no - an
    Athlon XP takes the 3DNow! table - and a cpuid helper returned bit 8;
  * Halo's own feature probe (case 31 of 0x5402a0): cpuid bit 26, then one
    xorpd inside __try.
The same holds for everything halo.exe loads (Keystone.dll, binkw32.dll,
ksimeui.dll, msvcr71.dll), and the retail 1.00 binary has the SAME profile -
this is the engine that shipped for 733 MHz Pentium IIIs.

scripts/gamegate/simdguard.py turns that trace into a tool: for every region
of SIMD code it asks HOW CONTROL GETS IN. The unit tests below pin its rules
on synthetic code (the OLD reading - a raw count - cannot tell a guarded
program from an unguarded one; the new one can). The share tests pin the
answer on the real binaries, keep UT99 469e's Core.dll (a genuine /arch:SSE2
build that killed three boxes) reading as a floor, and fail until the staged
requires.json stops declaring sse2.
"""
import hashlib
import json
import os
import struct
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts", "gamegate"))

import simdguard as sg  # noqa: E402  (pure core; pefile/capstone load lazily)

LIB = "/mnt/retro-share/Files/Games-Library"
HALO = os.path.join(LIB, "Halo")
HALO_110_MD5 = "b7aa8f68829a33a9f0b87ec81425f5db"


# ---------------------------------------------------------------------------
# a tiny assembler for synthetic programs: every instruction is 4 bytes,
# "label:" lines name addresses, {label} in an operand is replaced by it
# ---------------------------------------------------------------------------
def assemble(lines, base=0x1000):
    labels, addr = {}, base
    for ln in lines:
        if ln.endswith(":"):
            labels[ln[:-1]] = addr
        else:
            addr += 4
    ins, addr = [], base
    for ln in lines:
        if ln.endswith(":"):
            continue
        m, _, o = ln.partition(" ")
        o = o.strip()
        for k, v in labels.items():
            o = o.replace("{%s}" % k, "%#x" % v)
        ins.append((addr, 4, m, o))
        addr += 4
    return ins, labels


DETECT = [
    "detect:",
    "push ebx",
    "mov eax, 1",
    "cpuid",
    "test edx, 0x4000000",
    "je {d_out}",
    "mov dword ptr [0x9000], 1",       # the CRT's _sse2_mathfcns
    "d_out:",
    "pop ebx",
    "ret",
    "int3",
]
X87 = ["pow_x87:", "fld qword ptr [esp + 4]", "ret", "int3"]
INIT = ["init:", "call {detect}", "ret", "int3"]


def _image(ins, lab, start, **kw):
    """a program is live only from its entry point - the analysis treats code
    nothing reaches as dead, so every synthetic program names one"""
    return sg.Image(ins, roots=[(lab[start], "entry")], **kw)


def _crt_program(gated):
    body = ["pow:"]
    if gated:
        body += ["cmp dword ptr [0x9000], 0", "je {pow_x87}"]
    body += ["movsd xmm0, qword ptr [esp + 4]", "mulsd xmm0, xmm0", "ret", "int3"]
    game = ["game:", "call {pow}", "ret"]
    return assemble(DETECT + body + X87 + INIT + game)


def test_a_flag_gated_crt_routine_is_not_a_floor():
    ins, lab = _crt_program(gated=True)
    rep = sg.analyse(_image(ins, lab, "game"), "sse2")
    assert rep["count"] == 2, "the OLD reading: two SSE2 instructions exist"
    assert rep["verdict"] == "dispatched", rep
    assert rep["entries"] == {"flag_gate": 1}


def test_the_same_routine_called_directly_is_a_floor():
    ins, lab = _crt_program(gated=False)
    rep = sg.analyse(_image(ins, lab, "game"), "sse2")
    # identical count - which is exactly why a count cannot decide a floor
    assert rep["count"] == 2
    assert rep["verdict"] == "floor", rep
    assert [u["from"] for u in rep["unguarded"]] == [lab["game"]]


def _table_program(decided):
    init = ["init:"]
    if decided:
        init += ["call {detect}", "test eax, eax", "je {skip}"]
    init += ["call {fill}", "skip:", "ret", "int3"]
    fill = ["fill:", "mov dword ptr [eax + 8], {sse2_fn}", "ret", "int3"]
    fn = ["sse2_fn:", "movapd xmm0, xmm1", "addpd xmm0, xmm0", "ret", "int3"]
    return assemble(DETECT + init + fill + fn)


def test_a_table_filled_after_a_cpu_decision_is_dispatched():
    """D3DX9's shape: detect, branch, then fill the SSE2 function table"""
    ins, lab = _table_program(decided=True)
    rep = sg.analyse(_image(ins, lab, "init"), "sse2")
    assert rep["verdict"] == "dispatched", rep
    assert rep["entries"] == {"dispatch_only": 1}


def test_a_table_filled_unconditionally_is_a_floor():
    """a function pointer nobody chose by CPU (a vtable, a callback)"""
    ins, lab = _table_program(decided=False)
    rep = sg.analyse(_image(ins, lab, "init"), "sse2")
    assert rep["verdict"] == "floor", rep


def test_a_pointer_in_data_is_never_taken_as_a_guard():
    """a vtable in .rdata: conservative, it counts against the title"""
    ins, lab = assemble(["sse2_fn:", "movapd xmm0, xmm1", "ret", "int3",
                         "other:", "nop", "ret"])
    raw = struct.pack("<I", lab["sse2_fn"])
    rep = sg.analyse(sg.Image(ins, data=[(0x8000, lab["sse2_fn"], raw)]), "sse2")
    assert rep["verdict"] == "floor", rep
    assert rep["unguarded"][0]["kind"] == "data"


def test_switch_table_data_in_text_is_not_simd():
    """MSVC keeps jump tables in .text; linear sweep decodes their bytes as
    instructions. Keystone.dll's 'movups [edx], xmm4' at 0x102218c0 is the
    code address 0x1022110f. Without the raw bytes the fake counts."""
    ins, lab = assemble(["sw:", "jmp dword ptr [eax*4 + {tbl}]", "int3",
                         "case0:", "nop", "ret", "int3",
                         "tbl:", "movdqa xmm0, xmmword ptr [eax]", "nop"])
    lo = ins[0][0]
    blob = bytearray(ins[-1][0] + 4 - lo)
    struct.pack_into("<I", blob, lab["tbl"] - lo, lab["case0"])
    old = sg.analyse(sg.Image(ins), "sse2")
    new = sg.analyse(sg.Image(ins, code=[(lo, bytes(blob))]), "sse2")
    assert old["count"] == 1
    assert new["count"] == 0 and new["verdict"] == "none", new


def test_a_caller_nothing_reaches_is_not_a_floor_but_the_entry_point_is():
    """Keystone.dll keeps three D3DX SSE2 routines no call, pointer or export
    names; a call FROM dead code is no way in. The image entry point has no
    reference either - and must still count."""
    ins, lab = assemble(["dead:", "call {sse2_fn}", "ret", "int3",
                         "sse2_fn:", "movapd xmm0, xmm1", "ret"])
    dead = sg.analyse(sg.Image(ins), "sse2")
    assert dead["verdict"] == "dispatched", dead
    assert dead["entries"] == {"unreferenced": 1}
    entry = sg.analyse(sg.Image(ins, roots=[(lab["dead"], "entry")]), "sse2")
    assert entry["verdict"] == "floor", entry


def test_a_branch_that_only_touches_the_x87_tail_is_ignored():
    """libm blocks mix SSE2 and x87; a jne into the x87 tail is not a way in"""
    ins, lab = assemble(["f:", "movapd xmm0, xmm1", "ret",
                         "tail:", "fld qword ptr [esp + 4]", "ret", "int3",
                         "g:", "jne {tail}", "ret"])
    rep = sg.analyse(_image(ins, lab, "g"), "sse2")
    assert rep["entries"].get("UNGUARDED", 0) == 0, rep


# ---------------------------------------------------------------------------
# the real binaries (share-backed; SKIP LOUDLY without the share)
# ---------------------------------------------------------------------------
def _need_share_and_tools():
    if not os.path.isdir(HALO):
        pytest.skip("SKIPPED LOUDLY: %s not mounted - Halo's SSE2 dispatch was "
                    "NOT re-verified" % HALO)
    pytest.importorskip("pefile")
    pytest.importorskip("capstone")


def _md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def test_the_staged_halo_exe_is_the_one_that_was_traced():
    """if the exe changes, the trace below is about a different binary"""
    _need_share_and_tools()
    assert _md5(os.path.join(HALO, "halo.exe")) == HALO_110_MD5, (
        "Games-Library/Halo/halo.exe is no longer the 1.10 build that was "
        "traced - re-run scripts/gamegate/simdguard.py on it and re-decide "
        "requires.json before updating this pin")


@pytest.mark.parametrize("name", ["halo.exe", "Keystone.dll", "binkw32.dll",
                                  "ksimeui.dll", "msvcr71.dll"])
def test_halo_110_sse2_is_all_runtime_dispatched(name):
    _need_share_and_tools()
    rep = sg.analyse(sg.load_image(os.path.join(HALO, name)), "sse2")
    assert rep["count"] > 0, "%s has no SSE2 at all - the premise changed" % name
    assert rep["unguarded"] == [], (
        "%s: an UNGUARDED way into SSE2 code - Halo would #UD on an SSE-only "
        "CPU (.124): %r" % (name, rep["unguarded"][:5]))
    assert rep["verdict"] == "dispatched"


def test_halo_exe_carries_the_count_that_misled_the_first_reading():
    _need_share_and_tools()
    rep = sg.analyse(sg.load_image(os.path.join(HALO, "halo.exe")), "sse2")
    assert rep["count"] >= 298 and rep["cpuid_sites"] == 19


def test_ut99_469e_core_still_reads_as_a_floor():
    """the positive control: a real /arch:SSE2 build must not be explained away"""
    core = os.path.join(LIB, "UnrealTournament", "System", "Core.dll")
    if not os.path.isfile(core):
        pytest.skip("SKIPPED LOUDLY: %s absent - the floor control was NOT run" % core)
    _need_share_and_tools()
    rep = sg.analyse(sg.load_image(core), "sse2")
    assert rep["verdict"] == "floor"
    assert len(rep["unguarded"]) > 100


def test_staged_halo_requires_json_does_not_declare_sse2():
    """The FIX. Fails until Games-Library/Halo/requires.json drops "sse2".
    Its SSE2 floor was the only thing refusing Halo on .124 / .133 by
    instruction set (.143 lacks SSE entirely and stays refused by "sse")."""
    path = os.path.join(HALO, "requires.json")
    if not os.path.isfile(path):
        pytest.skip("SKIPPED LOUDLY: %s not mounted - requires.json NOT checked" % path)
    with open(path) as fh:
        req = json.load(fh)
    assert "sse2" not in [f.lower() for f in req.get("cpu_features", [])], (
        "Games-Library/Halo/requires.json still declares sse2; halo.exe 1.10 and "
        "every DLL it loads dispatch SSE2 at run time (simdguard.py)")
