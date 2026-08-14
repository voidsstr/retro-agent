# Bootstrapping the clean-room build toolchain WITHOUT root

The brain host had **no compiler at all** (no `gcc`, `make`, `mingw`, `wine`) and
`sudo` requires a password. The whole clean-room stack was still built by pulling
Ubuntu packages and extracting them into `$HOME` — no root, no system changes.

    PREFIX=$HOME/toolchain-mingw
    cd /tmp/tc
    apt-get download <pkgs>          # works unprivileged; uses existing apt lists
    for d in *.deb; do dpkg -x "$d" "$PREFIX"; done

Package sets (resolve with
`apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces --no-enhances <pkg>`):

* **cross**: `gcc-mingw-w64-i686` + deps (`binutils-mingw-w64-i686`,
  `mingw-w64-i686-dev`, `mingw-w64-common`, `gcc-mingw-w64-base`,
  `gcc-mingw-w64-i686-{win32,posix}{,-runtime}`)
* **host**: `gcc` + `libc6-dev` + deps — the Glide build needs a *host* compiler
  for its `fxgasm` code generator
* **runtime libs** gcc itself needs: `libisl23`, `libmpc3`, `libmpfr6`, `libgmp10`
* `nasm` (glide2x `cpuid.asm`) and `make`

## The four traps, all of which cost real time

1. **`dpkg -x` does not create the `update-alternatives` symlinks.** You get
   `i686-w64-mingw32-gcc-13-posix` but no `i686-w64-mingw32-gcc`. Symlink it
   yourself, else `build-stack.sh` aborts with "FATAL: i686-w64-mingw32-gcc missing".
2. **gcc needs its shared libs**: `export LD_LIBRARY_PATH=$PREFIX/usr/lib/x86_64-linux-gnu`
   or `cc1` dies with `libisl.so.23: cannot open shared object file`.
3. **libc linker scripts carry absolute paths.** `$PREFIX/usr/lib/x86_64-linux-gnu/libc.so`
   says `GROUP ( /usr/lib/x86_64-linux-gnu/libc_nonshared.a ... )`, which does not
   exist. `sed -i "s#/usr/lib/x86_64-linux-gnu/#$L/#g"` the `libc.so`/`libm.so`
   scripts. (Same again under `usr/lib32` if anything builds `-m32`.)
4. **DO NOT export `C_INCLUDE_PATH` / `LIBRARY_PATH` to give the host compiler its
   headers.** Those variables apply to *every* gcc invocation, including
   `i686-w64-mingw32-gcc` — the cross build then pulls **Linux** headers into a
   Windows target and fails deep inside glide with
   `gnu/stubs-32.h: No such file or directory` (mingw targets 32-bit, so
   `features.h` asks for the 32-bit stubs). This looks exactly like a missing
   multilib and sends you installing `libc6-dev-i386` for nothing.
   **Fix: isolate the host paths in a wrapper script** and keep the environment
   clean:

       $PREFIX/hostbin/gcc:
         exec $PREFIX/usr/bin/x86_64-linux-gnu-gcc-13 \
           -isystem $PREFIX/usr/include -isystem $PREFIX/usr/include/x86_64-linux-gnu \
           -B$PREFIX/usr/lib/x86_64-linux-gnu -L$PREFIX/usr/lib/x86_64-linux-gnu \
           -B$PREFIX/usr/lib/gcc/x86_64-linux-gnu/13 -L$PREFIX/usr/lib/gcc/x86_64-linux-gnu/13 "$@"

   then `PATH=$PREFIX/hostbin:$PREFIX/usr/bin:$PATH` and nothing else.

## Build the stack

**Use a Linux-native work dir.** `build-stack.sh` creates symlinks
(`glide3x/swlibs`), which fail on `/mnt/c` (DrvFs): `ln: failed to create
symbolic link ...: Not a directory`. Pass a workdir under `$HOME`:

    bash build-stack.sh $HOME/vcr-build      # out/ still lands in the repo

Artifacts: `out/glide3x_h3.dll` (=`glide3x.dll`, Voodoo3), **`out/glide3x_h5.dll`
(Voodoo4/5)**, `out/glide2x.dll`, `out/opengl32.dll` (MesaFX over our Glide),
`out/sdk/`. Then `build-mesafx-retail.sh` for the retail-glide-linked ICD.
