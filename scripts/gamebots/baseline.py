#!/usr/bin/env python3
"""baseline — measure how good the bots we ALREADY have are.

Phase 0 deliverable. Every later claim ("the cloned policy is better", "self-play
improved it") is meaningless without a number to beat, and that number has to be
captured before we change anything -- afterwards it is gone.

Samples a live server's scoreboard over time and reports frags per minute per
player. Bots are distinguished from humans by **ping 0**, the same tell the
game-server status probe uses: an engine bot has no network path, so it reports
zero, and no real player ever does.

This measures the *engine's own* bots (Quake III botlib, `bot_minplayers 4` on
our server) playing each other. That is exactly the control group Phase 2 needs.

    python3 baseline.py --port 27961 --seconds 300
    python3 baseline.py --port 27961 --seconds 300 --out baseline-q3-botlib.json
"""

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "game-servers"))
import gameservers  # noqa: E402

PLAYER_RE = re.compile(r'^\s*(-?\d+)\s+(-?\d+)\s+"(.*)"\s*$')


def scoreboard(port, host=None, timeout=2.0):
    """(map, [(name, score, ping)]) from a Quake-family status reply.

    Reuses the probe transport from scripts/game-servers/gameservers.py rather
    than opening a second implementation of the same protocol — that module
    already encodes which line the infostring is on for each engine, which is
    the part that is easy to get wrong.
    """
    data, _rtt = gameservers._ask(port, b"\xff\xff\xff\xffgetstatus\n",
                                  timeout, host)
    if not data:
        return None, []
    lines = data[4:].decode("latin-1", "replace").split("\n")
    info = gameservers._infostring(lines[1]) if len(lines) > 1 else {}
    players = []
    for line in lines[2:]:
        line = line.strip().strip("\x00").strip()
        if not line:
            continue
        m = PLAYER_RE.match(line)
        if m:
            players.append((m.group(3), int(m.group(1)), int(m.group(2))))
    return info.get("mapname", "?"), players


def run(port, host, seconds, interval):
    started = time.time()
    first = {}       # name -> first score seen
    last = {}        # name -> latest score
    is_bot = {}
    maps = set()
    samples = 0

    print(f"sampling :{port} every {interval}s for {seconds}s "
          f"(bots identified by ping 0)")
    while time.time() - started < seconds:
        mapname, players = scoreboard(port, host)
        if players:
            samples += 1
            maps.add(mapname)
            for name, score, ping in players:
                if name not in first:
                    first[name] = score
                last[name] = score
                # Once seen with a real ping, never call it a bot again.
                is_bot[name] = is_bot.get(name, True) and ping == 0
        time.sleep(interval)

    elapsed_min = (time.time() - started) / 60.0
    rows = []
    for name in sorted(last):
        gained = last[name] - first[name]
        rows.append({
            "name": name,
            "is_bot": bool(is_bot.get(name, True)),
            "score_start": first[name],
            "score_end": last[name],
            "score_gained": gained,
            "frags_per_min": round(gained / elapsed_min, 2) if elapsed_min else 0,
        })
    bots = [r for r in rows if r["is_bot"]]
    humans = [r for r in rows if not r["is_bot"]]
    return {
        "captured_at": time.time(),
        "port": port,
        "host": host or gameservers.HOST,
        "duration_min": round(elapsed_min, 2),
        "samples": samples,
        "maps": sorted(maps),
        "players": rows,
        "bot_count": len(bots),
        "human_count": len(humans),
        # The headline: what the engine's own bots manage against each other.
        "bot_frags_per_min_mean": round(
            sum(r["frags_per_min"] for r in bots) / len(bots), 2) if bots else None,
        "bot_frags_per_min_max": max(
            (r["frags_per_min"] for r in bots), default=None),
        "human_frags_per_min_mean": round(
            sum(r["frags_per_min"] for r in humans) / len(humans), 2)
            if humans else None,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=27961, help="query port")
    ap.add_argument("--host", default=None)
    ap.add_argument("--seconds", type=float, default=300.0)
    ap.add_argument("--interval", type=float, default=10.0)
    ap.add_argument("--out", help="write the report as JSON")
    args = ap.parse_args()

    report = run(args.port, args.host, args.seconds, args.interval)
    if not report["samples"]:
        print(f"no reply from :{args.port} — is the server up?", file=sys.stderr)
        return 1

    print(f"\nmaps: {', '.join(report['maps'])}   "
          f"{report['samples']} samples over {report['duration_min']:.1f} min\n")
    print(f"{'player':<24} {'kind':<6} {'start':>6} {'end':>6} "
          f"{'gained':>7} {'frags/min':>10}")
    for r in report["players"]:
        print(f"{r['name'][:24]:<24} {'bot' if r['is_bot'] else 'human':<6} "
              f"{r['score_start']:>6} {r['score_end']:>6} "
              f"{r['score_gained']:>7} {r['frags_per_min']:>10.2f}")
    print()
    if report["bot_frags_per_min_mean"] is not None:
        print(f"BASELINE — engine bots: {report['bot_frags_per_min_mean']:.2f} "
              f"frags/min mean, {report['bot_frags_per_min_max']:.2f} best "
              f"({report['bot_count']} bots)")
    if report["human_frags_per_min_mean"] is not None:
        print(f"           humans:      "
              f"{report['human_frags_per_min_mean']:.2f} frags/min mean "
              f"({report['human_count']})")

    if args.out:
        with open(args.out, "w") as fh:
            json.dump(report, fh, indent=2)
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
