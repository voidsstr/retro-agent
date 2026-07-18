# retro3dfx ICD — codebase review findings (2026-07-18)

Three deep source reviews of our MesaFX fork vs the H5 3dfx reference, to make the
driver more well-rounded / higher-quality / more compatible across games and
Win98+XP. Ranked, with file:line. Status tracked as we implement.

## CS 1.6 / GoldSrc — root cause CONFIRMED + fix path

**Confirmed empirically:** with texture-upload tracing on, GoldSrc crashes with
**zero textures uploaded** — it dies right after context creation, before any
render. Not textures, not extensions (paletted/multitexture both ruled out). It is
the **fullscreen-exclusive mode conflict**: `grSstWinOpen` →
`DXDRVR.C:433 SetCooperativeLevel(DDSCL_EXCLUSIVE|DDSCL_FULLSCREEN)` +
`SetDisplayMode`, which collides with GoldSrc's own `ChangeDisplaySettings(
CDS_FULLSCREEN)` + its GDI ownership of the primary surface.

**Fix = windowed Glide rendering (Option B).** H5 has a separate windowed API
(`GSFC.C grSurfaceCreateContext(GR_SURFACECONTEXT_WINDOWED)`, `DDSCL_NORMAL`, Blt
back→front to the window rect — `DXDRVR.C:363-426,812-853`). **The runtime
`glide3x.dll` on .124 (AmigaMerlin retail, 344064 B) EXPORTS this API** (verified:
`grSurfaceCreateContextExt`, `grSurfaceSetRenderingSurfaceExt`,
`grSurfaceSetAuxSurfaceExt`, `grSurfaceCalcTextureWHDExt`,
`grSurfaceReleaseContextExt`, `hwcAllocWinContext`). So windowed rendering — which
also makes the driver more flexible/well-rounded generally — is implementable.
Our fork currently binds none of these (`fxg.h` table has only `grSstWinOpenExt`),
and the "windowed" branch in `fxwgl.c:326` is a dead stub that still funnels into
the fullscreen `grSstWinOpen`.

## Quality wins (agent 3) — free on this present-bound card
- **A1 default gamma** — NOTHING sets gamma anywhere; this is why 16-bit 3dfx
  looks dark. Add `grLoadGammaTable` pow-ramp (~1.3) at context init in `fxapi.c`
  after `grSstWinOpen`; env `FX_GAMMA`; restore identity on destroy. H5 ref:
  `MINIHWC.C:6300` `hwcGammaRGB`. Zero fps cost. **Biggest visual win.**
- **A2 force GR_DITHER_4x4 at init** — Glide resets to 2x2 (`GSST.C:972`); Mesa
  only sets 4x4 on a live `glEnable(GL_DITHER)` toggle, so games that never toggle
  it run at 2x2. Force 4x4 once at init (`fxdd.c:1813` / `fxapi.c`). Free; smoother
  gradients/skies.
- **A3 LOD bias** already correct on both TMU paths (default -0.5); `-0.75` is a
  safe "max sharpness" option.

## Compatibility gaps (agent 2)
- **A1 (compat, CRITICAL) alpha PFD on Voodoo3** — `fxwgl.c:791 pfd_tablen()`
  returns only 2 formats on V3 (no alpha), and the matcher hard-rejects any
  `cAlphaBits>0` request (`fxwgl.c:850`) → **alpha-requesting games can't create a
  context at all** (SDL/GLFW default to 8 alpha bits). Expose the ARGB1555 entries
  (`pix[2]/[3]`) on V3. Highest compat unblock.
- **C1 paletted default OFF** — `fxdd.c:1859` defaults paletted-texture ON, which
  crashes GoldSrc/HL; invert to opt-in.
- **C2 texture_env_combine on V3** — `fxdd.c:1909` gates ARB/EXT_texture_env_combine
  behind Napalm-only `HaveCmbExt`, so V3 never gets it → lightmap games (Q3
  overbright, SoF, JK2…) render wrong/dark. Implement base-unit GL_COMBINE. Biggest
  correctness win (higher effort).
- **B1 wglUseFontBitmapsW** returns FALSE (`fxwgl.c:753`) → Unicode games get no
  text; delegate to the working A variant. One-liner.
- **B2/B4 wglChoosePixelFormatARB / wglGetPixelFormatAttribivARB** stubbed FALSE
  (`fxwgl.c:485,461`) → ARB-pixel-format games fail. Implement via pix[].
- **C3 S3TC on V3** via SW decode (`fxdd.c:1905`) — later games ship only DXT.

## Win98/XP robustness (agent 3)
- **B1 freopen("MESA.LOG") unguarded + relative path** (`fxwgl.c:301`) — on XP a
  read-only CWD makes freopen close stderr → later fprintf writes to a dead stream.
  Use GetTempPath + NULL-check.
- **B2 always-on debug fprintf** in `wglCreateContext`/fxapi grSstWinOpen traces —
  gate behind TDFX_DEBUG.
- **B3 WndProc restore from stale HDC** (`fxwgl.c:364`) — mode-toggling games leak
  the `__wglMonitor` subclass → dangling proc crash. Store HWND at create, IsWindow
  check, null all globals on delete.
- **B4** the `__wglMonitor` subclass intercepts nothing — consider removing it
  (deletes the B3 failure mode).

## Implementation order
Phase 1 (low-risk, broad value — quality + compat + robustness): A1-gamma,
A2-dither, compat-A1-alpha-PFD, C1-paletted-off, B1-fontW, robustness B1/B2. Build,
A/B Q2+Q3 (no-regression gate), quality screenshots.
Phase 2 (CS real fix): Option B windowed Glide surface path (bind grSurface*Ext,
windowed create + Blt present), guarded to fall back to fullscreen for Q2/Q3.
Phase 3 (higher effort): C2 texture_env_combine, B2/B4 ARB pixelformat, C3 S3TC.
