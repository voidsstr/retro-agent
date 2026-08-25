#!/usr/bin/env bash
# Regenerate lib/libmsvcrt.a from the host's MinGW-w64 import library.
#
# Why this exists: Win98SE's msvcrt.dll does not export _strtoi64/_strtoui64.
# If anything links their import stubs the whole exe fails to LOAD on Win98 --
# there is no lazy binding to save you. So we take the system import lib and
# delete exactly the two members that provide them, and put the result on -L
# ahead of the system copy.
#
# Run this after a MinGW-w64 upgrade. The previously committed copy was built
# against an older MinGW and lacked _initterm_e, which newer crt2.o requires,
# so it broke the link outright (undefined _initterm_e / _crt_atexit).
#
# NOTE: no `set -o pipefail` here, deliberately. `ar t | grep -q` makes grep
# exit on first match, ar dies of SIGPIPE, and under pipefail the pipeline
# reports failure even though the member WAS found -- which silently skipped
# the removal and then passed the verification for the same reason.
set -eu
cd "$(dirname "$0")"
SYS=$(i686-w64-mingw32-gcc -print-file-name=libmsvcrt.a)
[ -f "$SYS" ] || { echo "cannot find system libmsvcrt.a" >&2; exit 1; }
cp "$SYS" libmsvcrt.a
chmod u+w libmsvcrt.a

members=$(i686-w64-mingw32-ar t libmsvcrt.a)
# The _l locale variants are equally absent from Win98SE's msvcrt.dll.
for m in lib32_libmsvcrt_extra_a-strtoimax.o lib32_libmsvcrt_extra_a-strtoumax.o \
         libmsvcrt_defs00832.o libmsvcrt_defs00834.o; do
    if printf '%s\n' "$members" | grep -qx "$m"; then
        i686-w64-mingw32-ar d libmsvcrt.a "$m"
        echo "removed $m"
    else
        echo "note: $m not present (member names may have changed)" >&2
    fi
done
i686-w64-mingw32-ranlib libmsvcrt.a

syms=$(i686-w64-mingw32-nm libmsvcrt.a 2>/dev/null || true)
if printf '%s\n' "$syms" | grep -qE '__imp___strtoi64(_l)?$|__imp___strtoui64(_l)?$'; then
    echo "FAIL: Win98-absent strtoi64 imports still present" >&2
    printf '%s\n' "$syms" | grep -E '__imp___strtoi64(_l)?$|__imp___strtoui64(_l)?$' >&2
    exit 1
fi
if ! printf '%s\n' "$syms" | grep -q '_initterm_e'; then
    echo "FAIL: _initterm_e missing -- crt2.o will not link" >&2; exit 1
fi
echo "OK: $(stat -c%s libmsvcrt.a) bytes"
