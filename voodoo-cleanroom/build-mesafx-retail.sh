#!/usr/bin/env bash
# build-mesafx-retail.sh — build our MesaFX OpenGL ICD (retro3dfx-gl) linked
# against the RETAIL (leading-underscore) glide3x import lib.
#
# WHY THIS VARIANT EXISTS
# -----------------------
# build-stack.sh links MesaFX against our OWN glide3x import lib (libglide3x.dll.a),
# which imports the Glide entry points as `grFoo@N` (no leading underscore) — the
# naming our retro3dfx-glide fork exports. That is correct only when the machine
# also runs OUR display driver (vcr-disp) + OUR glide3x.dll.
#
# On a fleet box that instead has a retail/AmigaMerlin 3dfx driver installed, the
# system glide3x.dll exports `_grFoo@N` (retail MSVC decoration, WITH leading
# underscore). Our default MesaFX cannot bind to it, so the OpenGL ICD fails to
# load and the game silently falls back to the Microsoft "Direct3D GL 1.1" wrapper.
# (Confirmed on .124: AmigaMerlin glide3x.dll exports `_grBufferSwap@4`; our default
# opengl32.dll imports `grBufferSwap@4` -> mismatch -> LoadLibrary fails.)
#
# This script relinks MesaFX against libglide3x_retail.dll.a so the resulting
# opengl32.dll imports `_grFoo@N` and binds the retail/AmigaMerlin glide3x. Deploy
# the output as the game's r_glDriver DLL (e.g. Quake3 `retrogl.dll`).
#
# RESULT (Voodoo3 on .124, Quake III 1.32):
#   GL_VENDOR:   Brian Paul
#   GL_RENDERER: Mesa Glide v0.62 Voodoo3 (tm)   <- our stack, hardware accelerated
#   GL_VERSION:  1.2 Mesa 6.2.2
#
# Prereqs: build-stack.sh has been run once (provides out/sdk/include headers).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CROSS=i686-w64-mingw32-
CPU=pentium3            # -march floor: must stay P3, .124 has no SSE2
TUNE="${TUNE:-pentium4}" # -mtune: schedule for the P4 that runs the Voodoo 2 box; override
                        # for an A/B (TUNE=athlon-xp: the Athlon XP in .124) - the -march floor stays
WORK="$HERE/build"; OUT="$HERE/out"
RETAIL_LIB="$HERE/../scripts/3dfx/glide-sdk/lib/libglide3x_retail.dll.a"

command -v ${CROSS}gcc >/dev/null || { echo "FATAL: ${CROSS}gcc missing"; exit 1; }
[ -f "$RETAIL_LIB" ] || { echo "FATAL: retail import lib missing: $RETAIL_LIB"; exit 1; }
[ -d "$OUT/sdk/include" ] || { echo "FATAL: run build-stack.sh first (need out/sdk/include)"; exit 1; }

mkdir -p "$WORK"; cd "$WORK"
[ -d retro3dfx-gl ] || git clone -q "https://github.com/voidsstr/retro3dfx-gl.git"
GLTREE="$WORK/retro3dfx-gl"
mkdir -p "$GLTREE/glide3/include" "$GLTREE/glide3/lib"
cp "$OUT/sdk/include/"*.h "$GLTREE/glide3/include/"
# KEY: retail (underscore) import lib instead of our own
cp "$RETAIL_LIB" "$GLTREE/glide3/lib/libglide3x.a"

# --- voodoo-cleanroom driver versioning -------------------------------------------
# MAJOR.MINOR comes from voodoo-cleanroom/VERSION; BUILD auto-increments every build
# (.buildnum). Since 0.1.83 the full version is in GL_VERSION (fxdd.c rgl_build_tag),
# so every game log / benchmark still self-documents which driver build produced it:
#   GL_VERSION: "1.2 Mesa 6.2.2 [voodoo-cleanroom 0.1.83]"
# and GL_RENDERER is the same for every build ("Mesa Glide v0.62 Voodoo5 6000 (tm)"):
# id Tech 3 and Torque remember the renderer string and reset their graphics to the
# low preset when it changes, which a build number in it did after every update.
VER_MM="$(cat "$HERE/VERSION" 2>/dev/null || echo 0.1)"
BUILD=$(( $(cat "$HERE/.buildnum" 2>/dev/null || echo 0) + 1 ))
echo "$BUILD" > "$HERE/.buildnum"
DRVVER="$VER_MM.$BUILD"
FXAPI="$GLTREE/src/mesa/drivers/glide/fxapi.c"
FXDD="$GLTREE/src/mesa/drivers/glide/fxdd.c"
FXDRV="$GLTREE/src/mesa/drivers/glide/fxdrv.h"
# widen rendererString (stock 64B is tight for "Mesa Glide v0.62 Voodoo5 6000 (tm) SLI") - idempotent
sed -i 's/char rendererString\[64\];/char rendererString[96];/' "$FXDRV"
# no build tag in the renderer string - strip one an older source still carries
sed -i 's/ \[voodoo-cleanroom [0-9.]*\]//' "$FXAPI"
! grep -q "voodoo-cleanroom" "$FXAPI" || { echo "FATAL: a build tag is still in fxapi.c (the renderer string)"; exit 1; }
# refresh the tag GL_VERSION carries - idempotent
sed -i "s/\[voodoo-cleanroom [0-9.]*\]/[voodoo-cleanroom $DRVVER]/" "$FXDD"
grep -q "\[voodoo-cleanroom $DRVVER\]" "$FXDD" || { echo "FATAL: version inject failed (fxdd.c rgl_build_tag)"; exit 1; }
echo "== driver version: $DRVVER =="
# gcc-13 portability (idempotent)
sed -i 's/CFLAGS = -Wall -Werror/CFLAGS = -Wall -Wno-array-bounds -Wno-stringop-overflow -fcommon/' "$GLTREE/Makefile.mgw" || true
make -C "$GLTREE" -f Makefile.mgw clean >/dev/null 2>&1 || true
echo "== building MesaFX (retail glide3x link) =="
make -C "$GLTREE" -f Makefile.mgw FX=1 X86=1 CPU="$CPU" TUNE="$TUNE" GLIDE="$GLTREE/glide3" \
     CC="${CROSS}gcc" AR="${CROSS}ar rcu" RANLIB="${CROSS}ranlib" \
     DLLTOOL="${CROSS}dlltool" AS="${CROSS}gcc -c -x assembler-with-cpp" \
     RC="${CROSS}windres" >/tmp/mesa_retail.log 2>&1 \
  || { echo "BUILD FAILED (tail):"; tail -20 /tmp/mesa_retail.log; exit 1; }
DLL="$(find "$GLTREE" -iname '*mesa32.dll' -o -iname 'opengl32.dll' 2>/dev/null | head -1)"
[ -n "$DLL" ] || { echo "no output dll"; exit 1; }
cp "$DLL" "$OUT/opengl32_retail.dll"
cp "$DLL" "$OUT/opengl32_retail_v$DRVVER.dll"           # versioned archive
printf '%s\n' "$DRVVER" > "$OUT/opengl32_retail.dll.ver"  # sidecar
echo "output: $OUT/opengl32_retail.dll v$DRVVER ($(stat -c%s "$OUT/opengl32_retail.dll") bytes)"
# sanity: must import underscore-decorated glide3x names
# (no `grep -q`: with pipefail, -q's early exit SIGPIPEs objdump -> false negative)
if ${CROSS}objdump -p "$OUT/opengl32_retail.dll" | grep '_grBufferSwap@4' >/dev/null; then
    echo "OK: imports _grFoo@N (binds retail/AmigaMerlin glide3x)"
else
    echo "WARN: expected underscore glide3x imports not found"
fi
