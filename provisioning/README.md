# Fleet Onboarding

When a machine runs `retro_agent.exe` for the **first time**, the agent's
`onboard_thread` bootstraps it into the fleet: maps the file share, stages a
core set of games, and applies the retro desktop + dark "hacker" XP theme. It
prints an `ONBOARDING` banner to the console while it works.

```
provisioning/
  onboard.json      the manifest (core game list + desktop/theme) - EDIT THIS
  gen_onboard.py    onboard.json -> onboard.cmd
  onboard.cmd       generated idempotent batch the agent runs (committed for review)
  retro_unzip.js    Shell.Application unzip shim (no unzip tool on 9x/XP)
  push_onboard.py   publish the control files to the share via an online agent
```

## How it works

1. **Agent (C, `agent/src/onboard.c`)** runs once per machine, guarded by
   `HKLM\Software\RetroAgent\Onboarded`:
   - prints the onboarding banner to the console,
   - maps the share (`net use`, path/creds/drive from `HKLM\Software\RetroAgent`:
     `SharePath` / `ShareDrive` / `ShareUser` / `SharePass`, defaulting to
     `\\192.168.1.122\files` -> `Z:`),
   - if `…\Onboard\onboard.cmd` is on the share, copies it local and runs it,
   - **no-op if no payload is staged** (so the new binary is inert on the
     existing fleet until you publish) and it does **not** set the marker itself.
2. **`onboard.cmd`** (the data layer, idempotent) does the real work:
   - for each game: skip if the sentinel file already exists, else `copy /Y` the
     game's ZIP off the share and extract it with `retro_unzip.js`
     (`copy`+extract, **not** `xcopy` - xcopy hangs on NETMAP'd SMB on XP),
   - import `retro_theme.reg` (dark hacker XP theme) if staged, re-park icons,
   - set `Onboarded=1` via `regedit /s` (works on 98 **and** XP; no `reg.exe` on 98).
   Because the batch owns the marker and is idempotent, an interrupted
   onboarding just resumes on the next boot (finished games are skipped).

Wallpaper rotation + desktop-icon parking are (re)applied on **every** boot by
the agent's separate `retrowall` thread; onboarding only adds the theme + a
re-park.

## Core game set (edit `onboard.json`)

| id | game | dest | ZIP on share (`…\Games\`) |
|----|------|------|---------------------------|
| cs16 | Counter-Strike 1.6 (BC Romania) | `C:\Program Files\Counter-Strike 1.6` | `cs16-bc-romania.zip` |
| ut | Unreal Tournament (GOTY/469) | `C:\UnrealTournament` | `unreal-tournament.zip` |
| ra2 | Red Alert 2 (fleet build) | `C:\Westwood\RA2` | `red-alert-2.zip` |
| quake2 | Quake II | `C:\Quake2` | `quake2.zip` |
| quake3 | Quake III Arena | `C:\Quake III Arena` | `quake3.zip` |

To expand the list: edit `onboard.json`, run `python3 gen_onboard.py`, re-publish.
Paths marked NEEDS-VERIFY in the JSON are best-effort until confirmed on a live
machine - fix any the first onboarding reports as `[MISS]`/`[WARN]` and they stick.

## Publishing (do once, then per game)

Two things must be on the share:

1. **Control files** -> `…\Utility\Retro Automation\Onboard\`
   (`onboard.cmd`, `retro_unzip.js`, optional `retro_theme.reg`):
   ```bash
   # generate the theme reg from the wallpaper skill (optional):
   python3 ../scripts/retro-wallpaper/apply_hacker_theme.py --dump-reg > retro_theme.reg
   # publish through any online agent that has Z: mapped writable:
   python3 push_onboard.py <online-agent-ip> --theme-reg retro_theme.reg
   ```
2. **Game payloads** -> `…\Games\<id>.zip` (one ZIP per game, files at the ZIP
   root so extracting into `dest` yields `dest\<sentinel>`; for Quake III the ZIP
   contains the `Quake3\…` subtree). Place these on the share directly.

## Rollout safety

- The onboarding binary is **inert** until the payload is published, so shipping
  the new agent to the fleet changes nothing on already-set-up machines by itself.
- Onboarding is **idempotent** - even if it runs on a machine that already has
  the games, every step is a skip. Optionally pre-set `Onboarded=1` on known-good
  machines to suppress the banner.
- Validated for **XP / 2000** (the fleet). Win98 onboarding (command.com, `.bat`
  vs `.cmd`) is untested - flag if a 9x box needs it.

## Source of game payloads

Games come from the **share** (reliable distribution point). Pulling directly
from a peer agent is a documented future option but not implemented - the share
is the designed source, and `push-*` scripts already populate it.
