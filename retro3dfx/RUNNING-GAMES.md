# Running games on our retro3dfx stack

## Quake III Arena — WORKING on Voodoo3 (.124), via our MesaFX ✅

**Status (2026-07-16):** Quake III 1.32 runs hardware-accelerated on the real
Voodoo3 through **our own `retro3dfx-gl` MesaFX** OpenGL ICD:

```
GL_VENDOR:   Brian Paul
GL_RENDERER: Mesa Glide v0.62 Voodoo3 (tm)
GL_VERSION:  1.2 Mesa 6.2.2
...assuming 'retrogl' is a standalone driver
...hardware acceleration found  ->  PIXELFORMAT selected  ->  GL context: succeeded
```

### The blocker that was fixed: glide3x import-name ABI mismatch

Our MesaFX (from `build-stack.sh`) links our own glide3x import lib, importing
`grFoo@N` (no leading underscore). But .124 has the **AmigaMerlin** 3dfx driver
installed, whose `glide3x.dll` exports **`_grFoo@N`** (retail MSVC decoration,
WITH leading underscore). Mismatch → `LoadLibrary(retrogl.dll)` fails → Quake3
silently falls back to Microsoft's software "Direct3D GL 1.1" wrapper.

**Fix:** relink MesaFX against the retail (underscore) import lib
`scripts/3dfx/glide-sdk/lib/libglide3x_retail.dll.a` →
`retro3dfx/build-mesafx-retail.sh` → `retro3dfx/out/opengl32_retail.dll`. Deploy
that as the game's `r_glDriver` DLL. (Same underscore issue as the
`gfxbench_retail` benchmark variant.)

> Why not our own glide3x? Our `retro3dfx-glide` glide3x needs our own
> `retro3dfx-disp` display driver to answer its HWCEXT hardware-init escape.
> With only the AmigaMerlin driver installed, our glide3x loads but **hangs at
> hardware init** (verified: gfxbench with our glide3x logs `gfxbench start`
> then stalls). Until `retro3dfx-disp` is built + installed, bind our upper
> stack (MesaFX) to the proven AmigaMerlin glide3x via the retail import lib.

### Deploy recipe (.124)

1. Build: `retro3dfx/build-mesafx-retail.sh` → `out/opengl32_retail.dll`.
2. Copy it to the game as `retrogl.dll` **and** to `%SystemRoot%\system32\retrogl.dll`
   (Quake3 resolves `r_glDriver` via the system path). On .124 both are the
   retail build.
3. Ensure the retail/AmigaMerlin `glide3x.dll` is reachable (it is, in
   `system32`; a copy also sits in the Quake3 dir for certainty).
4. Launch: `C:\RETRO_AGENT\play_q3.bat` (installed) — cd's to the install and
   starts `quake3.exe +set r_glDriver retrogl +set r_mode 3 +set r_fullscreen 1
   +set r_colorbits 16 +set fs_homepath C:\q3home +set sv_pure 0`.

### Agent gotchas learned while doing this

- **Use `EXEC` + `start`, not `LAUNCH`, to run a GUI game.** On .124's agent
  `LAUNCH` returns a PID but the child command does not actually execute (empty
  output every time). `EXEC cmd /c cd /d "<dir>" ^&^& start "" quake3.exe +set ...`
  reliably detaches the game onto the console desktop. (`EXECW <secs> ...` gives
  a longer bound than EXEC's 60s for slow steps.)
- **GDI `SCREENSHOT` of a Glide fullscreen buffer is dark/interlaced** (the
  Voodoo3 fullscreen path bypasses GDI). Trust `qconsole.log`
  (`+set logfile 2`, at `fs_homepath\baseq3\qconsole.log`) for GL_RENDERER /
  hardware-accel confirmation, not the screenshot.

## Unreal Tournament — TODO

Not yet installed on .124. UT has a native Glide renderer (no MesaFX needed) —
it talks the retail glide3x directly, so no underscore relink is required for UT;
point its `[OpenGLDrv]`/Glide renderer at the installed driver.
