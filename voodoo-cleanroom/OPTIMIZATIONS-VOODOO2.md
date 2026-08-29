# Clean-room Voodoo **2** stack — optimization log

Companion to `OPTIMIZATIONS.md` (which covers the **Voodoo 3** lane on .124).
Different box, different card, different baseline, so it gets its own log.

Target: **.171** `NSC-5B996B81319` — Intel **Pentium 4 2.8 GHz** (SSE2), 509 MB,
Windows XP SP3, Dell i865G board. 2D is the **onboard Intel 865G**; 3D is a
single **3dfx Voodoo 2, 12 MB** (4 MB FB + 2 × 4 MB TMU). A second Voodoo 2 was
fitted until 2026-08-28 and was removed — with both cards in, Glide hung.

Stack: MesaFX ICD (`retrogl.dll`) over the box's **stock 3dfx Glide 3.03.00**.
Our own `glide3x_cvg.dll` is built but not yet deployed.

> **A Voodoo 2 is a 3D-only passthrough card** (INF `Class=MEDIA`), so it never
> appears as a display adapter and `VIDEODIAG`/`PCISCAN` will not show it. No
> display driver is needed — the Intel 865G keeps 2D. This is the only fleet box
> where the whole 3D stack can be ours without `vcr-disp`.

## Method

Same discipline as the Voodoo 3 lane, plus one rule learned expensively here:

- **One change per version**, stamped into `GL_RENDERER`
  (`Mesa Glide v0.62 Voodoo2 [voodoo-cleanroom 0.1.N]`).
- **A/B on real hardware**, Quake II `demo1` timedemo, 640×480×16, **vsync OFF**
  (`gl_swapinterval 0` + `cl_maxfps 1000`), 689 frames, ≥3 runs discarding the
  first. Variance on this box is essentially zero, so differences of 0.5 fps are
  real.
- **Deploy game-locally** (`C:\Games\Quake2Complete\retrogl.dll`, selected by
  Quake II's `gl_driver`). Nothing in `system32`, no registry, no reboot;
  rollback is one `del`.
- **Measure the ceiling before writing code.** Every proposal must come with a
  cheap switch that removes the work *entirely*, and that gets benchmarked
  first. Two agents estimated +12% for a texture-upload change whose real
  ceiling, measured with `gl_dynamic 0`, was **+0.7%**.
- **Verify the artifact, not the flags.** `objdump -p | grep "DLL Name"` after
  every build — a stray libgcc import makes the ICD fail `LoadLibrary` with no
  diagnostic beyond "could not load".

## Baseline and the bar

| renderer | fps | note |
|---|---|---|
| stock 3dfx MiniGL (`3dfxgl`) | **90.7** | the bar; single-pass multitexture |
| our ICD at first run | 51.0 | 56% of the bar |
| **our ICD, shipping (0.1.60)** | **57.2** | **63%** |
| Intel 865G onboard (control) | 58.8 | different GPU, for scale |

> Everything measured before 2026-08-28 was **vsync-capped at ~59 fps** and told
> us nothing: both renderers read 59.x and looked identical. Always force
> `gl_swapinterval 0`.

The 1998 Voodoo 2 beats the 2003 onboard Intel by **51%**, despite the 865G's
higher paper fill rate (266 vs 90 Mpixel/s) — the IGP is UMA and shares memory
bandwidth with the CPU.

## Where the frame goes

Fitting `t = a + b·pixels` across three resolutions (80.5 / 57.2 / 37.2 fps at
512×384 / 640×480 / 800×600):

```
a = 2.4 ms fixed CPU     b = 5.10e-5 ms/px
frame 17.48 ms  =  ~2.4 ms CPU + ~15.1 ms fill
pixels touched  =  5.12 x screen   (Q2 draws the world twice: ~2.56x per pass)
CPU idle        =  ~69% of every frame
```

**The card is the wall.** Every fps gain must come from making the GPU touch
fewer pixels; there is a large idle CPU surplus to spend doing it.

Cost of each chunk of GPU work, measured by removing it:

| switch | fps | what it isolates |
|---|---|---|
| baseline | 57.2 | — |
| **`r_fullbright 1`** | **92.9** | the lightmap pass costs **6.72 ms = 38% of frame** |
| `r_drawentities 0` | 66.3 | entities cost 2.40 ms |
| `r_drawworld 0` | 206.6 | the world costs 12.64 ms |
| `gl_dynamic 0` | 57.6 | dynamic lightmap uploads are negligible |

**Single-pass rendering reaches 92.9 fps — past the MiniGL.** The second pass is
the whole prize, worth ~+62%.

## MERGED (shipped wins)

| ver | change | effect |
|---|---|---|
| 0.1.44 | **Withdraw `GL_EXT_point_parameters`** (`FX_POINT_PARAMS=1` restores) | **51.0 → 57.2 fps, +12.2%** |
| 0.1.42 | `wglGetProcAddress` searches our `wgl_ext[]` **before** Mesa's glapi | correctness; unblocked every `gl*` entry point we add |
| 0.1.52 | `-static-libgcc` on the ICD; 0.1.60 on the Glide lanes | prevents a silent `LoadLibrary` failure |
| 0.1.52 | `-march`/`-mtune` split (`TUNE ?= $(CPU)`) | capability; **measured neutral** |
| 0.1.52 | `grTexCombine` added to the Glide state shadow | no regression, no gain (only 2 setup calls/frame) |
| 0.1.53+ | **`FX_PROFILE=1`** whole-frame instrumentation | how everything below was settled |

**Why point_parameters was a win:** we advertised an extension we do not
accelerate. Mesa expands distance-attenuated points into geometry, so Quake II
took a path *slower* than its own particle fallback. The MiniGL never advertised
it — the asymmetry was visible in the game's own log.

## TESTED-AND-REJECTED (honest negatives)

Each was implemented or switched and **measured on hardware**. None is merged.

| idea | test | result |
|---|---|---|
| `glTexSubImage2D` partial upload (est. +12%) | `gl_dynamic 0` removes it entirely | **+0.7% ceiling** — not worth the risk |
| `-mtune=pentium4` (est. +3%) | built and benched | **exactly neutral**, 57.2 either way |
| Mesa x86 vertex codegen | forced on and off | 57.1 vs 57.2; 30.2 vs 30.5 |
| `-O3`, LTO, `-march=pentium4` | — | rejected: SSE2 would fault on .124's PIII |

### The multitexture regression — seven refuted theories

`GL_SGIS_multitexture` (Quake II predates ARB and probes only the SGIS name) is
implemented and **works**: it cuts per-pixel fill by 65%. But it costs a **fixed
~30.5 ms/frame**, so it ships **off**, behind `FX_SGIS_MULTITEXTURE`.

Every driver-side metric is *identical* between the fast and slow modes:

| per frame | SGIS off (57.2) | SGIS on (32.0) |
|---|---|---|
| texture setup calls | 2 | 2 |
| pipeline runs / vertices | 75 / 4728 | 75 / 4716 |
| TNL pipeline cycles | ~15.2 M (5.4 ms) | ~15.6 M (5.5 ms) |
| immediate-mode `glBegin..End` | 0.26 ms | 0.29 ms |
| texture downloads / blocking swap | 0 / 0 | 0 / 0 |
| vertex fixups / immediate vertex size | 5 / 4 floats | 5 / 4 floats |

Refuted, each by measurement: texture thrashing (`gl_picmip 0/1/2`, 16× less
texture RAM → 34.0 vs 34.1 fps) · the `glClientActiveTextureARB` unconditional
`FLUSH_VERTICES` (32.0 → 32.9) · per-vertex texcoord submission (31.9 → 32.3) ·
redundant `grTexCombine` (profiler: 0 issued/frame) · Mesa vertex codegen ·
texture download traffic (0/frame) · vertex-format fixup thrash (5/frame both).

The penalty is also **flat against resolution** — 32.9 / 32.7 / 32.1 fps at
320×240 / 512×384 / 640×480 — so it is not fill at all.

> **Retracted:** 0.1.58 concluded this cost lived in Quake II. It does not. The
> stock MiniGL uses the *same* `GL_SGIS_multitexture` path and reaches 90.7 fps,
> so the ~24 ms is ours. What remains uninstrumented is **`glide3x.dll`
> itself** — a real suspect, because the MiniGL links **glide2x**, a different
> library with a different dual-TMU path.

## Current state

Shipping **0.1.60** at **57.2 fps**, 63% of the MiniGL. Per-*pass* fill already
beats it (2.55e-5 vs ~3.26e-5 ms/px) — the MiniGL wins only by doing one pass
where we do two.

## Open levers

1. **Instrument `glide3x.dll`** and deploy our `glide3x_cvg.dll`. Highest value
   by far: the single-pass ceiling is 92.9 fps.
2. **Software occlusion culling** — the Voodoo 2 has no early-Z or hierarchical
   Z, depth complexity is ~2.56× per pass, and the CPU is 69% idle. Bounded by
   how much Quake II's PVS already removes; measure the ceiling first.
3. **Paletted textures** (`GR_TEXFMT_P_8`) to halve texel bandwidth.
4. **CPU floor**: 5.4 ms of TNL for 4728 vertices (~3,200 cycles/vertex) becomes
   the binding constraint the moment pixels are cut ~50%.

## SLI (two Voodoo 2s)

3dfx's SLI check (`glide3x/cvg/init/sli.c:87-96`) compares **`numberTmus`,
`fbiBoardID`, `fbiVideoStruct`** — the RAM-size comparisons are commented out,
so **an 8 MB + 12 MB pair runs as 2 × 8 MB**. `SSTV2_MISMATCHED_SLI` bypasses
the board-ID check and exists **only in our Glide**, not the stock 3dfx one.
Board identity, and why the PCI subsystem ID is useless for it, are documented
in [`scripts/voodoo2/README.md`](../scripts/voodoo2/README.md).
