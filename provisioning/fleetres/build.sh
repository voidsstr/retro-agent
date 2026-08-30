#!/bin/bash
# Build FLEETRES.EXE - and build it so a GENUINE PENTIUM 1 can execute it.
#
# THIS SCRIPT EXISTS BECAUSE THE FLAGS ARE NOT OPTIONAL, and the obvious build
# line is wrong in a way nothing on an XP box can show you.
#
#     i686-w64-mingw32-gcc -O2 -s -o FLEETRES.EXE fleetres.c -ladvapi32 -luser32
#
# That is what the header comment used to say, and it produces a binary with
# 78 CMOV instructions in it. CMOV is a Pentium PRO instruction. A genuine
# Pentium (P54C/P55C) raises STATUS_ILLEGAL_INSTRUCTION 0xC000001D on the first
# one - an instant hard crash, not a slow frame rate.
#
# That matters more here than anywhere else in the tree, because FLEETRES.EXE
# is not a tool someone runs occasionally: it is staged into 32 game trees and
# `call`ed by the FIRST LINE of every "Play <Game>.bat". On a Pentium-1 box the
# whole staged library would fail at launch, every title, with an error that
# names our helper rather than the game.
#
# Where the 78 came from, and how they go away:
#   * 25 are in OUR code, from gcc's default i686 baseline -> -march=i586.
#   * 51 are inside MINGW'S OWN printf (__mingw_pformat, __pformat_*, __gdtoa),
#     which is prebuilt for i686 -> -D__USE_MINGW_ANSI_STDIO=0 routes
#     printf/snprintf to the box's own msvcrt.dll instead. fleetres.c uses only
#     %d %s %ld %lu %.2f %.400s, every one of which msvcrt handles, so this
#     costs nothing. It also halves the binary: 59,392 -> 30,208 bytes.
#   * 2 remain and are DEAD: _mark_section_writable and __GetPEImageBase are
#     libgcc's pseudo-relocator helpers, never called when the image has no
#     runtime pseudo-relocs. agent/Makefile documents the same two for the same
#     reason.
#
# NONE OF THIS IS NEW KNOWLEDGE. agent/Makefile has carried the whole recipe -
# and the note that it "surfaced on a Compaq Deskpro 2000 (Pentium 1)" - since
# the agent was made P5-safe. FLEETRES.EXE was written later and did not
# inherit it, which is exactly the shape of failure worth a build script: the
# fix was already written down one directory away.
#
# Verified 2026-08-30 on .240 (Athlon 64, XP SP3): the P5-safe build's `-cmd`
# and `-info` output is byte-for-byte identical to the old binary's.
set -eu

cd "$(dirname "$0")"
CC="${CC:-i686-w64-mingw32-gcc}"
OUT="${OUT:-FLEETRES.EXE}"

$CC -O2 -s \
    -D__USE_MINGW_ANSI_STDIO=0 \
    -march=i586 -mtune=pentium3 \
    -fno-stack-protector \
    -o "$OUT" fleetres.c -ladvapi32 -luser32 -lm

# SELF-CHECK, because a build that silently regressed this would ship to 32
# game trees and only fail on the one box nobody tests on. Two dead CMOVs are
# tolerated by name; anything else is a hard failure.
if command -v objdump >/dev/null 2>&1; then
    live=$(objdump -d -M intel --no-show-raw-insn "$OUT" 2>/dev/null \
           | grep -cE '\bcmov' || true)
    if [ "$live" -gt 2 ]; then
        echo "BUILD FAILED: $OUT contains $live CMOV instructions." >&2
        echo "  CMOV is Pentium Pro and later. A genuine Pentium 1 raises" >&2
        echo "  0xC000001D on the first one, and this binary is called by the" >&2
        echo "  first line of every staged game's launcher." >&2
        exit 1
    fi
    echo "$OUT: $(stat -c%s "$OUT") bytes, $live CMOV (<=2 dead ones) - P5-safe"
else
    echo "$OUT: $(stat -c%s "$OUT") bytes (objdump absent - CMOV NOT verified)"
fi
