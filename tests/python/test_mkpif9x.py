"""scripts/fleet/win9x/mkpif9x.c - a shortcut for a DOS program on Windows 9x.

WHAT WAS MEASURED (.243, Win98 SE, 2026-09-29), three PIFs side by side, each
proved to have run by a marker file its batch wrote:

    batch leaves text on screen, PIF byte 63h = 00  -> "Finished - T_PLAIN" stays
    same batch, PIF byte 63h = 10 (mkpif9x's patch)  -> window closes
    batch ends with CLS, PIF byte 63h = 00           -> window closes

So bit 4 of byte 63h in a PIF's basic section IS "Close on exit" - the tool's
comment had called it presumed. The Win9x shell writes <name>.pif for an
MS-DOS target whatever name it is handed (the same fact behind agent 1.93.1).

These tests pin the two ways the tool could hurt a box: writing anything but
that one bit, and an import Windows 9x cannot resolve (which kills the process
at load with no message at all - see test_agent_win9x_imports.py).
"""
import pathlib
import re
import shutil
import subprocess

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "scripts" / "fleet" / "win9x" / "mkpif9x.c"


def _code():
    s = SRC.read_text()
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def _body(code, sig):
    i = code.index(sig)
    j = code.index("{", i)
    depth = 0
    for k in range(j, len(code)):
        depth += {"{": 1, "}": -1}.get(code[k], 0)
        if depth == 0:
            return code[j:k + 1]
    raise AssertionError(sig)


def test_the_patch_sets_one_bit_of_one_byte():
    code = _code()
    setter = _body(code, "static int pif_set_bit(const char *pif, DWORD off, unsigned char bit)")
    assert setter.count("WriteFile(") == 1, "exactly one write"
    assert "b |= bit" in setter, "the bit is set, nothing cleared"
    assert re.search(r"if \(b & bit\)\s*ok = 1;", setter), "already set: no write at all"
    assert "GetFileSize(h, NULL) > off" in setter, "a file too short is left alone"
    close = _body(code, "static int pif_close_on_exit(const char *pif)")
    assert "pif_set_bit(pif, 0x63, 0x10)" in close        # measured: Close on exit
    dos = _body(code, "static int pif_msdos_mode(const char *pif, int with_1b0)")
    assert "pif_set_bit(pif, 0x1AF, 0x80)" in dos         # diffed: MS-DOS mode
    assert re.search(r"if \(ok && with_1b0\)\s*ok = pif_set_bit\(pif, 0x1B0, 0x10\);", dos)
    # nothing else in the file writes
    assert code.count("WriteFile(") == 3, "the one PIF write plus the two log writes"


def test_only_a_pif_is_ever_patched_and_options_are_explicit():
    main = _body(_code(), "void WINAPI _start(void)")
    first_patch = main.index("pif_close_on_exit(pif)")
    assert "GetFileAttributesA(pif)" in main[:first_patch], "the .pif must exist first"
    assert "no_close ? -1 : pif_close_on_exit(pif)" in main
    assert "msdos ? pif_msdos_mode(pif, msdos_1b0) : -1" in main
    assert "pif_close_on_exit(lnk)" not in main and "pif_msdos_mode(lnk" not in main
    assert '"FAIL: unknown option %s"' in main, "a mistyped option must not be ignored"
    assert "MS-DOS mode needs a .pif" in main


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    cc = shutil.which("i686-w64-mingw32-gcc")
    objdump = shutil.which("i686-w64-mingw32-objdump")
    if not cc or not objdump:
        pytest.skip("no mingw cross-compiler - mkpif9x's imports were NOT checked")
    out = tmp_path_factory.mktemp("mkpif") / "mkpif9x.exe"
    subprocess.run([cc, "-O1", "-march=i586", "-mwindows", "-nostdlib", "-e", "_start@0",
                    "-o", str(out), str(SRC), "-lkernel32", "-luser32", "-lole32",
                    "-luuid", "-s"], check=True)
    return subprocess.run([objdump, "-p", str(out)], capture_output=True, text=True,
                          check=True).stdout


def test_it_imports_only_what_windows_98_resolves(built):
    dlls = {m.lower() for m in re.findall(r"DLL Name: (\S+)", built)}
    assert dlls <= {"kernel32.dll", "user32.dll", "ole32.dll"}, dlls
    # objdump -p: "<vma>  <ordinal|<none>>  <hint>  <name>"
    names = set(re.findall(r"^\s+[0-9a-f]{8}\s+\S+\s+[0-9a-f]{4}\s+(\w+)\s*$",
                           built, flags=re.M))
    assert {"CreateFileA", "WriteFile", "CoCreateInstance"} <= names, (
        "the import listing was not parsed - this test would pass on anything", names)
    # Win9x's kernel32 has only stubs for most W functions; the one W-to-A
    # bridge used here, MultiByteToWideChar, is real on 98. Nothing NT-only.
    for n in names:
        if n.endswith("W"):
            assert n in {"MultiByteToWideChar"}, n
    assert not names & {"lstrcpynW", "GetFileAttributesExA", "SetFilePointerEx",
                        "GetFileSizeEx", "CoInitializeEx"}, names
