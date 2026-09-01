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
import re
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
#
# Three columns beyond the obvious, each of which is load-bearing:
#
#   query_port  where the server answers a QUERY, which is not always the game
#               port + 1. UT99 here is 7797/7798 but UT2004 is 7777/7787, and
#               guessing +1 makes our own live UT2004 server read as down.
#   gamename    what a client must be running to join it. All ten servers sit
#               on one IP, so without this a Counter-Strike box is handed the
#               Specialists server and a Quake III box the OpenArena one --
#               addresses that connect and then reject you.
#   name        the label written beside the address in the favourites file.
#
# Tribes 2 (docker, :28000) is deliberately absent: TribesNext encrypts the
# info response and no staged title has a favourites file to put it in.
LOCAL_SERVERS = [
    dict(engine="q3", port=27961, gamename="baseq3",
         name="NSC Retro Fleet Arena - Quake III"),
    dict(engine="q3", port=27960, gamename="baseoa",
         name="NSC Retro Fleet Arena - OpenArena"),
    # Added 2026-08-31 with the servers themselves. Each carries the gamename
    # its clients report, so `accepts` can keep them apart: all fourteen fleet
    # servers sit on one IP, and a Quake III box handed the Team Arena address
    # gets a connection that is then rejected.
    dict(engine="q3", port=27962, gamename="missionpack",
         name="NSC Retro Fleet Arena - Team Arena"),
    dict(engine="q3", port=29070, gamename="base",
         name="NSC Retro Fleet Arena - Jedi Academy"),
    dict(engine="q3", port=20100, gamename="sof2mp",
         name="NSC Retro Fleet Arena - SoF II"),
    # Return to Castle Wolfenstein, added 2026-09-01. The gamename it reports
    # is "main" -- RTCW's own basegame directory -- which is what keeps a
    # Quake III box from being handed this address and rejected on connect.
    dict(engine="q3", port=27963, gamename="main",
         name="NSC Retro Fleet Arena - RTCW"),
    dict(engine="q2", port=27910, gamename="baseq2",
         name="NSC Retro Fleet Arena - Quake II"),
    # NetQuake, NOT QuakeWorld. It is listed for the record and for
    # `sync.py --status`; no staged NetQuake client keeps a favourites file,
    # so nothing is written for it (see favorites.UNWRITABLE["quake"]).
    dict(engine="nq", port=26000, gamename="netquake",
         name="NSC Retro Fleet Arena - Quake"),
    dict(engine="qw", port=27502, gamename="qw",
         name="NSC Retro Fleet Arena - QuakeWorld"),
    dict(engine="goldsrc", port=27015, gamename="cstrike",
         name="NSC Retro Fleet Arena - CS 1.6"),
    dict(engine="goldsrc", port=27016, gamename="cstrike",
         name="NSC Retro Fleet Arena - CS 1.6 no-blood"),
    dict(engine="goldsrc", port=27017, gamename="ts",
         name="NSC Retro Fleet Arena - The Specialists"),
    dict(engine="unreal", port=7797, query_port=7798, gamename="ut",
         name="NSC Retro Fleet Arena - UT99"),
    dict(engine="ut2k4", port=7777, query_port=7787, gamename="ut2004",
         name="NSC Retro Fleet Arena - UT2004"),
    # Unreal Gold on OldUnreal 227k. gamename "unreal" (not "ut") is what the
    # UdpServerQuery reports and is what `accepts` matches on, so a UT99 box is
    # never handed it. NOTE the query is `\info\`, not the UT family's
    # `\status\` -- see masters._unreal_probe.
    dict(engine="unreal", port=7807, query_port=7808, gamename="unreal",
         name="NSC Retro Fleet Arena - Unreal Gold"),
    # Deus Ex. Same UE1 GameSpy shape as Unreal/UT99 and the same +1 query
    # port; gamename "deusex" is what keeps a UT99 or Unreal Gold box from
    # being handed it.
    dict(engine="unreal", port=7790, query_port=7791, gamename="deusex",
         name="NSC Retro Fleet Arena - Deus Ex"),
    # Serious Sam. TFE and TSE are DIFFERENT GAMES with different gamenames
    # (serioussam / serioussamse) - a TFE client handed the TSE address
    # connects and is rejected. No favourites file is written for either (see
    # favorites.UNWRITABLE["sam"]); they are listed so `sync.py --status`
    # reports them and so the probe verifies them each pass.
    dict(engine="serioussam", port=25600, query_port=25601, gamename="serioussam",
         name="NSC Retro Fleet Arena - Serious Sam TFE"),
    dict(engine="serioussam", port=25610, query_port=25611, gamename="serioussamse",
         name="NSC Retro Fleet Arena - Serious Sam TSE"),
    # DOOM 3. id Tech 4, so neither `getstatus` nor `\status\` reaches it.
    dict(engine="idtech4", port=27666, gamename="baseDOOM-1",
         name="NSC Retro Fleet Arena - DOOM 3"),
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
    """Our own servers on .132, probed directly and pinned.

    Returns (pinned, down) so a pass can say which of our own servers did not
    answer. They are pinned either way -- a box should still carry the address
    of a server that is merely restarting -- but "we asked and it did not
    reply" is worth reporting rather than swallowing.
    """
    pinned, down = 0, []
    for spec in LOCAL_SERVERS:
        engine, port, label = spec["engine"], spec["port"], spec["name"]
        addr = f"{ME}:{port}"
        row = masters.probe_server(engine, addr,
                                   query_port=spec.get("query_port", 0),
                                   gamename=spec.get("gamename", ""))
        if row is None:
            # Still pinned: we KNOW it is ours and where it is. What we do NOT
            # do is invent a player count for a server that did not answer.
            down.append(f"{engine}:{port}")
            row = {"addr": addr, "hostname": label, "map": "", "players": 0,
                   "maxplayers": 0, "ping_ms": 0, "passworded": 0}
        row["is_local"] = 1
        row["source"] = "local"
        row["hostname"] = row.get("hostname") or label
        row["query_port"] = spec.get("query_port") or row.get("query_port") or 0
        # The DECLARED gamename wins for our own servers. The probe's is
        # usually the same, but this table is the thing we actually control,
        # and one surprising cvar must not filter a box away from a server we
        # know it can join.
        row["gamename"] = spec["gamename"]
        db.upsert_servers(con, engine, [row])
        pinned += 1
    con.commit()
    return pinned, down


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

async def read_existing(conn, path):
    """Read the file we are about to merge into.

    Returns (text, state, why) where state is one of:

        "read"        we have the exact bytes; merge into them
        "missing"     the file genuinely is not there; safe to create
        "unreadable"  we could not tell; the caller MUST NOT write

    Three things make this fussier than it looks, all learned by getting it
    wrong:

    * **DOWNLOAD, not `EXEC cmd /c type`.** The shell path went through
      cmd.exe, so it could truncate on a big file, mangle encodings, and on
      Win98 it is a different shell entirely. DOWNLOAD returns the exact bytes
      with a real status code.
    * **A failed read is not an empty file.** The previous version caught every
      exception and set `existing = ""`, so a timeout or a busy box turned into
      "this file is empty" and the merge wrote only our block. That destroyed
      another session's staged settings on one machine while leaving them
      intact on another -- the hardest possible shape to debug.
    * **Existence is decided by a directory listing, not by error prose.** The
      previous version matched "cannot find" against the *file's own content*.
      Only a positive listing of the parent directory lets us say "missing"
      and create the file; anything else is "unreadable" and we leave it alone.
    """
    try:
        raw = await conn.command_binary(f"DOWNLOAD {path}", timeout=60)
        return raw.decode("ascii", "replace"), "read", ""
    except Exception as exc:  # noqa: BLE001
        first_error = f"{type(exc).__name__}: {exc}"[:120]

    # The read failed. It is only safe to create the file if we can positively
    # confirm it is absent, so ask the directory.
    parent, _, fname = path.rpartition("\\")
    try:
        listing = await conn.command_text(f"DIRLIST {parent}", timeout=30)
    except Exception as exc:  # noqa: BLE001
        return "", "unreadable", f"{first_error}; DIRLIST also failed: " \
                                 f"{type(exc).__name__}"
    try:
        entries = json.loads(listing)
        names = entries.get("entries", entries) if isinstance(entries, dict) \
            else entries
        present = any(
            (e.get("name") if isinstance(e, dict) else str(e)).lower()
            == fname.lower() for e in names)
    except Exception:  # noqa: BLE001
        # A listing we cannot parse is not evidence of absence.
        return "", "unreadable", f"{first_error}; DIRLIST unparseable"

    if present:
        # It is there and we still could not read it. Do not touch it.
        return "", "unreadable", f"{first_error}; the file exists"
    return "", "missing", ""



async def running_exes(conn):
    """Lowercased basenames of every process on the box.

    Used to skip a title that is running. Q3 rewrites q3config.cfg on exit and
    UT rewrites UnrealTournament.ini on exit, both from memory -- so a write
    made while the game is up is at best thrown away, and at worst reverts
    whatever the player changed in that session. The five-minute pass has no
    business landing in the middle of a game.

    Parsed by pulling every *.exe out of the reply rather than by walking the
    JSON: PROCLIST's exact shape is the agent's business, and a fleet running
    several agent versions must not turn a schema change into a lost guard.
    """
    try:
        raw = await conn.command_text("PROCLIST", timeout=30)
    except Exception:  # noqa: BLE001
        # Not knowing is not the same as "nothing is running", but refusing to
        # write anything at all because one command failed would be worse. Say
        # so by returning None, and let the caller decide.
        return None
    return {m.lower() for m in re.findall(r'[^"\\/:*?<>|]+\.exe', raw)}


async def push_favorites(con, ip, dry_run=False):
    """Write each installed game's favourites file, but only when it changed."""
    results = []
    games = db.games_for(con, ip=ip)
    if not games:
        return [("-", "-", "no games indexed for this box yet - nothing to write")]

    async def work(c, _greeting):
        running = await running_exes(c)
        # Two keys can name one file: the staged Quake III tree ships both
        # quake3.exe and ioquake3.x86.exe, so `quake3` and `ioquake3` are both
        # detected in the same directory and both resolve to
        # baseq3\autoexec.cfg. Writing it twice per pass is pure waste.
        done_paths = {}
        for g in games:
            key, gdir = g["game_key"], g["dir"]
            # The TITLE decides, not the engine the agent reported. An agent
            # says "-" for Deus Ex because that box has no server browser it
            # can name; the host knows Deus Ex is Unreal engine and where its
            # favourites live. Doing this here rather than in gameindex.c
            # keeps a favourites change off the critical path of a fleet-wide
            # agent republish.
            pol = favorites.policy_for(key, g["engine"])
            if not pol.get("supported"):
                results.append((key, g["engine"], f"skipped: {pol.get('why')}"))
                continue
            engine = pol["engine"]

            if favorites.SKIP_DIRS.search(gdir):
                results.append((key, engine,
                                f"skipped: {gdir} is a benchmark harness - "
                                f"nothing of ours goes in there"))
                continue

            servers = db.best_servers(con, engine, limit=pol["slots"],
                                      accepts=pol.get("accepts"),
                                      local_only=pol.get("local_only", False))
            if not servers:
                results.append((key, engine, "skipped: no live servers known"))
                continue

            # Checked HERE, after we know there is something to write.
            # Reporting BUSY for a title that had nothing to write anyway
            # inflates the retry list with work that will never happen -- and
            # the whole point of the bucket is that it means "come back".
            #
            # BUSY, not "skipped": this one needs the next pass, a title with
            # no writer does not. The prefix is what run_once buckets on, so
            # the two can never be read as the same outcome.
            exe = str(g["exe"] or "").rsplit("\\", 1)[-1].lower()
            if running is not None and exe and exe in running:
                results.append((key, engine,
                                f"BUSY: {exe} is running - not attempted. It "
                                f"rewrites this file on exit, so our write "
                                f"would be lost or would revert what the "
                                f"player just set; retry next pass"))
                continue

            path = favorites.target_path(engine, gdir, key)
            if path in done_paths:
                other, other_hash = done_paths[path]
                db.record_applied(con, ip, key, gdir, other_hash,
                                  f"same file as {other}")
                con.commit()
                results.append((key, engine,
                                f"unchanged (same file as {other} this pass)"))
                continue
            existing, state, why = await read_existing(c, path)
            if state == "missing" and not pol.get("create", True):
                # Not an error, and not something to fix by creating it. The
                # file's ABSENCE is the evidence: this build does not use this
                # mechanism (a WON Half-Life has no revSrvBrowser and so no
                # config\serverbrowser.vdf), and an ini holding nothing but a
                # favourites section would be worse than no ini at all.
                results.append((key, engine,
                                f"skipped: {path} does not exist, and this "
                                f"title's favourites file is one we update, "
                                f"never create"))
                continue
            if state == "unreadable":
                # NEVER write when we could not read. Merging against an empty
                # string would silently replace whatever is there with only our
                # block -- which is exactly how another session's r_fullscreen
                # and r_mode vanished from one box while surviving on another
                # (2026-08-29). "The file is not there" and "I could not read
                # the file" mean opposite things and must never collapse.
                results.append((key, engine,
                                f"skipped: cannot read {path} ({why}) — "
                                f"refusing to write, it would clobber the file"))
                continue

            try:
                text, h = favorites.render(engine, servers, existing, key=key)
            except favorites.WouldClobber as exc:
                # The merge itself found it would lose somebody's settings.
                # Leave the file alone and make the reason loud.
                log.error("[%s] %s %s: %s", ip, key, path, exc)
                results.append((key, engine, f"FAILED would clobber: {exc}"))
                continue
            if text is None:
                results.append((key, engine, f"skipped: {h}"))
                continue
            done_paths[path] = (key, h)
            # Compare against WHAT IS ON THE BOX, not against what we last
            # meant to put there.
            #
            # The DB's applied_hash records our own intent. If anything else
            # rewrites the file -- and something does: GAMESYNC re-copied the
            # staged UnrealTournament.ini over ours on .171 at 00:54, taking
            # the favourites back to the three the library ships -- then the
            # next pass renders the same output from the same staged base,
            # matches its own recorded hash, and skips. The box keeps the
            # reverted file FOREVER while the log says "unchanged". That is
            # the house failure mode exactly: a tool reporting success while
            # being wrong, and invisible because the reverted state and the
            # never-written state look identical from here.
            #
            # We already hold the current bytes from read_existing, so the
            # honest test is free. applied_hash stays for the status wall.
            if text.splitlines() == existing.splitlines():
                results.append((key, engine, f"unchanged ({h})"))
                if db.applied_hash(con, ip, key, gdir) != h:
                    db.record_applied(con, ip, key, gdir, h,
                                      f"{len(servers)} servers -> {path}")
                    con.commit()
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

    engines = sorted(set(db.engines_in_use(con))
                     | set(favorites.engines_for_keys(db.keys_in_use(con))))
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
    pinned, down = probe_local_servers(con)
    log.info("  pinned %d of our own servers%s", pinned,
             ("; NO REPLY from " + ", ".join(down)) if down else "")
    if down:
        report["errors"].append("our own servers did not answer: "
                                + ", ".join(down))
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
            elif note.startswith("BUSY"):
                # Reported at the top level too: a pass that reached every box
                # and wrote nothing because all eight were mid-game is a
                # completely different fact from a pass with nothing to do.
                report["writes"]["busy"] += 1
                report.setdefault("busy", []).append(f"{ip}/{key}: {note}")
            elif note.startswith("FAILED") or note.startswith("ERROR"):
                report["writes"]["failed"] += 1
                report["errors"].append(f"{ip}/{key}: {note}")
            else:
                report["writes"]["skipped"] += 1
            log.info("[%s] %-11s %-8s %s", ip, key, engine, note)

    if report["writes"]["busy"]:
        log.info("NOT ATTEMPTED on %d title(s) - a game was running. These "
                 "need the next pass; they are not 'unchanged'.",
                 report["writes"]["busy"])
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
