# Retro GPU Benchmark Automation

Automated benchmark suite for the SpecPicks 3dfx-vs-GeForce4 shootout. Plug a
card into the test machine, then from the retro chat on that box say "run the
benchmark" - the chat brain invokes the `retro-benchmark` skill, which runs this
runner and relays the numbers.

```
scripts/benchmarks/
  benchmark_runner.py   orchestrator (drives the agent over TCP)
  benchmarks.json       manifest: titles, configs, launch lines, parsers, cards
  templates/            per-run game config templates
  results/              one folder per run (git-ignored)
```

## Quick use

```bash
# 1. dry-run: detect GPU, check installs, print the matrix (no launches)
python3 benchmark_runner.py --host 192.168.1.143 --dry-run

# 2. full suite at the card's profile defaults
python3 benchmark_runner.py --host 192.168.1.143

# 3. scoped pass
python3 benchmark_runner.py --host 192.168.1.143 \
    --titles quake3,ut,3dmark2001 --resolutions 1024x768,1600x1200 --fsaa off,4x
```

Env: `RETRO_AGENT_SECRET` (default `retro-agent-secret`), `RETRO_AGENT_PORT`
(9898). SMB creds for staging: `RETRO_SMB_USER` / `RETRO_SMB_PASS`.

## The three contenders (manifest `cards`)

| id | card | depths | FSAA | notes |
|----|------|--------|------|-------|
| `voodoo5-5500` | Voodoo5 5500 | 16 | off, 2x | 2x VSA-100, RGSS, no T&L/shaders |
| `voodoo5-6000` | Voodoo5 6000 "Strange God" | 16 | off, 2x, 4x | 4x VSA-100, 4x FSAA is the point |
| `geforce4-ti4600` | GeForce4 Ti4600 | 16, 32 | off, 2x, 4x | true 32-bit, MSAA, HW T&L + DX8.1 |

Card is auto-detected from `VIDEODIAG`; override with `--card`.

## Titles (manifest `titles`)

| id | title | method | auto-quit | parser |
|----|-------|--------|-----------|--------|
| `quake3` | Quake III Arena | OpenGL timedemo (`demo four`) | yes | quake3 |
| `ut` | Unreal Tournament | `-benchmark` flyby (Glide/D3D) | yes | unreal_log |
| `deusex` | Deus Ex | Unreal `-benchmark` timedemo | yes | unreal_log |
| `serioussam` | Serious Sam TFE | built-in demo profile | yes | serioussam |
| `giants` | Giants: Citizen Kabuto | FRAPS fixed-time | no | fraps |
| `3dmark99max` | 3DMark99 MAX | `-batch` | yes | futuremark |
| `3dmark2000` | 3DMark2000 | `-batch` (HW T&L test) | yes | futuremark |
| `3dmark2001` | 3DMark2001 | `-batch` (Nature = N/A on Voodoo) | yes | futuremark |

## How a run works (per title x resolution x depth x FSAA)

1. **Config** - render `templates/<x>` with `{width}{height}{colordepth}...`,
   `UPLOAD` it as a `-ini=`/`+exec` override (never touches the real game ini).
2. **Clear** the stale result file; ensure the result dir exists.
3. **Launch** GUI via `LAUNCH cmd /c cd /d "<workdir>" && start "" "<exe>" <args>`
   (LAUNCH, not EXEC - EXEC runs GUIs hidden and hangs the agent).
4. **Wait** - poll `PROCLIST` until the game exe exits (`auto_quits`), or run a
   fixed `max_run_s` then `PROCKILL` (FRAPS titles / loopers).
5. **Collect** - `DOWNLOAD` the result file, run the tolerant parser, keep the
   raw log. Every connection closes gracefully (Win98 RST-crash rule).

Output: `results/<host>_<card>_<stamp>/` with `results.csv`, `summary.txt`
(plain ASCII for the chat), `meta.json`, and `<tag>.raw.txt` per run.

## Methodology (matches the article plan)

- **vsync OFF** everywhere (configs disable it) - uncapped frame rates.
- **Best native API per card**: Q3/Sam = OpenGL all; UT/Deus Ex = Glide on the
  Voodoos, D3D on the GeForce4 (`--renderdevice`); Giants/3DMark = D3D.
- **16-bit on all** for apples-to-apples, plus GeForce4 32-bit as its best-IQ
  pass. Voodoo "best IQ" = 16-bit + postfilter + 4x RGSS.
- **The story is FSAA quality, not just FPS**: RGSS (Voodoo) supersamples
  textures/alpha; MSAA (GeForce4) smooths polygon edges only. Capture IQ
  screenshots on alpha-heavy scenes (grates, foliage, fences, wires) separately
  with `retro_screenshot` - the numbers are only half the article.
- **Structural caveats to report, not hide**: Voodoo5 has no HW T&L (3DMark2000)
  and no pixel shaders (3DMark2001 Nature = N/A); at 1 GHz the low resolutions
  are CPU-bound and compress GPU differences - lean on high-res + FSAA runs.

## First-run calibration

3DMark batch flags and some log formats vary by build; first run on a machine
may show `parsed-empty(see raw)` rows. Open the `*.raw.txt`, then tune the
parser regex or launch flags in `benchmarks.json`. Fix any MISSING install paths
the dry-run reports (Q3 is double-nested under `...\Quake III Arena\Quake3\`; UT
is `C:\UT\` or `C:\UnrealTournament\`). On Win98, spaces in paths can break
LAUNCH - use 8.3 short paths in the manifest if needed.

## FSAA application

Resolution/depth are set automatically. Applying an FSAA *level* is
driver-specific and not yet wired per card - set it once in the 3dfx Tools /
NVIDIA control panel (or add a `fsaa_apply` command list to the card profile
after confirming the reg/env keys on the real hardware). The requested level is
always recorded in the results so runs stay labeled correctly.
