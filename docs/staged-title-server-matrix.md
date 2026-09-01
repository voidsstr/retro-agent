# Every staged title → does this host run a dedicated server for it?

**One row per staged title in `Games-Library/`, 46 of them, and a verdict with
a reason.** Three verdicts, never two, because they call for different things:

* **SERVER** — `192.168.1.132` runs a dedicated server for it, and the row says
  which unit and which port.
* **PEER BY DESIGN / NO MULTIPLAYER** — a dedicated server does not exist for
  this game, on any platform, or the game has no multiplayer at all.
  **Inventing one would produce a unit that reports itself healthy while
  nobody can join it.** For these the two-box proof IS the deliverable and it
  is recorded in [`lan-multiplayer-status.md`](lan-multiplayer-status.md).
* **NEEDS A DECISION** — a server binary exists and runs, but something outside
  the server stops the fleet's clients reaching it. Listed with the exact
  blocker rather than built.

Measured 2026-09-01. Check the servers with
`python3 scripts/game-servers/healthcheck.py` — it sends **the right query
packet per engine**, which a single `getstatus` sweep does not.

---

## The 46 staged titles

| # | Staged title | Verdict | Unit / port | Note |
|---|---|---|---|---|
| 1 | AliensVsPredator | peer by design | — | Rebellion shipped no dedicated server for AvP 1999; LAN is DirectPlay peer. Exclusive-fullscreen D3D also makes its screenshots black |
| 2 | BF1942 | **needs a decision** | — | `BF1942_w32ded.exe` runs with no disc and no CD key, but the CLIENT is blocked fleet-wide by SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll`. A server here would have no joinable client |
| 3 | Carmageddon1 | peer by design | — | DOS/IPX; tunnel proven both ends, the front end needs one click |
| 4 | Carmageddon2 | peer by design | — | IPXWrapper, peer-hosted |
| 5 | CounterStrike16 | **SERVER** | `cs16-server` :27015 (query 27018) · `cs16-noblood` :27016 (query 27019) | plus the A2S proxies that make them visible in a 2003 LAN browser |
| 6 | Descent1 | peer by design | — | DOSBox IPX tunnel; one player Hosts |
| 7 | Descent2 | peer by design | — | D2X-Rebirth is UDP/IP native and still peer-hosted — there is no dedicated build |
| 8 | Descent3 | **SERVER** | `descent3-server` :2092 | `main.exe -dedicated` under Wine in Docker |
| 9 | DeusEx | **SERVER** | `deusex-server` :7790 (query **7791**) | NEW 2026-09-01. `DEUSEX.EXE <map>?game=DeusEx.DeusExMPGame -server` under Wine |
| 10 | Doom3 | **SERVER** | `doom3-server` :27666 | NEW 2026-09-01. `DOOM3DED.exe` under Wine |
| 11 | FarCry | **SERVER** | `farcry-server` :49001 | `FarCry_WinSV.exe` under Wine, driven by xdotool |
| 12 | HalfLife1 | **needs a decision** | (`hldm-server` :27020 exists) | The staged tree is the **WON build, protocol 45/46**; every fleet GoldSrc server is protocol 48, so this tree cannot join them. The client that can is the `CounterStrike16` tree's engine |
| 13 | Halo | **needs a decision** | — | `haloded.exe` is not on the share, and Halo allows **one simultaneous player per CD key** (seven keys vaulted) |
| 14 | HexenII | peer by design | — | `h2ded` (uhexen2) refuses the staged retail 1.03 data: "You must patch your installation with Raven's 1.11 update". Hosted box-to-box |
| 15 | HiddenAndDangerous | peer by design | — | No dedicated server binary; the client stops at profile creation |
| 16 | JediAcademy | **SERVER** | `jka-server` :29070 | OpenJK `openjkded`, protocol 26 = retail `jamp.exe` 1.01 |
| 17 | JediKnightDF2 | peer by design | — | Its own staged README: "THERE IS NO DEDICATED SERVER, ON ANY PLATFORM" |
| 18 | JediKnightMotS | peer by design | — | Same Sith engine, same answer |
| 19 | MasterOfOrionII | peer by design | — | DOSBox IPX; MULTI PLAYER is a peer session |
| 20 | MaxPayne | **no multiplayer** | — | Max Payne 1 shipped none |
| 21 | Quake1 | **SERVER** | `quake1-server` :26000 (NetQuake) | `quakeworld-server` :27502 is a DIFFERENT protocol and the staged NetQuake client cannot join it |
| 22 | Quake2Complete | **SERVER** | `quake2-server` :27910 | Yamagi; note the live config is `~/.yq2/baseq2/server.cfg` |
| 23 | Quake3-TeamArena | **SERVER** | `quake3-server` :27961 · `q3ta-server` :27962 | different `fs_game`, so genuinely two servers. `openarena-server` :27960 is a third game |
| 24 | RedAlert2 | peer by design | — | Westwood peer, UDP broadcast; proven `.246` + `.143` |
| 25 | RedFaction | **needs a decision** | — | `rf.exe -dedicated dm` works (verified on `.171`, UDP 7755) but **no client can list or join it**: the THQ Game Tracker is dead and "Add Server → Refresh" leaves the list empty. `-trackerip` is an untested lead |
| 26 | RedneckRampage | peer by design | — | Build engine over an IPX tunnel |
| 27 | **ReturnToCastleWolfenstein** | **SERVER** | `rtcw-server` **:27963** | NEW 2026-09-01, ioRTCW 1.51c. **Proven two-box.** See `scripts/game-servers/rtcw/README.md` |
| 28 | SeriousSamFirstEncounter | **SERVER** | `ssam-tfe-server` :25600 (query 25601) | NEW 2026-09-01. `DedicatedServer.exe` under Wine — it has **no CD check**, only `SeriousSam.exe` does |
| 29 | SeriousSamSecondEncounter | **SERVER** | `ssam-tse-server` :25610 (query 25611) | NEW 2026-09-01. Different game from TFE (`serioussamse`), so a second server, not a second map |
| 30 | ShadowWarrior | peer by design | — | Build engine; SETUP.EXE drives the gather |
| 31 | Shogo | **SERVER** | `shogo-server` :27888 | NEW 2026-09-01. `ShogoSrv.exe` v2.2 — a stand-alone server that shipped in the retail tree and had never been run here |
| 32 | SiNGold | peer by design | — | Ritual never shipped a Linux SiN dedicated server. `ds_deathmatch.bat` hosts from a box |
| 33 | SoldierOfFortune | peer by design | — | Loki's Linux port topped out at 1.06a and a standalone `sofded` never existed |
| 34 | SoldierOfFortune2 | **SERVER** | `sof2-server` :20100 | Raven's own 32-bit `sof2ded`; the unit sets `LD_LIBRARY_PATH` for the bundled `libcxa.so.1` |
| 35 | StarCraft | peer by design | — | UDP broadcast LAN |
| 36 | SystemShock2 | peer by design | — | Co-op joins by typed address; no dedicated build |
| 37 | Thief2 | **no multiplayer** | — | |
| 38 | ThiefGold | **no multiplayer** | — | |
| 39 | TiberianSun | peer by design | — | IPX over IPXWrapper |
| 40 | Turok2 | peer by design | — | GameManager peer session; the host is listed in the joiner's browser but the join fails — an open client-side item, not a missing server |
| 41 | UT2004 | **SERVER** | `ut2004-server` :7777 (query **7787**) | not 7778 — guessing +1 makes our own live server read as down |
| 42 | UnrealGold | **SERVER** | `unrealgold-server` :7807 (query 7808) | NEW 2026-09-01, OldUnreal 227k `ucc-bin-amd64`. Advertises `mingamever 224` |
| 43 | UnrealTournament | **SERVER** | `ut99-server` :7797 (query 7798) | OldUnreal 469e |
| 44 | UnrealTournament436 | **SERVER (the same one)** | `ut99-server` :7797 | The pre-SSE2 436 client joins the 469e server: `HELLO REVISION=0 MINVER=400 VER=436 / Join succeeded`, verified on `.143`. **No second server needed** |
| 45 | WarcraftII | peer by design | — | IPXWrapper; its 8-bit DirectDraw surface is uncapturable by GDI, which is a screenshot problem, not a server one |
| 46 | WarcraftOrcsAndHumans | peer by design | — | Its own DOSBox, IPX |

## Servers this host runs that no staged title needs

Kept because they are played from other trees or from a mod directory:
`openarena-server` :27960 · `specialists-server` :27017 ·
`hldm-server` :27020 (joined from the `CounterStrike16` tree's engine) ·
`quakeworld-server` :27502 · `tribes2-server` :28000 (docker).

## Counting it honestly

| | |
|---|---|
| staged titles | 46 |
| titles with a dedicated server on this host | **19** (`UnrealTournament436` shares UT99's) |
| titles that cannot have one — peer by design or no multiplayer | **23** |
| titles needing a decision (server possible, something else blocks it) | **4** — BF1942, HalfLife1, Halo, RedFaction |

Servers running on `192.168.1.132`: **23** game servers plus 3 A2S proxies.

---

## What is actually PROVEN for each of the seven new servers

The proof standard this project uses is two machines with screenshots of both.
Not every new server reaches it yet, and the difference is stated per row
rather than averaged away — "the server answers its own query protocol" and
"a fleet box played on it" are different claims.

| Server | Answers its own query protocol | Reached from a fleet box | Two-box game |
|---|---|---|---|
| `rtcw-server` :27963 | yes | **yes** — `.123` + `.240` | **YES.** Both in the player list with live pings, Axis and Allied taken, **each box's chat showed the other's message**, both survived the `vstr` rotation `mp_beach` → `mp_village` |
| `doom3-server` :27666 | yes | **yes** — `.123` + `.240` | **YES.** Both named in the server's `infoResponse` player block, both screenshots show live play on `game/mp/d3dm1` at the same match clock with two entries on the scoreboard |
| `unrealgold-server` :7807 | yes | **yes, partially** — the server log records `Open MyLevel … 192.168.1.123` then `Pre: 'Player' 192.168.1.123: Player`, which is `DeathMatchGame` accepting a login | no. The staged client reaches the server and then leaves; its launch URL arrives as `unreal://192.168.1.132/Index.unr` — **the port is dropped** — and `.240` times out `<Unconnected>`. A client-side launch problem, on a server that demonstrably accepts logins |
| `deusex-server` :7790 | yes (`\info\`: hostname, map, `gamemode\openplaying`, 0/16) | not attempted | no |
| `ssam-tfe-server` :25600 | yes (`\status\` on 25601, `gamemode` reported) | not attempted | no |
| `ssam-tse-server` :25610 | yes (`\status\` on 25611) | not attempted | no |
| `shogo-server` :27888 | yes (`\status\` on the game port: hostname, `MCA_12FLOZ`, `gamemode\openplaying`, 0/16) | not attempted | no |

**A listening socket is not a working server** — Red Faction's dedicated server
binds UDP 7755 and *then* prints its refusal, so `netstat` shows a healthy
socket on a dead one. Every row above says "answers its own query protocol"
rather than "the port is open", and each was checked with the packet that
engine actually speaks.

### The two client-side items this leaves

* **Unreal Gold** — find the launch form that keeps the port. The staged
  `UnrealTournament436` tree solved the same class of problem and recorded that
  `unreal://` in a **.bat run on the box** worked where the agent's `start`
  did not; that has been tried here and the port is still dropped, so the next
  variable to change is the client's own Multiplayer → Open Location box.
* **Deus Ex, both Serious Sams, Shogo** — none has a favourites file the
  fleet agent can write, so each is joined by typing the address. That is one
  person, once, per title.
