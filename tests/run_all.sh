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
if ls "$NAT"/test_*.c >/dev/null 2>&1; then
  for src in "$NAT"/test_*.c; do
    name="$(basename "$src" .c)"
    if gcc -std=c11 -O0 -g -Wall -I"$NAT" -I"$NAT/stubs" "$src" -lm -o "$OBJ/$name" 2>"$OBJ/$name.log"; then
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

echo; echo "=================================================================="
[ $rc -eq 0 ] && echo " ALL SUITES PASSED" || echo " SOME SUITES FAILED (rc=$rc)"
echo "=================================================================="
exit $rc
