"""agent/lib/libmsvcrt.a — the Win98 CRT import-lib shim must stay valid.

Encodes the 2026-08-25 fix. The committed patched import library had been built
against an older MinGW-w64 and predated `_initterm_e`, which the current
crt2.o references. Because `-Llib` makes this copy SHADOW the system one, the
whole agent stopped linking:

    crt2.o: undefined reference to `_initterm_e'
    crt2.o: undefined reference to `_crt_atexit'

That is a total build outage, not a warning, and it is invisible until someone
tries to build the agent — which is exactly when you need it. So assert both
halves of what the shim is for:

  1. the Win98SE-absent symbols are STILL removed (the original purpose:
     Win98's msvcrt.dll exports no _strtoi64/_strtoui64, and importing them
     makes the exe fail to LOAD, with no lazy binding to save you), and
  2. the symbols modern crt2.o needs are STILL present (the regression).

Regenerate with agent/lib/make-libmsvcrt.sh after a MinGW upgrade.
"""
import pathlib
import shutil
import subprocess

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
LIB = REPO / "agent" / "lib" / "libmsvcrt.a"
NM = "i686-w64-mingw32-nm"

# Not in Win98SE's msvcrt.dll. Importing any of these makes the agent fail to
# load on a 9x box, so they must not be resolvable from this library.
WIN98_ABSENT = ("__imp___strtoi64", "__imp___strtoui64",
                "__imp___strtoi64_l", "__imp___strtoui64_l")

# Referenced by the MinGW-w64 startup object; without them nothing links.
# NOTE these are the *nm* spellings. ld reports the C-level name in its error
# ("undefined reference to `_initterm_e'") but the symbol carries the leading
# underscore on top of that, so nm prints __initterm_e. Matching ld's spelling
# here silently never matches and the test fails on a perfectly good library.
CRT_REQUIRED = ("__initterm_e", "__crt_atexit")

pytestmark = pytest.mark.skipif(
    shutil.which(NM) is None, reason="mingw-w64 binutils not installed")


def _symbols():
    assert LIB.is_file(), f"{LIB} is missing - run agent/lib/make-libmsvcrt.sh"
    out = subprocess.run([NM, str(LIB)], capture_output=True, text=True).stdout
    # An import library lists each symbol on its own line; take the last field
    # so we compare whole names and never substring-match (__imp___strtoi64
    # must not be satisfied by __imp___strtoi64_l).
    return {line.split()[-1] for line in out.splitlines() if line.split()}


def test_win98_absent_imports_are_removed():
    syms = _symbols()
    leaked = sorted(s for s in WIN98_ABSENT if s in syms)
    assert not leaked, (
        f"{LIB.name} re-exposes symbols Win98SE's msvcrt.dll does not have: "
        f"{leaked}. An agent linking these fails to LOAD on every 9x box. "
        f"Regenerate with agent/lib/make-libmsvcrt.sh.")


def test_modern_crt_startup_symbols_are_present():
    syms = _symbols()
    missing = sorted(s for s in CRT_REQUIRED if s not in syms)
    assert not missing, (
        f"{LIB.name} is missing {missing}, which crt2.o references. Because "
        f"-Llib shadows the system libmsvcrt.a, the agent will not link at "
        f"all. This is the 2026-08-25 outage. Regenerate with "
        f"agent/lib/make-libmsvcrt.sh.")


def test_regeneration_script_exists_and_is_runnable():
    script = REPO / "agent" / "lib" / "make-libmsvcrt.sh"
    assert script.is_file(), "the shim must stay reproducible, not hand-carved"
    # Strip comments before checking: the script explains WHY it avoids
    # pipefail, so a naive substring search matches its own warning comment.
    body = "\n".join(line for line in script.read_text().splitlines()
                     if not line.lstrip().startswith("#"))
    assert "pipefail" not in body, (
        "make-libmsvcrt.sh must not use pipefail: `ar t | grep -q` and "
        "`nm | grep -q` both SIGPIPE their producer on an early match, which "
        "silently inverted both the removal and its verification.")
