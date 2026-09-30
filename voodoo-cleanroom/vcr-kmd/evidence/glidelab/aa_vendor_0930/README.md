# In-game AA on the V5 6000 - 2026-09-30 (user at the box for every AA run)

Stack (clean-room lane): vcr-kmd (live-clock build + AA auto-disarm `045619b`),
our h5 Glide `38a891e8` (SLIAA-GUARD + AA-TRACE), MesaFX ICD 0.1.78, then
**0.1.79** (QUIT-TRACE: `RETROGL_SYNCTRACE=1` writes disk-flushed ICD lines
into the same file as Glide's trace), agent 1.96.0. Game: Quake II at
1280x960x32 through our ICD. Traced runs set `FX_GLIDE_TRACE=2` (every Glide
step `FlushFileBuffers`'d before the hardware access it names), so the last
line of a trace is the last step that ran before a hard freeze.

## The runs, in order

| run | recipe | config | what happened | evidence |
|---|---|---|---|---|
| 09:39 | default (dos_mode.c) | 6 = 2x | 3 min of play fine; **froze at quit**, 4 s after `quit` with the console open. The frozen NIC flooded 802.3x PAUSE frames and took the whole wired LAN down | `../aa_supervised_0930/` |
| 11:58 | default | 6 = 2x | **froze at the start of the 3dfx splash** (`splash: grSplash` is the last line), monitor black / no signal | `../aa_supervised_0930b/` |
| 12:59 | **vendor** | 6 = 2x | 80 s play, console 15 s, **clean quit** | `q2aa3.trace.gz` |
| 13:15, 13:16 | vendor | 6 = 2x | untraced; 60 s + 2 min play, console x3, **2 clean quits**; user: "smooth AA, looked right" | - |
| 13:22 | vendor | 7 = 4x | 70 s play, console, **clean quit** | `q2aa4x.trace.gz` |
| 14:48 | vendor | 7 = 4x | untraced, 90 s play, console, **clean quit**; user: "smooth AA, looked right" | - |
| 14:52 | vendor | 8 = 8x | **froze** right after the 3dfx splash (user); last flushed line = the ICD's context-create gamma load, `winopen: done` + 30 ms | `pm8x/` |

Every freeze was followed by the kernel's AA auto-disarm on the next boot
(`SliAAAutoOff` = 49, 52, 55; `SliAA` = 0; Glide back to cfg 5).

## Root cause of the 2x/4x freezes (fixed)

For every tuple that stores ONE sample per chip (cfg 6 = 2x, cfg 7 = 4x)
Glide sends a secondary colour base of 0 (`SLI_AA_REQUEST: secColor` = 0).
The default recipe writes it into `cfgAALfbCtrl`: read back **0x4C000000** on
all four chips (base 0, CPU + dispatch AA writes on). Every AA LFB write was
then duplicated into video memory from offset 0 - where Glide keeps its
command FIFO (`hwcInitFifo`: fifoStart **0x18000**, length 0xFF000). A write
landing on FIFO commands not yet executed hangs the chips at random: the
splash (drawn with LFB writes), Quake II's console at quit (top of the screen),
never the glidelab test pattern (no LFB writes). Our ICD takes 2-3 LFB write
locks per frame in Quake II, AA or not (measured: cfg 5 has the same rate).

The vendor recipe (modelled on 3dfx's NT miniport) points the base at
`tileMark`: read back **0xDF8F6000** (2x), **0xDF1EE000** chips 0/1 and
**0xCF1EE000** chips 2/3 (4x). With it: 5 launches, ~8 min of AA play, 6
console opens, 5 quits, all clean. **It is the kernel's default since this
fix** (`Diag\SliAAVendorRecipe` absent = 1; 0 = the old dos_mode.c arm, kept
for A/B). Pinned by `tests/native/test_vcr_kmd_sli.c`
`the_aa_lfb_base_that_overwrote_glides_fifo_and_its_fix_match_silicon`
(both the old and the new readbacks).

## 8x (open)

Glide's cfg 8 request: 4 chips, no SLI, `aaSampleHigh` 2, a real secondary
colour buffer at **0x023DC000** (below the primary at 0x031EE000),
`enable2ndbuffer` 1. The vendor recipe wrote `cfgAALfbCtrl` = **0xCE3DC000**
(base = the secondary buffer, AA reads off) and the depth aperture
**0x400031EE** (the whole-tiled-range branch it also takes for 4x). The splash
completed (5.4 s) and `winopen: done` was reached; the box died within the
~110 ms in which a healthy 2x/4x start does the rest of the ICD's context setup
and Quake II's GL init (the next ICD line would be `palette(global) at swap 0`).

## Also seen (to do after the multi-game AA testing)

The user: under AA the loading, menu and splash screens are **garbled** until
gameplay starts; gameplay itself is correct.
