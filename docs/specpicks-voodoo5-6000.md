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
shading. The quantified figure — **78% of edge columns across a high-contrast
boundary carry no blended pixel** — was measured on the *4×-AA-requested* UT99
frame (cfg 4, terrain horizon against sky, the cleanest two-tone edge captured);
`docs/evidence/voodoo5-6000-fsaa/edge_stats.py` reproduces it.

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

## Published — 2026-09-16

The first instalment is live on specpicks.com as a six-part SpecPicks Retro Lab
series (`series_slug` `voodoo5-6000-strange-god`, category `testbench`, tag
`specpicks-lab`), written from this dossier and the published dataset:

1. https://specpicks.com/reviews/voodoo5-6000-strange-god-part-1-history-2026
2. https://specpicks.com/reviews/voodoo5-6000-strange-god-part-2-test-method-2026
3. https://specpicks.com/reviews/voodoo5-6000-strange-god-part-3-quake3-benchmarks-2026
4. https://specpicks.com/reviews/voodoo5-6000-strange-god-part-4-fsaa-2026
5. https://specpicks.com/reviews/voodoo5-6000-strange-god-part-5-bricked-board-2026
6. https://specpicks.com/reviews/voodoo5-6000-strange-god-part-6-verdict-next-2026

The measurements are in the site database — `hardware_specs` row
`3dfx-voodoo5-6000`, 20 `gaming_benchmarks` rows whose `quality_preset` is the
driver's quoted label and whose `source_name` shows the host, rendered at
https://specpicks.com/benchmarks/3dfx-voodoo5-6000 — and in the fleet ledger
(`retro_benchmark_runs`, source `specpicks-lab-v56k`, 23 rows). The dataset
behind every figure is published at
https://nscagentstorage.blob.core.windows.net/article-images/voodoo5-6000-strange-god/dataset/
(results CSVs, versions.json, this dossier, the evidence README and the edge
metric). Loader and publisher: `specpicks/scripts/lab/`.

## Where testing stands — 2026-09-16 (checkpoint for the specpicks review)

This is the state the first published instalment of the review describes.
Everything below the line "Not yet run" is the second instalment.

### The card and the two hosts

- **The card:** a modern "Strange God" AGP reproduction of the 3dfx Voodoo 5
  6000 — four VSA-100s, identifying itself to Windows as
  `PCI\VEN_121A&DEV_0009&SUBSYS_0001121A` (`0001121A` is the 6000; a 5500 is
  `0002121A`), behind a HiNT bridge `VEN_3388&DEV_0021`. It carries a **128 MB
  / 256 MB mode switch**; every number here was taken in the mode it shipped
  in and the switch has **not** been exercised (see "Not yet run"). The driver
  reports 32 MB, which is per-chip framebuffer as Glide sees it, not the card.
- **Host 1 — `.191`:** EPoX EP-8RDA+ (nForce2), Athlon XP 2600+ @ 1921 MHz,
  511 MB, XP SP3. Produced four verified Quake III cells at 640×480 (below)
  before its BIOS was corrupted mid-campaign; a bootblock floppy recovery was
  built (`provisioning/bios-recovery/`) but the board was written off. **Its
  numbers are not comparable with host 2's** — same card, same driver files
  byte-for-byte, different machine.
- **Host 2 — `.124`:** Athlon XP 2400+ @ 2004 MHz (no SSE2), **255 MB**, XP SP3,
  chipset not in the published hardware record (lab notes said nForce2 — unverified),
  hostname `NSC-C543575F526`, agent 1.81.1. Every table in this document that
  is not explicitly marked `.191` is from this host.

### Software under test, exactly

| component | identity |
|---|---|
| driver package | `AMIGAMERLIN 3.1-R11 For Voodoo 5 6000 AGP`, DriverVersion `5.1.2605.5`, DriverDate 6-9-2005, `oem2.inf`, provider string "3dfx Interactive, Inc." |
| `glide3x.dll` | 344,064 B, md5 `8c376063b95fa9a4d05a03626a2a1e5c` |
| `glide2x.dll` | 94,208 B, md5 `a0d0841a178acda0dfc200ef9e31f5db` |
| `3dfxOGL.dll` (the OpenGL ICD) | 2,646,009 B, md5 `8912a1388a15a8f6b3a75b6a1a344ee9` — reports `GL_VENDOR: Brian Paul`, `GL_RENDERER: Mesa Glide v0.63 Voodoo5 6000 (tm)`, `GL_VERSION: 1.2 Mesa 6.3` |
| `3dfxvs.dll` (display driver) | 610,240 B, md5 `95634e870d73a33e64ddd138074f523b` |
| `3dfxvsm.sys` (miniport) | 174,720 B, md5 `83ee5503255fd9f9c2291253221cb466` |
| Quake III Arena | retail `quake3.exe` 1.32c, 872,448 B, md5 `b5cf3dd55e045aac6096ff97379d0cab`, `demo four`, 16-bit |
| Unreal Tournament | 436, `UnrealTournament.exe` 241,664 B, md5 `7dadcc7e3a9a3d66001e3b37cf636f0e`, `GlideDrv.GlideRenderDevice` |

### Measured and publishable (host 2, `.124`)

Quake III `demo four`, 16-bit, average fps, renderer string recorded on every
row (`scripts/benchmarks/results/v56k_sweep_192.168.1.124/results.csv`):

| driver setting | label the driver gives it | 640×480 | 800×600 | 1024×768 | 1280×960 | 1600×1200 |
|---|---|--:|--:|--:|--:|--:|
| cfg 0 | "Single Chip Only" | 97.4 | 88.1 | 58.6 | 38.8 | 25.1 |
| cfg 2 | "Dual Chip, no AA" | **124.7** | **140.1** | **136.1** | **134.5** | **87.0** |
| cfg 5 | "Quad Chip, no AA" | 108.1 | 120.2 | 116.9 | 115.4 | 78.7 |

Repeatability, same host, cfg 0 and cfg 5 measured on two different days
(first run → re-run): 640×480 **105.3 → 97.4** and **122.8 → 108.1**; every
other cell within 1.7% and three within 0.1 fps (800×600 88.0 → 88.1,
1280×960 38.8 → 38.8, 1600×1200 25.1 → 25.1). The CPU-bound cell is the noisy
one; the GPU-bound cells repeat.

**Resolved 2026-09-16 — it was background load, not noise.** With the box
quiet (the agent watchdog's 30-second `tasklist` loop paused, and the
"Found New Hardware Wizard" that XP raised after every boot suppressed for
good), 640×480 repeated within 2%: cfg 0 **117.5 and 116.3**, cfg 5 **119.4
and 121.6** (`results/v56k_repeat_nowd_192.168.1.124/`). The published
re-run values for that cell — cfg 0 97.4, cfg 5 108.1 — were taken with the
watchdog loop running and are 10–17% low; the first-run cfg 5 value (122.8,
taken before the watchdog existed) matches the quiet figure. GPU-bound cells
showed no such sensitivity. A second variable rides with it: **the first cell
after a boot reads low** — cfg 2 at 640×480 gave 105.9 as the first cell
after its boot and 117.5 two minutes later in the same boot (the agent's
startup threads and XP's post-logon work), and every 640×480 cell in the
first published sweep was a first-cell-after-boot. Quiet, settled, all three
settings meet at 640×480: cfg 0 ≈ 117, cfg 2 117.5, cfg 5 ≈ 120 — the cell is
purely CPU-bound and says nothing about the card. The watchdog is now a
filtered check every 90 s, the sweep settles 150 s after each boot and runs
resolutions high to low, and the 640×480 cells were re-measured quiet for
every setting before being republished. Lesson: on a single-core host the
CPU-bound cell measures everything else that is running.

Host 1 (`.191`), Quake III 640×480 only: cfg 0 = 126.6 and 127.8 (two runs),
cfg 5 = 150.2, cfg 1 ("Single Chip, 2× AA") = 126.9. Note that even there the
"2× AA" cell equals the no-AA cell — the FSAA finding was already in the data
before anyone looked for it.

### Established beyond the numbers

1. **FSAA does not engage on this card with this driver, through either
   rendering path** (retraction 0, above). Not "looks similar": byte-identical
   frames and no frame-time cost, on OpenGL and on native Glide.
2. **The driver's chip-count labels do not describe what the card does**
   (retraction 0b). A registry value takes 1600×1200 from 25.1 to 87.0 fps —
   3.47× — which is the publishable headline, label-free.
3. **The wedge is real and mechanical.** Under repeated Glide context creation
   the display driver hangs; sometimes it kills the agent process (a Run-key
   watchdog now restarts it in ~30 s), sometimes it hangs the box with the game
   on screen and needs a power cycle. One AA config per clean boot is
   necessary and not sufficient.

### Not yet run — the second instalment

- **128 MB vs 256 MB mode.** The card's switch has not been touched in this
  campaign. It was flipped once before it, on 2026-08-12, on a third machine
  (`.133`, dual Pentium III) under the lab's in-house H5 driver stack:
  `HardwareInformation.MemorySize` went `0x08000000` → `0x10000000` (64 MB per
  chip), Quake III at 1024×768 read 61.4 fps either way, and UT99's Glide
  texture space rose from 15.9 MB to 32.4 MB (`retro-3dfx/V56K-SLI-FINDINGS.md`
  §12/§14). Under AmigaMerlin, on either campaign host, the mode is untested;
  whether that driver even exposes the difference (texture memory per chip,
  `FX_GLIDE_FBRAM`) is the first question.
- **Other driver stacks on the same card:** SFFT, the official 3dfx
  1.04.00 beta, and the two in-house stacks (`voodoo-cleanroom/` MesaFX +
  open Glide; the vintage H5 source tree). AmigaMerlin was chosen first
  because it is the community default; whether FSAA works on *any* of them is
  now the central question.
- **Making FSAA engage at all** — the 3dfx Tools control-panel route, and the
  display-driver enables the binaries name (`SSTH3_ANTIALIAS`,
  `SSTH3_DIGITAL_SLI_AA`). If a route exists, every AA cell (cfg 1, 3, 4, 6, 7,
  8) is then measurable; today they are all measurements of AA being ignored.
- **Resolving the chip labels** — SLI band height (`FX_GLIDE_SLI_BAND_HEIGHT`)
  is the leading explanation for why cfg 2 beats cfg 5.
- **32-bit colour** at matched settings.
- **The other titles** the harness already knows how to drive: Quake II
  (MiniGL), GLQuake, RtCW, Serious Sam TFE/TSE, Unreal Gold and Deus Ex on
  native Glide vs OpenGL, and a Direct3D title.
- **UT99 frame rates as a table** — the engine's `-benchmark` never exits on
  this build; the working route is its on-screen stat overlay read off its
  own screenshot, which is a number per capture rather than a timedemo.
- **Host 1's board**, if it is repaired: a within-box comparison of the two
  memory modes on two hosts.

---

## What this campaign taught us — the learnings page

Each of these cost time; several cost a trip to the machine.

1. **Agent liveness is not board liveness.** Six of nine "this configuration
   wedges the driver" verdicts were measurements of a board that was already
   wedged. Check board health after any failure, and never attribute a hang to
   the cell that happened to be running.
2. **A value that reads back is not a feature that works.** `aa_verified`
   meant "the registry value read back" and read as "the card anti-aliases".
   The only honest post-condition is that the *rendering changed* — an fps
   delta against the matching no-AA cell, or a pixel difference.
3. **Free AA is not a feature, it is a bug in the measurement.** Anti-aliasing
   costs fill rate by construction. A cell that retains ≥97% of the no-AA
   frame rate is AA being ignored, and the table now says so.
4. **A driver's own labels are claims, not facts.** The same Tweak Map that
   produced the AA labels produced the chip-count labels, and the AA half was
   demonstrably inert. Publish only what the measurement supports: the spread,
   not the chip count.
5. **One topology write per boot, and it is still not enough.** The reboot is
   what clears the wedge; the retry is what makes the sweep finish; and a
   retry is only earned by a pass that measured something.
6. **The box must restart its own agent.** Nothing supervised the agent; every
   crash was a physical visit. A 30-second Run-key loop turned that into three
   minutes — and a hang that leaves the game on screen still needs a person.
7. **A results path that can vanish is worse than none.** Fourteen measured
   rows were lost to a session scratchpad. Results now refuse a `/tmp` outdir,
   and the evidence images live under `docs/`.
8. **Record what was running, on the row.** Game exe md5, driver package and
   version, Glide and ICD hashes, OS, agent — per row, not only in a sidecar.
   AmigaMerlin ships rebranded 3dfx binaries whose version resources lie;
   only a hash distinguishes two builds. A retroactive probe fills only empty
   cells and says so in the row.
9. **The engine takes the picture.** The agent's GDI capture of a Glide
   exclusive-fullscreen surface returns dark noise. `screenshotJPEG` in Quake
   III and `Shot` in UT99 are the only trustworthy frames.
10. **A deterministic scene is the hard part, and the shortcuts fail
    quietly.** `?quickstart=true` spawns at a random PlayerStart (74% of
    pixels differed for reasons unrelated to AA); a demo photographed too
    early is the loading screen, a 2D blit identical under every setting and
    therefore a vacuous pass that looks decisive. Look at the image.
11. **Edge quality is scene-independent; a pixel diff is not.** When the
    scene cannot be pinned, judge the staircase.
12. **UE1 accepts synthetic keystrokes in exclusive fullscreen; id Tech 3 does
    not.** A rule measured on one engine does not generalise. The `stat fps`
    overlay, read off the engine's own screenshot, is a measurement channel
    when no log can be parsed.
13. **UE1 leaves `System\Running.ini` behind when it is killed**, and the next
    launch stops on a modal Recovery Mode dialog with a 0-byte log — which
    looks exactly like a renderer failure and was misread as one.
14. **Every engine has its own dialect.** Quake II's `wait` takes no argument
    and delays one frame; RtCW's fork has no `r_mode -1`; Quake III's latched
    cvars must be set before `R_Init`. Copying one engine's recipe into
    another produces a complete, plausible, wrong set of images.
15. **Read the binary, not the forum.** `awdflash /F` is "use the flash
    routines in the original BIOS", the opposite of "force". `glide3x.dll`'s
    strings named the FSAA mechanism (`ExEscape:HWCEXT_SLI_AA_REQUEST`) in a
    minute.
16. **Do not compare hosts.** The same card and byte-identical driver gave
    150.2 fps on one machine and 122.8 on another at the same setting. Only
    within-box comparisons are valid.

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
