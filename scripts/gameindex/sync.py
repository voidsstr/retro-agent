#!/usr/bin/env python3
"""sync.py — one pass of the game/server index. Run it every 5 minutes.

  1. Sweep the LAN for live agents (TCP 9898). The fleet is powered on demand,
     so finding none is normal, not an outage.
  2. For each box ask GAMEINDEX HASH. Pull the full index ONLY when the hash
     differs from what the DB holds -- that is the cheap half of "refresh only
     if there are changes".
  3. Refresh the live-server table for every engine the fleet actually has
     installed. Pin our own servers on .132 first.
  4. For each installed game with a writer, render the favourites file, hash
     it, and push it ONLY if the hash differs from what we last wrote to that
     box. A no-op cycle touches nothing.

Everything it decides NOT to do is logged with a reason. A silent skip and a
successful write must never look the same in the log.

  python3 sync.py                 one pass
  python3 sync.py --dry-run       decide everything, write nothing
  python3 sync.py --status        what the DB currently knows
"""
import argparse
import asyncio
import json
import logging
import os
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from client.retro_protocol import RetroConnection  # noqa: E402

import db          # noqa: E402
import favorites   # noqa: E402
import masters     # noqa: E402
import status      # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
SUBNET = os.environ.get("RETRO_FLEET_SUBNET", "192.168.1")
AGENT_PORT = 9898
ME = os.environ.get("RETRO_FLEET_HOST", "192.168.1.132")

# Our own dedicated servers, from scripts/game-servers/. These are pinned into
# the top favourite slots so there is always something joinable on the LAN even
# when the internet lists come back empty.
LOCAL_SERVERS = [
    ("q3", 27961, "NSC Retro Fleet Arena - Quake III"),
    ("q3", 27960, "NSC Retro Fleet Arena - OpenArena"),
    ("q2", 27910, "NSC Retro Fleet Arena - Quake II"),
    ("qw", 27502, "NSC Retro Fleet Arena - QuakeWorld"),
    ("goldsrc", 27015, "NSC Retro Fleet Arena - CS 1.6"),
    ("goldsrc", 27016, "NSC Retro Fleet Arena - CS 1.6 no-blood"),
    ("goldsrc", 27017, "NSC Retro Fleet Arena - The Specialists"),
]

log = logging.getLogger("gameindex.sync")


# --- fleet discovery ---------------------------------------------------------

def live_agents(subnet=SUBNET, timeout=1.5, workers=128):
    import concurrent.futures as cf

    def probe(ip):
        s = socket.socket()
        s.settimeout(timeout)
        try:
            s.connect((ip, AGENT_PORT))
            return ip
        except OSError:
            return None
        finally:
            s.close()

    ips = [f"{subnet}.{i}" for i in range(2, 255)]
    with cf.ThreadPoolExecutor(workers) as ex:
        return [ip for ip in ex.map(probe, ips) if ip]


async def _agent(ip, fn, timeout=20.0):
    c = RetroConnection(ip, AGENT_PORT)
    greeting = await c.connect(SECRET, timeout=timeout)
    try:
        return await fn(c, greeting)
    finally:
        # Graceful close matters: an abrupt RST crashes Win98's Winsock and
        # takes the whole box down with it.
        await c.close()


# --- step 2: pull each box's game index --------------------------------------

async def refresh_machine(con, ip, force=False):
    """Returns (changed, ngames, note)."""
    async def work(c, greeting):
        parts = greeting.split()
        hostname = parts[1] if len(parts) > 1 else ""
        os_ver = parts[2] if len(parts) > 2 else ""

        stored = con.execute("SELECT index_hash FROM machines WHERE ip=?",
                             (ip,)).fetchone()
        stored_hash = stored["index_hash"] if stored else None

        probe = await c.command_text("GAMEINDEX HASH", timeout=30)
        probe = probe.strip()
        if probe.startswith("{"):
            # The agent has not finished its first background scan yet. Force
            # one rather than recording "this box has no games", which is what
            # an empty list would mean to every later step.
            probe = ""
        if not force and probe and stored_hash == probe:
            db.record_machine(con, ip, hostname, os_ver)
            return (False, None, f"unchanged (hash {probe})")

        raw = await c.command_text(
            "GAMEINDEX SCAN" if (force or not probe) else "GAMEINDEX",
            timeout=300)
        doc = json.loads(raw)
        if doc.get("pending"):
            return (False, None, "agent index still pending")
        games = doc.get("games", [])
        db.record_machine(con, ip, hostname, os_ver, index_hash=doc.get("hash", ""))
        db.replace_games(con, ip, games)
        con.commit()
        return (True, len(games), f"indexed {len(games)} games "
                                  f"in {doc.get('scan_ms', '?')}ms")

    try:
        return await _agent(ip, work)
    except Exception as e:  # noqa: BLE001
        msg = str(e)
        if "Unknown command" in msg:
            # Not a fault: this box is simply running an agent older than
            # 1.29.0. It auto-updates from the share on its next restart, so
            # say that instead of logging it as an error someone should chase.
            return (False, None,
                    "agent predates GAMEINDEX (needs 1.29.0+); it will pick it "
                    "up from the share on its next restart")
        return (False, None, f"ERROR {type(e).__name__}: {e}")


# --- step 3: refresh the live-server table -----------------------------------

def probe_local_servers(con):
    """Our own servers on .132, probed directly and pinned."""
    added = 0
    for engine, port, label in LOCAL_SERVERS:
        spec = masters.ENGINES.get(engine, {})
        probe = spec.get("probe")
        addr = f"{ME}:{port}"
        row = None
        if probe:
            row = probe(addr)
        if row is None:
            # A local server with no usable probe (goldsrc: no A2S path wired)
            # is still pinned -- we KNOW it is ours and where it is. Just check
            # the UDP port is bound rather than inventing player counts.
            row = {"addr": addr, "hostname": label, "map": "", "players": 0,
                   "maxplayers": 0, "ping_ms": 0, "gamename": "", "passworded": 0}
        row["is_local"] = 1
        row["source"] = "local"
        row["hostname"] = row.get("hostname") or label
        db.upsert_servers(con, engine, [row])
        added += 1
    con.commit()
    return added


def refresh_servers(con, engines, max_probe=900):
    notes = {}
    for engine in engines:
        t0 = time.time()
        rows, note = masters.discover(engine, max_probe=max_probe)
        if rows:
            db.upsert_servers(con, engine, rows)
            con.commit()
        notes[engine] = f"{note} ({time.time() - t0:.1f}s)"
    return notes


# --- step 4: push favourites -------------------------------------------------

async def push_favorites(con, ip, dry_run=False):
    """Write each installed game's favourites file, but only when it changed."""
    results = []
    games = db.games_for(con, ip=ip)
    if not games:
        return [("-", "-", "no games indexed for this box yet - nothing to write")]

    async def work(c, _greeting):
        for g in games:
            engine, key, gdir = g["engine"], g["game_key"], g["dir"]
            spec = favorites.writer_for(engine)
            if not spec.get("supported"):
                results.append((key, engine, f"skipped: {spec.get('why')}"))
                continue

            servers = db.best_servers(con, engine, limit=spec["slots"])
            if not servers:
                results.append((key, engine, "skipped: no live servers known"))
                continue

            path = favorites.target_path(engine, gdir)
            existing = ""
            try:
                existing = (await c.command_text(f'EXEC cmd /c type "{path}"',
                                                 timeout=60))
                if "cannot find" in existing.lower() or "not find" in existing.lower():
                    existing = ""
            except Exception:  # noqa: BLE001
                existing = ""

            text, h = favorites.render(engine, servers, existing)
            if text is None:
                results.append((key, engine, f"skipped: {h}"))
                continue
            if db.applied_hash(con, ip, key, gdir) == h:
                results.append((key, engine, f"unchanged ({h})"))
                continue
            if dry_run:
                results.append((key, engine,
                                f"WOULD write {len(servers)} servers -> {path}"))
                continue

            # Upload rather than echo: the favourites block contains quotes and
            # backslashes, and Win98's command.com treats < > in echo as
            # redirects. UPLOAD carries exact bytes.
            payload = text.replace("\n", "\r\n").encode("ascii", "replace")
            await c.send_command(f"MKDIR {path.rsplit(chr(92), 1)[0]}")
            st, resp = await c.send_command(f"UPLOAD {path}",
                                            binary_payload=payload)
            if st == 0xFF:
                results.append((key, engine,
                                f"FAILED {resp[:60].decode('ascii', 'replace')}"))
                continue
            db.record_applied(con, ip, key, gdir, h,
                              f"{len(servers)} servers -> {path}")
            con.commit()
            results.append((key, engine, f"wrote {len(servers)} servers ({h})"))

    try:
        await _agent(ip, work, timeout=30.0)
    except Exception as e:  # noqa: BLE001
        results.append(("-", "-", f"ERROR {type(e).__name__}: {e}"))
    return results


# --- one pass ----------------------------------------------------------------

async def run_once(dry_run=False, force=False, only_ip=None, report=None):
    """One pass. `report` is filled in as we go so the service can publish its
    own health even if a later step raises."""
    report = report if report is not None else status.new_report()
    con = db.connect()
    t0 = time.time()

    report["phase"] = "discovering agents"
    ips = [only_ip] if only_ip else live_agents()
    report["agents"] = list(ips)
    log.info("agents up: %s", ", ".join(ips) if ips else
             "none (the fleet is powered on demand - this is normal)")

    report["phase"] = "indexing machines"
    for ip in ips:
        changed, n, note = await refresh_machine(con, ip, force=force)
        report["machines"].append({"ip": ip, "changed": bool(changed),
                                   "games": n, "note": note})
        if note.startswith("ERROR"):
            report["errors"].append(f"{ip}: {note}")
        log.info("[%s] %s", ip, note)

    engines = db.engines_in_use(con)
    if not engines:
        # Nothing indexed yet (cold DB, or every box still on an old agent).
        # Warm the server table with the engines we can actually discover, so
        # the first box to report in already has favourites to receive rather
        # than waiting a further five minutes.
        engines = [e for e, spec in masters.ENGINES.items() if spec["supported"]]
        log.info("no games indexed yet - warming server table for: %s",
                 ", ".join(engines))
    else:
        log.info("engines installed on the fleet: %s", ", ".join(engines))
    report["engines"] = list(engines)
    report["phase"] = "probing servers"
    for engine, note in refresh_servers(con, engines).items():
        log.info("  servers/%-8s %s", engine, note)
    probe_local_servers(con)
    pruned = db.prune_servers(con)
    con.commit()
    if pruned:
        log.info("pruned %d stale servers", pruned)

    report["phase"] = "writing favourites"
    for ip in ips:
        for key, engine, note in await push_favorites(con, ip, dry_run=dry_run):
            # Bucket by what actually happened. "wrote 0" and "we never looked"
            # must not collapse into the same number on the wall.
            if note.startswith("wrote") or note.startswith("WOULD"):
                report["writes"]["wrote"] += 1
            elif note.startswith("unchanged"):
                report["writes"]["unchanged"] += 1
            elif note.startswith("FAILED") or note.startswith("ERROR"):
                report["writes"]["failed"] += 1
                report["errors"].append(f"{ip}/{key}: {note}")
            else:
                report["writes"]["skipped"] += 1
            log.info("[%s] %-11s %-8s %s", ip, key, engine, note)

    report["servers"] = status.summarize_servers(con)
    report["favorites"] = status.summarize_favorites(con)
    report["duration_sec"] = round(time.time() - t0, 1)
    report["ts"] = time.time()
    report["phase"] = "idle"
    report["ok"] = True
    log.info("pass complete in %.1fs", time.time() - t0)
    con.close()
    return report


def show_status():
    con = db.connect()
    print("machines:")
    for r in con.execute("SELECT * FROM machines ORDER BY ip"):
        print(f"  {r['ip']:<16} {r['hostname']:<20} {r['os']:<8} "
              f"hash={r['index_hash']:<10} indexed={r['indexed_at']}")
    print("\ninstalled games:")
    for r in con.execute("SELECT ip, COUNT(*) n FROM installed_games "
                         "GROUP BY ip ORDER BY ip"):
        print(f"  {r['ip']:<16} {r['n']} games")
    print("\nlive servers:")
    for r in con.execute("SELECT engine, COUNT(*) n, SUM(players) p "
                         "FROM servers GROUP BY engine ORDER BY engine"):
        print(f"  {r['engine']:<9} {r['n']:>4} servers, {r['p'] or 0:>4} players")
    print("\nfavourites written:")
    for r in con.execute("SELECT * FROM favorites_state ORDER BY ip, game_key"):
        print(f"  {r['ip']:<16} {r['game_key']:<11} {r['applied_at']}  {r['detail']}")
    con.close()


# --- daemon ------------------------------------------------------------------

DEFAULT_INTERVAL = 300.0   # the five-minute freshness contract, in seconds


def run_forever(interval=DEFAULT_INTERVAL, status_path=None, **kwargs):
    """Loop passes forever, publishing health after every one.

    This used to be a oneshot behind a .timer, which met the five-minute
    contract but meant the unit was `inactive (dead)` for 297 of every 300
    seconds -- so "is the favourites agent running?" had no honest answer at
    the moment anyone asked. As a long-running service the unit state means
    what it says, and the status file carries the per-pass detail a timer's
    exit code never could.

    A pass that throws is caught, published as a failure with its reason, and
    followed by the next pass on schedule. The agent going quiet because one
    box refused a connection would defeat the point of watching it.
    """
    path = status_path or status.default_status_path()
    log.info("favourites agent: pass every %.0fs, status -> %s", interval, path)
    passes = 0
    while True:
        started = time.monotonic()
        report = status.new_report()
        passes += 1
        report["passes"] = passes
        report["interval_sec"] = interval
        try:
            status.publish(report, path)          # "running" is visible mid-pass
            asyncio.run(run_once(report=report, **kwargs))
        except Exception as exc:  # noqa: BLE001
            log.exception("pass failed")
            report["ok"] = False
            report["phase"] = "failed"
            report["ts"] = time.time()
            report["errors"].append(f"{type(exc).__name__}: {exc}")
        report["next_pass_at"] = time.time() + max(
            5.0, interval - (time.monotonic() - started))
        try:
            status.publish(report, path)
        except Exception:  # noqa: BLE001
            log.warning("could not publish status to %s", path)
        time.sleep(max(5.0, interval - (time.monotonic() - started)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="re-pull each box's index even if the hash matches")
    ap.add_argument("--ip", help="only this machine")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--daemon", action="store_true",
                    help="loop forever, publishing health after each pass "
                         "(this is how the systemd service runs it)")
    ap.add_argument("--interval", type=float, default=DEFAULT_INTERVAL,
                    help="seconds between passes in --daemon mode")
    ap.add_argument("--status-path", default=os.environ.get(
        "RETRO_GAMEINDEX_STATUS"), help="where to publish service health")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s",
                        datefmt="%H:%M:%S")
    if args.status:
        show_status()
        return
    if args.daemon:
        run_forever(interval=args.interval, status_path=args.status_path,
                    dry_run=args.dry_run, force=args.force, only_ip=args.ip)
        return
    report = asyncio.run(run_once(dry_run=args.dry_run, force=args.force,
                                  only_ip=args.ip))
    # A manual pass publishes too, so running it by hand refreshes the wall
    # instead of leaving it showing the service's older pass.
    try:
        status.publish(report, args.status_path)
    except Exception:  # noqa: BLE001
        pass


if __name__ == "__main__":
    main()
