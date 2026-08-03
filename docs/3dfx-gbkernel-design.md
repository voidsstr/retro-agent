# gbkernel — kernel-mode Glide backend for fxD3D (M4b design)

Implementation design for the **kernel-mode `gb_*` backend** of the clean-room
fxD3D display driver (`fxd3ddd.dll`): drive the Voodoo3 (Avenger) hardware
directly via driver-mapped MMIO — no `grSstWinOpen`, no user-mode Glide DLL.
Derived 2026-07-24 exclusively from the **open 3dfx-GPL Glide3 h3 tree**
(**H3** = `voodoo-cleanroom/build/retro3dfx-glide/glide3x/h3`; the vendored
`scripts/3dfx/build/glide/` is the same sezero layout), our own code, and public
register knowledge. The proprietary H5 tree was not opened (clean-room).

The seam to implement is `scripts/3dfx/d3dhal/glidebackend.h`: state calls
(`gb_set_*`), textures (`gb_tex_*`), geometry (`gb_vtx_t`/`gb_draw_*`),
lifecycle (`gb_startup/gb_open/gb_clear/gb_swap/gb_finish`). The fxD3D core
links only the state/tex/draw subset; lifecycle is driven by the chassis
(`DrvEnableSurface`/`DrvEnableDirectDraw`/`d3d_ContextCreate`).

---

## 0. Hardware map (3 headers contain everything)

BAR0 = 32 MB register aperture, BAR1 = 32 MB linear framebuffer
(`voodoo-cleanroom/vcr-disp/disp_hw.c:12-17,34-35`; `H3/minihwc/minihwc.c:1285-1296`).
BAR0 sub-spaces (`H3/incsrc/h3defs.h:1273-1288`):

| offset | block |
|---|---|
| `0x000000` | `SST_IO_OFFSET` — `SstIORegs`: status, dramInit0/1, miscInit0, lfbMemoryConfig, pllCtrl, vidProcCfg, vidDesktopStartAddr… (`h3regs.h:…-115`) |
| `0x080000` | `SST_CMDAGP_OFFSET` — `SstCRegs`; `cmdFifo0` at +0x20: baseAddrL, baseSize, bump, readPtrL, readPtrH, aMin, aMax, depth, holeCount (`h3regs.h:117-160`) |
| `0x100000` | `SST_2D_OFFSET` — WAX 2D `SstGRegs`: dstBaseAddr, dstFormat, srcBaseAddr, srcFormat, srcXY, dstSize, dstXY, command… (`h3regs.h:163-199`) |
| `0x200000` | `SST_3D_OFFSET` — `SstRegs` (`h3regs.h:213-375`) |
| `0x600000` | `SST_TEX_OFFSET` (not needed — see §4) |
| `0x1000000` | `SST_LFB_OFFSET` (3D LFB space in BAR0) |

3D register byte offsets (from `SstRegs`, `h3regs.h:213-375`): `status 0x000`,
`FtriangleCMD 0x100`, `fbzColorPath 0x104`, `fogMode 0x108`, `alphaMode 0x10C`,
`fbzMode 0x110`, `lfbMode 0x114`, `clipLeftRight/BottomTop 0x118/0x11C`,
`nopCMD 0x120`, `fastfillCMD 0x124`, `swapbufferCMD 0x128`, `fogColor 0x12C`,
`zaColor 0x130`, `chromaKey 0x134`, `chromaRange 0x138`, `stipple 0x140`,
`c0/c1 0x144/0x148`, `fogTable[32] 0x160-0x1DC`, `colBufferAddr 0x1EC`,
`colBufferStride 0x1F0`, `auxBufferAddr 0x1F4`, `auxBufferStride 0x1F8`,
`swapBufferPend 0x24C`, `leftOverlayBuf 0x250`, TSU `sSetupMode 0x260 …
sBeginTriCMD 0x2A4`, TMU `textureMode 0x300`, `tLOD 0x304`, `texBaseAddr 0x30C`,
`trexInit0/1 0x31C/0x320`, `nccTable0/1 0x324-0x380`.

**Vendor these three headers verbatim** into the driver: `H3/incsrc/h3regs.h`,
`h3defs.h`, `h3gdefs.h` (self-contained, GPL — our license base, `FORKS.md`).

---

## 1. Init — replicate vs skip

`grSstWinOpen` sequence (`H3/glide3/src/gsst.c:941-1562`): map board →
`hwcInitRegisters` → `hwcAllocBuffers` → `hwcInitVideo` → `hwcInitFifo` → FIFO
software state → initial register writes → `assertDefaultState()` → clear.

**SKIP (miniport/BIOS owns it, or user-mode-only):** mode set
(`setVideoMode`, `minihwc.c:3279-3286`); video-overlay scan-out
(`hwcInitVideoOverlaySurface`, `minihwc.c:3324-3515`) — we keep GDI's desktop
scan-out and present by blit (§5); PLL/SGRAM/VGA init (`cinit`:
`h3InitPlls h3cinit.c:667`, `h3InitSgram :434`, `h3InitVga :710`) — BIOS +
miniport did it (`hwcInitRegisters` skips it on Win32 via the `HWC_EXT_INIT`
path, `minihwc.c:1357-1387`); `hwcMapBoard`'s ExtEscape (`minihwc.c:1154-1208`)
— the driver owns mappings; memory sizing `h3InitGetMemSize` — take VRAM size
from the miniport, read `dramInit1` only for SDRAM-vs-SGRAM
(`minihwc.c:1302-1310`, picks the fastfill flavor §5); windowed WinFifo + AGP
FIFO machinery.

**MUST replicate (all plain MMIO writes):**

a) **Buffer carve-out** — `hwcAllocBuffers` (`minihwc.c:1397-1610`): glide's
fullscreen layout = tram (textures) at 0 (`:1520`), CMDFIFO immediately below
the first color buffer (`fifoStart = fbOffset - fifoSize`,
`fifoLength -= 0x2000`, `:1521-1541`; `MAXFIFOSIZE 0x40000`, `0xff000` for
>8 MB boards, `:628-629`), color buffers top-down (even-4K-page aligned,
`:1466-1492`), aux/depth at top (odd-page aligned, `:1449-1464`). Stride/size:
`calcBufferStride/calcBufferSize` (`:3678-3745`); LFB addr for linear buffers ==
physical offset (`hwcBufferLfbAddr`, `:3766-3847,3842-3843`).
**Our variant:** GDI desktop occupies the bottom of BAR1 → shift the layout up:
`[0, desktopEnd)` GDI → tram → FIFO → back buffer(s) → aux at top; same
alignment; minimum-memory check as `gsst.c:1043-1048`.
**Use all-LINEAR buffers** (skip `lfbMemoryConfig` tile mapping + tile math;
linear path is fully supported: `gsst.c:1520-1523`, `gglide.c:918-921`).

b) **Y-origin**: `miscInit0 = (miscInit0 & ~SST_YORIGIN_TOP) |
((yRes-1) << SST_YORIGIN_TOP_SHIFT)` (`minihwc.c:3528-3534`).

c) **CMDFIFO enable** — `hwcInitFifo` (`minihwc.c:1613-1665`), 9 writes to
`SstCRegs`: `cmdFifo0.baseSize=0`; `baseAddrL=fifoStart>>12`;
`readPtrL=fifoStart`; `readPtrH=0`; `aMin=aMax=fifoStart-4`; `depth=0`;
`holeCount=0`; `cmdFifoThresh=(0xF<<SST_HIGHWATER_SHIFT)|0x8` (Avenger); then
`baseSize = ((len>>12)-1) | SST_EN_CMDFIFO` with hole-counting ON
(`h3gdefs.h:177-179`).

d) **Initial register state** (`gsst.c:1499-1541`): shadow
`fbzMode = SST_ENRECTCLIP|SST_ENZBIAS`; `leftOverlayBuf`, `swapbufferCMD=0`,
then `colBufferAddr/colBufferStride/auxBufferAddr/auxBufferStride`; then the
`assertDefaultState()` list (`gsst.c:602-672`) replayed as gbkernel defaults;
then clear (`clearBuffers`, `gsst.c:676-693`). Plus per-TMU shadow +
texture-flush packet init (`gsst.c:855-894`, §4).

e) **FIFO software state** (`gsst.c:1402-1451`): write ptr = BAR1 VA +
fifoOffset; `roomToEnd = fifoSize - FIFO_END_ADJUST(32)` (`fxcmd.h:189`);
`fifoRoom = roomToReadPtr = roomToEnd - 4`; precomputed wrap packet
`fifoJmpHdr = SSTCP_PKT0_JMP_LOCAL | (fifoOffset << (SSTCP_PKT0_ADDR_SHIFT-2))`
(`gsst.c:1446-1448`; pkt0 encoding `h3gdefs.h:197-205`).

---

## 2. Triangle path — CMDFIFO Packet-3 (the only path h3 uses)

h3 builds with `GLIDE_HW_TRI_SETUP=1 GLIDE_PACKET3_TRI_SETUP=1 USE_PACKET_FIFO=1`
(`H3/glide3/src/Makefile.mingw:105`); `grDrawTriangle` → `internal_trisetup`
(`gxdraw.c:258-351`): optional state flush, optional software cull
(`_grTriCull`, `gxdraw.c:198-255`), one PKT3 into the FIFO. No PIO path exists
in this build — CMDFIFO is the minimal correct path (init is only §1c).

**Transport (kernel-critical):** packets are 32-bit stores into the FIFO ring in
framebuffer memory via BAR1; hole-counting hardware detects the writes — no
doorbell (autoBump, `gsst.c:1216`). Space/wrap protocol (`fifo.c:838-1056`):
- before writing N bytes, if `roomToReadPtr < N` poll `cmdFifo0.readPtrL`
  (read **twice until stable**: `_grHwFifoPtr`, `fifo.c:1068-1090`);
- if `roomToEnd <= N`, write the JMP-to-start packet and wrap
  (`fifo.c:960-1013`) — hardware never auto-wraps (`gsst.c:1406-1415`);
- never write onto the read pointer (−4 margin).

**One gouraud triangle = one packet** (hdr per `fxcmd.h:898-917`; fields
`h3gdefs.h:184-237`; `SSTCP_PKT_SIZE=3`, PMASK shift 10, SMODE shift 22):
```
hdr = (smode << 22) | (pmask << 10) | (nVerts=3 << 6) | SSTCP_PKT3_BDDBDD | 3
```
- `pmask` (`h3defs.h:501-508`): `SST_SETUP_RGB(1)|SST_SETUP_A(2)|SST_SETUP_Z(4)|
  SST_SETUP_Wfbi(8)`; textured adds `SST_SETUP_ST0(0x20)`.
- `smode` cull bits = `kSetupCullEnable(0x02)|kSetupCullNegative(0x04)|
  kSetupPingPongDisable(0x08)` (`fxcmd.h:544-551`; `_grUpdateTriPacketHdr`,
  `gglide.c:2716-2755`). **Use hardware culling** — removes all FP math from the
  kernel draw path.
- Per vertex, raw IEEE floats, fixed order (mirrors `glidebackend.c:313-326`):
  `x, y, r, g, b, a` (colors 0-255 as floats), `ooz`, `oow(=1/w)`, textured adds
  `s·oow·scale, t·oow·scale`. Window coords, |x|<2048 (`gxdraw.c:296-303`).
- Untextured vertex = 8 floats (32 B); textured = 10 (40 B). `gb_draw_tris` may
  batch ≤5 triangles per PKT3 (nVerts≤15, `fxcmd.h:882`) or 1/packet for
  simplicity. Always `Z + Wfbi`, never `W0` (pruning logic
  `_grUpdateParamIndex`, `gglide.c:2504-2536`).

**State flush:** lazy shadow + dirty-flush as one **PKT4** group:
`hdr = (mask14 << 15) | (regOffset<<1 | chipField<<11) | 4` + masked values,
base `fbzColorPath` (`_grFlushCommonStateRegs`, `gglide.c:2200-2246`; PKT4
`fxcmd.h:573-608`; broadcast chip field, `kChipFieldShift=11`, `fxcmd.h:467`).
Single-register = PKT1: `hdr=(1<<16)|(regOffset<<1)|1` + value
(`fxcmd.h:737-756`).

---

## 3. State registers — gb_* → lifted glide computations (`gbkstate.c`)

Lift these bodies verbatim (pure bit-math on shadow registers):

| gb call | glide function(s) | register |
|---|---|---|
| `gb_set_depth` | `_grDepthBufferMode` `gglide.c:1823-1874`; `_grDepthBufferFunction` `:1802-1817` (SST_ZFUNC [7:5]); write-mask via `distate.c:967-1003` (SST_ZAWRMASK BIT10 / SST_RGBWRMASK BIT9) | `fbzMode` |
| `gb_set_blend` | `_grAlphaBlendFunction` `gglide.c:472-514` | `alphaMode` |
| `gb_set_alphatest` | `_grAlphaTestFunction` `:662-682`; `_grAlphaTestReferenceValue` `:688-707` | `alphaMode` |
| `gb_set_fog` | `_grFogMode` `:2012-2075` (+`SST_FOG_DITHER|SST_FOG_ZONES`); `_grFogColorValue` `:1673-1689`; `grFogTable` `:2080-2121` (64 entries + deltas, 2/word, delta=(e1−e0)<<2); generator `guFogGenerateLinear` (open `gu.c`) | `fogMode`,`fogColor`,`fogTable` |
| `gb_set_cull` | `grCullMode` `:1783-1796` + `_grUpdateTriPacketHdr` `:2716-2755` — **PKT3 header smode bits, not a register** | PKT3 hdr |
| `gb_set_dither` | `_grDitherMode` `:1965-2006` (h3 forces 2x2) | `fbzMode` |
| `gb_set_chromakey` | `_grChromakeyMode` `:1392-1407`; `_grChromakeyValue` `:1735-1745` | `fbzMode`,`chromaKey` |
| `gb_set_shade` + tex bind combines | `_grColorCombine` `:1519-1640`; `_grAlphaCombine` `:520-624` (SST_ENTEXTUREMAP resolution `:617-619`). **On any SST_ENTEXTUREMAP transition emit `nopCMD=0`** (`distate.c:910-915`) | `fbzColorPath` |
| `gb_set_texfactor` | `_grConstantColorValue` `:1753-1775` | `c0`,`c1` |
| `gb_set_texfilter` | `grTexFilterMode` `gtex.c:970`; `grTexClampMode` `gtex.c:636` | `textureMode` |

Internal color format `GR_COLORFORMAT_ARGB` (drop `_grSwizzleColor`). Defaults
at open: the `assertDefaultState` list (`gsst.c:602-672`). Triangle headers
collapse to two precomputed values (textured / untextured).

---

## 4. Texture path (`gbktex.c`)

TMU memory on h3 is **unified board memory** — the tram region carved in §1a
(`gsst.c:1194-1206`). No texture aperture:

- **Alloc:** bump allocator over `[tramOffset, tramOffset+tramSize)`, 16-byte
  aligned (`SST_TEXTURE_ADDRESS = MASK(20)<<4`, `h3defs.h:561`).
- **Upload:** straight into board memory at the physical offset ("directly to
  the 2d lfb space", `gtexdl.c:1321-1329`) via FIFO **PKT5 linear-write,
  space=LFB**: `hdr1=(0<<30)|byteEn|(nWords<<3)|5; hdr2=destByteAddr&0x1FFFFFF`
  + texel words (`FIFO_LINEAR_WRITE_BEGIN` `fxcmd.h:1046-1079`; PKT5
  `h3gdefs.h:245-257`; used `gtexdl.c:1054-1129`).
- **Avenger download-coherency errata (MANDATORY):** wrap every download —
  pre: 2D `command=SSTG_NOP|SSTG_GO` (PKT1-WAX); post: `texBaseAddr=~base`,
  3D `nopCMD=0`, `texBaseAddr=base`, 2D NOP again (`GR_TEX_FLUSH_PRE/POST`,
  `fxglide.h:1105-1121`; built at `gsst.c:871-894`; rationale
  `gtexdl.c:1237-1244,1355-1368`).
- **Bind:** lift `grTexSource` (`gtex.c:1250-1369`): `texBaseAddr`,
  `textureMode |= SST_TPERSP_ST|SST_TCLAMPW|(fmt<<SST_TFORMAT_SHIFT)`
  (`:1331-1347`); `tLOD` min=max, aspect (`:1352-1369`). Write TMU group as PKT4
  with chip-field 0x2 (`gglide.c:2287-2289`).
- Formats: 565/1555/4444 now; **defer P8** (needs nccTable palette download,
  `gtexdl.c:340-583`) — convert P8→565 host-side for bring-up.
- ST scale: keep the user-mode backend's validated convention
  (`glidebackend.c:295,322-326`; glide default scale 256, `gsst.c:899-900`).

---

## 5. Clear + present

**`gb_clear` = FASTFILL** (`grBufferClear`, `gglide.c:793-966`). SGRAM boards
(detect `dramInit1 & SST_MCTL_TYPE_SDRAM`==0): 2 packets (`:836-852`) —
PKT4 `zaColor`,`c1=color`; PKT4 `fastfillCMD=1`; restore. Fill honors the clip
rect (`SST_ENRECTCLIP` + clip regs) and `fbzMode` write masks. SDRAM fallback
(`:853-962`) only if such a board appears.

**`gb_swap` — ship (A) blit-present:** the open DRI swap
(`grDRIBufferSwap`, `gglide.c:1216-1257`): PKT4-WAX `srcBaseAddr=backBuffer`,
`srcFormat=stride|(3<<16)`, `dstBaseAddr=desktopOffset`,
`dstFormat=desktopStride|(3<<16)`; per rect `srcXY`, `dstSize=(h<<16)|w`,
`dstXY`, `command=(0xCC<<24)|0x1|BIT(8)` (SRCCOPY screen-to-screen, initiate).
GDI survives; works windowed + fullscreen; no vsync at bring-up.
**(B) flip-present later:** glide fullscreen swap = `leftOverlayBuf`,
`swapbufferCMD=interval_hw` (`gglide.c:1068-1071,1111-1126`); our flip
equivalent is `vidDesktopStartAddr` (`h3regs.h:108`) via DdFlip.
**Throttle:** count in-flight swaps via FIFO positions; poll `readPtrL`
(twice-stable) + `cmdFifo0.depth==0` (`_grBufferNumPending`,
`gglide.c:1304-1386`).

**`gb_finish`** = `grFinish` (`gsst.c:1914-1940`): `nopCMD=0`, poll
`status & SST_BUSY` (BIT9, `h3defs.h:104`) with a kernel timeout (§6).

---

## 6. Kernel constraints, file plan, bring-up ladder, risks

**Liftable as-is:** all §3 state math; PKT1/3/4/5 builders (~200 lines of clean
helpers); buffer-layout math (`minihwc.c:1397-1610,3678-3847`); FIFO init
(`:1613-1665`); FIFO space/wrap (`fifo.c:838-1056`); `grTexSource`/download
math; the 3 register headers.
**Rewrite/drop:** TLS+GrGC → one static device context; ExtEscape/HWCEXT/
CreateWindow/setVideoMode/DDraw → gone; getenv/sprintf/GDBG/MessageBox → gone
(EngDebugPrint no-op on free XP — use the debug escape); `\Device\PhysicalMemory`
mapping → not needed; asm paths → C only.

**MMIO plumbing gap:** `chassis.c` maps only the framebuffer today
(`chassis.c:321-333`); no miniport in-tree. The paired miniport must (a) expose
BAR0 via `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES`, (b) report full 32 MB BAR1 +
VRAM size + desktop offset/stride so gbkernel lays out memory above the GDI
surface. Single entry (authoritative form, see gbkernel.h + the M4c-1 status
below): `gbkernel_attach(bar0va, bar1va, vramBytes, desktopEnd, desktopStride,
w, h, depthBytes)` called from `DrvEnableSurface` (detach in DrvDisableSurface).

**File plan (~1.8–2.4 k LOC + 3 vendored headers):**
```
driver/nt/hw/h3regs.h,h3defs.h,h3gdefs.h  vendored verbatim (GPL)
driver/nt/gbkernel.c  (~900-1200) context/attach/layout/FIFO/packets/clear/swap/finish/draw
driver/nt/gbkstate.c  (~500-700)  shadow regs + lifted state math, PKT4 dirty-flush
driver/nt/gbktex.c    (~300-400)  tram allocator, PKT5 download + errata, bind
driver/nt/gbkdebug.c  (~100)      escape-driven status dump (FIFO ptrs, status)
SOURCES: replace gbstub.c (keep it for no-hw builds)
```
Pure-logic parts get host tests (`tests/native/test_gbkernel_*.c`) per the
regression-test rule.

**Bring-up ladder** (each rung behind a `DrvEscape` so the agent drives it
before D3D does): 1) attach+probe (read `status`, read back `cmdFifo0` after
init); 2) `gb_clear` FASTFILL to an offscreen buffer, CPU-verify via BAR1, then
a visible rect; 3) flat → gouraud triangle (PKT3 pmask 0x0F), then depth + the
state matrix; 4) PKT5 texture download + textured quad; then the real
DrawPrimitives2 path.

**Top risks & mitigations:**
1. **Kernel hang on FIFO stall** — every poll loop capped (~10⁷ iters) then:
   dump state via debug escape, disable FIFO (`baseSize=0`, as
   `hwcRestoreVideo`), fail the DDI call. Keep packet-size asserts in checked
   builds.
2. **Layout collision with the GDI desktop / FIFO scribble** — enforce
   `desktopEnd ≤ tram < fifo < colBuf[0] < aux ≤ vram` at attach; never
   `SST_ENDEPTHBUFFER` without an allocated aux ("they could stomp on the cmd
   fifo", `gglide.c:1830-1833`); always `SST_ENRECTCLIP` + clip inside target.
3. **Write ordering into the FIFO** — map non-cached first; write-combining
   later + `sfence` before dependent register writes (`P6FENCE`,
   `fifo.c:972-989`, `fxcmd.h:227-236`).
4. **Kernel FPU** — bracket DP2/gb execution in
   `EngSaveFloatingPointState/EngRestoreFloatingPointState`; hardware culling
   keeps the draw path a pure float copy.
5. **Texture corruption (Avenger errata / ENTEXTUREMAP transitions)** —
   unconditional pre/post flush packets + `nopCMD` on texture-enable changes;
   regression-test with an alternating-texture scene.

Secondary: SDRAM fastfill variant; non-256 ST scale (one-line switch); no vsync
in blit-present (acceptable; DdFlip later).

---

## M4b-1 implementation status (2026-07-24)

**Pure-logic core IMPLEMENTED + host-tested** (does NOT touch the driver link —
`gbstub.c` still satisfies the DDK build). Files under
`scripts/3dfx/driver/nt/`:
- `hw/h3hw.h` — self-contained register/bit/packet header extracted from the
  open glide h3 tree (each def cited).
- `gbk/gbk_packet.c` — PKT0/1/1-burst/3/4/5 encoders. `gbk/gbk_layout.c` —
  buffer carve-out + overlap-proof validator. `gbk/gbk_state.c` — all `gb_set_*`
  register lifts + `assertDefaultState` replay + PKT4 dirty-flush + fog-table
  packer. `gbk/gbk_fifo.c` — the 10-store init list + ring space/wrap reserve.
- `gbk/gbk.h` — the module API. Tests: `gbkernel-test/test_gbk_{packet,layout,
  state,fifo}.c` (`make -C scripts/3dfx/driver/nt/gbkernel-test` → ALL PASS 4/4).
  Modules compile clean under BOTH `i686-w64-mingw32-gcc -std=c89 -pedantic
  -Werror -nostdinc` AND **VC6** (the real DDK target compiler, verified
  2026-07-24 via `c:\clgbk.bat` in the Wine DDK — all 4 EXIT=0) — kernel-clean
  (no CRT/float/Win32), so M4b-2 can fold them into the DDK driver build.

**Ambiguities resolved from the glide source** (design corrected): (1)
`hwcInitFifo` is **10** stores, not 9 (`aMin`,`aMax` are separate). (2) linear
`calcBufferSize` is NOT page-rounded — parity adjust only. (3) `kSetupCullPositive
== 0` → CW-cull = CullEnable alone, cull-off = PingPongDisable. (4) blend
alpha-channel factors pinned to ONE/ZERO (only h3-supported). Defaults verified:
`fbzMode 0x80B21`, `alphaMode 0x40400`, `fbzColorPath 0x4000028`.

**✅ VERIFIED (2026-07-24).** The M4b-1 workflow's three adversarial verify agents
completed (9 agents, 0 errors): (1) **register-correctness** independently
re-derived every offset/shift/formula from the open glide source — all VALUES
correct (only a few comment line-numbers were off by one; no hardware impact);
(2) **kernel-safety** ASan-fuzzed gbk_layout/gbk_fifo (no OOB/overflow/overlap);
(3) **ABI/C89** confirmed fixed-width layout + C89-cleanliness. Fix loop added an
ILP32 `-m32 -Wshift-overflow=2` gate + `H3HW_MASK_IS_UNSIGNED` compile-asserts
(`SST_ALPHAREF`/`SST_ZACOLOR_ALPHA` → `0xFFUL`). Post-verify hardening: the PKT3
`nVerts` [9:6] and PKT5 `nWords` [21:3] encoders now mask to field width so an
out-of-range count can never leak into an adjacent field (FIFO-desync guard),
with regression tests. All 7 host tests green (`make -C scripts/3dfx test`),
modules clean under mingw + VC6 + ILP32. The pure-logic core is a verified,
safe foundation for the M4b-2 MMIO transport. (Residual: a few off-by-one comment
citations in h3hw.h — cosmetic, values all correct.)

## M4b-2 implementation status (2026-07-24)

**Kernel-mode MMIO transport IMPLEMENTED; the full driver now LINKS with the
REAL backend.** `gbstub.c` is out of the default build (kept for a no-hw smoke
link); `fxd3ddd.dll` links from `driver/nt/gbkernel.c` + the four verified
`gbk/gbk_*.c`. New files: `driver/nt/gbk/gbk_mmio.h` (volatile `GBK_WR32`/
`GBK_RD32` + `GBK_WMB` fence hook), `driver/nt/gbkernel.c` (transport + full
`gb_*` seam), `driver/nt/gbkernel.h` (the `gbkernel_attach`/`detach` entry for
the M4c chassis). `SOURCES` + `clfxd3d.bat` updated (glued `-Fo`). Build:
all 14 compiles `EXIT=0`, `LINKEXIT=0`; `make -C scripts/3dfx test` all-PASS
(gbk 4/4 + DP2 + glue unaffected).

Design decisions locked in bring-up:
- **One static device context** `g_dev {bar0,bar1,layout,fifo,state,w/h/depth,
  tramNext,backIdx,sscale/tscale,attached,faulted}`. `attach` = layout →
  10-store CMDFIFO init (direct PIO to BAR0; SstCRegs are never FIFO-routed) →
  FIFO sw-state → miscInit0 Y-origin (direct PIO) → colBuffer/aux/stride +
  leftOverlayBuf + swapbufferCMD=0 + full-rect clip (via FIFO PKT1) →
  `gbk_state_defaults` flush → initial FASTFILL. IO/CMDAGP registers are direct
  PIO; all 2D/3D register + memory writes go through the ring.
- **FIFO engine** `gbk_fifo_make_room(nBytes)` wraps the verified
  `gbk_fifo_reserve`: OK→return write offset; NEED_WRAP→store PKT0 JMP + wrap
  (≤1 wrap/reserve, else fault); STALL→re-poll `gbk_hw_readptr` (twice-stable,
  interleaved status read, capped) and retry. **Every loop is bounded**
  (`GBK_FIFO_STALL_CAP`/`GBK_STATUS_POLL_CAP` ~1e7) + an over-a-lap single
  packet fails safe (can't loop NEED_WRAP). On any cap-hit → `gbk_fault()`
  latches, disables the FIFO (`baseSize=0`), and refuses all further MMIO — a
  wedged card never hangs win32k (design risk #1).
- **Pure-integer transport, no FPU on the untextured path.** Vertex colors
  (uchar→float 0-255) come from an **integer-built IEEE-754 bit table**
  (`gbk_ub_to_f32bits`, no `float`); x/y/ooz/oow are **raw dword copies**
  (`GBK_F32`). The ONLY float arithmetic is textured perspective-ST
  (`s*texW*oow`), isolated to the tex branch and bracketed per design risk #4
  (the DP2 executor owns `EngSave/RestoreFloatingPointState`).
- **gb_* coverage = 100% of gbstub's symbol set** plus the extra draw/tex/
  lifecycle bodies the core calls: state setters → `gbk_state_*`; draws →
  flush + PKT3 (batched, split at MAXVERTS=15); clear → FASTFILL 2-packet;
  swap → 2D WAX blit-present (back→desktop); finish → nopCMD + status&BUSY
  poll; tex create/upload(PKT5 + Avenger errata flush)/bind/none/destroy;
  `gb_startup/open/close/shutdown/enum_modes/set_vertex_layout` thin.

**M4d hardware-validation residuals** (structurally faithful, exact values to
be confirmed on-card via the escape ladder; every MMIO touch is tagged
`M4d: validated on-card`): clip-register field order, the WAX blit BIT(31)
address flag + dst/format, the `tLOD` LOD/aspect encoding (reduced from
`grTexSource`), and P8 (download raw; convert P8→565 host-side for bring-up).

## M4c-1 implementation status (2026-07-24)

**Backend WIRED to the hardware in the chassis + the escape-driven bring-up
ladder BUILT.** The full driver links with attach live (`build_fxd3d.sh`: all
15 compiles `EXIT=0`, `LINKEXIT=0`); `make -C scripts/3dfx test` all-PASS.

- **Attach wiring — `driver/nt/chassis.c`.** `DrvEnableSurface` now calls
  `fxchassis_attach_backend()` after the framebuffer map: BAR0 register aperture
  ← `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES` (first memory-space range with a
  valid VA), VRAM size ← the FB map's `VIDEO_MEMORY_INFORMATION.VideoRamLength`,
  desktop geometry ← the set mode (`desktopEnd = cyScreen*lDeltaScreen`,
  `desktopStride = lDeltaScreen`, w/h = cx/cyScreen), z = 16-bit aux → one
  `gbkernel_attach(bar0,bar1=pjScreen,vram,desktopEnd,desktopStride,w,h,2)`.
  **Graceful degrade:** a failed query/attach logs and leaves a 2D-only
  surface (never fails the surface enable). `DrvDisableSurface` calls
  `gbkernel_detach()` then `IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES`.
- **BAR1 non-cached is a documented miniport CONTRACT** (gbk_mmio.h): the
  `IOCTL_VIDEO_MAP_VIDEO_MEMORY` mapping exposes no cache attribute, so the
  chassis states + relies on the paired `3dfxvsm.sys` mapping BAR1 non-cached,
  verified on-card at M4d via `FXDBG_PROBE`+`FXDBG_CLEAR`; if it maps
  write-combining, `GBK_WMB` must grow an `sfence`.
- **Desktop-stride threaded** (`gbkernel_attach` gained a `desktopStride`
  param): `gb_swap`'s blit-present **dst** now uses the desktop pitch, not the
  16bpp 3D color-buffer stride (M4b-2 minor #2). The 32bpp desktop dst-pixfmt
  convert was **done in M4c-2** (`gbk_present_dstformat()` in `gbk/gbk_surf.c`:
  16bpp = plain 565 copy, 32bpp = `SSTG_PIXFMT_32BPP` dst so the 2D engine expands
  565→8888 during the present blit — design §5a 16→32).
- **Bring-up ladder — `driver/nt/gbkdebug.c` + `gbkdebug.h` + `gbkernel_dbg_*`
  in gbkernel.c.** A `DrvEscape` handler (wired `INDEX_DrvEscape` in the DRVFN
  table) with a private opcode namespace `0x3DF0..`, each rung returning a small
  result buffer and NONE needing D3D:
  - `FXDBG_PROBE` (0x3DF0): read status reg (3D+IO), cmdFifo0 readPtr/baseSize/
    depth, computed layout offsets, attached/faulted, sw ring cursors — proves
    BAR0 mapping + FIFO init WITHOUT drawing.
  - `FXDBG_CLEAR` (0x3DF1): `gb_clear(rgb)`+`gb_swap` — proves FIFO exec e2e.
  - `FXDBG_TRI` (0x3DF2): one hardcoded gouraud triangle + swap — PKT3+state+
    submit.
  - `FXDBG_TEX` (0x3DF3): 32×32 checker upload+bind+textured quad+swap —
    PKT5+TMU.
  - `FXDBG_READBACK` (0x3DF4): copy a framebuffer (BAR1) rect back (back color
    buffer or desktop primary) so the agent verifies pixels without a GDI
    screenshot of the 3D surface.
  Every rung reuses the iteration-capped FIFO/fault path (a wedged card fails
  safe). The agent drives them via `ExtEscape` on the display DC from a tiny
  user tool at M4d.
- **M4d driver tool BUILT — `driver/nt/fxdbg/fxdbg.c` (+ `Makefile`,
  `selftest.c`).** The "tiny user tool" is now written and compile-verified:
  `CreateDC("DISPLAY")` + `ExtEscape` over the shared `gbkdebug.h` ABI, one
  subcommand per rung (`support|probe|clear R G B|tri|tex|readback WHICH X Y W H
  [out.bmp]|ladder`). `ladder` runs rungs 1..5 in order and exits non-zero on
  any fault, so it gates an automated deploy/verify. `readback` decodes the
  16bpp-565 rect into a 24bpp BMP host-side (no on-card GDI). It touches NO card
  state itself — every card touch stays inside the display driver. A host ABI
  guard (`selftest.c`, built `-m32` for the ILP32 driver ABI) pins the opcodes,
  `FXDBG_MAGIC`, and every wire-struct size/offset so a field edit fails the
  build, not the box; wired into `make -C scripts/3dfx test`. **The tooling side
  of M4d is done — the only remaining gate is deploying `fxd3ddd.dll` to .124
  (physical-recovery risk; needs explicit go-ahead) and running `fxdbg ladder`.**
- **DDI FP bracket (M4b-2 minor #9).** `d3d_DrawPrimitives2` (enable.c) now wraps
  the whole `fxd_dp2_execute_real_cb` in ONE `EngSaveFloatingPointState`/
  `EngRestoreFloatingPointState` region; the ladder's `FXDBG_TRI`/`FXDBG_TEX`
  bracket their float work (the `(float)` coord casts + textured ST) in
  `gbk_fpu_enter/leave` at the ladder entry. `gb_tex_bind` stays FPU-free
  (raw-bit scales) so it needs no bracket. gbkernel still brackets each draw
  batch internally; the outer regions nest cleanly.
- **Scope:** the full DDraw surface HAL (`DdCreateSurface/Lock/Flip/Blt`) + the
  §5a 16→32 present-convert remain NOTHANDLED stubs tagged
  `TODO(fxd3d M4c-2)` — deferred until this ladder validates the backend on-card
  at M4d. Every new card-touch here is **compile-verified only**; the ladder IS
  its hardware regression at M4d.
