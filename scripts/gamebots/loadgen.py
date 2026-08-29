#!/usr/bin/env python3
"""loadgen — answer "how many bots can we actually host?" with a measurement.

Stands in for the engine adapters: opens N connections (each one a pretend
game server), sends a full batch of observations every frame at the engine's
real tick rate, and measures the round trip the adapter would pay.

The number that matters is not throughput, it is **round-trip latency against
the frame budget**. A game server that spends 2 ms waiting for its bots has
lost 20% of a 10 ms GoldSrc tick, and every player feels it. Throughput is
almost irrelevant by comparison -- the GPU is ~0.01% busy at these batch sizes.

So the report is framed as "what fraction of a frame did we spend", and the
verdict is against the tightest budget we actually run, not a comfortable one.

Usage:
    python3 loadgen.py --servers 4 --bots 32 --hz 30 --seconds 10
    python3 loadgen.py --sweep                 # the Phase 0 measurement table
"""

import argparse
import os
import random
import socket
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema           # noqa: E402
import policyd          # noqa: E402

# The budgets from docs/game-ai-bots-plan.md, so the verdict is measured
# against what we really run rather than a flattering number.
BUDGETS = [
    ("Quake III sv_fps 20", 20),
    ("Quake III sv_fps 40", 40),
    ("GoldSrc usercmd 30Hz", 30),
    ("GoldSrc 100 tick", 100),
]
TIGHTEST_MS = 1000.0 / max(hz for _n, hz in BUDGETS)


def synth_obs(rng, bot_id, tick):
    """A plausible observation.

    Realistic *shape* matters more than realistic values: the entity slots are
    partly filled and the rays vary, so a policy actually walks the memory it
    would walk in production instead of short-circuiting on zeros.
    """
    obs = [0.0] * schema.OBS_DIM
    obs[0] = rng.uniform(0.2, 1.0)          # health_frac
    obs[1] = rng.uniform(0.0, 1.0)          # armor
    obs[2] = rng.uniform(0.0, 1.0)          # ammo
    obs[14] = 1.0                            # alive
    ray = _RAY_OFF
    for i in range(schema.NUM_RAYS_H):
        obs[ray + i] = rng.uniform(0.02, 1.0)
    # Half the entity slots occupied, which is about what a 4v4 looks like.
    for e in range(schema.MAX_ENTITIES // 2):
        base = _ENT_OFF[e]
        obs[base] = 1.0
        obs[base + 1] = 1.0 if e % 2 else 0.0
        obs[base + 2] = rng.uniform(-1, 1)
        obs[base + 3] = rng.uniform(-1, 1)
        obs[base + 4] = rng.uniform(-0.3, 0.3)
        obs[base + 5] = rng.uniform(0.01, 1.0)
        obs[base + 8] = rng.uniform(0.0, 1.0)
        obs[base + 9] = 1.0 if rng.random() < 0.6 else 0.0
    return obs


# Resolved once — see the same note in policyd.py. A per-call scan here
# inflates the generator rather than the server, which is worse: it would make
# the system look slower than it is and send us optimising the wrong half.
_OFFSETS = {fname: off for _g, fname, off, _c, _d in schema.FIELD_TABLE}


def _off(name):
    return _OFFSETS[name]


_RAY_OFF = _OFFSETS["ray_h"]
_ENT_OFF = tuple(_OFFSETS[f"e{i}_present"] for i in range(schema.MAX_ENTITIES))


class FakeServer:
    """One pretend game server: a connection plus its roster of bots."""

    def __init__(self, sock_path, n_bots, seed):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(sock_path)
        self.sock.setsockopt(socket.IPPROTO_TCP if False else socket.SOL_SOCKET,
                             socket.SO_SNDBUF, 1 << 20)
        self.n_bots = n_bots
        self.rng = random.Random(seed)
        self.resp_size = schema.HEADER_SIZE + n_bots * schema.ACTION_SIZE

    def tick(self, tick):
        entries = [(i, synth_obs(self.rng, i, tick)) for i in range(self.n_bots)]
        req = schema.pack_request(tick, entries)
        t0 = time.perf_counter_ns()
        self.sock.sendall(req)
        got = 0
        chunks = []
        while got < self.resp_size:
            b = self.sock.recv(self.resp_size - got)
            if not b:
                raise ConnectionError("policy server closed the connection")
            chunks.append(b)
            got += len(b)
        elapsed_us = (time.perf_counter_ns() - t0) / 1000.0
        _t, _f, actions = schema.unpack_response(b"".join(chunks))
        if len(actions) != self.n_bots:
            raise ValueError(f"got {len(actions)} actions for {self.n_bots} bots")
        return elapsed_us

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def run_case(sock_path, n_servers, n_bots, hz, seconds, warmup=0.5):
    servers = [FakeServer(sock_path, n_bots, seed=s) for s in range(n_servers)]
    period = 1.0 / hz
    lat = []
    try:
        # Warm up so first-touch page faults and connection setup do not land
        # in the reported percentiles.
        t_end = time.perf_counter() + warmup
        tick = 0
        while time.perf_counter() < t_end:
            for s in servers:
                s.tick(tick)
            tick += 1

        lat.clear()
        started = time.perf_counter()
        deadline = started + seconds
        next_tick = started
        frames = 0
        while time.perf_counter() < deadline:
            for s in servers:
                lat.append(s.tick(tick))
            tick += 1
            frames += 1
            next_tick += period
            sleep = next_tick - time.perf_counter()
            if sleep > 0:
                time.sleep(sleep)
            else:
                next_tick = time.perf_counter()   # we are behind; do not spiral
        wall = time.perf_counter() - started
    finally:
        for s in servers:
            s.close()

    lat.sort()
    def pct(p):
        return lat[min(len(lat) - 1, int(len(lat) * p))]
    return {
        "servers": n_servers,
        "bots_per_server": n_bots,
        "total_bots": n_servers * n_bots,
        "hz": hz,
        "frames": frames,
        "achieved_hz": frames / wall,
        "decisions_per_sec": n_servers * n_bots * frames / wall,
        "p50_us": pct(0.50),
        "p99_us": pct(0.99),
        "max_us": lat[-1],
        "mean_us": statistics.fmean(lat),
    }


def fmt_row(r):
    frac = r["p99_us"] / 1000.0 / TIGHTEST_MS * 100.0
    return (f"{r['servers']:>4} {r['bots_per_server']:>6} {r['total_bots']:>7} "
            f"{r['hz']:>5} {r['achieved_hz']:>9.1f} "
            f"{r['decisions_per_sec']:>11.0f} "
            f"{r['mean_us']:>8.1f} {r['p50_us']:>8.1f} {r['p99_us']:>8.1f} "
            f"{r['max_us']:>9.1f} {frac:>8.2f}%")


HEADER = (f"{'srv':>4} {'bots':>6} {'total':>7} {'hz':>5} {'achieved':>9} "
          f"{'dec/s':>11} {'mean us':>8} {'p50 us':>8} {'p99 us':>8} "
          f"{'max us':>9} {'of tick':>9}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--socket", default=os.environ.get(
        "GAMEBOTS_SOCKET", policyd.default_socket_path()))
    ap.add_argument("--servers", type=int, default=1)
    ap.add_argument("--bots", type=int, default=16)
    ap.add_argument("--hz", type=float, default=30.0)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--sweep", action="store_true",
                    help="run the Phase 0 measurement table")
    args = ap.parse_args()

    if not os.path.exists(args.socket):
        print(f"no policy server at {args.socket}\n"
              f"start one:  python3 scripts/gamebots/policyd.py --policy scripted",
              file=sys.stderr)
        return 2

    print(f"policy socket: {args.socket}")
    print(f"observation:   {schema.OBS_DIM} floats, "
          f"{schema.OBS_ENTRY_SIZE} B/bot request, "
          f"{schema.ACTION_SIZE} B/bot response")
    print(f"'of tick' is p99 as a fraction of the TIGHTEST budget we run "
          f"({TIGHTEST_MS:.1f} ms, GoldSrc 100 tick)\n")
    print(HEADER)

    if args.sweep:
        cases = [
            (1, 4, 30), (1, 16, 30), (1, 32, 30), (1, 64, 30),
            (2, 32, 30), (4, 32, 30), (8, 32, 30),
            (4, 32, 100), (8, 32, 100),
            (8, 64, 30),
        ]
        results = []
        for ns, nb, hz in cases:
            r = run_case(args.socket, ns, nb, hz, args.seconds)
            results.append(r)
            print(fmt_row(r))
        worst = max(results, key=lambda r: r["p99_us"])
        print()
        print(f"worst p99: {worst['p99_us']:.1f} us at {worst['total_bots']} bots "
              f"across {worst['servers']} servers "
              f"({worst['p99_us']/1000.0/TIGHTEST_MS*100:.2f}% of a "
              f"{TIGHTEST_MS:.0f} ms tick)")
        print("\nframe budgets for reference:")
        for name, hz in BUDGETS:
            print(f"  {name:<24} {1000.0/hz:>6.1f} ms")
    else:
        print(fmt_row(run_case(args.socket, args.servers, args.bots,
                               args.hz, args.seconds)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
