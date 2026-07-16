# Retro fleet benchmark results

One JSON per suite run: machine specs, driver stack (with **retro3dfx driver
version**), per-benchmark results. Every run is ALSO inserted into the
**specpicks production Postgres** (`retro_benchmark_machines`,
`retro_benchmark_runs`) — see the `3dfx-benchmark-optimization-project` shared
memory for the DSN pointer and conventions.

Naming: `<ip>_<YYYY-MM-DD>_<driver-version>.json` (e.g.
`192.168.1.124_2026-07-16_retro3dfx-0.1.1.json`).

Benchmarks:
- `q3-timedemo-four` — Quake III 1.32, `timedemo 1; demo four` (four.dm_66),
  16bpp, 2 runs/resolution (2nd run = official; 1st warms texture cache).
- `gfxbench-sweep` — our Glide micro-benchmark (`scripts/3dfx/gfxbench/`),
  fillrate/mode sweep, 300 frames/mode.

Reference numbers (Voodoo3, era-correct): thandor.net/benchmark/17, VOGONS wiki
"3dfx Benchmarks", VOGONS t=54517. Rule of thumb: 640x480 CPU-bound (V3 ceiling
~100-111 fps), 1024x768 fillrate-bound (~44-49 fps ceiling on V3 3000).
