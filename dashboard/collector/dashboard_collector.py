#!/usr/bin/env python3
"""Fleet dashboard collector — publishes one JSON state blob for the GDM
login-screen dashboard.

Why a collector at all: the dashboard is drawn by a GNOME Shell extension
running *inside the login screen* (`gnome-shell --mode=gdm`, as the
`gdm-greeter-*` dynamic user).  That process must never block — a stall in
the greeter is a machine you cannot log into — and it has no business
opening TCP sockets to the fleet or shelling out to `nvidia-smi`.  So all
the gathering happens here, in a normal system service, and the extension
only reads one small file and renders it.

Transport is a file, not D-Bus, for the same reason: a file read either
works or fails instantly, and the greeter is heavily sandboxed (systemd
`DynamicUser=yes` implies `PrivateTmp=yes`, so /tmp is NOT visible to it —
which is why the state file lives in /run and not /tmp alongside the rest
of this project's ephemeral state).

Written with write-tmp + os.replace so the greeter never sees a torn file,
matching the convention in scripts/ai_status_bus.py.

Two cadences, because the costs differ by two orders of magnitude:
  * fast loop  (default  2s) — local sensors, /proc, nvidia-smi
  * fleet loop (default 45s) — TCP to every known retro box, in a thread

Usage:
    sudo python3 dashboard_collector.py                # run forever
    python3 dashboard_collector.py --once --stdout     # one sample, to stdout
    python3 dashboard_collector.py --once --stdout --no-fleet   # skip the sweep

See dashboard/README.md for the full schema and the install steps.
"""

import argparse
import asyncio
import collections
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

SCHEMA_VERSION = 3

DEFAULT_STATE_PATH = "/run/retro-dashboard/state.json"
DEFAULT_CONFIG_PATH = "/etc/retro-dashboard/fleet.json"

_HERE = os.path.dirname(os.path.abspath(__file__))


def _find_repo():
    """Locate the retro-agent checkout, for `client.retro_protocol` et al.

    Deriving this from __file__ alone is wrong once installed: install.sh
    copies just this one script to /usr/local/lib/retro-dashboard/, where
    ../.. is /usr/local and the client package is nowhere in sight. That
    silently turned every fleet row to "down" the first time it ran as a
    service, which looks exactly like a powered-down fleet — so the repo path
    is now explicit, and its absence is reported rather than inferred.
    """
    for cand in (
        os.environ.get("RETRO_AGENT_REPO"),
        os.path.abspath(os.path.join(_HERE, "..", "..")),
        os.path.expanduser("~/development/retro-agent"),
        "/home/voidsstr/development/retro-agent",
        "/opt/retro-agent",
    ):
        if cand and os.path.isdir(os.path.join(cand, "client")):
            return cand
    return None


_REPO = _find_repo()

# How many samples of history to keep for the sparklines. omenfan's TUI keeps
# roughly a screen's worth; 120 samples at 2s is four minutes of trail, which
# is enough to see a fan ramp or a build kick off from across the room.
HISTORY_LEN = 120

# Fleet boxes we know about. Overridden wholesale by the config file if one
# exists. Sourced from docs/machines/ and CLAUDE.md; whitebeast answers on two
# NICs (.249 Ethernet, .82 Wi-Fi) so both are listed and de-duplicated by the
# hostname the agent reports back.
DEFAULT_FLEET = [
    {"ip": "192.168.1.82", "label": "whitebeast"},
    {"ip": "192.168.1.249", "label": "whitebeast"},
    {"ip": "192.168.1.122", "label": "P2-400"},
    {"ip": "192.168.1.124", "label": "VOODOO3"},
    {"ip": "192.168.1.133", "label": "P3-DUAL"},
    {"ip": "192.168.1.143", "label": "1GHZ"},
    {"ip": "192.168.1.176", "label": "box-176"},
    {"ip": "192.168.1.243", "label": "N5R5L9"},
]

# The chat daemon logs the set of agents it has actually claimed, which is the
# only live record of a box that was added without anyone updating a config.
# We union that into the poll list so a new machine appears on the dashboard
# by itself. Most of the fleet is powered on demand, so "down" is the normal
# resting state for most rows and is not an error.
_CLAIMED_RE = "claimed "

CHAT_ROOT = os.environ.get("RETRO_CHAT_ROOT", "/tmp/retro-chat")
AI_STATUS_DIR = os.environ.get("RETRO_AI_STATUS_DIR", "/tmp/retro-ai/status")

# The user whose `systemd --user` manager owns the game servers, the chat
# services and the favourites agent. This collector runs as root, where
# `systemctl --user` means *root's* manager -- which has none of those units and
# would report every one of them missing.
FLEET_USER = os.environ.get("RETRO_FLEET_USER", "voidsstr")

# Published by scripts/game-servers/gameservers_watch.py and
# scripts/gameindex/sync.py --daemon respectively. Both live in the owning
# user's $XDG_RUNTIME_DIR: root can read them, and they are not in /tmp, which
# the greeter cannot see at all.
GAMESERVERS_STATUS = os.environ.get("RETRO_GAMESERVERS_STATUS")
GAMEINDEX_STATUS = os.environ.get("RETRO_GAMEINDEX_STATUS")

PXE_ROOT = os.environ.get("RETRO_PXE_ROOT", "/srv/retro-pxe")
PXE_PORTS = {67: "proxyDHCP", 69: "TFTP", 4011: "BINL"}

# Every host-side service the wall reports on: (label, unit, scope). "user"
# units are reached through FLEET_USER's manager, "system" through our own.
HOST_SERVICES = [
    ("chat daemon",  "retro-chat-daemon",        "user"),
    ("chat brain",   "retro-chat-brain",         "user"),
    ("favourites",   "retro-gameindex",          "user"),
    ("gamesrv watch", "retro-gameservers-watch", "user"),
    ("dos games",    "retro-dosgames-http",      "user"),
    ("pxe server",   "retro-pxe",                "system"),
    ("dashboard",    "retro-dashboard-collector", "system"),
]

# Anything that shells out is cached on a slower cadence than the 2s sensor
# loop -- a `systemctl show` per panel per sample is ~1 fork/second forever for
# data that changes on the scale of minutes.
SLOW_TTL_SEC = 10.0
# Site panel: network + database, and nothing on it moves minute to minute.
SITES_TTL_SEC = 120.0


# --------------------------------------------------------------------------
# local vitals — omenfan does this well already, so reuse it wholesale
# --------------------------------------------------------------------------

def _load_omenfan():
    """Import omenfan's sensor stack, or return None if it isn't installed.

    omenfan lives in its own repo (voidsstr/omen-fan-control). We deliberately
    import rather than vendor: its sensor code is the thing being reused, and a
    copy here would rot the first time that repo gains a chip family.
    """
    for cand in (
        os.environ.get("OMENFAN_PATH"),
        os.path.expanduser("~/development/omen-fan-control"),
        "/home/voidsstr/development/omen-fan-control",
        "/opt/omen-fan-control",
    ):
        if cand and os.path.isdir(os.path.join(cand, "omenfan")):
            if cand not in sys.path:
                sys.path.insert(0, cand)
            try:
                from omenfan.detect import detect
                from omenfan.ec import ECDummy, open_ec
                from omenfan.sensors import SensorCollector
                return detect, open_ec, ECDummy, SensorCollector
            except Exception:
                return None
    return None


class Vitals:
    """Local machine sensors, sampled on the fast loop."""

    def __init__(self):
        self.ok = False
        self._sc = None
        self._ec = None
        self.hist = {
            k: collections.deque(maxlen=HISTORY_LEN)
            for k in ("cpu", "cpu_temp", "gpu", "gpu_temp", "mem", "net_rx", "net_tx")
        }
        loaded = _load_omenfan()
        if not loaded:
            return
        detect, open_ec, ECDummy, SensorCollector = loaded
        try:
            caps = detect()
            # Read-only EC: this service must never touch fan control. When the
            # EC is unavailable (ec_method "none", the usual case under kernel
            # lockdown) omenfan hands back a dummy and every EC field is simply
            # absent — the same graceful degradation its TUI relies on.
            if caps.ec_method == "none":
                self._ec = ECDummy()
            else:
                self._ec = open_ec(caps.ec_method, caps.ec_base, writable=False)
            self._ec.open()
            self._caps = caps
            self._sc = SensorCollector(caps)
            self.ok = True
        except Exception:
            self.ok = False

    def sample(self):
        if not self.ok:
            return {}
        try:
            self._sc.refresh(self._ec)
        except Exception:
            return {}
        sc = self._sc
        out = {}

        si = getattr(sc, "sysinfo", None)
        if si:
            model = (si.cpu_model or "").replace("Intel(R) Core(TM) ", "")
            for junk in ("(R)", "(TM)"):
                model = model.replace(junk, "")
            out["host"] = {
                "hostname": si.hostname,
                "distro": si.distro,
                "kernel": si.kernel,
                "cpu_model": model.strip(),
                "cpu_cores": si.cpu_cores,
                "ram_gb": round(si.ram_gb, 1),
                "uptime_sec": int(si.uptime_sec),
            }

        cpu = getattr(sc, "cpu", None)
        if cpu:
            out["cpu"] = {
                "usage_pct": round(cpu.usage_pct, 1),
                "per_core_pct": [round(c, 1) for c in cpu.per_core_pct],
                "freq_mhz": round(cpu.freq_mhz, 0),
                "freq_max_mhz": round(cpu.freq_max_mhz, 0),
                "load": [cpu.load_1, cpu.load_5, cpu.load_15],
                "proc_running": cpu.proc_running,
                "proc_total": cpu.proc_total,
                "governor": cpu.governor,
            }
            self.hist["cpu"].append(round(cpu.usage_pct, 1))

        mem = getattr(sc, "memory", None)
        if mem:
            out["memory"] = {
                "total_gb": round(mem.total_kb / 1048576, 1),
                "used_gb": round(mem.used_kb / 1048576, 1),
                "used_pct": round(mem.used_pct, 1),
                "cached_gb": round(mem.cached_kb / 1048576, 1),
                "swap_total_gb": round(mem.swap_total_kb / 1048576, 1),
                "swap_used_pct": round(mem.swap_used_pct, 1),
            }
            self.hist["mem"].append(round(mem.used_pct, 1))

        gpu = getattr(sc, "gpu", None)
        if gpu:
            out["gpu"] = {
                "name": gpu.name,
                "temp_c": gpu.temp_c,
                "fan_pct": gpu.fan_pct,
                "util_pct": gpu.util_pct,
                "power_w": round(gpu.power_w, 1) if gpu.power_w else 0,
                "vram_used_mb": gpu.vram_used_mb,
                "vram_total_mb": gpu.vram_total_mb,
                "vram_used_pct": round(gpu.vram_used_pct, 1),
                "clock_core_mhz": gpu.clock_core_mhz,
                "pstate": gpu.pstate,
                "driver_version": gpu.driver_version,
            }
            self.hist["gpu"].append(gpu.util_pct or 0)
            if gpu.temp_c:
                self.hist["gpu_temp"].append(gpu.temp_c)

        thermals = getattr(sc, "thermals", None) or []
        out["thermals"] = [
            {"name": t.name.split("/")[0], "temp_c": round(t.temp_c, 1)}
            for t in thermals[:8]
        ]
        hottest = max((t.temp_c for t in thermals), default=0)
        if hottest:
            self.hist["cpu_temp"].append(round(hottest, 1))

        fans = getattr(sc, "hwmon_fans", None) or []
        out["fans"] = [
            {"name": f.name, "rpm": f.rpm, "pct": getattr(f, "pct", None)}
            for f in fans
        ]
        # The discrete GPU reports its own fan even when no Super-I/O fan
        # driver is bound, which on this box is the only fan we can see.
        if not out["fans"] and gpu and gpu.fan_pct:
            out["fans"] = [{"name": "GPU", "rpm": None, "pct": gpu.fan_pct}]

        disks = getattr(sc, "disks", None) or []
        out["disks"] = [
            {
                "mount": d.mount,
                "model": d.model,
                "total_gb": round(d.total_gb, 1),
                "used_gb": round(d.used_gb, 1),
                "used_pct": round(d.used_pct, 1),
                "temp_c": d.temp_c or None,
            }
            for d in disks
            if d.total_gb >= 1.0  # skip the EFI stub and other slivers
        ]

        dio = getattr(sc, "disk_io", None)
        if dio:
            out["disk_io"] = {
                "read_bps": round(dio.read_bytes_sec),
                "write_bps": round(dio.write_bytes_sec),
            }

        # Busiest interfaces first, and drop loopback/virtual bridges — this
        # box runs Docker, so there are a dozen veth pairs that carry nothing
        # and would otherwise crowd out the real NIC.
        nets = getattr(sc, "net_interfaces", None) or []
        real = [
            n for n in nets
            if not n.name.startswith(("lo", "veth", "docker", "br-", "virbr"))
        ]
        real.sort(key=lambda n: n.rx_bytes_sec + n.tx_bytes_sec, reverse=True)
        out["net"] = [
            {
                "name": n.name,
                "rx_bps": round(n.rx_bytes_sec),
                "tx_bps": round(n.tx_bytes_sec),
            }
            for n in real[:4]
        ]
        if real:
            self.hist["net_rx"].append(round(real[0].rx_bytes_sec))
            self.hist["net_tx"].append(round(real[0].tx_bytes_sec))

        out["history"] = {k: list(v) for k, v in self.hist.items()}
        return out


# --------------------------------------------------------------------------
# fleet — who is answering on 9898
# --------------------------------------------------------------------------

def _claimed_from_daemon_log(limit=200000):
    """IPs the chat daemon says it has claimed, newest line wins.

    Keeps the dashboard honest when a box is added to the fleet but nobody
    updates fleet.json — the daemon already had to discover it to work.
    """
    path = os.path.join(CHAT_ROOT, "daemon.log")
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            fh.seek(max(0, size - limit))
            chunk = fh.read().decode("utf-8", errors="replace")
    except Exception:
        return []
    for line in reversed(chunk.splitlines()):
        if _CLAIMED_RE not in line or "agents:" not in line:
            continue
        try:
            listing = line.split("agents:", 1)[1].strip()
            ips = json.loads(listing.replace("'", '"'))
            return [ip for ip in ips if isinstance(ip, str)]
        except Exception:
            continue
    return []


def _load_fleet_config(path):
    """Configured nodes (or the built-in list), unioned with claimed IPs."""
    nodes = None
    try:
        with open(path) as fh:
            data = json.load(fh)
        candidate = data.get("nodes") if isinstance(data, dict) else data
        if isinstance(candidate, list) and candidate:
            nodes = [n for n in candidate if n.get("ip")]
    except Exception:
        nodes = None
    if nodes is None:
        nodes = list(DEFAULT_FLEET)

    known = {n["ip"] for n in nodes}
    for ip in _claimed_from_daemon_log():
        if ip not in known:
            nodes.append({"ip": ip, "label": ip})
            known.add(ip)
    return nodes


async def _probe_one(node, secret, timeout):
    """AUTH to one agent and return its greeting. Nothing heavier.

    Deliberately stops at the greeting: SYSINFO and PROCLIST wake real work on
    boxes that are 25 years old, and this runs unattended every 45 seconds.
    The greeting alone carries hostname and OS, which is all the panel shows.
    """
    ip = node["ip"]
    started = time.monotonic()
    rec = {
        "ip": ip,
        "label": node.get("label") or ip,
        "up": False,
        "name": None,
        "os": None,
        "rtt_ms": None,
        "error": None,
    }
    if not _REPO:
        rec["error"] = "retro-agent repo not found"
        return rec
    try:
        if _REPO not in sys.path:
            sys.path.insert(0, _REPO)
        from client.retro_protocol import RetroConnection

        conn = RetroConnection(ip, node.get("port", 9898))
        await conn.connect(secret, timeout=timeout)
        rec["up"] = True
        rec["name"] = conn.hostname or None
        rec["os"] = conn.os_version or None
        rec["family"] = conn.os_family or None
        rec["rtt_ms"] = round((time.monotonic() - started) * 1000)
        # Graceful close — Win9x agents crash on an abrupt RST (see
        # RetroConnection.close), and this fleet still has Win9x boxes.
        await conn.close(graceful=True)
    except asyncio.TimeoutError:
        rec["error"] = "timeout"
    except OSError as exc:
        rec["error"] = exc.strerror or type(exc).__name__
    except Exception as exc:
        rec["error"] = type(exc).__name__
    return rec


async def _probe_fleet(nodes, secret, timeout):
    results = await asyncio.gather(
        *[_probe_one(n, secret, timeout) for n in nodes],
        return_exceptions=True,
    )
    out = []
    for r in results:
        if isinstance(r, dict):
            out.append(r)
    # whitebeast answers on two NICs; collapse to the box, keeping the live one.
    by_name = {}
    for r in out:
        key = (r.get("name") or r["label"] or r["ip"]).upper()
        prev = by_name.get(key)
        if prev is None or (r["up"] and not prev["up"]):
            if prev is not None and prev["up"] and r["up"]:
                r["also_at"] = prev["ip"]
            by_name[key] = r
        elif prev["up"] and r["up"]:
            prev.setdefault("also_at", r["ip"])
    merged = sorted(by_name.values(), key=lambda r: (not r["up"], r["label"]))
    return merged


class FleetPoller(threading.Thread):
    """Polls the fleet on its own slow cadence, off the fast loop's back."""

    daemon = True

    def __init__(self, nodes, secret, interval, timeout):
        super().__init__(name="fleet-poller")
        self.nodes = nodes
        self.secret = secret
        self.interval = interval
        self.timeout = timeout
        self._lock = threading.Lock()
        self._state = {"polled_at": 0, "nodes": [], "up": 0, "total": len(nodes)}
        self._stop = threading.Event()

    def snapshot(self):
        with self._lock:
            return dict(self._state)

    def poll_once(self):
        try:
            nodes = asyncio.run(_probe_fleet(self.nodes, self.secret, self.timeout))
        except Exception:
            nodes = []
        state = {
            "polled_at": time.time(),
            "nodes": nodes,
            "up": sum(1 for n in nodes if n["up"]),
            "total": len(nodes),
        }
        # An all-down fleet is normal (the retro boxes are powered on demand),
        # so it must be distinguishable from a fleet we simply cannot reach.
        if not _REPO:
            state["error"] = "retro-agent repo not found; set RETRO_AGENT_REPO"
        with self._lock:
            self._state = state
        return state

    def run(self):
        while not self._stop.is_set():
            self.poll_once()
            self._stop.wait(self.interval)

    def stop(self):
        self._stop.set()


# --------------------------------------------------------------------------
# agents — the chat daemon, the brain, and whatever they are working on
# --------------------------------------------------------------------------

def _pid_alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except Exception:
        return False


def _tail_reason(log_path, limit=4000):
    """Last meaningful line of the daemon log, for the 'current work' line."""
    try:
        size = os.path.getsize(log_path)
        with open(log_path, "rb") as fh:
            fh.seek(max(0, size - limit))
            chunk = fh.read().decode("utf-8", errors="replace")
        for line in reversed(chunk.splitlines()):
            line = line.strip()
            if line and not line.startswith("="):
                return line[:200]
    except Exception:
        pass
    return None


def collect_agents():
    """Chat daemon + brain health, and any live AI runs."""
    out = {"brain": {}, "daemon": {}, "runs": [], "queue": {}}
    now = time.time()

    # --- chat daemon (pid file) ---
    pid_file = os.path.join(CHAT_ROOT, "daemon.pid")
    daemon_log = os.path.join(CHAT_ROOT, "daemon.log")
    d = {"state": "absent"}
    try:
        with open(pid_file) as fh:
            pid = fh.read().strip()
        if _pid_alive(pid):
            d = {"state": "running", "pid": int(pid)}
            try:
                d["age_sec"] = int(now - os.path.getmtime(pid_file))
            except OSError:
                pass
        else:
            d = {"state": "stale", "pid": int(pid)}
    except FileNotFoundError:
        d = {"state": "absent"}
    except Exception:
        d = {"state": "unknown"}
    if os.path.exists(daemon_log):
        try:
            d["log_age_sec"] = int(now - os.path.getmtime(daemon_log))
        except OSError:
            pass
        d["last"] = _tail_reason(daemon_log)
    out["daemon"] = d

    # --- brain (heartbeat file) ---
    hb = os.path.join(CHAT_ROOT, "processor.heartbeat")
    b = {"state": "absent"}
    try:
        age = now - os.path.getmtime(hb)
        # chat_status.sh calls 120s the liveness cut-off; match it exactly so
        # the dashboard and the CLI never disagree about whether it is alive.
        b = {
            "state": "alive" if age < 120 else "stale",
            "age_sec": int(age),
        }
    except FileNotFoundError:
        b = {"state": "absent"}
    except Exception:
        b = {"state": "unknown"}
    brain_log = os.path.join(CHAT_ROOT, "brain.log")
    if os.path.exists(brain_log):
        b["last"] = _tail_reason(brain_log)
    out["brain"] = b

    # --- queues ---
    for name in ("inbox", "outbox", "failed"):
        try:
            entries = os.listdir(os.path.join(CHAT_ROOT, name))
            out["queue"][name] = sum(1 for e in entries if e.endswith(".json"))
        except Exception:
            out["queue"][name] = None

    # --- history volume ---
    try:
        hist_root = os.path.join(CHAT_ROOT, "history")
        hosts = [d for d in os.listdir(hist_root)
                 if os.path.isdir(os.path.join(hist_root, d))]
        prompts = 0
        for h in hosts:
            try:
                prompts += sum(
                    1 for f in os.listdir(os.path.join(hist_root, h))
                    if f.startswith("prompt-")
                )
            except OSError:
                continue
        out["history"] = {"hosts": len(hosts), "prompts": prompts}
    except Exception:
        out["history"] = {}

    # --- live AI runs, via the existing status bus ---
    try:
        if not _REPO:
            raise RuntimeError("repo not found")
        scripts_dir = os.path.join(_REPO, "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        import ai_status_bus as bus

        for run in bus.list_active():
            out["runs"].append({
                "run_id": run.get("run_id"),
                "kind": run.get("kind"),
                "model": run.get("model"),
                "phase": run.get("phase"),
                "liveness": run.get("liveness"),
                "status": run.get("status"),
                "progress": run.get("progress") or {},
                "metrics": run.get("metrics") or {},
            })
    except Exception:
        # The bus is only present when an orchestration script is running;
        # its absence is the normal case, not an error worth surfacing.
        pass

    return out


# --------------------------------------------------------------------------
# remote — is anybody driving this box right now
# --------------------------------------------------------------------------

def _run(cmd, timeout=4):
    try:
        return subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        ).stdout
    except Exception:
        return ""


def collect_remote():
    """Chrome Remote Desktop / RDP / local seat occupancy.

    This is the panel that answers 'is the person who owns this machine
    actually looking at it right now, and how'.
    """
    out = {"crd": {"state": "off"}, "rdp": {"state": "off"}, "sessions": []}

    # --- logind sessions ---
    listing = _run(["loginctl", "list-sessions", "--no-legend"])
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        sid = parts[0]
        detail = _run(["loginctl", "show-session", sid])
        props = {}
        for kv in detail.splitlines():
            if "=" in kv:
                k, v = kv.split("=", 1)
                props[k] = v
        if props.get("Class") != "user":
            continue
        out["sessions"].append({
            "id": sid,
            "user": props.get("Name"),
            "seat": props.get("Seat") or None,
            "remote": props.get("Remote") == "yes",
            "type": props.get("Type"),
            "active": props.get("Active") == "yes",
        })

    # A CRD session is a logind session with no seat but Remote=yes; the host
    # also runs a --type=desktop child owned by whoever is logged in.
    crd_desktop = _run(["pgrep", "-af", "chrome-remote-desktop-host --type=desktop"])
    if crd_desktop.strip():
        remote_sessions = [s for s in out["sessions"] if s["remote"]]
        out["crd"] = {
            "state": "connected" if remote_sessions else "hosting",
            "user": remote_sessions[0]["user"] if remote_sessions else None,
        }
    elif _run(["pgrep", "-f", "chrome-remote-desktop-host --type=daemon"]).strip():
        out["crd"] = {"state": "idle"}

    # --- RDP: gnome-remote-desktop, plus any ESTABLISHED peer on 3389 ---
    grd = _run(["pgrep", "-x", "gnome-remote-de"]).strip()
    if grd:
        peers = []
        for line in _run(["ss", "-tnH", "state", "established", "sport", "= :3389"]).splitlines():
            cols = line.split()
            if len(cols) >= 4:
                peers.append(cols[3].rsplit(":", 1)[0])
        out["rdp"] = {
            "state": "connected" if peers else "listening",
            "peers": peers,
        }

    # --- who is on the physical seat ---
    seated = [s for s in out["sessions"] if s["seat"]]
    out["console"] = {
        "occupied": bool(seated),
        "user": seated[0]["user"] if seated else None,
    }
    return out


# --------------------------------------------------------------------------
# systemd — one place that knows how to ask about a unit in either manager
# --------------------------------------------------------------------------

_UNIT_PROPS = ("ActiveState", "SubState", "LoadState", "UnitFileState",
               "Result", "NRestarts", "ExecMainStartTimestampMonotonic")


def _uid_of(user):
    try:
        import pwd
        return pwd.getpwnam(user).pw_uid
    except Exception:
        return None


def _systemctl_prefix(scope):
    """Argv prefix reaching the right manager.

    A system unit is our own. A `--user` unit belongs to FLEET_USER, and we are
    root: `systemctl --user` here would query root's own (empty) manager and
    report every fleet service as not-found, which on the wall is
    indistinguishable from every fleet service having died.
    """
    if scope == "system":
        return ["systemctl"]
    uid = _uid_of(FLEET_USER)
    if uid is None or uid == os.geteuid():
        return ["systemctl", "--user"]
    # No --clear-groups: setgroups() does not survive this unit's sandbox and
    # setpriv then exits with "setgroups failed: Operation not permitted",
    # producing empty output rather than an error anyone would see. This path
    # is only a fallback now (see user_unit_states_from_watchdog), but a
    # fallback that cannot work is not one.
    return [
        "setpriv", "--reuid", str(uid), "--regid", str(uid),
        "env", f"XDG_RUNTIME_DIR=/run/user/{uid}",
        "systemctl", "--user",
    ]


def _boot_monotonic_usec():
    try:
        with open("/proc/uptime") as fh:
            return int(float(fh.read().split()[0]) * 1_000_000)
    except Exception:
        return 0


def unit_states(units, scope):
    """`systemctl show` for a batch of units — one fork for the whole panel."""
    out = {u: {"state": "unknown"} for u in units}
    if not units:
        return out
    cmd = _systemctl_prefix(scope) + ["show", "--no-pager",
                                      "-p", ",".join(_UNIT_PROPS)]
    cmd += [f"{u}.service" for u in units]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=8)
    except Exception:
        return out
    blocks = [b for b in res.stdout.split("\n\n") if b.strip()]
    now_mono = _boot_monotonic_usec()
    for unit, blk in zip(units, blocks):
        props = {}
        for line in blk.splitlines():
            if "=" in line:
                key, val = line.split("=", 1)
                props[key] = val
        if props.get("LoadState") == "not-found":
            out[unit] = {"state": "absent"}
            continue
        rec = {
            "state": props.get("ActiveState") or "unknown",
            "sub": props.get("SubState") or None,
            "enabled": props.get("UnitFileState") or None,
            "result": props.get("Result") or None,
        }
        try:
            rec["restarts"] = int(props.get("NRestarts") or 0)
        except ValueError:
            pass
        try:
            started = int(props.get("ExecMainStartTimestampMonotonic") or 0)
            if started and now_mono:
                rec["uptime_sec"] = max(0, (now_mono - started) // 1_000_000)
        except ValueError:
            pass
        out[unit] = rec
    return out


def user_unit_states_from_watchdog(units):
    """User-unit states as reported by the game-server watchdog, or None.

    Preferred over asking systemd ourselves. We are root inside a hardened
    unit, so `systemctl --user` means *root's* manager and reaching the fleet
    user's needs a privilege hop -- which does not survive this sandbox:
    `setpriv --clear-groups` calls setgroups(), that fails, stdout comes back
    empty, and every fleet service reads "unknown". On a wall whose whole job
    is service health, that is indistinguishable from all of them having died,
    and it is what shipped on the first deploy.

    The watchdog already IS the fleet user and already runs `systemctl --user`
    for the game servers, so it answers this directly and puts the result in
    the status file we are reading anyway. No hop, no sandbox interaction.
    """
    path = _runtime_status_path(GAMESERVERS_STATUS, "retro-gameservers")
    data, err = _read_status_file(path)
    if err or not data:
        return None
    reported = data.get("host_services")
    if not isinstance(reported, dict) or not reported:
        return None
    # A stale file means the watchdog stopped; its snapshot of everything else
    # stopped with it, so do not present old states as current.
    if data.get("stale_sec"):
        return None
    return {u: reported[u] for u in units if u in reported}


def collect_services():
    """State of every host-side service the fleet depends on."""
    rows = []
    for scope in ("user", "system"):
        units = [u for _, u, sc in HOST_SERVICES if sc == scope]
        states = None
        if scope == "user":
            states = user_unit_states_from_watchdog(units)
        if states is None:
            states = unit_states(units, scope)
        for label, unit, sc in HOST_SERVICES:
            if sc != scope:
                continue
            rec = dict(states.get(unit, {"state": "unknown"}))
            rec.update({"label": label, "unit": unit, "scope": sc})
            rows.append(rec)
    order = [u for _, u, _ in HOST_SERVICES]
    rows.sort(key=lambda r: order.index(r["unit"]))
    healthy = sum(1 for r in rows if r["state"] == "active")
    return {
        "services": rows,
        "up": healthy,
        "total": len(rows),
        "degraded": [r["unit"] for r in rows if r["state"] != "active"],
    }


# --------------------------------------------------------------------------
# game servers — read the watchdog's blob, never probe UDP from here
# --------------------------------------------------------------------------

def _runtime_status_path(explicit, subdir):
    """Find a service's status file in FLEET_USER's runtime dir."""
    if explicit:
        return explicit
    uid = _uid_of(FLEET_USER)
    candidates = []
    if uid is not None:
        candidates.append(f"/run/user/{uid}/{subdir}/status.json")
    candidates.append(f"/run/user/{os.geteuid()}/{subdir}/status.json")
    for cand in candidates:
        if os.path.exists(cand):
            return cand
    return candidates[0]


def _read_status_file(path, max_age=120):
    """(payload, error). A stale file is reported as stale, not as absent —
    'the watchdog died an hour ago' and 'there is no watchdog' need different
    answers from whoever is standing at the monitor."""
    try:
        with open(path) as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return None, "not running"
    except Exception as exc:
        return None, f"unreadable: {type(exc).__name__}"
    age = time.time() - (data.get("ts") or 0)
    if age > max_age:
        data["stale_sec"] = int(age)
    return data, None


def collect_gameservers():
    """Whatever gameservers_watch.py last published.

    This collector deliberately does NOT send the query packets itself. Ten
    UDP probes with timeouts belong on the watchdog's 20s loop, not on a 2s
    loop whose only job is to keep a login screen painting.
    """
    path = _runtime_status_path(GAMESERVERS_STATUS, "retro-gameservers")
    data, err = _read_status_file(path)
    if err:
        return {"error": err, "path": path, "servers": [],
                "up": 0, "total": 0, "players": 0}
    return {
        "servers": data.get("servers", []),
        "proxies": data.get("proxies", []),
        "up": data.get("up", 0),
        "total": data.get("total", 0),
        "players": data.get("players", 0),
        "humans": data.get("humans", 0),
        "bots": data.get("bots", 0),
        "down": data.get("down", []),
        "watchdog": data.get("watchdog", {}),
        "polled_at": data.get("ts"),
        "stale_sec": data.get("stale_sec"),
    }


def collect_gameindex():
    """The favourites agent's own report of its last pass."""
    path = _runtime_status_path(GAMEINDEX_STATUS, "retro-gameindex")
    # Its cadence is five minutes, so a two-minute-old report is perfectly
    # healthy; only a missed pass and a half counts as stale.
    data, err = _read_status_file(path, max_age=480)
    if err:
        return {"error": err, "path": path}
    servers = data.get("servers") or {}
    return {
        "ok": data.get("ok"),
        "phase": data.get("phase"),
        "ts": data.get("ts"),
        "duration_sec": data.get("duration_sec"),
        "next_pass_at": data.get("next_pass_at"),
        "agents": data.get("agents") or [],
        "machines": data.get("machines") or [],
        "writes": data.get("writes") or {},
        "favorites": data.get("favorites") or {},
        "engines": data.get("engines") or [],
        "servers_known": sum(v.get("servers", 0) for v in servers.values()),
        "servers_by_engine": servers,
        "errors": (data.get("errors") or [])[:3],
        "stale_sec": data.get("stale_sec"),
    }


# --------------------------------------------------------------------------
# PXE — is the install server armed, and is anything booting from it
# --------------------------------------------------------------------------

def _udp_listening():
    # `ss -ulnH` columns are: State Recv-Q Send-Q Local Peer. The LOCAL address
    # is column 3; column 4 is the peer, which for a listening UDP socket is
    # `0.0.0.0:*` — parsing that instead yields no ports at all and reports a
    # healthy PXE server as serving nothing.
    ports = set()
    for line in _run(["ss", "-ulnH"]).splitlines():
        cols = line.split()
        if len(cols) < 4:
            continue
        try:
            ports.add(int(cols[3].rsplit(":", 1)[1]))
        except (ValueError, IndexError):
            continue
    return ports


_PXE_LOG_RE = re.compile(r"^\[(\d\d:\d\d:\d\d)\]\s+(\S+)\s+(\S+)(.*)$")


def _pxe_recent(log_path, limit=60000):
    """Last activity from the PXE log: who, when, and what file.

    The log's timestamps are wall-clock times with no date, so the file's mtime
    is what actually dates the activity — parsing "19:25:38" back into a moment
    would silently be a day out every morning.
    """
    out = {}
    try:
        size = os.path.getsize(log_path)
        out["last_activity_sec"] = int(time.time() - os.path.getmtime(log_path))
        with open(log_path, "rb") as fh:
            fh.seek(max(0, size - limit))
            chunk = fh.read().decode("utf-8", errors="replace")
    except Exception:
        return out
    lines = [ln.strip() for ln in chunk.splitlines() if ln.strip()]
    if lines:
        out["last"] = lines[-1][:160]
    clients = []
    files = 0
    for line in reversed(lines):
        match = _PXE_LOG_RE.match(line)
        if not match:
            continue
        _stamp, kind, who, rest = match.groups()
        if kind == "tftp":
            if who not in clients:
                clients.append(who)
            words = rest.split()
            # `tftp <ip> DONE <path>` is one completed transfer; GET lines are
            # the requests, and counting those double-counts every retry.
            if words and words[0] == "DONE":
                files += 1
                if "last_file" not in out and len(words) > 1:
                    out["last_file"] = words[1][-42:]
        elif kind == "proxyDHCP" and "last_offer" not in out:
            out["last_offer"] = rest.strip()[:80]
    out["recent_clients"] = clients[:4]
    out["files_served_recent"] = files
    return out


def collect_pxe():
    state = unit_states(["retro-pxe"], "system")["retro-pxe"]
    out = dict(state)
    out["unit"] = "retro-pxe"

    listening = _udp_listening()
    out["ports"] = {
        name: (port in listening) for port, name in PXE_PORTS.items()
    }
    # Bound ports are the real liveness test. A `retro-pxe` that is `active`
    # but has lost its sockets serves nothing while looking perfectly healthy.
    out["serving"] = state.get("state") == "active" and out["ports"].get("TFTP", False)

    # Boot holds: MACs already served, which is what stops a machine that boots
    # from the network first reinstalling itself in a loop on every reboot.
    holds = []
    try:
        with open(os.path.join(PXE_ROOT, "pxe_state.json")) as fh:
            data = json.load(fh)
        now = time.time()
        for mac, served in sorted(data.items(), key=lambda kv: -kv[1]):
            holds.append({"mac": mac, "age_sec": int(now - float(served))})
    except Exception:
        pass
    out["holds"] = holds[:6]
    out["hold_count"] = len(holds)

    out.update(_pxe_recent(os.path.join(PXE_ROOT, "pxe_server.log")))
    return out


# --------------------------------------------------------------------------
# the two web properties (specpicks.com, aisleprompt.com)
# --------------------------------------------------------------------------
#
# Their agents run under the `reusable-agents` framework, which exposes a local
# FastAPI on 127.0.0.1:8090 and keeps all durable state in AZURE BLOB STORAGE.
#
# That last fact governs the whole design of this section. The framework's blob
# client is built with the SDK's default retry policy -- 20s connect, 60s read,
# three exponential retries -- and the API sets no request deadline of its own,
# so a single call CAN block for over three minutes when Azure is unreachable.
# A wall that freezes for three minutes because a cloud storage account is
# having a bad day is worse than a wall that says "unreachable" in 8 seconds.
# So every call here carries its own hard client-side timeout and every failure
# has a rendering. Never inherit the API's patience.
#
# The API is also NOT a systemd unit -- it is a bare uvicorn process that
# nothing restarts. Connection-refused is an expected state, not an anomaly.

SITES = ("specpicks", "aisleprompt")
SITE_API = "http://127.0.0.1:8090"
SITE_SECRETS = os.path.expanduser("~voidsstr/.reusable-agents/secrets.env")
SITE_API_TIMEOUT = 8.0
SITE_DB_TIMEOUT = 5


def _site_secret(name):
    """One value out of secrets.env, or None. Never raises, never logs it."""
    try:
        with open(SITE_SECRETS, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                if k.strip() == name:
                    return v.strip().strip("'").strip('"')
    except OSError:
        return None
    return None


def _site_api(path):
    """GET one API path. Returns (payload, error). Hard-capped at 8s."""
    import urllib.error
    import urllib.request

    token = _site_secret("FRAMEWORK_API_TOKEN")
    if not token:
        return None, "no FRAMEWORK_API_TOKEN in secrets.env"
    req = urllib.request.Request(
        f"{SITE_API}{path}", headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=SITE_API_TIMEOUT) as fh:
            return json.loads(fh.read().decode("utf-8", "replace")), ""
    except urllib.error.HTTPError as exc:
        return None, f"HTTP {exc.code}"
    except Exception as exc:                       # URLError, timeout, JSON
        return None, f"{type(exc).__name__}: {exc}"[:120]


def _agent_state(agent):
    """Map one framework agent onto the wall's shared status vocabulary.

    The framework's own states are: idle, starting, running, success, failure,
    blocked, cancelled -- plus "" when the agent has no status.json at all.

    Two orderings matter here and both were got wrong first time elsewhere:

    * `enabled` is checked BEFORE the run state, because a disabled agent can
      still be carrying a stale `failure` from before it was switched off.
      Reporting that as a fault sends someone to fix something deliberately
      turned off.
    * "" with no last_run_at is `absent` (never installed / never ran), which
      is NOT `fail`. One specpicks agent is in exactly that state right now.
    """
    if not agent.get("enabled", True):
        return "off"
    st = (agent.get("last_run_status") or "").strip()
    if not st:
        return "absent" if not agent.get("last_run_at") else "unknown"
    return {
        "success": "ok",
        "running": "busy",
        "starting": "busy",
        "idle": "ok",
        "failure": "fail",
        "blocked": "blocked",
        "cancelled": "warn",
    }.get(st, "unknown")


def _iso_age(ts):
    """Seconds since an ISO-8601 timestamp, or None. The framework always
    writes UTC with a +00:00 offset (never a bare Z), second precision."""
    if not ts:
        return None
    try:
        from datetime import datetime, timezone
        t = datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
        if t.tzinfo is None:
            t = t.replace(tzinfo=timezone.utc)
        return max(0.0, (datetime.now(timezone.utc) - t).total_seconds())
    except Exception:
        return None


def collect_site_agents():
    """Agent health per site, from one /api/agents call."""
    out = {"state": "unknown", "sites": {}, "error": ""}
    health, err = _site_api("/api/health")
    if err:
        # Refused is the common case: nothing supervises that uvicorn process.
        out["state"] = "fail"
        out["error"] = f"framework API unreachable ({err})"
        return out
    agents, err = _site_api("/api/agents")
    if err or not isinstance(agents, list):
        out["state"] = "fail"
        out["error"] = err or "unexpected /api/agents payload"
        return out

    out["state"] = "ok"
    for site in SITES:
        # Group on the id PREFIX, not the `application` field: at least one
        # agent (specpicks-user-growth-strategist) declares metadata.site =
        # "aisleprompt" and so is filed under the wrong application by the API.
        mine = [a for a in agents if str(a.get("id", "")).startswith(site + "-")]
        counts, ages = {}, []
        for a in mine:
            st = _agent_state(a)
            counts[st] = counts.get(st, 0) + 1
            if st == "ok":
                age = _iso_age(a.get("last_run_at"))
                if age is not None:
                    ages.append(age)
        failing = sorted(str(a.get("id", "")).split("-", 1)[-1]
                         for a in mine if _agent_state(a) in ("fail", "blocked"))
        out["sites"][site] = {
            "total": len(mine),
            "counts": counts,
            "failing": failing[:4],
            "last_ok_age": min(ages) if ages else None,
        }
    return out


def collect_site_articles():
    """Published-article counts from each site's PRODUCTION Postgres.

    Two traps, both of which produce a confident wrong number:

    * The Docker containers on this host (specpicks_postgres :5432,
      aisleprompt-db-1 :5436) are STALE DEV COPIES. Production is Azure
      Postgres, reached through the DATABASE_URL_<SITE> DSNs. The gap is not
      subtle -- 400 rows locally against 3029 in production.
    * aisleprompt FUTURE-DATES articles for scheduled publishing, so
      `published_at > now() - 7 days` alone counts pieces that have not been
      published yet and overstates it by about half. Hence the upper bound.
    """
    out = {"state": "unknown", "sites": {}, "error": ""}
    try:
        import psycopg2
    except ImportError:
        # The collector runs as root, and psycopg2 may only be installed in a
        # USER site-packages directory that root does not read -- which is
        # exactly the state this host was in when the panel was written.
        #
        # Deliberately NOT worked around by appending the user's site-packages
        # to sys.path: that would have a root service import code from a
        # user-writable directory, which is a privilege-escalation route for
        # the sake of a wall decoration. The fix is `apt install
        # python3-psycopg2`; until then the panel says precisely that, because
        # a blank number is indistinguishable from a site publishing nothing.
        out["state"] = "absent"
        out["error"] = "psycopg2 not installed for root"
        for site in SITES:
            out["sites"][site] = {"state": "absent",
                                  "why": "needs python3-psycopg2"}
        return out

    any_ok = False
    for site in SITES:
        dsn = _site_secret(f"DATABASE_URL_{site.upper()}")
        if not dsn:
            out["sites"][site] = {"state": "absent",
                                  "why": f"no DATABASE_URL_{site.upper()}"}
            continue
        try:
            con = psycopg2.connect(dsn, connect_timeout=SITE_DB_TIMEOUT)
            try:
                cur = con.cursor()
                cur.execute("SET statement_timeout = 8000")
                cur.execute(
                    "SELECT count(*) FROM editorial_articles "
                    " WHERE status = 'published'"
                    "   AND published_at >  now() - interval '7 days'"
                    "   AND published_at <= now()")
                week = cur.fetchone()[0]
                cur.execute(
                    "SELECT count(*) FROM editorial_articles "
                    " WHERE status = 'published' AND published_at > now()")
                scheduled = cur.fetchone()[0]
            finally:
                con.close()
            out["sites"][site] = {"state": "ok", "week": int(week),
                                  "scheduled": int(scheduled)}
            any_ok = True
        except Exception as exc:
            out["sites"][site] = {"state": "fail",
                                  "why": f"{type(exc).__name__}: {exc}"[:100]}
    out["state"] = "ok" if any_ok else "fail"
    return out


def collect_site_deploys():
    """Deploy activity, reported honestly rather than as a round number.

    There is no deployment counter to read. What exists is the seo-deployer
    agent's run index, and it is capped at the most recent runs -- so it
    CANNOT answer a 7-day question, and any count taken from it is a floor
    over whatever window it happens to span. The window is therefore reported
    alongside the count instead of being quietly assumed to be 7 days.

    Worse, and the reason no per-site deploy count appears on this wall:
    specpicks' pipeline deploys have been blocked at the test gate, yet the
    site IS being deployed -- by hand, through specpicks/scripts/deploy-azure.sh,
    which records nothing anywhere. A "deployments" figure here would
    under-report the one site it most looks like it is describing.
    """
    runs, err = _site_api("/api/agents/seo-deployer/runs")
    if err:
        return {"state": "unknown", "why": err}
    rows = runs if isinstance(runs, list) else (runs or {}).get("runs", [])
    if not rows:
        return {"state": "absent", "why": "no runs recorded"}
    ok = sum(1 for r in rows if r.get("status") == "success")
    bad = sum(1 for r in rows if r.get("status") in ("failure", "blocked"))
    ages = [a for a in (_iso_age(r.get("started_at")) for r in rows)
            if a is not None]
    newest = rows[0]
    return {
        "state": {"success": "ok", "failure": "fail",
                  "blocked": "blocked"}.get(newest.get("status"), "unknown"),
        "ok": ok,
        "bad": bad,
        "last_age": _iso_age(newest.get("started_at")),
        # The real window these counts cover, in days. Never assume 7.
        "window_days": (max(ages) / 86400.0) if ages else None,
        "partial": True,
    }


def collect_sites():
    """Everything the wall shows for specpicks.com and aisleprompt.com."""
    return {
        "agents": collect_site_agents(),
        "articles": collect_site_articles(),
        "deploys": collect_site_deploys(),
        "collected_at": time.time(),
    }


class SlowCache:
    """TTL cache for the panels that fork a subprocess.

    Held on the collector loop rather than a thread: each of these is a few
    milliseconds, and a thread per panel would need locking around state that
    is only ever read once every couple of seconds.
    """

    def __init__(self, ttl=SLOW_TTL_SEC):
        self.ttl = ttl
        self._at = {}
        self._val = {}

    def get(self, key, fn, ttl=None):
        now = time.monotonic()
        ttl = self.ttl if ttl is None else ttl
        if key not in self._val or now - self._at.get(key, 0) >= ttl:
            try:
                self._val[key] = fn()
            except Exception as exc:
                self._val[key] = {"error": f"{type(exc).__name__}: {exc}"}
            self._at[key] = now
        return self._val[key]


# --------------------------------------------------------------------------
# assembly + atomic publish
# --------------------------------------------------------------------------

def build_state(vitals, fleet_poller, slow=None):
    state = {
        "schema": SCHEMA_VERSION,
        "ts": time.time(),
        "generated_by": "dashboard_collector.py",
    }
    state.update(vitals.sample())
    state["fleet"] = fleet_poller.snapshot() if fleet_poller else {
        "polled_at": 0, "nodes": [], "up": 0, "total": 0, "disabled": True,
    }
    state["agents"] = collect_agents()
    state["remote"] = collect_remote()

    # These three read files or fork `systemctl`, so they go through the TTL
    # cache rather than the 2s sensor cadence. Reading the two status files is
    # cheap, but they are grouped with the rest so a single knob controls how
    # often the wall's service view refreshes.
    slow = slow if slow is not None else SlowCache()
    state["gameservers"] = slow.get("gameservers", collect_gameservers)
    state["gameindex"] = slow.get("gameindex", collect_gameindex)
    state["pxe"] = slow.get("pxe", collect_pxe)
    state["services"] = slow.get("services", collect_services)
    # The site panel makes network calls (local API + two Azure Postgres
    # round-trips), so it gets a much longer TTL than the systemctl panels.
    # A publishing count does not change meaningfully inside a minute.
    state["sites"] = slow.get("sites", collect_sites, ttl=SITES_TTL_SEC)
    reconcile_status_sources(state)
    return state


# Which status-file section is fed by which unit. Used only to tell two very
# different failures apart — see reconcile_status_sources().
_STATUS_OWNERS = {
    "gameservers": "retro-gameservers-watch",
    "gameindex": "retro-gameindex",
}


def reconcile_status_sources(state):
    """Cross-check a missing status file against its unit's actual state.

    `collect_gameservers()` sees only "the file is not there" and reports
    "not running", which is right almost always — these services are started
    by hand. But if the unit IS active and the file is still missing, the
    cause is something else entirely (a sandbox that cannot see
    /run/user/<uid>, a service that has not completed its first pass, a
    status path override) and telling someone to start a service that is
    already running sends them in exactly the wrong direction.

    This collector runs with ProtectHome=read-only, which covers /run/user —
    the very place both status files live — so that is not a hypothetical.
    """
    services = {s.get("unit"): s for s in (state.get("services") or {}).get("services", [])}
    for section, unit in _STATUS_OWNERS.items():
        block = state.get(section) or {}
        if block.get("error") != "not running":
            continue
        svc = services.get(unit) or {}
        if svc.get("state") == "active":
            block["error"] = "running, but no status file yet"
            block["hint"] = block.get("path")


def publish(state, path):
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    # World-readable on purpose: the reader is the GDM greeter, running as a
    # transient `gdm-greeter-*` dynamic user we cannot name ahead of time.
    os.chmod(directory, 0o755)
    tmp = f"{path}.tmp{os.getpid()}"
    with open(tmp, "w") as fh:
        json.dump(state, fh, separators=(",", ":"))
        fh.flush()
        os.fsync(fh.fileno())
    os.chmod(tmp, 0o644)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--state-path", default=os.environ.get(
        "RETRO_DASHBOARD_STATE", DEFAULT_STATE_PATH))
    ap.add_argument("--config", default=os.environ.get(
        "RETRO_DASHBOARD_CONFIG", DEFAULT_CONFIG_PATH))
    ap.add_argument("--interval", type=float, default=2.0,
                    help="fast (local sensor) loop seconds")
    ap.add_argument("--fleet-interval", type=float, default=45.0,
                    help="fleet poll seconds")
    ap.add_argument("--fleet-timeout", type=float, default=4.0)
    ap.add_argument("--no-fleet", action="store_true",
                    help="skip the fleet sweep entirely")
    ap.add_argument("--once", action="store_true", help="one sample, then exit")
    ap.add_argument("--stdout", action="store_true",
                    help="print the state instead of writing the state file")
    args = ap.parse_args()

    secret = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
    vitals = Vitals()
    slow = SlowCache()

    poller = None
    if not args.no_fleet:
        nodes = _load_fleet_config(args.config)
        poller = FleetPoller(nodes, secret, args.fleet_interval, args.fleet_timeout)
        if args.once:
            poller.poll_once()
        else:
            poller.start()

    if args.once:
        # The fast sensors are rate-derived (CPU%, net bytes/sec), so a single
        # refresh reports zeroes. Prime once, wait, then take the real sample.
        vitals.sample()
        time.sleep(min(1.0, args.interval))
        state = build_state(vitals, poller, slow)
        if args.stdout:
            json.dump(state, sys.stdout, indent=2)
            sys.stdout.write("\n")
        else:
            publish(state, args.state_path)
        return 0

    while True:
        started = time.monotonic()
        try:
            state = build_state(vitals, poller, slow)
            if args.stdout:
                json.dump(state, sys.stdout)
                sys.stdout.write("\n")
                sys.stdout.flush()
            else:
                publish(state, args.state_path)
        except Exception as exc:  # never let one bad sample kill the service
            print(f"collector: sample failed: {exc}", file=sys.stderr)
        time.sleep(max(0.2, args.interval - (time.monotonic() - started)))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
