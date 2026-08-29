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
# Hand the compiler down. The sibling retro-3dfx harness in section [3] takes
# CC="${CC:-gcc}", so exporting ours makes it work there too without editing
# another lane's repo — and without it, section [3] fails with the same
# "gcc: command not found" this block exists to solve.
[ -n "$CC" ] && export CC

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
# Discover the sibling repo instead of requiring $DFX. It was gated on that
# variable alone, which nobody exports, so this section never ran — and then
# reported "not found" for a harness that was sitting right next to us. A skip
# reason that names the wrong cause is worse than no message: it sends you
# looking for a missing file instead of an unset variable.
# This one is a genuinely OPTIONAL sibling (a host may legitimately not have
# the driver repo), so an honest skip is fine — unlike section [2], which is
# ours and must fail when it cannot run.
if [ -z "${DFX:-}" ]; then
  for cand in "$(dirname "$(dirname "$HERE")")/../retro-3dfx" \
              /mnt/c/development/retro-3dfx "$HOME/development/retro-3dfx"; do
    [ -x "$cand/tests/run_native.sh" ] && { DFX="$cand"; break; }
  done
fi
if [ -n "${DFX:-}" ] && [ -x "$DFX/tests/run_native.sh" ]; then
  echo "  repo: $DFX"
  bash "$DFX/tests/run_native.sh" || rc=1
elif [ -n "${DFX:-}" ]; then
  echo "  (skipped: DFX=$DFX has no executable tests/run_native.sh)"
else
  echo "  (skipped: no retro-3dfx checkout found; set DFX=/path/to/retro-3dfx)"
fi

# DOS lane. Needs Open Watcom + a dosbox that runs headless; both are optional
# on a given dev host, and the script skips (exit 0) when they are missing.
echo; echo "### [4] DOS game manager tests (DOSBox) ###"
if [ -x "$REPO/scripts/dosgames/tests/run_dos_tests.sh" ]; then
  bash "$REPO/scripts/dosgames/tests/run_dos_tests.sh" || rc=1
else
  echo "  (skipped: scripts/dosgames/tests/run_dos_tests.sh not found)"
fi

# Login-screen fleet dashboard. Pure-logic tests: the collector's publish/merge
# behaviour under pytest, and the omenfan-derived render primitives under gjs.
# No GNOME session and no hardware needed. gjs is optional on a headless host.
echo; echo "### [5] Login-screen dashboard (collector + render) ###"
if [ -f "$REPO/dashboard/tests/test_dashboard_collector.py" ]; then
  python3 -m pytest -q "$REPO/dashboard/tests/test_dashboard_collector.py" || rc=1
else
  echo "  (skipped: dashboard/tests not found)"
fi
if command -v gjs >/dev/null 2>&1; then
  gjs -m "$REPO/dashboard/tests/test_render.js" || rc=1
else
  echo "  (skipped render tests: gjs not installed)"
fi
# The service panels (game servers, favourites agent, PXE, host services) are
# driven through node with the GI imports stubbed, because what needs testing
# is their behaviour on the degenerate inputs a not-yet-started service
# produces -- not GNOME. See dashboard/tests/stub-gi.mjs.
if command -v node >/dev/null 2>&1; then
  ( cd "$REPO" && node --import ./dashboard/tests/stub-gi.mjs \
      dashboard/tests/test_panels.mjs ) || rc=1
else
  echo "  (skipped panel tests: node not installed)"
fi

### [6] PXE / unattended-image invariants ###
#
# These live at tests/*.py rather than tests/python/, and pytest.ini sets
# `testpaths = python`, so until now NOTHING ran them - they were written,
# committed, and then silently never executed again. Run them explicitly.
#
# They are standalone scripts (their own PASS/FAIL + exit code), not pytest
# modules, because most of them assert against the STAGED IMAGE on the SMB
# share and must degrade to SKIP when it is not mounted rather than error.
echo; echo "### [6] PXE / unattended-image invariants ###"
for t in "$HERE"/test_pxe_*.py "$HERE"/test_binl.py; do
  [ -f "$t" ] || continue
  echo "-- $(basename "$t")"
  python3 "$t" || rc=1
done

echo; echo "=================================================================="
[ $rc -eq 0 ] && echo " ALL SUITES PASSED" || echo " SOME SUITES FAILED (rc=$rc)"
echo "=================================================================="
exit $rc
