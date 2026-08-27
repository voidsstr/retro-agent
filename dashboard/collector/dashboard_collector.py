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
import shutil
import socket
import subprocess
import sys
import threading
import time

SCHEMA_VERSION = 1

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
# assembly + atomic publish
# --------------------------------------------------------------------------

def build_state(vitals, fleet_poller):
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
    return state


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
        state = build_state(vitals, poller)
        if args.stdout:
            json.dump(state, sys.stdout, indent=2)
            sys.stdout.write("\n")
        else:
            publish(state, args.state_path)
        return 0

    while True:
        started = time.monotonic()
        try:
            state = build_state(vitals, poller)
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
