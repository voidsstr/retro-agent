# The fleet compatibility matrix

**Which games work on which computers — per box, per title: does it run, how
does it render, does multiplayer work, and what is the evidence.**

```bash
python3 scripts/fleet/compat.py ingest      # refresh from every source
python3 scripts/fleet/compat.py matrix      # the grid
python3 scripts/fleet/compat.py status --box .143
python3 scripts/fleet/compat.py status --title Quake1
python3 scripts/fleet/compat.py gaps        # deployed, but nobody confirmed it runs
python3 scripts/fleet/compat.py gaps --evidence   # ...and a screenshot already exists
python3 scripts/fleet/compat.py conflicts   # measured vs derived disagreements
python3 scripts/fleet/compat.py doc --check # has the LAN status doc drifted?
```

Add `--json` to anything.

## It is not a fifth database

The tables live in **`~/.retro-fleet/fleetbook.db`**, beside the recipes and the
per-machine change log. There were four SQLite files before this and a fifth
silo would have been the mistake:

| file | role |
|---|---|
| `fleetbook.db` | recipes + change log + **the compat matrix** |
| `gamegate.db` | the capability gate's verdict cache — **an input, ingested** |
| `gameservers.db` | live server + installed-game state — **an input, ingested** |
| `gameindex.db` | 0 bytes, never populated |

`gamegate.db` and `gameservers.db` keep their own files because they are caches
other tools write concurrently. Nothing here writes back into them.

## Recording a verification

```bash
python3 scripts/fleet/compat.py record --box .143 --title Quake3-TeamArena \
    --runs verified --renderer opengl --res 1024x768 --fullscreen yes \
    --evidence /home/voidsstr/lan-proof/q3-143.png

python3 scripts/fleet/compat.py record --box .143 --title Quake1 \
    --mp verified_two_box --partner .240 --transport quake-control \
    --evidence /tmp/lanid/evidence/quake1_fraglist_143.png
```

Anything recorded this way is stored with `origin='measured'` and **is never
overwritten by a later ingest**. A machine-derived source that disagrees is kept
alongside it and reported by `conflicts`.

## The distinctions the schema will not let you collapse

Each of these has already cost this project real time.

| | |
|---|---|
| **deployed ≠ runs ≠ verified** | `GAMESYNC state=done` means files are on the disk. It is not evidence the game starts, and starting is not evidence anyone watched it render. |
| **gated ≠ skipped ≠ failed** | Three different follow-ups. A `gated` row carries the limiting factor **and both numbers** — "CPU too slow" without `have`/`need` cannot be argued with. |
| **rendering is per box** | One staged tree, eight different monitors. Resolution, refresh, fullscreen and renderer are per `(box, title)` by construction. |
| **multiplayer is not a boolean** | Eight states. `no_multiplayer` **measured** (Max Payne imports no winsock at all) is a finding; `untested` is an absence. |
| **evidence has a timestamp** | A `verified` row with no evidence is an opinion, and `record` says so. |
| **never tested ≠ failed ≠ n/a** | Three states, never two. `matrix` is a **cross join**, so a cell nobody has looked at is a row reading `untested` — never a blank a renderer could style as fine. |

## Where the data comes from

`ingest` runs these in order, and **fails loudly** if a source is missing
(`--lenient` downgrades that to a logged warning; `--no-probe` skips the live
sweep when the fleet is powered down).

| source | gives | origin |
|---|---|---|
| `fleet-roster.txt` + the boxes' published `HWPROFILE` records | the machines | derived |
| `Games-Library/` | the titles | derived |
| `gamegate.db` | gate verdict, limiting factor, both numbers | derived |
| `gameservers.db` | **presence only** — see below | derived |
| a live `DIRLIST` probe | presence *and absence* | derived |
| `docs/lan-multiplayer-status.md` | the two-box LAN proofs | **measured** |
| `lan-proof/`, `lanid/evidence/`, `retro-screenshots/` | screenshots attached to cells | — |

Two traps worth keeping:

- **`installed_games` cannot prove absence.** It is an *engine-aware* index —
  there is no `game_key` for Doom 3, Far Cry, Halo, Turok 2 or Master of Orion
  II — so "not in it" once marked Doom 3 absent on `.123`, a box where Doom 3 is
  LAN-verified. It writes `deployed` and nothing else.
- **An unreachable box is `untested`, never `absent`.** The fleet is powered on
  demand. So is a box whose `DIRLIST` reply will not parse: *"I could not read
  the answer"* and *"the directory is empty"* must never render the same.

## The dashboard

```bash
python3 scripts/fleet/compat-publish.py --from-vault
```

pushes a snapshot to the nsc-assistant dashboard's **Compatibility** page.

**The direction of travel is the point.** The LAN database is the record; the
dashboard is a view of something it is *handed*. Nothing in the fleet ever calls
the dashboard, so no retro box is affected by it being down. The payload is
scanned for anything key-shaped and the send is **refused, never scrubbed** —
silently stripping a secret would leave it in the local database with nobody
warned it got there. The publish token lives in Key Vault
(`fleet-dashboard-publish-token`) and is never passed on the command line.

Tests: `tests/python/test_fleet_compat.py` — most of them on the negative path.
