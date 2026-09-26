#!/usr/bin/env python3
"""check_imports.py - will XP actually LOAD this driver binary?

mingw-w64's import libraries describe a modern Windows: libntoskrnl.a
exports ~700 names XP SP3 does not have (memcmp among them). A driver that
imports one of them links cleanly and then fails to load on the box - at boot,
for a display driver, which is the worst possible time to find out.

So every import of every built binary is checked against the export tables of
the real XP SP3 binaries (tools/xp_exports/*.txt, extracted from the I386
CABs), and each binary may import only from the DLLs its role allows:

    vcrmp.sys (miniport)  videoprt.sys, ntoskrnl.exe, hal.dll
    vcrdd.dll (display)   win32k.sys   (GDI loads nothing else into session space)

Also checked: subsystem NATIVE, a base-relocation table (the kernel loader
relocates drivers), and a PE checksum that is either 0 or correct (a stale
non-zero one fails with STATUS_IMAGE_CHECKSUM_MISMATCH).

    check_imports.py out/vcrmp.sys out/vcrdd.dll      exit 0 = loadable
"""
import sys
from pathlib import Path

import pefile

HERE = Path(__file__).resolve().parent
EXPORTS = HERE / "xp_exports"

ALLOWED = {
    ".sys": {"videoprt.sys", "ntoskrnl.exe", "hal.dll"},
    ".dll": {"win32k.sys"},
}


def xp_exports(dll):
    f = EXPORTS / (dll.lower() + ".txt")
    if not f.exists():
        return None
    return {ln.strip() for ln in f.read_text().splitlines()
            if ln.strip() and not ln.startswith("#")}


def check(path):
    problems = []
    pe = pefile.PE(str(path))
    role = ALLOWED.get(Path(path).suffix.lower())
    if pe.OPTIONAL_HEADER.Subsystem != 1:
        problems.append(f"subsystem {pe.OPTIONAL_HEADER.Subsystem}, want 1 (native)")
    if not hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        problems.append("no base relocations")
    ck = pe.OPTIONAL_HEADER.CheckSum
    if ck and ck != pe.generate_checksum():
        problems.append(f"stale PE checksum {ck:#x} (want {pe.generate_checksum():#x} or 0)")
    n = 0
    for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = imp.dll.decode().lower()
        if role is not None and dll not in role:
            problems.append(f"imports from {dll}, not allowed for this role ({sorted(role)})")
            continue
        names = xp_exports(dll)
        for sym in imp.imports:
            n += 1
            if sym.name is None:
                problems.append(f"{dll}: import by ordinal {sym.ordinal}")
                continue
            name = sym.name.decode()
            if names is not None and name not in names:
                problems.append(f"{dll}!{name} does not exist on XP SP3")
    return n, problems


def main(argv):
    rc = 0
    for p in argv[1:]:
        n, problems = check(p)
        if problems:
            rc = 1
            print(f"FAIL {p}: {len(problems)} problem(s) in {n} imports")
            for pr in problems:
                print(f"     {pr}")
        else:
            print(f"ok   {p}: {n} imports, all present on XP SP3")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
