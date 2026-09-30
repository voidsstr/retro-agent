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
| 19:13 | vendor **+ READ_EN chips 0/1** (kernel 16:43) | 8 = 8x | traced (Glide AA write-lock trace + ICD 0.1.80): 70 s play, console, **clean quit**; `cfgAALfbCtrl` 0xDE3DC000 / 0xCE3DC000; user: "smooth AA, looked right" | `run8x_readen/` |
| 19:15, 19:17 | same | 8 = 8x | untraced, 60 s + 2 min play, console, **2 clean quits** | - |

Every freeze was followed by the kernel's AA auto-disarm on the next boot
(`SliAAAutoOff` = 49, 52, 55; `SliAA` = 0; Glide back to cfg 5).

**Totals with the fix:** 2x 5 launches, 4x 2, 8x 3 - every splash, console
and quit clean, ~14 minutes of AA play; the picture confirmed at every rate.

## What fixed it: AA LFB READ_EN on the master chip pair (corrected 19:30)

**Every freeze had `cfgAALfbCtrl` READ_EN (bit 28) clear on all four chips;
every clean run had it set on chips 0/1** - which is what both of 3dfx's
miniports write (CFG_AA_LFB_RD_EN for every AA request, cleared only on chips
2/3 of the 4-chip high-sample shapes; NT `SLIAA.C:2443-2449`, `:3409-3419`):

| run | cfgAALfbCtrl chips 0/1 / 2/3 | READ_EN on 0/1 | result |
|---|---|---|---|
| 2x, default recipe | 0x4C000000 / same | off | froze (quit, splash) |
| 8x, vendor recipe before the fix | 0xCE3DC000 / same | off | froze after the splash |
| 2x, vendor recipe | 0xDF8F6000 / same | on | 5 clean launches |
| 4x, vendor recipe | 0xDF1EE000 / 0xCF1EE000 | on | 2 clean launches |
| **8x, vendor recipe + READ_EN** | **0xDE3DC000 / 0xCE3DC000** | **on** | clean (see below) |

The 8x pair is the controlled experiment: the base (the real secondary buffer
0x023DC000), the depth aperture, /4 and every other register were identical;
only READ_EN on chips 0/1 changed (kernel: `vcrmp_sli.c`, READ_EN for every
AA shape of a 4-chip board). What READ_EN does beyond LFB read snooping is not
documented in any source here; a read handshake between the paired chips that
the master waits on forever would fit the random, hard freezes.

**WITHDRAWN: "AA LFB writes duplicated into Glide's command FIFO".** That was
this README's first explanation (base 0 + FIFO at 0x18000). The traced 8x run
with explicit AA write-lock tracing shows **zero** LFB write locks in 70 s of
play, and the per-frame `sliCtrl` lines in the 2x trace sit inside depth
clears and state validation (`ICD clear mask 100` ... `done`), not LFB
unlocks. There were no LFB writes for the base to misdirect, and 8x froze with
a correct base. The vendor recipe's base change is kept (it matches 3dfx), but
it is not what the evidence credits.

## 8x (fixed 19:14)

Glide's cfg 8 request: 4 chips, no SLI, `aaSampleHigh` 2, a real secondary
colour buffer at **0x023DC000** (below the primary at 0x031EE000),
`enable2ndbuffer` 1. Before the fix the box froze within ~110 ms of
`winopen: done`, in the ICD's context setup (`pm8x/`). With READ_EN on chips
0/1 (kernel deployed 16:43, boot #57): traced run 70 s play, console, clean
quit, user: "smooth AA, looked right" (`run8x_readen/`, `q2aa8x2.trace`:
`cfgAALfbCtrl` 0xDE3DC000 x2 / 0xCE3DC000 x2; no AA LFB write lock at all).

## Also seen (to do after the multi-game AA testing)

The user: under AA the splash, loading and menu screens are **garbled BEFORE
Quake II's first rendered game frame**; once the game renders, gameplay AND
the menus look right. So it is a start-up state, not AA rendering: candidates
are the ~9 s AA enable with `SliPersistAll` = 1 (the monitor shows un-merged
video memory until the SLI/AA enable finishes), secondary sample buffers not
yet cleared, and the per-chip AA jitter on pixel-exact 2D (the analysis
workflow's explanation - but menus drawn after the first frame look fine,
which argues against it).
