---
name: retro-benchmark
description: Run the automated retro GPU benchmark suite (Quake III, Unreal Tournament, Deus Ex, Serious Sam, Giants, 3DMark 99/2000/2001) on a fleet machine and collect the FPS/scores. Use when the user - typically chatting FROM the retro PC being tested - says to benchmark this machine, run the benchmark suite, test a newly-installed video card (Voodoo5 5500 / Voodoo5 6000 / GeForce4 Ti4600), or "kick off the benchmark". Produces a results folder + a plain-ASCII summary to relay back.
---

# Retro GPU Benchmark Automation

Drives a retro_agent machine through the full benchmark suite unattended and
collects the data for a SpecPicks GPU-shootout article. The user plugs a card
into the machine, then from the retro chat on that machine says "run the
benchmark" - you kick it off and relay the results.

Tooling lives at `scripts/benchmarks/` in this repo:
- `benchmark_runner.py` - the orchestrator (connects to the agent over TCP)
- `benchmarks.json` - the manifest: per-title config templates, launch lines,
  result files, parsers, per-card FSAA/color-depth profiles
- `templates/` - per-run game config templates (Q3 cfg, Unreal ini, Sam script)
- `results/` - one folder per run (CSV + raw logs + ASCII summary); git-ignored

## Step 1 - Resolve the target machine's IP

The benchmark must run against a specific IP. If the user is chatting from the
machine to benchmark ("benchmark THIS computer"), get the origin IP with
`retro_list_machines` (it prints `origin (this chat): <ip>`). Otherwise ask which
machine, or use the IP they name. The 1 GHz Athlon test bench is typically
`.143`.

IMPORTANT single-connection caveat: the runner opens its own connection to the
agent, and the game will take over the screen for minutes at a time. If the user
is chatting FROM the machine being benchmarked, tell them the chat will go quiet
during each run (the game is fullscreen) and the box is busy - that is expected.
Benchmarking a DIFFERENT machine than the one hosting the chat avoids all
contention.

## Step 2 - Dry-run first to confirm the plan and installs

Always start with `--dry-run`. It connects, detects the GPU via VIDEODIAG,
checks which titles are actually installed, and prints the run matrix WITHOUT
launching anything:

```bash
python3 scripts/benchmarks/benchmark_runner.py --host <IP> --dry-run
```

Relay to the user: which card was detected, which titles are installed vs
missing, and the resolution x depth x FSAA matrix that will run. If a card is
not auto-detected, pass `--card voodoo5-5500|voodoo5-6000|geforce4-ti4600`.

CAVEAT - the Voodoo5 6000 usually enumerates as a "Voodoo5 5500" in the 3dfx
driver (same VSA-100 family), so auto-detect will call a 6000 a 5500. When the
user says they installed the 6000, ALWAYS force `--card voodoo5-6000` so the 4x
FSAA runs are included and the results are labeled correctly.

## Step 3 - Kick off the run

Full suite at the card's profile defaults (resolutions/FSAA/depth chosen per
card in the manifest):

```bash
python3 scripts/benchmarks/benchmark_runner.py --host <IP>
```

Scope it down when the user wants a quick pass or a specific comparison:

```bash
# just the scriptable timedemos, two resolutions, off vs 4x FSAA
python3 scripts/benchmarks/benchmark_runner.py --host <IP> \
    --titles quake3,ut,serioussam --resolutions 1024x768,1600x1200 --fsaa off,4x
```

Per-card API selection matters for the Unreal-engine titles (UT, Deus Ex):
Voodoos run **Glide**, the GeForce4 runs **D3D**. Pass it explicitly when needed:
`--renderdevice Glide2Drv.Glide2RenderDevice` (Voodoo) or
`--renderdevice D3DDrv.D3DRenderDevice` (GeForce4).

The run is long (each 3DMark alone is several minutes). It is safe to let it
proceed autonomously; it closes the connection gracefully after every step.

## Step 4 - Relay results

When it finishes it prints an ASCII table (also saved to
`results/<host>_<card>_<stamp>/summary.txt`) with avg FPS / 3DMark score / status
per run. Relay that table. Full data + raw per-run logs are in the same folder
(`results.csv`, `*.raw.txt`) for the article.

## What is automated vs. not

- **Fully automated** (self-quitting timedemo/batch, parsed): Quake III,
  Unreal Tournament, Deus Ex, Serious Sam, 3DMark 99/2000/2001.
- **Semi-automated** (FRAPS-assisted, fixed-time capture): Giants - Citizen
  Kabuto has no built-in timedemo; flagged as `fraps` in the summary.
- **FSAA application**: resolution and color depth are set automatically via the
  per-run game config. FSAA level is recorded per run, but APPLYING it is
  driver-specific (3dfx tools / NVIDIA registry) and is left to the card's
  driver profile in the manifest - if the manifest has no `fsaa_apply` block for
  the card yet, set FSAA once in the driver control panel and note it, or add the
  reg/env commands to the profile after confirming them on the real card.

## First-run calibration (say this to the user once)

Exact 3DMark batch flags and some log formats vary by build. On the very first
run on a given machine, expect a few `parsed-empty(see raw)` rows - open the
matching `*.raw.txt` to see what the tool actually wrote, then tune the parser
regex / launch flags in `benchmarks.json`. After that the machine is calibrated
and runs clean. Paths in `benchmarks.json` are per-machine (Q3 is under
`C:\Quake III Arena\Quake3\`, UT may be `C:\UT\` or `C:\UnrealTournament\`) -
fix any that the dry-run reports as MISSING and they stick.

See `scripts/benchmarks/README.md` for the article's benchmark plan and the full
manifest schema.
