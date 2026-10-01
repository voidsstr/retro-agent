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
  completed every run. Visual check by the user: pending.
- **The first Quake III run after a boot read 123.2 fps; the next stock run
  read 104.2.** Compare overclocks against the steady state (104.2): 175 MHz
  +3.8%, 183 MHz +6.4%. A boot-fresh effect of ~18% is its own question.
- `mhz175_dialog_in_foreground/`: another session's `EXEC eventquery.vbs`
  (no cscript) opened a modal Windows Script Host box at 11:01:57 that held
  the foreground through that run.
