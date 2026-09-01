# `ssam-tfe-server` :25600 and `ssam-tse-server` :25610 — Serious Sam

Installed 2026-09-01. Both run the staged tree's own
`Bin/DedicatedServer.exe` under Wine in a container, because Croteam shipped
no Linux server for Serious Engine 1 and the staged binary is guaranteed to
speak exactly the protocol the fleet's `SeriousSam.exe` speaks.

| | TFE | TSE |
|---|---|---|
| unit | `ssam-tfe-server` | `ssam-tse-server` |
| game port | **25600** | **25610** |
| query port | 25601 | 25611 |
| gamename | `serioussam` | `serioussamse` |
| version | v1.00 retail | 1.05 retail |
| base dir | `~/ssam-tfe-server` | `~/ssam-tse-server` |
| config | `Scripts/Dedicated/NSCFleet/init.ini` | same |
| levels | `Levels\Deathmatch\DesertTemple.wld` (TFE ships one DM level) | 8-level rotation, `Yodeller` first |

**They are different games, not different maps.** A TFE client handed the TSE
address connects and is rejected — which is why each declares its own
`gamename` in `scripts/gameindex/sync.py`.

```bash
systemctl --user status ssam-tfe-server ssam-tse-server
tail -f ~/ssam-tfe-server/Dedicated_NSCFleet.log
python3 scripts/game-servers/healthcheck.py       # includes both
```

## Why these can be hosted from a Linux box at all

`DedicatedServer.exe` has **no CD check.** Only `SeriousSam.exe` walks the
drive letters for a `DRIVE_CDROM` volume and opens
`<drive>:\Install\Bin\SeriousSam.exe` on it. That asymmetry is already on
record — `.123` has no optical drive and no mounter and still hosted the
verified TFE LAN game — and it is what makes a container with no disc a legal
host.

## The four things that cost time

### 1. `ser_bWaitFirstPlayer` must be 0

The shipped default is 1, and it means the process is up, the log is clean,
and **the server is not hosting** until somebody happens to arrive. Both fleet
configs set it to 0 and say so in a comment.

### 2. TSE needs `ModEXT.txt` at the tree root

Contents: `MP`. Without it the engine loads TFE's module set and dies with

```
Cannot load DLL file 'Z:\game\Bin\Entities.dll': Module not found.
```

— naming a file the TSE tree has never contained (it ships `EntitiesMP.dll`
and `GameMP.dll`). The error points at the wrong thing entirely.

### 3. TSE is on 25610, not 25601

Serious Engine opens the port **and port+1**, so TFE already holds 25600 and
25601. A second server on 25601 would have bound over the first one's second
socket.

### 4. The query is GameSpy on game port + 1, with `mapname` not `maptitle`

```
\gamename\serioussam\gamever\Build10000.1\hostname\NSC Retro Fleet Arena (Serious Sam TFE)
\hostport\25600\mapname\Desert Temple\gametype\Fragmatch\numplayers\0\maxplayers\8
\gamemode\paused
```

`gamemode\paused` with zero players is **normal** — the engine pauses an empty
match. It flips to `openplaying` the moment somebody joins, which makes it a
better liveness signal than the player count.

## Verified

2026-09-01: `.240` joined `ssam-tfe-server` from the staged tree and played.
The server went `numplayers 0 / paused` → `numplayers 1 / openplaying`,
reported `player_0 Serious Sam` at `ping_0 38`, and the box's screenshot shows
live fullscreen gameplay on DesertTemple.

## THE CLIENT NEEDS ITS CONNECTION SETTINGS CHOSEN ONCE PER BOX

This is the real obstacle, and it is on the client, not the server. On a
freshly synced box `SeriousSam.exe` puts up two things before it will join
anything:

1. a modal — *"SeriousSam is starting for the first time…"* → **OK**
2. a full-screen **CONNECTION SETTINGS** page — *"Before joining a network
   game, you have to adjust your connection parameters. Choose one option from
   the list."* — offering `56k modem` ×2, `xDSL or Cable modem`, `ISDN`,
   **`LAN gaming`**, `33.6k modem or older`.

Until that is answered, `+connect <ip>` goes nowhere and the process just sits
there — which reads exactly like a broken server.

**It CAN be driven remotely**, and this is the sequence that worked on `.240`
at 1920x1080:

```
UICLICK 960 584     # OK on the first-time modal
UICLICK 487 463     # highlight "LAN gaming"  (a click only HIGHLIGHTS)
UIKEY  ENTER        # the page says "Enter - load this" - this is what loads it
UICLICK 958 738     # START on the SELECT PLAYERS page
```

**A click alone is not enough** on this menu: it highlights the row and prints
its description ("use this when playing in LAN"), and the page itself tells
you the confirm key. Coordinates must come from a screenshot at the box's own
resolution.

**Open item:** where that choice persists has not been found —
`Scripts/PersistentSymbols.ini` on the box carries no `net_`/`cli_` symbol for
it. If it can be located, staging it in the library would remove this step
from every future box instead of repeating it per machine.
