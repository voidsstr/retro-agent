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
2. **One AA config per clean boot.** See the retraction below — this is not
   fastidiousness, it is the only way to get trustworthy numbers.
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

## Screenshot / image-quality pass — planned

fps alone undersells an AA card, so the article needs matched-scene captures at
each AA level. Design constraints already known:

- **Use the engine's own screenshot command, not the agent's GDI capture.**
  A Glide exclusive-fullscreen surface is the case where GDI garbles; the
  engines all have `screenshot` (Q3/Q2/RtCW) or `SHOT` (UT99).
- **Capture a deterministic frame**, e.g. `demo four; wait <n>; screenshot`, so
  the same scene is compared across AA levels rather than a random frame.
- One resolution is enough for quality comparison (edge quality at a fixed
  resolution is the point), so a single boot per config can capture several
  games — no extra topology changes.
- The 3dfx driver also exposes a `Screen Capture Hotkey` in its Glide settings,
  which captures the actual Glide framebuffer — a fallback if an engine's own
  capture proves unreliable.

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
