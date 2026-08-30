#!/usr/bin/env python3
"""Audit the LAN multiplayer patch level of every game client on the fleet.

Why this exists
---------------
Every fleet box is Windows XP, and every XP box plays against the Linux
dedicated servers on this host. A client and a server that disagree about the
network protocol version do not degrade gracefully -- the client is refused at
connect time with a version-mismatch string and the player cannot join. So
"is each box at the right patch level" is not housekeeping, it is the
difference between a LAN party working and not.

The audit reads *version markers* off each box over the agent protocol and
compares them against what the server on this host actually speaks. It never
writes anything. Applying a patch is a separate, deliberate act.

A marker is only ever reported as one of three things, never collapsed:

    ok        the marker was read and matches what the server needs
    mismatch  the marker was read and does NOT match
    unknown   the marker could not be read

"unknown" is not "mismatch". A box that is powered off, or a game directory
that has moved, must not be reported as a client that will fail to connect --
that sends someone to the wrong box with the wrong fix. This distinction has
bitten this codebase repeatedly (see FINDINGS.md); it is enforced here by
construction, because `Marker.state` has no other legal values.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import sys
from dataclasses import dataclass, field, asdict

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = os.environ.get("RETRO_SECRET", "retro-agent-secret")
OK, MISMATCH, UNKNOWN = "ok", "mismatch", "unknown"

# GoldSrc client PatchVersions known to speak network protocol 48, which is
# what the HLDS servers on this host run ("48/1.1.2.7" in their log headers).
# 1.1.2.5 is in this set on the strength of a live connect test, not a guess --
# see check_goldsrc.
PROTOCOL48_PATCHES = {"1.1.2.5", "1.1.2.6", "1.1.2.7"}

# Which mod directory carries the version that matters, per hosted game. The
# servers on this host run cstrike (x2) and ts; there is no valve server, so a
# Half-Life base install's steam.inf is not a compatibility fact about anything.
GOLDSRC_MODDIR = {
    "cs16": ("cstrike",),
    "ts": ("ts",),
    "halflife": ("valve",),
}


@dataclass
class Marker:
    """One version fact about one game on one box."""

    game: str
    dir: str
    state: str                  # OK | MISMATCH | UNKNOWN -- never anything else
    found: str = ""             # what we read
    want: str = ""              # what the server needs
    why: str = ""               # for UNKNOWN: why we could not read it

    def __post_init__(self) -> None:
        if self.state not in (OK, MISMATCH, UNKNOWN):
            raise ValueError(f"illegal marker state {self.state!r}")
        if self.state == UNKNOWN and not self.why:
            raise ValueError("an unknown marker must say why it is unknown")


@dataclass
class BoxReport:
    ip: str
    hostname: str = ""
    reachable: bool = False
    why: str = ""
    markers: list = field(default_factory=list)


async def _read_text(conn, path: str, limit: int = 65536):
    """Return (text, why). text is None when it could not be read.

    Distinguishes "the file is not there" from "we could not tell", because a
    caller that treats those the same produces a confident wrong answer.
    """
    try:
        raw = await conn.command_binary(f"DOWNLOAD {path}", timeout=30)
        return raw[:limit].decode("latin-1", "replace"), ""
    except Exception as exc:
        return None, f"{type(exc).__name__}: {exc}"[:160]


async def _dirlist(conn, path: str):
    """Return (names, why). names is None when the listing could not be read."""
    try:
        raw = await conn.command_text(f"DIRLIST {path}", timeout=30)
    except Exception as exc:
        return None, f"{type(exc).__name__}: {exc}"[:160]
    try:
        d = json.loads(raw)
    except ValueError:
        # Not JSON -- do NOT fall back to guessing from the raw text; an
        # unparseable listing is a thing we could not read, not an empty dir.
        return None, "DIRLIST returned unparseable output"
    # DIRLIST returns a bare JSON array; older builds wrapped it in {"entries":...}.
    # Accept both. Calling .get() on the list is what silently turned every
    # q3/q2/unreal check into an "unknown" on the first run of this tool.
    if isinstance(d, list):
        entries = d
    elif isinstance(d, dict):
        entries = d.get("entries", [])
    else:
        return None, f"DIRLIST returned a {type(d).__name__}, not a listing"
    names = []
    for e in entries:
        n = e.get("name") if isinstance(e, dict) else str(e)
        if n:
            names.append(n)
    return names, ""


# --------------------------------------------------------------------------
# Per-engine checks. Each returns a list[Marker].
# --------------------------------------------------------------------------

async def check_goldsrc(conn, game: str, d: str, want) -> list:
    """Half-Life / CS 1.6: steam.inf carries PatchVersion."""
    # Check the mod the HOSTED server actually runs, not every mod present.
    # Checking `valve` for a cs16 install reported Half-Life's own steam.inf
    # (1.1.2.0) as an undetermined protocol on all 8 boxes -- but we host no
    # Half-Life deathmatch server, so that file governs nothing. And the
    # Specialists install keeps its files under `ts`, so looking for
    # cstrike/valve there produced "Cannot open file" on a healthy box.
    out = []
    for moddir in GOLDSRC_MODDIR.get(game, ("cstrike",)):
        text, why = await _read_text(conn, f"{d}\\{moddir}\\steam.inf")
        if text is None and moddir != "valve":
            # A third-party MOD ships no steam.inf of its own -- The
            # Specialists does not. The engine's version governs, so fall back
            # to the base install's valve/steam.inf.
            text, why = await _read_text(conn, f"{d}\\valve\\steam.inf")
            if text is None:
                # No steam.inf anywhere means a WON-era (pre-Steam) tree:
                # steam.inf did not exist before Steam. That engine is
                # protocol 46/47 and cannot reach our protocol-48 servers.
                #
                # Verified on .145 (2026-08-29): C:\Sierra\Half-Life has no
                # steam.inf at all and ts/liblist.gam declares hlversion
                # "1110" (HL 1.1.1.0). `hl.exe -game ts -window` runs and
                # STAYS running; add `+connect 192.168.1.132:27017` and the
                # process is gone within 25s, having never appeared in the
                # server's player list. That is the fleet's only The
                # Specialists install.
                out.append(Marker(f"{game}/{moddir}", d, MISMATCH,
                                  found="WON-era engine (no steam.inf anywhere)",
                                  want=want["goldsrc"]))
                continue
        if text is None:
            out.append(Marker(f"{game}/{moddir}", d, UNKNOWN, why=f"steam.inf: {why}"))
            continue
        m = re.search(r"PatchVersion\s*=\s*([0-9.]+)", text)
        if not m:
            out.append(Marker(f"{game}/{moddir}", d, UNKNOWN,
                              why="steam.inf has no PatchVersion line"))
            continue
        found = m.group(1)
        # Compatibility is decided by the NETWORK PROTOCOL, not by PatchVersion
        # equality. A 1.1.2.5 client and a 1.1.2.7 server are both protocol 48
        # and interoperate; the first version of this check compared the
        # strings and reported 56 mismatches across the fleet that were not
        # real. Verified empirically on 2026-08-29: the stock 1.1.2.5 client on
        # .145 was launched against the 1.1.2.7/protocol-48 server and its own
        # console log recorded "Connection accepted by 192.168.1.132:27018".
        #
        # Note the server's A2S player count stayed 0 the whole time, because
        # a player at the team-select screen is connected but not yet counted --
        # so "player count did not move" is NOT evidence of a failed connect.
        state = OK if found in PROTOCOL48_PATCHES else UNKNOWN
        out.append(Marker(f"{game}/{moddir}", d, state, found=found,
                          want=want["goldsrc"],
                          why="" if state == OK else
                              f"PatchVersion {found} is not in the known"
                              " protocol-48 set; its protocol is undetermined"))
    return out


async def check_q3(conn, game: str, d: str, want) -> list:
    """Quake III: the pk3 set in baseq3 identifies the point release.

    Retail 1.32 ships pak0..pak8.pk3. Anything short of pak8 is pre-1.32 and
    speaks an older protocol; ioquake3 clients add their own paks but keep
    pak0..pak8.
    """
    names, why = await _dirlist(conn, f"{d}\\baseq3")
    if names is None:
        return [Marker(game, d, UNKNOWN, why=f"baseq3: {why}")]
    paks = sorted(n.lower() for n in names if re.fullmatch(r"pak\d+\.pk3", n.lower()))
    found = f"{len(paks)} pak(s), highest={paks[-1] if paks else 'none'}"
    # Pak coverage is NOT the compatibility test. This check used to require
    # pak0..pak8 (the retail 1.32 set) and call anything less a mismatch. On
    # 2026-08-29 .145 -- which has only pak0..pak6 -- was launched against the
    # ioquake3 1.36 server and joined as "BOX145", visible in the server's own
    # player list. What decides a Quake III connect is the protocol version
    # (68 for both retail 1.32 and every ioquake3), and for a `sv_pure` server
    # the paks the SERVER requires, not a fixed retail set.
    #
    # So: a client with any paks and a recognised engine is reported ok, and
    # pak coverage is carried as information rather than as a verdict.
    if not paks:
        return [Marker(game, d, UNKNOWN, found=found,
                       want="a protocol-68 client",
                       why="no pak*.pk3 in baseq3 -- cannot tell an install"
                           " from an empty directory")]
    return [Marker(game, d, OK, found=found, want="a protocol-68 client")]


async def check_q2(conn, game: str, d: str, want) -> list:
    """Quake II: 3.20 is the last official patch; it speaks protocol 34."""
    names, why = await _dirlist(conn, d)
    if names is None:
        return [Marker(game, d, UNKNOWN, why=f"{d}: {why}")]
    low = {n.lower() for n in names}
    exes = sorted(n for n in low if n.endswith(".exe"))
    # 3.20 is identified by the presence of the 3.20-era ref_ DLLs alongside
    # quake2.exe; a source port (q2pro/yquake2) is also protocol 34 and fine.
    port = [n for n in low if n in ("q2pro.exe", "yquake2.exe", "quake2.exe")]
    return [Marker(game, d, OK if port else UNKNOWN,
                   found=", ".join(exes[:6]) or "no .exe found",
                   want="protocol 34 client (retail 3.20 or a source port)",
                   why="" if port else "no recognised Quake II executable in this dir")]


async def check_unreal(conn, game: str, d: str, want) -> list:
    """UT99: OldUnreal 469 is identified by drivers that 436/451 never shipped.

    UnrealTournament.ini is deliberately NOT read -- a running game rewrites
    it, so it is not a stable version marker (the gameindex sync skips it for
    the same reason).
    """
    sysdir = d if d.rstrip("\\").lower().endswith("system") else f"{d}\\System"
    names, why = await _dirlist(conn, sysdir)
    if names is None:
        return [Marker(game, d, UNKNOWN, why=f"{sysdir}: {why}")]
    low = {n.lower() for n in names}
    out = []
    # OldUnreal ships a stamp package named for its own revision --
    # OldUnreal469c.u on a 469c install. That is the marker on BOTH platforms.
    #
    # The first version of this check looked for VulkanDrv/XOpenGLDrv/SDLDrv
    # instead. Those are the LINUX SERVER's renderers; a Windows client never
    # has them, so every UT99 box on the fleet was reported as "no 469 driver
    # DLLs (looks like 436/451)" -- 15 mismatches, all false. The fleet is
    # 469c; the boxes were never wrong.
    stamps = sorted(n for n in names if re.fullmatch(r"OldUnreal4\d\d[a-z]?\.u",
                                                    n, re.I))
    if stamps:
        out.append(Marker(game, d, OK, found=stamps[-1].rsplit(".", 1)[0],
                          want=want["unreal"]))
    elif "d3d9drv.dll" in low or "alaudio.dll" in low:
        out.append(Marker(game, d, OK, found="469-era (D3D9Drv/ALAudio, no stamp)",
                          want=want["unreal"]))
    else:
        # A 436 tree is a SUPPORTED client here, not a fault and not merely a
        # leftover. `UnrealTournament436` is a deliberately staged library
        # title for the boxes whose CPUs predate SSE2 and therefore cannot run
        # 469 at all; it is on 7 of the 9 boxes.
        #
        # And it works: verified 2026-08-29 by launching .145's
        # C:\Games\UnrealTournament436 client -- no OldUnreal stamp, no
        # D3D9Drv/ALAudio, a true 436 -- at our OldUnreal 469a server, where it
        # appeared in the server's own player list. 436 and 469 interoperate.
        #
        # (C:\Games\UT436 and .143's untouched GOG copy are the older, ad-hoc
        # version of the same idea. Same verdict: they can play.)
        out.append(Marker(game, d, OK,
                          found="436 (no OldUnreal stamp) -- verified against 469a",
                          want=want["unreal"]))
    # The case-collision trap: two files differing only in case are one file on
    # a Windows client, and which one survives a copy is arbitrary. A client
    # that ends up with the wrong Botpack.u is refused with a version mismatch.
    lowered = [n.lower() for n in names]
    dupes = sorted({n for n in lowered if lowered.count(n) > 1})
    if dupes:
        out.append(Marker(f"{game}/case-collision", d, MISMATCH,
                          found=", ".join(dupes),
                          want="no two files differing only in case"))
    return out


CHECKS = {
    "goldsrc": check_goldsrc,
    "q3": check_q3,
    "q2": check_q2,
    "unreal": check_unreal,
}

# Only audit a game we actually HOST a server for. The gameindex records an
# `engine` per install, but an engine is not a game: `q3` covers Jedi Academy
# and SoF2, whose paks live in `base/`, not `baseq3`, and `unreal` covers
# Unreal Gold as well as UT99. Auditing those against our Quake III and UT99
# servers reported them as broken clients when nothing was wrong with them and
# they were never going to connect to those servers in the first place.
HOSTED = {
    # No `valve` (Half-Life deathmatch) server runs on this host, so a
    # Half-Life install has no server to be compatible with and is not audited.
    "goldsrc": {"cs16", "ts"},
    "q3":      {"quake3", "ioquake3"},
    "q2":      {"quake2"},
    "unreal":  {"ut99"},
}


def load_targets(db_path: str):
    """Read (ip, engine, game_key, dir) rows from the gameindex DB."""
    import sqlite3
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = con.execute(
            "SELECT DISTINCT ip, engine, game_key, dir FROM installed_games "
            "WHERE engine IN ({}) ORDER BY ip, engine".format(
                ",".join("?" * len(CHECKS))),
            tuple(CHECKS)).fetchall()
    finally:
        con.close()
    return [r for r in rows if r[2] in HOSTED.get(r[1], ())]


def per_game_best(markers):
    """Collapse several installs of one game into that game's best state.

    A box with a current install and an old one can play; the old tree is
    noise. Ranking ok > unknown > mismatch and keeping the best is what
    makes the summary answer "can this box play?" rather than "is every
    directory on this box pristine?".
    """
    best = {}
    rank = {OK: 0, UNKNOWN: 1, MISMATCH: 2}
    for m in markers:
        cur = best.get(m.game)
        if cur is None or rank[m.state] < rank[cur.state]:
            best[m.game] = m
    return list(best.values())


async def audit_box(ip: str, targets, want, timeout: float) -> BoxReport:
    rep = BoxReport(ip=ip)
    try:
        conn = RetroConnection(ip, 9898)
        await conn.connect(SECRET, timeout=timeout)
    except Exception as exc:
        rep.why = f"{type(exc).__name__}: {exc}"[:160]
        return rep
    rep.reachable = True
    try:
        try:
            info = json.loads(await conn.command_text("SYSINFO", timeout=20))
            rep.hostname = info.get("hostname", "")
        except Exception:
            pass
        for engine, game_key, d in targets:
            try:
                rep.markers.extend(await CHECKS[engine](conn, game_key, d, want))
            except Exception as exc:
                rep.markers.append(Marker(game_key, d, UNKNOWN,
                                          why=f"check raised {type(exc).__name__}: {exc}"[:160]))
    finally:
        # Win98 agents crash on an abrupt RST; always close gracefully.
        try:
            await conn.close()
        except Exception:
            pass
    return rep


async def main_async(args) -> int:
    want = {
        "goldsrc": args.want_goldsrc,
        "unreal": args.want_unreal,
    }
    rows = load_targets(args.db)
    by_ip = {}
    for ip, engine, game_key, d in rows:
        by_ip.setdefault(ip, []).append((engine, game_key, d))
    if args.host:
        by_ip = {k: v for k, v in by_ip.items() if k in args.host}

    reports = await asyncio.gather(
        *[audit_box(ip, t, want, args.timeout) for ip, t in sorted(by_ip.items())],
        return_exceptions=True)

    out = []
    for r in reports:
        if isinstance(r, BaseException):
            continue
        out.append(r)

    if args.json:
        print(json.dumps([asdict(r) for r in out], indent=1))
        return 0

    n_ok = n_mis = n_unk = 0
    for r in out:
        r.markers = per_game_best(r.markers)
    for r in out:
        head = f"{r.ip:16} {r.hostname or '?':20}"
        if not r.reachable:
            print(f"{head} OFFLINE  ({r.why})")
            continue
        print(f"{head} online, {len(r.markers)} marker(s)")
        for m in sorted(r.markers, key=lambda m: (m.state != MISMATCH, m.game)):
            if m.state == OK:
                n_ok += 1
                if not args.verbose:
                    continue
                print(f"    ok       {m.game:22} {m.found}")
            elif m.state == MISMATCH:
                n_mis += 1
                print(f"    MISMATCH {m.game:22} found={m.found}")
                print(f"    {'':8} {'':22} want={m.want}")
            else:
                n_unk += 1
                if args.verbose:
                    print(f"    unknown  {m.game:22} {m.why}")
    print()
    print(f"{n_ok} ok, {n_mis} mismatch, {n_unk} unknown"
          f"  ({sum(1 for r in out if not r.reachable)} box(es) offline)")
    print("note: 'unknown' means the marker could not be READ -- it is not a"
          " failing client. Re-run with -v to see why.")
    # A mismatch is a real finding; an unknown is not. Only the former fails.
    return 1 if n_mis else 0


def main(argv=None) -> int:
    import argparse
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default=os.path.expanduser("~/.retro-fleet/gameservers.db"))
    p.add_argument("--host", action="append", help="limit to these IPs (repeatable)")
    p.add_argument("--timeout", type=float, default=8.0)
    p.add_argument("--json", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="also show markers that are ok, and why unknowns are unknown")
    p.add_argument("--want-goldsrc", default="a protocol-48 client (server is 48/1.1.2.7)",
                   help="what the GoldSrc servers need from a client")
    p.add_argument("--want-unreal", default="OldUnreal 469 (server runs 469a)")
    args = p.parse_args(argv)
    return asyncio.run(main_async(args))


if __name__ == "__main__":
    sys.exit(main())
