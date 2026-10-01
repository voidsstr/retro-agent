# The garbled start-up screens under AA - found and fixed (2026-09-30 / 10-01, user at the box)

**Symptom (user):** under AA, every game's start-up screens were garbled until
3D rendering began; gameplay and later menus were fine. Quake II: the 3dfx
splash and the id cinematic. UT99: "the game's splash screen with Unreal's
console, duplicated over the screen multiple times", "a grid of many small
copies".

**Cause:** the diagnostic `Diag\SliPersistAll` = 1, set by hand for the
supervised AA runs. It makes every SLI/AA step a flushed registry phase, so an
AA enable takes **~9.6 s** instead of ~0.2 s (`sweep8x_flightrec.txt.gz`: resets
0-1.7 s, the slaves' mode and video copy 1.7-8.1 s, snoop / AA LFB / video mux /
PLL hand-over 8.1-9.5 s). Glide programs the master's video unit for its own
16-bit tiled buffers *before* it sends the request. For the whole enable the
monitor therefore shows the Windows desktop, which at that moment holds the
game's splash and log windows, read through a half-programmed four-chip
video path, every line folded into tiles. Quake II opens two contexts, so it
paid this twice (~17 s of garble around a clean splash and cinematic).

**Fix:** `SliPersistAll` = 0. That is the deployed default
(`tests/python/test_vcr_kmd_integration.py` pins `"SliPersistAll": "0"`); it is
a diagnostic for hunting a wedge, not for play.

## Evidence

| run | what it shows |
|---|---|
| `ut8x_snapshots.jsonl.gz` (23:06), `ut8x_b_*`, `ut8x_c_*` (one JSON object per snapshot, `_phase` = when) | `vcrctl snapshot` (every IO register of every chip) at UT99's menu and in its demo: all four chips scan out the same buffer (`vidCurrOverlayStartAddr` 0x03b4e000 / 0x038f6000), identical `vidProcCfg` 0x032401a1, identical overlay geometry, genlocked (the same scanline offsets in every snapshot). The display was right whenever these were taken. |
| `sweep8x_snapshots.jsonl.gz` + `sweep8x_flightrec.txt.gz` (10:37, `SliPersistAll` = 1) | snapshots every ~0.4 s from UT's launch: the master switches to a 1280x960x32 desktop at 5.6 s, then **no snapshot from 6.0 to 16.0 s** (the escape waits behind the ~9.6 s enable), then from 16.3 s every chip is final and Glide is swapping. |
| `sweep8x_fast*` (10:41-10:44, `SliPersistAll` = 0) | mode set at 2.2 s, the master in its intermediate 16-bit desktop at 3.6 s, every chip final at 3.8 s: the enable fits inside one 0.2 s snapshot gap. **User: "it started without the garbled and directly into the game. on exit there was a brief flash / garble and that is fine."** |
| menu check (10:47-10:53, 8x, `SliPersistAll` = 0) | UT99 OpenGL (menu, then the timedemo), Quake II (3dfx splash, id cinematic, attract demo, ESC main menu), Quake III (main menu ~1500 frames, then `demo four`): **user: menus and in-game rendering right in all three**, every game exited cleanly. Q3's `demo four` ran as a timedemo (a `timedemo 1` saved in q3config): 1260 frames, 183.6 s, **6.9 fps** at 1280x960, 8x, through the system ICD (retroicd 0.1.80). |

UT99 here ran OpenGLDrv at 1280x960x16 (the staged ini; these scripts did not
patch it the way `v56k_bench.py` does), through our ICD and our h5 Glide.

## Theories this ruled out, for the record

- **Uncleared secondary sample buffers.** Only 8x has a secondary buffer, and
  the garble was seen at 4x too.
- **A chip mask or T-buffer mask that leaves the slaves out.** The splash's
  draws go to mask 0xf / 0xffffffff. h5's `grTBufferWriteMaskExt` does
  mis-route 8x when `sliCount` = 1 (it tests `!gc->sliCount`), but its
  "primary only" case only rewrites the primary address and leaves the
  secondary on, so it is harmless (latent, noted for later).
- **Slaves scanning a different buffer / out of phase.** Disproved by the
  register snapshots above.

## Side findings

- **UT436 holds its `-log` file open exclusively** - `DOWNLOAD` answers
  `Cannot open file: error 32` (26 bytes, which first looked like a 26-byte
  log). A runner cannot read the timedemo summary until UT exits.
- **UT's F10 (`Exit` bind) was ignored twice after the demo ended** (23:17,
  23:19); the console `exit` worked at once.
- During the Q3 check another session replaced the agent with **1.97.1**
  (10:50:57: a host `DIRLIST C:\Games\Descent1` + `HWPROFILE`, then
  `another retro_agent is already running - exiting`, then 1.97.1 at 10:52:46).
  The agent was unreachable for ~2 min; the game, the OS and the AA session
  were untouched.
