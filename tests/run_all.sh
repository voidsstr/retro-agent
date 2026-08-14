#!/bin/bash
# Retro driver-stack regression suite — top-level runner.
# Runs the Python client-protocol tests here, plus the native C driver-logic
# tests in the sibling retro-3dfx repo if present. Exit non-zero on any failure.
#
# Usage: bash tests/run_all.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
DFX="$(cd "$REPO/../retro-3dfx" 2>/dev/null && pwd || true)"
rc=0

echo "=================================================================="
echo " Retro driver-stack regression suite"
echo "=================================================================="

echo; echo "### [1] Python client protocol/discovery tests ###"
if command -v pytest >/dev/null 2>&1; then
  ( cd "$HERE" && pytest ) || rc=1
else
  ( cd "$HERE" && python3 -m pytest ) || rc=1
fi

echo; echo "### [2] Native C agent-logic tests (true-source) ###"
NAT="$HERE/native"; OBJ="$HERE/.obj"; mkdir -p "$OBJ"

# Find a host compiler. This used to invoke bare "gcc", which is NOT on PATH on
# the dev box — so every native test failed to BUILD rather than to pass, and
# the whole suite was silently unrunnable for a full working session while
# reporting only "[BUILD FAIL] gcc: command not found". That hid a real
# regression: dosstage.c gained FILETIME/CompareFileTime and its true-source
# test stopped compiling, which nobody could see.
# Override with CC=... ; DOSGAME_TOOLCHAIN points at an unpacked toolchain tree.
: "${CC:=}"
if [ -z "$CC" ]; then
  for cand in gcc cc clang; do
    command -v "$cand" >/dev/null 2>&1 && { CC="$cand"; break; }
  done
fi
if [ -z "$CC" ]; then
  for tc in "${DOSGAME_TOOLCHAIN:-}" "$HOME/toolchain-mingw"; do
    [ -n "$tc" ] && [ -x "$tc/hostbin/gcc" ] || continue
    CC="$tc/hostbin/gcc"
    # cc1 needs the toolchain's own libisl/libmpc, which the host lacks
    export LD_LIBRARY_PATH="$tc/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    break
  done
fi
if [ -z "$CC" ]; then
  echo "  [SKIP] no host C compiler found (tried gcc/cc/clang, \$CC," \
       "\$DOSGAME_TOOLCHAIN, ~/toolchain-mingw)"
  echo "         the native suites did NOT run - this is not a pass"
  rc=1
elif ls "$NAT"/test_*.c >/dev/null 2>&1; then
  echo "  cc: $CC"
  for src in "$NAT"/test_*.c; do
    name="$(basename "$src" .c)"
    if "$CC" -std=c11 -O0 -g -Wall -I"$NAT" -I"$NAT/stubs" "$src" -lm -o "$OBJ/$name" 2>"$OBJ/$name.log"; then
      "$OBJ/$name" || rc=1
    else
      echo "  [BUILD FAIL] $name"; cat "$OBJ/$name.log"; rc=1
    fi
  done
else
  echo "  (no agent native tests)"
fi

echo; echo "### [3] Native C driver-logic tests (retro-3dfx/tests) ###"
if [ -n "${DFX:-}" ] && [ -x "$DFX/tests/run_native.sh" ]; then
  bash "$DFX/tests/run_native.sh" || rc=1
else
  echo "  (skipped: retro-3dfx/tests/run_native.sh not found)"
fi

# DOS lane. Needs Open Watcom + a dosbox that runs headless; both are optional
# on a given dev host, and the script skips (exit 0) when they are missing.
echo; echo "### [4] DOS game manager tests (DOSBox) ###"
if [ -x "$REPO/scripts/dosgames/tests/run_dos_tests.sh" ]; then
  bash "$REPO/scripts/dosgames/tests/run_dos_tests.sh" || rc=1
else
  echo "  (skipped: scripts/dosgames/tests/run_dos_tests.sh not found)"
fi

echo; echo "=================================================================="
[ $rc -eq 0 ] && echo " ALL SUITES PASSED" || echo " SOME SUITES FAILED (rc=$rc)"
echo "=================================================================="
exit $rc
