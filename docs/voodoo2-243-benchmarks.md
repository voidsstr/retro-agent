# Voodoo 2 on `.243` — what runs, and how fast (2026-09-25)

**Host 3.** Only compare within this host (the V5 6000 plan's rule): `.243` is a
Pentium 166 (P54C, no MMX, no CMOV), 127 MB, Windows 98 SE, a Cirrus Logic 5436
for 2D and a **3dfx Voodoo 2 12 MB** (the card that was in `.171`) on the 3dfx
**3.02.02** reference driver (Glide 2.56 / Glide3 3.03). Every OpenGL title
runs through a **3dfx MiniGL** — the Quake II 3.20 `3dfxgl.dll` — never an ICD.

Raw rows, in the V5 6000 campaign's CSV format (`v56k_bench.CSV_COLS`):
`.claude/worktrees/v56k-bench/scripts/benchmarks/results/voodoo2_192.168.1.243/results.csv`
(git-ignored, host-only, like every results folder). This page is the committed record.

Tool: [`scripts/benchmarks/glquake_win9x_bench.py`](../scripts/benchmarks/glquake_win9x_bench.py)
(`--game glquake|quake2`). It drives the box only through agent-internal commands
plus `LAUNCH` (EXEC of a shell is what has killed single-threaded 9x agents), reads
the score from the game's own console log, and stops the local `retro_chat.exe`
for the run.

## GLQuake 1.08 — `timedemo`, 16-bit

| mode | demo1 | demo2 | demo3 |
|---|---|---|---|
| 640x480 | 47.1, 46.0 (**46.5**) | 36.1\*, 50.1 | 44.0, 45.0 (**44.5**) |
| 800x600 | 44.5, 44.8 (**44.6**) | — | — |
| 512x384 | refused | | |

- \* demo2's first run follows a level change and includes loading it with the
  Win98 disk cache cold; the second run is the steady figure.
- **With `retro_chat.exe` running, demo1 read 43.3** — the chat client's three
  1-second pollers (agent 1.84.2) cost a P166 about 8%. 1.85.0 parks those polls.
- **800x600 costs about 4%**: the P166, not the Voodoo 2, is the limit. The
  launchers stay at 640x480 so a Voodoo 1 could run them.
- **512x384 is refused by GLQuake itself** ("Specified video mode not available"):
  it offers only modes the 2D card enumerates, and the Cirrus 5436 has none at
  512x384. The Voodoo could draw it; GLQuake never asks.

## Quake II 3.20 — `timedemo 1` + `demomap`, `ref_gl` → `3dfxgl`, 16-bit

The V5 6000 campaign's method (`v56k_bench.Quake2`): `cl_maxfps 1000`,
`gl_swapinterval 0`, mode verified from the log's `setting mode` line.

| mode | demo1 | demo2 |
|---|---|---|
| 640x480 | 25.0, 24.7 (**24.9**) | 24.4, 24.3 (**24.4**) |
| 800x600 | 25.1, 25.8 (**25.5**) | 24.1, 24.4 (**24.2**) |

**Flat across resolution — entirely CPU-bound on the P166**, the same shape the
V5 6000 showed on a far faster CPU. Every run had to be killed after its score
was printed: the demo ends with its own level change (to `base2`), which
overrides the harness's `nextserver "killserver; quit"`. The score line is
written before that, and the next run started cleanly each time.

## Verified in-game on the Voodoo (F11 frames from the Voodoo framebuffer)

| title | shortcut | evidence |
|---|---|---|
| Quake | `Play Quake - Voodoo.bat` | [E1M1 at 800x600](evidence/voodoo2-243/quake1_243_voodoo2_glquake_e1m1_800x600.png) |
| Hexen II | `Play Hexen II - Voodoo.bat` | [Blackmarsh](evidence/voodoo2-243/hexenii_243_voodoo2_blackmarsh_640x480.png) |
| Quake II | `Play Quake II - Voodoo.bat` (title `Quake2Win9x`) | [Outer Base](evidence/voodoo2-243/quake2win9x_243_voodoo2_base1_640x480.png) |

Recorded in the compatibility DB (`compat.py status --box .243`).

## What it took (details in `retro-3dfx/FINDINGS.md` and `CLAUDE.md`)

- The Voodoo 2 was not enumerated at boot until a PCI re-enumeration installed it
  (agent `PCIRESCAN`); since then two cold boots have found it unaided.
- `Quake2Win9x` is a new staged title — Quake II's base game with COMMAND.COM
  launchers (the full title is cmd.exe-only and 761 MB).
- Getting it onto a 1.2 GB Win98 disk found three agent defects, fixed in
  **1.85.0**: GAMESYNC's 300 MB free-space margin (now 150 MB under 4 GB), and a
  directory walk that treated a dropped SMB session as "no more files" — twice
  reporting a half-copied Quake II as complete.
- **DOS Glide:** `GLIDE2X.OVL` is installed in `C:\WINDOWS`; no staged DOS Glide
  title fits this disk yet (Carmageddon 1's 3dfx build needs 660 MB and a CD image).
