# Game servers — they run on the DEV HOST (192.168.1.132)

> ### 2026-08-24: the game servers moved OFF whitebeast, back onto the dev host
>
> **All fleet game servers are hosted on the Linux dev host `192.168.1.132`**, as
> `systemctl --user` units (user lingering is on, so they start at boot without a
> login). **whitebeast (192.168.1.82) hosts nothing** — its `C:\gameservers`
> trees are still on disk but no process is running, and it has **no autostart**
> for them (no Run key, no scheduled task, no Startup shortcut), so they do not
> come back on their own.
>
> Everything whitebeast used to serve now runs here: CS 1.6 vanilla, CS 1.6
> no-blood, and UT99. The whitebeast section below is kept as **history** — do
> not use it as the current layout.


## Watchdog + status (`gameservers_watch.py`)

`healthcheck.py` answers "is it up" for a human at a terminal. **`gameservers.py`
answers the same question for a machine** — up/down, players, bots, map, query
RTT and systemd unit state — and **`gameservers_watch.py`** runs it on a loop as
a service that both publishes that status and restarts what has died.

```bash
python3 scripts/game-servers/gameservers.py             # table, like healthcheck.py
python3 scripts/game-servers/gameservers.py --json      # machine-readable
python3 scripts/game-servers/gameservers_watch.py --once --stdout   # safe: never restarts

cp scripts/game-servers/retro-gameservers-watch.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now retro-gameservers-watch
```

The status blob lands in `$XDG_RUNTIME_DIR/retro-gameservers/status.json`, which
the login-screen dashboard collector reads (`dashboard/README.md`). Per-user
tmpfs, readable by root, and **not** `/tmp` — the GDM greeter that ultimately
displays it runs as a systemd `DynamicUser` and cannot see `/tmp` at all.

### Two process managers, declared not assumed

Nine servers are `systemd --user` units; **Tribes 2 is a docker container**
(it needs a 2001 userland). Asking systemd about `tribes2-server` returns
`not-found`, which this module reports as "never installed here" — so a
running game server was being dropped off the board entirely, and an outage on
it would have been invisible. Each row therefore declares its `manager`, state
is fetched with one call per manager, and restarts dispatch accordingly
(`systemctl restart` vs `docker restart`).

The three-way distinction is what makes it safe:

| what we found | means | counted? | restarted? |
|---|---|---|---|
| `active` / `failed` / `inactive` | the manager answered | yes | per policy |
| `absent` | never installed on this host | no | never |
| `unknown` | we could not ask (no docker binary, daemon down) | no | never |

`unknown` existing separately matters: restarting on the strength of a failed
*lookup* is how a watchdog starts bouncing healthy services because docker was
briefly busy.

### Two facts, deliberately kept apart

Per server the watchdog records the **unit** state from systemd *and* the
**probe** result from the game's own query protocol. A server can be `active`
and mute (wedged, or mid map-change), and it can be `inactive` for the honest
reason that it was never installed here. A watchdog that cannot tell those
apart either restarts healthy servers or ignores dead ones.

### Restart policy

- `failed`/`inactive` → restart immediately (systemd already knows it is gone).
- `active` but mute for **3 consecutive cycles** → restart. One silent cycle is
  a map change; restarting on it would kick everyone off a healthy server.
- **5-minute cooldown** per unit, **4 restarts per hour** maximum. A server
  broken for a reason a restart cannot fix (missing pak, bad cfg) is left with
  "needs a human" rather than flapped forever.
- Never installed → never touched, and not counted in up/total.
- `--no-restart` (or `RETRO_GAMESERVERS_NO_RESTART=1`) watches without acting;
  `--once --stdout` never restarts, so it is safe to run from a terminal.

Every decision, **including every decision not to act**, is logged with its
reason. A silent skip and a successful restart must never look the same.

### Bots are not players

A Quake III server pinned at `bot_minplayers 4` reports four players forever.
GoldSrc's A2S reply carries a bot count directly; on the Quake family the tell
is **ping 0** in the player line. Without that separation the dashboard would
permanently claim someone was playing.

### Reply layouts (all verified against the live servers)

| engine | query | where the numbers are |
|---|---|---|
| GoldSrc (CS 1.6, TS) | `TSource Engine Query` | after 4 NUL-terminated strings + u16 appid: `players`, `maxplayers`, `bots`. May first answer `A` + a 4-byte challenge that **must** be echoed back |
| Quake III | `getstatus` | infostring on **line 1** (line 0 is `statusResponse`); one line per player |
| Quake 2 | `status` (not `getstatus`) | infostring on **line 1** (line 0 is `print`) |
| QuakeWorld (mvdsv) | `status` | infostring on **line 0** — the `n` header is glued to the first key. The reply ends `\n\x00`, and `str.strip()` does not remove a NUL, so a naive line count reports one phantom player |
| UT99 / UT2004 | `\status\` on **game port + 1** | `numplayers` / `maxplayers` given directly |
| Tribes 2 | Torque binary `0x0E` → `0x10` | **liveness only, and not by choice** — under TribesNext the info response body is encrypted (`0x12` returns a well-formed `0x14` full of ciphertext). The reply *does* echo the request's four key bytes, so we send a random key and check it comes back: that proves the packet answers *our* query rather than being any UDP traffic that happened to arrive |

Tests: `tests/python/test_gameservers.py` (parsers against captured bytes, and
every bound of the restart policy).

## What runs here (verify with `healthcheck.py`)

| Server | Unit | Port (UDP) | Install root |
|---|---|---|---|
| CS 1.6 vanilla | `cs16-server` | **27018** (browser via proxy **27015**) | `~/hlds-cs16` |
| CS 1.6 no blood | `cs16-noblood` | **27019** (browser via proxy **27016**) | `~/hlds-cs16-noblood` |
| The Specialists | `specialists-server` | **27017** | `~/hlds-ts` |
| Quake III Arena | `quake3-server` | **27961** | `~/q3a-server` |
| OpenArena | `openarena-server` | **27960** | `~/q3-server` |
| Quake 2 | `quake2-server` | **27910** | `~/q2-server` |
| QuakeWorld | `quakeworld-server` | **27502** | `~/qw-server` |
| UT99 (469e) | `ut99-server` | **7797** (query 7798) | `~/ut99-server` |
| UT2004 | `ut2004-server` | **7777** (query 7787) | `~/ut2004-server` |
| Tribes 2 | `tribes2-server` (docker) | **28000** | `retro-agent-private/.../tribes2-docker` |

**Tribes 2 must be built with the legacy builder**: `docker build --network=host
-t nsc-tribes2-tribesnext:latest .` then `docker compose up -d --no-build`. Under
buildkit its apt step hangs indefinitely with zero bytes fetched — see that
directory's README.

`bash`-free one-shot health check of every server, each with the query packet its
own engine actually answers:

```bash
python3 scripts/game-servers/healthcheck.py     # exit 0 = all up
```

**Probe each engine with its own query — a single protocol gives false "down".**
Quake 2 answers `status`, not `getstatus`; UT answers GameSpy `\status\` on
**game port + 1**; Tribes 2 answers only the Torque binary query
(`0E 00 00 00 00 00`). `healthcheck.py` encodes all of this.

### The CS servers sit behind an A2S proxy — that is deliberate

Modern HLDS answers the browser query with an anti-reflection challenge that old
CS 1.6 clients (the fleet's "BCS Romania" build) never echo back, so the server
shows as *Not Responding* in the LAN tab. `a2s_oldquery_proxy.py` takes the
canonical port and does the challenge dance on the client's behalf:
**27015 → 27018** (vanilla) and **27016 → 27019** (no blood).

> The proxy must target the address HLDS actually **bound**. Both CS units run
> `-ip 192.168.1.132` (never `0.0.0.0`) on this multi-homed host, so a proxy
> pointed at `127.0.0.1` comes up fine and then never answers. The shipped
> `a2s-proxy-cs16-public.service` had exactly that bug and was fixed 2026-08-24.

### Gotcha: 2007-era Half-Life mod `.so` files need their exec-stack flag cleared

`specialists-server` crash-looped with:

```
LoadLibrary failed on ts/dlls/ts_i386.so: cannot enable executable stack
  as shared object requires: Invalid argument
Host_Error: Couldn't get DLL API from ts_i386.so!
```

`ts_i386.so` (built 2007) has **no `PT_GNU_STACK` program header at all**, so the
kernel assumes it wants an executable stack — and current kernels refuse to grant
one at `dlopen` time. It is not a corrupt install and not a Steam problem. Fix,
once, per `.so`:

```bash
patchelf --clear-execstack ~/hlds-ts/ts/dlls/ts_i386.so    # adds PT_GNU_STACK RW
patchelf --clear-execstack ~/hlds-ts/ts/dlls/ts_i686.so
```

Originals are kept beside them as `*.so.orig`. Expect the same failure on any
other pre-2008 HL mod game DLL.

### Not installed on this host

`rtcw-server` and `mohaa-server` appear in the game-servers skill's table but
have **never existed here** — no install directory, no install script, and no
retail game data staged. Treat those rows as a wish list, not as something that
regressed.

They are deliberately **absent from `gameservers.py`'s table** rather than
listed-and-absent: a row that can never come up would sit permanently on the
status wall looking like an outage, and "we never built this" is not a fault
report. Add them there on the day they are actually installed.

---

## Publishing to the internet so public players can find the servers

**Status as of 2026-08-25: zero servers are publicly visible.** Every server is
healthy on the LAN and every master uplink is configured correctly — the gateway
simply is not forwarding inbound UDP, so the masters cannot probe us back and
therefore will not list us. Check any time with:

```bash
python3 scripts/game-servers/publish_status.py     # exit 0 = at least one listed
```

That asks each game's real master whether our public IP is in its list — a
different question from `healthcheck.py`, which only proves the server is up on
the LAN.

### Cloudflare Tunnel CANNOT publish these servers — do not try again

**cloudflared does not carry UDP, and every one of these servers is UDP-only.**
Its ingress types are `http`, `https`, `tcp`, `ssh`, `rdp`, `smb`, `unix` and
`http_status` — there is no `udp` service type. Cloudflare's UDP story is
WARP-to-Tunnel private networking, which requires the **WARP client on the
player's machine**; a stranger running a stock Quake 3 or CS 1.6 binary has no
way to reach a tunnel. Cloudflare Spectrum does UDP only on **Enterprise**.

The `~/.cloudflared/` credential on this host belongs to
`config-ollama.yml` → `ollama-failover.aisleprompt.com` → `http://localhost:11434`.
That is the **Ollama HTTP failover tunnel and has nothing to do with the game
servers.** It is almost certainly the "cloudflare tunnel we configured" being
remembered — there has never been a game-server tunnel, and there cannot be a
useful one.

### What actually works: UDP port-forwards on the gateway

The gateway is an **AT&T BGW at 192.168.1.254**; its admin UI needs the **Access
Code printed on the router label**. **UPnP is disabled** on it (verified by SSDP
M-SEARCH — no IGD responds), so the forwards cannot be added programmatically;
someone has to enter them in `Firewall → NAT/Gaming → Custom Services`.

Forward **UDP** from the WAN to **192.168.1.132** (the wired NIC, `enp129s0`):

| Port(s) | Server | Needed for |
|---|---|---|
| 7777, 7778, 7787 | UT2004 | game, browser, GameSpy query |
| 7797, 7798 | UT99 | game, query |
| 27910 | Quake 2 | game + master probe |
| 27960 | OpenArena | game + master probe |
| **27961** | **Quake III Arena** | game + master probe |
| **27502** | **QuakeWorld** | game + master probe |
| **28000** | **Tribes 2** | game + master probe |

> The last three were **missing from the old forward table** — Q3A, QuakeWorld
> and Tribes 2 were never forwarded, so they could never have listed even when
> the rest worked.

CS 1.6 (27015–27019) and The Specialists (27017) are deliberately **not** in the
list — see below.

**MAC-binding trap (this has bitten before):** BGW NAT rules bind by **MAC, not
IP**. Pick the *wired* MAC. If the host ever shows twice in the device list,
the Wi-Fi entry creates asymmetric routing — inbound to the Wi-Fi IP, replies out
the wired IP with a different NAT source port — and masters silently drop the
listing. Rules can also spontaneously re-bind to an unrelated device when DHCP
state shifts; if listings vanish, re-check `/cgi-bin/apphosting.ha`.

### Where each server publishes itself (already configured, verified 2026-08-25)

| Server | Masters / listing | Configured |
|---|---|---|
| Quake III Arena | `dpmaster.deathmask.net`, `master.ioquake3.org`, `master3.idsoftware.com` | ✅ 4 masters + `dedicated 2` |
| OpenArena | `dpmaster.deathmask.net`, `master.ioquake3.org` | ✅ + `dedicated 2` |
| Quake 2 | `master.quakeservers.net`, `master.q2servers.com` | ✅ + `public 1` |
| QuakeWorld | `master.quakeworld.nu`, `master.quakeservers.net`, `qwmaster.ocrana.de` | ✅ `setmaster` |
| UT99 | `master.333networks.com`, `master.oldunreal.com`, `master.errorist.eu`, … | ✅ `DoUplink=True` |
| UT2004 | `ut2004master.333networks.com`, `utmaster.openspy.net` | ✅ + `ServerBehindNAT=True`, MasterServerMirror |
| Tribes 2 | `master.tribesnext.com` (outbound HTTP heartbeat) | ✅ |

Nothing here needs changing. These community masters are what the public server
browsers and the tracker websites (333networks, deathmask.net, quakeservers.net,
tribesnext) scrape, so a correct forward is the *only* missing piece.

### CS 1.6 and The Specialists are LAN-only on purpose

Both run `sv_lan 1`, which disables Steam authentication — that is precisely what
lets the fleet's non-Steam **"BCS Romania"** clients join. Going public means
`sv_lan 0` **plus a Steam Game Server Login Token (GSLT)**, and that **breaks
every non-Steam client on the retro fleet**. It is a real either/or, not an
oversight. Decide deliberately before changing it; if you do want a public CS
server, run it as a *third* instance on its own port rather than converting one
the fleet depends on.

### If the router genuinely cannot be used

A UDP relay (a cheap VPS running WireGuard + `iptables` DNAT, or a service like
playit.gg) makes the servers *connectable by address*, but public **master
listing usually still breaks**: the master records the source IP of the heartbeat
(your real WAN IP) and probes that, not the relay. So a relay is a fallback for
"friends connect by address", not a substitute for a port-forward if the goal is
strangers discovering you in a server browser.


# History — the whitebeast (F:/C:) era

*Everything below describes the old whitebeast layout and is retained only so the
2026-08 notes stay readable. It is NOT the current setup.*

`whitebeast` (**192.168.1.82**, Windows 11) has taken over from the old box that
ran the game servers. This directory holds the configs, mods and notes for what
runs there.

> **Servers run natively on Windows, never in WSL.** WSL2 here is in **NAT**
> mode (`172.19.188.220/20`), so anything bound inside WSL is unreachable from
> the 192.168.1.0/24 fleet, and `netsh portproxy` cannot help because it is
> **TCP-only** while GoldSrc is UDP. Windows has the LAN address; the servers
> belong there.

## What runs (2026-08-20 rebuild — F: died, everything on C: now)

The F: volume no longer exists; both CS trees were rebuilt from SteamCMD at
`C:\gameservers\` and a UT99 dedicated server was added (469e, from the
share's `Unreal Tournament (Installed)` tree + OldUnreal 469e patch).

| Server | Port | Install root | Config in this repo |
|---|---|---|---|
| CS 1.6 vanilla | UDP **27018** | `C:\gameservers\cs16-vanilla` | [`cs16-vanilla/cfg/server.cfg`](cs16-vanilla/cfg/server.cfg) |
| CS 1.6 no blood | UDP **27017** | `C:\gameservers\cs16-noblood` | [`cs16-noblood/`](cs16-noblood/) |
| UT99 (469e) | UDP **7777** game / 7778 query / 8777 LAN beacon | `C:\gameservers\ut99` | ini configured in place |

**Why 27018 and not 27016**: launching hlds via WSL interop or schtasks on
this box produces *unkillable zombie processes* (`taskkill /F` reports "no
running instance", children die, parent survives) that pin their UDP port
until reboot. 27015/27016/27019 are pinned by such corpses as of 2026-08-20.
We standardized on 27018/27017 permanently — do not move back after a reboot,
the fleet's favorites now point here.

**The only safe launch contexts**: (a) an elevated interactive PowerShell run
by the logged-in user — i.e. `start-game-servers.ps1`; (b) `EXEC cmd /c start
"" /min /D <dir> ...` through the Windows retro_agent (it executes in the
interactive session). Both verified working 2026-08-20.

**noblood tree must be its own SteamCMD install** (or validate-passed): a
file-copy of the vanilla tree carries its Steam identity and the second
instance dies with `FATAL ERROR ... Unable to initialize Steam`. After ANY
`app_update`/validate, re-point `liblist.gam` `gamedll` at metamod (the
validate reverts it silently) — see cs16-noblood/README.md.

Run everything with **`start-game-servers.ps1`** in this directory (elevated,
idempotent, verifies via A2S/\status\ loopback queries).

## What ran historically (F: era)


| Server | Port | Install root | Config in this repo |
|---|---|---|---|
| CS 1.6 vanilla | UDP **27016** | `F:\gameservers\cs16-vanilla` | [`cs16-vanilla/cfg/server.cfg`](cs16-vanilla/cfg/server.cfg) |
| CS 1.6 no blood | UDP **27017** | `F:\gameservers\cs16-noblood` | [`cs16-noblood/`](cs16-noblood/) |

Both are stock HLDS (SteamCMD app 90) with `sv_lan 1`. Two **separate install
trees**, not one tree with two game dirs — that keeps `logs/`, `banned.cfg`,
`liblist.gam` and the AMXX config independent, and it is the only sane way to
have one instance Metamod-hooked and the other not. Disk is cheap on F:.

Both instances must run with **`-game cstrike`**. A different game directory
would make stock clients think it is a mod they do not have; the no-blood
variant is a *server-side* change precisely so the client stays vanilla.

## Starting them

```powershell
Start-Process -FilePath 'F:\gameservers\cs16-vanilla\hlds.exe' `
  -WorkingDirectory 'F:\gameservers\cs16-vanilla' `
  -ArgumentList '-console','-game','cstrike','-port','27016','-maxplayers','16','+map','de_dust2' `
  -WindowStyle Minimized

Start-Process -FilePath 'F:\gameservers\cs16-noblood\hlds.exe' `
  -WorkingDirectory 'F:\gameservers\cs16-noblood' `
  -ArgumentList '-console','-game','cstrike','-port','27017','-maxplayers','16','+map','de_dust2' `
  -WindowStyle Minimized
```

### Do NOT redirect hlds.exe's stdout

`hlds.exe -console` needs a real console. Launching it from a `.bat` that does
`hlds.exe ... > log.txt 2>&1` makes it abort into a **"Microsoft Visual C++
Runtime Library"** dialog at `BreakpadMiniDumpSystemInit` and hang there
forever. This cost a long debugging detour and looks exactly like a corrupt
install. Launch it with a console (as above) and read the server's own logs in
`cstrike\logs\` instead.

Worse, each hung instance **keeps its UDP port bound after the process has
exited** (`HasExited=True` but still `OwningProcess` on the endpoint, because
the parent `cmd.exe` holds a handle). That is why the vanilla server is on
**27016 and not the usual 27015** — 27015 is pinned by such a corpse and will
free on the next reboot of whitebeast. Both ports are inside the 27015-27020
range the CS LAN browser scans, so discovery still works.

## LAN visibility — REQUIRES a firewall rule (not yet applied)

Verified from a fleet box: with no rule, every port is `NO RESPONSE` from the
LAN even though the servers answer fine on `127.0.0.1`. whitebeast's LAN
adapter is **Wi-Fi, classified Public**, so Windows Firewall drops unsolicited
inbound UDP.

Run this **elevated** on whitebeast (scoped to the local subnet, so it is not a
blanket hole):

```bat
netsh advfirewall firewall add rule name="CS 1.6 LAN servers (UDP)" ^
  dir=in action=allow protocol=UDP localport=27015-27020 ^
  remoteip=LocalSubnet profile=any
```

Then re-verify from a fleet box (see below). Reclassifying Wi-Fi as Private
(`Set-NetConnectionProfile -InterfaceAlias 'Wi-Fi' -NetworkCategory Private`)
also works but is a much broader change — prefer the scoped rule.

## Verifying

From whitebeast (always works, proves the server itself is healthy):

```powershell
# A2S_INFO; expect the hostname, de_dust2, cstrike
.\a2s.ps1 -ip 127.0.0.1 -port 27016
```

From a fleet box — this is the test that actually matters, because it exercises
the firewall and the LAN path. Drive it through the retro agent:

```
EXEC powershell -NoProfile -ExecutionPolicy Bypass -File C:\lantest.ps1
```

From a retro PC's CS 1.6 client, if the LAN tab is empty, try the explicit
connect first — it separates "not discoverable" from "not reachable":

```
connect 192.168.1.82:27016
connect 192.168.1.82:27017
```

## Client compatibility

`sv_lan 1` is load-bearing, not a nicety: it disables Steam authentication,
which is the only reason the fleet's **non-Steam BCS 1.6** clients can connect.
It also keeps these servers off the public master list, which is what we want
for a LAN box.
