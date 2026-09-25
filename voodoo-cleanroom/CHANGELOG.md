# voodoo-cleanroom ICD changelog (MesaFX fork)

> The dated overview of every change — ICD, Glide, display driver, tooling —
> with its current status is in [`README.md` §15](README.md#15-change-history--optimizations-fixes-and-changes-with-dates).
> This file is the detailed per-version log of the OpenGL ICD.

Versioning: `VERSION` (MAJOR.MINOR) + auto-incrementing `.buildnum` → `0.1.N`,
injected into `GL_RENDERER` so logs and benchmarks self-document. The stamp is
`[voodoo-cleanroom 0.1.N]` from 0.1.32 on (2026-07-21); earlier builds stamped
`[retro3dfx 0.1.N]` — not to be confused with the vintage lane's
`[retro3dfx 0.2.x+]` SGL ICD. One functional change per version. Every benchmark row in the
specpicks DB (`retro_benchmark_runs`) carries a `driver_stack` JSON naming the
exact composition of all three layers, and `driver_version` = the ICD version.

## 0.1.71 – 0.1.73 — Quake II's single-pass multitexture wall, found and taken down (2026-09-25)

Quake II with `GL_SGIS_multitexture` (opt-in, `FX_SGIS_MULTITEXTURE=1`) was the
open mystery of 0.1.57/0.1.58: a "fixed CPU wall" that made single-pass slower
than two-pass although every counter the old instrumentation had was the same.
It reproduced on the V5 6000, harder - **50.8 fps single-pass vs 213.2
two-pass** (640×480, 4 chips, all-ours) - and the 0.1.67 sampler named it in
one run. Three fixes, each pixel-identical on the box
(`icd_frame_compare.py`, 0 of 307,200 pixels differ against the fix switched
off), each with an env switch to undo it:

| build | fix | Q2 single-pass 1024×768 | 640×480 |
|---|---|---|---|
| 0.1.70 | — | 49.3 | 50.8 |
| **0.1.71** | `glTexSubImage2D` re-sent the **whole mip level**: `grTexDownload_Default_32_WideS` was **55 %** of all CPU. Quake II's single-pass path updates a dynamic lightmap with one small sub-image per lit surface per frame, and each re-sent a 128×128 32-bit page (64 KB, ×2 on both TMUs) through the command FIFO. Now only the changed rows (`fxTMReloadSubMipMapLevel`, rewritten: it had never run, used a Glide2-era LOD formula, and computed the first row in 16-bit units - half the offset for a 32-bit texture). `FX_FULL_TEXSUB=1` undoes it. | 108.2 | 116.5 |
| **0.1.72** | `fx_glSelectTextureSGIS` read `FX_SGIS_NO_CLIENTTEX` with `getenv` on **every call** - twice per surface. XP msvcrt's getenv is locale-aware (`MultiByteToWideChar`, `CompareStringA`, `GetVersionExW` per call): **~27 %** of the frame. Read once. | 152.4 | 172.2 |
| **0.1.73** | `glActiveTexture` / `glClientActiveTexture` flushed the buffered vertices on every unit switch, so each surface was its own trip through the TNL pipeline with the vertex format rebuilt (`_tnl_wrap_upgrade_vertex` 6.3 % vs 0.8 % two-pass). Selecting a unit changes no rendering state, so mark it dirty instead (`src/mesa/main/texstate.c`). `MESA_NO_LAZY_UNIT_SELECT=1` restores the flush. | **162.6** | **184.2** |

Two-pass, same builds: 132.1 at 1024×768; 640×480 rose from ~213-217 to
**228.5** (its dynamic lightmaps also go through `glTexSubImage2D`, now partial).

So at 1024×768 single-pass now **beats** two-pass (+23 %) and the stack's
Quake II there is **+26 %** over 0.1.68 (129.3). At 640×480 - CPU-bound on four
chips - two-pass still leads (228.5 vs 184.2): single-pass still pays for
per-surface lightmap uploads (full 128-texel rows for a small patch, 9 % of
the frame) and more pipeline runs. SGIS therefore stays opt-in until that gap
closes or a single-chip measurement says otherwise. The profiler also learned
to name functions in stripped system DLLs from their export tables
(`v56k_diag.pe_export_table`), which is what turned "17 % in msvcrt" into
"getenv".

## 0.1.70 — 0.1.69 reverted: identical to 0.1.68 (2026-09-25)

`fxtris.c` is byte-for-byte the 0.1.68 source again (checked against the 0.1.68
patch). See 0.1.69 for why.

## 0.1.69 — clipped-path triangle batching: pixel-identical, no measurable gain, REVERTED (2026-09-25)

A vertex buffer with ANY clipped vertex is rendered by Mesa's clip-aware
tables, which call `Render.Triangle` once per triangle - one `grDrawTriangle`
DLL call each (stub, indirect jump, triangle-setup prologue). The 0.1.67
profiler put triangle submission at ~10 % of a CPU-bound Quake III frame. The
plain render variant (`render_index` 0 only) now queues the vertex pointers
and sends a run as one `grDrawVertexArray(GR_TRIANGLES)`; quads queue as the
same two triangles as the fan they replaced.

Order is preserved: the clipped-polygon, points, lines, primitive-change,
multipass, pass-end and state-change paths all flush first, and the offset /
two-sided / unfilled / flat variants - which rewrite vertices in place around
each draw - never batch. `FX_NO_TRI_BATCH=1` turns it off.
`scripts/benchmarks/icd_frame_compare.py` checks pixel identity on the box.

**Result.** Pixel-identical: Quake II's last timedemo frame at 640×480 on four
chips, batching on vs off, 0 of 307,200 pixels differ (and the frame is a real
rendered frame, read back through the LFB under SLI). **Speed: nothing.**
Interleaved A/B ×2, cfg 5: Quake II 640×480 +0.4 %, 800×600 −0.5 %; Quake III
640×480 −0.2 %, 800×600 +0.1 %. The profiler says why: `grDrawTriangle` barely
moved (102 vs 96 samples) - Quake III's geometry already reaches Glide through
0.1.3's batched unclipped path, and the per-triangle cost that remains is
Glide's own triangle setup (`_internal_trisetup`, 6-7 %), paid per triangle on
every path. A change with no measurable benefit is not worth its ordering risk,
so 0.1.70 removes it; the pixel-compare harness stays.

## 0.1.68 — teardown breadcrumbs (2026-09-25)

Diagnostic only: `wglDeleteContext`, `fxMesaDestroyContext`, `fxCloseHardware`,
`cleangraphics` and `DllMain(DLL_PROCESS_DETACH)` each append a line to
`C:\retrogl.log`, and the detach line says whether the process is exiting or the
DLL is being `FreeLibrary`'d (an engine renderer restart). Their **absence** after a
run means the process was terminated, not exited - which is how it was found that
every Quake II benchmark cell had ended in a force-kill (the runner's
`nextserver` was wiped by `demomap`; fixed in `v56k_bench.py`). On our h5 Glide a
force-killed process never unmaps the board, and the display driver later hands
its dead mapping to whichever process reuses the PID.

## 0.1.67 — render-thread sampling profiler (`RETROGL_PROF`); Glide errors logged, never a hidden dialog (2026-09-25)

**Profiler.** `RETROGL_PROF=<file>` in the game's environment: at the first
`wglMakeCurrent` the calling thread becomes the target, and a TIME_CRITICAL
thread suspends it about once a millisecond, records `EIP` and resumes it. A
sample counts only while `wglSwapBuffers` ran in the last 100 ms (no loading
screens), and the histogram is written at exit as `eip count base module`
(`src/mesa/drivers/glide/fxprof.c`). While the target is suspended the sampler
calls only kernel entry points and a static table, so it cannot wait on a lock
the target holds; `winmm` is `LoadLibrary`'d, so the ICD gained no import.
Unset, it costs one store per swap. `scripts/benchmarks/icdprof.py` names the
functions from the unstripped DLLs. About 1 % of frame rate when on.

First profile - Quake III, 320×240, one chip (CPU-bound, 131 fps), all-ours
stack: `quake3.exe` 46.8 %, **our ICD 20.5 %**, **our Glide 17.3 %**, QVM code
6.9 %, ntdll 6.5 % (almost all `KiFastSystemCallRet`). In Glide, triangle
submission (`grDrawTriangle` + 3DNow! setup + `grDrawVertexArray`) is ~10.7 %;
in the ICD, vertex emit ~3.5 %, clipping ~3.2 %, ubyte→float colour
conversion 1.3 %, state changes ~2 %. No single hot spot.

**Glide error callback.** Glide's default callback reports a fatal error with
`MessageBox(NULL, …)` + `exit(1)`; behind a fullscreen window nobody can see the
box and the game waits forever. That was the intermittent Quake III "hang in
`grGlideInit`" on the V5 6000 (ntsd: `USER32!MessageBoxA` ←
`glide3x!_grErrorDefaultCallback`, text in minihwc's `errorString`, a refused
stale board mapping). `fxQueryHardware` now installs `fxGlideErrorCallback`
before `grGlideInit`: it logs `GLIDE FATAL ERROR: <text>` to `retrogl.log` and
returns, so Glide skips the board and the context fails cleanly. Our h5 Glide
keeps a pre-installed callback (fork `5439bb8`); AmigaMerlin's resets it, so
that lane behaves as before. The callback is cdecl (`GrErrorCallbackFnc_t` has
no `FX_CALL`).

## 0.1.66 — Mesa's span scratch arrays on the heap: SiN Gold runs (I12) (2026-09-24)

SiN Gold died of a **stack overflow** (`c00000fd`) inside our ICD: Dr Watson put
the fault in `___chkstk_ms`, and the return addresses on the raw stack map to
`fxDDTexImage2D` → `_mesa_texstore_argb8888` → `_mesa_make_temp_chan_image` →
`_mesa_unpack_color_span_chan`. That last function had an **80 KB stack frame**
(81,980 B): Mesa 6.2's per-span scratch arrays are `MAX_WIDTH` (4096) × 4 floats
plus indexes, declared on the stack. A Quake II-engine game already holds ~768 KB
of its own texture scratch on its 1 MB main-thread stack when it calls
`glTexImage2D`, so the upload ran it off the end — very likely the long-standing
I12 ("SiN stalls at GL init on our ICD").

Mesa already has a heap variant of `DEFARRAY`/`DEFMARRAY` (written for the classic
Mac's 32 KB stack); the Win32 FX build now selects it. Every declaring site was
checked for a matching `UNDEFARRAY` on each exit; the two `s_texture.c` crossbar
early returns did not have one and now do. Frames after: `_mesa_unpack_color_span_chan`
16,444 B, `_mesa_pack_rgba_span_chan` 76 B, `read_rgba_pixels` 32,888 B.

Verified on the V5 6000: SiN Gold starts and keeps running. Interleaved A/B
against 0.1.65 shows no cost (Quake II 1024×768 127.2–128.0 vs 127.9–128.3,
640×480 174–181 vs 173–175; Quake III level). Test:
`tests/python/test_cleanroom_heap_span_arrays.py`. 2,764,449 B.

The ICD patch is now regenerated by `tools/regen-icd-patch.sh`, which takes the
file list from `git status` of the fork clone. The list used to be typed by hand
and twice nearly dropped a new file.

## 0.1.65 — the activation pump is bounded: ioquake3 runs on the system ICD (2026-09-24)

`wglCreateContext` pumps the thread's messages before Glide takes the board (the
idTech2 `ref_gl` activation deadlock fix). Its inner loop was
`while (PeekMessage(...)) DispatchMessage(...)` with **no bound** — and `WM_PAINT`
is not a queued message: Windows synthesises it for as long as the window has an
update region. Loaded as the **system ICD**, Microsoft's `opengl32` subclasses
the window as well, and under SDL (ioquake3) the region never cleared: the game
sat inside `wglCreateContext` forever. Caught with `ntsd` on `.124`, twice:
`DispatchMessageA(WM_PAINT)` → `__wglMonitor` → `opengl32` hook → SDL's WndProc →
`EndPaint`.

Now `WM_PAINT` is validated instead of dispatched (the game repaints every frame
anyway), the pump dispatches at most 256 messages, a `WM_QUIT` is handed back with
`PostQuitMessage`, and `retrogl.log` records what the pump did. Verified on the
V5 6000: ioquake3 creates its 1024×768 context and runs; Quake II's staged
launcher (the case the pump exists for) still does.
Test: `tests/python/test_cleanroom_activation_pump.py`. 2,764,902 B.

## 0.1.64 — fullscreen refresh is the monitor's best again (I1; the lost 0.1.34, re-implemented) (2026-09-24)

`fxMesaCreateBestContext` passed `GR_REFRESH_60Hz` to every fullscreen game —
Glide programs the video timing itself, so nothing in Windows could raise it,
and a CRT flickered at 60 Hz. The 0.1.34 fix for exactly this was **lost from
every source** (README §15.4) while its native test kept passing, because the
test mirrors the logic instead of reading it. `fxBestRefresh()` is back, to the
letter of that test: env override (`FX_GLIDE_REFRESH_RATE` / `SSTV2_REFRESH_RATE` /
`MESA_FX_REFRESH`, Hz) else the highest rate the display driver enumerates for
W×H (0/1 Hz "default" sentinels ignored), snapped down to a `GR_REFRESH_*` timing;
below 60 or no answer → 60. New `tests/python/test_cleanroom_refresh_source.py`
reads the **patch**, so this cannot silently vanish a second time.

Verified on the V5 6000 (`.124`, cfg 2): 1024×768 → 100 Hz (`GR_REFRESH_100Hz`),
640×480 → 120 Hz; the board opens at both. Performance unchanged in an
interleaved A/B against 0.1.63 (Quake II 1024×768 129.0 vs 129.2–130.2;
640×480 210.0–211.9 vs 209.5). 2,764,496 B. Installed on `.124` as the system ICD
(`system32\retroicd.dll`; 0.1.63 kept as `retroicd_0.1.63.dll`).

## 0.1.63 — the DLL is also a Microsoft ICD (`Drv*` front end) (2026-09-24)

Until now the ICD could only be reached by games that load an OpenGL library
**by name** (Quake II `gl_driver`, Quake III / RtCW `r_glDriver`). Everything
that links the system `opengl32.dll` — Counter-Strike 1.6 / GoldSrc, UT99's
OpenGLDrv — never saw it, because `opengl32` is a KnownDLL on XP (a game-local
copy is ignored) and Microsoft's `opengl32` talks to an ICD only through `Drv*`
entry points and the 336-entry dispatch table `DrvSetContext` returns.

New `src/mesa/drivers/glide/fxicd.c` maps the 17 `Drv*` entry points one-for-one
onto the existing single-context `wgl*` layer (`fxwgl.c`) and hands back the
dispatch table in Microsoft's order, taken from Mesa's own
`drivers/windows/icd/icdlist.h`. The same DLL still works game-local by name.

Install as the system ICD (reversible — nothing is overwritten): copy the DLL to
`system32\retroicd.dll` and set `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\
OpenGLDrivers\<name>\DLL` = `retroicd.dll` (on `.124` `<name>` is `3dfx`, original
value `3dfxOGL.dll`). Use `reg.exe`: the agent's `REGWRITE` splits on the space
in "Windows NT".

Verified on the V5 6000 (`.124`, cfg 0): **Counter-Strike 1.6 runs** (119.9 fps
640×480, 63.4 at 1024×768, best of 3) — note Microsoft's pixel-format chooser
picks our 32-bit ARGB8888 format, so compare with AmigaMerlin's 32-bit rows
(119.5 / 63.1). **UT99 OpenGLDrv runs** (58.3 fps 800×600) — on AmigaMerlin's own
ICD it GPFs at init. Test: `tests/python/test_cleanroom_icd_frontend.py`.
2,763,236 B.

## 0.1.62 — Glide is shut down at process exit; first Voodoo 5 numbers (2026-09-24)

`fxCloseHardware` has kept Glide initialised across a context destroy since
0.1.31, so a `vid_restart` does not do `grGlideShutdown → grGlideInit` mid-flight
(which wedged the Voodoo 3). The same rule applied at **process exit**, so
`grGlideShutdown` was never called: the teardown was left to the OS. The exit
handler (`cleangraphics`) now sets `glbProcessExiting` and shuts Glide down
itself, including when the game already deleted its context before quitting.
`vid_restart` behaviour is unchanged. Test: `tests/native/test_icd_exit_shutdown.c`.

Found while chasing an intermittent dead board mapping in AmigaMerlin's
`grGlideInit` on the V5 6000 (`gc->ioRegs` unmapped → c0000005). **An
interleaved A/B on a fresh cfg 0 boot did not reproduce that fault with either
build** (0/10 each, identical fps: Quake II 90.0–90.6, Quake III 117.2–119.8),
so this is hygiene, not a proven cure — and the fault then struck 0.1.62 once
in the next 11 launches (retried; the next launch was clean). It lives below
the ICD. 2,757,177 B.

**The ICD runs on the Voodoo 5 6000** (`.124`, AmigaMerlin 3.1-R11 Glide + display
driver underneath, game-local `retrogl.dll`) — README I11 does not reproduce
with 0.1.61/0.1.62. Quake II beats AmigaMerlin's own Mesa 6.3 ICD in every cell
(4-chip 640×480: 209.8–221.5 vs 172.6–175.3); Quake III is level on one chip
and ahead on four at 1280×960 and below. Numbers in README §13.3.

## 0.1.61 — renderer re-stamp only (2026-09-04)

No code change: the build differs from 0.1.60 only in the version string. It
was built after the fork clone under `build/` was re-created from GitHub on
2026-09-04, and it is the current `out/opengl32_retail.dll` (2,757,140 B, md5
`bcf0b1bd…`). Like every build since 0.1.41 it **lacks 0.1.34 and 0.1.35**
(see the warning on that section below).

## 0.1.60 — -static-libgcc for the Glide lanes too, and where the hunt stands

`dual_abi_relink` / `dual_abi_relink2` now link with **`-static-libgcc`**. Our
`glide3x_cvg.dll` had picked up an import on `libgcc_s_dw2-1.dll`, which is not
present on the retro boxes — it would have failed `LoadLibrary` on first use
with no diagnostic beyond the game saying it could not load the driver, exactly
as the ICD did before 0.1.52. Caught by `objdump -p | grep "DLL Name"` while
staging it, *before* it ever ran.

`FX_PROFILE` also now times the immediate-mode `glBegin..glEnd` span, which the
pipeline timer never covered.

### The single-pass prize, quantified — and an earlier conclusion retracted

Ceiling tests on real hardware (Q2 demo1, 640×480, vsync off):

| configuration | fps | what it isolates |
|---|---|---|
| baseline, two-pass | 57.2 | — |
| **`r_fullbright 1`** (no lightmap pass) | **92.9** | the lightmap pass costs **6.72 ms = 38% of frame** |
| `gl_dynamic 0` | 57.6 | lightmap uploads are negligible |
| `r_drawentities 0` | 66.3 | entities cost 2.40 ms |
| `r_drawworld 0` | 206.6 | the world costs 12.64 ms |

**Single-pass rendering reaches 92.9 fps, past the stock MiniGL's 90.7.** So the
second pass is the whole prize — worth about +62%.

**Retraction:** 0.1.58 concluded the multitexture cost was in Quake II's code.
That is wrong. The stock MiniGL uses the *same* `GL_SGIS_multitexture` path
(its log says so, and it benchmarks 90.7 fps with multitexture active), so the
~24 ms is ours. Since then, measurement has eliminated every part of our DLL:
TNL pipeline 5.4 ms, immediate-mode accumulation 0.26 vs 0.29 ms, texture setup
~0, texture downloads 0, blocking swap 0, and the immediate-mode vertex size
identical at 4 floats in both modes.

What is left, and where to resume: **`glide3x.dll` itself, which has never been
instrumented.** It is a real suspect because the MiniGL links **glide2x**, not
glide3x — a different library with a different dual-TMU path. Testing that needs
our own cvg glide deployed, which is what the `-static-libgcc` fix above
unblocks.

## 0.1.58 — the multitexture cost is NOT in our driver (2026-08-29)

`FX_PROFILE=1` now instruments the whole frame — texture setup, the TNL
pipeline (cycles + vertex count), texture downloads, `grBufferSwap`, and
vertex-format fixups — and dumps every 100 frames to `C:\retrogl.log`.

Run against the single-pass multitexture regression, **every metric our driver
controls is identical between the fast and slow modes**:

| per frame | SGIS off (57.2 fps) | SGIS on (32.0 fps) |
|---|---|---|
| texture setup calls | 2 | 2 |
| pipeline runs | 75 | 75 |
| vertices | 4728 | 4716 |
| pipeline cycles | ~15.2 M | ~15.6 M |
| texture downloads | 0 | 0 |
| blocking swap cycles | 0 | 0 |
| vertex fixups / chooses | 5 / 7 | 5 / 7 |
| **frame time** | **17.5 ms** | **31.2 ms** |

And the penalty is a fixed CPU wall, not fill: multitexture sits at ~30.5 ms
per frame at **every** resolution (32.9 / 32.7 / 32.1 fps at 320×240 / 512×384 /
640×480, against 121.0 / 80.6 / 54.3 without it).

So ~14 ms per frame is spent somewhere our driver does not measure and does not
control — most plausibly in Quake II's own `ref_gl` multitexture path, which
does more per-frame CPU work than its two-pass path. **This is not our
optimisation to make**, which is the useful conclusion: it retires the largest
apparent opportunity in the stack.

**Seven theories, each implemented or switched and measured on hardware, none
of them the cause:** texture thrashing (`gl_picmip`, 16× less texture RAM),
`glClientActiveTextureARB`'s unconditional flush, per-vertex texcoord
submission, redundant `grTexCombine`, Mesa x86 vertex codegen, texture download
traffic, and vertex-format fixup thrash.

`GL_SGIS_multitexture` therefore stays behind `FX_SGIS_MULTITEXTURE`. Default
build unchanged at **57.2 fps**.

## 0.1.52 — robustness, shadowing, and five refuted optimisation theories (2026-08-29)

**`-static-libgcc` on the ICD link.** Any use of a libgcc helper — a 64-bit
divide is enough — silently adds an import on `libgcc_s_dw2-1.dll`, which is
not present on the retro boxes. The ICD then fails `LoadLibrary` and the game
reports only `could not load "retrogl"`, with nothing pointing at a missing
DLL. Hit while adding profiling counters; worth having permanently.

**`-march` and `-mtune` are separable** (`TUNE ?= $(CPU)`). They were welded to
one variable, so the ICD could not be scheduled for the CPU that runs it
without also raising the instruction-set floor and faulting on .124's Pentium
III. Now `-march=pentium3 -mtune=pentium4`. **Measured: exactly neutral**
(57.2 either way) — kept for the capability, not for a gain.

**`grTexCombine` is shadowed.** It was the one texture-state call the 0.1.5
Glide shadow missed, and on a 2-TMU part it is issued twice per bind. No
regression (57.2), no measurable gain — the profiler then explained why.

**`FX_PROFILE=1`** adds a per-frame counter/cycle dump to `C:\retrogl.log`.

### The 22 ms multitexture cost: five theories, all refuted

Enabling single-pass multitexture cuts per-pixel fill **65%** but adds ~22 ms
of fixed per-frame CPU. Each of these was implemented or switched and measured
on hardware, and none of them is the cause:

| theory | test | result |
|---|---|---|
| texture thrashing in a 4MB bank | `gl_picmip 0/1/2` (16× less texture RAM) | 34.0 → 34.1 fps. No. |
| `glClientActiveTextureARB` flush | skipped it | 32.0 → 32.9. No. |
| per-vertex texcoord submission | dropped the 2nd texcoord | 31.9 → 32.3. No. |
| redundant `grTexCombine` | shadowed it | profiler shows 0 issued/frame. No. |
| Mesa x86 vertex codegen | forced on and off | 30.2 vs 30.5. No. |

The profiler settles what it is *not*: **2 texture-setup calls per frame**, not
thousands, and ~0 cycles in setup. So the cost is not per-surface state at all.
It is flat against resolution (35.6 / 33.9 / 30.2 fps at 512×384 / 640×480 /
800×600), so it is per-frame or per-vertex CPU, still unattributed.

Worth continuing: our per-*pass* fill already beats the MiniGL
(2.55e-5 vs ~3.26e-5 ms/px), so single-pass is the only thing between us and
overtaking it — the model says ~127 fps if the 22 ms goes.

## 0.1.44 — stop advertising an extension we do not accelerate (2026-08-29)

`GL_EXT_point_parameters` is now **withdrawn by default** (`FX_POINT_PARAMS=1`
restores it). We advertised it; Mesa implements distance-attenuated points by
expanding each one into geometry, so an application that takes the extension
gets a *slower* path than its own fallback. 3dfx's MiniGL never advertised it —
Quake II logs `...GL_EXT_point_parameters not found` against the MiniGL and
`...using GL_EXT_point_parameters` against us, then draws its particles the
expensive way.

Measured on .171 (Q2 demo1, 640x480, vsync off, 4 runs per arm, same binary,
zero variance in both):

| | fps |
|---|---|
| advertised (old default) | 51.0 |
| **withdrawn (new default)** | **57.2** |

**+12.2%**, and it needs no env var to get it. Standing against the stock
MiniGL's 90.7: we move from 56% to **63%**.

## 0.1.42 — wglGetProcAddress consulted Mesa before us (2026-08-29)

`wglGetProcAddress` called `_glapi_get_proc_address()` **before** searching our
own `wgl_ext[]` table. Mesa's glapi does not fail on an unknown `gl*` name — it
**synthesizes a dispatch stub** — so it answered for `glSelectTextureSGIS` with
a stub wired to nothing, and the real implementation below it was never
reached. Quake II called that stub the instant multitexture engaged and the
demo1 timedemo stopped completing at all (>180s). Our table is now searched
first; everything we do not implement still falls through to glapi unchanged.

This is a correctness fix independent of SGIS: **any** entry point we add to
`wgl_ext[]` was previously unreachable if its name began with `gl`.

With the shim actually reachable, single-pass multitexture now runs to
completion — and is **slower**, which is a real result rather than a hang:

| config | fps |
|---|---|
| SGIS off (two-pass, shipped default) | **51.0** |
| SGIS on (single-pass) | **30.9** |
| stock 3dfx MiniGL | 90.7 (re-verified, was 91.1) |

So the missing extension was never the whole story. The suspected cause is
texture-bank placement on the split-TMU Voodoo 2: with multitexture engaged the
base-texture pool is confined to one 4MB bank, and Q2's working set then
thrashes. That is the next thing to fix; SGIS stays behind
`FX_SGIS_MULTITEXTURE` until it is a win.

## 0.1.41 — Voodoo 2 (cvg) lane: the stack runs on a Voodoo 2 (2026-08-29)

First execution of the clean-room stack on **Voodoo 2** silicon (.171, Pentium 4
2.8GHz, XP SP3, 12MB card = 4MB FB + 2 TMUs x 4MB). `GL_RENDERER` reports
`Mesa Glide v0.62 Voodoo2 [voodoo-cleanroom 0.1.41]` — MesaFX detects the chip
correctly via `GR_SSTTYPE_Voodoo2`.

- **New `cvg` build lanes** (`FX_GLIDE_HW=cvg`) for glide3x and glide2x →
  `out/glide3x_cvg.dll`, `out/glide2x_cvg.dll`, both dual-ABI. CVG is 3dfx's
  codename for the Voodoo 2; the vintage `retro-3dfx` tree cannot drive this
  card at all (its INFs cover DEV_0003/0005/0009/000B — no DEV_0002).
- **Fixed: the cvg relink emitted no DLL.** `dual_abi_relink` globbed only the
  chip directory, but glide3x/cvg links the SHARED `swlibs/newpci/pcilib`
  objects — `fxnt.c`, the NT layer that opens `\\.\GpdDev` (fxgpio.sys) and
  `\\.\MAPMEM` (fxptl.sys). The relink failed, gcc deleted its output, and the
  next `cp` aborted the script under `set -euo pipefail`. Both relink helpers
  now take an extra object dir.
- **No display driver needed.** A Voodoo 2 is a 3D-only passthrough card
  (Class=MEDIA), so the Intel 865G keeps 2D and `vcr-disp` is out of scope —
  this is the first box where the whole 3D stack can be ours without one.
- **`-mtune=pentium4`** for the cvg lane (`-march=pentium3` retained so one
  artifact still runs on .124's Pentium III).
- **GL_SGIS_multitexture: implemented but OPT-IN** (`FX_SGIS_MULTITEXTURE=1`),
  see `patches/mesafx-voodoo2-icd.patch`. Quake II predates
  ARB_multitexture and probes only the SGIS name, so the stock MiniGL gets
  single-pass lightmapping and we do not. Advertising SGIS does flip Q2 over
  (`...using GL_SGIS_multitexture` in qconsole.log) but the timedemo then never
  finishes (>180s vs 13.5s). Not root-caused, so it stays off by default.

**Measured** (Quake II demo1, 640x480, vsync OFF, 689 frames, zero variance):

| renderer | fps |
|---|---|
| stock 3dfx MiniGL (`3dfxgl`) | **91.1** |
| our MesaFX ICD (`retrogl`) | **51.0** |
| Intel 865G onboard (control) | 58.8 |

The MiniGL implements only what Quake needs; part of that gap is structural.
Closing the rest is the open work — the SGIS path above is the main lead.

## Stack composition legend

A "driver version" here is OUR OpenGL ICD (`retro3dfx-gl`, MesaFX 6.2 fork).
Until 0.1.6 every benchmark ran on a **hybrid** stack:

| Layer | 0.1.1–0.1.6 benchmarks | target (top-to-bottom ours) |
|---|---|---|
| XP display driver (kernel) | **AmigaMerlin 2.9** (retail) | our `3dfxvsm.sys`+`3dfxvs.dll` (deployed build from the private retro-3dfx repo) |
| glide3x.dll (3D HAL) | **AmigaMerlin retail** (`_grFoo@N` underscore ABI) | retro3dfx-glide (our sezero/glide fork) |
| OpenGL ICD (`retrogl.dll`) | **ours** — retro3dfx-gl vN | ours |

The hybrid required linking our ICD against the *retail* glide import lib
(`build-mesafx-retail.sh`); the all-ours stack uses the default
`build-stack.sh` link (our glide exports both `grFoo` and `grFoo@N`).

## Versions

### 0.1.1 — baseline (versioning introduced)
- **Change:** none functional; version stamp added to `GL_RENDERER`, buffer
  widened 64→96B for the marker.
- **Why:** benchmarks must self-document the driver build.
- **Results (Q3 1.32 `timedemo four`, 16bpp, P3-845/Voodoo3/XP, hybrid stack):**
  640x480 **53.7**, 800x600 50.4, 1024x768 **38.7** fps.
  With `FX_GLIDE_SWAPINTERVAL=0` env: 57.6 / — / 51.0.

### 0.1.2 — modern compiler codegen (fork `1bfd219`)
- **Change:** build flags `-march=$(CPU) -mfpmath=sse -DNDEBUG` (was
  `-mtune` only → gcc13 emitted pentiumpro **x87** for every C hot loop);
  `FX_PACK_UBYTE` branchless SSE color pack in `fxvbtmp.h` (7 sites) replacing
  `UNCLAMPED_FLOAT_TO_UBYTE` (store-forwarding stall + 2 branches/component);
  host-tool `gen_matypes` rule filters target-only flags (64-bit host gcc
  errors on `-march=pentium3`).
- **Why:** the audit found fxvb.o had **zero** SSE instructions; P3 has SSE1.
  After: 2729 SSE scalar ops in fxvb.o.
- **Result:** 640x480 54.2 (**+0.9%** vs 53.7). Honest read: most of the
  640x480 frame is Q3 engine + Glide, not our C loops.

### 0.1.3 — batched triangle submission (fork `3a4e790`)
- **Change:** `fx_render_vb_triangles` → one
  `grDrawVertexArrayContiguous(GR_TRIANGLES, ...)`; indexed path
  `fx_render_triangles_elts_batched` submits 768-vert pointer-array chunks via
  `grDrawVertexArray` instead of one `grDrawTriangle` DLL call **per triangle**.
- **Why:** cut DLL-boundary overhead (~1 call/tri on Q3 world geometry).
- **Result:** tuned-env 640x480 58.1 (**+0.9%** vs 57.6), 1024x768 51.2 (flat).
  Lesson: retail glide3x loops per-triangle internally, so only the call
  boundary was saved — smaller win than estimated.

### 0.1.4 — swap-default env injection (fork `424bd32`) — **INERT**
- **Change:** set `FX_GLIDE_SWAPINTERVAL=0` etc. via `_putenv` +
  `SetEnvironmentVariableA` before `grGlideInit` when the user hasn't.
- **Why:** make the measured +7%/+32% swap tuning a driver default.
- **Result:** no effect. Retail glide3x is static-CRT and snapshots the
  environment at **DLL load** — before any ICD code can run. Kept (harmless;
  correct for late env readers) but superseded by the machine-env deploy step.

### 0.1.5 — Glide state shadow cache (fork `204555d`)
- **Change:** TU-local shadow of `grTexClampMode/FilterMode/MipMapMode/
  TexSource/grAlphaCombine/grColorCombine` in `fxsetup.c`; identical calls
  skipped. Reset on `grSstWinOpen` and `grGlideSetState` (MakeCurrent).
- **Why:** every `glBindTexture` re-issued the full 8-10-call register set;
  Q3 rebinds per surface.
- **Result:** 640x480 54.9 no-env (**+0.7%** vs 54.2). Quality screenshot
  (in-engine glReadPixels, q3dm1): pristine — no regressions from 0.1.2-0.1.5.

### 0.1.6 — swap-interval env-read fix (fork `06497b2`)
- **Change:** read `FX_GLIDE_SWAPINTERVAL` from the **process env** with our
  CRT (`getenv`) instead of Glide's `grGetRegistryOrEnvironmentStringExt`;
  default 0 when unset.
- **Why:** bisect proved `FX_GLIDE_SWAPINTERVAL=0` alone = 51.3 @1024 (+32%);
  the other tuned vars were inert. Via Glide's reader, a system-wide
  `FX_GLIDE_SWAPINTERVAL=1` (planted by the 3dfx tools install) reached us.
- **Result:** still 38.7 @1024 — because the retail **glide3x itself** also
  reads the env var from its own load-time snapshot and **ignores the
  `grBufferSwap(interval)` argument**. No ICD-side code can override it.
  Machine-wide env (`HKLM\...\Session Manager\Environment`) is the working
  mitigation for the hybrid stack; on OUR glide3x the default is ours in code.

## Swap-interval saga (summary of findings)

1. Launcher env `FX_GLIDE_SWAPINTERVAL=0` → 51.3 fps @1024 (works; process
   env at creation is in every snapshot).
2. Runtime env injection from ICD (0.1.4) → inert (glide snapshots at DLL load).
3. Registry `Services\{3dfxvs,banshee,3Dfx}\Device0` → inert for this build.
4. ICD-side interval fix (0.1.6) → inert (glide ignores the swap argument).
5. Root cause of the "mystery vsync": the environment actually contains
   `FX_GLIDE_SWAPINTERVAL=1` system-wide (3dfx tools artifact) — glide honors
   it. Overwritten to 0 in `Session Manager\Environment` on .124.

## MILESTONE 2026-07-17 — all-retro3dfx stack live, beats AmigaMerlin

Our XP kernel display driver (deployed build from the private retro-3dfx repo)
replaced AmigaMerlin on .124 via SetupAPI (`deploy-3dfx-driver` skill). Desktop
2D at 1024x768x32@75 correct. Our glide3x build (underscore ABI) binds the
existing retail-linked MesaFX 0.1.6 without a rebuild. Q3 renders pristine
(in-engine screenshot parity with the hybrid baseline — mean pixel diff
4.1/255 = animation noise).

## Cumulative scoreboard (Q3 timedemo four 16bpp)

| Config | 640x480 | 1024x768 |
|---|---|---|
| HYBRID 0.1.1, env untouched (`SWAPINTERVAL=1` system-wide) | 53.7 | 38.7 |
| HYBRID 0.1.6, env untouched | 54.2 (+0.9%) | 38.7 |
| HYBRID 0.1.6 + `FX_GLIDE_SWAPINTERVAL=0` | ~58 (+8%) | ~51 (+32%) |
| **ALL-RETRO3DFX 0.1.6, no env tuning** | **58.8 (+9.5%)** | **51.3 (+32.6%)** |
| Era references (P3-850/933 + V3 3000, 3dfx ICD) | 75-91 | **44.3 — we beat this** |

The all-ours stack needs no tuning: our glide3x's swap defaults are sane in
code. At fillrate-bound 1024x768 we exceed the era's official 3dfx ICD
reference by ~16%.

Remaining CPU-side gap at 640x480 (~ -30%) is the target of the queued deep
work: SSE intrinsics vertex emit, SSE 4-wide cliptest (rcpps + Newton-Raphson
replacing serial x87 fdiv), end-to-end ubyte colors.

## Optimization campaign 2026-07-17 (0.1.7–0.1.11) — the "queued deep work", answered

The queued CPU-side work above was attempted as one fork branch per idea, each
A/B'd on `.124` (P3-845 Voodoo3) against the 58.8/51.3 baseline across the
resolution × quality grid, every run in specpicks. **Result: the vertex path is
already near-optimal — none of the fps optimizations merged.** This is a real,
useful finding, not a failure: the stack that already beat AmigaMerlin and the
era 3dfx ICD has little vertex headroom left on this card.

| Ver / branch | Change | 640x480 | Verdict |
|---|---|---|---|
| 0.1.7 `opt/lto` | `-O3 -funroll-loops` (was `-O2`) | 58.7 | **INERT** — the hot path is already SSE; `-O` can't remove the algorithmic cost. Not merged. |
| 0.1.8/0.1.9 `opt/sse-cliptest` | SSE 4-wide cliptest + `rcpps`+Newton perspective divide (transpose-load in 0.1.9) | 38.7 | **REGRESSION** — Josh Vanderhoof's hand-tuned x86-asm cliptest beats C intrinsics at the CPU-bound res. Renders correctly (4.5/255). Confirms the cliptest *is* a real hot-path lever — just already optimal. Dropped. |
| 0.1.10 `opt/sse-emit` | SSE `movaps` viewport emit (vs 3 scalar MACs) | 58.7 | **INERT** — emit is a negligible fraction of the frame. Renders identically (0.60/255). Not merged. |
| **0.1.11 `opt/lod-bias`** | **QUALITY: default `-0.5` texture LOD bias** | (quality) | **MERGE candidate** — sharpens textures on the V3 bilinear+nearest-mip path (classic 3dfx trick); `FX_LOD_BIAS` env-tunable. |

**Conclusion for the MesaFX/V3 lane:** the transform is SSE (`sse.c`), the
cliptest is tuned asm, the emit is small, and the V3 is single-TMU (no
multitexture single-pass lever, unlike the Voodoo5 lane). The remaining wins on
this card are **quality** (LOD bias) and higher-res/quality coverage, not vertex
fps. The 640x480 gap to the era P3-850/933 references is CPU-clock (845 MHz) +
engine, not driver inefficiency.

## Q2 support + game-integration findings 2026-07-18 (0.1.12–0.1.19)

**Quake II now runs on our MesaFX ICD** (was stock `3dfxgl` only). Root cause of
the prior green-screen / "driver stopped working" crash: `retrogl.dll` binds
`glide3x.dll`, and the Q2 dir shipped none, so the loader resolved an incompatible
build and `grSstWinOpen` faulted. Fix = stage the known-good retail `glide3x.dll`
(344064 B, AmigaMerlin) next to `quake2.exe`. Also added a window message-pump
before `grSstWinOpen` in `fxwgl.c` (harmless; a freshly-shown ref_gl window's
activation messages were queued). Full diagnosis: `retro3dfx/DEBUGGING-NOTES.md`.
- Q2 @640×480×16: **93.6 fps** on our ICD vs **75.7** stock 3dfxgl (+23%),
  and stable. Res sweep to Voodoo3 max: 640=93, 800=69, 960=51, 1024=47,
  1152=38, 1280=32, 1600×1200=20.8 fps.
- New env knobs (default OFF): `FX_NO_PALETTED_TEXTURE`, `FX_NO_MULTITEXTURE`
  (hide those extensions to force an engine onto the RGBA / single-texture path).

**Counter-Strike 1.6 (GoldSrc): not supported on our ICD** — architecture
mismatch (GoldSrc GDI-fullscreen-mode + render-into-desktop vs our Glide-exclusive
board grab). hl.exe exits after GL init. Runs on the stock `gldrv\3dfxgl.dll`
MiniGL. Not an extension issue (ruled out paletted + multitexture). Details in
DEBUGGING-NOTES.md.

**Q3 unchanged**: 57.9 fps @640 (tied with the 0.1.11 best); high-res sweep added
to 1600×1200 (22.9 fps). The V3 vertex/transform path remains near-optimal for fps.

## Refresh + cursor session 2026-08-03 (0.1.34–0.1.35)

> **⚠️ LOST (found 2026-09-23).** Neither change below exists in any source
> today — not in any `retro3dfx-gl` commit, not in `patches/`, not in either
> local clone. They were never committed. Builds 0.1.36 onward came from a fresh
> clone of the fork (the `cvg` worktree, 2026-08-28), and the main clone that
> may still have held them was re-created on 2026-09-04. Every build from 0.1.36
> to 0.1.61 opens fullscreen at 60 Hz and has no software cursor. `tests/native/test_fx_best_refresh.c` and
> `test_fx_cursor_overlay.c` still pass because they copy the logic. Restoring
> both is on the roadmap (README §17.3).

### 0.1.34 — fullscreen refresh: monitor-max instead of hardcoded 60Hz
- **Change:** `fxapi.c fxMesaCreateBestContext()` no longer hardcodes
  `GR_REFRESH_60Hz`. New `fxBestRefresh(w,h)`: env override
  (`FX_GLIDE_REFRESH_RATE` / `SSTV2_REFRESH_RATE` / `MESA_FX_REFRESH`, Hz), else
  the monitor's max refresh for that WxH from `EnumDisplaySettings`
  (EDID-filtered — can't exceed monitor caps), snapped DOWN to the nearest
  `GR_REFRESH_*` Glide has a timing for. If `grSstWinOpen` still rejects the
  rate, one retry at 60Hz (a bad rate degrades, never fails the context).
  `FX_GLIDE_REFRESH_RATE=60` restores the old behaviour.
- **Why:** Glide programs the video timing itself in fullscreen — GoldSrc's
  `-freq`, GDI mode sets, and XP's refresh dialogs are all bypassed, so every
  GL game ran at 60Hz on a 100Hz-capable monitor (.124 CS 1.6 verified 60→100Hz
  via retrogl.log: `grSstWinOpen ref=6`, open OK).
- **Test:** `tests/native/test_fx_best_refresh.c` (snap table mirror).

### 0.1.35 — fullscreen software cursor overlay (+ FX_DUMP_FRONT debug dump)
- **Change:** `fxapi.c fxDrawCursorOverlay()` — when `GetCursorInfo` says the
  cursor is showing, stamp a classic 11×19 arrow (black outline / white fill,
  transparent elsewhere) into the back buffer via `grLfbLock` right before
  `grBufferSwap`. Desktop→Glide coordinate scaling, full edge clipping,
  565/1555/8888 paths. `FX_CURSOR=0` disables. Gameplay hides the OS cursor, so
  the overlay costs nothing in-game. Also `fxDumpFrontBuffer()`:
  `FX_DUMP_FRONT=<path>` dumps the front buffer raw every 64th swap (GDI
  screenshots can't see Glide scanout; this is the remote verification path).
- **Why:** fullscreen Glide scanout never composites the GDI/hardware cursor
  plane — CS 1.6's GL menu pointer was invisible (D3D mode showed it). Verified
  on .124: front-buffer dump shows the arrow at the clicked position in the CS
  menu.
- **Test:** `tests/native/test_fx_cursor_overlay.c` (bitmap + stamp/clip mirror).

## glide2x bring-up session 2026-08-04 (Unreal Gold 3dfx renderer)

### glide2x: XP bring-up fixes + dual-ABI exports (fork 79ee51e)

> **⚠️ LOST (found 2026-09-23).** Fork commit `79ee51e` was never pushed: it is
> in neither local clone nor on GitHub, so the current `out/glide2x.dll` lacks
> these guards. The dual-ABI half (`dual_abi_relink2`, repo `809c567`) is intact.
> `tests/native/test_glide2x_mapboard_guards.c` copies the logic and still passes.
- **Problem:** selecting the 3dfx renderer in Unreal Gold (GOG) hard-wedged
  .124 — the GOG install ships **nGlide** as game-local `glide2x.dll`, whose
  failing grSstOpen attempts froze the chip (physical power cycle needed).
  Our own glide2x had never worked either: no `_grFoo@N` (MSVC) exports, and
  a GPF inside `grGlideInit`.
- **Fixes:**
  1. `build-stack.sh`: dual-ABI relink for glide2x (same as glide3x's) — the
     Glide2-era games are MSVC-linked and import `_grFoo@N`.
  2. `glide2x/h3/minihwc/minihwc.c` (fork 79ee51e): port of the verified
     glide3x XP fixes — GETLINEARADDR prime before ALLOCCONTEXT, zero-base /
     failed-escape guards in hwcMapBoard, plus clearing
     `linearInfo.initialized` on failure (Unreal's error callback doesn't
     exit; hwcInitRegisters' only defense is that flag).
- **Deployed:** game-local `Unreal Gold\System\glide2x.dll` (nGlide kept as
  `.nglide` backup), `system32\glide2x.dll` (2003-era copy kept as
  `.old2003`).
- **Verified on .124:** standalone Glide2 exerciser full pass (init → query →
  WinOpen → 60 swaps → close, desktop restored); Unreal Gold fullscreen Glide
  640x480x16 **@100Hz**, stable.
- **Operational rule (hard-won): NEVER `taskkill /f` a fullscreen Glide2
  game** — killing mid-FIFO-packet wedges the chip beyond the display
  driver's bounded waits (bus-level hang, physical power cycle). Exit via the
  game's own quit path.
- **Test:** `tests/native/test_glide2x_mapboard_guards.c`.
