#!/usr/bin/env bash
# build-glide.sh - cross-compile the open-source 3dfx Glide DLLs for the fleet.
#
# Produces Windows i386 DLLs from the open (2000, "Napalm release") Glide source
# using the SAME mingw toolchain that builds retro_agent.exe:
#
#   out/glide3x.dll  - Glide3, h5 tree  = VSA-100 (Voodoo4/5), incl. Voodoo5 6000
#                      4-way SLI + "V5 6000 DAC workaround for 4x/8x FSAA" paths
#   out/glide2x.dll  - Glide2, h3 tree with H4=1 ("high speed Avenger/Napalm"),
#                      what Win98 Glide2 games use on Voodoo4/5
#
# Upstream: https://github.com/sezero/glide (fork of the SourceForge CVS).
# See docs/3dfx-drivers.md for the full driver-landscape research.
#
# Host quirks handled here (all discovered/validated 2026-07-15 on this repo's
# build host - see the doc):
#   1. fxgasm/fxbldno are HOST tools built with -m32; without 32-bit glibc
#      headers that fails -> strip -m32 from HOST_CFLAGS. Safe because the
#      makefile builds with GLIDE_USE_C_TRISETUP (asm offset consumers disabled).
#   2. swlibs/ is a sibling of glide2x/glide3x but makefiles expect it inside
#      -> symlink.
#   3. glide2x's fxglide.h #errors on a 64-bit host compiler (P6FENCE inline
#      asm is only defined for __i386__) -> add a no-op fallback for the host
#      offset-generator build. (Target DLL code is always __i386__; unaffected.)
#   4. glide2x needs nasm for cpuid.asm -> auto-build nasm locally if missing.
#
# Usage:  ./build-glide.sh [workdir]     (default workdir: ./build)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HERE/build}"
OUT="$HERE/out"
CROSS=i686-w64-mingw32-
JOBS="$(nproc 2>/dev/null || echo 4)"
NASM_VER=2.16.03

command -v ${CROSS}gcc >/dev/null || { echo "FATAL: ${CROSS}gcc not found (apt install gcc-mingw-w64-i686)"; exit 1; }

mkdir -p "$WORK" "$OUT"
cd "$WORK"

# --- nasm (needed by glide2x cpuid.asm) --------------------------------------
if ! command -v nasm >/dev/null; then
    if [ ! -x "$WORK/nasm-install/bin/nasm" ]; then
        echo "== building local nasm $NASM_VER =="
        curl -sL --max-time 120 -o nasm.tar.xz \
            "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VER/nasm-$NASM_VER.tar.xz"
        tar xf nasm.tar.xz
        (cd "nasm-$NASM_VER" && ./configure --prefix="$WORK/nasm-install" >/dev/null \
            && make -j"$JOBS" >/dev/null && make install >/dev/null)
    fi
    export PATH="$WORK/nasm-install/bin:$PATH"
fi
echo "nasm: $(command -v nasm)"

# --- source ------------------------------------------------------------------
if [ ! -d glide ]; then
    echo "== cloning sezero/glide =="
    git clone -q --depth 1 https://github.com/sezero/glide.git
fi
ln -sfn ../swlibs glide/glide3x/swlibs
ln -sfn ../swlibs glide/glide2x/swlibs

# --- patch: P6FENCE fallback for 64-bit host offset-generator builds ---------
python3 - <<'EOF'
from pathlib import Path
for f in Path("glide").glob("glide2x/*/glide/src/fxglide.h"):
    s = f.read_text(encoding="latin-1")
    if "host-side offset-generator" in s:
        continue  # already patched
    old = ('#elif defined(__GNUC__) && defined(__i386__)\n'
           '#define P6FENCE asm("xchg %%eax, %0" : : "m" (_GlideRoot.p6Fencer) : "eax");\n'
           '#else')
    if old in s:
        new = old.replace('#else',
            '#elif defined(__GNUC__)\n'
            '/* host-side offset-generator build (fxgasm) on a 64-bit host: fence unused */\n'
            '#define P6FENCE\n'
            '#else')
        f.write_text(s.replace(old, new), encoding="latin-1")
        print(f"patched {f}")
EOF

# HOST_CFLAGS: same filter the makefile uses, minus -m32 (host tools only)
HOSTFIX='HOST_CFLAGS=$(filter-out -m32 -mcpu=% -mtune=% -DFX_DLL_ENABLE -DHWC_EXT_INIT=% -march=%,$(CFLAGS))'

# --- glide3x (h5 = VSA-100: Voodoo4/5, incl. 6000 4-way SLI) ------------------
echo "== building glide3x (h5 / VSA-100) =="
make -C glide/glide3x -f Makefile.mingw CROSS="$CROSS" FX_GLIDE_HW=h5 "$HOSTFIX" >/dev/null
cp glide/glide3x/h5/lib/glide3x.dll "$OUT/glide3x.dll"

# --- glide2x (h3 tree + H4=1 = Napalm-capable Glide2 for Win98 games) ---------
echo "== building glide2x (h3 + H4=1 / Napalm) =="
make -C glide/glide2x -f Makefile.mingw CROSS="$CROSS" FX_GLIDE_HW=h3 H4=1 "$HOSTFIX" >/dev/null
cp glide/glide2x/h3/lib/glide2x.dll "$OUT/glide2x.dll"

echo
echo "== artifacts =="
file "$OUT"/glide2x.dll "$OUT"/glide3x.dll
ls -la "$OUT"/*.dll
echo
echo "Deploy per-game (next to the game exe) or system-wide (C:\\WINDOWS\\SYSTEM[32])"
echo "on a machine that already has a working 3dfx display driver installed."
