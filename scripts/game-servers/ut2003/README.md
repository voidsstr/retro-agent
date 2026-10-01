# `ut2003-server`: Unreal Tournament 2003 (2225), the staged tree's own UCC.exe under Wine

Added **2026-10-01**. It's the staged `Games-Library/UT2003` tree's own
**`System\UCC.exe`** (build **2225**, the last patch) over the same tree's
packages, run under Wine in the `retro-wine:bookworm` container. That's the
same shape as Unreal Gold (`../unrealgold/`), DOOM 3, Deus Ex and Serious Sam.

| | |
|---|---|
| unit | `ut2003-server` (`systemctl --user`, enabled; linger keeps it across reboots) |
| container | `ut2003srv` (`--net=host`, `--init`, runs as the tree's owner, not root) |
| command | `wine UCC.exe server "DM-Antalus?game=XGame.xDeathMatch?MinPlayers=6?MaxPlayers=12" ini=UT2003Server.ini -port=7757` |
| tree | `~/ut2003-server`: System, Maps, Textures, StaticMeshes, Animations, Sounds, KarmaData (2.6 GB). No Music, Help, Benchmark, Web or ForceFeedback |
| ports (UDP) | join **7757** · native browser query **7758** · LAN responder **10777** (shared with UT2004, see below) |
| log | `~/ut2003-server/server.log` (UCC's own log, via stdout) |
| probe | `gameservers.py` `probe_ut2003`: UE2 native query on 7758; net version must be **121** and `ServerVersion` **2225** |
| bots | `MinPlayers=6` (fills to six, like UT2004), DM rotation over all 15 stock DM maps |
| admin | `retroadmin` (`[Engine.AccessControl] AdminPassword`) |

## Why the staged UCC.exe, and why these ports

**A UE2 client joins only its own build.** Every native browser reply opens
with an int32 net version: **121** is UT2003 2225 and **128** is UT2004 3369.
Epic's Linux `ut2003-lnxded` is not on the share. The staged tree's own
`UCC.exe` is guaranteed to match the staged client: the same files, the same
package GUIDs.

**A UE2 server holds three UDP ports: game, game+1 (native query) and game+10
(GameSpy query).** Measured with `ss -ulpn` on 2026-10-01: `ut2004-server`
holds 7777, 7778 (two sockets) and **7787**. So the obvious "7787/7788" for
UT2003 collides with UT2004's GameSpy port. Base **7757** gives 7757/7758/7767,
all free and clear of UT99 (7797-7806, 8777), Unreal Gold (7807/7808, beacon
7775) and Deus Ex (7790/7791). This server doesn't uplink to GameSpy, so 7767
is not bound today. It's kept free so turning the uplink on later cannot collide.

## LAN visibility beside UT2004 (it works, and here's why)

The UT2003 and UT2004 clients both find LAN servers by broadcasting the
native query to **UDP 10777** (`[IpDrv.MasterServerLink] LANServerPort`).
Both servers bind 10777 on 0.0.0.0 with `SO_REUSEADDR`, and Linux delivers a
**broadcast** datagram to every socket bound that way. Measured: one broadcast
of `80 00 00 00 00` to `192.168.1.255:10777` drew two replies: the UT2003
server (netver 121, from :7758) and the UT2004 server (netver 128, from
:7778). Each client keeps the replies of its own build. Neither server stole
the port from the other, and UT2004's responder stayed healthy.

The server keeps `ServerActors=IpDrv.MasterServerUplink` because the LAN
responder lives in that actor. It runs whatever `DoUplink` is set to (gated on
`LANServerPort >= 0` in IpDrv.u). `make_server_ini.py` refuses a staged ini
without that actor, or with a `LANServerPort` other than 10777.

**The reply's source address.** The host is multi-homed (`enp129s0` .132 +
.196, WiFi .129, all on 192.168.1.0/24). A fleet client's broadcast is answered
from the wired NIC's primary address, so its LAN tab may list the server as
**192.168.1.196:7757** rather than .132. Either works: UCC binds 0.0.0.0. To
join by address, use `open 192.168.1.132:7757` in the console, or
`UT2003.exe 192.168.1.132:7757`.

## Query protocol (what the probes speak)

UE2 native, to **game+1 = 7758**: `80 00 00 00 <type>`.

| type | reply |
|---|---|
| 0 | `<int32 netver><byte 0><int32 ServerID><FString ip><int32 port><int32 qport><FString name><FString map><FString gametype><int32 players><int32 max>` |
| 1 | rules as FString pairs, including `ServerVersion 2225` and `minplayers 6` |
| 2 | player list: **no reply at all** with only bots on (measured); not yet measured with a human |

- **The type-0 player count includes bots.** An empty server reads 6/12.
  `probe_ut2003` reports `players` = everyone and `bots` = total minus the
  listed players, the same convention as GoldSrc (`collect()` derives humans
  as the difference).
- **There's no GameSpy `\status\`** (game+10): it exists only with
  `DoUplink`+`UplinkToGamespy`, and this server uplinks to nothing.
  `healthcheck.py`'s UT2004 probe (`\status\` on 7787) does NOT apply here.
- `scripts/gameindex/masters.py` already speaks the same native query
  (`_ut2k3_native_probe`). The favourites agent's `LOCAL_SERVERS` row has
  **no `query_port`** on purpose, so that native probe (with its netver-121
  filter) is used.

## Install / re-install

```bash
bash scripts/game-servers/ut2003/install.sh            # copy tree + ini + unit
bash scripts/game-servers/ut2003/install.sh --no-copy  # ini + scripts + unit only
```

It copies from the read-only `/mnt/retro-share` mount, **verifies the copy by
file count and byte total per directory**, checks every rotation map exists in
the tree, derives `System/UT2003Server.ini` from the staged `UT2003.ini` with
`make_server_ini.py`, then installs, enables and restarts the unit. The staged
`UT2003.ini` is never edited; the server reads its own `UT2003Server.ini`.

## Traps

* **`du` says the staged tree is 106 GB. It's 2.6 GB.** The CIFS mount
  (`/mnt/retro-share`) reports **262,144 blocks (128 MiB) allocated for every
  file**, whatever its size: 848 files × 128 MiB ≈ 106 GiB. Every title on the
  share does this (UT2004, UnrealGold and Doom3 files read the same). Use
  `du --apparent-size`. Nothing is wrong with the library.
* **`wine` returns at once.** The entry script backgrounds UCC and blocks on
  `wineserver -w`, as every Wine server here does.
* **No `xvfb-run`.** UCC is a console program. Without a display, a fatal
  error exits (and systemd restarts it) instead of hiding behind a dialog.
  (If you ever do need `xvfb-run` in this image, pass `docker run --init`:
  as PID 1 it never receives Xvfb's ready signal and hangs forever, which
  cost time on 2026-10-01.)
* **Run as the tree's owner** (`--user $(id -u):$(id -g)`), or UCC leaves
  root-owned logs and ini behind that `install.sh` cannot rewrite.
* **The staged client cannot be smoke-tested under Wine on this host.**
  `UT2003.exe` under the `retro-wine` image stops at *"Please install DirectX
  8.1b or later"*, even with `HKLM\Software\Microsoft\DirectX\Version` set.
  The join test has to happen on a fleet box.

## Verify (the post-condition, not the unit state)

```bash
RETRO_GAMESERVER_HOST=127.0.0.1 python3 scripts/game-servers/gameservers.py | grep UT2003
#  OK  UT2003             active       6/12    7.0ms  DM-Antalus  [gamever 2225]
RETRO_GAMESERVER_HOST=127.0.0.1 python3 scripts/game-servers/healthcheck.py | grep ut2003
#  [ OK ] ut2003-server  UT2003 (query 7758)  :7758  NSC Retro Fleet Arena (UT2003) | map=DM-Antalus | 6/12 (incl. bots) | netver=121
```

## Open item

**A fleet client hasn't joined yet.** On 2026-10-01 the only box online with
UT2003 installed was `.124`, which was reserved for other work (the LAN had
been re-cabled that day and most of the fleet was off). The proof still owed:
from a box with the staged tree, `UT2003.exe 192.168.1.132:7757` (or the LAN
tab), a fullscreen screenshot in-game, and a type-2 player-list capture with
that human on the server, to confirm `parse_ue2_players`.
