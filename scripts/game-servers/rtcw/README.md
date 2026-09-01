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
| bots | **none, ever** — RTCW MP shipped no bots, so its player count is always human |

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
