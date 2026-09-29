# Supervised AA on the V5 6000 - 2026-09-29 (user at the box)

Stack: vcr-kmd (live-clock build), our h5 Glide 38a891e8 (SLIAA-GUARD + AA-TRACE),
MesaFX 0.1.78, agent 1.94.0. Monitor HP P1120. Test: `glidelab_run.py ... edges
--res 1024x768 --refresh 60 --cfg 3` (4 chips, SLI, 2-sample AA, analog), no LFB
read; the user judged the monitor. `Diag\SliAA`=1 and `SliPersistAll`=1 for every run.

| run | switches | frames | what the user saw |
|---|---|---|---|
| r1, r1b | + `SliAAFifoGate`=1 (clean boot) | 1800 / 3600 | "run it again" (not judged) |
| r1c | + `SliAAFifoGate`=1 | 7200 | first garbled/offset, then the monitor re-synced and it "looked good and anti-aliased" |
| r2, r2b | + `SliAAFifoGate`=1 | 36000 | garbled first, then the monitor changed and it looked good |
| r3 | `SliAAFifoGate` ABSENT (clean boot) | 36000 | **single, smooth** after the re-sync |

## What this settles

* **2x AA (Glide cfg 3 = 6 on 4 chips) works on this board, with or without the
  cfg 3 ghost arm.** The "ghost / double image" of 2026-09-27 (`aa_supervised/`)
  was very likely the SETUP window, not the AA output.
* **The AA request takes 7-8 s with `SliPersistAll`=1** (every step flushed to the
  registry): Glide's trace has the escape at t and its return 7.6-8.2 s later. During
  that time the monitor shows whatever is in video memory, un-merged - leftover
  triangles, which read as a ghost. Then the SLI/AA enable re-syncs the monitor and
  the AA image is correct.
* **Under AA the frames are not held to the refresh**: 7200 frames ran in ~13 s, so a
  "--frames 1800" run draws AA for ~3 s - the 09-27 edges run was mostly setup.
  In r2b the AA phase lasted 62 s (kernel SLI/AA on 1628 s -> off 1690 s).
* The recorder ring (1024 entries) holds a whole SLI enable sequence, so the start of a
  long run scrolls out; `vcrlog_boot_r1.txt` is the r1c boot's ring.

## Next (in progress when the host went down)

Quake II through our ICD at `SSTH3_SLI_AA_CONFIGURATION`=6 (MesaFX asks for
GR_PIXFMT_AA_2_*). After that launch `.124` stopped answering entirely (no ARP,
445 closed): most likely a hard freeze - needs a power cycle, then
`vcrphases.py --prev` names the last step.
