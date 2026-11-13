# Direct3D on the Voodoo 5 6000 (vcr-kmd): limitations and fix plan

**Date:** 2026-11-13. **Driver state:** `.124` runs vcrdd 95678704 / vcrmp fda1b8c9 with
`Diag\D3D32=1`, `Diag\D3DBigTex=7`, `LogLevel=3`. The code is as of commit e3056d1.
The clean-room lane is `voodoo-cleanroom/vcr-kmd` (not the vintage H5 lane).

**Evidence:** everything cited is under
[`../evidence/d3dplan_1112/`](../evidence/d3dplan_1112/):

- `reports/` holds eight reader reports: our HAL, 3dfx's H5 HAL, the VSA-100 spec, the games, the history, performance, the D3D8/9 runtime, and every saved screenshot.
- `onbox/` holds the 2026-11-12 run on `.124`, with its README and a run-by-run table.
- `designs/G*.md` holds one design per fixable or open gap.
- `workflow_results.json` holds the 97 deduplicated gaps, 86 designs and 45 adversarial verdicts as data.
- `caps_124.txt` holds the D3D8 caps as the runtime reports them.

> **How this plan was made, and what is not verified.** A multi-agent workflow produced it. Its stages were:
> - eight read-only readers and one supervised on-box run;
> - a merge into 97 gaps;
> - one design per gap;
> - two adversarial verdicts per design: code-and-hardware, and impact-and-priority.
>
> **The weekly usage limit stopped the workflow partway through the verify stage.** Verdicts exist only for
> **G01-G22, G26, G28 and G30** (G18 has none; G22, G26 and G30 have one each). Every other design is the designer's own
> priority and has **no skeptic review**. The table in the appendix says which is which. The automated
> plan writer and completeness critic never ran, so this document was written by hand from the designs
> and verdicts. Where the two verdicts disagreed, the lower priority was taken unless the evidence for the higher
> one was measured. Re-run the verify stage for the unreviewed designs after 2026-11-18, with the same
> script and `resumeFromRunId wf_f3ff1396-113` so finished work replays from cache, before you
> start any P2/P3 item.

---

## 1. What limits Direct3D on this card today

Our HAL is a DX7-level NT DDI (DrawPrimitives2/DP2EX, no T&L). D3D8/9 titles run on it through the
runtime's software vertex processing. **3dfx's own W2K/XP HAL was also DX7-only**, so D3D8 titles saw
caps built by the runtime on their driver too (`reports/vendor.md`, `reports/runtime.md`). The limits fall into
four kinds.

### 1.1 Stability: the engine can be left unusable, and Direct3D cannot tell (P0)

- **A killed Glide or OpenGL client leaves command FIFO 0 enabled** (G01). Every later D3D triangle is then
  silently dropped while clears still work. This was measured on 11-12: 9/40 and 7/42 passing, with
  `cmdFifo0` on and readPtr stuck. Once the FIFO was recovered by a paced mode set, the same probes passed
  40/40 and 42/42. A windowed run in that state made the 2D idle check give up, and the D3D HAL was gone
  until the next mode set (`onbox/README.md` runs 5-10).
- **That give-up is silent** (G02). DrawPrimitives2 and Clear2 keep answering DD_OK and draw nothing, so the
  picture freezes while the game runs. This matches Max Payne's 2026-10-04 symptom (G04). A Glide session
  had run just before that freeze, which makes G01 the leading suspect, but this is **not proven**.
- **The HAL relies on 3D state that Glide leaves behind** (G03): combineMode, the 2-pixels-per-clock exit,
  clip rect 1, chroma, stipple and aaCtrl. A TMU always takes its selects from combineMode, so after any
  cleanly closed MesaFX OpenGL title, D3D can draw through MesaFX's last extended combine.
  `Diag\Reset3D` (default off) already clears this state at a clean owner release. It does nothing for
  a killed client, and it is not armed on `.124`.

### 1.2 Visible rendering defects in the staged titles (P0/P1)

These were measured in the first in-game D3D frames on this HAL (11-12: the UT2004 botmatch, the UT2003
flyby and Halo's timedemo):

| symptom | cause (status) | gap |
|---|---|---|
| Block "confetti" noise on terrain and rock (UT2003, UT2004, some Halo rock) | Mip chains are split in video memory. XP creates one surface per level and we place each level separately, while the TMU reads levels at fixed offsets from the top. Proven by address arithmetic on UT2003's 11-08 placement log; the fix is not yet tried on silicon | G07 (cause, fix), G06 (in-game proof) |
| UE2 world very dark, lightmaps half-bright | MODULATE2X/4X drawn as MODULATE (stage 0 and stage 1) | G05 |
| UE2 solid-white sky and horizon, white polygons | DM-Antalus's sky gradient is an 8x512 DXT5 texture (64:1). The hardware's limit is 8:1, and the texture is refused. Found from the staged data, not yet proven on silicon | G18 |
| Halo glows and lens flares drawn as flat additive squares; the a30 sky black | X8R8G8B8 textures are never created (the runtime maps a DX7 32-bit RGB format to X8 only for one exact flag value), so the stage draws vertex colour | G17 |
| UT2003 blue grass, grey checkerboard | open | G44 |
| UE2 renders at 16 bpp although the ini asks for 32 | **not a driver bug**: UE2's D3DDrv forces 16 bpp for PCI vendor 0x121a | G19 |
| Decals and bullet holes z-fight | ZBIAS stored and never used. UE2, Hidden & Dangerous and the UE1 D3D renderers all send it | G22 |

### 1.3 Missing Direct3D features the VSA-100 *can* do (P1-P3)

3dfx's own HAL shipped every one of these on this chip (`reports/vendor.md` and `reports/hw.md`, both
citing the chip spec `avenger_spec.doc` Rev 1.13):

- **The extended combine.** One register, `combineMode` with bit 31 set, gives:
  - 2X/4X output shift;
  - ADDSIGNED, SUBTRACT, ADDSMOOTH and BLEND\*;
  - per-TMU TFACTOR;
  - the COMPLEMENT and ALPHAREPLICATE argument modifiers.

  We advertise 7 texture ops and 3dfx advertised 20. Our HAL writes combineMode = 0 or leaves it alone (G05, G12, G13, G14).
- **Depth and stencil:**
  - Z bias (G22);
  - the W-buffer, where ZENABLE=USEW today turns depth **off** (G23);
  - 8-bit stencil at 32 bpp (G35);
  - destination alpha at 32 bpp (G28).
- **Filtering:**
  - single-pass trilinear (both TMUs on one chain; G26);
  - LOD bias and MAXMIPLEVEL (G27);
  - MIRROR addressing (G33).
- **Texture colour key** (G09). TRANSPARENCY is advertised, but nothing implements it, and the key never reaches the driver
  because the DirectDraw caps do not offer it.
- **Legacy DX5 texture states** (G08): TEXTUREMAPBLEND, TEXTUREMAG/MIN and the rest. These are ignored today, so DX5 titles never
  mipmap.
- **More texture formats** (G34, G93), **specular** (G25), **lines and points as 1-pixel quads**
  (G16; advertised today but dropped), and **flat shading** (G24).
- **Over-promises to remove or honour:**
  - ValidateTextureStageState says yes to everything (G15);
  - D24S8 is listed with no stencil (G35);
  - STATESET is skipped while DP2EX is reported (G40).

### 1.4 Performance: bounded by the transport, not the chip (P1 measurement, then P2)

The UT2003 flyby ran at 640x480x16 with **9.41 fps average and 2.07 minimum**, spending 116.7 of every 123 ms in DrawPrimitive. UT2004
ran 4-6 fps at 1280x960. The causes, from `reports/perf.md`:

- **No command FIFO.** Every register write is an uncached PCI store into chip 0's 31-entry FIFO, with a status read
  before every vertex (G47, G48).
- **Strips and fans are sent as independent triangles** (G49).
- **One pixel per clock.** 2PPC is never programmed; it is worth about 1.7x at 16 bpp (G50).
- **One chip of four.** 3dfx defaulted D3D to 4-chip SLI (G51, effort XL).
- **A full engine idle wait on every flip and every Lock** (G54, G56). Halo's Bink movies run about 10x slow.

**The CPU/HAL split has not been measured** (G46). That measurement comes before any of these.

### 1.5 Impossible on this hardware (no work planned)

- **No hardware T&L or shaders** (G83). The runtime's software vertex processing runs on the Athlon XP.
- **Unsupported texture types and effects** (G84, G85, G86):
  - cube and volume maps;
  - DOT3, EMBM and PREMODULATE;
  - anisotropic filtering;
  - border colour.
- **AGP texturing** (G87). The board sits on PCI-66.
- **Texture shape limits** (G88): power of two only, at most 2048, aspect ratio at most 8:1.
- **D3D's 32x32 stipple** (G90).

### 1.6 Not driver work

- **BF1942** stops at SafeDisc 2.80 before D3D starts (G72).
- **The top-aligned 3D view in JK/MotS** is not a driver defect (G97).
- **Titles with D3D paths that are not on `.124`** (G96).

---

## 2. Plan

### 2.0 Ground rules for every item

- **Behind a Diag switch, default off, read at boot**, as for D3D32 and D3DBigTex. The proven path must stay byte for
  byte the same until a supervised boot shows otherwise. A switch is turned on by default only after
  `render` 40/40, windowed 42/42, the item's own d3dprobe tests and its in-game scene all pass on `.124`.
- **Allocate switch bits and event codes once, centrally, before writing code.** The verdicts found
  the independent designs colliding:
  - eight designs claim `vcr_info` flag 0x8000 or higher;
  - about seven claim display event 513/24-25;
  - G06 and G07 define `Diag\D3DMipChain` differently;
  - G05 and G12 both want to own combineMode.

  **Step 0 of milestone M1 is a one-page allocation table** in `include/vcr_ioctl.h`. Use **one**
  `Diag\D3DSafe` word for stability, **one** `Diag\D3DFeat` word for features, and grow `vcr_info` by a field, as
  G02 and G34 propose, instead of taking more flag bits. Give engine events their own id (514).
- **One owner per register.** `vcr_combine.h` is the only writer of combineMode (G05, then G12 and G14 on
  top). G03's reset sequence and G05's neutral word must agree. A TMU combineMode word with bit 30
  clear is never written unless TMU chromaRange was zeroed first.
- **The repo's test rules apply.** Each fix lands with a native test of its state-to-register translation (G77
  builds the harness), a d3dprobe check, and an entry in `tests/README.md`.
- **The `.124` rules apply.**
  - D3D titles are run only with someone watching; Max Payne only with the user present.
  - No vcrctl, vcrlog or fbshot while a D3D title runs.
  - Mode switches at least 5 s apart, through `vcr_pace.h` or `--pace 5000`.
  - Never `taskkill /f` a fullscreen Glide game.
  - `safe-reboot.py` only.
  - Set `Diag\LogEntries=16384` for instrumented boots; the 1024-entry ring held 0.5 s of Halo.

### 2.1 Milestone M0: measure first (no driver behaviour change)

These items change no driver behaviour. They exist so that each later fix has a before-and-after measurement.

| # | item | gap | effort |
|---|---|---|---|
| M0.1 | Allocation table (bits, events, `vcr_info` field), as in §2.0 | - | S |
| M0.2 | Per-context usage counters and a bigger ring (log-only, default on): which gaps does each title actually hit? | G76 | M |
| M0.3 | **Boot 0 on the deployed driver, no build.** Cold boot. Hexen II GL to its menu, then a clean quit. `vcrctl reg 0x80024` (check cmdFifo0 is off). d3dprobe `modulate`/`flat` checks. This decides whether G03 is P0, and whether arming the existing `Reset3D=1` already helps | G03 | S |
| M0.4 | Free d3dprobe additions: `caps` checks `CheckDeviceType(X8R8G8B8, A8R8G8B8)` plus the A8R8G8B8 zmatch, and `render` gets `--bbfmt a8r8g8b8`. These settle G10's real exposure without a mode switch | G10, G74 | S |
| M0.5 | Max Payne control off `.124`: read thread priority and CPU at its menu on `.240` and `.133` (typeperf). Uses no `.124` time | G04 | S |
| M0.6 | Host-side test harness that executes the state-to-register translation | G77 | L |
| M0.7 | d3dprobe gains mip-fragmentation, DXT/8888 mip, TFACTOR, 2X, ZBIAS and aspect checks; a DX5/DX7 probe follows | G74, G75 | L |

### 2.2 Milestone M1: stability (P0), behind `Diag\D3DSafe`

| # | item | gap | effort | notes from the verdicts |
|---|---|---|---|---|
| M1.1 | **Stale-FIFO guard.** Turn a Glide command FIFO off once its owner is provably gone, reusing the proven `VcrHwResetEngine` | G01 | M | The trigger must be the owner process's exit, **not** the display reset, which an alt-tab also produces. Check the FIFO once per DP2/Clear2 and at all three drawable sites, SETRENDERTARGET included. The cure must also clear cmdFifo1, or drop it from the decision. The producer is Glide's lost-context close (`gsst.c:3258/:3315`). The on-box proof varies the close path (quit, `taskkill /im`, alt-tab) across SoF/Q2, Q3 and a UE1 Glide title |
| M1.2 | **Per-context 3D state reset.** Write the 22-register sequence (combineMode, 2PPC exit, clip 1, chroma, stipple, aaCtrl) at context create | G03 | M | The verdict checked every field against the spec. Gate post-GL D3D steps on cmdFifo0 being off **and** no engine reset in the ring |
| M1.3 | **Visible give-up.** Part A, the record (flushed phase + `EngineLostCount`), lands default-on and log-only. Placement no longer depends on the live `g2d_ok`. ContextCreate never refuses for a lost engine | G02 | M | P1 rather than P0: G01 removes the only recorded producer. The recover and report parts (B, C) go on separate bits and stay off until the D3D8 and DX5-7 runtime reactions are measured on the 86Box bed |
| M1.4 | **Lift `.124`'s Max Payne block** after M1.1 + M1.3 are verified, plus one supervised Max Payne record with the user present | G04 | S | Cut to effort S. The heartbeat and d3dwatch tooling go into G73's sweep tooling |
| M1.5 | Mode-set cache: possible use-after-free of a PDEV pointer cached by D3D contexts | G43 | M | design only, unreviewed |

### 2.3 Milestone M2: the staged titles render correctly (P0/P1), behind `Diag\D3DFeat` and `D3DMipChain`

| # | item | gap | effort | notes |
|---|---|---|---|---|
| M2.1 | **Mip-chain contiguity.** Part a (P0): a census plus a guard that clamps LODMAX to the in-place prefix of a split chain. Part b (P1): whole chains via ATOMICSURFACECREATION (3dfx's method on this chip and OS), or a per-level reservation, chosen from boot A's creation log | G07 + G06 merged | M + M | Bit 0 is **not** register-neutral: it lowers LODMAX by one on every full R5G6B5 chain, so re-run `mip`/`mip2048` explicitly. Judge with G06's confetti metric (at most 5 per mille in fixed regions). Use the deterministic UT2003 `-benchmark` flyby and Halo `-timedemo` in **both** A/B boots. If the census reads 0 scatter splits in all three titles, demote to P2 and reopen G06. Its 2D checks need a complex flip chain (RA2 or Tiberian Sun), not StarCraft |
| M2.2 | **X8R8G8B8 textures** (Halo flares, sky). Also zero-initialise the `tmu_view` structs, and build `g_texfmt` from the shared list | G17 | M | Both lenses rated it P0. Retitle it "X8R8G8B8 bitmaps are never created". Add a counter for a stage 1 dropped because its texture handle is 0 |
| M2.3 | **Texture aspect above 8:1** (the UE2 white sky): split or stretch textures beyond the hardware's 8:1, with a creation WARN (log-only part first) | G18 | M | **no verdict** (the verifier hit the limit); P0 on the designer's evidence from the staged map data |
| M2.4 | **MODULATE2X/4X via OUTSHIFT**: the first slice of the combine engine, in `vcr_combine.h` | G05 | M | P1 per the impact lens. Land it first among the combine items, because it is cheap and exact. Put the stage-1 shift in the FBI. Leave TextureOpCaps at 0x184f (UE2 never reads the caps for 2X); widening the caps is a later P2 bit |
| M2.5 | **ZBIAS** via zaColor and FZ_ZBIAS, plus RasterCaps 0x4000 | G22 | M | UE2, H&D and the UE1 D3D renderers send it |
| M2.6 | **TFACTOR with a texture** (part A) | G14 | M | Pass both stages' ops into the stage-0 compiler, and fold only MODULATE×MODULATE. Part B (stage-1 TFACTOR) moves into G12 |
| M2.7 | **Legacy DX5 texture states** map to stage 0 | G08 | M | Must follow M2.1's guard, which then reaches DX5 titles too |
| M2.8 | **Texture colour key**, including key *delivery* (DDCAPS_COLORKEY, DdSetColorKey) | G09 | M | Step 1, with the bit off: count DdSetColorKey calls in Rainbow Six (expect 0). Confirmed users: Rainbow Six; AvP near-certain after G71 |
| M2.9 | UT2003 blue grass and checkerboard; Halo in-game beyond the cutscenes; render-to-texture | G44, G67, G36 | M-L | investigations, using M0.2's counters |

### 2.4 Milestone M3: the remaining feature parity (P2/P3)

These follow, roughly in this order. Each item is a design under `designs/`; most are **unreviewed**.

- **G12:** the full combine compiler on top of G05 (stage order, stage-1 ops, argument routing).
- **G13:** the COMPLEMENT and ALPHAREPLICATE modifiers.
- **G15:** a truthful ValidateTextureStageState.
- **G16:** lines and points as quads.
- **G24:** flat shading and wireframe.
- **G26, G27:** trilinear, LOD bias and MAXMIPLEVEL.
- **G23:** the W-buffer.
- **G25:** specular.
- **G28:** dest alpha at 32 bpp.
- **G30:** no dithering at 32 bpp; both lenses agree, effort S.
- **G31, G32:** Z-unit table fog and vertex fog through the fog unit.
- **G33:** MIRROR.
- **G34, G93:** more texture formats and 1555 targets.
- **G35:** stencil at 32 bpp.
- **G37, G39, G41, G42:** TMU cache flush, clearing video memory at a mode set, fog-table cache invalidation, and handle-table capacity.
- **G57:** guard band.
- **G61:** DirectDraw caps.
- **G10:** X8R8G8B8 + D16 drawn by allocating the Z at 32-bit. P2 unless the census shows a staged title asking for it. Red Faction asks only if raised to 32 bpp, which G66 deliberately does not do.
- **G11:** P3. The two CRT mode switches on a refused CreateDevice disappear with G10.
- **G20:** P2. Replace the compiled-default flip with a committed per-box Diag profile, installed and checked by `tools/deploy_box.py`.
- **G21:** the default (non-BigTex) path's placement bugs. P3 while G20's profile keeps BigTex armed.

### 2.5 Milestone M4: performance (P2), after M0's G46 split is measured

1. G52 removes logging from the drawing path.
2. G48 removes the per-vertex status read.
3. G53 stops re-sending all state per DP2.
4. G49 adds strip and fan setup mode.
5. G50 turns on 2PPC (16 bpp, single texture).
6. G47 builds a frame-buffer CMDFIFO with packet-3 vertices.
7. G54 and G56 remove the idle waits at flip and Lock.
8. G51 adds 4-chip SLI for fullscreen D3D (XL, last).

Measure each step with G78's vsync-on method (the user's choice: vsync on, stock clock, no AA), using the UT2003 flyby and Halo's timedemo.

### 2.6 Library, config and evidence items (no driver change)

- **G65:** staged configs contradict vsync-on (UT2003/UT2004 `UseVSync=False`).
- **G66:** Red Faction's launcher writes HKLM while the game reads HKCU.
- **G19:** fix it per box in `stage-fleetres.py` (UT2003 FirstRun and the 0x121a device-id policy), **not** library-wide: Epic's auto-tune still serves the nine other boxes.
- **G62, G68, G69, G70, G71:** DOSBox ddraw, Red Faction, Rainbow Six, Shogo and AvP each need their own supervised look.
- **G73, G79:** a repeatable, instrumented in-game sweep with a scene book and capture tooling that works under exclusive mode. This is the evidence base every M2 item is judged by.
- **G80:** documentation drift; the README's D3D sections still describe pre-11-08 behaviour in places.

---

## 3. Decisions for the user

1. **Max Payne.** The plan keeps it blocked on `.124` until M1.1 and M1.3 are verified, then needs one
   attended run with you present.
2. **Supervised D3D boots on `.124`.** M0.3, M1 and M2 each need a few supervised boots, with in-game runs
   and paced mode switches. They are covered by the standing fleet autonomy, but D3D titles have been kept out of
   unattended sweeps since the Max Payne freeze. Say whether you want to be present for them.
3. **The UE2 16 bpp policy (G19).** UT2003/UT2004 deliberately choose 16 bpp on any 3dfx card.
   Forcing 32 bpp is possible per box, through the device-id switch, but it costs fill rate on a transport that already
   manages 4-9 fps. The recommendation is to leave it at 16 until M4.

## 4. Next step

**M0.1 to M0.4.** They cost no driver change and settle three priorities (G03, G10, G07's census).
After that comes **M1.1 (G01)**, the root cause of every silent D3D failure on record.

---

## Appendix: every gap

`final` is this plan's priority. `design` is the designer's own. `verdicts` shows the two adversarial
lenses' priorities, or *not reviewed* where the usage limit stopped the verify stage.
`hardware-limit` and `not-ours` rows carry no priority.

| id | kind | final | design | verdicts (code / impact) | effort | title |
|---|---|---|---|---|---|---|
| [G01](../evidence/d3dplan_1112/designs/G01.md) | fixable | **P0** | P0 | P0 / P0 | M | Glide command FIFO left enabled after a not-idle Glide/OpenGL close: every D3D triangle silently vanishes (clears still work); a D3D surface in the stale FIFO window escalates to a 2D give-up and loss of the HAL |
| [G02](../evidence/d3dplan_1112/designs/G02.md) | fixable | **P1** | P0 | P1 / P2 | M | Engine give-up is silent: DrawPrimitives2 and Clear2 keep answering DD_OK and draw nothing, ContextCreate does not check, and acceleration plus the D3D HAL stay off until the next mode set |
| [G03](../evidence/d3dplan_1112/designs/G03.md) | fixable | **P0** | P0 | P0 / P0 | M | The HAL relies on 3D state Glide leaves behind: combineMode, the 2PPC exit, clip rect 1, chroma, stipple, aaCtrl and (at 16 bpp) stencil are never written; Diag\Reset3D is default off, never ran on silicon, and cannot cover a killed client |
| [G04](../evidence/d3dplan_1112/designs/G04.md) | needs-investigation | **P1** | P0 | P1 / P1 | L | Max Payne freeze (2026-10-04): the game froze, not the box; the cause is not established, no ring was saved, and every D3D title on .124 is 'blk' in readiness rounds because of it |
| [G05](../evidence/d3dplan_1112/designs/G05.md) | fixable | **P1** | P0 | P0 / P1 | M | MODULATE2X/4X drawn as MODULATE on stage 0 and stage 1: the Unreal Engine 2 lightmapped world renders half-bright |
| [G06](../evidence/d3dplan_1112/designs/G06.md) | needs-investigation | **P0** | P0 | P0 / P0 | L | In-game blocky multi-coloured texture noise on terrain and rock meshes (UT2004, UT2003, Halo), on a path no d3dprobe test exercises |
| [G07](../evidence/d3dplan_1112/designs/G07.md) | fixable | **P0** | P0 | P0 / P0 | M | Mip chains: XP sends one CreateSurface per level, but the allocator packs only the levels of one call while the TMU assumes a packed chain; no whole-chain reservation, and multi-level DXT/8888 chains were never probed |
| [G08](../evidence/d3dplan_1112/designs/G08.md) | fixable | **P1** | P1 | P1 / P1 | M | Legacy DX5 texture render states (TEXTUREMAPBLEND, TEXTUREMAG/MIN, TEXTUREADDRESS/U/V, WRAPU/V, MIPMAPLODBIAS, BORDERCOLOR) and D3DTSS_ADDRESS are stored but never applied: DX5/DX6 titles draw point-sampled and MODULATE-only |
| [G09](../evidence/d3dplan_1112/designs/G09.md) | fixable | **P1** | P1 | P1 / P0 | M | Texture colour key: D3DPTEXTURECAPS_TRANSPARENCY is advertised, but COLORKEYENABLE and the texture's source key are never read (old Dark-engine foliage draws as flat key-colour silhouettes) |
| [G10](../evidence/d3dplan_1112/designs/G10.md) | fixable | **P2** | P0 | P1 / P2 | M | X8R8G8B8 + D16: the D3D8 runtime reports the pair as matching, ContextCreate refuses it, and CreateDevice fails with D3DERR_INVALIDCALL (confirmed on silicon) |
| [G11](../evidence/d3dplan_1112/designs/G11.md) | fixable | **P3** | P2 | P2 / P3 | M | A CreateDevice refused at ContextCreate costs two CRT mode switches about 150 ms apart (the runtime sets the mode first, then reverts) |
| [G12](../evidence/d3dplan_1112/designs/G12.md) | fixable | **P2** | P1 | P1 / P2 | L | Texture-stage combine covers a fraction of D3D: two stages are computed in the wrong order, most colour ops and every alpha op but ADD/SELECT are drawn as MODULATE, and stage 1 is dropped when stage 0 has no texture |
| [G13](../evidence/d3dplan_1112/designs/G13.md) | fixable | **P2** | P2 | P2 / P1 | M | Argument modifiers D3DTA_COMPLEMENT and D3DTA_ALPHAREPLICATE are masked off before the mapping |
| [G14](../evidence/d3dplan_1112/designs/G14.md) | fixable | **P1** | P1 | P1 / P1 | M | TFACTOR: paired with a texture it becomes the diffuse colour, and only the FBI's c1 is loaded, so a TMU stage can never use it |
| [G15](../evidence/d3dplan_1112/designs/G15.md) | fixable | **P2** | P1 | P1 / P3 | M | ValidateTextureStageState always answers 1 pass / DD_OK |
| [G16](../evidence/d3dplan_1112/designs/G16.md) | fixable | **P2** | P2 | P2 / P1 | M | Lines and points: LineCaps are a copy of the triangle caps, but every LINE and POINT opcode is parsed and skipped |
| [G17](../evidence/d3dplan_1112/designs/G17.md) | needs-investigation | **P0** | P1 | P0 / P0 | M | Halo: glow and soft sprites drawn as sharp-edged squares |
| [G18](../evidence/d3dplan_1112/designs/G18.md) | needs-investigation | **P0** | P0 | not reviewed | M | Unreal Engine 2: solid-white sky and horizon areas, white polygons and striped frames |
| [G19](../evidence/d3dplan_1112/designs/G19.md) | needs-investigation | **P2** | P1 | P1 / P2 | M | UT2003/UT2004 render at 16 bpp with D16 although the staged ini says Use16bit=False and D3D32 is armed; UT2003 also drops to 640x480 |
| [G20](../evidence/d3dplan_1112/designs/G20.md) | fixable | **P2** | P1 | P2 / P2 | M | D3D32 and D3DBigTex are default OFF: on a fresh install seven titles cannot run, and the docs still call BigTex unproven |
| [G21](../evidence/d3dplan_1112/designs/G21.md) | fixable | **P3** | P2 | P2 / P3 | M | The default texture path (no D3DBigTex) keeps two bugs the BigTex path fixed: DirectDraw's 4-byte placement for single-level and render-target textures, and the 24-bit texBaseAddr (textures above 16 MB sampled 16 MB lower) |
| [G22](../evidence/d3dplan_1112/designs/G22.md) | fixable | **P1** | P1 | P1 / - | M | Z bias (D3DRENDERSTATE_ZBIAS) ignored and not reported: decals and bullet holes z-fight |
| [G23](../evidence/d3dplan_1112/designs/G23.md) | fixable | **P2** | P2 | not reviewed | M | ZENABLE = D3DZB_USEW turns depth testing OFF; the W-buffer is not implemented and WINFO is skipped |
| [G24](../evidence/d3dplan_1112/designs/G24.md) | fixable | **P2** | P2 | not reviewed | M | SHADEMODE FLAT and FILLMODE wireframe/point are ignored, although COLORFLATRGB/ALPHAFLATBLEND are advertised |
| [G25](../evidence/d3dplan_1112/designs/G25.md) | fixable | **P2** | P2 | not reviewed | M | Specular highlights are never drawn: SPECULARENABLE is ignored and the specular colour is used only to carry vertex fog |
| [G26](../evidence/d3dplan_1112/designs/G26.md) | fixable | **P2** | P2 | P2 / - | M | No trilinear filtering: MIPFILTER LINEAR is drawn as nearest-level mipmapping (visible mip bands) |
| [G27](../evidence/d3dplan_1112/designs/G27.md) | fixable | **P2** | P2 | not reviewed | M | MIPMAPLODBIAS, MAXMIPLEVEL, SETTEXLOD and nearest-LOD rounding are ignored |
| [G28](../evidence/d3dplan_1112/designs/G28.md) | fixable | **P3** | P2 | P2 / P3 | M | Alpha channel always blends ONE/ZERO; DESTALPHA factors are unadvertised yet passed to the hardware; SRCALPHASAT is advertised but unverified |
| [G29](../evidence/d3dplan_1112/designs/G29.md) | fixable | **P3** | P3 | not reviewed | S | SRCCOLOR as a source factor and DESTCOLOR as a destination factor are mapped to ONE ('the chip cannot'), but the VSA-100's A_SAMECOLOR does exactly this |
| [G30](../evidence/d3dplan_1112/designs/G30.md) | fixable | **P2** | P2 | P2 / - | S | Dithering is left on for 32 bpp targets, which the VSA-100 spec forbids |
| [G31](../evidence/d3dplan_1112/designs/G31.md) | fixable | **P3** | P3 | not reviewed | S | Table fog is always W-based: a Z-unit FOGSTART/END (1.0 or less) fogs every pixel |
| [G32](../evidence/d3dplan_1112/designs/G32.md) | fixable | **P2** | P2 | not reviewed | M | Vertex fog is approximated through a synthetic 1/W and is off without a specular FVF; the fog unit can take iterated alpha, Z or W directly; FOGGOURAUD is not reported; the semantics of fogMode bits 3+4 are unsettled |
| [G33](../evidence/d3dplan_1112/designs/G33.md) | fixable | **P3** | P3 | not reviewed | S | MIRROR texture addressing falls back to WRAP |
| [G34](../evidence/d3dplan_1112/designs/G34.md) | fixable | **P3** | P3 | not reviewed | M | Texture formats: L8, A8L8, A4L4, R3G3B2, A8R3G3B2 and the X variants (X8R8G8B8, X1R5G5B5, X4R4G4B4) are not offered; A8 cannot be offered through a DX7 DDI |
| [G35](../evidence/d3dplan_1112/designs/G35.md) | fixable | **P3** | P3 | not reviewed | M | Stencil: D24S8 is listed in the Z formats, but its stencil is never written or tested (StencilCaps 0; stencil states and D3DCLEAR_STENCIL dropped) |
| [G36](../evidence/d3dplan_1112/designs/G36.md) | needs-investigation | **P1** | P1 | not reviewed | M | Render-to-texture is untested on silicon: render-target textures keep DirectDraw's placement and pitch, the runtime's PCI-id table grants no X8R8G8B8 render-target texture, and Halo runs with DisableRenderTargets |
| [G37](../evidence/d3dplan_1112/designs/G37.md) | fixable | **P2** | P2 | not reviewed | M | The TMU texture cache is not flushed after a 2D-engine Blt into a texture, or after the 3D engine renders into one |
| [G38](../evidence/d3dplan_1112/designs/G38.md) | needs-investigation | **P3** | P3 | not reviewed | L | Palettized textures (P8/A8P8) are not offered, SETPALETTE/UPDATEPALETTE are skipped, and P8 has never been shown to draw correctly on this silicon |
| [G39](../evidence/d3dplan_1112/designs/G39.md) | fixable | **P1** | P1 | not reviewed | S | A mode set never clears video memory: the old 32 bpp desktop shows as garbage at the new depth until a title's first frame, or for its whole session if it stalls |
| [G40](../evidence/d3dplan_1112/designs/G40.md) | needs-investigation | **P3** | P3 | not reviewed | S | STATESET is skipped while DRAWPRIMITIVES2EX is reported: DX7-interface state blocks would be applied at record time and never replayed |
| [G41](../evidence/d3dplan_1112/designs/G41.md) | fixable | **P3** | P3 | not reviewed | S | The fog-table cache is never invalidated after a Glide session or a display reset |
| [G42](../evidence/d3dplan_1112/designs/G42.md) | fixable | **P2** | P2 | not reviewed | M | The 4096-entry surface handle table names every mip level and can fill; lookups degrade near capacity |
| [G43](../evidence/d3dplan_1112/designs/G43.md) | needs-investigation | **P1** | P1 | not reviewed | M | D3D contexts cache a PDEV pointer that a mode change frees (possible use-after-free) |
| [G44](../evidence/d3dplan_1112/designs/G44.md) | needs-investigation | **P1** | P1 | not reviewed | M | UT2003: the grass decoration layer renders blue, and a grey checkerboard patch appears |
| [G45](../evidence/d3dplan_1112/designs/G45.md) | needs-investigation | **P3** | P3 | not reviewed | S | UT2004 menu: a one-pixel bright line on the bottom row of the screen |
| [G46](../evidence/d3dplan_1112/designs/G46.md) | needs-investigation | **P1** | P1 | not reviewed | M | In-game D3D performance is very low, and its CPU/HAL split is unmeasured: UT2003 flyby 9.41 fps at 640x480x16, with 116.7 ms of a 123 ms frame inside DrawPrimitive |
| [G47](../evidence/d3dplan_1112/designs/G47.md) | fixable | **P2** | P2 | not reviewed | L | No command FIFO: every 3D register write is an uncached PIO store into chip 0's 31-entry PCI FIFO, so the CPU and the chip run one after the other, under the display lock |
| [G48](../evidence/d3dplan_1112/designs/G48.md) | fixable | **P2** | P2 | not reviewed | M | A status-register read before every vertex, plus 27/36/45 uncached writes per untextured/1-texture/2-texture triangle (about 5-8 us each, inferred) |
| [G49](../evidence/d3dplan_1112/designs/G49.md) | fixable | **P2** | P2 | not reviewed | M | Strips, fans, indexed primitives and immediate fans are sent as independent triangles, with three full vertices each |
| [G50](../evidence/d3dplan_1112/designs/G50.md) | fixable | **P2** | P2 | not reviewed | M | Two pixels per clock, its band height and the column band are never programmed: D3D fills at about 1 textured pixel per clock per chip |
| [G51](../evidence/d3dplan_1112/designs/G51.md) | fixable | **P2** | P2 | not reviewed | XL | Direct3D renders on one VSA-100 of four (no SLI), while 3dfx's D3D defaulted to 4-chip SLI and our kernel driver already runs SLI for Glide |
| [G52](../evidence/d3dplan_1112/designs/G52.md) | fixable | **P2** | P2 | not reviewed | S | Logging on the drawing path: every VcrDd() call formats a message and makes an IOCTL whatever the level, and every texture bind logs a DEBUG event |
| [G53](../evidence/d3dplan_1112/designs/G53.md) | fixable | **P2** | P2 | not reviewed | M | The full state and render target are re-sent at the first triangle of every DrawPrimitives2 call and on every change; no registers are shadowed |
| [G54](../evidence/d3dplan_1112/designs/G54.md) | fixable | **P2** | P2 | not reviewed | L | Every flip makes the CPU wait for the whole engine to go idle, then writes vidDesktopStartAddr by IOCTL; there is no hardware swap queue (and a future one must cap pending swaps at 6) |
| [G55](../evidence/d3dplan_1112/designs/G55.md) | needs-investigation | **P1** | P1 | not reviewed | M | Render-after-flip hazard (unverified): DP2 and Clear2 draw at once into the buffer a pending vsync'd flip is still scanning out |
| [G56](../evidence/d3dplan_1112/designs/G56.md) | fixable | **P2** | P2 | not reviewed | M | Every DirectDraw Lock and texture upload drains the engine; Halo's Bink movies run about 10x slow (masked by -novideo) |
| [G57](../evidence/d3dplan_1112/designs/G57.md) | fixable | **P2** | P2 | not reviewed | M | No guard band is reported, so the D3D runtime clips every screen-edge triangle in software |
| [G58](../evidence/d3dplan_1112/designs/G58.md) | fixable | **P2** | P2 | not reviewed | M | Vertex colour goes as four floats to sRed..sAlpha, which reach the FBI only: TMU stages cannot see diffuse, and every vertex carries 3-4 extra writes |
| [G59](../evidence/d3dplan_1112/designs/G59.md) | needs-investigation | **P3** | P3 | not reviewed | S | 16 bpp DirectDraw flips on .124 measured at about half the refresh rate (42-47 flips/s at 85 Hz); unexplained |
| [G60](../evidence/d3dplan_1112/designs/G60.md) | fixable | **P3** | P3 | not reviewed | M | Small CPU-side performance trims: per-call FPU save and setup, IOCTLs for vblank polls, and Glide's skip-transparent-pixels alpha test |
| [G61](../evidence/d3dplan_1112/designs/G61.md) | fixable | **P1** | P1 | not reviewed | L | DirectDraw caps under-report what the 2D engine does: no colour-key or stretch caps, DEPTHFILL refused, no overlay |
| [G62](../evidence/d3dplan_1112/designs/G62.md) | needs-investigation | **P1** | P1 | not reviewed | M | DOSBox output=ddraw (Descent 1/2) shows a black 640x400: one SDL DirectDraw setup call fails; overlay output falls back for lack of overlay caps |
| [G63](../evidence/d3dplan_1112/designs/G63.md) | needs-investigation | **P2** | P2 | not reviewed | M | The DirectDraw heap starts at offset 0, which HeapVidMemAllocAligned reports as failure; the cure (Diag\DdHeapFloor) has never run on silicon, while every BigTex texture now goes through that allocator |
| [G64](../evidence/d3dplan_1112/designs/G64.md) | needs-investigation | **P3** | P3 | not reviewed | S | dwVidMemFree is a constant equal to the total: engines that size their texture cache from the driver's free-memory report may misjudge it |
| [G65](../evidence/d3dplan_1112/designs/G65.md) | fixable | **P1** | P1 | not reviewed | M | Staged game configs contradict the campaign's vsync-on and no-AA choices (UT2003/UT2004 UseVSync=False at 60 Hz, Red Faction Vsync=0, Thief II 8x MSAA) |
| [G66](../evidence/d3dplan_1112/designs/G66.md) | fixable | **P1** | P1 | not reviewed | M | Staged resolution not applied for several D3D titles: Red Faction's launcher writes HKLM while the game reads HKCU; System Shock 2 and Thief Gold stage no resolution and run at 640x480 |
| [G67](../evidence/d3dplan_1112/designs/G67.md) | needs-investigation | **P1** | P1 | not reviewed | L | Halo: only the menu and today's timedemo cutscenes have been seen, behind three workarounds (DisableRenderTargets, -novideo, an SSE2-floor override); player-controlled gameplay never checked |
| [G68](../evidence/d3dplan_1112/designs/G68.md) | needs-investigation | **P1** | P1 | not reviewed | M | Red Faction: the 09-29 black frame was never explained, and gameplay was never seen; in-game testing needs its disc mounted |
| [G69](../evidence/d3dplan_1112/designs/G69.md) | needs-investigation | **P1** | P1 | not reviewed | M | Rainbow Six: its 16-over-32 Z fix rests on one 320x240 strip; the deployed build 95678704 has never had a full sweep, and the title has never been checked in game |
| [G70](../evidence/d3dplan_1112/designs/G70.md) | needs-investigation | **P1** | P1 | not reviewed | M | Shogo: the main menu is not clearly evidenced on vcr-kmd (near-black 60 s frames, a black backdrop mesh) |
| [G71](../evidence/d3dplan_1112/designs/G71.md) | needs-investigation | **P1** | P1 | not reviewed | M | Aliens vs Predator has never reached a D3D device on .124: the 11-12 library fix was proven on seven other boxes |
| G72 | not-ours | **-** | - | not reviewed | - | Battlefield 1942 single player is blocked by SafeDisc 2.80 before Direct3D starts |
| [G73](../evidence/d3dplan_1112/designs/G73.md) | needs-investigation | **P1** | P1 | not reviewed | L | No D3D title has been checked in game on the current build: in-game frames exist only for JK, MotS and Thief II from 09-28 (before the fixes), and the compat DB's .124 rows are from the GeForce2 era |
| [G74](../evidence/d3dplan_1112/designs/G74.md) | fixable | **P1** | P1 | not reviewed | L | d3dprobe misses most of the gaps: large parts of the HAL have no check, its readback window is 256x256, and its caps mode does not report several values this plan depends on |
| [G75](../evidence/d3dplan_1112/designs/G75.md) | fixable | **P1** | P1 | not reviewed | L | No DX5/DX7 probe: legacy render states, execute buffers, texture colour keys and DX7 state blocks have no lab test (D3D8 cannot send them) |
| [G76](../evidence/d3dplan_1112/designs/G76.md) | fixable | **P1** | P1 | not reviewed | M | No per-context usage counters for the gaps, and the flight-recorder ring wraps within seconds of a game starting |
| [G77](../evidence/d3dplan_1112/designs/G77.md) | fixable | **P1** | P1 | not reviewed | L | No host test executes the state-to-register translation |
| [G78](../evidence/d3dplan_1112/designs/G78.md) | fixable | **P1** | P1 | not reviewed | L | D3D benchmark tooling and a vsync-on measurement method are missing or incomplete |
| [G79](../evidence/d3dplan_1112/designs/G79.md) | fixable | **P1** | P1 | not reviewed | L | Capture and sweep-verdict tooling under D3D exclusive mode: the default fbshot blocks, --probe is slow, GDI frames can be the back buffer, and the luma verdict passes garbage |
| [G80](../evidence/d3dplan_1112/designs/G80.md) | fixable | **P1** | P1 | not reviewed | M | Documentation drift, undocumented kill switches, and verification recorded only as prose |
| [G81](../evidence/d3dplan_1112/designs/G81.md) | needs-investigation | **P2** | P2 | not reviewed | S | Half-pixel centre: the pixel half is fixed and holds on silicon; the texel-centre half and Max Payne's edge are unverified |
| [G82](../evidence/d3dplan_1112/designs/G82.md) | needs-investigation | **P2** | P2 | not reviewed | M | Glide/OpenGL titles keep dormant Direct3D renderers that have never been exercised on .124 |
| G83 | hardware-limit | **-** | - | not reviewed | - | No hardware T&L or shaders: every D3D8/9 title uses software vertex processing on the Athlon XP |
| G84 | hardware-limit | **-** | - | not reviewed | - | Cube maps, volume textures, DOT3, BUMPENVMAP(LUMINANCE) and PREMODULATE are impossible on the VSA-100 |
| G85 | hardware-limit | **-** | - | not reviewed | - | Anisotropic filtering is impossible: one bilinear footprint per TMU per pixel |
| G86 | hardware-limit | **-** | - | not reviewed | - | Border-colour addressing and cylindrical wrap (WRAP0-7) have no hardware; BORDER currently falls through to WRAP |
| G87 | hardware-limit | **-** | - | not reviewed | - | AGP (non-local) texturing is impossible: the texture base is a local 26-bit address, and the 6000's chips sit on PCI-66 behind a bridge |
| G88 | hardware-limit | **-** | - | not reviewed | - | Texture size limits: 2048x2048 maximum (TBIG), aspect ratio up to 8:1, power of two only, base 16-byte aligned |
| [G89](../evidence/d3dplan_1112/designs/G89.md) | needs-investigation | **P2** | P2 | not reviewed | M | Texture repeat range is bounded by the 14.18 S/T iterators (about +/-32 repeats at our 256-texel scaling), and dwMaxTextureRepeat is reported as 0 |
| G90 | hardware-limit | **-** | - | not reviewed | - | Stipple: only a 4x8 pattern or a rotate mode; D3D's 32x32 STIPPLEPATTERN and STIPPLEDALPHA cannot be expressed exactly |
| [G91](../evidence/d3dplan_1112/designs/G91.md) | needs-investigation | **P3** | P3 | not reviewed | S | BLENDOP (subtract/reverse subtract) and per-channel COLORWRITEENABLE exist in silicon, but it is unknown whether a DX7-DDI driver can expose them; MIN/MAX blend ops are impossible |
| [G92](../evidence/d3dplan_1112/designs/G92.md) | needs-investigation | **P2** | P2 | not reviewed | M | Projected texture coordinates (TEXTURETRANSFORMFLAGS PROJECTED) are dropped; per-TMU 1/W may make them possible, but this is unverified |
| [G93](../evidence/d3dplan_1112/designs/G93.md) | fixable | **P3** | P3 | not reviewed | M | 1555 render targets (A1R5G5B5/X1R5G5B5) are native, but not offered |
| [G94](../evidence/d3dplan_1112/designs/G94.md) | fixable | **P2** | P2 | not reviewed | M | DDI level: vcrdd is a DX7 (DP2EX, non-T&L) driver to D3D8/9, which works; a DX8-level DDI would add A8 textures, X8R8G8B8 render-target textures and Caps2 at a high cost |
| G95 | not-ours | **-** | - | not reviewed | - | Reference-source caveats: vendor HAL bugs, guesses and per-app workarounds not to copy, and stale passages in 3dfx's own spec |
| G96 | not-ours | **-** | - | not reviewed | - | Library titles not on .124 that have D3D paths: Wing Commander Prophecy, Rogue Squadron and Die by the Sword are Win9x-only |
| G97 | not-ours | **-** | - | not reviewed | - | Not a driver defect: Jedi Knight's and MotS's 3D view sits top-aligned |
