# Direct3D performance on vcr-kmd (V5 6000, `.124`): where the time goes, and a plan to make it fast

Investigation of 2026-11-12, perf lane of the D3D campaign. Read-only: nothing was built, run, committed or
deleted, and no fleet box was contacted. Facts quoted from `evidence/d3dplan_1112/onbox/` were recorded today by
the agent that owns `.124`.

**Conventions.** Every claim carries `path:line`. **(inferred)** marks anything taken from general
DirectX/3dfx knowledge or from a model rather than from a file. That applies to all bus-cost numbers below:
measurement M1 (section 6) exists to replace them with measured ones.

| abbreviation | absolute path |
|---|---|
| `KMD` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/vcr-kmd` |
| `EV` | `KMD/evidence` |
| `VH5` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5` (3dfx's own W2K/XP display driver + D3D HAL) |
| `VINC` | `/home/voidsstr/development/retro-3dfx/3dfx Driver Code/H5/INCLUDE` |
| `GL5` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5` (our h5 Glide: `glide3/src`, `minihwc`, `incsrc`) |
| `GBK` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/scripts/3dfx/driver/nt` (fxD3D's kernel Glide backend, never on silicon) |
| `PLAN` | `/home/voidsstr/development/retro-agent/.claude/worktrees/v56k-bench/docs/v56k-benchmark-plan.md` |

---

## 0. Summary

1. **Every 3D register write is a single uncached store into chip 0's 31-entry PCI FIFO.** There is no command
   FIFO. `VcrDdRoom` reads `status` before every vertex. As a result the CPU can never be more than about one
   triangle ahead of the chip, and the game's CPU work (software T&L, logic) and the chip's rasterising run one
   after the other instead of overlapping. This is the biggest structural limit.
2. **Bus cost per triangle:** 27 (untextured), 36 (one texture) or 45 (two textures) uncached writes, plus at
   least 3 uncached status reads. That is roughly 5-8 µs per textured triangle (inferred), or about
   120-220 K triangles/s with the CPU doing nothing else. At 85 Hz that budget runs out at about 1,400-2,500
   textured triangles per frame.
3. **Strips, fans and indexed primitives are expanded into independent triangles**, so three full vertices are
   sent per triangle. Strip and fan content therefore costs up to 3x what it needs to.
4. **The fill path probably runs at one pixel per clock.** The HAL never sets VSA-100 two-pixels-per-clock
   (`combineMode` bit 29), the 2PPC band height, the column band or hardware guard-band. 3dfx's HAL and our
   Glide enable these by default. The one silicon perf number fits this model: 151 fps on a scene of about
   1.3 Mpix per frame matches ~1 px/clk, not 2. Fixing it is cheap and could give up to ~2x single-texture fill.
5. **D3D runs on one VSA-100 out of four.** Measured Glide fill is 330 Mpix/s for one chip against
   1,125 Mpix/s for four. At the staged 1280x960 a single chip cannot hold 85 fps on overdraw-heavy scenes, even
   with 2PPC (inferred model).
6. **Smaller, cheap costs:**
   - every `VcrDd()` call is a `vsnprintf` plus an IOCTL whatever the log level, and a DEBUG event is logged
     per texture bind;
   - the HAL re-sends the full state and the render target at the start of every DP2 call and on every change;
   - every flip, Lock and texture upload drains the whole engine.
7. **Correctness risk found on the way, unverified.** The HAL advertises `CANRENDERAFTERFLIP` but draws at
   once into the buffer a pending vsync'd flip is still scanning out.
8. **There is no D3D game frame rate on vcr-kmd at all.** The `d3dprobe perf` scene is fill-bound and does not
   measure the triangle path.

The plan has ten steps, ordered for safety and value:

| step | what it does |
|---|---|
| P0 | instrument first, including driver-side vsync-on accounting |
| P1 | gate the logging |
| P2 | 2PPC, band height and column band |
| P3 | slim the direct path: packed colour, credit-based FIFO-room checks, strips and fans via the setup unit, autostrip |
| P4 | shadow registers |
| P5 | guard band |
| P6 | **CMDFIFO with packet 3**: the big one; host-tested code to reuse exists in `GBK` |
| P7 | queue the hardware swap (`swapbufferCMD`) |
| P8 | upload textures with packet 5 |
| P9 | SLI for D3D (last, supervised) |

Each new behaviour sits behind its own Diag switch and is measured before and after with vsync on, by flip-
interval statistics and frame-time headroom recorded inside the driver.

---

## 1. How a Direct3D frame reaches the chip today

### 1.1 Transport: direct register writes into the PCI FIFO, no command FIFO

- The 3D engine is driven by "straight register writes through the PCI FIFO, the same transport as the 2D
  engine". Triangles use the triangle-setup unit's `s*` registers, with `sBeginTriCMD`/`sDrawTriCMD`
  (`KMD/display/vcrdd_3d.c:5-10`).
  - `w3()` is a volatile 32-bit store to `pjRegs + 0x200000 + off` (`vcrdd_3d.c:24-27`).
- **The register window is mapped uncached.** `IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES` maps chip 0's BAR0
  with `VIDEO_MEMORY_SPACE_MEMORY` only (`KMD/miniport/vcrmp.c:728-738`).
  - The LFB the display driver gets is write-combined (`VIDEO_MEMORY_SPACE_P6CACHE`, `vcrmp.c:699`).
  - So is the DirectDraw user view (`KMD/miniport/vcrmp_dd.c:28-29`).
- **The FIFO is 31 entries.**
  - `ST_FIFO_FREE 0x1f` (`KMD/display/vcrdd_2d.c:68`); vendor `SST_PCIFIFO_FREE 0x1F` (`VINC/H3DEFS.H:111`).
  - Measured on silicon: "FIFO free when empty 31" (`EV/silicon/d3d32_step16/d3d32_boot_vcrlog.txt:106`).
- **`VcrDdRoom(pd, n)` reads `status` at least once per call** and spins until `status[4:0] >= n`, bounded by
  `SPIN_CAP` = 2,000,000 reads (`vcrdd_2d.c:71`, `vcrdd_2d.c:92-108`).
- **No command FIFO of our own.** "No command FIFO of our own and no interrupts: the CMDFIFO is Glide's alone"
  (`KMD/README.md:76-79`).
  - In KMD's sources `cmdFifo0` appears only in the Reset3D pre-check (`KMD/include/vcr_3dseq.h:165-182`,
    `vcrdd_2d.c:410`) and in the miniport's engine reset, which stops it (`KMD/miniport/vcrmp_hw.c:190`).
  - The README roadmap already lists it: "The CMDFIFO instead of PCI-FIFO writes." (`KMD/README.md:796`).

### 1.2 Per-vertex and per-triangle traffic

`vertex()` (`vcrdd_3d.c:183-223`) does the following for each vertex:

- one `VcrDdRoom(9 [+3] [+3])`, i.e. at least one status read (`:188`);
- `sVx`, `sVy` (`:190-191`);
- the colour as **four floats**, `sRed`/`sGreen`/`sBlue`/`sAlpha` (`:197-200`);
- `sVz` and `sOowfbi` (`:201-209`);
- per texture stage, `sOow`/`sSow`/`sTow` (`:210-221`). For two stages TMU1 gets its own `sOow1`, the same
  value as `sOow0` (`:218`).

`VcrDd3dTriangle` writes `sBeginTriCMD` after vertex 1 and `sDrawTriCMD` after vertices 2 and 3
(`vcrdd_3d.c:225-239`).

| triangle kind | uncached writes | status reads (minimum) | notes |
|---|---|---|---|
| untextured (gouraud) | 3 x (8 + 1) = **27** | 3 | +1 read per spin while the chip is behind |
| one texture | 3 x (11 + 1) = **36** | 3 | |
| two textures | 3 x (14 + 1) = **45** | 3 | `sOow1` duplicates `sOow0` |
| a strip or fan triangle | the same as a list triangle | 3 | expanded, see 1.3 |

With 31 FIFO entries and 12-15 entries per vertex, the CPU can queue at most ~2.5 vertices (less than one
triangle) before `VcrDdRoom` spins (`vcrdd_2d.c:101-105`). It spins until the setup unit and rasteriser drain
the FIFO. **The CPU therefore waits through most of every large triangle's rasterisation.**

### 1.3 Primitive expansion: everything becomes independent triangles

`walk()` turns every primitive into `tri(a, b, c)` (`KMD/display/vcrdd_d3d.c:1612-1689`), and `tri()` sends
three complete vertices (`:1367-1374`):

- TRIANGLESTRIP (`:1621-1633`) and TRIANGLEFAN (`:1634-1642`);
- INDEXEDTRIANGLELIST and INDEXEDTRIANGLELIST2 (`:1643-1658`);
- INDEXEDTRIANGLESTRIP and INDEXEDTRIANGLEFAN (`:1659-1677`);
- TRIANGLEFAN_IMM (`:1678-1689`, through `imm_tri`, `:1376-1381`).

Nothing uses the setup unit's own strip/fan continuation (`sSetupMode` `SM_FAN` and `SM_NO_PINGPONG`,
`KMD/include/vcr_3dregs.h:193-196`), packet 3, or autostrip.

### 1.4 State: recomputed and re-sent whole, at every DP2 call and every change

- `D3d_DrawPrimitives2` zeroes its `dp2walk` (`vcrdd_d3d.c:1835`), so `prepared` = 0.
  - The first triangle of **every** DP2 call therefore goes through `prepare()` → `compute_regs()` →
    `VcrDd3dTarget()` + `VcrDd3dState()` (`vcrdd_d3d.c:1322-1365`, `1335-1337`, `1361`).
  - Any later `dirty` does the same again.
- **What one re-send writes:**
  - The target: `renderMode` plus 6, or 9 writes at 32 bpp (`KMD/include/vcr_3dseq.h:56-78`,
    `vcrdd_3d.c:45-70`).
  - The state: `VcrDdRoom(12)` (`vcrdd_3d.c:76`), 8 FBI registers (`:78-85`), 4 TMU0 registers (`:113-118`),
    4 more TMU1 registers after another room check (`:119-126`), and a broadcast `nopCMD` before compressed
    textures (`:107-112`).
  - In total, **~19-30 uncached writes plus 2-4 status reads**.
  - The fog table is the one thing shadowed: 32 writes only when it changes (`vcrdd_3d.c:86-101`,
    `KMD/display/vcrdd.h:153-154`).
- **`compute_regs()` costs CPU each time** (`vcrdd_d3d.c:921-1029`):
  - two `handle_get` hash lookups (`:924-925`);
  - `view_of` → `vcr_texlod_compute`, plus a mip-chain walk (`:754-799`);
  - `log_combo`, a hash plus a linear scan of up to 64 entries (`:882-919`, called at `:1027`).
- **Unchanged texture state still marks the context dirty.**
  - `D3DRENDERSTATE_TEXTUREHANDLE` always sets `dirty` (`vcrdd_d3d.c:1400-1405`).
  - So does `D3DTSS_TEXTUREMAP`, even when the handle is the same (`:1600-1602`).
- **Clears and SETRENDERTARGET force a full re-send too.**
  - A clear sets `dirty` and `prepared = 0` (`vcrdd_d3d.c:1419-1420`).
  - `Clear2` re-sends the target for every rectangle (`:1890-1899`, `clear_rects` → `VcrDd3dTarget`, `:1416`).
  - SETRENDERTARGET sets `dirty` (`:1741-1742`).

### 1.5 Clears

These are cheap and correct. `VcrDd3dClear` is a `fastfillCMD` per rectangle (3 writes, plus 3 per rectangle
and 2 at the end), with no idle wait (`vcrdd_3d.c:136-179`). Their only cost is the state re-send they trigger
(1.4).

### 1.6 Texture uploads and the TMU-cache flush

- **The D3D8 runtime uploads managed textures by Lock plus CPU writes, with no TEXBLT**
  (`/home/voidsstr/development/retro-3dfx/FINDINGS.md:1214-1215`).
  - `Dd_Lock` calls `VcrDd2dSync`, a full idle wait (`KMD/display/vcrdd_ddraw.c:658`).
  - The runtime then copies through the write-combined DirectDraw view (`vcrmp_dd.c:28-29`).
  - `Dd_Unlock` flags the texture (`vcrdd_ddraw.c:676-681` → `vcrdd_d3d.c:606-614`).
- **TEXBLT (DX7 titles) also drains first**, then copies in kernel memory with `memcpy` into the
  write-combined `pvRamBase` (`vcrdd_d3d.c:1513`, `1532`, `1424-1449`).
- **The first draw that samples a written texture runs `VcrDd3dTexFlush`** (`vcrdd_d3d.c:1287-1320` →
  `vcrdd_3d.c:253-270`):
  - two 2D NOPs, then `texBaseAddr` pointed away and back with a `nopCMD`;
  - one **uncached read of a texel** from video memory (`:264`), written back through the texture port (`:266`);
  - in all, 7 writes plus 1 status read plus 1 read of video memory.

### 1.7 Flip, vsync, GetFlipStatus, Lock and Blt

- **`Dd_Flip` (`vcrdd_ddraw.c:315-361`):**
  - If the previous vsync'd flip has not latched it returns `DDERR_WASSTILLDRAWING` (`:330-333`); the runtime
    retries, and each retry is an `IOCTL_VCR_VBLANK` (`:246-255`).
  - Otherwise it calls **`VcrDd2dSync` (`:334`), a full drain of the 3D engine before the flip**, then:
    - `IOCTL_VCR_DDFLIP`, in which the miniport writes `vidDesktopStartAddr` directly (`vcrmp_dd.c:65-80`);
    - a TRACE log IOCTL (`vcrdd_ddraw.c:342`);
    - another vblank IOCTL (`:352`).
  - The chip latches the address at the next vsync (`KMD/include/vcr_flip.h:9-16`).
  - Completion is a status-bit poll through IOCTLs, with a deadline (`vcr_flip.h:28-33`).
- **`DDFLIP_NOVSYNC` is honoured** (`vcrdd_ddraw.c:102-105`, `319-324`).
  - This is what raised `d3dprobe perf --novsync` from 85.0 to 151 fps (`PLAN:1097`).
- **`Dd_Lock` and `Dd_Blt` refuse the buffer a pending flip is taking off the screen**
  (`vcrdd_ddraw.c:501-504`, `652-657`). **DP2 and Clear2 do not check it** (section 4, perf-10).
- **`D3DDEVCAPS_CANRENDERAFTERFLIP` is advertised** (`vcrdd_d3d.c:2068`).

### 1.8 Logging on the hot path

- **`VcrDd()` always formats the message and always issues an IOCTL** (`KMD/display/vcrdd_log.c:20-39`).
  - The level is filtered only in the miniport, after the round trip (`KMD/miniport/vcrmp_log.c:322-327`).
  - The default `LogLevel` is DEBUG (`vcrmp_log.c:279`), so DEBUG events are also stored.
- **Calls on the drawing path:**
  - a DEBUG event on **every `D3DTSS_TEXTUREMAP`** in the DP2 stream (`vcrdd_d3d.c:1603`);
  - DEBUG events per TEXBLT (`:1765`) and per render-target change (`:1854`);
  - TRACE events, dropped after the round trip, on every Flip (`vcrdd_ddraw.c:342`), Lock (`:650`) and
    Blt (`:479`).
- `vcrdd_log.c:4` says "Never on a drawing path"; the texture-bind event is on one.

### 1.9 One chip of four

- The D3D target and state go to chip 0's window only. "With snooping off (the mode set before this turned SLI
  off) only chip 0 sees any of these writes" (`KMD/include/vcr_3dseq.h:97-99`).
- SLI is enabled only by Glide's request (`KMD/README.md:76-80`).

### 1.10 Rasteriser modes left at their defaults, read back on silicon today

The on-box agent's register read of a live D3D state (`EV/d3dplan_1112/onbox/regs/regs_after_windowed_1009.txt`)
shows:

- `combineMode` (0x200208) = 0, so 2PPC is off;
- `renderMode` (0x2001e0) = 0x001e0002: 32 bpp plus RGBA write, 2PPC band selection 0, guard band (bit 21) off;
- `fbzColorPath` (0x200104) = 0x0c002401: bits [31:30] = 0, the column of 8.

This matches the code:

- `compute_regs` builds `fbzColorPath` from `color_path() | CP_PARMADJUST` only (`vcrdd_d3d.c:977`).
- `vcr_rt_rendermode()` is the depth plus RGBA write enables only (`KMD/include/vcr_rtfmt.h:174-177`).
- Nothing in the HAL writes `combineMode`. Only the Reset3D sequence writes it, as 0 (`vcr_3dseq.h:193`).

---

## 2. What 3dfx's HAL and our h5 Glide do instead

| aspect | vcr-kmd D3D HAL today | 3dfx H5 D3D HAL (`VH5`) | our h5 Glide (`GL5`) |
|---|---|---|---|
| transport | uncached PCI-FIFO register writes (`vcrdd_3d.c:24-27`) | CMDFIFO in frame-buffer memory, hole counting (`VH5/CFIFO.C:86-168`, `:163`) | CMDFIFO at 96 KB + 0xff000, hole counting (`GL5/minihwc/minihwc.c:2567-2591`, `:2816-2900`) |
| room check | status read per vertex (`vcrdd_3d.c:188`) | `readPtrL` read only when the cached `roomToReadPtr` runs out (`VH5/CFIFO.C:304`, `:456-465`) | the same, `fifo.c:900-948`, as cited at `GBK/gbk/gbk_fifo.c:84-87` |
| triangle | 36 writes (1 tex) | **packet 3**, packed `sARGB`: 1 header + 3 x 8 dwords (`VH5/D6TRI2I.C:1118-1210`, `:1131-1134`) | packet 3, up to 15 vertices per packet (`GL5/glide3/src/gdraw.c:1097-1124`) |
| strips/fans | expanded | **autostrip**: shared-edge detection, then a 1-vertex `CMD_CONT` packet (`VH5/D6TRI2I.C:1080-1108`, `:1242-1276`) | `SSTCP_PKT3_BDDDDD`/`DDDDDD`, 1 vertex per triangle (`GL5/glide3/src/gstrip.c:166-269`) |
| TMU1 W | `sOow1` sent | never sent (0 case-insensitive hits for `sOow1` in `D6TRI2I.C`/`D6STRIPI.C`/`D6FANI.C`; only `DEBUG.C`) | pruned per parameter mask (`docs/3dfx-gbkernel-design.md:135-138`) |
| state | full re-send per DP2 batch and per change | per-group dirty flags (`VH5/D3GLOBAL.H:1166-1196`); state written only `if (HW_STATE_CHANGED)` (`VH5/D6DP2.C:1871-1873`) | register shadows + invalidation (`GL5/glide3/src/gtex.c:1004-1080`) |
| 2PPC | off | **on by default** (`VH5/D3INIT.C:1044-1060`); `combineMode` FBI/T0/T1 (`VH5/D6MT.C:4409-4414`); `ModeUses2PixPerClkRender` on Napalm (`VH5/DDFXNT.C:790-791`) | on when one TMU suffices (`GL5/glide3/src/gpci.c:1875`, `distate.c:1735-1750`, `gtex.c:986-1080`) |
| 2PPC band / column band | 0 / 0 | band log2 = 2 (`VH5/D3INIT.C:1062-1081`, `VH5/D6DP2.C:6351-6354`); column band 2 (`VH5/D3INIT.C:955-963`); "bought us 6 MTexels/s" (`VH5/D3INIT.C:66-68`) | band 2 (`GL5/glide3/src/gsst.c:4182-4183`, `gpci.c:1877`); column 32 (`gpci.c:1801`, `gsst.c:3940-3966`) |
| guard band | caps 0 (`(void)guard`, `vcrdd_d3d.c:2152-2162`) | Napalm -200/-200/2048/1536 (`VH5/D3INIT.C:3221-3236`), on by default (`:1084-1092`) | `SST_RM_ENGUARDBAND` on by default (`GL5/glide3/src/gpci.c:1872`, `gsst.c:4177-4178`) |
| flip | CPU drain + IOCTL write of `vidDesktopStartAddr` | `leftOverlayBuf` + `swapbufferCMD` queued in the CMDFIFO (`VH5/DDFLIP.C:440-446`); throttled on `status[30:28]` to `WaitOnVsync + 1` pending (`:330-366`, `:377`); `GetFlipStatus` answers OK for 3D apps (`:701-718`) | `leftOverlayBuf` + `swapbufferCMD` in the FIFO, throttled by recording each swap's FIFO position (`GL5/glide3/src/gglide.c:2757-2828`); the scanout is the overlay (`KMD/README.md:156`) |
| texture upload | idle wait + CPU copy + 7-write flush | **packet 5** to the texture port, in order with draws (`VH5/D3TXTR.C:3549-3560`); cache flush via an inverted `texBaseAddr` in the FIFO (`:2532-2540`) | packet 5 |
| chips | 1 | SLI-aware FIFO (`VH5/CFIFO.C:266-273`, `:282-391`) | 4-chip SLI proven |

---

## 3. What has been measured, and what it does and does not show

### 3.1 Every D3D frame-rate number on the V5 6000 + vcr-kmd

| run | mode | fps | source |
|---|---|---|---|
| battery `d3dperf` before `DDCAPS2_FLIPNOVSYNC` | 640x480x16, `--novsync` (honoured as vsync) | 85.0 | `EV/silicon/vcrkmd-v5-4.jsonl:5` |
| battery `d3dperf` | 640x480x16, novsync | 151.2 / 151.1 / 150.9 | `EV/silicon/vcrkmd-v5-6.jsonl:11`, `vcrkmd-v5-7.jsonl:14`, `vcrkmd-256-1.jsonl:14` |
| D3D32 step 16 | 640x480x32, vsync | 84.2 | `EV/silicon/d3d32_step16/step16_run.log:25-28`, `KMD/README.md:788` |
| today, on-box agent | 640x480x16 and 1024x768x32, vsync | 85.1 / 84.4 | `EV/d3dplan_1112/onbox/session_log.md:15` |

- The battery runs `perf --full --novsync --res 640x480 --frames 300` (`KMD/tools/silicon_battery.py:256-258`).
- **There is no frame rate for any D3D game on vcr-kmd.** The 2026-10-15 and 11-08 sweeps record only frame
  statistics: width, height, luma mean and spread
  (`EV/d3dtune_1014/sweep_1108_fixed/20261108_020841/sweep.json`; `run.out:3-27`).
- Today's on-box agent has started a UT2004 `-benchmark` botmatch at 1280x960
  (`EV/d3dplan_1112/onbox/session_log.md:16`;
  `EV/d3dplan_1112/onbox/games/UT2004_botmatch/UT2004_botmatch.bat`). Its result, if it produces one, will be
  the first D3D game number.
- **The only D3D game number on this card is the vendor stack's:** UT99 436 D3DDrv on AmigaMerlin's D3D HAL,
  4-chip, 69.1 fps at 1024x768 and 69.5 at 800x600 (`PLAN:91`).
- 86Box Voodoo3 numbers exist: ours 42.8 fps (`EV/silicon/86box-v3-paced-1.jsonl:13`), the in-box driver
  60.8 fps (`EV/86box_v3/inbox_d3d_perf_full_800x600x16.json`). They come from an emulator at different modes
  and are **not** a valid comparison.

### 3.2 The `perf` scene is fill-bound; it says almost nothing about the triangle path

`d3dprobe perf` (`KMD/tools/d3dprobe.c:1497-1530`) draws `N = 200` bilinear-textured, modulated XYZRHW triangles
per frame. It also does one target-only clear and a Present. The vertices are `x = 20 + (i·37) mod (W-40)` and
`y = 20 + (i·53) mod (H-40)` (`d3dprobe.c:1502-1507`).

**Computed from that formula** (a host script; nothing was run on the box):

- at 640x480, only **67 of the 200 triangles have area**: consecutive vertices step by (37, 53), so a triangle
  is collinear unless a coordinate wraps;
- those 67 cover **~0.99 Mpix**;
- the clear adds 0.31 Mpix;
- per frame that is **~1.3 Mpix**.

| model (VSA-100 at 166.8 MHz, `KMD/README.md:231-233`) | textured triangles | clear | frame | fps |
|---|---|---|---|---|
| 1 textured px/clk, fastfill 2 px/clk (inferred) | 5.9 ms | 0.9 ms | 6.9 ms | ~146 |
| 2 textured px/clk (2PPC) | 3.0 ms | 0.9 ms | 3.9 ms | ~255 |
| **measured** | | | **6.6 ms** | **151** |

**Reading.**

- The measurement fits ~1 textured pixel per clock, which is supporting evidence that 2PPC is off and costs
  fill (perf-03).
- The submission cost of the 200 triangles must be largely hidden under the rasterisation of the 67 large ones,
  so this scene cannot measure triangles/s.
- Its `tris_s` field (30,246) is not a triangle throughput.
- With vsync on, it shows only that ~1.3 Mpix plus 200 triangles fit in 11.76 ms.

### 3.3 Fill references measured with our Glide on the same box

- One chip: 330.2 Mpix/s flat and 315.8 Mpix/s blended, at 1024x768 (`EV/glidelab/vcrkmd-paced-2.jsonl:1-2`).
- Four chips in SLI: 1124.6 Mpix/s flat and 1122.4 Mpix/s blended (`:4-8`).
- These are the ceilings a D3D path could reach on one chip (330, at 2 px/clk) and on four.
- The fill rate follows the core clock exactly (`KMD/README.md:268-271`).

### 3.4 Per-session triangle counts the HAL already keeps

- `ctx_free` logs DP2 calls, triangles, clears, unparsed opcodes and texture flushes per context
  (`vcrdd_d3d.c:1210-1215`).
- Known values:
  - Unreal Gold D3DDrv flyby on 86Box: 7,373 DP2, 1,096,654 triangles, 1,884 clears
    (`EV/86box_v3/unreal_gold_d3d_vcrkmd_stats.json`). That is ~580 triangles and ~4 DP2 calls per clear,
    assuming one clear per frame (inferred).
  - Mysteries of the Sith on `.124`: 7,014 DP2 and 342k triangles, session length unknown (`PLAN:674`).
- **No per-frame count exists for any title.** P0 adds one.

---

## 4. Bottlenecks, ranked by expected impact

The ranking weighs the effect on frame time for the D3D titles as staged on `.124`. Several of them run at
1280x960, from the sweep frame stats and the compat DB:

- Halo, Hidden & Dangerous Deluxe, Shogo, Thief II, UT2003, UT2004 (`EV/d3dtune_1014/sweep_1108_fixed/run.out:5-27`);
- Max Payne (`compat.py status --box .124`).

Others run at 640x480: Jedi Knight, MotS, Red Faction, System Shock 2, Thief Gold and Rainbow Six.

| # | id | bottleneck | expected effect | effort / risk |
|---|---|---|---|---|
| 1 | perf-01, perf-02 | no CMDFIFO: CPU and chip serialised through a 31-entry FIFO; 27-45 uncached writes and 3+ status reads per triangle | dominant for geometry-heavy titles, 2001+ engines (inferred 5-20K+ triangles per frame); present in every title | high / medium (P3 interim, P6) |
| 2 | perf-03 | ~1 textured px/clk: 2PPC, band and column band unset | up to ~2x single-texture fill; the main term at 1280x960 | low / low-medium |
| 3 | perf-04 | one VSA-100 of four | up to 3.4x fill at high resolution (measured with Glide); no help when CPU- or setup-bound | very high / high |
| 4 | perf-05 | strips, fans and indexed primitives sent as independent triangles | up to 3x fewer vertex writes on strip and fan content | medium / low |
| 5 | perf-07 | `VcrDd()` = `vsnprintf` + IOCTL per call; DEBUG event per texture bind | possibly several ms per frame for texture-switch-heavy titles (inferred) | trivial / none |
| 6 | perf-08 | full state + target re-send per DP2 call and per change; no shadows | ~20-30 writes + 2-4 reads + `compute_regs` per change | low-medium / low |
| 7 | perf-09 | flip = CPU drain + 3-4 IOCTLs; no hardware swap queue | small today (the FIFO is tiny); large once the CMDFIFO lets the CPU run ahead | medium / medium (needs P6) |
| 8 | perf-06 | 4 float colours, duplicate `sOow1` | -25-33% writes per vertex | low / low |
| 9 | perf-11 | full drain on every Lock and TEXBLT, plus a 7-write flush per uploaded texture | per-upload stall; matters for streaming titles | medium / medium (needs P6) |
| 10 | perf-12 | no guard band: the runtime clips every triangle that crosses the screen edge in software | CPU in the runtime's clipper (inferred: a few %) | low / low-medium |
| 11 | perf-13 | per-DP2 FPU save, `target_of`, `memset`; vblank polls as IOCTLs | µs per call | low / none |

### 4.1 Rough numbers (all bus costs inferred, to be replaced by M1)

**Assumptions** (inferred):

- an uncached single-dword posted write costs t_w ≈ 45-150 ns on the PCI-66/33-class bus the VSA-100s sit on;
- an uncached status read costs t_r ≈ 1 µs (the vendor comment, "Each spin is an uncached MMIO read (~1us)",
  `VH5/CFIFO.C:12-15`). A read also waits for every posted write ahead of it;
- write-combined bursts into frame-buffer memory run at 100-200 MB/s.

| path | dwords / triangle | status reads / triangle | µs / triangle | triangles/s with the CPU 100% in the HAL | single-textured triangles per 85 Hz frame at 50% CPU |
|---|---|---|---|---|---|
| today, untextured list | 27 | 3 | 4.2-7.1 | 140-240 K | 830-1,400 |
| **today, 1 texture, any primitive** | 36 | 3 | **4.6-8.4** | **120-220 K** | **700-1,280** |
| today, 2 textures | 45 | 3 | 5.0-9.8 | 100-200 K | 600-1,180 |
| P3 diet, 1-texture list (packed ARGB, credit checks) | 27 | ~1 | 2.2-5.1 | 200-450 K | 1,150-2,700 |
| P3 diet, 1-texture strip (setup-unit continuation) | 9 | ~0.4 | 0.8-1.8 | 0.55-1.25 M | 3,200-7,300 |
| P6 CMDFIFO packet 3, 1-texture list (25 dw = 100 B, write-combined) | 25 | ~0 | 0.5-1.0 | 1-2 M | 5,900-11,800 |
| P6 CMDFIFO packet 3, 1-texture strip (~8.5 dw) | 8.5 | ~0 | 0.17-0.34 | 3-6 M | 17,000-35,000 |

The CMDFIFO rows are upper bounds. The VSA-100 setup rate and the D3D runtime's software T&L on the 2 GHz
Athlon XP (inferred) will cap them first. The point is that the HAL stops being the limit.

**Fill.** One chip, overdraw 2.5 (inferred, typical for that era):

| resolution | Mpix per frame | 1 px/clk (today) | 2PPC, 1 chip | 4 chips, SLI + 2PPC (1,125 Mpix/s measured) |
|---|---|---|---|---|
| 640x480 | 0.77 | 4.6 ms | 2.3 ms | 0.7 ms |
| 1024x768 | 1.97 | 11.8 ms (85.0 fps cap) | 5.9 ms | 1.8 ms |
| 1280x960 | 3.07 | **18.4 ms (54 fps)** | **9.2 ms (108 fps)** | 2.7 ms |

- Dual-textured single-pass rendering is 1 px/clk in either mode (inferred).
- **At the staged 1280x960, a locked 85 fps on one chip is only plausible with 2PPC and with CPU work overlapped
  with rasterisation (the CMDFIFO).** On multitexture- or overdraw-heavy scenes only SLI clears it comfortably.
- The interim choice (the operator's, not a driver change) is to stage those titles at 1024x768 until SLI exists.

### 4.2 The individual findings

**perf-01 (performance): no command FIFO; the CPU and the chip are serialised.**

- **Where:** see 1.1.
- **Why it matters:** with at most ~31 queued writes, the chip goes idle as soon as the DP2 call returns.
  - The CPU then does the next batch's software T&L while the chip waits, and the chip draws while the CPU
    spins in `VcrDdRoom`.
  - Frame time ≈ CPU work + GPU work, instead of max(CPU, GPU) (inferred model).
  - 3dfx's HAL and our Glide both use the CMDFIFO (section 2).
- **Measured:** nothing isolates this today (3.2). Measure with the P0 counters (time spent spinning for FIFO
  room) and M1.

**perf-02 (performance): per-triangle bus cost.**

- **Where:** see 1.2.
- **Cost:** three status reads per triangle at minimum (`vcrdd_3d.c:188`, called three times by `:228-236`),
  each about 1 µs (inferred), plus 27-45 writes.
- **Interim fix without a CMDFIFO:** track FIFO credit. Read `status` once, then spend the free count it
  reported, as the text path's `VCR_MONO.credit` already does (`KMD/display/vcrdd.h:199-203`).

**perf-03 (performance): two-pixels-per-clock, band height and column band are never programmed.**

- **Where:** see 1.10.
- **The vendor:** 2PPC is on by default (`VH5/D3INIT.C:1044-1060`), applied when 0 or 1 texture stage is used
  (`VH5/D6MT.C:4409-4414`, `4452-4456`).
- **Glide:**
  - enables it when TMU1 is unused, after cloning TMU0's state into TMU1 (`GL5/glide3/src/distate.c:1735-1750`);
  - sends **12 `nopCMD`** when going back to 1 px/clk (`GL5/glide3/src/gtex.c:1043-1053`);
  - sets the band in `renderMode[24:22]` (`GL5/glide3/src/gsst.c:4182-4183`).
- **Register bits:**
  - `SST_CM_ENABLE_TWO_PIXELS_PER_CLOCK` = bit 29 (`GL5/incsrc/h3defs.h:463`);
  - band = `renderMode[24:22]` (`GL5/incsrc/h3defs.h:135-136`);
  - column band = `fbzColorPath[31:30]` (`GL5/incsrc/h3defs.h:835-836`).
- **Evidence it costs fill today:** 3.2 (inferred model; confirm with the M1 fill mode).
- **Hazards from Glide's own comments:**
  - "When 2PPC mode is enabled, any writes that go to one TMU will get sent to both"
    (`GL5/glide3/src/distate.c:1836-1840`);
  - the 2→1 transition needs the 12-`nopCMD` flush.

**perf-04 (performance / hardware): D3D uses one chip.**

- **Where:** see 1.9.
- **The cost:** 330 Mpix/s against 1,125 Mpix/s measured (3.3).
- **What it would take** (inferred design):
  - an SLI enable from the display driver for the life of a fullscreen D3D device;
  - all command traffic and texture writes snooped to the slaves, as Glide's are;
  - an SLI-aware flip;
  - per-chip FIFO room checks (`VH5/CFIFO.C:282-391`).
- **Risk:** the SLI/AA wedge history (`KMD/README.md:100-109`) makes this the last step.

**perf-05 (performance): primitive expansion.**

- **Where:** see 1.3.
- **The setup unit can already continue a strip or fan in the register interface:** one vertex plus
  `sDrawTriCMD`, with `SM_FAN` and `SM_NO_PINGPONG` in `sSetupMode` (`vcr_3dregs.h:193-196`) (inferred; the
  vendor's non-CMDFIFO branch emits only `sBeginTriCMD`/`sDrawTriCMD` per vertex, `VH5/D6TRI2I.C:1162-1164`,
  `1198-1200`, `1234-1236`).
- **Indexed lists:** the vendor's autostrip (`VH5/D6TRI2I.C:1080-1108`) finds shared edges and turns those
  triangles into continuations.

**perf-06 (performance): per-vertex payload.**

- **The colour is four floats** (`vcrdd_3d.c:197-200`). The vendor sends one packed `sARGB`
  (`VH5/D6TRI2I.C:1133-1134`).
- **This is pinned by a test:** `tests/python/test_vcr_kmd_d3d.py:171-187` asserts that `V3D_SARGB` is never
  written. So the change needs a Diag switch, and the test must keep the default path to floats.
- **`sOow1`** (`vcrdd_3d.c:218`) is never sent by the vendor (section 2). Dropping it depends on the setup unit
  taking TMU1's W from W0 when `SM_W1` is clear (inferred; verify with `d3dprobe tex2mod`/`tex2add`).
- **Effect:** 12 → 9 writes per single-textured vertex, and 15 → 11 per dual-textured vertex.

**perf-07 (performance): hot-path logging.**

- **Where:** see 1.8.
- **Cost:** each event is a `vsnprintf` and an EngDeviceIoControl round trip into videoprt and the miniport,
  about 5-15 µs (inferred).
- **Exposure:** a title that binds a few hundred textures per frame (Unreal-engine and Dark-engine titles,
  inferred) pays one event per bind. The events also fill the flight-recorder ring with per-draw noise.
- **Fix:** cache the miniport's level in the PDEV and return early in `VcrDd()`. Demote "stage %u texture %u" to
  TRACE, or count it instead of logging it. No register traffic changes.

**perf-08 (performance): state re-send.**

- **Where:** see 1.4.
- **The vendor's model:** per-group dirty flags, with state written only when it changed (section 2).

**perf-09 (performance): flip.**

- **Where:** see 1.7.
- **Why it matters later:** today the drain at the flip costs about one triangle, because the FIFO is tiny.
  With a CMDFIFO it would stall the CPU for the whole tail of the frame.
- **Two hardware swap routes:**
  - **(a) The overlay, as our own Glide already runs on this box.** `leftOverlayBuf` = front buffer, then
    `swapbufferCMD` = interval, both in the FIFO. Pending swaps are counted by recording each swap's FIFO
    position (`bufferSwaps[]`) against the read pointer (`GL5/glide3/src/gglide.c:2757-2828`). A fullscreen
    Glide game scans out through the overlay (`KMD/README.md:156`). 3dfx's D3D HAL does the same
    (`dd3DInOverlay`, `VH5/DDFLIP.C:440-446`, `701-718`). The cost is overlay set-up when the flip chain is
    created.
  - **(b) Keep the desktop layer.**
    - `leftDesktopBuf` at 3D offset 0x218 (`VINC/H3REGS.H:403`, the field after `chipMask`), then
      `swapbufferCMD` at 0x128 (`VINC/H3REGS.H:368`);
    - with `SST_SWAP_DESKTOP_EN` (bit 10) | `SST_SWAP_EN_WAIT_ON_VSYNC` (bit 0) (`VINC/H3DEFS.H:1663-1667`).
    - Both the vendor and our Glide use this only for the AA secondary buffer (`VH5/DDFLIP.C:404-415`, with a
      tearing caveat for `DDFLIP_NOVSYNC`; `GL5/glide3/src/gglide.c:2816-2823`). Unproven for a plain flip.
  - Either way, pending swaps also show in `status[30:28]` (`VINC/H3DEFS.H:121-122`).

**perf-10 (correctness, unverified): render-after-flip.**

- DP2 and Clear2 never test `pd->flip_from` or the pending-flip state. The only flip reference in
  `vcrdd_d3d.c` is reading the target at `:1848-1856`.
- `CANRENDERAFTERFLIP` is advertised (`vcrdd_d3d.c:2068`).
- So the first draws of frame N+1 (the Clear first) go straight into the buffer that is still scanned out until
  the vsync latch, up to 11.8 ms at 85 Hz.
- `vcr_flip.h:28-30` itself calls not polling between a flip and the next wait "the D3D pattern".
- **Whether the D3D8 runtime waits before rendering anyway is not established** (inferred: the DX7 DDK leaves
  it to the driver).
- It would show as tearing or a flash in the lower part of the screen, never in a GDI capture.
- **Measure before acting:** the P0 counter "draws into a pending flip_from".

**perf-11 (performance): texture upload and Lock.**

- **Where:** see 1.6.
- **Fix:** packet 5 through the CMDFIFO (`VH5/D3TXTR.C:3549-3560`). Then make Lock wait only when queued
  commands still touch that surface (P8).

**perf-12 (performance): no guard band.**

- **Where:** `vcrdd_d3d.c:2152-2162`. The `-2048/2048` guard values are computed and then discarded.
- **The vendor:** Napalm -200/-200/2048/1536 with guard-band clipping on by default (`VH5/D3INIT.C:1084-1092`,
  `3221-3236`).
- **Glide:** sets `SST_RM_ENGUARDBAND` (`renderMode` bit 21, `VINC/H3DEFS.H:143`; `GL5/glide3/src/gsst.c:4177-4178`).

**perf-13 (performance, minor): per-call overheads.**

- Each DP2 call does `EngSaveFloatingPointState`/`Restore` (`vcrdd_d3d.c:1861`, `1868`), `memset`,
  `target_of` and `fvf_layout` (`:1835-1856`).
- Each vblank or flip-completion poll is an IOCTL (`vcrdd_ddraw.c:147-151`, `232-255`), though the DLL already
  maps the register window (`vcrdd.h:135`).

---

## 5. Optimisation plan

**Rules applied throughout:**

- Every new hardware behaviour sits behind its own `Diag` switch and is off when absent. With the switch off,
  the register traffic stays byte for byte what `tests/native/test_vcr_kmd_d3dseq.c` and
  `tests/python/test_vcr_kmd_d3d.py` pin.
- Each verified step lands with:
  - a regression test asserting both the new value and the old one;
  - a `d3dprobe` check;
  - a before/after run of the measurements in section 6.
- One Diag change per clean boot.
- Never `vcrctl` or any ExtEscape tool while a D3D game runs (`PLAN:541-555`).
- Max Payne only with a person watching (`PLAN:240`, `553-555`).

**Correctness gate first.** Today's on-box run shows that a killed Glide client's `cmdFifo0`, left enabled,
"silently stops every D3D triangle (clears still work)" until a mode set resets the engine
(`EV/d3dplan_1112/onbox/session_log.md:9-12`). Every perf baseline must start from:

- a cold boot, or a clean `vcrctl modeseq`;
- `cmdFifo0.baseSize` = 0;
- `d3dprobe render` passing 40/40.

### P0. Instrument first (Diag `D3DStats`)

**Counters** (pure-logic header plus host test; RDTSC for intervals inside the HAL, QPC for wall clock).
Logged per session at the points the flip counters already use, "exclusive end", "mode off" and "DD local
gone" (`vcrdd_ddraw.c:262-313`):

- frames: successful flips;
- DP2 calls, triangles, vertices sent;
- uncached 3D writes, status reads, `VcrDdRoom` spin iterations;
- `VcrDd2dSync` calls and spins;
- `compute_regs` calls and state re-sends;
- texture flushes;
- Locks, and Locks that had to sync;
- TEXBLT bytes;
- `VcrDd` IOCTLs issued;
- time spent in DP2, Clear2, Flip and Lock;
- **flip intervals in refresh units** (≤1.25 / ≤2.25 / ≤3.25 / more);
- **work time and vsync wait per frame** (section 6.2);
- **"DP2 or Clear into a pending `flip_from`"** (perf-10);
- chip counters sampled at each Nth flip (one uncached read each), from 3D offsets computed from the
  `VINC/H3REGS.H:377-384` field order: `fbiPixelsIn` 0x14c, `fbiZfuncFail` 0x154, `fbiPixelsOut` 0x15c,
  `fbiTrianglesOut` 0x25c (`VINC/H3REGS.H:417`).
- The pixel counters give pixels per frame and overdraw with no tool attached.

**Tools:**

- `d3dprobe perf --mode setup|fill|state|upload|flip`, with `--prim list|strip|fan|ilist` and `--tex 0|1|2`
  (section 6.1);
- `glidelab tris` (packet-3 triangles/s with our Glide on `.124`, the target for P6).

**Gate:** the counters must not change register traffic. Test that the hardware writes are identical with
`D3DStats` on and off.

### P1. Logging gate (no register change, can be on by default)

**Change:**

- In `VcrDd()` (`vcrdd_log.c:20-39`), return before `vsnprintf`/IOCTL when the level is above the miniport's.
  Read the level once per PDEV, for example from `IOCTL_VCR_INFO`.
- Turn `vcrdd_d3d.c:1603` into a counter; keep the first N binds per context at DEBUG for diagnosis.

**Measure:** P0's `VcrDd` IOCTLs per frame, and the `perf --mode state` time per texture change, before and
after.

**Test:** a Python check that no `VcrDd` at DEBUG or TRACE sits inside `walk()`'s per-command loops, plus a
native or Python check of the early return.

### P2. 2PPC + band height + column band (Diag `D3D2ppc`; bits: 1 = 2PPC, 2 = band, 4 = column band)

**Change:**

- In `compute_regs` and `VcrDd3dState`, when 0 or 1 texture stage is used:
  - set `combineMode` bit 29 on FBI, TMU0 and TMU1, with three chip-field writes as `gtex.c:1056-1080` does;
  - clone TMU0's `textureMode`/`tLOD`/`tDetail`/`texBaseAddr` into TMU1.
- On the way to two stages, send 12 `nopCMD` to TMU0|TMU1 first.
- Set `renderMode[24:22]` = 2 in `vcr_3d_target_seq`, and `fbzColorPath[31:30]` = 2.
- With the switch off, keep both sequences byte for byte.

**Measure:**

- `perf --mode fill --tex 1` Mpix/s: expect about 1.0 → 2.0 textured px/clk, ~165 → ~330 Mpix/s on one chip
  (inferred);
- `--tex 2` should not change;
- the existing `perf` scene: expect 151 → ~250 fps novsync (3.2 model);
- vsync on: P0 headroom at 1280x960 in one 1-texture title.

**Correctness:** `d3dprobe render` 40/40 at 16 and 32 bpp; `mip`, `tex2mod`, `tex2add`, `blend`, `fogtable`,
`ztest`; a mixed 1-stage/2-stage/1-stage `d3dprobe` test that exercises the transitions.

**Tests:** a native sequence test (`vcr_3dseq.h`-style) for both values of the switch.

### P3. Slim the direct path (Diag `D3DFastSetup`; bits: packed colour, credit-based room checks, strip/fan continuation, autostrip, W1 sharing)

**Change:**

- Packed `sARGB` with `sSetupMode` unchanged (vendor precedent).
- A FIFO credit counter that reads `status` only when the remaining credit is short.
- STRIP, FAN and IMM-fan emitted as 1 vertex plus `sDrawTriCMD`, with `SM_FAN`/`SM_NO_PINGPONG` set
  accordingly. Watch the strip winding: today's code swaps two vertices on odd triangles (`vcrdd_d3d.c:1627-1630`).
  The setup unit's ping-pong culling must give the same result.
- Autostrip consecutive indexed triangles that share an edge (the `VH5/D6TRI2I.C:1080-1108` idea).
- Drop `sOow1` only after `d3dprobe` proves W1 = W0 is taken.
- This emitter is written once and reused as packet 3 in P6.

**Measure:** `perf --mode setup` µs per triangle, for each primitive type and stage count. Expect -30-40% on
lists and -65-75% on strips (4.1, inferred).

**Correctness:** new pixel-exact `d3dprobe` tests `strip`, `fan`, `istrip`, `ifan` and `ilist_shared`, with
culling on and off, plus the existing set.

**Tests:** extend `test_vertex_colour_goes_to_the_setup_unit_as_floats_not_packed_sargb`
(`tests/python/test_vcr_kmd_d3d.py:171-187`) to switch on/off, and native tests of the emitter's register
sequences.

### P4. Register shadows (Diag `D3DShadow`)

**Change:**

- Keep a PDEV shadow of every 3D register the HAL writes on chip 0, and write only what differs.
- Invalidate the shadow at mode set, `DrvAssertMode`, Glide exclusive acquire and release, Reset3D, engine
  reset and Clear (which rewrites `fbzMode`/`c1`/clip).
- Re-send the target only at context create, SETRENDERTARGET or a changed `rt_off` (a flip), not per DP2.
- Set `dirty` for TEXTUREMAP/TEXTUREHANDLE only when the value changes.

**Measure:** P0 writes per frame and `perf --mode state`.

**Test:** a native shadow-and-invalidation test that asserts the old full re-send when the switch is off.

### P5. Guard band (Diag `D3DGuardBand`)

**Change:**

- Report -200/-200/2048/1536 (the vendor's Napalm values) and set `renderMode` bit 21.
- Keep `FZ_RECTCLIP` with the clip at the target size, as today (`vcr_3dseq.h:75-76`).

**Measure:** a `d3dprobe` clipping scene with untransformed (software VP) vertices crossing the edges, timing
the CPU per frame. The runtime clipper's share comes from P0's "outside HAL" time.

**Correctness:** a pixel-exact guard-band test with vertices beyond the screen inside the band, and a sliver
along each edge.

### P6. CMDFIFO + packet 3 (Diag `D3DCmdFifo`; the big one)

**Design:**

- **Ring:**
  - ~1 MB of video memory at 96 KB, the same place as Glide's (`GL5/minihwc/minihwc.c:2567-2591`);
  - while the switch is on, the DirectDraw heap floor is raised past it (`KMD/include/vcr_ddheap.h:44-61`);
  - `cmdFifo0` is programmed by the display driver through its register window, which includes CMDAGP
    (`vcrmp.c:720-725`);
  - the 10 ordered stores of `hwcInitFifo`, hole counting ON (`GL5/minihwc/minihwc.c:2850-2895`;
    `GBK/gbk/gbk_fifo.c:20-58`).
- **Writes:**
  - go through `pvRamBase`, which is write-combined;
  - store fence (`sfence`; the Athlon XP has SSE, but a locked-op fallback keeps Banshee/Voodoo3 hosts safe)
    at packet boundaries before any read-pointer-dependent decision;
  - **a fence every 64 K dwords**, for the hole-counting quirk (`GL5/glide3/src/fxcmd.h:281-296`;
    `VH5/FIFOMGR.H:108-114`);
  - the wrap is a fenced `PKT0 JMP_LOCAL` (`GL5/glide3/src/fifo.c:1153-1185`).
- **Room:**
  - cached `roomToReadPtr`; read `readPtrL` (twice-stable) **only** when it runs out (`VH5/CFIFO.C:456-465`);
  - never request more than one usable lap (the vendor's 3DMark2000 TDR hypothesis,
    `/home/voidsstr/development/retro-3dfx/D3D-DRIVER-PLAN.md:214-225`);
  - every wait bounded by `SPIN_CAP` (2 M, `vcrdd_2d.c:71`), not by GBK's 10 M (`GBK/gbkernel.c:74`);
  - on a timeout: disable the FIFO, engine reset, fall back to the direct path, log.
- **Packets:**
  - packet 3 for triangles: `BDDBDD` up to 15 vertices, `BDDDDD`/`DDDDDD` for strips and fans
    (`VINC/H3GDEFS.H:225-237`);
  - packet 1 or 4 for state (with P4's shadows);
  - 2D-NOP and `texBaseAddr` flush as packet 1;
  - `fastfillCMD` clears in the FIFO.
- **Coexistence:**
  - 2D (`vcrdd_2d.c`) and GDI keep direct writes. Any direct 2D or 3D access first drains the FIFO (an
    extended `VcrDd2dSync`) and disarms it.
  - Disarm at exclusive acquire, mode set, `DrvAssertMode(FALSE)` and context destroy.
  - Re-arm at the first DP2 after.
  - **Refuse to arm while someone else's `cmdFifo0` is on**: the leftover-FIFO case above.
- **Reuse:** port GBK's pure-logic `gbk_fifo.c`/`gbk_packet.c` and their host tests
  (`GBK/gbkernel-test/test_gbk_fifo.c`, `test_gbk_packet.c`) into a KMD header (`vcr_cmdfifo.h`). Do **not**
  copy two things from `gbkernel.c`:
  - `gbk_fifo_make_room` polls `readPtrL` on every reserve (`GBK/gbkernel.c:260-266`), which would recreate
    perf-02;
  - its no-op `GBK_WMB` is only valid on an uncached ring (`GBK/gbk/gbk_mmio.h:33-58`), and our LFB is
    write-combined.

**Measure:**

- `perf --mode setup`: expect µs per triangle → ~0.3-1 (4.1);
- parity with `glidelab tris`;
- `perf --mode fill` unchanged;
- P0's CPU-in-HAL share per frame;
- game timedemos (6.3).

**Correctness:** the whole `d3dprobe render` matrix, the texture matrix and `present` (windowed Blt-present
mixes 2D direct writes with 3D FIFO work); Glide-after-D3D and D3D-after-Glide sequences; a killed-game
recovery test.

### P7. Hardware swap queue (Diag `D3DHwSwap`; needs P6)

**Change:**

- The flip becomes a FIFO swap, with no `VcrDd2dSync`:
  - **preferred:** our Glide's own route on this box, overlay scanout + `leftOverlayBuf` + `swapbufferCMD`
    (`GL5/glide3/src/gglide.c:2757-2828`; vendor `VH5/DDFLIP.C:440-446`);
  - **fallback:** `leftDesktopBuf` + `swapbufferCMD` | `SST_SWAP_DESKTOP_EN`.
- Throttle to N pending swaps. Count them as Glide does, by FIFO position against `readPtrL`, or from
  `status[30:28]` (vendor: `WaitOnVsync + 1`, `VH5/DDFLIP.C:340-366`).
- Flip, GetFlipStatus, Lock and Blt answer from the pending count.
- DP2 into the old front buffer is safe because it is ordered behind the swap. This also closes perf-10.

**Measure:** the P0 flip histogram (on-time rate), work and wait per frame, `ddlab flip` (`flips_s`,
`vblank_hz`), `d3dprobe perf` with vsync.

**Prove first on silicon:**

- the overlay set-up for a D3D flip chain at every staged mode and depth;
- if the fallback is used, that `SST_SWAP_DESKTOP_EN` moves `vidDesktopStartAddr` at vsync with no tear (both
  existing uses are AA-only);
- that rendering queued after the swap waits for it (inferred).

### P8. Texture downloads by packet 5, and per-surface fences (Diag `D3DTexPkt5`; needs P6)

**Change:**

- TEXBLT becomes packet 5 to the texture port, chunked below one lap, as `VH5/D3TXTR.C:3549-3560` does.
- Lock waits only if the surface's last-use FIFO position has not been passed (`readPtr`).
- The TMU-cache flush moves into the FIFO.

**Measure:** `perf --mode upload` MB/s and the stall per upload; texture streaming in UT2003/UT2004 with P0
counters.

### P9. SLI for D3D (separate design, supervised only)

Only after P6 and P7 are proven:

- SLI enable and disable around a fullscreen D3D device, through the existing kernel SLI path (`vcrmp_sli.c`);
- snooped FIFO and texture writes;
- per-chip room checks;
- an SLI flip.

**Measure:** `perf --mode fill` (expect ~3.4x, 3.3) and 1280x960 titles.

**Risk:** the highest in the plan; the AA wedge record applies (`KMD/README.md:100-109`).

---

## 6. Measurement plan with vsync ON (the user's choice)

With vsync on, fps saturates at the refresh: 85 Hz, or whatever the runtime picks. Today's 640x480 run planned
120 Hz and ran at 85 (`EV/d3dplan_1112/onbox/d3dprobe_perf_640x480x16_vsync.txt:2-4`), so **record the refresh
actually used** in every run.

Average fps hides everything below the cap, and with double buffering a frame of 12 ms shows as 42.5 fps
(inferred). The primary numbers must therefore describe **slack and missed refreshes**, not fps.

### 6.1 Synthetic: `d3dprobe perf` modes

| mode | what it isolates | KPI |
|---|---|---|
| `setup` | N tiny triangles (1-2 px, or degenerate), `--prim list/strip/fan/ilist`, `--tex 0/1/2` | µs per triangle and triangles/s (novsync); P0 writes and reads per triangle |
| `fill` | L full-screen quads per frame, `--tex 0/1/2`, `--blend` | Mpix/s and px/clk (core clock from `vcrctl clock`, read before the run, never during) |
| `state` | K texture binds or render-state changes per frame with tiny draws | µs per change; writes per change |
| `upload` | S KB texture Lock/fill/Unlock + draw per frame | MB/s; stall per upload |
| `flip` | Clear + Present only | flip overhead; on-time rate with vsync |
| existing `perf` | for continuity with 3.1 | fps |

Each mode runs **twice**:

- `--novsync`, which measures the chip and the HAL;
- vsync on, which records P0's flip histogram and per-frame headroom.

Run at 640x480x16, 1024x768x16, 1280x960x16, and at 32 bpp in an armed D3D32 boot.

### 6.2 Driver-side vsync-on accounting (P0), the honest number with vsync on

- `W_n` = t(first `Dd_Flip` call of frame n) − t(the successful `Dd_Flip` of frame n−1): the CPU and HAL work
  for frame n. In today's design the GPU drain happens inside the successful call, so count that drain into
  `W_n` as well.
- `V_n` = t(successful `Dd_Flip` n) − t(first `Dd_Flip` call n): the vsync wait (`DDERR_WASSTILLDRAWING`
  retries, `vcrdd_ddraw.c:330-333`).
- **Headroom** = 1 − `W_n`/T_refresh. Report the median and 95th percentile per session.
- **On-time rate** = share of flips latched one refresh after the previous one; histogram of 1/2/3+ refreshes.
- **Breakdown of `W_n`:**
  - time in the HAL (RDTSC), split into FIFO-room spins, idle syncs, logging IOCTLs and the rest;
  - time outside the HAL: the game plus the runtime's software T&L;
  - GPU estimate = `fbiPixelsOut`/frame ÷ fill rate.
- **What this says:** once HAL time ≪ `W_n`, more HAL work cannot help. The remaining levers are fill (P2, P9)
  or resolution.

### 6.3 Game measurements

- **UT 436 UTbench, D3DDrv:** with `scripts/benchmarks/utbench_sweep.py`; renderer list includes
  `D3DDrv.D3DRenderDevice` (`utbench_sweep.py:2-12`, `85`); demo on the share (`:30`). This is the best ready
  D3D benchmark on `.124`, where `UnrealTournament436` is deployed and verified (`compat.py status --box .124`).
  Vendor-HAL reference: 69.1 / 69.5 fps at 1024x768 / 800x600, 4 chips (`PLAN:91`).
- **UT2004, and UT2003 when it runs:** the engine's `-benchmark` botmatch, exactly as today's on-box agent
  launches it
  (`DM-Antalus?spectatoronly=true?numbots=8?quickstart=true?attractcam=true -benchmark -seconds=60`,
  `EV/d3dplan_1112/onbox/games/UT2004_botmatch/UT2004_botmatch.bat`). Results land under the game's `Benchmark\`
  folder (inferred).
- **Halo:** `-timedemo` (inferred flag; writes `timedemo.txt`, inferred). The launcher already uses `-novideo`
  (`PLAN:627`). The menu takes no synthetic input, so a timedemo is the only unattended route (inferred).
- **3DMark2000 and 3DMark2001 SE:** on the share but not staged
  (`/mnt/retro-share/Files/Games/Benchmarks & Tech Demos/3DMark2000_Pro_v1.1_DX7`, `3DMark2001_SE_DX8.zip`).
  - 3DMark2000's fill-rate and polygon-throughput tests map directly onto P2/P3/P6.
  - Install them in the build VM and stage them per the project rules.
  - The batch flags are unconfirmed (`.claude/skills/retro-benchmark/SKILL.md:101-106`).
  - 3DMark99 needs DX6.1 (`/home/voidsstr/development/retro-3dfx/D3D-DRIVER-PLAN.md:113`).
- **Titles without a timedemo** (Thief Gold/II, System Shock 2, Shogo, Hidden & Dangerous Deluxe, Red Faction,
  Max Payne (attended only), Jedi Knight/MotS, AvP): a fixed scripted segment with P0's driver counters. P0
  replaces FRAPS (`benchmarks.json` "giants"), which hooks the process and is not on `.124` as far as the files
  show.
- **Unreal Gold:** UE1 `-benchmark` never exits (`PLAN:94`); it needs the UTbench console route.

### 6.4 Protocol

- **One Diag change per clean boot.** Arm, cold boot, run, disarm.
- **Prepare the box:**
  - check `cmdFifo0` is off (`vcrctl reg 80024` before the game, never during);
  - quiesce as in the v56k plan (AI engine off, Found New Hardware wizard, etc.);
  - set the same desktop mode and refresh, and record both.
- **Runs:** same scene, one warm-up run discarded, then 3 runs; report the median and the range.
- **Report:**
  - vsync on: on-time rate and headroom;
  - vsync off, diagnostic only: uncapped fps for the before/after delta.
- **The vsync-off diagnostic:** use the title's own setting (UT2004 `UseVSync=False`,
  `EV/d3dplan_1112/onbox/session_log.md:16`) or a diagnostic Diag `D3DNoVsync`, which treats every flip as
  `DDFLIP_NOVSYNC` (the vendor had the same override, `waitOnVsync = _DD(WaitOnVsync)`, `VH5/DDFLIP.C:153`).
  Never leave it armed, and never compare across hosts.
- **Record with every row:** the `vcrdd.dll`/`vcrmp.sys` md5, the Diag values (`513/16`, `513/18` lines in the
  ring) and the core clock.

---

## 7. Constraints and hazards for the implementation

- **A leftover Glide FIFO blocks D3D.** A Glide client killed with `cmdFifo0` on leaves D3D triangles silently
  undrawn until a mode set resets the engine (`EV/d3dplan_1112/onbox/session_log.md:9-12`).
  - Our own FIFO must detect someone else's and refuse, or reset with Reset3D-style idle checks
    (`vcr_3dseq.h:173-183`).
  - It must never mix direct 3D writes with an armed FIFO.
- **Glide and D3D take turns on `cmdFifo0`.** While Glide holds the chip, `exclusive_pid` already stops D3D
  drawing (`vcrdd_d3d.c:1859-1860`). On release, D3D must re-arm and re-send all state: shadows invalidated.
- **Write-combining:** fence before handing data to hole counting, every 64 K dwords, and around the wrap
  (6, P6).
- **Bounded waits:** any wait in win32k must stay well inside XP's video watchdog. `SPIN_CAP` is already sized
  for that (`vcrdd_2d.c:71`; `VH5/CFIFO.C:12-17`).
- **2PPC:** TMU writes go to both TMUs; the 12-`nopCMD` flush on 2→1. Also, a Glide session can leave
  `combineMode` with `USE_COMBINE_MODE` set. Today only an armed Reset3D clears it (`vcr_3dseq.h:119-122`), so
  P2 should write `combineMode` explicitly whatever its switch says, once proven.
- **Testing rules:** keep `d3dprobe` as the gate, never run tools against a live D3D game, and pace mode
  switches at least 5 s apart (`KMD/tools/vcr_pace.h`).

## 8. Open questions to settle on silicon (each cheap with P0 + `d3dprobe`)

1. Real t_w and t_r on `.124` (M1 `setup` mode). These replace every inferred number in 4.1.
2. Is textured single-stage fill 1 px/clk today, and 2 with 2PPC (M1 `fill`)?
3. Does the D3D8 runtime ever draw into a pending `flip_from` (P0 counter)?
4. Does the setup unit take TMU1's W from W0 when `SM_W1` is clear?
5. Does `swapbufferCMD` with `SST_SWAP_DESKTOP_EN` flip the desktop address on the VSA-100 cleanly, and does it
   hold back the rendering queued after it?
6. Per-title triangles, binds, DP2 calls and pixels per frame. Rank the titles by HAL-bound vs fill-bound
   before choosing between P6 and P9.

## Appendix A. Register and packet facts used

| item | value | source |
|---|---|---|
| status free count | `status[4:0]`, 31 when empty | `vcrdd_2d.c:68`; `d3d32_boot_vcrlog.txt:106` |
| swaps pending | `status[30:28]` | `VINC/H3DEFS.H:121-122` |
| `cmdFifo0` (CMDAGP + 0x20) | baseAddrL 0x80020, baseSize 0x80024, bump 0x80028, readPtrL 0x8002c, aMin 0x80034, aMax 0x8003c, depth 0x80044, holeCount 0x80048 | `VINC/H3REGS.H:197-209`; read back on silicon in `EV/d3dplan_1112/onbox/regs/regs_after_windowed_1009.txt` |
| packet 3 header | `[2:0]`=3, `[5:3]` cmd (0 = BDDBDD, 1 = BDDDDD, 2 = DDDDDD), `[9:6]` vertices (≤15), `[21:10]` parameter mask, `[27:22]` setup mode, bit 28 packed colour | `VINC/H3GDEFS.H:225-237` |
| packet 5 | `[21:3]` words, byte enables, space `[31:30]`, then a base-address word | `VINC/H3GDEFS.H:252-265` |
| `swapbufferCMD` bits | 0 wait on vsync, `[8:1]` interval, 9 don't swap, 10 desktop | `VINC/H3DEFS.H:1663-1667` |
| 3D offsets (computed from struct order) | `swapbufferCMD` 0x128, stats 0x14c-0x15c, `combineMode` 0x208, `chipMask` 0x214, `leftDesktopBuf` 0x218, `swapBufferPend` 0x24c, `leftOverlayBuf` 0x250, `fbiTrianglesOut` 0x25c, `sSetupMode` 0x260 | `VINC/H3REGS.H:358-420`; cross-checks `vcr_3dregs.h:46-50` |
| `renderMode` | bit 21 guard band, `[24:22]` 2PPC band, bit 25 dither rotation | `GL5/incsrc/h3defs.h:134-137` |
| `combineMode` | bit 29 two pixels per clock | `GL5/incsrc/h3defs.h:463` |
| `fbzColorPath[31:30]` | column band (0 = 8, 1 = 16, 2 = 32, 3 = 4) | `GL5/incsrc/h3defs.h:835-836`; `GL5/glide3/src/gsst.c:3920-3966` |

**Note for anyone re-checking these facts by searching.** `GL5/glide3/src/gglide.c` is ISO-8859 text, so a
plain `grep` treats it as binary and reports **nothing**: `grep -i swapbuffercmd gglide.c` is silent, while
`grep -a` finds `grBufferSwap` at `:2619`. Use `grep -a` (and `-i`) on the Glide and vendor trees before
reporting that something is absent.
