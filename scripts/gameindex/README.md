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
4. Renders each favourites **file** the box's games read, and uploads it
   **only when the game would see a difference** from what the box already
   holds (`favorites.same_favourites`). Titles that share a file (Quake III's
   `quake3` and `ioquake3`) are one file and one decision.

Steps 2 and 4 are what "refresh only if there are changes" means in practice.
Without them every pass would rewrite every config on every machine every five
minutes — which on a Pentium III over SMB is not free, and would clobber a
game's config while someone is playing.

## What works, and what does not

Both halves are declared explicitly, because "we found nothing" and "we never
looked" are different facts and must not look the same in the log.

| engine | server discovery | favourites writer |
|---|---|---|
| `q3` (Q3A, ioquake3, OpenArena) | **yes** — ioquake3 + quake3arena + dpmaster, ~575 alive; only servers with **people** on them count (a ping-0 player line is a bot) | **yes** — `baseq3\autoexec.cfg`, `seta server1..16` |
| `q3cache` (Quake III: **Team Arena**) | the `q3` table, filtered to gamename `missionpack` for internet servers too | **yes** — `%APPDATA%\Quake3\servercache.dat`, the ENGINE's favourites list (ioquake3 1.36 layout, binary) |
| `q2` | best-effort — `master.q2servers.com` has not answered from here | **yes** — `baseq2\autoexec.cfg`, `set adr0..8` |
| `unreal` (UT99 469e **and** 436, Unreal Gold) | **seeded** — GameSpy is dead, so a curated address list, every entry probed; 17 of 18 alive | **yes** — `System\<Game>.ini`, `[UBrowser.UBrowserFavoritesFact]` |
| `ut2k4` (UT2004) | **yes** — the OpenSpy UT master `utmaster.openspy.net:28902` (Epic's TCP master protocol, client `UT2K4CLIENT` 3369), each server verified with the in-game browser's own query on game port + 1; ~500 alive of ~600 (2026-09-29). Ours on `.132` is still pinned first | **yes** — `System\UT2004.ini`, `[XInterface.ExtendedConsole]` |
| `ut2k3` (UT2003) | **yes** — the same master, client `CLIENT` 2225, verified the same way (net version 121); 7 alive of 8 (2026-09-29) | **yes** — `System\UT2003.ini`, `[XInterface.Browser_ServerListPageFavorites]` - a DIFFERENT section from UT2004 (read from UT2003's `XInterface.u`) |
| `goldsrc` (CS 1.6, TFC, DoD, TS) | no — every `*.steampowered.com` master hostname fails DNS from this host; the A2S **probe** is wired and verifies our own | **yes** — `config\serverbrowser.vdf`, `filters > favorites` |
| `qw` (QuakeWorld, ezQuake) | **yes** — quakeworld.nu, ~540 alive | no — classic QW has no favourites store; ezQuake's differs per build |
| `t2`, `rtcw`, `nq` | no | no — RTCW's browser keeps the ENGINE's list (`servercache.dat`), not `server1..16`; Tribes 2 has a prefs store nobody has verified joinable without its master |

**Deus Ex and RTCW are not written, on purpose (2026-09-29).** Both used to be
reported "wrote"/"unchanged" every pass for a file no screen of theirs reads:
Deus Ex's join menus never open `UBrowserFavoritesFact`, and RTCW's
multiplayer UI (Team Arena's lineage) has no `server%d` at all - its
`servercache.dat` read `favourites=0` after sessions whose config carried
`server1`. Their reasons are in `favorites.UNWRITABLE`.

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
- **UT2003.** Same five fields, different class: its `XInterface.u` declares
  `struct FavoritesServerInfo { config int ServerID; config string IP; config
  int Port; config int QueryPort; config string ServerName; }` and
  `var() config array<FavoritesServerInfo> Favorites;` on
  `Browser_ServerListPageFavorites`, whose class chain names no `config(User)`,
  so the lines go in `UT2003.ini` under
  `[XInterface.Browser_ServerListPageFavorites]`. Writing UT2004's
  `ExtendedConsole` section there would be read by nothing.
- **UT2003/UT2004 internet servers** come from the OpenSpy UT master, read
  from openspy-core's `code/utmaster` (framing: `<uint32 LE length><body>`,
  UE FStrings; challenge → cdkey hash/response/client/version/os/language →
  `APPROVED` (→ `VERIFIED` for a 3000+ client) → request 0 with no filters →
  one packet per server). It does not check the CD key. Each listed server is
  then asked the browser's own 5-byte query on game port + 1; the reply's net
  version (128 UT2004, 121 UT2003) keeps the two games' servers apart.
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
- **Unreal Gold is the same engine as UT99 and a different game.** It was
  about to be handed a list of UT99 servers. (So was Deus Ex - which, it
  turned out, has no favourites screen at all.)
- **The staged Half-Life tree is WON protocol 45; every fleet GoldSrc server
  answers protocol 48.** Its hw.dll compares the server's protocol with 45
  (`cmp eax,2Dh`), so listing our servers in it would be a favourites list of
  dead entries. (The CounterStrike16 tree's HLDM IS protocol 48 - see
  `favorites.UNWRITABLE["halflife"]` for why it is still not written.)

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
- **Select by liveliness, render in a stable order - and a settled box says
  "unchanged".** Ordering the file by player count meant it changed whenever
  anyone joined a server anywhere in the world (measured on `.171`: two passes
  ninety seconds apart rewrote Quake III and both UT99 trees purely from
  reordering). Until 2026-09-29 every pass still wrote 7-17 files and never 0:
  the Q3 cut reshuffled on bucket crossings and same-host port flips, UT99
  flipped between two seeds on one host, and CS re-saving its vdf with its own
  tabs looked like a revert. Now:
  - **incumbents stay** - a server the box already lists keeps its place while
    it is alive and eligible; the ranking only fills vacancies
    (`db.best_servers(incumbent=...)`). "Alive" for an incumbent means seen
    in the last HOUR with people on at that observation - the horizon
    `prune_servers` keeps a row for - not the 15 minutes a newcomer needs:
    discovery does not re-probe every server every pass (what the masters
    return varies - 389 addresses on one pass, 862 either side - a third of
    it does not answer any one probe, and the bot farms answer bursts
    partially), and with the
    15-minute rule a pass that merely missed a listed server rewrote every
    Q3 file (the boxes' files replayed through five sampled real passes,
    05:04-05:39 on 2026-09-29: all 7 Q3 autoexecs rewritten in three of them;
    judged over the hour, one rewrite - a server unseen for over an hour);
  - **slots are stable** - a kept server keeps its exact `serverN`/`adrN`
    slot or its place in the list, so one server leaving changes one line;
  - **"unchanged" means the GAME would see nothing new** - the same servers
    and every non-favourite line identical; order, labels, whitespace and a
    `lastplayed` the game stamped are not differences
    (`favorites.same_favourites`);
  - **curated seeds are never deduped by host** - `85.214.243.170:7777` and
    `:9000` are two servers, and the "ping" that picked one was a 2.5 s read
    timeout (the round trip is now timed to the FIRST reply).
  A file somebody else reverted (GAMESYNC copying the library's copy back) is
  still a real difference and is repaired on the next pass.
- **A label is never trusted.** Quake II/III split a config line at every `;`
  outside double quotes - inside a `//` comment too - so a public server named
  `x; quit` in a favourites comment would have run `quit` on every fleet box
  at every start. `favorites.clean_label` removes `;`, `"` and every control
  character, and a file already holding such a line is always rewritten.
- **Never write while the game is running - decided per FILE.** Quake III
  rewrites `q3config.cfg` on exit and UT rewrites its ini on exit, both from
  memory, so a write landing mid-session is at best thrown away and at worst
  reverts what the player just set. Each pass asks `PROCLIST` once
  (case-insensitively - Win98 names are upper-case paths) and a file is BUSY
  if the exe of ANY title that reads it is up; the log names the file. Team
  Arena's `servercache.dat` is busy while any Quake III client runs, and
  PROCLIST is asked again right before it is uploaded.
- **Not while GAMESYNC is copying either.** A sync copies the library's files
  back over ours seconds later; a box whose `GAMESYNC STATUS` says `sizing` or
  `copying` is deferred to the next pass.
- **Update, never create — except autoexec.cfg, and never a folder.** The
  absence of `config\serverbrowser.vdf` is the cheapest possible evidence that
  a build does not use that browser (a WON Half-Life at `C:\Sierra\Half-Life`
  has no `revSrvBrowser` at all), and an `.ini` containing nothing but a
  favourites section would be worse than none. `autoexec.cfg` is the opposite
  case: not existing is its normal state, so the Quake writers create it - but
  only in a folder that positively exists. The agent's `DIRLIST` answers `[]`
  for a folder that does not exist, so an empty listing is checked one level
  up; there is no `MKDIR` any more (it planted favourites-only `baseq3\` trees
  in Jedi Academy, SoF2 and MOHAA).
- **Not from a stale index.** A box whose re-index failed this pass is not
  written: the rows are keyed by IP, and an IP DHCP moved can carry another
  machine's paths.
- **Bytes are bytes.** Files are read and written as latin-1, so a byte
  >= 0x80 in a game's config survives the merge instead of becoming `?`.
- **Nothing is written into a benchmark harness.** `C:\q3bench` is a real
  Quake III install whose whole value is that nothing changes underneath it.
- **Probe, never trust the master.** Of ~900 Q3 addresses about 580 answer.
- **The infostring is not the first line.** Q3 replies
  `\xff\xff\xff\xffstatusResponse\n\key\value...\n<players>`. Reading line 0
  yields the header and an empty dict, which reported "0 alive of 400" for
  servers that all answered.

## Team Arena: the engine's own favourites file

Team Arena's UI never reads `server1..16` (measured on `.123`, `.195`, `.240`:
Favorites empty with them set). Its Favorites tab is the ENGINE's list, loaded
from `<fs_homepath>\servercache.dat` when the UI starts and saved over it when
the UI shuts down - for ioquake3 1.36 on Windows, `%APPDATA%\Quake3\servercache.dat`,
one file for every mod. `sync.push_servercache` writes it:

- **derived, not indexed** - Team Arena is the `missionpack\` folder beside an
  `ioquake3` install, so the `missionpack` title is derived from those rows;
- **the path comes from the box** - `REGREAD HKCU ...\Shell Folders AppData`
  (agent-internal, the user the games run as); nothing is created if
  `%APPDATA%\Quake3` does not exist;
- **layout proven, not assumed** - `int numglobal, int numfav, int size
  (692736)`, then 4096 global + 128 favourite `serverInfo_t` of 164 bytes
  (`netadr_t` 32 bytes, port in network order), checked against the ioq3
  source at the 1.36 release point AND the staged `ioquake3.x86.exe`'s own
  `LAN_LoadCachedServers`/`CL_SetServerInfo`/`NET_CompareAdr`; a file in any
  other layout is refused, never rewritten;
- **only ours is ever removed** - an entry we add carries a mark in
  `netadr_t.ip6`, a field the engine never reads for an IPv4 address and
  copies verbatim when it saves; the player's own favourites and the 4096
  cached internet servers are carried through byte for byte;
- **never while any Quake III client runs**, PROCLIST asked again right before
  the upload, and the upload **read back** - anything but an exact match is
  `FAILED`, not `wrote`;
- **a fleet server it could not add is said so** - a list already holding 128
  of the player's own favourites, or an address with no IPv4 form, gives
  `skipped: ... NOT ADDED <addr> - <why>`, never `unchanged`; and with no file
  yet, nothing is created unless something of ours goes in it.
- **Only ioquake3 reads this file.** On a box where `FLEETGL.BAT` picks the
  retail `quake3.exe` (a Voodoo 2 within its 800x600 ceiling) Team Arena runs
  on the retail engine, whose homepath is the install directory - it never
  sees `%APPDATA%\Quake3\servercache.dat`, so there Team Arena's Favorites
  stay empty (harmless; no LCD box takes that path).

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
`test_gameindex_staged_library.py`, `test_gameindex_status.py` and
`test_patch_favourites.py` (labels vs. the Q3 command splitter, Team Arena's
servercache.dat against the staged binary, BUSY per file, the settled-box
contract, and every UNWRITABLE reason checked against the staged library).
