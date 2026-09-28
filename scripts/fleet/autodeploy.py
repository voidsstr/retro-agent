#!/usr/bin/env python3
r"""Carry every library change to each box as it comes online.

WHY THIS EXISTS - IT IS NOT AUTOMATIC, AND EVERYONE ASSUMES IT IS
-----------------------------------------------------------------
The agent's GAMESYNC startup thread runs once per boot and its gate is a bare
marker check (agent/src/gamesync.c):

    if (gs_file_exists(GS_MARKER)) {
        log_msg(LOG_GS, "already provisioned (%s present) - idle", GS_MARKER);
        return 0;
    }

There is no library comparison and no title count in that condition. Once
`gamesync.done` exists the thread idles FOREVER, so **a title staged today
never reaches a box that was provisioned yesterday.** Rainbow Six landed on
.123 only because a human issued `GAMESYNC RESET` by hand.

That is a quiet failure: the library grows, `validate-staged-library.py` says
DEPLOYABLE, the boxes look healthy, and the new games are simply not there.

WHAT THIS DOES
--------------
Finds every box answering the protocol, and when EITHER

  * the library's TITLE SET has changed since that box was last synced, or
  * the library's DEPLOY GENERATION differs from the one the box last took,

issues `GAMESYNC RESET` + `GAMESYNC START` and waits for the run to finish.
A run that does not meet the post-condition (below) is not recorded, so the
box is offered the library again later.

THE DEPLOY GENERATION - HOW A FIX *INSIDE* A TITLE REACHES THE FLEET
--------------------------------------------------------------------
Keying on the title set alone carries NEW titles and nothing else. A fix made
inside an existing title changes no title name. The new icons for Serious
Sam, Warcraft I/II, Carmageddon 1 and Descent have been on the share since
2026-09-28, and they never reach a box that was switched off when they were
pushed by hand. Nothing says so. CLAUDE.md's staged-game loop (step 6)
requires every box to get every fix.

Working out "a file inside a title changed" from here would mean walking
~30 GB over CIFS on every pass, or hashing it. It would also fire on work in
progress, while several sessions edit Games-Library at once. So the signal
is EXPLICIT: a one-line file in the library root.

    \\192.168.1.122\files\Files\Games-Library\_deploy_generation.txt
    (read here as /mnt/retro-share/Files/Games-Library/_deploy_generation.txt)

Its first line that is neither blank nor a `#` comment IS the generation. It
can be any string, and it is compared for EQUALITY, not order. A box whose
recorded generation differs gets RESET + START once.

  * PUSHING A STAGED FIX TO THE WHOLE FLEET = change that line, once the fix
    is verified on one box (the loop's steps 1-5). Every box that is online
    takes it on the next pass. Every box that is offline takes it the moment
    it next answers. That covers step 6, including the boxes nobody
    remembered.
  * ABSENT FILE = generation "0". A record written before generations existed
    also reads as "0", so deploying this code changes nothing until someone
    writes the file.
  * PRESENT BUT DECLARING NOTHING (empty, or only comments) = "0" as well,
    and the log SAYS so. A gvfs write that dropped its bytes looks exactly
    like this (CLAUDE.md "WRITING TO THE SHARE"), and a bump that silently
    reads as "0" would push nothing.
  * UNREADABLE (the share mount is down) = NO PASS. "I could not read it" is
    not "0", or one flaky mount would look like a generation change.

The `_` prefix keeps it out of everything that walks titles. The agent
enumerates only directories and skips `_` entries (gs_run), and
validate-staged-library.py walks only title directories.

WHY IT KEYS ON THE LIBRARY AND NOT ON "WHAT IS MISSING FROM THE BOX"
-------------------------------------------------------------------
The obvious design - compare C:\Games against the library and sync if anything
is missing - LOOPS FOREVER. The capability gate legitimately refuses titles per
box: .143's CPU lacks SSE so Halo is refused there, .133 lacks SSE2, .240 is
refused Halo 2 on free disk space. Those titles are missing by DESIGN and will
never appear, so "missing" is not a signal that work is needed. Keying on a
change in the library's own title set asks the right question - "is there
anything here the box has not been offered yet?" - and settles. The generation
keeps that property: it changes only when a person changes it.

FINDING THE BOXES - THE HARDCODED IP LIST WAS ALREADY WRONG
----------------------------------------------------------
This tool used to carry a list of nine addresses. On 2026-09-28 that list
named 192.168.1.246, although DHCP had moved the Windows 7 box ADMIN-PC to
.195. It also lacked .110, the Dell imaged that week. A box that moves is
never synced again and a new box never at all, and neither case is reported.
The fleet moves cards between boxes and boxes between addresses, so the
address list is now DISCOVERED.

  * On the first pass, and then every `--discover-every` passes, it sweeps
    192.168.1.1-254:9898, the whole /24 at once. The passes in between
    re-probe only the boxes the last sweep found. `--boxes` adds addresses
    outside the sweep, and `--no-discover` limits it to exactly those.
  * ALIVE = a completed AUTH handshake AND a PONG, within 8 s. The slow
    single-threaded boxes need that long, and shorter probes miss .171. A
    TCP connect alone is not liveness: a dead Win9x agent's :9897 accepts
    sockets and never answers.
  * A refused connection is retried for an address we have reason to expect.
    The Win9x agents refuse while busy, and calling that DOWN produces a fake
    power-cycle. An address that has never answered gets one try, because
    most of a /24 is phones and printers.

STATE IS KEYED BY HOSTNAME, NOT BY ADDRESS
------------------------------------------
~/.retro-fleet/autodeploy.json holds one record per box, keyed by the box's
own name. The source is HWPROFILE's `hostname` (GetComputerNameA). The AUTH
greeting carries the same name, which covers an agent too old for HWPROFILE,
and the address is only the last resort. An address says where a box is
today. A name says which box it is.

The records the address-keyed version wrote are MIGRATED the first time their
box is seen. The box takes the record for the address it answers at, or, if
DHCP has moved it, the record for the address fleet-roster.txt lists for its
name. The roster is a VETO as well as a hint: a record whose address the
roster assigns to a DIFFERENT box stays where it is. Inheriting another
machine's "already synced" would mean this box silently misses a deploy,
while refusing costs one incremental sync. (Measured 2026-09-28: .124 now
answers as NSC-C543575F526, but the roster lists NSC-CABE14B7486 there. The
box was re-imaged, and it gets a fresh sync, not the old install's record.)
A legacy record has generation "0", so the first generation bump re-syncs
every migrated box whatever the migration decided.

If two live boxes answer with the same name, both are keyed name@address
while the clash lasts, and the log says so. One shared record would let one
box's sync stand in for the other's.

A MODERN WINDOWS BOX IS NOT MANAGED
-----------------------------------
WHITEBEAST (.249, Windows 11) runs the agent so the fleet can reach it, and
it answers the sweep like any fleet box. The agent refuses GAMESYNC there,
but a host tool that writes to fleet boxes must not rely on that. CLAUDE.md
("A Modern Windows Box Is NOT Managed") requires the tool itself to skip
HWPROFILE `host_policy.managed == false`, and this tool does so BEFORE any
GAMESYNC command. An agent too old to report host_policy that greets Win6.2+
counts as modern. 6.2 is the GetVersionEx shim value on every Windows
8.1/10/11 box, and the fleet has no Windows 8. On such a box, no HWPROFILE
answer is not permission to write. This follows the same rule as
scripts/gameindex/sync.py:unmanaged_modern_host, and a test pins the two
together.

WHAT A RUN MUST SHOW BEFORE IT IS RECORDED
------------------------------------------
`state` alone hides partial failures. A run is recorded only when it meets
all of these:

  * state == "done" AND failed_files == 0. gs_write_marker is skipped when
    failed_files != 0, and an aborted run reports state "failed".
  * titles_total == the number of titles this pass listed in the library,
    whenever the box syncs from this library. `titles_total` is what the BOX
    enumerated, so a truncated enumeration (the Win9x gs_dir_size bug that
    left .243 with 25 of 46 titles) otherwise reports 25 == 25 and passes.
  * done + gated + skipped == total.
  * it is OUR run. GAMESYNC START returns before the worker sets "sizing":
    the worker first sweeps the desktop and stages wallpapers. So for a while
    after START, STATUS still shows the PREVIOUS run's "done", with that
    run's counters. A finished state is accepted only after this tool has
    watched the run go busy, or when `elapsed_s` fits inside the time since
    we sent START. A stale "done" is older than that by at least the whole
    previous run. The old code took the first "done" it saw after 20 s.

A run that fails is retried after a back-off, 30 min doubling to 8 h. Without
it, a permanently failing file would mean RESET + START every 90 s.

SAFETY
------
* A box mid-GAMESYNC (sizing/copying) is left alone. "failed" and "skipped"
  are FINISHED states, not busy ones: GAMESYNC STATUS reports idle, sizing,
  copying, done, failed or skipped. The old code waited for "error", a state
  the agent never reports, and counted "failed" as busy. So one aborted run
  took a box out of the rotation until its agent next started, while the log
  promised "it will be retried next pass".
* A box whose GAMESYNC STATUS cannot be read is left alone that pass. Not
  knowing whether it is busy is not permission to RESET it.
* Every connection ends with a FIN and a drain. That includes a handshake
  that failed halfway, because an abrupt close crashes Win98's Winsock.
* An error from one box ends that box's turn, not the service.
* NEVER reboots anything. GAMESYNC is a file copy plus a registry merge.
* --dry-run sends only PING, HWPROFILE and GAMESYNC STATUS, prints what a
  real pass would do, and writes NOTHING. That includes the state file, so
  neither a migration nor a moved address is saved.
* --status prints the records and the current generation without touching
  the network.
"""
import argparse
import asyncio
import copy
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)

from client.retro_protocol import RetroConnection  # noqa: E402

STATE = os.path.expanduser("~/.retro-fleet/autodeploy.json")
LIBRARY = "/mnt/retro-share/Files/Games-Library"
# What the agents call the same directory (GAMESYNC LIBRARY answers this).
LIBRARY_UNC = r"\\192.168.1.122\files\Files\Games-Library"
GENERATION_FILE = "_deploy_generation.txt"
ROSTER = os.path.join(HERE, "fleet-roster.txt")
SECRET = "retro-agent-secret"
PORT = 9898
SUBNET = "192.168.1"
PROBE_TIMEOUT = 8.0             # .171 is missed by anything shorter
SWEEP_CONCURRENCY = 256         # the whole /24 at once
DISCOVER_EVERY = 4              # full sweep every N passes (~6 min at 90 s)
INTERVAL = 60.0
REFUSAL_RETRIES = 3
REFUSAL_BACKOFF = 4.0
IDENT_TTL = 3600.0              # re-ask HWPROFILE (name + host policy) hourly
FAIL_BACKOFF = 1800.0           # first retry after a failed run...
FAIL_BACKOFF_MAX = 8 * 3600.0   # ...doubling up to this
START_GRACE = 600.0             # START -> "sizing" can take this long on .243
CLOCK_SLACK = 5.0               # seconds; elapsed_s is whole seconds
POLL = 20.0
MAX_POLLS = 360                 # 2 h: a full library sync is slow over SMB1

BUSY = ("sizing", "copying")
FINISHED = ("done", "failed", "skipped")
AT_REST = ("idle",) + FINISHED

_IP = re.compile(r"^\d{1,3}(?:\.\d{1,3}){3}$")
_WIN = re.compile(r"^Win(\d+)\.(\d+)$")


def log(msg):
    print("%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg), flush=True)


def is_ip(s):
    return bool(_IP.match(s or ""))


def _ip_order(ip):
    try:
        return (0, tuple(int(p) for p in ip.split(".")))
    except ValueError:
        return (1, ip)


# --- the library -------------------------------------------------------------

def library_titles(library=None):
    """The staged titles, by directory name. `_`-prefixed dirs are not titles."""
    library = library or LIBRARY
    try:
        return sorted(d for d in os.listdir(library)
                      if not d.startswith("_")
                      and os.path.isdir(os.path.join(library, d)))
    except OSError:
        return []


def read_generation(library=None):
    """(generation, how) - generation is None when it could not be read.

    The name is matched case-insensitively: this is a Windows share, and
    whoever writes the file from a Windows box may well write
    _DEPLOY_GENERATION.TXT.
    """
    library = library or LIBRARY
    try:
        names = os.listdir(library)
    except OSError as e:
        return None, "library unreadable: %s" % e
    hit = [n for n in names if n.lower() == GENERATION_FILE.lower()]
    if not hit:
        return "0", "no %s - generation 0" % GENERATION_FILE
    try:
        with open(os.path.join(library, hit[0]), encoding="utf-8-sig",
                  errors="replace") as f:
            text = f.read(4096)
    except OSError as e:
        return None, "%s present but unreadable: %s" % (hit[0], e)
    for line in text.splitlines():
        s = line.strip()
        if s and not s.startswith("#"):
            return s[:200], "from %s" % hit[0]
    return "0", ("WARNING: %s is present but declares nothing (empty or only "
                 "comments) - read as generation 0; if a bump was written, it "
                 "did not land" % hit[0])


def load_roster(path=None):
    """{ip: HOSTNAME} fleet-roster.txt expects. Used ONLY to migrate records."""
    out = {}
    try:
        with open(path or ROSTER, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and not line.lstrip().startswith("#") \
                        and is_ip(parts[0]):
                    out[parts[0]] = parts[1].upper()
    except OSError:
        pass
    return out


# --- state -------------------------------------------------------------------

def load_state(path=None):
    try:
        with open(path or STATE, encoding="utf-8") as f:
            st = json.load(f)
        return st if isinstance(st, dict) else {}
    except (OSError, ValueError):
        return {}


def save_state(st, path=None):
    path = path or STATE
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(st, f, indent=1, sort_keys=True)
    os.replace(tmp, path)           # atomic: a torn state file re-syncs the fleet


def sync_reason(rec, titles, generation):
    """Why this box needs a sync, or None when it has been offered all of it.

    A record from before generations existed has none, and reads as "0" -
    the value an absent generation file also reads as - so nothing changes
    until somebody writes the file.
    """
    if not isinstance(rec, dict) or rec.get("titles") is None:
        return "never synced by autodeploy"
    why = []
    had = rec.get("titles") or []
    if had != titles:
        new = [t for t in titles if t not in had]
        gone = [t for t in had if t not in titles]
        bits = []
        if new:
            bits.append("%d new title(s) - %s" % (len(new), ", ".join(new[:6])))
        if gone:
            bits.append("%d title(s) gone from the library" % len(gone))
        why.append("; ".join(bits) or "title set changed")
    was = str(rec.get("generation", "0"))
    if was != generation:
        why.append("deploy generation %r -> %r" % (was, generation))
    return "; ".join(why) or None


def migrate_legacy(st, host, ip, roster):
    """Adopt a record the IP-keyed version wrote for this box. Returns its key.

    Candidates, in order: the record for the address the box answers at now,
    then the record for the address the roster lists for this name (a box
    DHCP has moved). A record whose address the roster gives to a DIFFERENT
    box is never adopted - see the module docstring.
    """
    if not host or is_ip(host) or host in st:
        return None
    cands = []
    if is_ip(ip) and roster.get(ip, host) == host:
        cands.append(ip)
    cands += sorted((r for r, h in roster.items() if h == host and r != ip),
                    key=_ip_order)
    for legacy in cands:
        rec = st.get(legacy)
        if isinstance(rec, dict) and rec.get("hostname") in (None, host):
            rec = st.pop(legacy)
            rec.update(hostname=host, ip=ip, migrated_from=legacy)
            st[host] = rec
            return legacy
    return None


# --- policy ------------------------------------------------------------------

def parse_greeting(greeting):
    """("HOSTNAME", (major, minor) or None) from "OK <host> Win5.1"."""
    parts = (greeting or "").split()
    host = parts[1].upper() if len(parts) >= 2 else ""
    m = _WIN.match(parts[2]) if len(parts) >= 3 else None
    return host, ((int(m.group(1)), int(m.group(2))) if m else None)


def unmanaged_reason(winver, profile):
    """Why this box must not be synced, or None. `profile` is HWPROFILE ({} = none).

    The agent's own answer wins. Without one, a Win6.2+ greeting is modern:
    that is the GetVersionEx shim value on every Windows 8.1/10/11 box and the
    fleet has no Windows 8.
    """
    hp = profile.get("host_policy") if isinstance(profile, dict) else None
    if isinstance(hp, dict) and "managed" in hp:
        return None if hp.get("managed") else \
            "modern Windows host - its agent reports host_policy.managed=false"
    if winver is not None and winver >= (6, 2):
        if profile:
            return ("Win%d.%d greeting on an agent too old to report "
                    "host_policy - treated as a modern host" % winver)
        return ("Win%d.%d greeting and no HWPROFILE answer - not permission "
                "to write" % winver)
    return None


# --- judging a run -----------------------------------------------------------

def assess(status, run, now=None):
    """Where the run we started stands: running | finished | starting | lost.

    `run` is {"t0": monotonic time START was sent, "saw": watched it busy}.
    """
    now = time.monotonic() if now is None else now
    state = (status or {}).get("state")
    if state in BUSY:
        run["saw"] = True
        return "running"
    since = now - run["t0"]
    if state in FINISHED:
        el = status.get("elapsed_s")
        if run.get("saw") or (isinstance(el, int) and el <= since + CLOCK_SLACK):
            return "finished"
    if not run.get("saw") and since < START_GRACE:
        return "starting"           # the previous run's status, still showing
    return "lost"


def _same_library(unc):
    def norm(s):
        return s.replace("/", "\\").rstrip("\\").lower()
    return not unc or norm(unc) == norm(LIBRARY_UNC)


def judge(res, library_count, box_library=None):
    """(ok, why) - the post-condition a finished run must meet to be recorded."""
    if not res:
        return False, "unreachable"
    if res.get("state") != "done":
        return False, "state=%s" % res.get("state")
    ff = res.get("failed_files", 0)
    if ff != 0:
        return False, "failed_files=%s (%s)" % (ff, res.get("failed_file", ""))
    total = res.get("titles_total")
    if isinstance(total, int):
        if library_count and _same_library(box_library) and total != library_count:
            return False, ("the box enumerated %d title(s) and the library holds "
                           "%d - a truncated enumeration is not a sync"
                           % (total, library_count))
        parts = [res.get(k) for k in ("titles_done", "titles_gated",
                                      "titles_skipped")]
        if all(isinstance(p, int) for p in parts) and sum(parts) != total:
            return False, ("%d of %d title(s) accounted for (done+gated+skipped)"
                           % (sum(parts), total))
    return True, ("done: %s/%s title(s), %s gated, %s skipped (no room), "
                  "0 file error(s), %s file(s) written"
                  % (res.get("titles_done"), total, res.get("titles_gated"),
                     res.get("titles_skipped"), res.get("files_written")))


# --- the network -------------------------------------------------------------

async def _close(c):
    try:
        await c.close()             # graceful: FIN + drain; an RST crashes Win98
    except Exception:
        pass


async def _connect(ip, timeout, retries=REFUSAL_RETRIES):
    """(connection, greeting), or (None, None) when no handshake completed."""
    refused = 0
    while True:
        c = RetroConnection(ip, PORT)
        try:
            g = await c.connect(SECRET, timeout=timeout)
            return c, g
        except ConnectionRefusedError:
            # contention, not death - the single-threaded agents refuse while busy
            refused += 1
            if refused > retries:
                return None, None
            await asyncio.sleep(REFUSAL_BACKOFF)
        except Exception:
            await _close(c)         # a handshake that died halfway still leaves with a FIN
            return None, None


async def handshake(ip, timeout=PROBE_TIMEOUT, retries=REFUSAL_RETRIES):
    """The greeting when the agent completes AUTH *and* answers PING, else None."""
    c, g = await _connect(ip, timeout, retries)
    if c is None:
        return None
    try:
        r = await c.command_text("PING", timeout=timeout)
        return g if "PONG" in r.upper() else None   # accepts sockets, answers nothing
    except Exception:
        return None
    finally:
        await _close(c)


async def sweep(ips, expected=(), timeout=PROBE_TIMEOUT):
    """{ip: greeting} for every address that is really an agent."""
    sem = asyncio.Semaphore(SWEEP_CONCURRENCY)
    expected = set(expected)

    async def one(ip):
        async with sem:
            return ip, await handshake(
                ip, timeout, REFUSAL_RETRIES if ip in expected else 0)

    res = await asyncio.gather(*(one(ip) for ip in ips))
    return {ip: g for ip, g in res if g}


async def identify(ip, timeout=PROBE_TIMEOUT):
    """HWPROFILE as a dict; {} if the agent did not answer it; None if unreachable."""
    c, _ = await _connect(ip, timeout)
    if c is None:
        return None
    try:
        prof = json.loads(await c.command_text("HWPROFILE", timeout=60))
        return prof if isinstance(prof, dict) else {}
    except Exception:
        return {}
    finally:
        await _close(c)


async def gamesync_status(ip, timeout=PROBE_TIMEOUT):
    """(reachable, status dict or None, what went wrong)."""
    c, _ = await _connect(ip, timeout)
    if c is None:
        return False, None, "unreachable"
    try:
        raw = await c.command_text("GAMESYNC STATUS", timeout=25)
    except Exception as e:
        return True, None, "GAMESYNC STATUS failed: %s" % e
    finally:
        await _close(c)
    try:
        st = json.loads(raw)
    except ValueError:
        return True, None, "GAMESYNC STATUS unparsable: %r" % raw[:80]
    if not isinstance(st, dict):
        return True, None, "GAMESYNC STATUS unparsable: %r" % raw[:80]
    return True, st, ""


async def start_run(ip, timeout=20.0):
    """RESET + START. Returns the run handle, or {"error": ...}."""
    c, _ = await _connect(ip, timeout)
    if c is None:
        return {"error": "unreachable"}
    try:
        lib = await c.command_text("GAMESYNC LIBRARY", timeout=30)
        # an agent that predates LIBRARY answers it with the STATUS json
        lib = "" if lib.lstrip().startswith("{") else lib.strip()
        await c.command_text("GAMESYNC RESET", timeout=30)
        t0 = time.monotonic()
        reply = await c.command_text("GAMESYNC START", timeout=40)
    except Exception as e:
        return {"error": "%s: %s" % (type(e).__name__, e)}
    finally:
        await _close(c)
    if "started" not in reply.lower():
        # "OK already running": a run we did not start - not ours to record
        return {"error": "GAMESYNC START answered %r" % reply.strip()}
    return {"t0": t0, "saw": False, "library": lib}


async def sync(ip, timeout=20.0, wait=True):
    """Start a run and (by default) wait for it. Returns (status, run)."""
    run = await start_run(ip, timeout)
    if "error" in run or not wait:
        return None, run
    for _ in range(MAX_POLLS):
        await asyncio.sleep(POLL)
        alive, st, _ = await gamesync_status(ip, timeout)
        if not st:
            continue                # refused or slow - keep polling
        where = assess(st, run)
        if where == "finished":
            return st, run
        if where == "lost":
            return {"state": st.get("state"), "lost": True,
                    "note": "the run we started was never seen finishing - "
                            "did the agent restart mid-sync?"}, run
    return {"state": "timeout"}, run


# --- a pass ------------------------------------------------------------------

class Watch:
    """What one process remembers between passes (nothing here is persisted)."""

    def __init__(self, roster=None):
        self.roster = roster if roster is not None else load_roster()
        self.passes = 0
        self.online = {}            # ip -> greeting hostname, from the last probe
        self.idents = {}            # ip -> {"greeting_host", "hostname", "why", "at"}
        self.pending = {}           # key -> run handle (--no-wait)
        self.fails = {}             # key -> (failures in a row, retry monotonic, retry clock)
        self.said = {}              # key -> last repeated message printed
        self.last_gen = None

    def say(self, key, msg):
        """Print a CONDITION once, not every 90 s."""
        if self.said.get(key) != msg:
            self.said[key] = msg
            log(msg)

    async def identity(self, ip, ghost, winver):
        now = time.monotonic()
        got = self.idents.get(ip)
        if got and got["greeting_host"] == ghost and now - got["at"] < IDENT_TTL:
            return got
        prof = await identify(ip)
        if prof is None:
            return None
        got = {"greeting_host": ghost,
               "hostname": str(prof.get("hostname") or "").strip().upper() or ghost,
               "why": unmanaged_reason(winver, prof),
               "agent": prof.get("agent_version", ""),
               "at": now}
        self.idents[ip] = got
        return got


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Carry library changes (new titles, and fixes inside "
                    "titles via %s) to every fleet box as it comes online."
                    % GENERATION_FILE)
    ap.add_argument("--boxes", default="",
                    help="extra addresses to probe every pass (comma-separated)")
    ap.add_argument("--subnet", default=SUBNET,
                    help="the /24 to sweep for agents (default %(default)s)")
    ap.add_argument("--no-discover", action="store_true",
                    help="probe only --boxes; do not sweep the subnet")
    ap.add_argument("--discover-every", type=int, default=DISCOVER_EVERY,
                    help="full subnet sweep every N passes (default %(default)s)")
    ap.add_argument("--interval", type=float, default=INTERVAL)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--no-wait", action="store_true",
                    help="start GAMESYNC and move on; the run is judged on a "
                         "later pass instead of waited for")
    ap.add_argument("--dry-run", action="store_true",
                    help="read-only: PING, HWPROFILE and GAMESYNC STATUS; "
                         "writes nothing, not even the state file")
    ap.add_argument("--state", default=STATE)
    ap.add_argument("--status", action="store_true",
                    help="print the records and the current generation, then exit")
    a = ap.parse_args(argv)
    a.boxes = [b.strip() for b in a.boxes.split(",") if b.strip()]
    a.discover_every = max(1, a.discover_every)
    if a.no_discover and not a.boxes:
        ap.error("--no-discover needs --boxes")
    return a


def _record(a, st, key, host, ip, titles, generation, res):
    st[key] = {"hostname": host, "ip": ip,
               "titles": titles, "generation": generation,
               "at": time.strftime("%Y-%m-%d %H:%M:%S"),
               "files_written": res.get("files_written")}
    if not a.dry_run:
        save_state(st, a.state)


def _failed(w, key, why):
    n = w.fails.get(key, (0, 0, ""))[0] + 1
    wait = min(FAIL_BACKOFF * (2 ** (n - 1)), FAIL_BACKOFF_MAX)
    when = time.strftime("%Y-%m-%d %H:%M", time.localtime(time.time() + wait))
    w.fails[key] = (n, time.monotonic() + wait, when)
    log("    NOT recorded (%s) - failure %d in a row, retry after %s"
        % (why, n, when))


async def handle_box(a, w, ip, greeting, titles, generation, st, dupes):
    ghost, winver = parse_greeting(greeting)
    if winver is None:
        w.say(ip, "  %s: %r is not a Windows agent - GAMESYNC is Windows-only"
              % (ip, greeting))
        return
    ident = await w.identity(ip, ghost, winver)
    if ident is None:
        return                      # went away between the sweep and now
    host = ident["hostname"]
    if ident["why"]:                # before ANY GAMESYNC command
        w.say(host or ip, "  %s (%s) skipped: %s" % (host or ip, ip, ident["why"]))
        return
    key = host or ip
    if host in dupes:
        key = "%s@%s" % (host, ip)
    elif host and host not in st:
        frm = migrate_legacy(st, host, ip, w.roster)
        if frm:
            log("  %s (%s): adopted the record the IP-keyed version kept "
                "under %s%s" % (host, ip, frm,
                                " (dry run - not saved)" if a.dry_run else ""))
            if not a.dry_run:
                save_state(st, a.state)
        elif isinstance(st.get(ip), dict) and w.roster.get(ip) not in (None, host):
            w.say("veto:" + ip, "  %s (%s): the IP-keyed record for %s belongs to "
                  "%s per fleet-roster.txt - not adopted; this box is synced "
                  "afresh" % (host, ip, ip, w.roster[ip]))
    rec = st.get(key)
    if isinstance(rec, dict) and rec.get("ip") != ip:
        if rec.get("ip"):
            log("  %s moved: %s -> %s" % (key, rec.get("ip"), ip))
        rec["ip"] = ip
        if not a.dry_run:
            save_state(st, a.state)

    run = w.pending.get(key)
    if run:                         # --no-wait: judge the run we started earlier
        alive, gs, err = await gamesync_status(ip)
        if not gs:
            return
        where = assess(gs, run)
        if where in ("running", "starting"):
            return
        del w.pending[key]
        if where == "finished":
            ok, why = judge(gs, len(run["titles"]), run.get("library"))
        else:
            ok, why = False, ("the run we started was never seen finishing "
                              "(state now %s)" % gs.get("state"))
        log("  %s (%s): the run started earlier ended - %s" % (key, ip, why))
        if ok:
            w.fails.pop(key, None)
            _record(a, st, key, host, ip, run["titles"], run["generation"], gs)
        else:
            _failed(w, key, why)
        return

    reason = sync_reason(rec, titles, generation)
    if not reason:
        return                      # this box has been offered this library
    n, retry_at, when = w.fails.get(key, (0, 0, ""))
    if n and time.monotonic() < retry_at:
        w.say(key, "  %s (%s) needs a sync; its last run failed - not "
                   "retrying before %s" % (key, ip, when))
        return
    alive, gs, err = await gamesync_status(ip)
    if not alive:
        return
    state = gs.get("state") if gs else None
    if state in BUSY:
        log("  %s (%s) busy (%s) - leaving it alone" % (key, ip, state))
        return
    if state not in AT_REST:
        w.say(key, "  %s (%s) needs a sync, but its GAMESYNC state is unknown "
                   "(%s) - not touching it" % (key, ip, err or state))
        return
    log("  %s (%s) needs a sync: %s" % (key, ip, reason))
    if a.dry_run:
        log("    dry run - not syncing")
        return
    w.said.pop(key, None)
    try:
        res, run = await sync(ip, wait=not a.no_wait)
    except Exception as e:          # one box's trouble must not end the service
        res, run = None, {"error": "%s: %s" % (type(e).__name__, e)}
    if "error" in run:
        _failed(w, key, run["error"])
        return
    if a.no_wait:
        run.update(titles=titles, generation=generation)
        w.pending[key] = run
        log("    started - it will be judged on a later pass")
        return
    ok, why = judge(res, len(titles), run.get("library"))
    log("    -> %s" % json.dumps(res or {"state": "unreachable"}))
    if ok:
        w.fails.pop(key, None)
        _record(a, st, key, host, ip, titles, generation, res)
        log("    recorded: %s" % why)
    else:
        _failed(w, key, why)


async def pass_once(a, w, titles, generation, st):
    w.passes += 1
    full = not a.no_discover and (w.passes == 1 or
                                  (w.passes - 1) % a.discover_every == 0)
    # Between full sweeps only the boxes that answered last time are asked;
    # a box that has just been switched on is found by the next full sweep.
    targets = set(a.boxes)
    if full:
        targets |= {"%s.%d" % (a.subnet, i) for i in range(1, 255)}
    elif not a.no_discover:
        targets |= set(w.online)
    # a refusal is retried only where an agent is expected (see the docstring)
    expected = set(a.boxes) | set(w.online) | {
        r.get("ip") for r in st.values() if isinstance(r, dict) and r.get("ip")
    } | {k for k in st if is_ip(k)}
    live = await sweep(sorted(targets, key=_ip_order), expected)

    now = {ip: parse_greeting(g)[0] for ip, g in live.items()}
    for ip in sorted(set(now) - set(w.online), key=_ip_order):
        log("+ %s answers as %s (%s)" % (ip, now[ip] or "?", live[ip]))
    for ip in sorted(set(w.online) - set(now), key=_ip_order):
        log("- %s (%s) no longer answers" % (ip, w.online[ip] or "?"))
    w.online = now

    names = {}
    for ip, h in now.items():
        if h:
            names.setdefault(h, []).append(ip)
    dupes = {h for h, ips in names.items() if len(ips) > 1}
    for h in sorted(dupes):
        w.say("dupe:" + h, "  WARNING: %s answers at %s - keyed per address "
              "until only one box has that name" % (h, ", ".join(sorted(names[h]))))

    for ip in sorted(live, key=_ip_order):
        try:
            await handle_box(a, w, ip, live[ip], titles, generation, st, dupes)
        except Exception as e:      # noqa: BLE001 - a bad box must not end the pass
            log("  %s: %s: %s - skipped this pass" % (ip, type(e).__name__, e))


def print_status(a):
    st = load_state(a.state)
    titles = library_titles()
    gen, how = read_generation()
    print("library: %d title(s); deploy generation %r (%s)"
          % (len(titles), gen, how))
    print("%-22s %-15s %-12s %6s  %s" % ("box", "last address", "generation",
                                          "titles", "synced at"))
    for key in sorted(st, key=lambda k: (is_ip(k), k)):
        r = st[key] if isinstance(st[key], dict) else {}
        g = str(r.get("generation", "0"))
        mark = ""
        if is_ip(key):
            mark = "  (address-keyed record, not yet migrated)"
        elif sync_reason(r, titles, gen or "0"):
            mark = "  <- needs a sync"
        print("%-22s %-15s %-12s %6d  %s%s" % (key, r.get("ip", key if is_ip(key) else ""),
                                              g, len(r.get("titles") or []),
                                              r.get("at", ""), mark))


async def main_async(a):
    st = load_state(a.state)
    if a.dry_run:
        st = copy.deepcopy(st)      # a dry run may migrate in memory; never on disk
    w = Watch()
    while True:
        titles = library_titles()
        generation, how = read_generation()
        if not titles or generation is None:
            w.say("library", "library unreadable at %s (%s) - waiting"
                  % (LIBRARY, how if generation is None else "no titles"))
        else:
            w.said.pop("library", None)
            if (generation, how) != w.last_gen:
                log("deploy generation %r (%s); %d title(s)"
                    % (generation, how, len(titles)))
                w.last_gen = (generation, how)
            await pass_once(a, w, titles, generation, st)
        if a.once:
            return
        await asyncio.sleep(a.interval)


def main(argv=None):
    a = parse_args(argv)
    if a.status:
        print_status(a)
        return
    asyncio.run(main_async(a))


if __name__ == "__main__":
    main()
