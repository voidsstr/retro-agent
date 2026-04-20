# Linux Game Servers

Install scripts for four retro-era dedicated game servers that run on the
NSC dev box so the Win98/XP retro fleet (and anyone on the public internet)
can connect to them.

| Game | Engine | UDP ports | Masters |
|---|---|---|---|
| Unreal Tournament 2004 | Epic dedicated server 3369.3 (amd64 Linux binary) | 7777 / 7778 / 7787 | `utmaster.openspy.net`, `ut2004master.333networks.com`, `ut2004master.errorist.eu` (via MasterServerMirror mod) |
| Unreal Tournament 99 | OldUnreal 469e (amd64 Linux) | 7797 / 7798 (shifted from 7777 to avoid UT2004 collision) | `master.333networks.com`, `master.oldunreal.com` ×2, `master.errorist.eu`, `master.openspy.net`, `master.qtracker.com`, `master.hypercoop.tk`, `master.telefragged.com` — all shipped in 469e default ini |
| Quake 2 | Yamagi Quake 2 (apt) | 27910 | `master.yamagi.org`, `master.quakeservers.net`, `master.q2servers.com` |
| Quake 3-compatible | OpenArena (ioq3 engine, apt) | 27960 | `dpmaster.deathmask.net`, `master.ioquake3.org`, `master3.idsoftware.com` |

All four run as **systemd user units** owned by the installing user, with
`Restart=always`. `loginctl enable-linger` keeps them running through logout
and reboots.

## Quick start

```bash
cd /path/to/retro-agent/scripts/game-servers

# One of these requires ~1.5 GB of download and a few sudo prompts
# (apt install, ufw allow, enable-linger, disable conflicting system unit)
./install-all.sh

# …or install individually:
./install-ut2004-server.sh
./install-ut99-server.sh
./install-quake2-server.sh
./install-openarena-server.sh
```

After each install you'll see the path the logs are written to, the configured
hostname, and the expected listen ports.

## What each script does

### `install-ut2004-server.sh`

1. Downloads the Epic dedicated server 3369.3 bonus-pack bundle from
   [archive.org](https://archive.org/details/ut2004-server) (~724 MB zip,
   cached at `~/.cache/game-servers/ut2004.zip` — re-runs skip the download).
2. Extracts to `~/ut2004-server` using Python's `zipfile` module — Info-Zip's
   `unzip` can't handle the LZMA compression method (14) the archive uses.
3. Downloads the [MasterServerMirror mod](https://github.com/0xC0ncord/MasterServerMirror)
   into `System/` and wires it into `ServerActors`. The built-in
   `IpDrv.MasterServerUplink` still tries the dead Epic masters forever — that's
   just log noise, not a functional problem.
4. Patches `UT2004.ini`:
   - Strips the 4 stray `0x1B` (ESC) bytes Epic shipped inside some maplist
     section headers. Without this, any section-header string match fails.
   - Sets `ServerName`, `AdminPassword`, `MaxPlayers=12`, `MinPlayers=6`
     (6 bots backfill an empty server), `ServerBehindNAT=True`.
   - Rewrites `[DefaultDM MaplistRecord]` and `[XInterface.MapListDeathMatch]`
     with a 30-map stock rotation.
5. Adds UFW allow for UDP 7777–7787 if UFW is active.
6. Enables `loginctl linger` if not already on.
7. Installs and starts `ut2004-server.service` as a user unit.

### `install-ut99-server.sh`

1. Pulls base UT99 game data (Maps, Textures, Music, Sounds, System — ~830 MB)
   from the configured SMB share, using `share-inventory.json` from the
   nsc-assistant repo as the file manifest.
2. Overlays the [OldUnreal 469e Linux amd64 patch](https://github.com/OldUnreal/UnrealTournamentPatches/releases)
   on top. 469e (released Nov 2025) is actively maintained, still supports
   WinXP clients via the separate `OldUnreal-UTPatch469e-WindowsXP-x86.zip`,
   and is cross-compatible with legacy Epic 436/451 clients.
3. Runs `ucc-bin-amd64 help` once to initialize `~/.utpg/System/` with the
   default `UnrealTournament.ini` (which already lists 8 community master
   servers — 333networks, OldUnreal ×2, errorist.eu, OpenSpy, qtracker,
   hypercoop, telefragged — so no master-mirror mod is needed).
4. Patches `UnrealTournament.ini`: `ServerName`, `AdminPassword`,
   `Port=7797` (shifted from 7777 to coexist with the UT2004 server),
   `FragLimit=25`, `TimeLimit=15`, `MinPlayers=6`, `MaxPlayers=12`. Also
   **removes `ServerPackages=TCowMeshSkins / TNaliMeshSkins / TSkMSkins`**
   from the default list because those Epic bonus-pack mesh skins aren't
   shipped with the stock install — leaving them in aborts startup with
   `"Failed to load TCowMeshSkins"`.
5. UFW allow UDP 7797–7798, systemd user unit `ut99-server.service`.

### `install-quake2-server.sh`

1. `apt install yamagi-quake2` (amd64 engine — no 32-bit libs needed).
2. Pulls `pak0.pak` / `pak1.pak` / `pak2.pak` from the configured SMB share
   (`Games/Windows 9x/Quake II v3.20/baseq2/` by default) into
   `~/q2-server/baseq2/`.
3. Writes `~/.yq2/baseq2/server.cfg` (Yamagi's config search path) and mirrors
   it into the datadir. Note: Yamagi **rejects** the `serverinfo` flag suffix
   on `set` that stock Q2 accepted — `set x "val" serverinfo` is invalid;
   plain `set x "val"` works.
4. UFW allow UDP 27910, systemd user unit `quake2-server.service`.

### `install-openarena-server.sh`

1. `apt install openarena-server openarena-data`.
2. **Disables the system-wide `openarena-server.service`** that the Debian
   package auto-enables. Without this, two ioq3ded instances race for UDP
   27960 and the status query will come from whichever one bound the socket
   first — usually the stock-config one. This is the cause of the `sv_hostname
   "noname" fraglimit 20 timelimit 0 sv_maxclients 8` symptom.
3. Writes `~/q3-server/.openarena/baseoa/server.cfg` with hostname, rotation,
   and 3 community masters. The wrapper `/usr/games/openarena-server` runs
   `ioq3ded` with `com_basegame=baseoa` and `com_homepath=.openarena`, so
   from the `WorkingDirectory=~/q3-server` systemd sets, it finds
   `.openarena/baseoa/server.cfg` automatically.
4. UFW allow UDP 27960, systemd user unit `openarena-server.service`.

## Pushing the UT99 XP patch to the retro fleet

When you install the Linux UT99 server (469e), the retro XP machines should
be upgraded to the matching WinXP 469e client so they can connect to both this
server *and* other community servers. The XP-specific client patch is staged
on the SMB share at
`\\192.168.1.122\files\Game Updates\UT99-Multiplayer\OldUnreal-UTPatch469e-WindowsXP-x86.zip`.

```bash
cd /path/to/retro-agent
python3 scripts/game-servers/push-ut99-xp-patch.py 192.168.1.143 192.168.1.133 192.168.1.123 192.168.1.124
```

The script pulls the zip from the share to each agent's `C:\WINDOWS\TEMP\`,
extracts it via a small JScript shim (XP's PowerShell is 2.0 so no
`Expand-Archive`), then either runs the setup.exe silently (`/S`) or
xcopies the unpacked tree onto `C:\UT\`. 469e is cross-compatible with
legacy 436/451 clients, so the upgrade is non-disruptive — existing UT99
installs just get newer `.u` and renderer DLLs.

## Why OpenArena and not real Quake 3?

Quake 3 Arena's `pak0.pk3`–`pak8.pk3` are id/Activision proprietary game data.
OpenArena is a standalone game that uses the same Q3 engine (ioquake3) with
free/libre replacement assets — and it registers on the same community
masters. Real Q3 clients can connect to OpenArena servers by passing
`+set com_legacyprotocol 71`.

If you want to run *actual* Q3A content on your server, drop `pak0.pk3` from
your Q3 install into `~/q3-server/.openarena/baseq3/`, change the
`com_basegame` in the wrapper to `baseq3`, and rename `baseoa` → `baseq3`
throughout `server.cfg`. Community masters will accept either.

## Router port-forward

All three games need **UDP** ports forwarded. TCP alone won't work — the
status query, game traffic, and master probes are all UDP.

If you have an AT&T BGW-series gateway and there's more than one interface
on your server (Wi-Fi + Ethernet), be aware that the NAT/Gaming rule binds
by MAC, and AT&T's device list may show **two entries with the same hostname**
(one per NIC). Pick the wired-adapter MAC explicitly — picking the Wi-Fi
one creates asymmetric routing that masters silently drop.

## Environment variables

All scripts honor these (via `common.sh`):

| Var | Default | Meaning |
|---|---|---|
| `INSTALL_ROOT` | `$HOME` | Parent directory for server installs |
| `SERVER_HOSTNAME_PREFIX` | `NSC Retro Fleet Arena` | Appended with ` (Q2)`, ` (OA)` |
| `SERVER_ADMIN_PASSWORD` | `retroadmin` | UT2004 admin password |
| `SMB_HOST` / `SMB_USER` / `SMB_PASS` / `SMB_SHARE` | `192.168.1.122` / `admin` / `password` / `files` | SMB source for Q2 paks |
| `Q2_PAKS_REMOTE_DIR` | `Games/Windows 9x/Quake II v3.20/baseq2` | SMB subpath to Q2 paks |
| `UT2004_ZIP_URL` | archive.org 3369.3 | Alternate UT2004 download |
| `MASTERMIRROR_VERSION` | `v1.0.0` | UT2004 master-mirror mod release |

## Verifying a server is reachable from the internet

```bash
# UT2004 gamespy query (should reply ~270 bytes with "\hostname\...")
printf '\\status\\' | nc -u -w3 <public.ip> 7787 | head -c 400

# Quake 2 status
printf '\xff\xff\xff\xffstatus\n' | nc -u -w3 <public.ip> 27910 | head -c 400

# OpenArena / Q3 getstatus
printf '\xff\xff\xff\xffgetstatus' | nc -u -w3 <public.ip> 27960 | head -c 400
```

Or use a public docker container so the probe egresses through your NAT:

```bash
docker run --rm python:3-alpine python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
s.sendto(b'\\\\status\\\\', ('<your.public.ip>', 7787))
print(s.recv(4096)[:400])
"
```

If probes succeed but master-server web listings stay empty for 30+ minutes,
the masters may have de-registered the server during an earlier reachability
failure. Restart the user unit to force a fresh uplink:

```bash
systemctl --user restart ut2004-server.service
```

## Logs and inspection

```bash
# systemd
systemctl --user status ut2004-server.service quake2-server.service openarena-server.service
journalctl --user -u ut2004-server.service -n 50

# per-server logs
tail -f ~/ut2004-server/server.log
tail -f ~/q2-server/server.log
tail -f ~/q3-server/server.log
```
