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

`retro-gameindex.service` runs `sync.py --daemon`, which does a pass every 5
minutes. Each pass:

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
| `q3` (Q3A, ioquake3, OpenArena) | **yes** — ioquake3 + quake3arena + dpmaster, ~575 alive | **yes** — `baseq3\autoexec.cfg`, `seta server1..16` |
| `q2` | best-effort — `master.q2servers.com` has not answered from here | **yes** — `baseq2\autoexec.cfg`, `set adr0..8` |
| `unreal` (UT99 469e **and** 436, Unreal Gold, Deus Ex) | **seeded** — GameSpy is dead, so a curated address list, every entry probed; 17 of 18 alive | **yes** — `System\<Game>.ini`, `[UBrowser.UBrowserFavoritesFact]` |
| `ut2k4` (UT2004) | no — no live master and no seed list; ours on `.132` is pinned directly | **yes** — `System\UT2004.ini`, `[XInterface.ExtendedConsole]` |
| `goldsrc` (CS 1.6, TFC, DoD, TS) | no — every `*.steampowered.com` master hostname fails DNS from this host; the A2S **probe** is wired and verifies our own | **yes** — `config\serverbrowser.vdf`, `filters > favorites` |
| `qw` (QuakeWorld, ezQuake) | **yes** — quakeworld.nu, ~540 alive | no — classic QW has no favourites store; ezQuake's differs per build |
| `t2`, `rtcw`, `nq` | no | no |

Local servers on `.132` are pinned into the top slots for **every** engine,
including ones with no internet discovery — so a box always has something
joinable on the LAN even when the internet list is empty.

## Where each mechanism came from

None of the three new writers was inferred from the Quake pattern. Each was
read out of the game's own files in the staged library:

- **Unreal engine 1.** `System\UBrowser.u` carries the format as a literal
  comment — `/* eg Favorites[0]=Host Name\10.0.0.1\7778\True */` — and
  `Query()` passes field 2 to `FoundServer` as the **query port**. The fleet's
  UT99 is 7797 game / 7798 query, so this matters.
- **UT2004.** `XInterface.u` declares `struct ServerFavorite { int ServerID;
  string IP; int Port; int QueryPort; string ServerName; }` on
  `class ExtendedConsole`, which `UT2004.ini` names as the Console class. Its
  query port is **7787 for game port 7777** — `+10`, not `+1`, which is why
  the port is carried on the row and never derived.
- **GoldSrc.** The staged CS 1.6 tree's own `revSrvBrowser.dll` contains the
  `printf` template it writes into `config\ServerBrowser.vdf`, keys and tabs
  included.

## The per-title policy, and why the engine is not enough

`favorites.TITLES` is keyed on the game key, not the engine, because four
things are invisible at engine level and each one silently produced a
favourites list that could not work:

- **Soldier of Fortune II and Jedi Academy are Quake III engine but keep their
  data in `base`, not `baseq3`.** The writer was creating a directory the game
  never reads and reporting it as a success.
- **The agent reports a game's directory as the one the EXE was in**, which for
  every Unreal-engine title is `System\` — so appending `System` again gave
  `...\System\System\UnrealTournament.ini`, a path that cannot exist.
- **Unreal Gold and Deus Ex are the same engine as UT99 and a different game.**
  They were about to be handed a list of UT99 servers.
- **The staged Half-Life tree is WON protocol 46; every fleet GoldSrc server
  answers protocol 48.** Half-Life is box-to-box LAN only here, so listing our
  servers in it would be a favourites list of dead entries.

Every staged title is therefore either written or listed in
`favorites.UNWRITABLE` **with a reason in its own words** — Red Alert 2 and
Tiberian Sun are LAN broadcast/IPX with no list to populate, Thief is
single-player, Descent 1 is a DOSBox `ipxnet` tunnel. A title that falls
through to the generic "no verified favourites mechanism" is one nobody has
looked at yet, and `test_gameindex_favorites.py` fails if a staged title ever
does.

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
- **Dedupe by host IP — but not our own.** Big hosts run eight ports of the
  same server and will otherwise eat all 16 slots. Applying the same rule to
  `.132` was a bug: all ten fleet servers live on one IP, so a box was given
  Quake III *or* OpenArena, CS 1.6 *or* the no-blood server, never both.
- **Filter our own servers by what the title can join.** We know exactly what
  runs on `.132`, so a Counter-Strike client is never given the Specialists
  server and a Quake III client is never given the OpenArena one. The same
  applies to the seeded list, which we also curate — but *not* to a master's
  output, where we have no reliable mod taxonomy and a permissive list beats a
  silently emptied one.
- **Select by liveliness, render in a stable order.** Ordering the file by
  player count means it changes whenever anyone joins a server anywhere in the
  world, so the applied-hash check never matches and every box is rewritten
  every five minutes — the exact cost this design exists to avoid. Measured on
  `.171`: two passes ninety seconds apart rewrote Quake III and both UT99
  trees purely from reordering. Ranking is now on bucketed player counts
  (fours) and pings (25ms), tie-broken on the address. Quake III still changes
  most passes — with ~575 live servers and 16 slots the membership genuinely
  moves, by about one server per pass — but UT99, CS 1.6, Quake II and UT2004
  now sit unchanged.
- **Never write while the game is running.** Quake III rewrites `q3config.cfg`
  on exit and UT rewrites its ini on exit, both from memory, so a write landing
  mid-session is at best thrown away and at worst reverts what the player just
  set. Each pass asks `PROCLIST` once and skips any title whose exe is up.
- **Update, never create — except autoexec.cfg.** The absence of
  `config\serverbrowser.vdf` is the cheapest possible evidence that a build
  does not use that browser (a WON Half-Life at `C:\Sierra\Half-Life` has no
  `revSrvBrowser` at all), and an `.ini` containing nothing but a favourites
  section would be worse than none. `autoexec.cfg` is the opposite case: not
  existing is its normal state, so the Quake writers create it.
- **Nothing is written into a benchmark harness.** `C:\q3bench` is a real
  Quake III install whose whole value is that nothing changes underneath it.
- **Probe, never trust the master.** Of ~900 Q3 addresses about 580 answer.
- **The infostring is not the first line.** Q3 replies
  `\xff\xff\xff\xffstatusResponse\n\key\value...\n<players>`. Reading line 0
  yields the header and an empty dict, which reported "0 alive of 400" for
  servers that all answered.

## A service, not a timer

It used to be a `oneshot` behind `retro-gameindex.timer`. That met the
five-minute contract but meant the unit read `inactive (dead)` for 297 of every
300 seconds — so **"is the favourites agent running?" had no honest answer at
the moment anyone asked**, which matters now that the login-screen status wall
reports on it. It is a long-running `Type=simple` service instead; the timer is
gone (leaving it enabled would start a second pass fighting the daemon over the
same SQLite file).

```bash
cp scripts/gameindex/retro-gameindex.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user disable --now retro-gameindex.timer   # if you had the old one
systemctl --user enable --now retro-gameindex
```

After every pass it publishes its own health to
`$XDG_RUNTIME_DIR/retro-gameindex/status.json` — when the pass ran, how long it
took, which boxes it reached, how many favourites files it rewrote versus left
alone, per-engine live-server counts, and any errors.

**Why publish at all, when there are logs.** The fleet is powered on demand, so
a completely healthy pass across zero live boxes writes nothing and logs almost
nothing. Judged by its output, a healthy agent looks dead every time the retro
machines are switched off. "Nothing to do" and "did not run" must not look the
same, so the agent says outright that a pass completed.

A pass that throws is caught, published as a failure with its reason, and
followed by the next pass on schedule — one box refusing a connection must not
take the agent down.

## Use

```bash
python3 scripts/gameindex/sync.py             # one pass (also refreshes the status file)
python3 scripts/gameindex/sync.py --dry-run   # decide everything, write nothing
python3 scripts/gameindex/sync.py --status    # what the DB knows
python3 scripts/gameindex/sync.py --daemon    # loop forever (this is how the unit runs it)
python3 scripts/gameindex/sync.py --ip 192.168.1.240 --force
```

DB: `~/.retro-fleet/gameservers.db` (override with `RETRO_GAMEINDEX_DB`).
Tests: `tests/python/test_gameindex.py`, `test_gameindex_favorites.py` (the
non-Quake writers and the per-title policy), `test_gameindex_no_clobber.py`,
`test_gameindex_staged_library.py` and `test_gameindex_status.py`.
