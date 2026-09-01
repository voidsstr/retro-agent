# `shogo-server` — Shogo: Mobile Armor Division on 192.168.1.132:27888

Installed 2026-09-01. **`ShogoSrv.exe` v2.2 — Monolith's stand-alone Shogo
server — shipped in the retail tree all along and had never been run here.**
It sits in `Games-Library/Shogo/` beside `Shogo.exe`, with its own
`ShogoSrv.cfg`, `ShogoSrv.txt` and `server.dll`.

| | |
|---|---|
| binary | `ShogoSrv.exe` 292,352 B, 1999-02-08, from the staged tree |
| runtime | Wine in docker (`retro-wine:bookworm`, `--net=host`, `xvfb-run`) |
| base dir | `~/shogo-server` |
| port | **UDP 27888** (the `0` in the wizard's port box means "use 27888") |
| query | **GameSpy `\status\` on the GAME PORT ITSELF** — not port+1 |
| wine prefix | docker named volume `shogo-wineprefix` |

```bash
systemctl --user status shogo-server
tail -f ~/shogo-server/server.log
python3 scripts/game-servers/healthcheck.py       # includes it
```

## Why the wizard is driven on every single start

`ShogoSrv.exe` is a **GUI wizard**, not a command-line server. `ShogoSrv.txt`
offers `-go` — *"skip all of the dialogs (the previous settings will be
used)"* — and then adds the sentence that decides the design:

> If there aren't any previous settings to use, the dialogs will still come up.

ShogoSrv commits those settings **on a clean exit**, and a systemd service is
killed rather than closed. So there are never any previous settings, `-go`
always shows the wizard, and a unit that just runs `ShogoSrv.exe -go` sits
forever on page 1 while the container looks perfectly healthy.

`_run/entry.sh` therefore drives the whole four-page wizard with `xdotool`
every time — about 70 seconds, hence `TimeoutStartSec=300`. Ugly, and it is
the difference between "a person has to click through a dialog after every
reboot" and a server that comes back on its own.

A persistent Wine prefix was tried first and does **not** help: nothing is
written to `win.ini`, `user.reg` or `ShogoSrv.cfg` because the process is
killed before it can. The named volume is kept anyway so `wineboot` does not
rebuild the prefix on every start.

## Three things that cost real time

### 1. DO NOT untick "Communicate with GameSpy"

Page 1 offers three broadcast options and two of them really are dead services
(GameSpy's master, and registration with the Shogo web site). **The GameSpy
one is not a master uplink — it is the switch for the server's own query
responder.**

Measured both ways on 2026-09-01:

| | UDP 27888 | `\status\` | window title |
|---|---|---|---|
| unticked | **bound** | every query form times out | `Shogo Server` |
| ticked | bound | answers with hostname, map, players | `Shogo Server v2.2` |

Unticked it is a listening socket that no browser can see — the same shape as
the Red Faction trap, and `netstat` says everything is fine. Only the Shogo
web-site registration is unticked; that host really is gone.

### 2. There is one more modal AFTER Finish

After the level list and Finish, ShogoSrv puts up an info box —
*"Power users can specify the -go command-line parameter when running ShogoSrv
to skip all the dialogs"* — and does nothing further until it is dismissed. At
that moment the window list reads `Shogo Server`, which looks exactly like a
running server while the port is unbound. `entry.sh` clicks its OK.

### 3. Neither triple-click nor Ctrl+A selects text in its name box

Page 2's Server Name field is a classic Win32 EDIT control. Under Wine a
triple-click does not select its contents, and neither does `ctrl+a` (that is a
modern shell affordance the control never implemented). Both leave the default
in place and the typed name is **appended**, so the server advertised itself as
`Shogo ServerNSC Retro Fleet Arena - Shogo`. `End` then 40 × `BackSpace`
works.

## Levels

The wizard's page 4 has `Retail Levels` → `Add >` → `Game Levels`, and
**Finish stays greyed until at least one level is added**. `entry.sh` adds
eight. The retail MCA_* list (`MCA_12FLOZ`, `MCA_AVERNUS`, `MCA_ENTRANCE`,
`MCA_MADSKILLS`, `MCA_MARITROPA1`, …) lives inside `SHOGO.REZ`; to rotate a
different set, change the `Down`/`Add` loop in `entry.sh`.

## What is NOT proven

The server answers its query correctly and reports `gamemode\openplaying`, but
**no two-box join has been completed from the fleet**. Shogo's client joins by
typed address (Multiplayer → the address box), and no favourites file exists to
pre-fill (see `favorites.UNWRITABLE["shogo"]`).
