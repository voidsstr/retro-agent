# `rtcw-server` — Return to Castle Wolfenstein on 192.168.1.132:27963

Installed 2026-09-01. Until then the game-servers skill listed `rtcw-server`
**aspirationally** — "no directory, no install script, no game data" — while
RTCW itself was staged and its LAN was being played box-to-box.

## What it is

| | |
|---|---|
| engine | **ioRTCW 1.51c** (`iowolfded.x86_64`), the maintained RTCW fork |
| base dir | `~/rtcw-server` (`fs_basepath`), `~/rtcw-server/.wolf` (`fs_homepath`) |
| config | `main/server.cfg` |
| port | **UDP 27963** |
| gametype | `g_gametype 5` = **GT_WOLF** (objective) |
| rcon | Q3 single packet, password `retroadmin` |
| bots | **8, Omni-Bot 0.93** since 2026-11-13 (mod `omnibot`) — *the engine still cannot do it*, see below |

```bash
systemctl --user status rtcw-server
tail -f ~/rtcw-server/server.log
python3 scripts/game-servers/healthcheck.py     # includes it
```

## The three things that decide whether this works at all

### 1. `qagame.mp.x86_64.so` comes from ioRTCW, NOT from the retail paks

The retail 1.41 tree ships `qagame_mp_x86.dll` — a **Windows** DLL. A Linux
server cannot load it and dies with `VM_Create on game failed`, which reads
like a corrupt install rather than a wrong-platform file. The Linux game module
is in the ioRTCW download at `main/qagame.mp.x86_64.so` and must sit in `main/`
beside the paks.

### 2. The protocol the retail client speaks is 60, and ioRTCW serves it anyway

ioRTCW's own `com_protocol` is **61**. Retail RTCW 1.41 — which is what the
staged `ReturnToCastleWolfenstein` tree is — speaks **60**, and a client whose
protocol does not match is told `Server uses protocol version %i (yours is %i)`
and refused.

ioRTCW carries `com_legacyprotocol`, **defaulting to exactly 60**, and the unit
sets it explicitly so a future default change cannot silently orphan the whole
fleet. The proof that it took is in the server's own `getinfo` reply:

```
\gametype\5\sv_maxclients\16\mapname\mp_beach\hostname\NSC Retro Fleet Arena (RTCW)\protocol\60\gamename\w...
                                                                                    ^^^^^^^^^^^^
```

`getstatus` advertises `com_protocol\61` and `getinfo` advertises `protocol\60`.
**Read `getinfo`** — that is the one the client's browser filters on.

### 3. Port 27963 is not an arbitrary free port

The Quake III engine's LAN scan (`CL_LocalServers`) broadcasts `getinfo` to
`PORT_SERVER + 0..3` — **27960, 27961, 27962, 27963 and nothing else**. Those
first three are already OpenArena, Quake III and Team Arena on this host, so
27963 is the only slot left in the window, and a server outside it would never
appear in RTCW's own LAN browser no matter how healthy it was. The other Q3
servers do answer the same broadcast; the client filters them out by gamename.

## Maps — all 14 retail 1.41 MP maps are staged

`mp_beach mp_castle mp_village mp_depot mp_base mp_sub mp_destruction
mp_assault mp_ice mp_trenchtoast mp_keep mp_chateau mp_tram mp_dam mp_rocket`

Rotation is the usual `vstr` chain (`d1`..`d15`) in `main/server.cfg`. Live map
change: `rcon map mp_castle`.

`sp_pak*.pk3` are deliberately **not** copied — 320 MB of single-player assets a
multiplayer server never opens.

## Correction to the old skill text

The game-servers skill said "`g_gametype` (3=Objective 4=Stopwatch 5=Checkpoint)".
That is wrong and the binary says so itself:

```
$ strings main/qagame.mp.x86_64.so | grep gametype
g_gametype %i is out of range, defaulting to GT_WOLF(5)
```

**5 = objective, 6 = stopwatch, 7 = checkpoint.**

## Verified

2026-09-01, two boxes: `.123` and `.240` both connected to `192.168.1.132:27963`
from the staged retail tree, appeared in the server's `getstatus` player list
with live pings, took Axis and Allied respectively, and **each box's chat
showed the other's message** — traffic relayed by this server, not a
peer-to-peer session that happened to be open.

## Correction: UICLICK *does* drive RTCW's limbo menu

The staged tree's own `Main/autoexec.cfg` carries this note, and it is wrong:

> RTCW MULTIPLAYER'S LIMBO MENU IS A RELATIVE-MOUSE MENU. UICLICK sets an
> ABSOLUTE pointer position, which that menu does not follow - measured on
> .143: two clicks straight on ALLIES left SPECTATOR still selected. So a
> fleet box has no way to pick a team, and a two-box LAN proof cannot get
> either end into the game.

Measured again on 2026-09-01 against this server, on `.123` and `.240` at
`r_mode 3` (640x480 fullscreen, so screen and screenshot coordinates are 1:1):

* `UICLICK 533 314` (**AXIS**) → the panel switched from `CONFIGURE` to the
  `SPAWNING` tab showing *Spawn Closest To: Auto Pick / axis side / allied
  side*. That is the menu responding to an absolute click.
* `UICLICK 533 458` (**CLOSE**) → the limbo screen faded and both clients'
  server-side pings changed from 1 to 3 and 4 — they left the limbo state.

The `F9`/`F10` `team allies` / `team axis` binds that note added are still
useful and are left alone; they are simply not the only route. What is true is
that **the coordinates have to come from a screenshot at the resolution the
game is actually running**, and that `UIKEY CONSOLE` does *not* open RTCW's
console from the limbo screen — the keystrokes land in the limbo CHAT box
instead. That turned out to be a better proof than the console would have
been: `.123`'s chat pane showed **both** `Fleet123`'s and `Fleet240`'s
messages, which is this server relaying between two boxes.

## Bots: Omni-Bot, because the engine genuinely cannot (2026-11-13)

This file said **"none, ever"** for two months. The first half of that was
right and the second half was not: RTCW multiplayer shipped without bots, and
the way to have them anyway is to stop asking the engine.

### What the engine can do: nothing, and it is not a cvar away

```
rcon addbot sarge 3   ->  ^1Error: Bot 'sarge' not defined
rcon bot_enable 1     ->  (accepted, changes nothing)
```

Three independent reasons, all measured on the running server:

* **No bot definitions.** Not one of the 15 pk3s in `main/` contains a
  `botfiles/` directory, so there is no "sarge" to add.
* **No navigation.** The botlib walks a map with `.aas` files. `pak0.pk3` has
  **64** of them and every one is for a **single-player** map (`castle_b0`,
  `chateau_b1`, `crypt1_b0`...). `maps/mp_*.aas` across all 15 paks: **zero**.
  This is the decisive one — it is map DATA, not code, so no engine fix
  produces it.
* **The botlib is never set up.** `iowolfded` has the whole Q3 botlib compiled
  in (`SV_BotInitBotLib`, `BotImport_*`), and the MP server path never calls
  it: `^1Error: SV_BotLibSetup without SV_BotInitBotLib`.

So the leftover `addbot` and `bot_enable` are Quake III scaffolding that RTCW
MP inherited and never used. **They look exactly like a working feature that
is switched off**, which is why this took a measurement rather than a glance.

### What works: Omni-Bot 0.93

[Omni-Bot](https://github.com/jswigart/omni-bot) brings its **own** waypoint
navigation (309 nav files in `omni-bot/rtcw/nav`, covering all 14 maps in our
rotation) and its **own** qagame, and runs as the mod `omnibot`.

```bash
bash scripts/game-servers/rtcw/install-omnibot-rtcw.sh      # ~/rtcw-server, 0.93
```

| | |
|---|---|
| mod dir | `~/rtcw-server/omnibot/` — `qagame.mp.x86_64.so` + the two client paks |
| bot dir | `~/rtcw-server/omni-bot/` — `omnibot_rtcw.x86_64.so`, nav, `.gm` scripts |
| loaded by | `+set fs_game omnibot` on the unit's ExecStart |
| switched on by | `omnibot_enable 1` + `omnibot_path`, in `main/server.cfg` |
| count / skill | `omni-bot/rtcw/user/omni-bot.cfg`; live `rcon bot minbots N`, `bot difficulty N`, `bot moveskill 0..4` |
| difficulty range | **1..6, 1 easiest** (`rtcw_commands.gm` clamps -1..7, -1 = random per bot). The shipped `readme.txt` says "0 to 5, default 3" and is **stale** |
| fleet setting | 8 bots, **difficulty 1 (easiest)**, moveskill 1, plus the profile tuning below |

#### Four things that cost time, or would have

* **0.86 and 0.87 are useless here.** Their only Linux binaries are i386
  (`omnibot_rtcw.so`, `qagame.mp.i386.so`) and `iowolfded.x86_64` cannot load
  a 32-bit module — that route needs a 32-bit engine build and multilib dev
  packages. **0.9+ ships `qagame.mp.x86_64.so` and
  `omnibot_rtcw.x86_64.so`**, so the existing engine runs it untouched. The
  installer asserts the arch rather than discovering it at runtime.
* **`omnibot_enable` / `omnibot_path` must be set BEFORE the map loads.**
  `server.cfg` ends by starting the rotation with `vstr d1`, so a block
  appended after that line is read too late and the bot library silently never
  loads. They go above it; the installer asserts that shape.
* **The qagame looks for `<omnibot_path>/omnibot_rtcw.x86_64.so`** (and
  `<omnibot_path>/omni-bot/omnibot_rtcw.x86_64.so`), from the strings
  `%s/omni-bot/%s.x86_64.so` and `%s/%s.x86_64.so` — so `omnibot_path` is the
  `omni-bot` directory itself. Success is one log line:
  `Omni-bot: ^2Looking for .../omnibot_rtcw.x86_64.so, found.`
* **"the unit is active" proves nothing** — it was active for the whole two
  months this server had no bots. The post-conditions are
  `Omni-bot 0.93 initialized` in the log, bots in `rcon status` at ping 0 with
  a `^4[BOT]^7` name prefix, and `Kill:` lines naming them.

#### Difficulty alone does NOT stop them feeling like cheats (2026-11-13)

Set to difficulty 2 they still played like aimbots, and the reason is not the
skill number - it is the stock **`omni-bot/rtcw/scripts/def_bot.gm`** profile,
which every bot loads when it joins:

| setting | stock | fleet | why |
|---|---|---|---|
| `AimPersistance` | 2.0 | **0.5** | a bot stays aimed at you for two seconds AFTER you break line of sight. From the receiving end that is indistinguishable from being tracked through a wall - this is the one that reads as cheating. |
| `MaxTurnSpeed` | 720 deg/s | **300** | 720 is a 180-degree flick in a quarter second: the classic aimbot snap |
| `MaxViewDistance` | 4500 | **2500** | the file's own comment suggests exactly this for "a closer to human view distance" |
| `MemorySpan` | 2.0 | **1.0** | forgets a lost target sooner |
| `AimStiffness` | 75 | **40** | softer approach onto the target instead of an instant lock |
| `AimDamping` | 10 | **10 - left alone** | see below |

**Leave `AimDamping` at 10.** The file warns that bad aim values cause
oscillation, which invites "lower the damping too". It is a spring: critical
damping is `2*sqrt(stiffness)`, so cutting stiffness 75 -> 40 moves the
critical point 17.3 -> 12.6 and a fixed damping of 10 ends up *closer* to
critical, i.e. less oscillation than stock. Cutting damping as well would
reintroduce it. `test_server_bots.py` pins the damping line as untouched.

**`def_bot.gm` is read when a bot JOINS**, so this applies with no server
restart: `rcon bot kickall` and they refill to `MinBots` with the new profile
(they come back under fresh names - that is how you know it reloaded).

#### The client side

The server now runs a mod, so a joining client needs
`omnibot/omnibot_rtcw.pk3` and `omnibot/ob_media.pk3` in its RTCW folder.
Those are staged at
`Games-Library/ReturnToCastleWolfenstein/omnibot/` and were pushed to the live
boxes, so joins are instant. **A box without them still joins** — the engine
downloads them over UDP (`sv_allowDownload 1`), which is how `.123` and `.240`
got in by themselves within two minutes of the restart. Verified: both, plus
`.139`, were in the player list with live pings alongside 8 bots.

#### Verified on hardware, 2026-11-13

```
Omni-bot: ^2Looking for /home/voidsstr/rtcw-server/omni-bot/omnibot_rtcw.x86_64.so, found.
^2Omni-bot 0.93 initialized in 0.02 seconds.
   8 bots, 4 ALLIES / 4 AXIS, picking classes (soldier/engineer/medic)
   Kill: 3 8 21: [BOT]Noammo killed [BOT]Gordac by MOD_SNIPERRIFLE
   3 fleet boxes joined alongside them (.123, .139, .240)
```
