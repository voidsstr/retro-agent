#!/usr/bin/env bash
# build-stack.sh - build the voodoo-cleanroom user-mode stack from OUR forks.
#
# Produces, from source we own and can optimize:
#   out/glide3x.dll   <- retro3dfx-glide  (our fork of sezero/glide)   [Voodoo4/5 h5]
#   out/glide3x_h3.dll<- retro3dfx-glide  (Voodoo3)
#   out/glide2x.dll   <- retro3dfx-glide  (Glide2, Win98 games)
#   out/opengl32.dll  <- retro3dfx-gl     (our MesaFX fork -> Q3 OpenGL ICD)
#   out/sdk/          <- Glide3 SDK (headers + import libs) both DLLs share
#
# The kernel/display layer (vcr-disp) needs the DDK and builds separately
# (disp/, via the fleet DDK). This script builds everything DDK-independent.
#
# Forks (created under github.com/voidsstr):
#   voidsstr/retro3dfx-glide <- sezero/glide
#   voidsstr/retro3dfx-gl    <- sezero/MesaFX-6.2
#
# Usage: ./build-stack.sh [workdir]   (default: ./build)   [--debug]

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CROSS=i686-w64-mingw32-
JOBS="$(nproc 2>/dev/null || echo 4)"
NASM_VER=2.16.03
CPU=pentium3            # -mtune target; override for the actual retro CPU
GLIDE_FORK=https://github.com/voidsstr/retro3dfx-glide.git
GL_FORK=https://github.com/voidsstr/retro3dfx-gl.git

DEBUGBUILD=""; ARGS=()
for a in "$@"; do case "$a" in --debug) DEBUGBUILD="DEBUG=1";; *) ARGS+=("$a");; esac; done
WORK="${ARGS[0]:-$HERE/build}"; OUT="$HERE/out"
command -v ${CROSS}gcc >/dev/null || { echo "FATAL: ${CROSS}gcc missing"; exit 1; }
mkdir -p "$WORK" "$OUT" "$OUT/sdk/include" "$OUT/sdk/lib"
cd "$WORK"

# --- nasm (glide2x cpuid.asm) ------------------------------------------------
if ! command -v nasm >/dev/null && [ ! -x "$WORK/nasm-install/bin/nasm" ]; then
    echo "== building local nasm =="
    curl -sL --max-time 120 -o nasm.tar.xz \
        "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VER/nasm-$NASM_VER.tar.xz"
    tar xf nasm.tar.xz
    (cd "nasm-$NASM_VER" && ./configure --prefix="$WORK/nasm-install" >/dev/null \
        && make -j"$JOBS" >/dev/null && make install >/dev/null)
fi
[ -x "$WORK/nasm-install/bin/nasm" ] && export PATH="$WORK/nasm-install/bin:$PATH"

# --- clone/update our forks --------------------------------------------------
[ -d retro3dfx-glide ] || git clone -q "$GLIDE_FORK"
[ -d retro3dfx-gl ]    || git clone -q "$GL_FORK"

# ============================================================================
#  1) retro3dfx-glide  ->  glide3x.dll / glide2x.dll  (our Glide fork)
# ============================================================================
GTREE="$WORK/retro3dfx-glide"
ln -sfn ../swlibs "$GTREE/glide3x/swlibs"
ln -sfn ../swlibs "$GTREE/glide2x/swlibs"

# P6FENCE host-tool portability patch (64-bit build host) - idempotent
python3 - "$GTREE" <<'EOF'
import sys,glob,pathlib
for f in glob.glob(sys.argv[1]+"/glide2x/*/glide/src/fxglide.h"):
    p=pathlib.Path(f); s=p.read_text(encoding="latin-1")
    if "host-side offset-generator" in s: continue
    old='#elif defined(__GNUC__) && defined(__i386__)\n#define P6FENCE asm("xchg %%eax, %0" : : "m" (_GlideRoot.p6Fencer) : "eax");\n#else'
    if old in s:
        p.write_text(s.replace(old, old.replace('#else','#elif defined(__GNUC__)\n/* host-side offset-generator build */\n#define P6FENCE\n#else')), encoding="latin-1")
EOF

HOSTFIX='HOST_CFLAGS=$(filter-out -m32 -mcpu=% -mtune=% -DFX_DLL_ENABLE -DHWC_EXT_INIT=% -march=%,$(CFLAGS))'
# ABI fix: export both grFoo and grFoo@N; import lib w/o -U
LDFIX='LDFLAGS=-shared -m32 -Wl,--enable-auto-image-base -Wl,--no-undefined -Wl,--add-stdcall-alias'
DTFIX='DLLTOOL_FLAGS=--as-flags=--32 -m i386'

emit(){ cp "$1/glide3x.dll" "$OUT/$2"; cp "$1/libglide3x.dll.a" "$OUT/sdk/lib/$3"; }

# dual_abi_relink <glide-build-dir> — relink a freshly-built glide DLL so it
# exports BOTH `grFoo@N` (mingw) AND `_grFoo@N` (MSVC-style, underscore). Our
# MesaFX ICD (and the retail-linked build) IMPORT the underscore-decorated glide
# names (same as retail 3dfx glide exports); a pure-mingw glide exports only the
# non-underscore names, so the ICD fails LoadLibrary. Exporting both makes our
# glide a drop-in for either ICD. (Root cause proven 2026-07-21 by objdump: ICD
# imports _grBufferSwap@4, mingw glide exported grBufferSwap@4.)
dual_abi_relink(){
  local D="$1" def us objs
  def="$D/lib/glide3x.def"; us="$D/lib/glide3x_us.def"
  [ -f "$def" ] || { echo "  dual_abi: no def in $D, skipping"; return 0; }
  # augment the auto-generated def with `_NAME = NAME` underscore aliases
  python3 - "$def" "$us" <<'PY'
import re,sys
src=open(sys.argv[1]).read().splitlines(); out=list(src); al=[]
for line in src:
    m=re.match(r'\s*(\w+@\d+)\s+@\d+\s*$', line)
    if m: al.append("    _%s = %s"%(m.group(1), m.group(1)))
open(sys.argv[2],"w").write("\n".join(out+al)+"\n")
PY
  objs=$(find "$D" -name "*.o" | tr '\n' ' ')
  ${CROSS}gcc -shared -m32 -Wl,--enable-auto-image-base -Wl,--no-undefined -Wl,--add-stdcall-alias \
    -o "$D/lib/glide3x.dll" "$us" $objs \
    -luser32 -lkernel32 -lddraw -lgdi32 -ldxguid -ladvapi32 2>/dev/null \
    && echo "  dual_abi: $D/lib/glide3x.dll now exports grFoo@N + _grFoo@N" \
    || echo "  dual_abi: relink FAILED for $D (keeping single-ABI dll)"
}

echo "== retro3dfx-glide: glide3x h5 (Voodoo4/5) =="
make -C "$GTREE/glide3x" -f Makefile.mingw CROSS="$CROSS" FX_GLIDE_HW=h5 $DEBUGBUILD "$HOSTFIX" "$LDFIX" "$DTFIX" >/dev/null
dual_abi_relink "$GTREE/glide3x/h5"
emit "$GTREE/glide3x/h5/lib" glide3x.dll libglide3x.dll.a
cp "$GTREE"/glide3x/h5/glide3/src/{glide,g3ext,glidesys,glideutl}.h "$OUT/sdk/include/" 2>/dev/null || true
cp "$GTREE"/glide3x/h5/incsrc/sst1vid.h "$OUT/sdk/include/" 2>/dev/null || true
cp "$GTREE"/swlibs/fxmisc/3dfx.h "$OUT/sdk/include/" 2>/dev/null || true

echo "== retro3dfx-glide: glide3x h3 (Voodoo3) =="
make -C "$GTREE/glide3x" -f Makefile.mingw CROSS="$CROSS" FX_GLIDE_HW=h3 $DEBUGBUILD "$HOSTFIX" "$LDFIX" "$DTFIX" >/dev/null
dual_abi_relink "$GTREE/glide3x/h3"
cp "$GTREE/glide3x/h3/lib/glide3x.dll" "$OUT/glide3x_h3.dll"

echo "== retro3dfx-glide: glide2x (Napalm) =="
make -C "$GTREE/glide2x" -f Makefile.mingw CROSS="$CROSS" FX_GLIDE_HW=h3 H4=1 "$HOSTFIX" "$LDFIX" "$DTFIX" >/dev/null
cp "$GTREE/glide2x/h3/lib/glide2x.dll" "$OUT/glide2x.dll"

# ============================================================================
#  2) retro3dfx-gl (MesaFX)  ->  opengl32.dll  (OpenGL ICD over our Glide)
# ============================================================================
GLTREE="$WORK/retro3dfx-gl"
# lay out the Glide3 SDK where MesaFX's Makefile.mgw expects it: $TOP/glide3
mkdir -p "$GLTREE/glide3/include" "$GLTREE/glide3/lib"
cp "$OUT/sdk/include/"*.h "$GLTREE/glide3/include/"
# MesaFX links -lglide3x -> provide libglide3x.a (h5 import lib)
cp "$OUT/sdk/lib/libglide3x.dll.a" "$GLTREE/glide3/lib/libglide3x.a"

echo "== retro3dfx-gl: MesaFX ICD (FX=1) over our Glide =="
# Modernize for gcc 13 (era code assumes ~gcc 3): drop -Werror, allow common
# symbols. Our fork's build fix; idempotent.
sed -i 's/CFLAGS = -Wall -Werror/CFLAGS = -Wall -Wno-array-bounds -Wno-stringop-overflow -fcommon/' "$GLTREE/Makefile.mgw"
# Makefile.mgw hardcodes native tool names (CC=gcc, RC=windres, ...); point every
# one at the cross toolchain so it builds Win32 on this Linux host.
make -C "$GLTREE" -f Makefile.mgw FX=1 X86=1 CPU="$CPU" GLIDE="$GLTREE/glide3" \
     CC="${CROSS}gcc" AR="${CROSS}ar rcu" RANLIB="${CROSS}ranlib" \
     DLLTOOL="${CROSS}dlltool" AS="${CROSS}gcc -c -x assembler-with-cpp" \
     RC="${CROSS}windres" >/tmp/mesa_build.log 2>&1 \
  && { cp "$GLTREE"/lib/*mesa32.dll "$OUT/opengl32.dll" 2>/dev/null \
         || find "$GLTREE" -iname "opengl32.dll" -exec cp {} "$OUT/opengl32.dll" \; ; \
       echo "   opengl32.dll (MesaFX over our Glide) built"; } \
  || { echo "   MesaFX build needs iteration - see /tmp/mesa_build.log (tail):"; tail -12 /tmp/mesa_build.log; }

echo; echo "== voodoo-cleanroom user-mode stack artifacts =="
ls -la "$OUT"/*.dll 2>/dev/null
