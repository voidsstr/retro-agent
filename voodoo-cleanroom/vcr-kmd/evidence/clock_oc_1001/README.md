# Live graphics-clock overclock on the V5 6000 - 2026-10-01 (user at the box)

`vcrctl clock set <MHz>` moves the master's core clock live, in <= 5 MHz
steps, reading back each one; chips 1-3 take the master's word at every SLI
enable. Nothing is persisted, so every boot is the VBIOS's 166.806 MHz. The
runs below are clean-boot #60, 4-chip SLI (cfg 5, no AA). Fill is
`glidelab_run.py fill --res 640x480 --cfg 5`. Quake III is `v56k_bench.py
--titles quake3:allours` (our ICD 0.1.76 + our h5 Glide, `demo four`,
1024x768x32).

| clock | fill Mpix/s | vs stock | Quake III fps |
|---|---|---|---|
| 166.806 (stock) | 1110.7 | - | 123.2 (first run after the boot), 104.2 (later) |
| 174.999 | 1165.2 | x1.0491 (= the clock ratio) | 106.5 (WSH dialog in the foreground - discard), 108.2 |
| 182.954 | 1218.2 | x1.0968 (= the clock ratio) | 110.6, 111.2 |

- **Fill scales exactly with the clock** at both steps, and both overclocks
  completed every run.

| 1024x768x32, cfg 5 | stock 166.8 MHz | 183 MHz |
|---|---|---|
| Quake II, `quake2:allours` (our ICD + our h5 Glide) | 174.6 | **177.2** (+1.5%; CPU-bound at this rate) |
| Counter-Strike 1.6 (`cs16_bench.dem`) | 31.1, 59.7 | 54.0 |

- **User at the box: Counter-Strike and Quake II at 183 MHz "clean, no
  artifacts"**, both exited cleanly.
- **Counter-Strike's timedemo cannot resolve a 10% clock step**: the same
  1045 frames took 33.6 s and 17.5 s in two consecutive stock runs. Its number
  needs several runs and a median before it says anything about the clock.
- Clock restored to stock at 19:03 (all chips read 166.806 MHz after the next
  SLI enable); a reboot does the same, since nothing is persisted.
- **The first Quake III run after a boot read 123.2 fps; the next stock run
  read 104.2.** Compare overclocks against the steady state (104.2): 175 MHz
  +3.8%, 183 MHz +6.4%. A boot-fresh effect of ~18% is its own question.
- `mhz175_dialog_in_foreground/`: another session's `EXEC eventquery.vbs`
  (no cscript) opened a modal Windows Script Host box at 11:01:57 that held
  the foreground through that run.
