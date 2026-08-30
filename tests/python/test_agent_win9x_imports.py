"""The built agent must import NO entry point Windows 9x lacks.

THE FAILURE THIS GUARDS, measured on .243 (Win98SE, Pentium-1) 2026-08-30
-------------------------------------------------------------------------
That box was stranded on agent **1.30.0** while the fleet ran **1.78.0**, and
1.78.0 would not start on it *at all* - no log file was created, so `main()`
never ran. `agent/src/main.c` names this exact symptom at its `log_init()` call:
no "main() entered" line means the failure was at **EXE LOAD**, before a single
instruction of ours executed.

Diffing the PE import tables of the 1.30.0 that runs there against the 1.78.0
that does not produced seven new names, every one NT-only:

    OpenSCManagerA  OpenServiceA  ControlService  QueryServiceStatus
    CloseServiceHandle  ChangeServiceConfigA
        -> Windows 9x has no Service Control Manager; its advapi32.dll
           exports none of that family.
    CM_Get_DevNode_Status
        -> setupapi.dll on NT, cfgmgr32.dll on 9x.

A static import the loader cannot resolve kills the WHOLE PROCESS at load time.
There is no lazy binding, no error dialog, and nothing on the box to point at
it. The fix (agent 1.79.0) resolves all seven through `agent/src/ntdyn.c` with
`GetProcAddress`, degrading to "this Windows has no SCM - skipping" on 9x.

WHY THIS TEST PARSES THE BINARY AND NOT THE SOURCE
--------------------------------------------------
A source grep would not have caught the regression: `OpenSCManagerA(...)` in
retrowall.c and `CM_Get_DevNode_Status(...)` in gamesync.c look like perfectly
ordinary C, and both files already sat beside modules that *did* resolve the
same functions dynamically. ONE direct call anywhere in the tree recreates the
import. The only honest post-condition is the import table of the linked EXE,
so that is what is asserted here.

This also became a SAFETY mechanism the moment .243 got an agent whose
auto-update thread works (`spawn_helper` passing NULL for lpThreadId was
rejected on 9x, which is why that box never pulled an update and never bricked
itself). From now on it pulls whatever the share advertises - so a future
refactor that re-adds an NT-only import would take the box dark with no
supervision, and recovery needs someone physically at the machine.

The four `SetupDi*` functions, `AdjustTokenPrivileges`, `OpenProcessToken` and
`LookupPrivilegeValueA` are imported by 1.30.0 as well and that binary runs
fine on this box: they resolve on Win98 and are deliberately NOT in the ban
list. Do not "helpfully" add them.
"""

import pathlib
import re
import shutil
import struct
import subprocess

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
AGENT_DIR = REPO / "agent"
AGENT_EXE = AGENT_DIR / "retro_agent.exe"
CC = "i686-w64-mingw32-gcc"

# The seven names that made 1.78.0 unloadable on Win98SE.
WIN9X_ABSENT = (
    "OpenSCManagerA",
    "OpenServiceA",
    "ControlService",
    "QueryServiceStatus",
    "CloseServiceHandle",
    "ChangeServiceConfigA",
    "CM_Get_DevNode_Status",
)

# Present in 1.30.0 too, and that binary runs on .243. The control group: if a
# future change makes the ban list "everything that looks NT-ish", these are
# what it would wrongly sweep up, and removing them is pure churn.
KNOWN_GOOD = (
    "SetupDiGetClassDevsA",
    "SetupDiEnumDeviceInfo",
    "SetupDiGetDeviceRegistryPropertyA",
    "SetupDiDestroyDeviceInfoList",
    "AdjustTokenPrivileges",
    "OpenProcessToken",
    "LookupPrivilegeValueA",
)

# ntdyn.c legitimately names them as GetProcAddress STRINGS; service.c has kept
# its own dynamic table since forever. Every other agent source must not name
# them at all.
DYNAMIC_RESOLVER_SOURCES = {"ntdyn.c", "ntdyn.h", "service.c"}

# The EIGHT DLLs agent v1.30.0 depends on. That binary demonstrably loads and
# runs on .243's Win98SE, so this set is proven, not assumed. A NEW DLL in the
# import table is the same class of fault as a new function: if Win98 does not
# ship it, the loader fails the process before main(). iphlpapi, shlwapi, ole32
# and shell32 are all used by the agent and are all loaded with LoadLibrary for
# exactly this reason - keep it that way.
WIN98_PROVEN_DLLS = {
    "advapi32.dll", "gdi32.dll", "kernel32.dll", "msvcrt.dll",
    "setupapi.dll", "user32.dll", "winmm.dll", "ws2_32.dll",
}


# ---------------------------------------------------------------------------
# A minimal PE import-table reader. No pefile dependency - the repo already
# hand-parses PE headers in scripts/fleet/pe-audit.py.
# ---------------------------------------------------------------------------
def pe_imports(data: bytes) -> dict:
    """{DLL name (lowercase): {imported function names}} for a PE32 image."""
    assert data[:2] == b"MZ", "not a DOS/PE image"
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "no PE signature"

    coff = e_lfanew + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    assert magic == 0x10B, "expected PE32 (the agent is 32-bit)"

    # PE32: data directories start at offset 96 in the optional header.
    imp_rva, imp_size = struct.unpack_from("<II", data, opt + 96 + 1 * 8)
    if not imp_rva:
        return {}

    sections = []
    sec = opt + opt_size
    for i in range(n_sections):
        off = sec + i * 40
        vaddr, rawsize, rawptr = struct.unpack_from("<III", data, off + 12)
        vsize = struct.unpack_from("<I", data, off + 8)[0]
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def to_off(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        return None

    def asciiz(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("ascii", "replace")

    out = {}
    d = to_off(imp_rva)
    assert d is not None, "import directory RVA is outside every section"
    while True:
        oft, _tds, _fc, name_rva, first_thunk = struct.unpack_from("<IIIII", data, d)
        if not (oft or name_rva or first_thunk):
            break
        dll = asciiz(to_off(name_rva)).lower()
        names = set()
        thunk_rva = oft or first_thunk
        t = to_off(thunk_rva)
        while True:
            entry = struct.unpack_from("<I", data, t)[0]
            if entry == 0:
                break
            if not entry & 0x80000000:          # not an ordinal import
                names.add(asciiz(to_off(entry) + 2))   # skip the 2-byte hint
            t += 4
        out.setdefault(dll, set()).update(names)
        d += 20
    return out


def _build_agent():
    """Build the agent, or skip loudly. Never assert on a missing toolchain."""
    if shutil.which(CC) is None:
        pytest.skip(
            "mingw-w64 (%s) not installed - CANNOT verify the agent's import "
            "table. This check is the only thing standing between a refactor "
            "and a bricked Win98 box; install the cross-compiler." % CC)
    r = subprocess.run(["make", "-j4"], cwd=AGENT_DIR,
                       capture_output=True, text=True)
    assert r.returncode == 0, (
        "the agent must build before its imports can be checked:\n"
        + r.stdout[-2000:] + r.stderr[-2000:])
    assert AGENT_EXE.is_file(), "make succeeded but %s is missing" % AGENT_EXE


@pytest.fixture(scope="module")
def imports():
    _build_agent()
    return pe_imports(AGENT_EXE.read_bytes())


def test_agent_imports_nothing_windows_9x_lacks(imports):
    every = {name for names in imports.values() for name in names}
    leaked = sorted(n for n in WIN9X_ABSENT if n in every)
    assert not leaked, (
        "retro_agent.exe statically imports %s. Windows 9x cannot resolve "
        "these, so the EXE FAILS TO LOAD on every 9x box - no log line, no "
        "error, the agent simply never starts and the machine goes dark with "
        "nothing supervising it. Resolve them through agent/src/ntdyn.c with "
        "GetProcAddress instead, and degrade gracefully when absent."
        % leaked)


def test_the_control_group_is_still_imported(imports):
    """These DO resolve on Win98 - 1.30.0 imports them and runs on .243.

    Asserted so the fix cannot be "widened" into removing working imports:
    a ban list that grows by resemblance rather than by evidence is how a
    one-line fix turns into a refactor nobody can review.
    """
    every = {name for names in imports.values() for name in names}
    missing = sorted(n for n in KNOWN_GOOD if n not in every)
    assert not missing, (
        "%s vanished from the import table. These are Win98-safe (agent 1.30.0 "
        "imports them and runs on .243); losing them means real functionality "
        "was removed, not a Win9x fix." % missing)


def test_the_agent_depends_on_no_dll_beyond_the_proven_eight(imports):
    """Verified 2026-08-30 by building v1.30.0 and diffing: its DLL set and
    v1.78.1's are IDENTICAL, and all 17 new function imports between them
    (GetFileTime, SetFileTime, QueryPerformanceCounter, FindWindowExA,
    SendMessageA, ...) are Windows 95-era ANSI entry points present in Win98SE.
    """
    extra = sorted(set(imports) - WIN98_PROVEN_DLLS)
    assert not extra, (
        "retro_agent.exe now links against %s. Windows 98SE may not ship it, "
        "and a missing DLL fails the process at LOAD exactly like a missing "
        "function does. Use LoadLibrary/GetProcAddress instead - that is why "
        "iphlpapi, ole32, shell32 and shlwapi are absent from this list." % extra)


def test_the_parser_would_actually_see_a_bad_import(imports):
    """A checker that can only say OK is the failure this project keeps paying
    for. Prove the parser reads real names out of the real binary."""
    every = {name for names in imports.values() for name in names}
    assert "GetProcAddress" in every, (
        "the import parser found no GetProcAddress - it is not reading the "
        "table, so its clean verdict on the banned names means nothing")
    assert len(every) > 100, (
        "only %d imports parsed; the agent has ~230 - the parser is truncating"
        % len(every))


def test_no_agent_source_calls_them_directly_except_the_resolvers():
    """Cheap, build-free companion: catch it at review time, not at link time.

    Not a substitute for the import-table check above (a call could arrive via
    a macro or a header), but it names the offending FILE, which the PE check
    cannot.
    """
    offenders = {}
    for src in sorted((AGENT_DIR / "src").glob("*.c")):
        if src.name in DYNAMIC_RESOLVER_SOURCES:
            continue
        text = src.read_text(errors="replace")
        # strip string literals so a GetProcAddress("OpenServiceA") is not a hit
        text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)
        for fn in WIN9X_ABSENT:
            if re.search(r"(?<![\w.>])" + fn + r"\s*\(", text):
                offenders.setdefault(src.name, []).append(fn)
    assert not offenders, (
        "these agent sources call NT-only entry points directly, which puts "
        "them in the import table and makes the EXE unloadable on Win98: %s. "
        "Call the ntdyn_* wrapper instead (agent/src/ntdyn.h)." % offenders)
