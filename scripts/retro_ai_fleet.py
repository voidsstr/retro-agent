#!/usr/bin/env python3
"""Fleet training coordinator (roadmap M7) — data-parallel SGD across retro
boxes over the agent AI transport (NTINIT/NTSTEP/NTAPPLY via AI_RAW/AI_RAWP,
agent v1.9.1+).

Every node starts from identical weights (same seed); each step the brain
shards the global batch, collects per-node gradients, averages them
(tree-allreduce with the brain as root — with >=3 AI nodes the TENSOR slots
support node-to-node ring relay instead), and broadcasts the averaged
gradient back. Straggler/failover: a node that errors mid-epoch is dropped
and its shard is folded into the survivors; training continues.

Usage:
  dp-train:  python3 scripts/retro_ai_fleet.py dp-train --ips A,B \
      --arch 784,128,10 --epochs 2 --global-batch 128 --train-n 20000
  baseline:  same flags with --ips A (single node)
  failover:  dp-train --kill-node B --kill-at-step 100
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
OUT = os.path.join(os.path.dirname(__file__), "..", "tools", "rim", "out")


class Node:
    def __init__(self, ip):
        self.ip = ip
        self.conn = None
        self.ai = None
        self.alive = True

    async def connect(self):
        self.conn = RetroConnection(self.ip, 9898)
        await self.conn.connect(SECRET, timeout=15.0)
        self.ai = RetroAI(self.conn)

    async def close(self):
        try:
            if self.conn:
                await self.conn.close()
        except Exception:
            pass


def load_mnist(train_n, test_n=2000):
    Xtr = np.fromfile(f"{OUT}/mnist_train.images.bin", dtype=np.uint8)
    Xtr = Xtr.reshape(-1, 784)[:train_n]
    ytr = np.fromfile(f"{OUT}/mnist_train.labels.bin", dtype=np.uint8)[:train_n]
    Xte = np.fromfile(f"{OUT}/mnist_test.images.bin", dtype=np.uint8)
    Xte = Xte.reshape(-1, 784)[:test_n]
    yte = np.fromfile(f"{OUT}/mnist_test.labels.bin", dtype=np.uint8)[:test_n]
    return Xtr, ytr, Xte, yte


def step_payload(X, y):
    return struct.pack("<I", len(y)) + X.tobytes() + y.tobytes()


async def ntstep(node, X, y, timeout=120):
    st, data = await node.ai.rawp("NTSTEP", step_payload(X, y), timeout=timeout)
    if st == 0xFF:
        raise RuntimeError(f"{node.ip} NTSTEP: {data.decode('ascii', 'replace')}")
    arr = np.frombuffer(data, dtype="<f4")
    return float(arr[0]), arr[1:]


async def dp_train(args):
    ips = args.ips.split(",")
    nodes = [Node(ip) for ip in ips]
    for n in nodes:
        await n.connect()
        st, d = await n.ai.raw(
            f"NTINIT {args.arch} {args.seed} {args.lr} {args.momentum}")
        if st == 0xFF:
            raise RuntimeError(f"{n.ip} NTINIT: {d.decode()}")
        print(f"[{n.ip}] NTINIT {d.decode('ascii', 'replace')}")

    Xtr, ytr, Xte, yte = load_mnist(args.train_n)
    rng = np.random.RandomState(args.seed)
    gstep = 0
    t0 = time.time()
    allreduce_ms = []

    for epoch in range(args.epochs):
        order = rng.permutation(len(ytr))
        bstart = 0
        while bstart + args.global_batch <= len(ytr):
            idx = order[bstart:bstart + args.global_batch]
            bstart += args.global_batch
            live = [n for n in nodes if n.alive]
            if not live:
                raise RuntimeError("all nodes dead")
            shards = np.array_split(idx, len(live))

            # optional failover injection
            if (args.kill_node and gstep == args.kill_at_step):
                victim = next((n for n in live if n.ip == args.kill_node), None)
                if victim:
                    print(f"*** failover test: killing engine on {victim.ip} "
                          f"at step {gstep}")
                    try:
                        await victim.conn.command_text(
                            "EXEC taskkill /f /im retro-infer.exe", timeout=30)
                    except Exception:
                        pass

            results = await asyncio.gather(
                *[ntstep(n, Xtr[s], ytr[s]) for n, s in zip(live, shards)],
                return_exceptions=True)

            grads, losses, failed = [], [], []
            for n, s, r in zip(live, shards, results):
                if isinstance(r, Exception):
                    print(f"[{n.ip}] step failed ({r}); dropping node, "
                          f"reassigning shard")
                    n.alive = False
                    failed.append((n, s))
                else:
                    losses.append(r[0])
                    grads.append((r[1], len(s)))
            # reassign failed shards to the first surviving node
            for _, s in failed:
                survivor = next((n for n in nodes if n.alive), None)
                if survivor is None:
                    raise RuntimeError("no survivors for reassignment")
                loss, g = await ntstep(survivor, Xtr[s], ytr[s])
                losses.append(loss)
                grads.append((g, len(s)))

            # weighted average (allreduce root)
            t_ar = time.time()
            total = sum(w for _, w in grads)
            avg = np.zeros_like(grads[0][0])
            for g, w in grads:
                avg += g * (w / total)
            payload = avg.astype("<f4").tobytes()
            live = [n for n in nodes if n.alive]
            acks = await asyncio.gather(
                *[n.ai.rawp("NTAPPLY", payload) for n in live],
                return_exceptions=True)
            for n, r in zip(live, acks):
                if isinstance(r, Exception) or r[0] == 0xFF:
                    print(f"[{n.ip}] NTAPPLY failed; dropping node")
                    n.alive = False
            allreduce_ms.append((time.time() - t_ar) * 1000)
            gstep += 1
            if gstep % 50 == 0:
                print(f"epoch={epoch+1} step={gstep} loss={np.mean(losses):.4f} "
                      f"allreduce_ms={np.mean(allreduce_ms[-50:]):.0f} "
                      f"nodes={len([n for n in nodes if n.alive])} "
                      f"elapsed={time.time()-t0:.0f}s", flush=True)

        # epoch eval on the first live node
        evaluator = next(n for n in nodes if n.alive)
        st, d = await evaluator.ai.rawp(
            "NTEVAL", step_payload(Xte, yte), timeout=300)
        print(f"epoch={epoch+1} EVAL[{evaluator.ip}] "
              f"{d.decode('ascii', 'replace')}", flush=True)

    # weight-sync check: eval on every live node (identical-ish acc)
    for n in nodes:
        if n.alive:
            st, d = await n.ai.rawp("NTEVAL", step_payload(Xte, yte),
                                    timeout=300)
            print(f"final EVAL[{n.ip}] {d.decode('ascii', 'replace')}")
    if args.export:
        evaluator = next(n for n in nodes if n.alive)
        st, d = await evaluator.ai.raw(f"NTEXPORT {args.export}")
        print(f"export[{evaluator.ip}]: {d.decode('ascii', 'replace')}")
    print(f"total_secs={time.time()-t0:.0f} steps={gstep} "
          f"mean_allreduce_ms={np.mean(allreduce_ms):.0f}")
    for n in nodes:
        try:
            await n.ai.raw("NTFREE")
        except Exception:
            pass
        await n.close()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    dp = sub.add_parser("dp-train")
    dp.add_argument("--ips", required=True)
    dp.add_argument("--arch", default="784,128,10")
    dp.add_argument("--epochs", type=int, default=2)
    dp.add_argument("--global-batch", type=int, default=128)
    dp.add_argument("--train-n", type=int, default=20000)
    dp.add_argument("--lr", type=float, default=0.1)
    dp.add_argument("--momentum", type=float, default=0.9)
    dp.add_argument("--seed", type=int, default=42)
    dp.add_argument("--kill-node")
    dp.add_argument("--kill-at-step", type=int, default=-1)
    dp.add_argument("--export")
    args = ap.parse_args()
    asyncio.run(dp_train(args))


if __name__ == "__main__":
    main()
