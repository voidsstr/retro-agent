# `requires.json` — the staged-title requirement schema (v1)

One file per title, at `Games-Library/<Title>/requires.json`. It is read by two
independent implementations that must agree:

* the **agent**, in C, with nothing but the staged tree in front of it
  (`agent/shared/gamegate.h`) — a freshly PXE-imaged box syncs its games before
  any host tool has ever seen it, so the gate has to work with no host at all;
* the **host**, in Python (`scripts/gamegate/rules.py`), which plans and
  explains the whole fleet without waking a single machine.

`tests/python/test_gamegate_mirror.py` compiles the C header and compares every
answer against the Python one, so the two cannot drift.

---

## The contract in one paragraph

**Absent data never blocks a title.** A missing file, an unparsable one, a field
left out, an unclassifiable GPU, a clock that could not be measured — all
deploy. The gate blocks only on *positive evidence* that a machine cannot run
something. Fail-closed would produce a box that silently receives no games and
says nothing about why, which is worse than a box that receives one game too
many.

---

## Minimal file — and why you should still write one

```json
{
  "requirements_version": 1,
  "title": "Hexen II",
  "year": 1997,
  "notes": "Quake engine with a software renderer fallback. No meaningful floor on this fleet - verified."
}
```

This states **"somebody checked and there is no floor"**, which is a different
fact from "nobody wrote one" and the schema keeps them apart: the host reports a
title with no file as `missing` and this one as `no-floor (checked)`. Four of
the six titles you just staged are this shape.

## Full file

```json
{
  "requirements_version": 1,
  "title": "Battlefield 1942",
  "year": 2002,

  "min_cpu_mhz": 500,
  "min_ram_mb": 128,
  "min_vram_mb": 32,
  "disk_mb": 1800,

  "gpu_feature_level": "tnl",
  "cpu_features": ["mmx", "sse"],
  "min_os": "winxp",
  "max_os": "winxp",

  "requires_capabilities": ["disc_mount"],

  "notes": "EA minimum is PIII 500 / 128 MB / a 32 MB hardware-T&L card. SafeDisc 2 on Mods\\bf1942\\Mod.dll plus an EA drive walk matching ProductGUID in <X>:\\setup.ini.",

  "shortcuts": {
    "Play Battlefield 1942.bat": {
      "requires_capabilities": ["disc_mount"]
    },
    "Host Battlefield 1942 - LAN.bat": {
      "requires_capabilities": []
    },
    "Join Battlefield 1942 - LAN.bat": {
      "requires_capabilities": []
    }
  }
}
```

---

## Fields

| key | type | meaning |
|---|---|---|
| `requirements_version` | int | bump when you change the numbers. It is part of the host's cache key, so bumping it invalidates every cached verdict for the title — which is exactly what you want after a correction. |
| `title`, `year`, `notes` | string / int / string | not gated on; `notes` and `year` are handed to the LLM for the borderline cases, so write them for a reader who knows games but not this fleet. |
| `min_cpu_mhz` | int | published minimum clock. |
| `min_ram_mb` | int | published minimum system RAM. |
| `min_vram_mb` | int | published minimum video RAM. |
| `disk_mb` | int | installed size in MB. A **hard floor with no margin band** (a tree either fits or it does not, and 90% of Far Cry is not a playable game), checked *after* the cpu/ram/vram floors so a box that cannot run the title is told that rather than sent to free up space it would then waste. Fails open when the agent could not measure free space. GAMESYNC's own room check still backstops it with the real tree size; this exists to refuse the copy **before** the bandwidth is spent. |
| `gpu_feature_level` | enum | `none` · `fixed` · `tnl` · `sm1.x` · `sm2.0` · `sm3.0`. **Ordered.** See below. |
| `cpu_features` | string[] | `fpu` `mmx` `cmov` `sse` `sse2` `sse3` `ssse3` `sse4.1` `3dnow`. Instructions the binary *executes*, not ones it prefers. |
| `min_os` / `max_os` | enum | `win9x` `win2k` `winxp` `vista` `win7` `win8` `win10`. Ordered. |
| `requires_capabilities` | string[] | currently only `disc_mount`. See below. |
| `shortcuts` | object | per-shortcut overrides, keyed by the **first column of `launch.txt`**. |

Unknown keys are ignored. An unknown *enum value* reads as "no opinion" rather
than as level 0 — so a typo degrades to not gating, never to gating wrongly.

---

## `gpu_feature_level` — the axis that actually separates this fleet

VRAM megabytes are not the discriminator; **hardware T&L is**. `.171`'s Intel
82865G has 3D acceleration and no hardware T&L at all, and any rule that only
counts megabytes passes it and is wrong.

| level | means | fleet examples |
|---|---|---|
| `none` | **no 3D pipeline at all** | S3 Trio64, Matrox Millennium, Tseng ET4000, Cirrus GD54xx, Trident TGUI — the Pentium-1 **Compaq Deskpro** class |
| `fixed` | fixed-function rasteriser, **no hardware T&L** | every 3dfx Voodoo, RIVA TNT2, **Intel 865G (.171)**, S3 ViRGE/Savage, Rage 128 |
| `tnl` | DX7 hardware T&L, no programmable shaders | **GeForce2 GTS (.124)**, GeForce4 **MX**, Radeon 7x00 |
| `sm1.x` | DX8 shaders | GeForce3, GeForce4 **Ti**, Radeon 8500 |
| `sm2.0` | DX9 shaders | GeForce FX, Radeon 9500+, Intel 915/945 |
| `sm3.0` | SM3.0 and everything after | GeForce 6xxx+, **8400 GS (.145)**, Radeon X1000+ |

GeForce4 **MX** is `tnl`, not `sm1.x` — it is a DX7 part wearing a DX8 part's
name. Getting that one wrong hands it every shader title on the shelf.

**A one-level shortfall is `marginal`** (many titles of that era ship a
lower-detail path) and is the *only* thing that reaches the LLM. **Two levels or
more is a flat `no`**, decided by arithmetic.

### `none` is the one exception to the marginal band

**A box at `none` against ANY declared floor is a flat `no`, even though that
is only one level below `fixed`.** The marginal band exists because a title
usually ships a *lower-detail* path for a weaker rasteriser; there is no
lower-detail path from "has a rasteriser" to "has none". Aliens versus Predator
is the title this protects — its own note reads "Direct3D only, there is no
software renderer, so a 3D accelerator is mandatory rather than optional".

Until 2026-08-30 nothing was ever classified `none`: every S3, Matrox, Trident,
Cirrus and Tseng id fell through one vendor-wide catch-all at `fixed`, so a
Trio64 — a chip with no 3D pipeline whatsoever and no Direct3D driver ever
written for it — claimed a fixed-function rasteriser. The level existed and was
unreachable. Only `min_vram_mb` happened to catch some of the 3D-only titles on
such a box, which is luck, not a gate.

### STATE A FLOOR ONLY WHEN THE TITLE CANNOT START BELOW IT

`gpu_feature_level` means **"will not launch below this"**, never "runs better
above this". Nine staged titles used to state `fixed` while their own notes
said a software renderer shipped — harmless while every box was `fixed` or
better (a gap of zero), and a hard `no` the moment a `none` box arrived.

So before writing this field, look in the tree for the software renderer and
name the file you found in `notes`: `sw.dll` (GoldSrc), `System\SoftDrv.dll`
(Unreal Engine 1), `soft.ren` (LithTech), `Carma2_SW.exe`. If one is there and
the launcher does not force a 3D device, **state no `gpu_feature_level` and no
`min_vram_mb`** — both describe the 3D path — and let the cpu/ram floors
describe the software path, which is what decides whether copying is worth it.
A title with two launchers says it per-shortcut instead: Hexen II puts `fixed`
on the three `glh2.exe` shortcuts and nothing on the `h2.exe` one, so a 2D box
receives the game and loses only the OpenGL icon.

---

## `cpu_features` — how to tell a real floor from a fast path

An absent instruction is `#UD` — an immediate hard crash — so this field is the
most valuable one in the schema and the easiest to get catastrophically wrong in
**both** directions. UT99 469e is the worked example: it declared only a clock
and a RAM figure, and the gate cheerfully approved a build compiled with SSE2
throughout onto three boxes that die on it with `0xC000001D`, misread as a
broken GAMESYNC about thirty times.

The opposite mistake is just as bad and much easier to make, because almost
every game of this era **detects the CPU at runtime and picks a path**. Declare
a floor from a raw instruction count and you refuse a title on machines that
run it perfectly.

**Counting instructions is the start of the answer, never the end.** Disassemble
the binaries the launcher actually loads and then apply three discriminators:

| discriminator | reads as | what it means |
|---|---|---|
| **`cpuid` count** | 0, or 1 at startup | no dispatch — a REAL floor |
| | 3–19 sites | runtime dispatch — **not** a floor |
| **dispatch symbols** | `GIsMMX`, `GIsKatmai`, `GIs3DNow` (Unreal Engine 1), `Sys_GetProcessorFeatures` (id Tech 3) | the engine branches — **not** a floor |
| **address adjacency** | the vectorised block sits immediately after a `cpuid` | a fast path — **not** a floor |

And beware the disassembler itself. `objdump` disassembles a PE's `.text`
linearly, including padding, tables and any packed/encrypted region, so a
**small count is usually noise**. `WINQUAKE.EXE` "has" five CMOVs that are
really `add BYTE PTR [edi],al` zero-padding, and `MaxPayne.exe` — SafeDisc, so
most of its image is data to a disassembler — reports 785 CMOVs including
`cmove esp,[ecx]`, which is not an instruction any compiler emits. **Look at
the actual lines** before believing a count. Real MMX looks like coherent
register use (`movd mm0,[esi]` … `punpcklwd mm4,mm2` … `pmulhw`), and real
vector code appears inside a loop.

A 2026-08-30 sweep of all 38 staged titles under this method found exactly one
unconditional floor beyond UT99's — Aliens versus Predator's MMX (1126
MMX-register references, one `cpuid` in the whole binary, matching Rebellion's
published "Pentium 200 MMX" minimum). Everything else large was dispatched:
Unreal Engine 1's software renderer (29,598 MMX references in `SoftDrv.dll`,
gated on `GIsMMX`), GoldSrc's (`hl.exe` carries five `cpuid` sites), id Tech 3's
`jamp.exe` and `quake3.exe`, and StarCraft's SSE2 double math, which begins
0x4e bytes after the two `cpuid` instructions that select it.

**Side finding worth not re-deriving, CORRECTED 2026-08-30:** DXX-Rebirth is a
real CMOV floor, and *not* where the first version of this note said. The
launcher binaries themselves are clean — `d1x-rebirth.exe` and
`d2x-rebirth.exe` carry **0 CMOV** — so an instruction count on "the game" finds
nothing. The floor is in what they **import at load time**: `d1x-rebirth.exe`'s
import table names `SDL.dll` (**286 CMOV**) and `SDL_mixer.dll` (**117**), which
map at process start, not lazily. `libmikmod-2.dll` (**544**) is behind
SDL_mixer. `SDL_mixer.dll` and `libmikmod-2.dll` contain **zero `cpuid`**, so
there is no dispatch to fall back on and the i686 baseline is unconditional.

The same binaries show the *opposite* answer for MMX, and it is the discriminator
table above doing the work: `SDL.dll` has 73 MMX-register references, **14
`cpuid` sites**, and exports `SDL_HasMMX`/`SDL_HasSSE`/`SDL_Has3DNow` — a runtime
dispatch, so **`mmx` must not be declared**. Declaring it would refuse the title
on every machine that runs it perfectly.

So `Games-Library/Descent1/requires.json` and `Descent2/requires.json` state
`"cpu_features": ["cmov"]` on the DXX-Rebirth shortcut **only**, and nothing on
the DOS ones. **Look past the executable named in `launch.txt` to its load-time
imports**: a floor can live entirely in a DLL the launcher never mentions.

---

## `requires_capabilities` — software state, deliberately not a verdict

A capability is something the box **lacks but can be given**. Right now there is
one: `disc_mount`, a virtual disc/CD image mounter.

This matters more than it sounds. Seven already-staged titles mount an image at
launch — SystemShock2, Shogo, RedFaction, StarCraft, Descent2, Descent3,
SoldierOfFortune2 — and `.123` and `.246` have no mounter at all, so on those
boxes those titles have never once worked and nothing reported it.

**A capability gap is never folded into `run`/`marginal`/`no`.** A GeForce2 will
never grow a pixel shader; a missing mounter is an installer away. Calling both
"cannot run" tells the operator to give up rather than to fix the box. So:

* the **copy** decision is hardware-only — the title still deploys;
* the individual **shortcut** that needs the capability is not created, and the
  agent logs `blocked: disc_mount — install a virtual disc mounter (Daemon Tools)`;
* GAMESYNC re-runs on every boot, so the shortcut appears by itself once the box
  is fixed. Nothing needs re-running by hand.

The agent detects it by driver service key (`d347bus`, `sptd`, `ElbyCDIO`,
`mcdbus`, `ImDisk`, …) rather than by looking for an executable, so it survives
the program directory moving.

---

## `shortcuts` — because the halves of a title need different machines

`launch.txt` is already one line per shortcut, and BF1942 is the case that
forced this: single player wants a mounted disc, while the LAN launchers check
neither disc nor CD key. Gating the whole title on the harder half would take
working multiplayer off most of the fleet to protect a shortcut nobody could
have used anyway.

* The key is the **`launch.txt` first column** (the relative exe or `.bat`),
  matched **case-insensitively**.
* A shortcut's block **overlays** the title level: fields it states win, fields
  it omits are inherited.
* **Presence decides, not value.** `"requires_capabilities": []` *clears* a
  title-level requirement — an empty list and an absent list are
  indistinguishable by value and mean opposite things.
* A shortcut with no block just gets the title's rules; a title with no
  `shortcuts` map behaves exactly as before, so this is opt-in per title.
* Values inside `shortcuts` never leak upward into the title level.

**Reminder from CLAUDE.md:** a generated launcher filename must not contain
`(` or `)`. Use `Host Battlefield 1942 - LAN.bat`.

---

## What each verdict causes

| verdict | GAMESYNC does | who decides |
|---|---|---|
| `run` | copies the title | rules |
| `marginal` | copies the title | rules, or the LLM for the borderline band |
| `no` | **skips** the title and logs the limiting factor | rules, or the LLM |
| any + `missing_caps` | copies, drops that shortcut, logs the remedy | rules |

---

## Worked examples from the current fleet

```json
{ "requirements_version": 1, "title": "Deus Ex", "year": 2000,
  "min_cpu_mhz": 300, "min_ram_mb": 128, "min_vram_mb": 16,
  "gpu_feature_level": "fixed",
  "notes": "Unreal Engine 1. Software, Direct3D and OpenGL renderers." }
```
→ `run` everywhere, decided by arithmetic, no LLM call.

```json
{ "requirements_version": 1, "title": "Unreal Tournament 2004", "year": 2004,
  "min_cpu_mhz": 1000, "min_ram_mb": 128, "min_vram_mb": 32,
  "gpu_feature_level": "tnl",
  "notes": "Unreal Engine 2." }
```
→ `.124` (845 MHz) is 15% short: **marginal**, so the LLM is asked. `.145` and
`.246`: `run`.

```json
{ "requirements_version": 1, "title": "Doom 3", "year": 2004,
  "min_cpu_mhz": 1500, "min_ram_mb": 384, "min_vram_mb": 64,
  "gpu_feature_level": "sm2.0", "cpu_features": ["sse"] }
```
→ `.124` is two GPU levels short: flat **no**, no LLM call. That case is the
whole reason the rules run first.

---

## Authoring rules of thumb

1. **Write the published minimum, not your guess at playability.** The 25%
   marginal band and the LLM exist to soften an optimistic box quote; a
   pre-softened number gets softened twice.
2. **Set `gpu_feature_level` whenever the title needs hardware T&L or shaders.**
   It is the axis that separates this fleet, and leaving it out is how a shader
   title reaches a Voodoo.
3. **`cpu_features` is for instructions the binary executes.** A missing SSE2 is
   `#UD` on the first vectorised instruction — an immediate crash, not a slow
   frame rate — so it is a hard `no`. Do not list `sse2` because a title
   "benefits from" it.
4. **Prefer `notes` over precision you do not have.** The borderline band is
   where the LLM reads them, and "has a software renderer" or "runs on DX7-class
   cards at reduced detail" changes the answer.
5. **Bump `requirements_version` when you correct a number**, so cached verdicts
   are invalidated rather than outliving the correction.
6. **NEVER state a WRAPPER's cost as the TITLE's floor.** Four staged DOS titles
   declared `min_cpu_mhz` 350–400 at the title level, with notes that said in
   so many words *"the floor is the emulator's host cost"*. That is true of the
   `Play <Game>.bat` shortcuts, which start the DOSBox staged beside the game,
   and true of nothing else — the game's own DOS binary is in the same tree and
   runs natively on a Pentium. The title-level floor decides whether the tree is
   **copied at all**, so it refused the whole library's DOS half on the one
   machine that can run those games at full speed, and the note explaining why
   was sitting in the file. **A floor a shortcut pays belongs in `shortcuts`.**
   Descent 1 now reads 66 MHz / 8 MB at the title level — its own staged
   `DESCENT.FAQ` says *"486 or Pentium processor, 8 MB RAM"* — with 350 on each
   of the four DOSBox launchers. Guarded by
   `tests/python/test_dosnative.py::test_the_dosbox_host_cost_is_per_shortcut_not_per_title`.
7. **A title-level `requires_capabilities` is inherited by EVERY shortcut**, so
   it suppresses the ones that do not need it. Descent 2 required `disc_mount`
   at the title level; its default shortcut is DXX-Rebirth, whose own launcher
   header reads *"NO DISC, NO MOUNTER"* — and on `.123` and `.246`, neither of
   which has a mounter, **Descent II therefore had no desktop icon at all** and
   nothing said why. State a capability on the shortcut that needs it, and clear
   it on the ones that do not with `"requires_capabilities": []` — remember an
   empty list and an absent list are indistinguishable by value and mean
   opposite things.
6. Validate: `python3 scripts/gamegate/gamegate.py lint`.
