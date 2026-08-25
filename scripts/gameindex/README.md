# Game + server index — keeping every box's favourites full of live servers

The fleet's retro PCs each carry a different set of games, and a game's
built-in server browser is useless in 2026: the master lists are mostly dead
addresses, and several games' masters are gone entirely. This pipeline keeps a
current list of servers **that actually have people on them** and writes it
into each box's favourites, refreshed every five minutes.

Work is split so the slow half happens once, centrally:

| where | does |
|---|---|
| **agent** (`GAMEINDEX`, v1.29.0+) | keeps a cached index of what is installed on that box, refreshed by a background thread, and answers instantly |
| **host** (this directory) | queries the masters, probes every server, keeps the SQLite index, and writes each box's favourites file |

## The five-minute contract

`retro-gameindex.timer` runs `sync.py` every 5 minutes. Each pass:

1. Sweeps the LAN for agents. **Finding none is normal** — the fleet is
   powered on demand.
2. Asks each box `GAMEINDEX HASH` and pulls the full index **only when the
   hash differs** from the DB.
3. Refreshes the live-server table for the engines the fleet actually has
   installed, then pins our own servers on `.132`.
4. Renders each game's favourites file, hashes it, and uploads it **only when
   that hash differs** from what we last wrote to that box.

Steps 2 and 4 are what "refresh only if there are changes" means in practice.
Without them every pass would rewrite every config on every machine every five
minutes — which on a Pentium III over SMB is not free, and would clobber a
game's config while someone is playing.

## What works, and what does not

Both halves are declared explicitly, because "we found nothing" and "we never
looked" are different facts and must not look the same in the log.

| engine | server discovery | favourites writer |
|---|---|---|
| `q3` (Q3A, OpenArena, ET, JK2/JKA, SoF2, MOHAA) | **yes** — ioquake3 + quake3arena + dpmaster, ~580 alive | **yes** — `baseq3\autoexec.cfg`, `seta server1..16` |
| `qw` (QuakeWorld, ezQuake) | **yes** — quakeworld.nu, ~540 alive | no — classic QW has no favourites store; ezQuake's differs per build |
| `q2` | best-effort — `master.q2servers.com` has not answered from here | **yes** — `baseq2\autoexec.cfg`, `set adr0..8` |
| `goldsrc` (CS 1.6, HL, TFC, DoD, TS) | no — every `*.steampowered.com` master hostname fails DNS from this host | no — non-Steam CS 1.6 builds vary in where the browser keeps favourites |
| `unreal`, `ut2k4` | no — the 333networks/GameSpy master is unverified here | no — the `.ini` is rewritten by a running game |
| `t2`, `rtcw`, `nq` | no | no |

Local servers on `.132` are pinned into the top slots for **every** engine,
including ones with no internet discovery — so a box always has something
joinable on the LAN even when the internet list is empty.

## Hard-won details

- **Write `autoexec.cfg`, never the game's own config.** Quake III rewrites
  `q3config.cfg` from memory on exit, so an edit made while the game is running
  is silently undone. Init order is `default.cfg` → `q3config.cfg` →
  `autoexec.cfg`, and the game never writes `autoexec.cfg` back.
- **Merge, do not overwrite.** An existing `autoexec.cfg` usually carries
  `r_mode`/`com_maxfps` someone tuned. Only our marked block and stray
  `seta serverN` lines are replaced.
- **Blank unused slots.** A stale address in a slot we stop writing haunts the
  in-game favourites list forever.
- **Dedupe by host IP.** Big hosts run eight ports of the same server and will
  otherwise eat all 16 slots.
- **Probe, never trust the master.** Of ~900 Q3 addresses about 580 answer.
- **The infostring is not the first line.** Q3 replies
  `\xff\xff\xff\xffstatusResponse\n\key\value...\n<players>`. Reading line 0
  yields the header and an empty dict, which reported "0 alive of 400" for
  servers that all answered.

## Use

```bash
python3 scripts/gameindex/sync.py             # one pass
python3 scripts/gameindex/sync.py --dry-run   # decide everything, write nothing
python3 scripts/gameindex/sync.py --status    # what the DB knows
python3 scripts/gameindex/sync.py --ip 192.168.1.240 --force
```

DB: `~/.retro-fleet/gameservers.db` (override with `RETRO_GAMEINDEX_DB`).
Tests: `tests/python/test_gameindex.py`.
