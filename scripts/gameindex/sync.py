#!/usr/bin/env python3
"""sync.py — one pass of the game/server index. Run it every 5 minutes.

  1. Sweep the LAN for live agents (TCP 9898). The fleet is powered on demand,
     so finding none is normal, not an outage.
  2. For each box ask GAMEINDEX HASH. Pull the full index ONLY when the hash
     differs from what the DB holds -- that is the cheap half of "refresh only
     if there are changes".
  3. Refresh the live-server table for every engine the fleet actually has
     installed. Pin our own servers on .132 first.
  4. For each FILE the box's installed games read favourites from, render
     it and push it ONLY if the game would see a difference from what the box
     already holds. A no-op cycle touches nothing, and says "unchanged".

Everything it decides NOT to do is logged with a reason. A silent skip and a
successful write must never look the same in the log.

  python3 sync.py                 one pass
  python3 sync.py --dry-run       decide everything, write nothing
  python3 sync.py --status        what the DB currently knows
"""
import argparse
import asyncio
import errno
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
    # UT2003 2225 -- the staged tree's own UCC.exe under Wine, added
    # 2026-10-01 (scripts/game-servers/ut2003/). NO query_port on purpose:
    # without one, masters.probe_server uses the UE2 NATIVE probe on game+1,
    # which checks net version 121 -- so a UT2004 reply (128) is never pinned
    # into a UT2003 box's favourites. The server does not uplink to GameSpy,
    # so a game+10 `\status\` probe would find nothing.
    dict(engine="ut2k3", port=7757, gamename="ut2003",
         name="NSC Retro Fleet Arena - UT2003"),
    # Unreal Gold 226 -- the staged tree's own UCC.exe under Wine since
    # 2026-09-28 (it replaced an OldUnreal 227k server that the staged 226
    # clients could not join; same ports, so this row did not change).
    # gamename "unreal" (not "ut") is what the UdpServerQuery reports and is
    # what `accepts` matches on, so a UT99 box is never handed it. NOTE the
    # hostname is in the `\info\` reply, not the first `\status\` datagram --
    # see masters._gamespy_status.
    dict(engine="unreal", port=7807, query_port=7808, gamename="unreal",
         name="NSC Retro Fleet Arena - Unreal Gold"),
    # Deus Ex. Same UE1 GameSpy shape as Unreal/UT99 and the same +1 query
    # port; gamename "deusex" is what keeps a UT99 or Unreal Gold box from
    # being handed it. Probed and reported, but no favourites file is written
    # for it: Deus Ex has no screen that reads one (favorites.UNWRITABLE).
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
    # Shogo. GameSpy on the GAME PORT ITSELF, not port + 1 like the UT family
    # and Serious Sam -- so query_port is 27888 too, and declaring it skips a
    # pointless probe of 27889. (It does NOT make the probe fast: every
    # GameSpy probe here reads three datagrams for a reply that may be split,
    # so a single-packet answer always costs the remaining timeouts. UT99
    # measures the same 2.5s.)
    dict(engine="lithtech", port=27888, query_port=27888, gamename="shogo",
         name="NSC Retro Fleet Arena - Shogo"),
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


def host_holds_address(addr):
    """True if this host holds `addr`, False if it positively does not, None
    if the question cannot be asked (a hostname that does not resolve, no
    sockets). Binding a UDP socket to it is the kernel's own answer."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    except OSError:
        return None
    try:
        s.bind((addr, 0))
        return True
    except OSError as exc:
        return False if exc.errno == errno.EADDRNOTAVAIL else None
    finally:
        s.close()


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

async def _listing(conn, path, dirs_only=False):
    """Lower-cased names in one directory, or (None, why) if we cannot tell."""
    try:
        listing = await conn.command_text(f"DIRLIST {path}", timeout=30)
    except Exception as exc:  # noqa: BLE001
        return None, f"DIRLIST also failed: {type(exc).__name__}"
    try:
        entries = json.loads(listing)
        items = entries.get("entries", entries) if isinstance(entries, dict) \
            else entries
        names = set()
        for e in items:
            if isinstance(e, dict):
                if dirs_only and e.get("is_dir") is False:
                    continue
                names.add(str(e.get("name", "")).lower())
            else:
                names.add(str(e).lower())
        return names, ""
    except Exception:  # noqa: BLE001
        # A listing we cannot parse is not evidence of absence.
        return None, "DIRLIST unparseable"


async def _read_file(conn, path):
    """Read a file we are about to merge into, as exact bytes.

    Returns (bytes, state, why) where state is one of:

        "read"        we have the exact bytes; merge into them
        "missing"     the file is not there and its folder IS; safe to create
        "no-parent"   the folder itself is not there; create NOTHING
        "unreadable"  we could not tell; the caller MUST NOT write

    Four things make this fussier than it looks, all learned by getting it
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
      Only a positive listing lets us say "missing" and create the file;
      anything else is "unreadable" and we leave it alone.
    * **DIRLIST answers `[]` - success, no error - for a folder that does not
      exist**, exactly as it does for an empty one (agent/src/files.c sends an
      empty array when FindFirstFileA fails). Reading that as "the folder is
      there and the file is not" is how favourites-only baseq3 folders were
      created inside Jedi Academy, SoF2 and MOHAA. So an empty listing sends us
      one level up, and the folder has to appear there by name.
    """
    try:
        raw = await conn.command_binary(f"DOWNLOAD {path}", timeout=60)
        return bytes(raw), "read", ""
    except Exception as exc:  # noqa: BLE001
        first_error = f"{type(exc).__name__}: {exc}"[:120]

    # The read failed. It is only safe to create the file if we can positively
    # confirm it is absent AND that its folder is present, so ask.
    parent, _, fname = path.rpartition("\\")
    names, why = await _listing(conn, parent)
    if names is None:
        return b"", "unreadable", f"{first_error}; {why}"
    if fname.lower() in names:
        # It is there and we still could not read it. Do not touch it.
        return b"", "unreadable", f"{first_error}; the file exists"
    if names:
        return b"", "missing", ""          # the folder listed things: it exists
    grand, _, pname = parent.rpartition("\\")
    if not grand:
        return b"", "missing", ""          # the drive root itself
    gnames, gwhy = await _listing(conn, grand, dirs_only=True)
    if gnames is None:
        return b"", "unreadable", f"{first_error}; {gwhy}"
    if pname.lower() in gnames:
        return b"", "missing", ""          # an empty folder, positively there
    return b"", "no-parent", f"{parent} does not exist"


async def read_existing(conn, path):
    """The text of the file we are about to merge into: (text, state, why).

    Decoded as latin-1, which maps every byte to one character and back, so a
    byte >= 0x80 anywhere in a game's config survives our merge unchanged. It
    used to be ascii/'replace', which silently turned any such byte into '?'
    on the next write - and then saw its own '?' as a difference to "fix" on
    every pass after.
    """
    raw, state, why = await _read_file(conn, path)
    return raw.decode("latin-1"), state, why


async def running_exes(conn):
    """Lowercased basenames of every process on the box.

    Used to skip a file whose game is running. Q3 rewrites q3config.cfg on
    exit and UT rewrites UnrealTournament.ini on exit, both from memory -- so
    a write made while the game is up is at best thrown away, and at worst
    reverts whatever the player changed in that session. The five-minute pass
    has no business landing in the middle of a game.

    Parsed by pulling every *.exe out of the reply rather than by walking the
    JSON: PROCLIST's exact shape is the agent's business, and a fleet running
    several agent versions must not turn a schema change into a lost guard.
    CASE-INSENSITIVELY: Win98's PROCLIST names are upper-case full paths
    (C:\\WINDOWS\\EXPLORER.EXE), and a pattern that only knew `.exe` found
    nothing there, so on .243 this guard could never fire.
    """
    try:
        raw = await conn.command_text("PROCLIST", timeout=30)
    except Exception:  # noqa: BLE001
        # Not knowing is not the same as "nothing is running", but refusing to
        # write anything at all because one command failed would be worse. Say
        # so by returning None, and let the caller decide.
        return None
    return {m.lower() for m in re.findall(r'[^"\\/:*?<>|]+\.exe', raw,
                                          re.IGNORECASE)}


# GAMESYNC STATUS reports idle, sizing, copying, done, failed or skipped; the
# first two are a sync in flight (scripts/fleet/autodeploy.py BUSY).
GAMESYNC_BUSY = ("sizing", "copying")


async def gamesync_busy(conn):
    """GAMESYNC's state when it is copying on this box, else None.

    A favourites file is a staged file with our block added, so its size and
    mtime differ from the library's and GAMESYNC copies the staged one back
    over it. Measured on .123 at 23:47 on 2026-09-28: all nine files written
    at 23:47:35 were overwritten by a sync that started at 23:47:39. Writing
    while it runs is work thrown away within seconds; the next pass does it
    properly. An agent that cannot answer (older build, busy) is not a reason
    to stop - it simply is not known to be syncing.
    """
    try:
        st = json.loads(await conn.command_text("GAMESYNC STATUS", timeout=25))
    except Exception:  # noqa: BLE001
        return None
    state = str(st.get("state", "")).lower() if isinstance(st, dict) else ""
    return state if state in GAMESYNC_BUSY else None


_NT_VERSION = re.compile(r"Win(\d+)\.(\d+)")


async def unmanaged_modern_host(c, greeting):
    """Why this box's game configs must NOT be written, or None to proceed.

    Windows 10/11 hosts run the agent only so the fleet can reach them (the
    server box WHITEBEAST is one). The agent itself leaves those hosts alone,
    but this push is host-side and swept every live agent with no OS filter: on
    2026-09-24 it had written 15 servers into WHITEBEAST's own
    UnrealTournament.ini. So ask the box. HWPROFILE's host_policy is the
    agent's own answer (1.83.2+). An older agent on Windows 10/11 reports the
    GetVersionEx shim value 6.2, so 6.2+ with no host_policy is treated as
    modern - the fleet has no Windows 8 machines. 9x/XP/Vista/7 greetings are
    never queried, which keeps the Win98 box's single-threaded agent quiet.
    """
    parts = greeting.split()
    m = _NT_VERSION.search(parts[2]) if len(parts) > 2 else None
    if not m or (int(m.group(1)), int(m.group(2))) < (6, 2):
        return None
    try:
        prof = json.loads(await c.command_text("HWPROFILE", timeout=60))
    except Exception:  # noqa: BLE001 - no answer is not permission to write
        prof = {}
    hp = prof.get("host_policy") if isinstance(prof, dict) else None
    if isinstance(hp, dict):
        return None if hp.get("managed") else \
            "modern Windows host - its agent reports it is not managed"
    return ("Windows 6.2+ on an agent too old to report host_policy - "
            "treated as a modern host (the fleet has no Windows 8 boxes)")


def _exe_name(g):
    return str(g["exe"] or "").replace("/", "\\").rsplit("\\", 1)[-1].lower()


def plan_targets(games):
    """Group a box's indexed titles by the FILE each would write.

    Returns (targets, skips). `skips` are per-title results for titles with
    nothing to write; each target is one file with every title that reads it:

        {path, engine, pol, keys, dirs, exes}

    Per FILE, because that is what a write is. The staged Quake III tree ships
    quake3.exe and ioquake3.x86.exe, so `quake3` and `ioquake3` are both
    indexed in one directory and both mean baseq3\\autoexec.cfg. The old
    per-title loop checked each title's OWN exe for BUSY: with ioquake3
    running it logged "ioquake3 BUSY - not attempted" and then let `quake3`
    (not running) write the same file on the same pass - on .123, .145, .195
    and .240, every pass the 1080p testers had ioquake3 up. One file, one
    decision, and the exes of every title that uses it are what can make it
    busy. Paths compare case-insensitively: they are Windows paths.
    """
    skips, groups, order = [], {}, []
    for g in games:
        key, gdir, reported = g["game_key"], g["dir"], g["engine"]
        pol = favorites.policy_for(key, reported)
        if not pol.get("supported"):
            skips.append((key, reported, f"skipped: {pol.get('why')}"))
            continue
        engine = pol["engine"]
        if favorites.writer_for(engine).get("binary"):
            continue           # a store outside the tree: push_servercache
        if favorites.SKIP_DIRS.search(gdir):
            skips.append((key, engine,
                          f"skipped: {gdir} is a benchmark harness - "
                          f"nothing of ours goes in there"))
            continue
        path = favorites.target_path(engine, gdir, key)
        if not path:
            skips.append((key, engine, "skipped: no file to write for this title"))
            continue
        norm = path.replace("/", "\\").lower()
        t = groups.get(norm)
        if t is None:
            t = groups[norm] = dict(path=path, engine=engine, pol=pol,
                                    keys=[], dirs=[], exes=set())
            order.append(norm)
        elif t["engine"] != engine:
            skips.append((key, engine,
                          f"skipped: {path} is also {t['keys'][0]}'s "
                          f"{t['engine']} file - two writers for one file"))
            continue
        t["keys"].append(key)
        t["dirs"].append(gdir)
        if _exe_name(g):
            t["exes"].add(_exe_name(g))
    return [groups[n] for n in order], skips


def _record(con, ip, t, h, detail, force=False):
    """Note what a file now holds, against every title that reads it."""
    for key, gdir in zip(t["keys"], t["dirs"]):
        if force or db.applied_hash(con, ip, key, gdir) != h:
            db.record_applied(con, ip, key, gdir, h, detail)
    con.commit()


async def push_favorites(con, ip, dry_run=False):
    """Write each favourites FILE the box's games read, but only when the game
    would see a difference."""
    results = []
    games = db.games_for(con, ip=ip)
    if not games:
        return [("-", "-", "no games indexed for this box yet - nothing to write")]

    async def work(c, greeting):
        why = await unmanaged_modern_host(c, greeting)
        if why:
            results.append(("*", "-", f"skipped: {why}"))
            return
        sync_state = await gamesync_busy(c)
        if sync_state:
            results.append(("*", "-",
                            f"BUSY: GAMESYNC is {sync_state} on this box - it "
                            f"copies the staged files back over ours; deferred "
                            f"to the next pass"))
            return
        running = await running_exes(c)
        targets, skips = plan_targets(games)
        results.extend(skips)
        for t in targets:
            path, engine, pol = t["path"], t["engine"], t["pol"]
            keys = "+".join(t["keys"])
            src = pol["servers_from"]
            pick = dict(limit=pol["slots"], accepts=pol.get("accepts"),
                        local_only=pol["local_only"],
                        strict_accepts=pol["strict_accepts"])
            if not db.best_servers(con, src, **pick):
                results.append((keys, engine,
                                f"skipped: no live servers known for {path}"))
                continue

            # Checked HERE, after we know there is something to write.
            # Reporting BUSY for a file that had nothing to write anyway
            # inflates the retry list with work that will never happen -- and
            # the whole point of the bucket is that it means "come back".
            #
            # BUSY, not "skipped": this one needs the next pass, a title with
            # no writer does not. The prefix is what run_once buckets on, so
            # the two can never be read as the same outcome.
            busy = sorted(t["exes"] & running) if running else []
            if busy:
                results.append((keys, engine,
                                f"BUSY: {path}: {busy[0]} is running - not "
                                f"attempted; {favorites.busy_why(engine)}; "
                                f"retry next pass"))
                continue

            existing, state, why = await read_existing(c, path)
            if state == "unreadable":
                # NEVER write when we could not read. Merging against an empty
                # string would silently replace whatever is there with only our
                # block -- which is exactly how another session's r_fullscreen
                # and r_mode vanished from one box while surviving on another
                # (2026-08-29). "The file is not there" and "I could not read
                # the file" mean opposite things and must never collapse.
                results.append((keys, engine,
                                f"skipped: cannot read {path} ({why}) — "
                                f"refusing to write, it would clobber the file"))
                continue
            if state == "no-parent":
                # The title is not installed where the index says (purged, or
                # an index older than the box's current tree). Creating the
                # folder would plant a favourites-only skeleton of it.
                results.append((keys, engine,
                                f"skipped: {why} - the title is not installed "
                                f"there, and nothing is ever created above "
                                f"its own folder"))
                continue
            if state == "missing" and not pol["create"]:
                # Not an error, and not something to fix by creating it. The
                # file's ABSENCE is the evidence: this build does not use this
                # mechanism (a WON Half-Life has no revSrvBrowser and so no
                # config\serverbrowser.vdf), and an ini holding nothing but a
                # favourites section would be worse than no ini at all.
                results.append((keys, engine,
                                f"skipped: {path} does not exist, and this "
                                f"title's favourites file is one we update, "
                                f"never create"))
                continue

            # What the box already lists stays while it is still eligible;
            # only vacancies are filled from the ranking (db.best_servers).
            listed = favorites.incumbents(engine, existing)
            servers = db.best_servers(
                con, src, **pick,
                incumbent=lambda r, e=engine, s=listed: favorites.identity(e, r) in s)
            try:
                text, h = favorites.render(engine, servers, existing,
                                           key=t["keys"][0])
            except favorites.WouldClobber as exc:
                # The merge itself found it would lose somebody's settings.
                # Leave the file alone and make the reason loud.
                log.error("[%s] %s %s: %s", ip, keys, path, exc)
                results.append((keys, engine, f"FAILED would clobber: {exc}"))
                continue
            if text is None:
                results.append((keys, engine, f"skipped: {h}"))
                continue

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
            # And compare what the GAME would see (favorites.same_favourites):
            # the same servers in another order, a label that changed its
            # colour codes, the tabs revSrvBrowser saves with - none of those
            # is a reason to write, and each used to rewrite files on every
            # pass so that a settled box never once said "unchanged".
            if favorites.same_favourites(engine, existing, text):
                on_box = h if existing.splitlines() == text.splitlines() \
                    else favorites.content_hash(existing)
                results.append((keys, engine, f"unchanged: {path} ({on_box})"))
                _record(con, ip, t, on_box, f"{len(servers)} servers -> {path}")
                continue
            if dry_run:
                results.append((keys, engine,
                                f"WOULD write {len(servers)} servers -> {path}"))
                continue

            # Upload rather than echo: the favourites block contains quotes and
            # backslashes, and Win98's command.com treats < > in echo as
            # redirects. UPLOAD carries exact bytes - latin-1, so every byte
            # read in goes back out as itself. No MKDIR: "missing" above means
            # the folder was positively seen, so it is already there.
            payload = text.replace("\n", "\r\n").encode("latin-1", "replace")
            st, resp = await c.send_command(f"UPLOAD {path}",
                                            binary_payload=payload)
            if st == 0xFF:
                results.append((keys, engine,
                                f"FAILED {path}: "
                                f"{resp[:60].decode('ascii', 'replace')}"))
                continue
            _record(con, ip, t, h, f"{len(servers)} servers -> {path}", force=True)
            results.append((keys, engine,
                            f"wrote {len(servers)} servers -> {path} ({h})"))

        results.extend(await push_servercache(c, con, ip, games, running,
                                              dry_run))

    try:
        await _agent(ip, work, timeout=30.0)
    except Exception as e:  # noqa: BLE001
        results.append(("-", "-", f"ERROR {type(e).__name__}: {e}"))
    return results


# --- Team Arena: ioquake3's servercache.dat ----------------------------------

_SHELL_FOLDERS = r"Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders"


async def box_appdata(conn):
    """%APPDATA% of the user the agent runs as - which is the user the games
    run as: the agent starts from the Run key at the console logon, and the
    desktop shortcuts start the games in that same session. ioquake3 1.36's
    homepath is SHGetFolderPath(CSIDL_APPDATA) + "\\Quake3", and Shell
    Folders\\AppData is where Windows records that path, already expanded
    (C:\\Documents and Settings\\<user>\\Application Data on XP,
    C:\\Users\\<user>\\AppData\\Roaming on 7). REGREAD runs inside the agent -
    no child process - which is what a Win9x agent needs too."""
    try:
        st, data = await conn.send_command(
            f'REGREAD HKCU "{_SHELL_FOLDERS}" AppData', timeout=30)
    except Exception as exc:  # noqa: BLE001
        return None, f"REGREAD failed: {type(exc).__name__}"
    text = bytes(data).decode("latin-1")
    if st == 0xFF:
        return None, f"REGREAD: {text[:80]}"
    try:
        doc = json.loads(text)
    except ValueError:
        return None, "REGREAD answered no AppData value"
    val = doc.get("value") if isinstance(doc, dict) else None
    path = str(val.get("data", "")) if isinstance(val, dict) else ""
    if not re.match(r"^[A-Za-z]:\\", path):
        return None, f"AppData reads {path!r}"
    if any(not 32 <= ord(ch) < 127 for ch in path):
        # Agent commands travel as ASCII; a path this protocol cannot spell
        # is one it cannot write to, and guessing an 8.3 name is not an answer.
        return None, f"AppData {path!r} is not plain ASCII"
    return path.rstrip("\\"), ""


async def _has_subdir(conn, path, sub):
    names, why = await _listing(conn, path, dirs_only=True)
    if names is None:
        return None, why
    return sub.lower() in names, ""


def _record_one(con, ip, key, where, h, detail, force=False):
    if force or db.applied_hash(con, ip, key, where) != h:
        db.record_applied(con, ip, key, where, h, detail)
        con.commit()


async def push_servercache(c, con, ip, games, running, dry_run=False):
    """Team Arena's favourites: ioquake3 1.36's %APPDATA%\\Quake3\\servercache.dat.

    Team Arena's UI never reads server1..16 - on .123, .195 and .240 its
    Favorites tab was EMPTY with them set - because its favourites are the
    ENGINE's list, loaded from this file when the UI starts and saved over it
    when the UI shuts down. The file belongs to every mod the player runs and
    also caches the internet server list, so:

      * it is written only when NO Quake III client is running - any mod may
        be about to save its in-memory copy over ours - and PROCLIST is asked
        again immediately before the upload;
      * the header's global count, all 4096 global records and every
        favourite the PLAYER added are carried through byte for byte
        (favorites.sc_merge); only entries carrying our mark are ever removed;
      * the upload is read back and compared, and anything but an exact match
        is reported as FAILED, not as written.

    Team Arena is not an indexed title: it is the missionpack\\ folder beside
    an ioquake3 install, so it is derived from the box's `ioquake3` rows.
    """
    key = "missionpack"
    pol = favorites.policy_for(key)
    if not pol.get("supported"):
        return []
    engine = pol["engine"]
    rows = [g for g in games if g["game_key"] in pol["derived_from"]]
    if not rows:
        return []
    usable = [g for g in rows if not favorites.SKIP_DIRS.search(g["dir"])]
    if not usable:
        return [(key, engine, "skipped: the only ioquake3 install here is a "
                              "benchmark harness - nothing of ours goes near it")]
    found, errors = [], []
    for g in usable:
        has, why = await _has_subdir(c, g["dir"], pol["requires_subdir"])
        if has:
            found.append(g["dir"])
        elif has is None:
            errors.append(f"{g['dir']}: {why}")
    if not found:
        if errors:
            return [(key, engine, "skipped: cannot tell whether Team Arena is "
                                  "installed (%s)" % errors[0])]
        return [(key, engine, "skipped: no %s folder beside ioquake3 in %s - "
                              "Team Arena is not installed"
                 % (pol["requires_subdir"], ", ".join(g["dir"] for g in usable)))]

    src = pol["servers_from"]
    pick = dict(limit=pol["slots"], accepts=pol.get("accepts"),
                local_only=pol["local_only"], strict_accepts=pol["strict_accepts"])
    if not db.best_servers(con, src, **pick):
        return [(key, engine, "skipped: no live Team Arena servers known")]

    appdata, why = await box_appdata(c)
    if not appdata:
        return [(key, engine, f"skipped: cannot resolve %APPDATA% ({why}) - "
                              f"ioquake3 keeps servercache.dat there")]
    home = appdata + "\\Quake3"
    path = home + "\\servercache.dat"

    exes = set(favorites.Q3_FAMILY_EXES) | {_exe_name(g) for g in rows if _exe_name(g)}
    if running is None:
        return [(key, engine, f"BUSY: {path}: cannot tell whether a Quake III "
                              f"client is running (PROCLIST failed) - not "
                              f"writing a file the game saves over on exit; "
                              f"retry next pass")]
    busy = sorted(exes & running)
    if busy:
        return [(key, engine, f"BUSY: {path}: {busy[0]} is running - not "
                              f"attempted; {favorites.busy_why(engine)}; "
                              f"retry next pass")]

    data, state, why = await _read_file(c, path)
    if state == "unreadable":
        return [(key, engine, f"skipped: cannot read {path} ({why}) — refusing "
                              f"to write, it would clobber the player's lists")]
    if state == "no-parent":
        return [(key, engine, f"skipped: {home} does not exist - ioquake3 has "
                              f"never run as this user here, so there is "
                              f"nothing to seed yet (the next pass after its "
                              f"first start will)")]
    existing = data if state == "read" else None

    ours = favorites.sc_our_addresses(existing)
    servers = db.best_servers(
        con, src, **pick,
        incumbent=lambda r: favorites.identity(engine, r) in ours)
    try:
        new, summary = favorites.sc_merge(existing, servers)
    except favorites.ServerCacheError as exc:
        log.error("[%s] %s %s: %s", ip, key, path, exc)
        return [(key, engine, f"FAILED would clobber: {path} is not ioquake3 "
                              f"1.36's servercache.dat ({exc}) - refusing to "
                              f"write")]
    h = summary["hash"]
    n = len(summary["ours"])
    detail = f"{n} servers -> {path}"
    # A server we wanted that the merge could not put in the list - the list
    # is full of the player's own, or the address has no IPv4 form - is never
    # reported as "unchanged" or as a clean "wrote". Saying "unchanged" there
    # is the shape this project keeps paying for: success reported, the fleet
    # server absent from the player's Favorites, and nothing saying so.
    missing = favorites.sc_missing(summary)
    if new is None:
        if missing:
            return [(key, engine, f"skipped: {path}: {missing} - nothing else "
                                  f"to change ({favorites.sc_listing(summary)})")]
        _record_one(con, ip, key, home, h, detail)
        return [(key, engine, f"unchanged: {path} ({h}) - "
                              f"{favorites.sc_listing(summary)}")]
    if dry_run:
        return [(key, engine, f"WOULD write {n} servers -> {path} "
                              f"(+{len(summary['added'])} "
                              f"-{len(summary['dropped'])})"
                              + (f"; {missing}" if missing else ""))]

    # PROCLIST was read before this box's other files were written; a game
    # started since would load a file that is half ours. Ask again - it costs
    # one command, only on a pass that actually writes.
    again = await running_exes(c)
    if again is None or exes & again:
        return [(key, engine, f"BUSY: {path}: a Quake III client started while "
                              f"this pass was deciding - not attempted; retry "
                              f"next pass")]
    st, resp = await c.send_command(f"UPLOAD {path}", binary_payload=new)
    if st == 0xFF:
        msg = bytes(resp[:80]).decode("ascii", "replace")
        if existing is not None and msg.startswith("Write failed"):
            # The agent truncated the file and then could not finish writing
            # it. Put the player's own bytes back rather than leave them a
            # file the game will reject.
            st2, _ = await c.send_command(f"UPLOAD {path}",
                                          binary_payload=existing)
            msg += "; original restored" if st2 != 0xFF else \
                "; RESTORING THE ORIGINAL ALSO FAILED - the file may be truncated"
        return [(key, engine, f"FAILED {path}: {msg}")]
    try:
        back = await c.command_binary(f"DOWNLOAD {path}", timeout=60)
    except Exception as exc:  # noqa: BLE001
        return [(key, engine, f"FAILED {path}: written but not read back "
                              f"({type(exc).__name__}) - not recorded")]
    if bytes(back) != new:
        return [(key, engine, f"FAILED {path}: read back {len(back)} bytes that "
                              f"are not the {len(new)} written - something else "
                              f"wrote it; not recorded")]
    _record_one(con, ip, key, home, h, detail, force=True)
    return [(key, engine, f"wrote {n} servers -> {path} ({h}); kept "
                          f"{summary['theirs']} of the player's own favourites "
                          f"and {summary['numglobal']} cached internet servers"
                          + (f"; {missing}" if missing else ""))]


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
    unindexed = {}
    for ip in ips:
        changed, n, note = await refresh_machine(con, ip, force=force)
        report["machines"].append({"ip": ip, "changed": bool(changed),
                                   "games": n, "note": note})
        if note.startswith("ERROR"):
            report["errors"].append(f"{ip}: {note}")
            unindexed[ip] = note
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
    fatal = None
    if down and len(down) == len(LOCAL_SERVERS) and \
            host_holds_address(ME) is False:
        # Every one of our servers silent AND this host not holding the
        # address they are pinned at: every favourite this pass writes points
        # at nothing. 09-26 to 09-28 this ran for ~56 hours with ok:true and a
        # green mark on the wall, because only a probe error was recorded.
        fatal = (f"this host does not hold {ME} - every fleet favourite "
                 f"points at an address nothing answers on (see "
                 f"docs/host-issues-log.md)")
        report["errors"].insert(0, fatal)
        log.error("%s", fatal)
    pruned = db.prune_servers(con)
    con.commit()
    if pruned:
        log.info("pruned %d stale servers", pruned)

    report["phase"] = "writing favourites"
    for ip in ips:
        if ip in unindexed:
            # The index in the DB is keyed by IP. If this pass could not ask
            # the box what it has, the rows may be an older tree - or another
            # machine's, when DHCP moved an address (ADMIN-PC went .246 ->
            # .195) - and a write would plant that machine's paths here.
            results = [("*", "-", "skipped: re-indexing this box failed this "
                                  "pass (%s) - not writing from an index that "
                                  "may be stale" % unindexed[ip][:80])]
        else:
            results = await push_favorites(con, ip, dry_run=dry_run)
        for key, engine, note in results:
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
        log.info("NOT ATTEMPTED on %d file(s) - a game or a sync was running. "
                 "These need the next pass; they are not 'unchanged'.",
                 report["writes"]["busy"])
    report["servers"] = status.summarize_servers(con)
    report["favorites"] = status.summarize_favorites(con)
    report["duration_sec"] = round(time.time() - t0, 1)
    report["ts"] = time.time()
    report["phase"] = "idle"
    report["ok"] = fatal is None
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
