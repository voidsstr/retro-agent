# Game servers on whitebeast

`whitebeast` (**192.168.1.82**, Windows 11) has taken over from the old box that
ran the game servers. This directory holds the configs, mods and notes for what
runs there.

> **Servers run natively on Windows, never in WSL.** WSL2 here is in **NAT**
> mode (`172.19.188.220/20`), so anything bound inside WSL is unreachable from
> the 192.168.1.0/24 fleet, and `netsh portproxy` cannot help because it is
> **TCP-only** while GoldSrc is UDP. Windows has the LAN address; the servers
> belong there.

## What runs

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
