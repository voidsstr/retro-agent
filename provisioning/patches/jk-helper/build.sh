#!/bin/bash
# Build JKMODE.EXE - see jkmode.c. Usage: build.sh [OUTDIR]  (default ~/.retro-fleet/patch-out/jk-helper)
#
# -march=pentium -mno-sse: the fleet includes a P166 and Pentium III / no-SSE2 boxes.
# -nostdlib -nostartfiles: no C runtime at all (a mingw msvcrt helper has hung on
#   Win98 here before), and - just as important - no mingw default-manifest.o:
#   the games have no manifest, so under UAC they are registry-virtualized, and a
#   manifested helper would write where the game does not read.
# ddraw.dll is LoadLibrary'd, so the import table is KERNEL32/USER32/ADVAPI32 only;
# apply.py and the test assert that from `objdump -p`.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HOME/.retro-fleet/patch-out/jk-helper}"
CC="${CC:-i686-w64-mingw32-gcc}"
mkdir -p "$OUT"
"$CC" -Os -march=pentium -mno-sse -mno-sse2 -mno-mmx -ffreestanding -fno-builtin \
      -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables \
      -Wall -Wextra -Wno-unused-function -Werror \
      -nostdlib -nostartfiles -Wl,-e,_jkmode_entry -Wl,--subsystem,console \
      -Wl,--no-insert-timestamp -s \
      -o "$OUT/JKMODE.EXE" "$HERE/jkmode.c" -lkernel32 -luser32 -ladvapi32 -lgcc
echo "$OUT/JKMODE.EXE"
