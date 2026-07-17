#!/usr/bin/env python3
"""Distributed GBDT across retro boxes (roadmap M7): rows sharded per node,
per-level histogram aggregation over the agent AI transport (GB* verbs).
The brain aggregates node histograms, finds splits (same gain formula and
tie-breaks as the on-device single-node trainer), broadcasts split/leaf
decisions, and keeps the full tree ensemble for local validation scoring.

Usage:
  python3 scripts/retro_ai_gbdt.py --ips 192.168.1.124,192.168.1.143 \
      --rounds 100 --depth 6 --lr 0.1
"""
import argparse
import asyncio
import os
import struct
import sys
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
import numpy as np  # noqa: E402

from client.retro_protocol import RetroConnection  # noqa: E402
from client.retro_ai import RetroAI  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")


class LocalAI:
    """Direct serve-protocol shim for local testing (--local: ips are ports)."""

    def __init__(self, port):
        import socket
        self.s = socket.create_connection(("127.0.0.1", int(port)))

    def _frame(self, p):
        self.s.sendall(struct.pack("<I", len(p)) + p)

    def _recv(self):
        h = b""
        while len(h) < 4:
            h += self.s.recv(4 - len(h))
        ln = struct.unpack("<I", h)[0]
        d = b""
        while len(d) < ln:
            d += self.s.recv(ln - len(d))
        return d[0], d[1:]

    async def raw(self, cmd, timeout=None):
        self._frame(cmd.encode())
        return self._recv()

    async def rawp(self, cmd, payload, timeout=None):
        self._frame(cmd.encode())
        self._frame(payload)
        return self._recv()
T = os.path.join(os.path.dirname(__file__), "..", "tools", "rim", "tabular", "out")


def auc(scores, labels):
    order = np.argsort(scores, kind="mergesort")
    ranks = np.empty(len(scores))
    ranks[order] = np.arange(1, len(scores) + 1)
    # average ties
    s_sorted = scores[order]
    i = 0
    while i < len(scores):
        j = i
        while j + 1 < len(scores) and s_sorted[j + 1] == s_sorted[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2.0 + 1.0
        i = j + 1
    pos = labels == 1
    n_pos, n_neg = pos.sum(), (~pos).sum()
    return (ranks[pos].sum() - n_pos * (n_pos + 1) / 2) / (n_pos * n_neg)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ips", required=True)
    ap.add_argument("--rounds", type=int, default=100)
    ap.add_argument("--depth", type=int, default=6)
    ap.add_argument("--min-child", type=int, default=20)
    ap.add_argument("--lr", type=float, default=0.1)
    ap.add_argument("--lam", type=float, default=1.0)
    ap.add_argument("--local", action="store_true",
                    help="ips are local serve ports (testing)")
    args = ap.parse_args()
    ips = args.ips.split(",")

    X = np.fromfile(f"{T}/adult.features.bin", dtype=np.uint8).reshape(-1, 14)
    y = np.fromfile(f"{T}/adult.labels.bin", dtype=np.uint8)
    n_va = int(len(y) * 0.2)
    n_tr = len(y) - n_va
    Xtr, ytr = X[:n_tr], y[:n_tr]
    Xva, yva = X[n_tr:], y[n_tr:]
    F = X.shape[1]

    conns, ais = [], []
    for ip in ips:
        if args.local:
            ais.append(LocalAI(ip))
            continue
        c = RetroConnection(ip, 9898)
        await c.connect(SECRET, timeout=15.0)
        conns.append(c)
        ais.append(RetroAI(c))

    # shard rows round-robin
    shards = [np.arange(i, n_tr, len(ips)) for i in range(len(ips))]
    for ai, sh, ip in zip(ais, shards, ips):
        payload = Xtr[sh].tobytes() + ytr[sh].tobytes()
        st, d = await ai.rawp(f"GBINIT {len(sh)} {F}", payload, timeout=300)
        assert st != 0xFF, d
        print(f"[{ip}] GBINIT {len(sh)} rows")

    # global base score
    tot_pos, tot_n = 0, 0
    for ai in ais:
        st, d = await ai.raw("GBSUMY")
        kv = dict(p.split("=") for p in d.decode().split())
        tot_pos += int(kv["sum"])
        tot_n += int(kv["n"])
    p = min(max(tot_pos / tot_n, 1e-6), 1 - 1e-6)
    base = float(np.log(p / (1 - p)))
    for ai in ais:
        await ai.raw(f"GBSTART {base:.9g}")
    print(f"base={base:.6f} (pos={tot_pos}/{tot_n})")

    trees = []          # list of dicts node_id -> (feat, thresh, l, r) / leaf
    Fva = np.full(n_va, base)
    t0 = time.time()

    for rnd in range(args.rounds):
        for ai in ais:
            st, d = await ai.raw("GBNEWTREE")
            assert st != 0xFF
        tree = {}
        frontier = [0]
        node_stats = {}      # id -> (G, H) aggregated
        next_id = 1
        for depth in range(args.depth):
            if not frontier:
                break
            fpay = struct.pack("<I", len(frontier)) + \
                np.array(frontier, dtype="<i4").tobytes()
            hists = None
            for ai in ais:
                st, d = await ai.rawp("GBHIST", fpay, timeout=300)
                assert st != 0xFF, d
                raw_f = np.frombuffer(bytes(d), dtype="<f4").reshape(
                    len(frontier), F, 256, 3)
                raw_u = np.frombuffer(bytes(d), dtype="<u4").reshape(
                    len(frontier), F, 256, 3)
                h3 = np.stack([raw_f[..., 0].astype(np.float64),
                               raw_f[..., 1].astype(np.float64),
                               raw_u[..., 2].astype(np.float64)], axis=-1)
                hists = h3 if hists is None else hists + h3
            decisions = []
            new_frontier = []
            for fi, node in enumerate(frontier):
                Hh = hists[fi]                       # [F,256,3]
                G = Hh[0, :, 0].sum()
                Hs = Hh[0, :, 1].sum()
                C = Hh[0, :, 2].sum()
                node_stats[node] = (G, Hs)
                best = (0.0, -1, -1)                 # gain, feat, thresh
                parent = G * G / (Hs + args.lam)
                if C >= 2 * args.min_child:
                    for f in range(F):
                        gl = np.cumsum(Hh[f, :, 0])[:-1]
                        hl = np.cumsum(Hh[f, :, 1])[:-1]
                        cl = np.cumsum(Hh[f, :, 2])[:-1]
                        cr = C - cl
                        ok = (cl >= args.min_child) & (cr >= args.min_child)
                        if not ok.any():
                            continue
                        gr = G - gl
                        hr = Hs - hl
                        gain = gl * gl / (hl + args.lam) + \
                            gr * gr / (hr + args.lam) - parent
                        gain[~ok] = -np.inf
                        b = int(np.argmax(gain))
                        if gain[b] > best[0]:
                            best = (float(gain[b]), f, b)
                if best[1] < 0:
                    tree[node] = ("leaf", float(-G / (Hs + args.lam)))
                    continue
                l_id, r_id = next_id, next_id + 1
                next_id += 2
                tree[node] = ("split", best[1], best[2], l_id, r_id)
                decisions.append((node, best[1], best[2], l_id, r_id))
                new_frontier += [l_id, r_id]
            if decisions:
                dp = struct.pack("<I", len(decisions)) + \
                    np.array(decisions, dtype="<i4").tobytes()
                for ai in ais:
                    st, d = await ai.rawp("GBSPLIT", dp, timeout=120)
                    assert st != 0xFF
            frontier = new_frontier
        # depth cap: leaves for whatever's left in the frontier need stats —
        # fetch one more histogram round for those nodes
        if frontier:
            fpay = struct.pack("<I", len(frontier)) + \
                np.array(frontier, dtype="<i4").tobytes()
            hists = None
            for ai in ais:
                st, d = await ai.rawp("GBHIST", fpay, timeout=300)
                raw_f = np.frombuffer(bytes(d), dtype="<f4").reshape(
                    len(frontier), F, 256, 3)
                h3 = np.stack([raw_f[..., 0].astype(np.float64),
                               raw_f[..., 1].astype(np.float64)], axis=-1)
                hists = h3 if hists is None else hists + h3
            for fi, node in enumerate(frontier):
                G = hists[fi][0, :, 0].sum()
                Hs = hists[fi][0, :, 1].sum()
                tree[node] = ("leaf", float(-G / (Hs + args.lam)))
        leaves = [(nid, v[1]) for nid, v in tree.items() if v[0] == "leaf"]
        lp = struct.pack("<I", len(leaves))
        for nid, val in leaves:
            lp += struct.pack("<if", nid, val)
        for ai in ais:
            st, d = await ai.rawp(f"GBLEAF {args.lr:.9g}", lp, timeout=300)
            assert st != 0xFF
        trees.append(tree)

        # local val update
        def predict_rows(Xr):
            out = np.zeros(len(Xr))
            for i, row in enumerate(Xr):
                node = 0
                while True:
                    t = tree[node]
                    if t[0] == "leaf":
                        out[i] = t[1]
                        break
                    node = t[3] if row[t[1]] <= t[2] else t[4]
            return out
        Fva += args.lr * predict_rows(Xva)
        if (rnd + 1) % 10 == 0:
            pva = 1.0 / (1.0 + np.exp(-Fva))
            ll = -np.mean(np.where(yva == 1, np.log(np.clip(pva, 1e-12, 1)),
                                   np.log(np.clip(1 - pva, 1e-12, 1))))
            print(f"round={rnd+1} val_logloss={ll:.5f} "
                  f"elapsed={time.time()-t0:.0f}s", flush=True)

    pva = 1.0 / (1.0 + np.exp(-Fva))
    a = auc(pva, yva)
    acc = ((pva >= 0.5).astype(int) == yva).mean()
    print(f"distributed_gbdt.val_auc={a:.5f} val_acc={acc:.5f} "
          f"nodes={len(ips)} rounds={args.rounds} "
          f"total_secs={time.time()-t0:.0f}")
    print(f"single_node_reference_auc=0.92159 delta={abs(a-0.92159):.5f} "
          f"{'WITHIN 0.01' if abs(a-0.92159) <= 0.01 else 'OUT OF BAND'}")
    for ai in ais:
        try:
            await ai.raw("GBFREE")
        except Exception:
            pass
    for c in conns:
        await c.close()


asyncio.run(main())
