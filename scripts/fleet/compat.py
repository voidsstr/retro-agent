#!/usr/bin/env python3
r"""The fleet compatibility matrix - which games work on which computers.

    python3 scripts/fleet/compat.py ingest             # refresh from every source
    python3 scripts/fleet/compat.py matrix             # the per-box x per-title grid
    python3 scripts/fleet/compat.py status --box .143  # one machine
    python3 scripts/fleet/compat.py status --title Quake1
    python3 scripts/fleet/compat.py record ...         # a hand-recorded verification
    python3 scripts/fleet/compat.py conflicts          # measured vs derived disagreements
    python3 scripts/fleet/compat.py gaps               # what nobody has ever tested
    python3 scripts/fleet/compat.py export --out f.json # what the dashboard eats
    python3 scripts/fleet/compat.py doc --check         # has the LAN doc drifted?

THE CLI IS THE PRIMARY INTERFACE.  The dashboard is a consumer of `export`,
and the fleet must keep working with the dashboard down - the SQLite file is
the source of truth and no retro box ever reaches a cloud service.

WHY `ingest` CANNOT LOSE A VERIFICATION
=======================================
Every source ingested here is MACHINE-DERIVED and lands with `origin='derived'`,
except the LAN status document, whose rows were proved on real hardware by a
person and land as `origin='measured'`.  `record` also writes `measured`.  The
two origins are separate rows with `origin` in the primary key, so re-running
`ingest` any number of times can never destroy something somebody watched
happen; where they disagree, `conflicts` says so out loud.

WHY A MISSING ROW MUST RENDER AS `untested`
===========================================
The matrix is a CROSS JOIN of boxes x titles.  A cell nobody has looked at is a
row reading `untested`, never a blank and never an inherited pass.  This is the
project's standing "three states, never two" rule and it is the single thing
this database exists to protect: `state=done` from GAMESYNC is not evidence a
game runs, and a game that runs is not evidence anybody watched it render.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import compat_db as C  # noqa: E402

LIBRARY = "/mnt/retro-share/Files/Games-Library"
INVENTORY_DIR = "/mnt/retro-share/Utility/Retro Automation/fleet-inventory"
LAN_DOC = os.path.join(REPO, "docs", "lan-multiplayer-status.md")
ROSTER = os.path.join(HERE, "fleet-roster.txt")


class IngestError(RuntimeError):
    """A source that should have been there was not, or would not parse.

    Raised, never swallowed.  An ingest that silently skips a missing source
    produces a matrix that looks complete and is not - which is this project's
    signature failure and the reason every checker here reports `absent`,
    `unreadable` and `down` as three different things.
    """


# ---------------------------------------------------------------------------
# Titles that are not their own Games-Library directory but ARE their own fact.
# Half-Life's mods have genuinely different multiplayer answers - Blue Shift is
# MEASURED to have none while TFC is verified on two boxes - so each is a row
# in the matrix, carrying `parent` so its deployment follows the parent's tree.
# ---------------------------------------------------------------------------
MODS = {
    "HalfLife-TFC":          ("HalfLife1", "Half-Life: Team Fortress Classic", "goldsrc"),
    "HalfLife-OpposingForce": ("HalfLife1", "Half-Life: Opposing Force", "goldsrc"),
    "HalfLife-DMC":          ("HalfLife1", "Half-Life: Deathmatch Classic", "goldsrc"),
    "HalfLife-BlueShift":    ("HalfLife1", "Half-Life: Blue Shift", "goldsrc"),
    "HalfLife-Deathmatch":   ("HalfLife1", "Half-Life Deathmatch", "goldsrc"),
    "Quake3Arena":           ("Quake3-TeamArena", "Quake III Arena", "idtech3"),
    "YurisRevenge":          ("RedAlert2", "Yuri's Revenge", "westwood"),
}

# Titles withdrawn from the library but deliberately REMEMBERED, so nobody
# re-stages them.  "We looked and it cannot work yet" and "we never looked"
# are different facts and the schema must be able to say both.
WITHDRAWN = {
    "SeriousSamTFE": ("Serious Sam: The First Encounter",
                      "disc-locked: SeriousSam.exe walks drive letters for a "
                      "CD-ROM-typed volume; no later patch drops the check"),
    "SeriousSamTSE": ("Serious Sam: The Second Encounter",
                      "disc-locked, already retail v1.05 - the Doom 3 escape "
                      "(a later official patch without the wrapper) does not exist"),
}

# The LAN document names titles for a reader; the library names directories.
# This map is EXPLICIT rather than fuzzy on purpose: a near-match that silently
# picks the wrong tree would attribute one box's verification to another title.
DOC_ALIASES = {
    # The document uses a title's REAL name; the matrix is keyed on the
    # Games-Library DIRECTORY name. Those differ often enough that guessing is
    # not an option -- "Serious Sam: The First Encounter" against
    # SeriousSamFirstEncounter differs by punctuation AND a dropped "The", so
    # no normalisation rule short of a fuzzy match connects them, and a fuzzy
    # match here would silently mis-attribute a verification to the wrong
    # title. An unmapped title is REPORTED rather than skipped, which is what
    # surfaced this one: three two-box LAN proofs of Serious Sam sat in the
    # database while `doc --check` said the document had never heard of it.
    "serious sam: the first encounter": "SeriousSamFirstEncounter",
    "serious sam: the second encounter": "SeriousSamSecondEncounter",
    "serious sam - the first encounter": "SeriousSamFirstEncounter",
    "serious sam - the second encounter": "SeriousSamSecondEncounter",
    "half-life": "HalfLife1",
    "half-life - team fortress classic": "HalfLife-TFC",
    "half-life - opposing force": "HalfLife-OpposingForce",
    "half-life - deathmatch classic": "HalfLife-DMC",
    "half-life: blue shift": "HalfLife-BlueShift",
    "half-life deathmatch": "HalfLife-Deathmatch",
    "counter-strike 1.6": "CounterStrike16",
    "quake 1": "Quake1",
    "daggerfall": "Daggerfall",
    "quake ii": "Quake2Complete",
    "quake iii arena": "Quake3Arena",
    "quake iii: team arena": "Quake3-TeamArena",
    "hexen ii": "HexenII",
    "sin gold": "SiNGold",
    "soldier of fortune ii": "SoldierOfFortune2",
    "soldier of fortune 1": "SoldierOfFortune",
    "jedi knight: dark forces ii": "JediKnightDF2",
    "mysteries of the sith": "JediKnightMotS",
    "jedi academy": "JediAcademy",
    "unreal tournament (436 client)": "UnrealTournament436",
    "unreal tournament (469e)": "UnrealTournament",
    "ut2004": "UT2004",
    "unreal gold": "UnrealGold",
    "deus ex": "DeusEx",
    "red alert 2": "RedAlert2",
    "yuri's revenge": "YurisRevenge",
    "tiberian sun": "TiberianSun",
    "starcraft": "StarCraft",
    "descent 1": "Descent1",
    "descent 2": "Descent2",
    "descent 3": "Descent3",
    "doom 3": "Doom3",
    "return to castle wolfenstein": "ReturnToCastleWolfenstein",
    "warcraft ii: battle.net edition": "WarcraftII",
    "warcraft: orcs & humans": "WarcraftOrcsAndHumans",
    "shadow warrior classic complete": "ShadowWarrior",
    "master of orion ii": "MasterOfOrionII",
    "bf1942": "BF1942",
    "halo": "Halo",
    "carmageddon 1 / 2": "Carmageddon1|Carmageddon2",
    "hidden & dangerous": "HiddenAndDangerous",
    "aliens vs predator": "AliensVsPredator",
    "turok 2": "Turok2",
    "redneck rampage": "RedneckRampage",
    "shogo": "Shogo",
    "red faction": "RedFaction",
    "far cry": "FarCry",
    "max payne": "MaxPayne",
    "system shock 2": "SystemShock2",
}

TRANSPORT_HINTS = {
    "goldsrc": "goldsrc", "netquake": "quake-control", "id tech 2": "idtech2",
    "id tech 3": "idtech3", "id tech 4": "idtech4", "ue1": "ue1", "ue2": "ue2",
    "directplay": "directplay", "ipx": "ipx-tunnel", "udp/ip": "udp-native",
    "westwood": "westwood-peer", "quake-derived": "quake-control",
    "sith": "directplay", "udp lan": "udp-native",
}


def _norm_title(s):
    s = s.strip().strip("*`").replace("\u2014", "-").replace("\u2013", "-")
    s = re.sub(r"\s+", " ", s).strip().lower()
    return s


def _boxes_from(text):
    """Pull `.143` style box references out of a doc cell -> full IPs."""
    return ["192.168.1." + m for m in re.findall(r"\.(\d{2,3})\b", text)]


def _short(ip):
    return ip.replace("192.168.1.", ".")


def _full(ip):
    ip = ip.strip()
    if ip.startswith("."):
        return "192.168.1." + ip.lstrip(".")
    if re.fullmatch(r"\d{1,3}", ip):
        return "192.168.1." + ip
    return ip


# ===========================================================================
# INGEST
# ===========================================================================

def ingest_roster_and_inventory(con, strict=True):
    """The boxes.  Roster says which SHOULD exist; the published hardware
    records say what they are.  A rostered box with no record is `never seen`,
    which is a different fact from a box that does not exist."""
    if not os.path.exists(ROSTER):
        raise IngestError("roster missing: %s" % ROSTER)
    rostered = {}
    for line in open(ROSTER):
        line = line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        parts = re.split(r"\t+|  +", line.strip(), maxsplit=2)
        if len(parts) >= 2:
            rostered[parts[0]] = (parts[1], parts[2] if len(parts) > 2 else "")

    for ip, (host, note) in rostered.items():
        C.put_box(con, ip, hostname=host, note=note, state="never seen",
                  source="fleet-roster")

    n = 0
    if not os.path.isdir(INVENTORY_DIR):
        msg = "hardware records unreadable: %s" % INVENTORY_DIR
        if strict:
            raise IngestError(msg + " (is /mnt/retro-share mounted?)")
        C.log_ingest(con, "fleet-inventory", False, detail=msg)
        return len(rostered), 0

    for fn in sorted(os.listdir(INVENTORY_DIR)):
        if not fn.endswith(".json"):
            continue
        path = os.path.join(INVENTORY_DIR, fn)
        try:
            d = json.load(open(path))
        except Exception as e:                       # a torn or truncated write
            C.log_ingest(con, "fleet-inventory", False,
                         detail="unreadable %s: %s" % (fn, e))
            continue
        ips = []
        for iface in d.get("network", {}).get("interfaces", []):
            ips += iface.get("ipv4", [])
        ip = next((i for i in ips if i.startswith("192.168.1.")), None)
        if not ip:
            continue
        gpu = d.get("gpu", {})
        disp = d.get("display", {})
        accel = ", ".join(a.get("description", "") for a in d.get("accelerators", []))
        C.put_box(
            con, ip, hostname=d.get("hostname", ""),
            profile_hash=d.get("profile_hash", ""),
            os=d.get("os", {}).get("product", ""),
            cpu=d.get("cpu", {}).get("brand", ""),
            cpu_mhz=d.get("cpu", {}).get("mhz"),
            ram_mb=d.get("ram_mb"),
            gpu="%s (%s MB)" % (gpu.get("name", ""), gpu.get("vram_mb", "?")),
            gpu_class=gpu.get("feature_level", ""),
            accelerators=accel or "none",
            display_mode="%sx%s" % (disp.get("width"), disp.get("height")),
            agent_version=d.get("agent_version", ""),
            state="current", measured_at=d.get("reported_at", ""),
            source="fleet-inventory")
        n += 1
    C.log_ingest(con, "fleet-inventory", True, rows_in=len(rostered), rows_written=n)
    return len(rostered), n


def ingest_library(con, strict=True):
    """The titles.  The Games-Library directory IS the list of staged games;
    anything else would be a second hand-maintained inventory, which is the
    exact mistake ONBOARD was removed for."""
    if not os.path.isdir(LIBRARY):
        msg = "staged library not readable: %s" % LIBRARY
        if strict:
            raise IngestError(msg + " (is /mnt/retro-share mounted?)")
        C.log_ingest(con, "library", False, detail=msg)
        return 0
    n = 0
    for d in sorted(os.listdir(LIBRARY)):
        if d.startswith("_") or not os.path.isdir(os.path.join(LIBRARY, d)):
            continue
        engine = ""
        rj = os.path.join(LIBRARY, d, "requires.json")
        disp = d
        if os.path.exists(rj):
            try:
                r = json.load(open(rj))
                disp = r.get("title", d)
            except Exception:
                pass
        C.put_title(con, d, display_name=disp, engine=engine, kind="title",
                    in_library=1)
        n += 1
    for slug, (parent, disp, engine) in MODS.items():
        C.put_title(con, slug, display_name=disp, parent=parent, kind="mod",
                    engine=engine, in_library=1)
        n += 1
    for slug, (disp, why) in WITHDRAWN.items():
        C.put_title(con, slug, display_name=disp, kind="title", in_library=0,
                    note=why)
        n += 1
    C.log_ingest(con, "library", True, rows_in=n, rows_written=n)
    return n


def ingest_gamegate(con, strict=True):
    """The capability gate's verdicts - `run`/`marginal`/`no` per profile_hash.

    THE GATE VERDICT IS NOT THE SAME FACT AS DEPLOYMENT, and this is where it
    would be easiest to lose the distinction.  `no` means the gate refused to
    copy the title, so `state='gated'` is genuinely derivable.  `run` means only
    that the gate PERMITS it - whether the files actually landed is a separate
    question answered by the box index, so a `run` verdict leaves `state`
    alone and records itself in `gate`.
    """
    if not C.GAMEGATE_DB.exists():
        msg = "gamegate cache absent: %s" % C.GAMEGATE_DB
        if strict:
            raise IngestError(msg)
        C.log_ingest(con, "gamegate.db", False, detail=msg)
        return 0
    g = sqlite3.connect("file:%s?mode=ro" % C.GAMEGATE_DB, uri=True)
    g.row_factory = sqlite3.Row
    # A box has had several profile_hashes over time (a re-measure, a swapped
    # card).  Only the LATEST hash per IP describes the machine now; verdicts
    # under a superseded hash describe hardware that box no longer has.
    latest = {}
    for r in g.execute("SELECT profile_hash, ip, seen FROM profiles "
                       "WHERE ip <> '' ORDER BY seen"):
        latest[r["ip"]] = r["profile_hash"]
    by_hash = {v: k for k, v in latest.items()}
    rows = written = 0
    for r in g.execute("SELECT * FROM verdicts WHERE shortcut='' ORDER BY created"):
        rows += 1
        ip = by_hash.get(r["profile_hash"])
        if ip is None:
            continue                      # a superseded profile - not this box
        v = r["verdict"]
        state = {"no": "gated", "marginal": "marginal"}.get(v, "untested")
        have = need = ""
        # "not enough video RAM (have 32 MB, needs 64)" -> 32 / 64.
        # Strip the trailing bracket: `needs 1000)` must not become "1000)".
        m = re.search(r"have\s+([\d.]+)[^,]*,\s*needs?\s+([\d.]+)",
                      r["reason"] or "")
        if m:
            have, need = m.group(1), m.group(2)
        C.put_deploy(con, ip, r["title"], "derived", state, gate=v,
                     reason=r["reason"] or "", limiting=r["limiting"] or "",
                     have=have, need=need, decided_by=r["decided_by"],
                     confidence=r["confidence"], source="gamegate.db",
                     measured_at=_dt.datetime.fromtimestamp(
                         r["created"]).strftime("%Y-%m-%d %H:%M:%S"))
        written += 1
    g.close()
    C.log_ingest(con, "gamegate.db", True, rows_in=rows, rows_written=written)
    return written


def ingest_installed(con, strict=True):
    r"""What the game-index sweep FOUND on each box.

    IT CAN PROVE PRESENCE AND IT CANNOT PROVE ABSENCE, and getting that wrong
    was a real bug in the first cut of this ingest.  `installed_games` is an
    ENGINE-AWARE index: `gameservers.py` walks C:\Games looking for engines it
    recognises, so it lists ThiefGold and Quake1 but has no game_key for Doom3,
    Far Cry, Halo, Turok 2, Master of Orion II, Warcraft II or Shadow Warrior.
    Deriving `absent` from "not in this table" marked Doom 3 absent on .123 -
    a box where Doom 3 is LAN-verified against .246.

    So this writes `deployed` and NOTHING ELSE.  Real absence needs a real
    directory listing, which is what `--probe` does.
    """
    if not C.GAMESERVERS_DB.exists():
        msg = "gameservers db absent: %s" % C.GAMESERVERS_DB
        if strict:
            raise IngestError(msg)
        C.log_ingest(con, "gameservers.db", False, detail=msg)
        return 0
    s = sqlite3.connect("file:%s?mode=ro" % C.GAMESERVERS_DB, uri=True)
    s.row_factory = sqlite3.Row
    known = {r["title"] for r in con.execute(
        "SELECT title FROM compat_title WHERE kind='title'")}
    seen_at = {r["ip"]: (r["indexed_at"] or r["last_seen"] or "")
               for r in s.execute("SELECT ip, indexed_at, last_seen FROM machines")}
    rows = written = 0
    for r in s.execute("SELECT ip, dir FROM installed_games"):
        rows += 1
        parts = re.split(r"[\\/]+", r["dir"].rstrip("\\/"))
        # C:\Games\DeusEx\SYSTEM -> DeusEx.  The indexer points at the exe's
        # directory, which for UE1 titles is one below the tree root.
        for cand in reversed(parts):
            match = next((t for t in known if t.lower() == cand.lower()), None)
            if match:
                prev = con.execute(
                    "SELECT gate FROM compat_deploy WHERE ip=? AND title=? "
                    "AND origin='derived'", (r["ip"], match)).fetchone()
                C.put_deploy(con, r["ip"], match, "derived", "deployed",
                             gate=prev["gate"] if prev else "",
                             source="gameservers.db",
                             measured_at=seen_at.get(r["ip"], ""),
                             reason="found by the engine index")
                written += 1
                break
    s.close()
    C.log_ingest(con, "gameservers.db", True, rows_in=rows, rows_written=written)
    return written


def ingest_probe(con, strict=True):
    r"""Ask each box what is actually in C:\Games - the only sound source of
    ABSENCE.

    Read-only (one DIRLIST per box, nothing launched, nothing rebooted).  A box
    that does not answer is left ALONE rather than marked absent: the fleet is
    powered on demand, so unreachable means `untested`, not `not installed`.
    Those are the two states this project keeps paying for confusing.
    """
    import asyncio
    sys.path.insert(0, REPO)
    try:
        from client.retro_protocol import RetroConnection
    except Exception as e:
        msg = "cannot import the agent client: %s" % e
        if strict:
            raise IngestError(msg)
        C.log_ingest(con, "probe", False, detail=msg)
        return 0
    secret = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
    boxes = [r["ip"] for r in con.execute("SELECT ip FROM compat_box ORDER BY ip")]
    known = [r["title"] for r in con.execute(
        "SELECT title FROM compat_title WHERE kind='title' AND in_library=1")]

    async def one(ip):
        """Return (ip, set_of_dir_names) or (ip, None) for "could not ask".

        THE DISTINCTION IS THE WHOLE POINT AND IT IS EASY TO LOSE. The first
        cut of this function wrapped the parse in `except Exception: continue`,
        so when DIRLIST turned out to return a BARE JSON ARRAY rather than
        `{"entries": [...]}`, every box came back with an empty set and 414
        cells were confidently reported `absent` - including titles that are
        LAN-verified on those very boxes. A parse failure now poisons the whole
        box to None (untested), because "I could not read the answer" and
        "the directory is empty" must never render the same.
        """
        # .171 answers slowly - use a generous timeout or sweeps miss it.
        try:
            c = RetroConnection(ip, 9898)
            await c.connect(secret, timeout=10.0)
        except Exception:
            return ip, None                # unreachable: NOT the same as empty
        found = set()
        ok = False
        try:
            for root in (r"C:\Games", r"D:\Games"):
                st, data = await c.send_command("DIRLIST " + root)
                if st != 0:
                    continue               # no such directory on this box
                parsed = json.loads(data.decode("ascii", "replace"))
                # The agent answers with a bare ARRAY of entries. Accept an
                # object too, but never treat an unrecognised shape as empty.
                entries = parsed if isinstance(parsed, list) \
                    else parsed.get("entries")
                if not isinstance(entries, list):
                    raise IngestError(
                        "DIRLIST %s on %s returned an unrecognised shape %r - "
                        "refusing to read it as an empty directory"
                        % (root, ip, type(parsed).__name__))
                for e in entries:
                    if e.get("is_dir"):
                        found.add(e["name"].lower())
                ok = True
        except IngestError:
            raise
        except Exception:
            return ip, None
        finally:
            await c.close()
        return ip, (found if ok else None)

    async def sweep():
        return await asyncio.gather(*(one(b) for b in boxes))

    results = asyncio.run(sweep())
    when = C.now()
    written = unreachable = 0
    for ip, found in results:
        if found is None:
            unreachable += 1
            continue
        for t in known:
            prev = con.execute(
                "SELECT * FROM compat_deploy WHERE ip=? AND title=? "
                "AND origin='derived'", (ip, t)).fetchone()
            gate = prev["gate"] if prev else ""
            if t.lower() in found:
                state = "deployed"
            elif gate == "no":
                # Not there BECAUSE the gate refused it.  `gated` names the
                # remedy and `absent` does not, so the gated reading survives.
                state = "gated"
            else:
                state = "absent"
            # CARRY THE GATE'S DIAGNOSIS FORWARD. `put_deploy` replaces the
            # whole row, so writing presence without these silently erased the
            # limiting factor and BOTH NUMBERS that the gamegate ingest had
            # just recorded - leaving "capability gate refused it" with nothing
            # to argue with. A gated verdict is only actionable while it still
            # says `cpu_mhz: have 701, needs 1000`.
            keep = dict(limiting="", have="", need="", decided_by="",
                        confidence=None, reason="")
            if prev:
                keep = dict(limiting=prev["limiting"], have=prev["have"],
                            need=prev["need"], decided_by=prev["decided_by"],
                            confidence=prev["confidence"],
                            reason=prev["reason"])
            if state == "deployed":
                keep["reason"] = (
                    "on the box, but the capability gate says this machine "
                    "cannot run it: " + (prev["reason"] if prev else "")
                    if gate == "no" else "")
            elif state == "absent":
                keep["reason"] = "not present in the box's Games directory"
            C.put_deploy(con, ip, t, "derived", state, gate=gate,
                         source="probe", measured_at=when, **keep)
            written += 1
    detail = "%d box(es) did not answer - left untested, NOT marked absent" \
             % unreachable if unreachable else ""
    if detail:
        print("note: %s" % detail, file=sys.stderr)
    C.log_ingest(con, "probe", True, rows_in=len(boxes), rows_written=written,
                 detail=detail)
    return written


def propagate_mods(con):
    """A mod's tree is its parent's tree, so its deployment is the parent's.

    Recorded as `derived` with its own source so it is never mistaken for
    somebody having looked at the mod directly.
    """
    n = 0
    for r in con.execute("SELECT title, parent FROM compat_title "
                         "WHERE parent <> ''").fetchall():
        for d in con.execute(
                "SELECT * FROM compat_deploy WHERE title=? AND origin='derived'",
                (r["parent"],)).fetchall():
            C.put_deploy(con, d["ip"], r["title"], "derived", d["state"],
                         gate=d["gate"], source="mod-of-" + r["parent"],
                         measured_at=d["measured_at"],
                         reason="ships inside the %s tree" % r["parent"])
            n += 1
    return n


def imply_runs_from_lan(con):
    """A two-box LAN proof is proof the game STARTED on both boxes.

    Not proof of how it rendered - so this writes `runs`, never `verified`, and
    marks it `derived`.  The distinction is the whole point: somebody watched
    a match happen, nobody wrote down the resolution.
    """
    n = 0
    for m in con.execute(
            "SELECT ip, title, measured_at FROM compat_mp WHERE origin='measured'"
            " AND status IN ('verified_two_box','verified_server')").fetchall():
        if con.execute("SELECT 1 FROM compat_render WHERE ip=? AND title=? "
                       "AND shortcut='' AND origin='measured'",
                       (m["ip"], m["title"])).fetchone():
            continue                       # a real observation already wins
        C.put_render(con, m["ip"], m["title"], "derived", "runs",
                     source="lan-proof-implied", measured_at=m["measured_at"],
                     detail="implied by a two-box LAN proof: the game started "
                            "on this box. Rendering was NOT characterised.")
        n += 1
    return n


def _mp_from_doc(text):
    """Parse docs/lan-multiplayer-status.md into per-box multiplayer facts.

    THE DOCUMENT IS A HAND-PROVED SOURCE, so its rows land as `measured`.  The
    parser is strict: a section header it does not recognise is reported, not
    skipped, because a silently-dropped section would remove verifications from
    the matrix while everything still looked fine.
    """
    out = []          # (title, ip, status, partner, transport, blocker, remedy)
    unmapped = []
    section = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith("#"):
            low = line.lower()
            if "verified lan" in low:
                section = "verified"
            elif "gather not yet proven" in low:
                section = "unproven"
            elif "needs a person" in low:
                section = "human"
            elif "no multiplayer" in low:
                section = "none"
            elif "peer-hosted by design" in low:
                section = "peer"
            elif "withdrawn" in low:
                section = "withdrawn"
            else:
                section = None
            continue
        if section in ("verified", "unproven", "human") and line.startswith("|"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            if len(cells) < 3 or set(cells[0]) <= set("-: ") or \
               cells[0].lower() in ("title",):
                continue
            key = _norm_title(cells[0])
            slug = DOC_ALIASES.get(key)
            if not slug:
                unmapped.append(cells[0])
                continue
            for t in slug.split("|"):
                if section == "verified":
                    transport = "unknown"
                    for hint, val in TRANSPORT_HINTS.items():
                        if hint in cells[1].lower():
                            transport = val
                            break
                    boxes = _boxes_from(cells[2])
                    if not boxes:
                        # "fleet server" - the doc does not name the CLIENT box,
                        # so no per-box row can honestly be written.  Recording
                        # it against an invented machine would be a fabricated
                        # verification; the cells stay untested on purpose.
                        out.append((t, None, "verified_server", "", transport,
                                    "", cells[2]))
                        continue
                    for i, b in enumerate(boxes):
                        partner = boxes[1 - i] if len(boxes) == 2 else ""
                        out.append((t, b, "verified_two_box", partner,
                                    transport, "", ""))
                elif section == "unproven":
                    out.append((t, None, "peer_unproven", "", "unknown",
                                cells[2], cells[1]))
                else:
                    out.append((t, None, "needs_human", "", "unknown",
                                cells[1], cells[2]))
        elif section == "none" and line.startswith("- **"):
            m = re.match(r"- \*\*(.+?)\*\*", line)
            if m:
                slug = DOC_ALIASES.get(_norm_title(m.group(1)))
                if slug:
                    out.append((slug, None, "no_multiplayer", "", "unknown",
                                line.split("—", 1)[-1].strip()[:400], ""))
                else:
                    unmapped.append(m.group(1))
    return out, unmapped


def ingest_lan_doc(con, strict=True):
    if not os.path.exists(LAN_DOC):
        msg = "LAN status doc missing: %s" % LAN_DOC
        if strict:
            raise IngestError(msg)
        C.log_ingest(con, "lan-doc", False, detail=msg)
        return 0
    facts, unmapped = _mp_from_doc(open(LAN_DOC).read())
    when = _dt.datetime.fromtimestamp(
        os.path.getmtime(LAN_DOC)).strftime("%Y-%m-%d %H:%M:%S")
    boxes = [r["ip"] for r in con.execute("SELECT ip FROM compat_box")]
    written = 0
    withdrawn = 0
    for title, ip, status, partner, transport, blocker, remedy in facts:
        if ip is None:
            # A title-level fact.  Applied to every box would be a fabrication
            # for `verified_server`; for the states that ARE title-level
            # properties (no multiplayer at all, needs one person once, peer
            # gather not done) it genuinely holds everywhere the title is.
            if status == "verified_server":
                C.put_title(con, title, mp_design="fleet-server")
                continue
            C.put_title(con, title, mp_design=status)
            for b in boxes:
                # A title-level multiplayer property applies only where the
                # title IS. Stamping "needs a person once" onto a box that does
                # not have the game reads as a pending action that nobody can
                # take, and buries the real ones.
                d = con.execute("SELECT state FROM v_compat_deploy WHERE ip=? "
                                "AND title=?", (b, title)).fetchone()
                if d is None or d["state"] not in ("deployed", "marginal"):
                    # AND WITHDRAW A ROW THIS RULE WOULD NO LONGER WRITE.
                    # The guard above only stops a wrong row being CREATED. A
                    # row written before the title was gated - or before the
                    # guard existed - is never revisited, so it survives every
                    # later ingest looking exactly like a current fact.
                    # Measured 2026-09-01: `.171` still carried Halo's old
                    # "needs a person / one System Link join" long after the
                    # gate refused Halo there for lack of hardware T&L, which
                    # is precisely the buried-pending-action the comment above
                    # says it is avoiding. Only lan-doc's own TITLE-LEVEL rows
                    # are withdrawn: a two-box proof (the `ip is not None`
                    # branch below) names its boxes explicitly and is never
                    # touched here, nor is any row from another source.
                    cur = con.execute(
                        "DELETE FROM compat_mp WHERE ip=? AND title=? "
                        "AND origin='measured' AND source='lan-doc' "
                        # partner_ip is NOT NULL DEFAULT '' - a title-level
                        # row has the empty string here, a two-box proof
                        # carries the partner. `IS NULL` would match
                        # nothing and delete nothing, silently.
                        "AND partner_ip = ''", (b, title))
                    withdrawn += cur.rowcount or 0
                    continue
                C.put_mp(con, b, title, "measured", status, transport=transport,
                         blocker=blocker[:400], remedy=remedy[:400],
                         source="lan-doc", measured_at=when)
                written += 1
            continue
        C.put_mp(con, ip, title, "measured", status, partner_ip=partner,
                 role="both", transport=transport, source="lan-doc",
                 measured_at=when)
        C.put_evidence(con, ip, title, "mp", "doc",
                       "docs/lan-multiplayer-status.md",
                       when, "two-box proof, both ends screenshotted")
        written += 1
    detail = ""
    if withdrawn:
        # Say it out loud. A row disappearing from the matrix is a change to
        # what the fleet reports, and a silent deletion is worse than a stale
        # row - nobody can tell it happened.
        detail = ("withdrew %d stale title-level row(s) for boxes where the "
                  "title is no longer deployed; " % withdrawn)
        print("lan-doc: %s" % detail.strip("; "), file=sys.stderr)
    if unmapped:
        # LOUD, not silent: an unmapped title is a verification that did not
        # reach the matrix, and it must never look like a clean run.
        detail += "UNMAPPED titles (not in DOC_ALIASES): " + ", ".join(sorted(set(unmapped)))
        print("WARNING: %s" % detail, file=sys.stderr)
    C.log_ingest(con, "lan-doc", not unmapped, rows_in=len(facts),
                 rows_written=written, detail=detail)
    return written


EVIDENCE_DIRS = ["/home/voidsstr/lan-proof", "/tmp/lanid/evidence",
                 "/tmp/retro-screenshots"]

# Filename token -> library title. EXPLICIT, because a fuzzy match here would
# attach one game's screenshot to another game's cell, and a fabricated piece
# of evidence is worse than none: it makes an untested cell look proved.
EVIDENCE_TOKENS = {
    "quake1": "Quake1", "quake2": "Quake2Complete", "q3": "Quake3Arena",
    "teamarena": "Quake3-TeamArena", "q3ta": "Quake3-TeamArena",
    "hexen2": "HexenII", "sin": "SiNGold", "sof1": "SoldierOfFortune",
    "sof2": "SoldierOfFortune2", "jkdf2": "JediKnightDF2",
    "mots": "JediKnightMotS", "jediacademy": "JediAcademy",
    "jka": "JediAcademy", "doom3": "Doom3", "shogo": "Shogo",
    "cs16": "CounterStrike16", "hldm": "HalfLife-Deathmatch",
    "tfc": "HalfLife-TFC", "opfor": "HalfLife-OpposingForce",
    "dmc": "HalfLife-DMC", "halflife": "HalfLife1", "hl1": "HalfLife1",
    "deusex": "DeusEx", "ut2004": "UT2004", "ut99": "UnrealTournament",
    "ut436": "UnrealTournament436", "unrealgold": "UnrealGold",
    "ra2": "RedAlert2", "yuri": "YurisRevenge", "tibsun": "TiberianSun",
    "starcraft": "StarCraft", "descent1": "Descent1", "descent2": "Descent2",
    "descent3": "Descent3", "rtcw": "ReturnToCastleWolfenstein",
    "farcry": "FarCry", "halo": "Halo", "bf1942": "BF1942",
    "carmageddon": "Carmageddon1", "carma2": "Carmageddon2",
    "redneck": "RedneckRampage", "redfaction": "RedFaction",
    "turok2": "Turok2", "avp": "AliensVsPredator", "maxpayne": "MaxPayne",
    "sshock2": "SystemShock2", "thief2": "Thief2", "thiefgold": "ThiefGold",
    "warcraft2": "WarcraftII", "moo2": "MasterOfOrionII",
    "shadowwarrior": "ShadowWarrior", "hd": "HiddenAndDangerous",
    "seriousfirst": "SeriousSamFirstEncounter",
    "serioussecond": "SeriousSamSecondEncounter",
}


def ingest_evidence(con, strict=True):
    """Attach the screenshots on disk to the cells they belong to.

    IT ATTACHES EVIDENCE AND IT NEVER SETS A VERDICT. A file called
    `doom3_240_fullscreen.png` is a strong hint that Doom 3 ran fullscreen on
    .240, and promoting that hint to `verified` from a FILENAME is precisely
    the fabrication the rest of this module exists to prevent. The screenshot
    is attached; the cell keeps whatever state a real observation gave it, and
    `gaps --evidence` then lists the cells that have a picture and no recorded
    verification, which is a short, actionable worklist rather than a guess.
    """
    boxes = {r["ip"].rsplit(".", 1)[1]: r["ip"]
             for r in con.execute("SELECT ip FROM compat_box")}
    known = {r["title"] for r in con.execute("SELECT title FROM compat_title")}
    n = unmatched = 0
    missing_dirs = []
    for d in EVIDENCE_DIRS:
        if not os.path.isdir(d):
            missing_dirs.append(d)
            continue
        for root, _dirs, files in os.walk(d):
            for fn in files:
                if not re.search(r"\.(png|jpg|jpeg|bmp|txt|log)$", fn, re.I):
                    continue
                low = fn.lower()
                nums = re.findall(r"(?<!\d)(\d{2,3})(?!\d)", low)
                ip = next((boxes[x] for x in nums if x in boxes), None)
                # Longest token first so `descent3` never matches as `descent`.
                title = next((v for k, v in sorted(
                    EVIDENCE_TOKENS.items(), key=lambda kv: -len(kv[0]))
                    if re.search(r"(?<![a-z0-9])%s(?![a-z0-9])" % re.escape(k), low)),
                    None)
                if not ip or not title or title not in known:
                    unmatched += 1
                    continue
                path = os.path.join(root, fn)
                when = _dt.datetime.fromtimestamp(
                    os.path.getmtime(path)).strftime("%Y-%m-%d %H:%M:%S")
                axis = "mp" if re.search(
                    r"lan|host|join|browser|frag|scoreboard|sees", low) else "render"
                C.put_evidence(con, ip, title, axis, "screenshot", path, when,
                               note=fn)
                n += 1
    detail = ""
    if missing_dirs:
        detail = "evidence dirs absent: " + ", ".join(missing_dirs)
    if unmatched:
        detail += ("  %d file(s) matched no (box, title) pair and were NOT "
                   "guessed at" % unmatched)
    if detail:
        print("note: %s" % detail, file=sys.stderr)
    C.log_ingest(con, "evidence", True, rows_in=n + unmatched, rows_written=n,
                 detail=detail)
    return n


SOURCES = {
    "inventory": ingest_roster_and_inventory,
    "library": ingest_library,
    "gamegate": ingest_gamegate,
    "installed": ingest_installed,
    "probe": ingest_probe,
    "lan-doc": ingest_lan_doc,
    "evidence": ingest_evidence,
}
# Order matters: the probe must run after the gate (so a gated title keeps the
# reading that names its remedy) and the LAN doc after deployment is known (so
# a title-level multiplayer fact is only written where the title actually is).
ORDER = ["inventory", "library", "gamegate", "installed", "probe",
         "lan-doc", "evidence"]


def cmd_ingest(con, a):
    names = [a.source] if a.source else [
        n for n in ORDER if not (a.no_probe and n == "probe")]
    total = {}
    failures = []
    for name in names:
        fn = SOURCES[name]
        try:
            r = fn(con, strict=not a.lenient)
            total[name] = r if not isinstance(r, tuple) else r[1]
        except IngestError as e:
            failures.append("%s: %s" % (name, e))
            C.log_ingest(con, name, False, detail=str(e))
            print("FAILED  %-12s %s" % (name, e), file=sys.stderr)
            continue
        print("ok      %-12s %s row(s)" % (name, total[name]))
    if not a.source:
        print("ok      %-12s %s row(s)" % ("mods", propagate_mods(con)))
        print("ok      %-12s %s row(s)" % ("lan-implies-run", imply_runs_from_lan(con)))
    if failures and not a.lenient:
        print("\n%d source(s) FAILED - the matrix below is incomplete."
              % len(failures), file=sys.stderr)
        return 1
    return 0


# ===========================================================================
# QUERIES
# ===========================================================================

def _matrix_rows(con, box=None, title=None, include_withdrawn=False,
                 stale_days=None):
    sql = "SELECT * FROM v_compat_matrix WHERE 1=1"
    args = []
    if not include_withdrawn:
        sql += " AND in_library=1"
    if box:
        sql += " AND ip=?"
        args.append(_full(box))
    if title:
        sql += " AND lower(title)=lower(?)"
        args.append(title)
    sql += " ORDER BY title, ip"
    rows = [dict(r) for r in con.execute(sql, args)]
    _mark_stale(rows, stale_days)
    return rows


def _mark_stale(rows, days=None):
    """A stale row must LOOK stale.

    The staged library is redeployed constantly, so a verification from six
    weeks ago has survived several GAMESYNCs and may describe a tree that no
    longer exists. `stale` is not a fault - the fleet is powered on demand and
    old data is normal - it means RE-MEASURE BEFORE TRUSTING THIS. A cell that
    was never measured is `None`, not stale: absence of a date and an old date
    are different facts.
    """
    days = C.DEFAULT_STALE_DAYS if days is None else days
    now = _dt.datetime.now()
    for r in rows:
        r["stale"] = None
        r["age_days"] = None
        when = r.get("measured_at")
        if not when:
            continue
        for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d"):
            try:
                age = (now - _dt.datetime.strptime(when[:19], fmt)).days
            except ValueError:
                continue
            r["age_days"] = age
            r["stale"] = age > days
            break


DEPLOY_MARK = {"deployed": "+", "gated": "G", "marginal": "~", "skipped": "S",
               "failed": "!", "absent": "-", "untested": "?"}
RUN_MARK = {"verified": "V", "runs": "r", "failed": "X", "untested": "?", "n/a": "."}
MP_MARK = {"verified_two_box": "2", "verified_server": "S", "peer_unproven": "p",
           "needs_human": "H", "no_multiplayer": "0", "blocked": "B",
           "untested": "?", "n/a": "."}


def cmd_matrix(con, a):
    rows = _matrix_rows(con, a.box, a.title, a.all)
    if a.json:
        print(json.dumps(rows, indent=1))
        return 0
    boxes = [r["ip"] for r in con.execute(
        "SELECT ip FROM compat_box ORDER BY ip")]
    titles = sorted({r["title"] for r in rows})
    cell = {(r["ip"], r["title"]): r for r in rows}
    w = max([len(t) for t in titles] + [12])
    hdr = " " * (w + 2) + "  ".join("%-4s" % _short(b) for b in boxes)
    print(hdr)
    print("  key: deploy/run/mp   deploy + deployed G gated ~ marginal "
          "- absent ? untested")
    print("                       run    V verified r runs X failed ? untested")
    print("                       mp     2 two-box S server p peer-unproven "
          "H needs-human 0 none ? untested")
    print()
    for t in titles:
        line = "%-*s  " % (w, t)
        for b in boxes:
            r = cell.get((b, t))
            if r is None:
                line += "%-6s" % "???"
            else:
                line += "%-6s" % (DEPLOY_MARK.get(r["deploy"], "?") +
                                  RUN_MARK.get(r["runs"], "?") +
                                  MP_MARK.get(r["mp"], "?"))
        print(line)
    print()
    cmd_summary(con, a, rows=rows)
    return 0


def cmd_summary(con, a, rows=None):
    rows = rows if rows is not None else _matrix_rows(con)
    n = len(rows)
    if not n:
        print("no cells - run `compat.py ingest` first")
        return 1
    def count(key, val):
        return sum(1 for r in rows if r[key] == val)
    never = sum(1 for r in rows
                if r["deploy"] == "untested" and r["runs"] == "untested"
                and r["mp"] == "untested")
    print("%d cells (%d boxes x %d titles)" % (
        n, len({r["ip"] for r in rows}), len({r["title"] for r in rows})))
    print("  deploy : %3d deployed  %3d gated  %3d marginal  %3d absent  %3d untested"
          % (count("deploy", "deployed"), count("deploy", "gated"),
             count("deploy", "marginal"), count("deploy", "absent"),
             count("deploy", "untested")))
    print("  runs   : %3d verified  %3d runs  %3d failed  %3d untested"
          % (count("runs", "verified"), count("runs", "runs"),
             count("runs", "failed"), count("runs", "untested")))
    print("  mp     : %3d two-box  %3d peer-unproven  %3d needs-human  "
          "%3d no-mp  %3d untested"
          % (count("mp", "verified_two_box"), count("mp", "peer_unproven"),
             count("mp", "needs_human"), count("mp", "no_multiplayer"),
             count("mp", "untested")))
    stale = sum(1 for r in rows if r.get("stale"))
    print("  %d cell(s) NEVER TESTED on any axis - that is the honest gap, "
          "not a pass." % never)
    print("  %d cell(s) carry a fact older than %d days - re-measure before "
          "quoting them." % (stale, C.DEFAULT_STALE_DAYS))
    return 0


def cmd_status(con, a):
    rows = _matrix_rows(con, a.box, a.title, a.all)
    if a.json:
        print(json.dumps(rows, indent=1))
        return 0
    if not rows:
        print("nothing matches (unknown box or title?)")
        return 1
    key = "title" if a.box else "ip"
    if a.box:
        b = con.execute("SELECT * FROM compat_box WHERE ip=?",
                        (_full(a.box),)).fetchone()
        if b:
            print("%s  %s  %s" % (b["ip"], b["hostname"], b["os"]))
            print("  %s | %s MB | %s [%s]" % (b["cpu"], b["ram_mb"], b["gpu"],
                                              b["gpu_class"]))
            print("  3D accelerators: %s" % (b["accelerators"] or "unknown"))
            print("  record: %s (%s)" % (b["state"], b["measured_at"] or "-"))
            print()
    print("%-26s %-10s %-9s %-18s %s" % (key.upper(), "DEPLOY", "RUNS", "MULTIPLAYER", "DETAIL"))
    for r in rows:
        detail = r["mp_blocker"] or r["deploy_reason"] or ""
        if r["gate"] == "no" and r["deploy"] == "deployed" and not detail:
            detail = "GATE SAYS NO on this box"
        if r["renderer"] and r["renderer"] != "unknown":
            detail = "%s %sx%s %s" % (r["renderer"], r["width"], r["height"],
                                      r["fullscreen"]) + (" | " + detail if detail else "")
        star = "*" if r["mp_origin"] == "measured" or r["render_origin"] == "measured" else " "
        print("%-26s %-10s %-9s %-18s%s%s" % (
            r[key][:26], r["deploy"], r["runs"], r["mp"], star, detail[:70]))
    print("\n* = hand-recorded (measured).  Unmarked rows are machine-derived.")
    return 0


def cmd_gaps(con, a):
    """What nobody has ever looked at.  The point of the whole exercise: a gap
    must be countable, not merely absent."""
    rows = _matrix_rows(con)
    gaps = [r for r in rows if r["runs"] == "untested"
            and r["deploy"] in ("deployed", "marginal")]
    if getattr(a, "evidence", False):
        gaps = [r for r in gaps if r["evidence"]]
        print("cells that HAVE a screenshot on disk but no recorded "
              "verification - the shortest route to a proved matrix:\n")
    if a.json:
        print(json.dumps(gaps, indent=1))
        return 0
    print("%d cell(s) where the title IS on the box and nobody has confirmed "
          "it runs:\n" % len(gaps))
    by_box = {}
    for r in gaps:
        by_box.setdefault(r["ip"], []).append(r["title"])
    for ip in sorted(by_box):
        print("  %-16s %3d  %s" % (ip, len(by_box[ip]),
                                   ", ".join(sorted(by_box[ip])[:8]) +
                                   (" ..." if len(by_box[ip]) > 8 else "")))
    return 0


def cmd_conflicts(con, a):
    rows = [dict(r) for r in con.execute("SELECT * FROM v_compat_conflict")]
    if a.json:
        print(json.dumps(rows, indent=1))
        return 0
    if not rows:
        print("no disagreements between hand-recorded and machine-derived facts.")
        return 0
    bad = [r for r in rows if r["kind"] == "contradiction"]
    up = [r for r in rows if r["kind"] == "upgrade"]
    print("%d cell(s) where a MEASURED fact disagrees with a DERIVED one. "
          "Both are kept;\nthe measured value wins in every view.\n" % len(rows))
    print("CONTRADICTIONS - %d. Worth looking at: two sources that both claim "
          "to know." % len(bad))
    for r in bad:
        print("  %-7s %-16s %-26s measured=%-14s derived=%-14s (%s vs %s)" % (
            r["axis"], _short(r["ip"]), r["title"], r["measured"], r["derived"],
            r["measured_source"], r["derived_source"]))
    print("\nUPGRADES - %d. The system working: somebody measured a cell that "
          "was\nguessed or unknown. Listed with --all." % len(up))
    if a.all:
        for r in up:
            print("  %-7s %-16s %-26s measured=%-14s derived=%-14s (%s vs %s)"
                  % (r["axis"], _short(r["ip"]), r["title"], r["measured"],
                     r["derived"], r["measured_source"], r["derived_source"]))
    return 0


def cmd_record(con, a):
    """A hand-recorded verification.  Always `origin='measured'`, so no later
    ingest can overwrite it."""
    ip = _full(a.box)
    if not con.execute("SELECT 1 FROM compat_box WHERE ip=?", (ip,)).fetchone():
        print("unknown box %s - run `ingest` first, or check the IP" % ip,
              file=sys.stderr)
        return 1
    if not con.execute("SELECT 1 FROM compat_title WHERE title=?",
                       (a.title,)).fetchone():
        print("unknown title %r - it must be a Games-Library directory name "
              "(or a registered mod). Run `compat.py titles` to list them."
              % a.title, file=sys.stderr)
        return 1
    when = a.measured_at or C.now()
    wrote = []
    try:
        if a.runs:
            w = h = None
            if a.res:
                w, h = (int(x) for x in a.res.lower().split("x"))
            C.put_render(con, ip, a.title, "measured", a.runs,
                         shortcut=a.shortcut or "", renderer=a.renderer,
                         width=w, height=h, refresh_hz=a.refresh,
                         fullscreen=a.fullscreen, detail=a.detail or "",
                         source=a.source, measured_at=when)
            wrote.append("runs=%s" % a.runs)
            # A PER-SHORTCUT ROW IS INVISIBLE UNLESS A TITLE-LEVEL ROW EXISTS.
            #
            # `v_compat_matrix` (and `status`, and `gaps`) join on
            # `shortcut=''`, because the grid has one cell per box x TITLE. So
            # a measurement recorded against a single launcher lands in the
            # table and is then absent from every view -- `gaps` even lists the
            # cell as never-looked-at, which sends the next agent to re-measure
            # something already proved.
            #
            # Measured 2026-08-31: six real observations on .246 were sitting
            # in compat_render, four of them `verified` with evidence, none of
            # them visible.
            #
            # So a shortcut-level record also refreshes the title-level cell,
            # but ONLY when that cell has no `measured` row already -- a
            # title-level observation is the stronger statement (it is about
            # the game, not one launcher), and one shortcut must never silently
            # overwrite it. Recording every shortcut of a title therefore
            # settles the title on whichever was recorded first, which is
            # visible and correctable, rather than on whichever ran last.
            if a.shortcut:
                have = con.execute(
                    "SELECT 1 FROM compat_render WHERE ip=? AND title=? "
                    "AND shortcut='' AND origin='measured'",
                    (ip, a.title)).fetchone()
                if not have:
                    C.put_render(con, ip, a.title, "measured", a.runs,
                                 shortcut="", renderer=a.renderer,
                                 width=w, height=h, refresh_hz=a.refresh,
                                 fullscreen=a.fullscreen,
                                 detail=("via shortcut %s; %s"
                                         % (a.shortcut, a.detail or "")).strip("; "),
                                 source=a.source, measured_at=when)
                    wrote.append("(also title-level, so the matrix can see it)")
        if a.mp:
            C.put_mp(con, ip, a.title, "measured", a.mp,
                     partner_ip=_full(a.partner) if a.partner else "",
                     role=a.role or "", transport=a.transport,
                     blocker=a.blocker or "", remedy=a.remedy or "",
                     source=a.source, measured_at=when)
            wrote.append("mp=%s" % a.mp)
        if a.deploy:
            C.put_deploy(con, ip, a.title, "measured", a.deploy,
                         reason=a.detail or "", source=a.source,
                         measured_at=when)
            wrote.append("deploy=%s" % a.deploy)
    except C.BadState as e:
        print("REJECTED: %s" % e, file=sys.stderr)
        return 2
    if not wrote:
        print("nothing to record - give at least one of --runs / --mp / --deploy",
              file=sys.stderr)
        return 1
    for ev in a.evidence or []:
        axis = "mp" if a.mp else "render"
        kind = "screenshot" if re.search(r"\.(png|bmp|jpg|jpeg)$", ev, re.I) \
            else "logline"
        C.put_evidence(con, ip, a.title, axis, kind, ev, when)
    if a.runs == "verified" and not a.evidence:
        # Not refused - a verification without a screenshot is still better
        # than nothing - but it must SAY it is weaker, or "verified" quietly
        # comes to mean "somebody thought so".
        print("WARNING: recorded `verified` with NO evidence. A verified row "
              "with no evidence is an opinion; pass --evidence.", file=sys.stderr)
    print("recorded %s %s: %s (measured, %s)" % (ip, a.title,
                                                 ", ".join(wrote), when))
    return 0


def cmd_titles(con, a):
    for r in con.execute("SELECT title, kind, parent, in_library, display_name "
                         "FROM compat_title ORDER BY title"):
        flag = "" if r["in_library"] else "  [WITHDRAWN]"
        par = "  <- %s" % r["parent"] if r["parent"] else ""
        print("%-30s %-6s %s%s%s" % (r["title"], r["kind"], r["display_name"],
                                     par, flag))
    return 0


def cmd_export(con, a):
    """What the dashboard eats.  A plain JSON document, no credentials of any
    kind - the payload is boxes, titles, verdicts and evidence PATHS.  No CD
    key, no agent secret, no share password ever enters this file."""
    rows = _matrix_rows(con, include_withdrawn=True)
    boxes = [dict(r) for r in con.execute(
        "SELECT ip,hostname,os,cpu,cpu_mhz,ram_mb,gpu,gpu_class,accelerators,"
        "display_mode,agent_version,state,note,measured_at FROM compat_box "
        "ORDER BY ip")]
    titles = [dict(r) for r in con.execute(
        "SELECT title,display_name,engine,parent,kind,mp_design,in_library,note"
        " FROM compat_title ORDER BY title")]
    ingest = [dict(r) for r in con.execute(
        "SELECT * FROM compat_ingest ORDER BY id DESC LIMIT 25")]
    doc = {
        "generated": C.now(),
        "schema": 1,
        "legend": {"deploy": list(C.DEPLOY_STATES), "runs": list(C.RUN_STATES),
                   "mp": list(C.MP_STATES), "renderer": list(C.RENDERERS)},
        "boxes": boxes, "titles": titles, "matrix": rows,
        "conflicts": [dict(r) for r in con.execute("SELECT * FROM v_compat_conflict")],
        "ingest_log": ingest,
    }
    blob = json.dumps(doc, indent=1)
    _assert_no_secrets(blob)
    if a.out:
        with open(a.out, "w") as f:
            f.write(blob)
        print("wrote %s (%d cells, %d boxes, %d titles)"
              % (a.out, len(rows), len(boxes), len(titles)))
    else:
        print(blob)
    return 0


_SECRET_PATTERNS = [
    re.compile(r"retro-agent-secret"),
    re.compile(r"\b[A-Z0-9]{5}(-[A-Z0-9]{5}){4}\b"),        # XXXXX-XXXXX-... keys
    re.compile(r"\b[A-Za-z0-9]{13,25}\b(?=\s*(?:cd ?key|serial))", re.I),
    re.compile(r"(?i)\b(password|passwd|rcon_password)\s*[=:]\s*\S+"),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
]


def _assert_no_secrets(blob):
    """NO SECRET MAY REACH THE DASHBOARD OR ANY CLOUD STORE.

    The export is published to an Azure-hosted dashboard, so this is not a
    style check: a CD key or the agent secret leaving the LAN in a payload is
    the one irreversible mistake available here.  It fails the export rather
    than stripping the value, because a silently-scrubbed export teaches nobody
    that a secret got into the database in the first place.
    """
    for pat in _SECRET_PATTERNS:
        m = pat.search(blob)
        if m:
            raise SystemExit(
                "REFUSING TO EXPORT: the payload contains something that looks "
                "like a secret (%r at offset %d). Secrets live in Azure Key "
                "Vault nsc-secrets-kv and must never reach the dashboard."
                % (m.group(0)[:40], m.start()))


def cmd_doc(con, a):
    """Report where docs/lan-multiplayer-status.md and the database disagree.

    The brief for this database was that the LAN document should become a
    RENDERING of it rather than a rival source of truth. It is not regenerated
    wholesale, and deliberately so: most of that file is hard-won prose - why
    four titles stall on the same DOSBox mouse problem, why Serious Sam was
    withdrawn, which probe each engine answers - and a generator would flatten
    all of it into tables, destroying the part that is actually expensive.

    So the document keeps its prose and this keeps them honest. Drift only
    happens in one direction that matters: somebody records a verification with
    `compat.py record` and does not write it up. That is what this finds.
    """
    facts, unmapped = _mp_from_doc(open(LAN_DOC).read())
    in_doc = {(t, ip) for t, ip, st, *_ in facts if ip} | \
             {(t, None) for t, ip, st, *_ in facts if not ip}
    doc_titles = {t for t, _ip, *_ in facts}
    drift = []
    for r in con.execute(
            "SELECT ip, title, status, partner_ip, source FROM compat_mp "
            "WHERE origin='measured' AND status IN "
            "('verified_two_box','verified_server','blocked','no_multiplayer')"
            " ORDER BY title, ip"):
        if r["source"] == "lan-doc":
            continue                      # it CAME from the doc; not drift
        if (r["title"], r["ip"]) in in_doc or r["title"] in doc_titles:
            continue
        drift.append(dict(r))
    if a.json:
        print(json.dumps({"drift": drift, "unmapped": sorted(set(unmapped))},
                         indent=1))
        return 0
    if unmapped:
        print("%d title(s) in the document map to NO library directory, so "
              "their verifications never reached the matrix:" % len(unmapped))
        for u in sorted(set(unmapped)):
            print("    %s" % u)
        print()
    if not drift:
        print("no drift: every hand-recorded multiplayer verification is "
              "either from the document or already written up in it.")
        return 0 if not unmapped else 1
    print("%d verification(s) recorded in the database but NOT in "
          "docs/lan-multiplayer-status.md:" % len(drift))
    for d in drift:
        print("    %-16s %-26s %-18s %s" % (_short(d["ip"]), d["title"],
                                            d["status"], d["source"]))
    print("\nWrite them up, or the document stops being true.")
    return 1


def main(argv=None):
    p = argparse.ArgumentParser(
        description="fleet-wide game compatibility matrix",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument("--db", help="override the database path (testing)")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("ingest", help="refresh from every machine-derived source")
    s.add_argument("--source", choices=list(SOURCES))
    s.add_argument("--no-probe", action="store_true",
                   help="skip the live DIRLIST sweep (fleet powered down)")
    s.add_argument("--lenient", action="store_true",
                   help="report a missing source instead of failing (default "
                        "is to FAIL LOUDLY: a silent skip yields a matrix that "
                        "looks complete and is not)")

    for name, helptext in (("matrix", "the per-box x per-title grid"),
                           ("status", "one box or one title, in detail"),
                           ("gaps", "deployed but nobody confirmed it runs"),
                           ("conflicts", "measured vs derived disagreements"),
                           ("summary", "the counts, including never-tested")):
        s = sub.add_parser(name, help=helptext)
        s.add_argument("--box")
        s.add_argument("--title")
        s.add_argument("--all", action="store_true",
                       help="include withdrawn titles")
        s.add_argument("--json", action="store_true")
        s.add_argument("--evidence", action="store_true",
                       help="gaps: only cells that already have a screenshot")

    s = sub.add_parser("titles", help="list every title the matrix knows")

    s = sub.add_parser("record", help="record a HAND-OBSERVED verification")
    s.add_argument("--box", required=True)
    s.add_argument("--title", required=True)
    s.add_argument("--shortcut", default="")
    s.add_argument("--runs", choices=C.RUN_STATES)
    s.add_argument("--renderer", choices=C.RENDERERS, default="unknown")
    s.add_argument("--res", help="WxH as actually rendered on THIS box")
    s.add_argument("--refresh", type=int)
    s.add_argument("--fullscreen", choices=("yes", "no", "unknown"),
                   default="unknown")
    s.add_argument("--mp", choices=C.MP_STATES)
    s.add_argument("--partner", help="the other box in a two-box proof")
    s.add_argument("--role", choices=("host", "join", "both", "client"))
    s.add_argument("--transport", choices=C.TRANSPORTS, default="unknown")
    s.add_argument("--blocker")
    s.add_argument("--remedy")
    s.add_argument("--deploy", choices=C.DEPLOY_STATES)
    s.add_argument("--detail")
    s.add_argument("--evidence", action="append",
                   help="screenshot path or log line; repeatable")
    s.add_argument("--measured-at", dest="measured_at",
                   help="when it was OBSERVED (default now) - for backfill")
    s.add_argument("--source", default="manual")

    s = sub.add_parser("export", help="write the dashboard's JSON payload")
    s.add_argument("--out")

    s = sub.add_parser("doc", help="has the LAN status doc drifted from the DB?")
    s.add_argument("--check", action="store_true",
                   help="accepted for symmetry; this command only ever checks")
    s.add_argument("--json", action="store_true")

    a = p.parse_args(argv)
    con = C.connect(a.db)
    fn = {"ingest": cmd_ingest, "matrix": cmd_matrix, "status": cmd_status,
          "gaps": cmd_gaps, "conflicts": cmd_conflicts, "summary": cmd_summary,
          "record": cmd_record, "titles": cmd_titles, "export": cmd_export,
          "doc": cmd_doc}[a.cmd]
    try:
        return fn(con, a)
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
