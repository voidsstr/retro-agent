# retro3dfx Driver Debugging Notes

A growing knowledge base of hard-won debugging findings for the retro3dfx MesaFX
ICD (`retrogl.dll` / `opengl32_retail.dll`) on Voodoo3 hardware. Each entry:
symptom → how we isolated it → root cause → fix → how to reproduce the diagnosis.

Newest first.

---

## 2026-07-18 — Quake II (`ref_gl`) `wglCreateContext` crash/hang on our ICD

**Symptom.** Launching Quake II with `gl_driver retrogl` (our ICD) produced a
green screen + a Windows "driver has stopped working" dialog, and the process
never rendered. Q3 on the same ICD worked perfectly. The stock `gl_driver 3dfxgl`
path "worked" but was unstable (crashed intermittently, took the agent offline)
and low-quality textures.

**Isolation (instrumented ICD).** Added `[q2diag]` `fprintf(stderr,...)` tracing
around context creation in `fxwgl.c::wglCreateContext` and the `grSstWinOpen` call
in `fxapi.c::fxMesaCreateContext`, gated on `MESA_FX_INFO=r` (which `freopen`s
stderr to `MESA.LOG` in the game's working dir). Launched with
`set MESA_FX_INFO=r` and read `C:\Games\Quake2\MESA.LOG`.

Trace showed the flow reaching **`pre grSstWinOpen`** but **never `post`** — i.e.
the fault/hang is *inside* `grSstWinOpen` (in `glide3x.dll`, a retail binary we
can't instrument). Logged params were **byte-identical** to a working Q3 launch:
```
Q2 (fails): pre grSstWinOpen win=1b00a2 res=7 ref=0 HavePixExt=0 pixFmt=3 aux=1 vis=1 fg=1   <-- no post
Q3 (works): pre grSstWinOpen win=d0104  res=7 ref=0 HavePixExt=0 pixFmt=3 aux=1 vis=1 fg=1   <-- post ctx=valid
```
So it was **not** the resolution, refresh, pixel format, aux-buffer count, or
window visibility/foreground state. The only difference was the process/window.

**Dead ends (ruled out, don't re-try):**
- *Desktop color depth* (16 vs 32-bit): Q2 hung at both. Not it.
- *Message pump before context* (drain `PeekMessage`/`DispatchMessage` +
  `SetActiveWindow` so the freshly-shown window is truly active before Glide
  grabs the board): no change. The pump is *kept* in the source — it's harmless
  and correct-in-principle — but it was not the fix.
- *`ChangeDisplaySettings(NULL,0)` to undo `ref_gl`'s pre-context CDS to a
  fullscreen-exclusive mode*: **counterproductive** — it resized the game window
  to the restored desktop size (640x480 → 1024x768, `res=7` → `res=12`), so the
  ICD then tried to bring up Glide at the wrong resolution. Reverted.

**Root cause.** `grSstWinOpen` was faulting because our `retrogl.dll` was binding
**the wrong `glide3x.dll`**. The Q2 directory had *no* `glide3x.dll` and neither
did `system32`; the Windows loader resolved the import from elsewhere on the
search path to an incompatible build, and `grSstWinOpen` in that build crashed.
Q3 worked purely because its own directory ships the correct
`glide3x.dll` (344064 bytes, AmigaMerlin retail) right next to `quake3.exe`, and
the exe dir is searched before the rest of the path.

**Fix.** Place the known-good `glide3x.dll` (the 344064-byte AmigaMerlin retail
build, copied from `C:\Quake III Arena\Quake3\glide3x.dll`) into the game's own
directory, next to `quake2.exe`:
```
copy /Y "C:\Quake III Arena\Quake3\glide3x.dll" "C:\Games\Quake2\glide3x.dll"
```
After that, Q2 creates the context and renders on our ICD:
```
GL_RENDERER: Mesa Glide v0.62 Voodoo3 (tm) [retro3dfx 0.1.16]
689 frames, 7.2 seconds: 95.5 fps    (640x480x16, demo1)
```
**95.5 fps vs 75.7 fps for stock `3dfxgl`** — our ICD is both faster and carries
the LOD-bias texture sharpening (0.1.11). Any game that loads our ICD must have
the matching retail `glide3x.dll` in its own directory — treat this as a
deploy-time invariant (the launcher/bench harness now stages it).

**`Voodoo ! fallback (%x), raster (%x)` log spam is NOT software rasterization.**
The `fallback`/`raster` values come from `fxDDChooseRenderState` (`fxtris.c`) and
report which *primitive render path* was selected (smooth points, stippled lines,
flat-shaded tris route through the general TNL `rast_tab` path instead of the
fast `fx_render_tab` path) — still hardware-rasterized. True pixel-level software
fallback prints `Voodoo ! enter SW 0x...` (we never saw it). Critically, this
per-state `fprintf` to `MESA.LOG` under `MESA_FX_INFO=r` **destroys the framerate
by itself** — always benchmark with verbose OFF. Only enable `MESA_FX_INFO=r` for
diagnosis.

**Repro the diagnosis.** Build the ICD with the `[q2diag]` traces (present on
branch history around 0.1.12–0.1.16), deploy `retrogl.dll` to the game dir +
`system32`, `set MESA_FX_INFO=r`, launch, and read `<gamedir>\MESA.LOG`. Absence
of a `post grSstWinOpen` line = fault inside Glide → check the `glide3x.dll` the
process actually loaded.
