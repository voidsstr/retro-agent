# `unrealgold-server` — Unreal Gold 226, the staged tree's own UCC.exe under Wine

Since **2026-09-28** the fleet's Unreal Gold server is the staged
`Games-Library/UnrealGold` tree's own **`System\UCC.exe`** (Unreal 226 Final),
run under Wine inside the `retro-wine:bookworm` container — the same shape as
Deus Ex, DOOM 3, Descent 3 and Far Cry. It replaced an **OldUnreal 227k** Linux
server that no fleet box could join.

| | |
|---|---|
| unit | `unrealgold-server` (`systemctl --user`, enabled; linger keeps it across reboots) |
| container | `ugsrv` (`--net=host`, runs as the tree's owner, not root) |
| command | `wine UCC.exe server "DmDeck16.unr?game=UnrealShare.DeathMatchGame?maxplayers=12" -ini=UnrealServer.ini -port=7807` |
| tree | `~/unrealgold-server` — a copy of the staged tree minus `Help/` and `Manual/` |
| ports (UDP) | join **7807** · GameSpy query **7808** · LAN beacon **7775** |
| log | `~/unrealgold-server/server.log` (UCC's own log, via stdout) |
| probe | `gameservers.py` `probe_unreal226` — `\info\`, and `gamever` must be **226** |

The ports are the ones the 227k server used, so every client's favourites and
the `retro-gameindex` pin (`sync.py` `LOCAL_SERVERS`) stayed valid unchanged.

## Why 226, and why not the 227k Linux build

The staged tree is retail Unreal Gold patched to **226 Final**
(`Games-Library/_patches/README.txt`). A 226 client connecting to a 227 server
aborts on the package check — measured on `.124` on 2026-09-28 with
`open 192.168.1.196:7807`:

```
Warning: Failed to load 'UnrealI': Package 'UnrealI' version mismatch
```

The 227k server advertises `\mingamever\224`, which reads as "224 and up are
welcome". **It is only the version-number floor**; the package *generation*
check runs regardless, and 227's `Engine.u`/`Core.u`/`UnrealI.u` are a later
generation (227k `Engine.u` 3,699,955 B vs the staged 1,033,475 B). This had
been measured once already on 2026-08-31 (`retro-3dfx/FINDINGS.md`, "the 227k
trap") and the 227k unit was installed anyway the next day, with a comment in
`gameservers.py` claiming "the staged 227k client joins it" — there is no
staged 227k client. The probe then checked only that the server *answered*,
and it answered perfectly.

So the probe now checks the answer: `probe_unreal226` reports the `gamever` it
saw, and a reply that is not 226 comes back **not up**, with the reason, and
the watchdog does not restart it (a restart brings back the same version).

Upgrading the clients to 227k instead is not an option: the 227k Windows build
is SSE2, which would cost `.124`, `.133` and `.143` the game outright.

Unreal 226 has no Linux build, and running the staged tree's own `UCC.exe`
has a second advantage: the server's packages are **the same files** the
clients have, so every package GUID matches by construction.

## Install / re-install

```bash
bash scripts/game-servers/unrealgold/install.sh            # copy tree + ini + unit
bash scripts/game-servers/unrealgold/install.sh --no-copy  # ini + scripts + unit only
```

It copies the tree from the read-only `/mnt/retro-share` mount, **verifies the
copy by file count and byte total per directory** (a copy's exit code has lied
here before), derives `System\UnrealServer.ini` from the staged `Unreal.ini`
with `make_server_ini.py`, installs both unit files, disables the 227k one and
(re)starts this one. **Re-run it whenever the staged tree changes** — the
server must carry the same packages as the clients.

`make_server_ini.py` changes exactly four things from what every fleet box
receives: `[URL] Port=7807`; the three `IpServer.UdpServerUplink` ServerActors
(gamespy/epicgames/telefragged masters, dead for decades) are dropped while
`UdpBeacon` and `UdpServerQuery` stay; `[Engine.GameInfo] AdminPassword` (the
fleet's `retroadmin` convention); and `[Engine.GameReplicationInfo]
ServerName/ShortName`, the same name the 227k server advertised.

## Traps

* **The query port is not configured anywhere.** UdpServerQuery binds "the
  game port, or the next one free", i.e. 7808. The UT99 server's master
  uplinks already occupy 7800–7806 with the same next-free rule, so if 7808 is
  ever taken the query silently moves and the probe reads the server as down.
* **The LAN beacon needs 7775 to itself.** A 226 server that cannot bind it
  logs `ServerBeacon failed: Could not bind port 7775` and keeps running —
  joinable by address, invisible in the LAN browser. Look for
  `ServerBeacon listening on port 7775` in `server.log`.
* **`wine` returns at once** — the entry script backgrounds UCC and blocks on
  `wineserver -w`, as every Wine server here does.
* **No `xvfb-run`.** UCC is a console program and opens no window; without a
  display a fatal error exits (and systemd restarts it) instead of parking the
  server behind a dialog nobody can see.
* **Run as the tree's owner.** As root, UCC leaves root-owned `UCC.log` /
  `UnrealServer.ini` in `~/unrealgold-server`, and the next `install.sh`
  cannot rewrite them. `run-ug-server.sh` passes `--user $(id -u):$(id -g)`;
  wine then needs its prefix directory created first (`'/tmp' is not owned by
  you`), which `entry.sh` does.

## Verify (the post-condition, not the unit state)

```bash
RETRO_GAMESERVER_HOST=127.0.0.1 python3 scripts/game-servers/gameservers.py | grep 'Unreal Gold'
#  OK  Unreal Gold        active       0/12   36.4ms  DMDeck16  [gamever 226]
RETRO_GAMESERVER_HOST=127.0.0.1 python3 scripts/game-servers/healthcheck.py | grep unrealgold
```

A client joins with the console command `open <host>:7807`, or from the
in-game LAN tab (beacon on 7775).

## Rollback to 227k

The 227k install is untouched at `~/unreal-server/` and its unit file is kept,
disabled, as `unrealgold227-server.service` (`units/unrealgold227-server.service`).
Both bind 7807/7808/7775, so only one may run.

**Do not just `enable --now unrealgold227-server`.** The watchdog
(`retro-gameservers-watch`) restarts `unrealgold-server` the moment it reads
`inactive`, so disabling one unit and starting the other ends with two servers
fighting over the same ports. Put the 227k definition back under the name the
watchdog, the wall and `host-duties.py` all know instead:

```bash
install -m 0644 scripts/game-servers/units/unrealgold227-server.service \
        ~/.config/systemd/user/unrealgold-server.service
systemctl --user daemon-reload
systemctl --user stop unrealgold-server; docker stop -t 10 ugsrv 2>/dev/null
systemctl --user start unrealgold-server
```

(If the watchdog restarts it between `stop` and `start`, it starts the 227k
definition, and the final `start` is a no-op — either way 227k ends up
running.) Then set the `unrealgold-server` row in `gameservers.py` back to
`"probe": "unreal227"` (and `healthcheck.py`'s to `unreal227`), or
`probe_unreal226` will report the 227k server as `BAD` — correctly: no staged
client can join it. The watchdog will not restart a `BAD` server. To return to
226, re-run `install.sh --no-copy`.
