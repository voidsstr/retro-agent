# Login-screen fleet dashboard

A full-bleed status wall on this host's **GDM login screen** — everything going
on with the machine and the fleet, visible on the physical monitor whenever
nobody is logged in at it (which, when you work over Chrome Remote Desktop or
RDP, is most of the time).

Press any key or move the mouse and it fades out to the normal password prompt.
It fades back in after 45 seconds idle.

```
╭────────────────────────────────────────────────────────────────────────────╮
│ OMEN · FLEET CONTROL          Ultra 9 285K · 24C · 61 GB · Ubuntu 26.04 LTS│
├────────────────────────────────────────────────────────────────────────────┤
│  13:50           up 3d 0h  CPU 12%  GPU 5% 25°  FLEET 2/7  GAMES 9/9  SVC 7/7
│  Thursday 27 August                                                        │
│                                                                            │
│  ╭─ CPU ─────────────╮  ╭─ GPU ─────────────╮  ╭─ FLEET  2/7 up ─────────╮ │
│  │ Usage ⣿⣿⣿░░░  12% │  │ RTX 5090          │  │ ● WHITEBEAST  Win6.2    │ │
│  │ Freq  ⣿⣿⣿⣿░  4.3G │  │ Util  ⣿░░░░░   5% │  │ ○ 1GHZ        offline   │ │
│  │ use   ⣠⣤⣴⣶⣾⣴⣤⣠   │  │ VRAM  ⣿⣿⣿⣿⣿  58% │  ╰─────────────────────────╯ │
│  │ cores ▃▅▂▇▁▄▂▆▃▁  │  ╰───────────────────╯  ╭─ GAME SERVERS 9/9 up ───╮ │
│  ╰───────────────────╯  ╭─ TEMPERATURES ────╮  │ ● CS 1.6    0/16  11ms  │ │
│  ╭─ MEMORY ──────────╮  │ acpitz   ⣿⣿⣿  48°│  │ ● Quake III 4/16   0ms  │ │
│  │ RAM   ⣿⣿⣿░░░  26% │  ╰───────────────────╯  │ ● UT99      0/10   2ms  │ │
│  ╰───────────────────╯  ╭─ PXE   serving ───╮  │   all up · watchdog on  │ │
│  ╭─ DISK ────────────╮  │ ● retro-pxe  5h   │  ╰─────────────────────────╯ │
│  │ /      ⣿⣿⣿░  41% │  │  ports DHCP✓TFTP✓ │  ╭─ FAVOURITES AGENT ──────╮ │
│  ╰───────────────────╯  │  holds 4 served   │  │ ● server lists   idle   │ │
│  ╭─ REMOTE ──────────╮  ╰───────────────────╯  │  last pass 1m · next 4m │ │
│  │ ● Chrome RD  conn │  ╭─ SERVICES  7/7 up ╮  │  boxes 3 · 6 written    │ │
│  ╰───────────────────╯  │ ● chat brain      │  ╰─────────────────────────╯ │
│                         │ ● favourites      │  ╭─ AGENTS ────────────────╮ │
│                         │ ● gamesrv watch   │  │ ● chat daemon  running  │ │
│                         ╰───────────────────╯  ╰─────────────────────────╯ │
│                                    press any key to log in                 │
╰────────────────────────────────────────────────────────────────────────────╯
```

The visual language is lifted from **omenfan**
([voidsstr/omen-fan-control](https://github.com/voidsstr/omen-fan-control)) —
the same braille `⣿` bars, the same 8-level `⠀⢀⣀⣠⣤⣴⣶⣾⣿` sparkline ramp, and the
same muted teal→sage→gold→coral→rose gradient, so the login screen reads as the
same instrument as `sudo python3 -m omenfan`. The sensor data is not
reimplemented either: the collector imports `omenfan.sensors` directly.

## Install

```bash
sudo bash dashboard/install.sh              # install, enable, restart greeter
sudo bash dashboard/install.sh --uninstall  # back to a stock login screen
```

Requires GNOME Shell 50 on GDM (Ubuntu 26.04). The installer refuses to run if
`metadata.json` does not declare the running shell's major version.

## Layout

| Path | What it is |
|---|---|
| `collector/dashboard_collector.py` | Gathers everything, publishes one JSON file |
| `extension/metadata.json` | Extension manifest — `session-modes: ["gdm"]` is what puts it on the login screen |
| `extension/extension.js` | Builds and updates the wall; dismiss/return logic |
| `extension/render.js` | omenfan's bars, sparklines and palette, as Pango markup |
| `extension/stylesheet.css` | Ground, frame and type |
| `systemd/retro-dashboard-collector.service` | Runs the collector |
| `tests/test_render.js` | Unit tests for the render primitives (`gjs -m`) |
| `tests/test_dashboard_collector.py` | Unit tests for the collector (`pytest`) |
| `tests/test_panels.mjs` | Unit tests for the four service panels (`node`) |
| `tests/preview_panels.mjs` | Renders the panels to your terminal — the only way to *see* the wall |
| `tests/stub-gi.mjs`, `tests/stub-gi-hooks.mjs` | Loader hooks that let node import `extension.js` |
| `install.sh` | Wires all of the above together |

## How it works

```
  dashboard_collector.py  ──writes──>  /run/retro-dashboard/state.json
   (root system service)                        │
                                                │ read every 2s, async
                                                ▼
                        gnome-shell --mode=gdm  +  extension.js
                                  (the login screen)
```

**Two processes, one file.** The extension runs *inside the login screen*. A
stall there is a machine nobody can log into, so it does no I/O beyond an async
read of one small file — no sockets, no subprocesses, no blocking calls. All the
gathering happens in the collector.

**Why `/run` and not `/tmp`.** The greeter runs as a systemd `DynamicUser`
(`gdm-greeter`, `gdm-greeter-2`, … — the name changes on every restart, and its
home is a tmpfs under `/run/gdm3/home/`). `DynamicUser=yes` implies
`PrivateTmp=yes`, so the greeter **cannot see `/tmp`** — which is where the rest
of this project keeps its ephemeral state. `/run/retro-dashboard/state.json`,
mode 0644, is readable by whatever transient user the greeter happens to be.

**Two cadences.** Local sensors are cheap and sampled every 2s; the fleet sweep
opens a TCP connection to every known box and runs every 45s on its own thread.

**Writes are atomic** — write-tmp then `os.replace`, the same convention as
`scripts/ai_status_bus.py`, so the greeter never reads a half-written file.

### What each panel reads

| Panel | Source |
|---|---|
| CPU / MEMORY / DISK / GPU / TEMPERATURES / NETWORK | `omenfan.sensors.SensorCollector` |
| FLEET | `client.retro_protocol.RetroConnection` — AUTH to :9898, greeting only |
| AGENTS | `/tmp/retro-chat/` (daemon.pid, processor.heartbeat, queues, logs) + `scripts/ai_status_bus.py` |
| REMOTE | `loginctl`, the CRD host processes, `gnome-remote-desktop` + `ss` on :3389 |
| GAME SERVERS | `$XDG_RUNTIME_DIR/retro-gameservers/status.json`, published by `scripts/game-servers/gameservers_watch.py` |
| FAVOURITES AGENT | `$XDG_RUNTIME_DIR/retro-gameindex/status.json`, published by `scripts/gameindex/sync.py --daemon` |
| PXE | `systemctl show retro-pxe`, `ss -uln`, `/srv/retro-pxe/pxe_state.json`, `/srv/retro-pxe/pxe_server.log` |
| SERVICES | `systemctl show` in both managers — see below |

### The collector never sends a game query itself

GAME SERVERS and FAVOURITES AGENT are read from status files, not gathered
here. Ten UDP probes with timeouts, plus a fleet-wide favourites pass, belong
on their own services' cadences (20s and 5min); putting them on the 2s loop
whose only job is to keep a login screen painting would be the one stall this
whole design exists to avoid. The collector's contribution is deciding whether
a status file is **absent**, **stale**, or **current** — and those three
render differently, because "the watchdog died an hour ago" and "there is no
watchdog" need different answers from whoever is standing at the monitor.

### Reaching two systemd managers

The services split across both: `retro-pxe` and the collector are **system**
units (privileged ports, and the collector must read `/srv` and every user's
runtime dir), while the chat daemon, chat brain, favourites agent, game-server
watchdog and the game servers themselves are **`--user`** units owned by
`voidsstr`.

The collector runs as root, where a bare `systemctl --user` queries *root's*
manager — which has none of them, so every fleet service would read "not
installed", which on the wall is indistinguishable from every fleet service
having died. `_systemctl_prefix()` therefore drops to the owning uid with its
`XDG_RUNTIME_DIR` set. One batched `systemctl show` covers each manager, so
the whole SERVICES panel costs two forks per refresh, not fourteen.

`LoadState=not-found` is reported as **`absent`**, distinct from `failed`:
never installed and crashed are different calls to action.

The fleet probe deliberately stops at the AUTH greeting: `SYSINFO` and
`PROCLIST` wake real work on boxes that are 25 years old, and this runs
unattended every 45 seconds. The greeting already carries hostname and OS.

### Fleet list

Built-in defaults, overridable by `/etc/retro-dashboard/fleet.json`:

```json
{"nodes": [{"ip": "192.168.1.82", "label": "whitebeast"}]}
```

Either way, the list is **unioned with whatever the chat daemon has claimed**
(parsed from its `claimed N agents: [...]` log line), so a box added to the
fleet without anyone editing a config still appears. whitebeast answers on two
NICs (`.249` Ethernet, `.82` Wi-Fi) and is collapsed to one row by the hostname
the agent reports back, with the second address shown as `also_at`.

**An all-offline fleet is the normal resting state** — the retro boxes are
powered on demand. The panel says "fleet powered down" rather than looking
broken, and a genuine inability to reach them (missing repo, missing client
library) is reported as a distinct error instead of masquerading as "down".

## The three service panels

### GAME SERVERS — and keeping them up

Every fleet game server, with **players, bots, map and query RTT**, from
`scripts/game-servers/gameservers_watch.py`. That watchdog does two jobs on one
20-second loop: publish the status file this panel reads, and **restart what
has actually died**.

Its guardrails matter more than the restarting does:

- a unit systemd calls `failed`/`inactive` is restarted at once;
- a unit that is `active` but has not answered its query for **three
  consecutive cycles** is restarted — one silent cycle is a map change, three
  is a wedge;
- never twice inside **five minutes** for the same unit, and never more than
  **four times an hour**, so a server broken for a reason a restart cannot fix
  (a missing pak, a bad cfg) is left alone with "needs a human" rather than
  flapped forever;
- a unit that was never installed here is never touched, and does not count
  against the up/total.

It is a **`--user`** unit on purpose: the game servers are `--user` units, so
this is the one manager that can restart them without crossing a privilege
boundary.

**Bots are not people.** A Quake III server pinned at `bot_minplayers 4`
reports four players forever. GoldSrc gives a bot count directly; on the Quake
family a player line with **ping 0** is a bot. The panel title and the hero
line both count *humans*, and show bots separately — otherwise the wall would
permanently claim someone was playing.

### FAVOURITES AGENT

`scripts/gameindex/sync.py`, which keeps every box's in-game favourites full of
servers that actually have people on them (see
[`scripts/gameindex/README.md`](../scripts/gameindex/README.md)). The panel
shows the last pass's age and duration, when the next one is due, how many
boxes it reached, how many favourites files it rewrote versus left alone, and
how many live servers it knows about.

**It is now a long-running service rather than a oneshot behind a timer.** It
still runs a pass every five minutes, so the freshness contract is unchanged;
what changes is that "is the favourites agent running?" has an honest answer at
the moment anyone asks. As a timer-driven oneshot the unit read
`inactive (dead)` for 297 of every 300 seconds — fine for cron, useless on a
status wall, and indistinguishable from a service that had stopped.

The other half of that problem is subtler and is why the agent publishes a
report at all: **the retro fleet is powered on demand**, so a perfectly healthy
pass across zero live boxes writes nothing and logs almost nothing. Judged by
its output, a healthy agent looks dead every time the machines are switched
off. So it states outright that a pass completed, when, and with what result.

### PXE

`retro-pxe` is a proxyDHCP + TFTP server for network-installing the fleet.
Beyond the unit state the panel shows the two things that actually tell you it
works:

- **the bound sockets** (67 proxyDHCP, 69 TFTP, 4011 BINL). An `active` unit
  that has lost its sockets serves nothing while looking perfectly healthy, so
  the title says `serving` only when TFTP is really bound.
- **boot holds** — the MACs already served, which is what stops a machine that
  boots from the network first reinstalling itself on every reboot.

Plus the last client seen, the last file served, and how many transfers
completed recently (counted from `DONE` lines, not `GET`s — a slow client
retries a GET several times and would otherwise look like an install storm).

## Verifying it, when you cannot see the screen

The login screen **cannot be screenshotted**: mutter scans out its own buffers
so `/dev/fb0` reads back blank, and the greeter's `org.gnome.Shell.Screenshot`
refuses with *"Saving to disk is disabled"*. So the extension logs a line on
enable and whenever its health or fleet count changes:

```bash
journalctl -b | grep retro-fleet-dashboard
#  retro-fleet-dashboard: enabled
#  retro-fleet-dashboard: rendering (live), fleet 2/7 up, game servers 9/9, services 7/7
```

**Better: render the wall into your terminal.** `preview_panels.mjs` drives
`extension.js`'s own `_render*` methods — not a copy of them — against a real
collector sample, and prints the result with the Pango colours mapped to ANSI.
A panel that would throw on the login screen throws here instead, where you can
see it:

```bash
sudo python3 dashboard/collector/dashboard_collector.py --once --stdout > /tmp/s.json
node --import ./dashboard/tests/stub-gi.mjs dashboard/tests/preview_panels.mjs /tmp/s.json
node --import ./dashboard/tests/stub-gi.mjs dashboard/tests/preview_panels.mjs /tmp/s.json games pxe
```

(The `gi://Clutter` imports are satisfied by inert stubs; the harness swaps
`this._panels` for capture objects and never touches an actor.)

Ask the shell directly whether it loaded (`state: 1` is enabled, and `error`
should be empty):

```bash
GP=$(pgrep -f 'gnome-shell --mode=gdm' | head -1)
GU=$(ps -o uid= -p "$GP" | tr -d ' ')
sudo -u "#$GU" env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$GU/bus \
  gdbus call --session --dest org.gnome.Shell --object-path /org/gnome/Shell \
  --method org.gnome.Shell.Extensions.GetExtensionInfo 'retro-fleet-dashboard@voidsstr'
```

And check the data the greeter is reading:

```bash
systemctl status retro-dashboard-collector
python3 -m json.tool /run/retro-dashboard/state.json | head -40
```

## Safety

The greeter is how you log in at the machine, so:

- **The overlay is `reactive: false`.** It never takes a grab and never consumes
  an event. Input reaches the login dialog underneath even if every other line
  of `extension.js` is broken — the dismiss logic only *observes* events through
  a `captured-event` handler that always returns `EVENT_PROPAGATE`.
- **Nothing blocks.** `Gio.File.load_contents_async` only; no sync I/O.
- **Everything is torn down in `disable()`** — timers, signal handlers, actors.
- **Failures are contained.** A throw during construction is caught and the
  overlay simply isn't added; a throw during render is logged, not propagated.
- **`sshd` is your way back in.** If the login screen ever misbehaves, run
  `sudo bash dashboard/install.sh --uninstall` over SSH.

### Restarting the greeter — do not do it the obvious way

`install.sh` handles this, but if you do it by hand:

- **Never `systemctl restart gdm`.** GDM also owns any Chrome Remote Desktop
  session, so restarting it drops whoever is connected remotely.
- **Killing `gnome-shell --mode=gdm` does not work.** Once a user session is
  registered, `GdmLocalDisplayFactory` reaps the login screen instead of
  respawning it — you get a greeter session with no shell and a black monitor.
  `org.gnome.Shell@gdm.service` and `gnome-session-manager@gnome-login.service`
  both set `RefuseManualStart`, so neither can be restarted by hand either.
- **What works:** `loginctl terminate-session <greeter-session-id>`. GDM notices
  seat0 has no greeter and builds a fresh one.

## The monitor stays awake

`install.sh` also writes these into the greeter's dconf block:

```ini
[org/gnome/desktop/session]
idle-delay=uint32 0

[org/gnome/settings-daemon/plugins/power]
sleep-inactive-ac-timeout=0
sleep-inactive-ac-type='nothing'
```

Without them the greeter inherits GNOME's defaults — **blank after 5 minutes
idle, display to sleep after 20** — and the dashboard renders perfectly into a
screen that has been dark for hours. That is exactly what happened on the first
deploy: the extension logged `rendering (live), fleet 2/7 up` all night to a
monitor that had been asleep for twenty of them. A status wall nobody can see is
not a status wall.

The trade is that the panel is now lit 24/7. On an OLED, or if you would rather
it slept, delete those two stanzas from
`/etc/gdm3/greeter.dconf-defaults` (keep the `[org/gnome/shell]` one) and re-run
`/usr/share/gdm/generate-config`.

Check what is actually in effect, and whether the panel is powered:

```bash
sudo -u gdm env DCONF_PROFILE=gdm dconf read /org/gnome/desktop/session/idle-delay
grep -l connected /sys/class/drm/card*-*/status | xargs -n1 dirname \
  | xargs -I{} sh -c 'echo "$(basename {}) dpms=$(cat {}/dpms)"'
```

## Configuration

Tunables are constants at the top of `extension.js`:

| Constant | Default | Effect |
|---|---|---|
| `REFRESH_MS` | 2000 | State file re-read interval |
| `IDLE_RETURN_MS` | 45000 | Idle before the wall fades back in |
| `FADE_MS` | 400 | Fade duration |
| `STALE_AFTER_SEC` | 15 | Sample age before the header says "stale" |

And collector flags:

```bash
python3 dashboard_collector.py --once --stdout            # one sample to stdout
python3 dashboard_collector.py --once --stdout --no-fleet # skip the sweep
python3 dashboard_collector.py --interval 5 --fleet-interval 120
```

`/etc/retro-dashboard/collector.env` (written by the installer) carries
`RETRO_AGENT_REPO` and `OMENFAN_PATH`. **These are required**, because the
collector is copied to `/usr/local/lib/retro-dashboard/` and cannot find either
checkout by walking up from `__file__` — the first time it ran as a service
without them, every fleet row silently read "down".

## Tests

```bash
gjs -m dashboard/tests/test_render.js                  # 52 assertions
pytest dashboard/tests/test_dashboard_collector.py     # 42 tests
node --import ./dashboard/tests/stub-gi.mjs \
     dashboard/tests/test_panels.mjs                   # 42 assertions
```

The panel tests are mostly about **degenerate input**, because every service
panel reads a file written by something that may not be running: an absent
section, the `{error: ...}` shape, a stale report, a mid-pass report with no
timings, and a server row with every optional field missing. They also pin the
two mistakes that would actively mislead someone at the monitor — bots counted
as people, and `absent` rendered the same as `failed`.

Layout is one column narrower than it looks like it should be: the physical
monitor is **1600×1200**, so width is the scarce dimension and a fourth column
would truncate server names and file paths. The new panels went on the bottom
of the existing three columns, which were using barely half the height.

Both run in under a second on the dev host with no hardware and no GNOME
session. `tests/run_all.sh` includes them.
