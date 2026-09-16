# Voodoo 5 6000 + AmigaMerlin — editorial dossier for specpicks.com

Running record for the long-form benchmark article: what was measured, how,
what went wrong, and **what was retracted**. Kept as work happens rather than
reconstructed afterwards, because the corrections below were only findable
while the hardware was in front of us and would be invisible in the final
numbers.

**The data tables are GENERATED — never hand-edit them:**

```bash
python3 scripts/benchmarks/v56k_article.py <results.csv> > /tmp/data.md
```

Same rule as `docs/fleet-inventory.md` and `docs/staged-library.md`: a
hand-copied benchmark table goes stale the moment another cell is measured,
and a stale table in an editorial piece is worse than no table.

---

## The headline result

4-chip SLI scaling on a Voodoo 5 6000, Quake III `demo four`, 16-bit, no AA:

| resolution | 1 chip | 4 chips | scaling |
|---|--:|--:|--:|
| 640x480 | 105.3 | 122.8 | **1.17x** |
| 800x600 | 88.0 | 122.3 | **1.39x** |
| 1024x768 | 58.7 | 117.9 | **2.01x** |
| 1280x960 | 38.8 | 114.6 | **2.95x** |
| 1600x1200 | 25.1 | 78.5 | **3.13x** |

This is the story: the four chips buy almost nothing at 640x480 and nearly
quadruple the frame rate at 1600x1200. The card is **CPU-bound until about
1280x960** on a 2 GHz Athlon XP, and only past that does the silicon become
the limit — which is exactly what the published fill-rate analysis for four
VSA-100s predicts (4 x 333 Mpixel/s = 1.33 Gpixel/s; 640x480 at 120 fps with
3x overdraw needs a small fraction of it).

It also means **any single-resolution benchmark of this card is misleading**,
in either direction, and the 1-chip column is the control that proves it.

---

## What is under test

| | |
|---|---|
| card | 3dfx Voodoo 5 6000 AGP, `PCI\VEN_121A&DEV_0009&SUBSYS_0001121A`, 4 x VSA-100, 128 MB |
| driver | **AmigaMerlin 3.1-R11**, `5.1.2605.5`, INF section `3dfxvsV6` |
| host | `.124` — Athlon XP 2400+ @ 2004 MHz, **255 MB**, nForce2, XP SP3 |
| monitor | Gateway VX1120, 4:3 CRT, 40x30 cm, native 1920x1440 @ 75 Hz |
| engine | retail `quake3.exe` 1.32c on the 3dfx ICD, `demo four` timedemo |

`SUBSYS_0001121A` is what identifies the board as a **6000** rather than a
5500 (`0002121A`) — worth stating, because the driver reports both as
"Voodoo5" and the 6000 never shipped commercially.

### The AmigaMerlin files, by hash

Every one byte-identical to the set that ran on the previous host, so the
board swap did not change the software under test:

```
3dfxOGL.dll  2646009  8912a1388a15a8f6b3a75b6a1a344ee9
3dfxvs.dll    610240  95634e870d73a33e64ddd138074f523b
glide2x.dll    94208  a0d0841a178acda0dfc200ef9e31f5db
glide3x.dll   344064  8c376063b95fa9a4d05a03626a2a1e5c
```

### AmigaMerlin's OpenGL is Mesa — worth a paragraph in the article

`3dfxOGL.dll` reports:

```
GL_VENDOR:   Brian Paul
GL_RENDERER: Mesa Glide v0.63 Voodoo5 6000 (tm)
GL_VERSION:  1.2 Mesa 6.3
```

It is **not** 3dfx's own ICD. The community AmigaMerlin package ships a
MesaFX-derived OpenGL on top of 3dfx's Glide, and it is byte-identical to the
file in the official installer — so this is the genuine article, not
contamination. Every "3dfx OpenGL" number here is really *Mesa 6.3 over 3dfx
Glide*, which reframes any comparison against a modern clean-room MesaFX build
as same-lineage, different-vintage rather than third-party-vs-ours.

---

## The configuration axis, read out of the driver

Chip count and FSAA are **one** setting, not two:
`SSTH3_SLI_AA_CONFIGURATION`, under the display class instance's
`Settings\Glide`. The enumeration is not guessed — 3dfx Tools' own descriptor
subkeys each carry a `List` of human labels and a `Tweak Map` of the values
they write (`QuadChipAASLI` = `0,5,6,7,8`, `DualChipAASLI` = `0,2,3,4`,
`SingleChipAASLI` = `0,1`):

| cfg | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| chips | 1 | 1 | 2 | 2 | 2 | 4 | 4 | 4 | 4 |
| AA | – | 2x | – | 2x | 4x | – | 2x | 4x | **8x** |

**cfg 8 is why this card exists**: 8-sample RGSS needs four chips, so no other
card of the era can produce that row.

---

## Method, and why each precaution is there

1. **The AA value is written AND read back** before every run. `REGWRITE`
   answers `OK` for a write that silently created a subkey instead of setting
   a value, so the `OK` is not evidence.
2. **One AA config per clean boot.** See the retractions below. This is
   necessary but, as it turns out, **not sufficient** — cfg 1 wedged inside a
   clean single-config boot — so the harness also has to detect a wedge and
   refuse to attribute it to the next cell.
2b. **The box restarts its own agent.** A wedge takes the agent's process down
   with it, and nothing on a fleet box supervises the agent: the `Run` value
   fires only at logon, so until this session a wedge meant the machine was
   unreachable until somebody walked to it. That is why the first sweep stopped
   after a single measured cell. A Run-key watchdog loop now relaunches the
   agent within 30 seconds — **verified by killing it deliberately: back in
   under 5 seconds, with the restart logged** — which does not fix the wedge
   (only a reboot does) but makes the reboot issuable remotely. A wedge costs
   about three minutes instead of a physical visit, and that is the difference
   between a matrix that can be measured unattended and one that cannot.
2c. **A retry is earned, not automatic.** A config gets another boot only when
   the previous pass actually measured a cell. A wedged pass still writes its
   failed row, so "did we make progress?" has to be counted from rows that
   *succeeded* — otherwise every config buys itself another boot forever.
3. **The renderer string is recorded per run**, from the last renderer init in
   the engine's log. A benchmark that cannot say which driver drew the frames
   is not a driver benchmark.
4. **The display-class instance is resolved, never assumed.** It was `\0000`
   on the previous host and is `\0001` here.
5. **Board health is checked after any failed cell** by asking Glide to
   initialise (`glideprobe --noopen`). Agent liveness is a different question.
6. **The box is quiesced** (AI engine off, Windows Update, `3dfxMan`, tray apps
   killed) — single-core-era hosts lose several fps to any background process.
7. **Resolutions match the monitor.** It is a 4:3 tube, so the high mode is
   **1280x960, not 1280x1024** — 5:4 on a 4:3 CRT is geometrically wrong and
   would also be a different pixel count.

---

## Retractions and corrections

The article should carry these; they are more interesting than the numbers.

### 1. "cfg 2 and cfg 8 are box-killers" — WRONG

Both hung the display driver and took the agent down, so they were recorded as
bad configurations. Then the card moved hosts and **cfg 5 — the default, which
had already produced 150 fps — hung identically.** Lining every hang up by its
position within its boot session:

| session | sequence | outcome |
|---|---|---|
| host A | cfg 0 ok → cfg 5 ok | 2 changes, fine |
| host A | cfg 0 ok → cfg 1 ok → **cfg 2 HUNG** | 3rd change |
| host A | cfg 5 ok → **cfg 8 HUNG** | 2nd change |
| host B | cfg 0 ok → cfg 1 ok → **cfg 5 HUNG** | 3rd change |
| host B | after reboot, cfg 5 ok | 1st, fine |

**No configuration is inherently bad.** Writing
`SSTH3_SLI_AA_CONFIGURATION` repeatedly within one boot wedges the driver, on
the second or third write. A *resolution* change is not a topology change —
five resolutions at one config run back to back cleanly.

Consequence: everything measured after a hang was measured against broken
hardware, which produced **six false "wedges the driver" verdicts in a single
screening run**. And the reason it was not caught immediately: **agent
liveness is not board liveness.** The agent survived every wedge; only the
graphics subsystem died, so a liveness check that pings the agent sails
straight past it.

### 0b. THE CHIP-COUNT LABELS ARE NOT ESTABLISHED EITHER

The same Tweak Map that produced the AA labels produced the chip-count labels,
and the AA half is now known not to take effect — so the chip half cannot be
taken on trust. The measurement says it is wrong:

| resolution | cfg 0 | cfg 2 | cfg 5 | cfg 2 ÷ cfg 0 | cfg 5 ÷ cfg 0 |
|---|--:|--:|--:|--:|--:|
| 640×480 | 97.4 | 124.7 | 108.1 | 1.28× | 1.11× |
| 800×600 | 88.1 | 140.1 | 120.2 | 1.59× | 1.36× |
| 1024×768 | 58.6 | 136.1 | 116.9 | 2.32× | 1.99× |
| 1280×960 | 38.8 | 134.5 | 115.4 | **3.47×** | 2.97× |
| 1600×1200 | 25.1 | 87.0 | 78.7 | **3.47×** | 3.14× |

**cfg 2 was labelled "2 chips" and it reaches 3.47× a single chip.** Two chips
cannot do that. And cfg 2 beats cfg 5 — the config labelled "4 chips" — at
every single resolution, including the fill-limited ones where more chips must
win if they are really there.

So exactly one of these is true, and this campaign has not yet distinguished
them:

1. the labels are permuted, and cfg 2 is the real four-chip mode;
2. cfg 0 is not a single chip, which would make every scaling ratio in this
   document wrong by a constant factor;
3. both cfg 2 and cfg 5 drive four chips and differ in something else (SLI band
   height is the obvious candidate — `FX_GLIDE_SLI_BAND_HEIGHT` and
   `FX_GLIDE_FORCE_SLI_BAND_HEIGHT` both exist in `glide3x.dll`), with cfg 5
   simply the less efficient arrangement.

**What IS measured, and is safe to publish:** one setting of this card runs
Quake III at 25.1 fps at 1600×1200 and another runs the identical workload at
87.0 — a **3.47× spread from a registry value**, with the renderer string
recorded on every row. That is the real headline, and it does not depend on
knowing how many chips each mode lights up.

**What must NOT be published until it is verified:** any sentence of the form
"two chips give X and four chips give Y". `GR_NUM_FB` cannot settle it — it
reports chips *present*, not chips *ganged* (see retraction 2 below).

### 0. THE AA AXIS IS NOT MEASURED — the card never anti-aliased

**This is the most important correction in the file, and it invalidates every
AA number this campaign has produced so far.** It belongs near the top of the
article, not in a footnote.

`SSTH3_SLI_AA_CONFIGURATION` writes, reads back, survives a reboot — and
changes nothing at all. Two independent measurements, either one sufficient:

| evidence | 4 chips, no AA (cfg 5) | 4 chips, 4× AA (cfg 7) |
|---|---|---|
| Quake III 1024×768 screenshot, md5 | `6361acde…a18a` | **`6361acde…a18a`** |
| pixel difference, per channel | — | **(0,0) (0,0) (0,0)** |
| fps cost of 2× AA on one chip at 1024×768 | 58.6 | **58.2 — 0.99×** |

The frames are **byte-identical**. And at a fill-bound resolution, 2× AA costs
**nothing** — where real 2× AA must roughly halve fill rate. A third run with
`FX_GLIDE_AA_SAMPLE=4` added produced the **same md5 again**: three settings,
one image.

This is not a capture artefact. The screenshots are the *engine's own*, from a
deterministic `demo four` frame — and a deterministic frame rendered with
identical settings *should* be byte-identical, which is exactly why identical
images prove identical rendering.

**The harness said these rows were fine, and it was asking the wrong
question.** The column was called `aa_verified` and it meant *"the registry
value read back"*. That proves the write landed; it says nothing about whether
the driver acted on it. It now records `reg-readback-only`, and each AA cell is
checked against its matching no-AA cell — a cell that costs nothing is reported
as **AA not applied** rather than as a remarkable free feature.

**Where the setting was supposed to go**, from strings in the installed
binaries — and it is not one value:

- `glide3x.dll` reads `SSTH3_SLI_AA_CONFIGURATION` and also carries
  `FX_GLIDE_AA_SAMPLE`, `FX_GLIDE_AA_CLIP`, `FX_GLIDE_FORCE_OLD_AA`, the full
  `FX_GLIDE_AA{2,4,8}_OFFSET_{X,Y}n` sub-pixel jitter tables, and
  `grTBufferWriteMaskExt` — the T-buffer entry point.
- Glide **asks the miniport** for the configuration rather than applying it
  itself: `hwcInitVideo: HWC_MINIVDD_HACK: ExEscape:HWCEXT_SLI_AA_REQUEST`.
- The display driver `3dfxvs.dll` references the value **six** times and owns
  the *enables*: `SSTH3_ANTIALIAS`, `SSTH3_DIGITAL_SLI_AA`,
  `SSTH3_AA_ENABLE_OUTOFMEMORY`, `SSTH3_AAJITTER_FORCEFLAG`, plus per-chip
  dither-matrix selectors (`SSTH3_DITHMATSEL_4SMPL_CHP0` …
  `_8SMPL_CHP2`) — which is what four chips cooperating on eight samples would
  need.
- `3dfxOGL.dll` — the ICD actually in use, 2,646,009 bytes, reporting
  `GL_VENDOR: Brian Paul`, `GL_VERSION: 1.2 Mesa 6.3` — references the value
  exactly **once**.

So the configuration value *selects* a mode and something else *enables* it,
and the live key holds only the configuration (plus `FX_GLIDE_ANALOG_SLI=1`).
That hypothesis — a Mesa-derived ICD that never issues the T-buffer request,
leaving FSAA reachable from native Glide — has now been **tested and refuted.**

UT99 was run through `GlideDrv.GlideRenderDevice`, with no OpenGL anywhere in
the path, reading the engine's own on-screen `stat fps` overlay off its own
screenshot:

| configuration | FRAME | RENDER | polys |
|---|--:|--:|--:|
| cfg 5 — no AA | 6.9 ms | 5.4 ms | 058 |
| cfg 8 — **8× AA** | 6.6 ms | **5.1 ms** | 056 |

**8× AA renders faster than no AA.** Eight samples cannot be free. And the
polygon edges in that 8× frame are hard staircases with no intermediate
shading — 78% of edge columns across a high-contrast boundary carry no blended
pixel at all.

So it is not the ICD. **On this card with AmigaMerlin 3.1-R11, FSAA does not
work through either rendering path**, and the flagship feature of the flagship
3dfx card is simply absent — which is the story, and a far better one than a
table of AA frame rates would have been.

Which is a better story than an AA benchmark table. The Voodoo 5 6000's whole
reason to exist is T-buffer anti-aliasing, and the community driver everyone
recommends appears to be unable to deliver it to an OpenGL game.

### 1b. ...and "it is the count of topology writes" is ALSO not the whole story

The count theory above explained four hangs and then failed on the fifth. cfg 1
(1 chip, 2x AA) was given a clean boot and exactly one topology write, ran
640x480 fine at 106.9 fps, and then **wedged on its second cell** - 800x600 at
the same config. Meanwhile cfg 0 and cfg 5 each completed all five resolutions
in a single boot without trouble.

So the honest state of knowledge is:

- **Established:** the AmigaMerlin driver wedges under repeated Glide context
  creation, the wedge takes the retro agent's process down with it, and the
  machine then needs attending. cfg 0 and cfg 5 are the only configurations
  that have completed a full five-resolution run.
- **Not established:** which variable predicts it. "That config is bad" is
  refuted (cfg 5 works from a clean boot). "The number of topology writes" is
  refuted (cfg 1 died on one write). "Any AA enabled" fits cfg 1 and cfg 8 but
  not cfg 2 or cfg 5, both of which are no-AA.

This is worth reporting in the article exactly as it stands. **Driver
instability under repeated mode setup is itself the finding** - it is the
reason a 2000-era halo card with four chips is hard to benchmark at all, and
guessing at a mechanism we have not isolated would be the same error as the
retraction above, one level up.

### 2. `GR_NUM_FB` is chips PRESENT, not chips ganged

It reads 4 on this card for *every* configuration, including "Single Chip
Only". It cannot be used to confirm that a topology change took effect — the
fps delta is the only evidence. An earlier note here claiming "4 chips ganged"
from that field was wrong.

### 3. `/F` in `awdflash` is not "force"

Tangential to the benchmarks but it came up recovering a bricked host, and the
web is confidently wrong about it: `/F` = *"Use Flash Routines in Original
BIOS For Flash Programming"*. Also `/tiny` destroys the BIOSLock signature,
and `/QI` is an extra part-number check rather than a force. All three read out
of the binary's own help text.

### 4. The benchmark harness recorded an FSAA level it never applied

The pre-existing fleet runner iterated an `fsaa` axis and, at the point of
use, only logged *"apply via the card's driver profile if configured"* — then
wrote the requested level into the CSV. On a card whose entire selling point is
RGSS anti-aliasing, that would have produced an article's worth of "4x" rows
rendered with no AA at all. This is the single most important reason the data
here is trustworthy and the reason the read-back in method (1) exists.

---

## Hardware and driver facts discovered along the way

- **XP ships an in-box Voodoo5 driver** (`3dfxvs2k.inf`, `5.1.2001.0`, 2001)
  that provides **no Glide and no OpenGL ICD at all**. A box on it shows an
  empty `OpenGLdrivers` key and a "3dfx device not found" dialog, and Glide
  hangs in `grGlideInit()`. Installing AmigaMerlin fixes it — a clean A/B where
  only the driver changed.
- **`OpenGLdrivers\<vendor>` is the leftover that breaks a card swap.** A box
  that previously had an NVIDIA card keeps `RIVATNT` there, and every OpenGL
  app tries that ICD regardless of which GPU is fitted.
- **On an nForce board, "remove all NVIDIA drivers" is a trap.** The chipset
  (`Class=System`) and audio (`Class=Media`) INFs are *also* NVIDIA-provided.
  Only the `Class=Display` one may go, and the file list should come from that
  INF's own `[SourceDisksFiles]` rather than an `nv*` glob, which would also
  catch `nvata`/`nvraid` storage drivers.
- **A ghost devnode has no `Control` subkey**; a live one does. That is the
  cheap discriminator for "is this card still physically fitted".
- **Game-local DLLs beat system32.** Two staged titles shipped a 1,310,720-byte
  nGlide `glide2x.dll`, which on the one machine with real Glide silicon
  guarantees the card is never used. Retiring those is part of benchmarking
  honestly.

---

## Screenshot / image-quality pass

fps alone undersells an AA card, so the article carries matched-scene captures
at each AA level. Four constraints, three of which are engine facts that cost
something to learn:

- **The engine takes the picture, not the agent.** A Glide exclusive-fullscreen
  surface is precisely the case GDI cannot be trusted for — the board renders
  into its own framebuffer and what `SCREENSHOT` hands back is not necessarily
  what was scanned out. A resampled or mis-paletted capture used to judge
  anti-aliasing would be worse than no screenshots: it would look like evidence.
- **A fixed frame of a demo**, via `demo <name>` + `wait <N frames>` +
  `screenshot`. `wait` counts FRAMES, not seconds, which is the whole trick:
  the same N is the same viewpoint whether the card is managing 25 fps or 120.
  A shot taken "a few seconds in" lands on a different frame each run, and
  edge-quality differences are far subtler than scene differences.
- **Each engine's dialect is its own.** Quake III's latched cvars
  (`r_mode`, `r_customwidth`, `r_colorbits`) must be set *before* `R_Init`, not
  in the file the command line execs — the `vid_restart` that would otherwise
  be needed is what hung the driver solid at 4 chips / 8× AA. Quake II has no
  `r_mode -1` at all, and its `wait` **takes no argument and delays exactly one
  frame**, so the id Tech 3 spelling would have photographed the opening frame
  of the demo at every AA level and produced a complete, plausible, entirely
  wrong set of images. RtCW's id Tech 3 fork has no `r_mode -1` branch either:
  it renders 640×480 rather than erroring, so it is given a real mode index and
  refuses an off-table resolution instead of quietly rounding it.
- **One resolution is enough**, because edge quality at a fixed resolution is
  the point — so a single boot per config captures every game with no extra
  topology changes.

RtCW ships no demo, so its fixed scene is a map spawn point instead: same map,
same spawn, player standing still. The 3dfx driver also exposes a
`Screen Capture Hotkey` in its Glide settings which captures the actual Glide
framebuffer — the fallback if an engine's own capture proves unreliable.

## Open questions

- 16-bit vs 32-bit at matched settings: the one published test of this card's
  own reference driver reported only a **1.03x** gain for halving colour depth
  and called it a driver defect, where community drivers got **1.20x**. Worth
  measuring here.
- UT99 and Deus Ex on **native Glide** vs the OpenGL path on the same card.
- Do not compare these fps against the previous host's. Same driver, same card,
  different machine: the earlier host was an XP 2600+ @ 1921 MHz with 511 MB
  and gave 150.2 fps at 640x480 where this one, at a *higher* 2004 MHz, gives
  122.8. The 255 MB of RAM here is the obvious suspect. Only within-box
  comparisons are valid.
