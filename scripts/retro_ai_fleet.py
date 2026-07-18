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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402

from client.retro_protocol import RetroConnection  # noqa: E402
from client.retro_ai import RetroAI  # noqa: E402
import ai_status_bus as bus  # noqa: E402
import ai_metrics  # noqa: E402

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")
OUT = os.path.join(os.path.dirname(__file__), "..", "tools", "rim", "out")


class Node:
    def __init__(self, ip):
        self.ip = ip
        self.conn = None
        self.ai = None
        self.alive = True
        self.backend = "?"

    async def connect(self):
        self.conn = RetroConnection(self.ip, 9898)
        await self.conn.connect(SECRET, timeout=15.0)
        self.ai = RetroAI(self.conn)
        try:
            hello = await self.ai.hello()
            self.backend = (hello.get("backends") or ["?"])[0]
        except Exception:
            pass

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


async def timed_ntstep(node, X, y, timeout=120):
    t0 = time.time()
    loss, grad = await ntstep(node, X, y, timeout=timeout)
    return loss, grad, (time.time() - t0) * 1000.0


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

    model_name = f"mlp-{'x'.join(args.arch.split(','))}"
    Xtr, ytr, Xte, yte = load_mnist(args.train_n)
    steps_per_epoch = len(ytr) // args.global_batch
    total_steps = steps_per_epoch * args.epochs
    run_id = bus.new_run(
        "dp-train", model=model_name, phase="train", arch=args.arch,
        precision="f32", command=" ".join(sys.argv), nodes=ips)
    bus.publish(run_id, status="running",
                progress={"total_epochs": args.epochs,
                          "total_steps": total_steps},
                fleet={"nodes_total": len(nodes)})

    rng = np.random.RandomState(args.seed)
    gstep = 0
    t0 = time.time()
    allreduce_ms = []
    step_wall_s = []
    final_acc = None

    try:
        for epoch in range(args.epochs):
            order = rng.permutation(len(ytr))
            bstart = 0
            while bstart + args.global_batch <= len(ytr):
                t_step0 = time.time()
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
                    *[timed_ntstep(n, Xtr[s], ytr[s]) for n, s in zip(live, shards)],
                    return_exceptions=True)

                grads, losses, failed = [], [], []
                node_updates = {}
                for n, s, r in zip(live, shards, results):
                    if isinstance(r, Exception):
                        print(f"[{n.ip}] step failed ({r}); dropping node, "
                              f"reassigning shard")
                        n.alive = False
                        failed.append((n, s))
                        node_updates[n.ip] = {
                            "alive": False, "last_error": str(r)[:200]}
                    else:
                        loss, g, step_ms = r
                        losses.append(loss)
                        grads.append((g, len(s)))
                        sps = len(s) / (step_ms / 1000.0) if step_ms > 0 else None
                        node_updates[n.ip] = {
                            "alive": True, "backend": n.backend,
                            "precision": "f32", "last_step_ms": round(step_ms, 1),
                            "samples_per_sec": round(sps, 1) if sps else None,
                            "last_error": None}
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
                        node_updates.setdefault(n.ip, {})["alive"] = False
                allreduce_ms.append((time.time() - t_ar) * 1000)
                step_wall_s.append(time.time() - t_step0)
                gstep += 1

                mean_step_s = float(np.mean(step_wall_s[-20:]))
                eta = max(0.0, (total_steps - gstep) * mean_step_s)
                fleet_sps = (args.global_batch / mean_step_s) if mean_step_s > 0 else None
                bus.publish(
                    run_id,
                    progress={"epoch": epoch + 1, "step": gstep,
                              "percent": round(100.0 * gstep / total_steps, 1)},
                    metrics={"loss": round(float(np.mean(losses)), 4)},
                    fleet={"nodes_alive": len([n for n in nodes if n.alive]),
                           "samples_per_sec": round(fleet_sps, 1) if fleet_sps else None,
                           "eta_seconds": round(eta, 1),
                           "allreduce_ms_avg": round(float(np.mean(allreduce_ms[-50:])), 1)},
                    nodes=node_updates)

                if gstep % 50 == 0:
                    line = (f"epoch={epoch+1} step={gstep} loss={np.mean(losses):.4f} "
                            f"allreduce_ms={np.mean(allreduce_ms[-50:]):.0f} "
                            f"nodes={len([n for n in nodes if n.alive])} "
                            f"elapsed={time.time()-t0:.0f}s")
                    print(line, flush=True)
                    bus.publish(run_id, log_line=line)

            # epoch eval on the first live node
            evaluator = next(n for n in nodes if n.alive)
            st, d = await evaluator.ai.rawp(
                "NTEVAL", step_payload(Xte, yte), timeout=300)
            eval_line = (f"epoch={epoch+1} EVAL[{evaluator.ip}] "
                        f"{d.decode('ascii', 'replace')}")
            print(eval_line, flush=True)
            bus.publish(run_id, log_line=eval_line)

        # weight-sync check: eval on every live node (identical-ish acc)
        for n in nodes:
            if n.alive:
                st, d = await n.ai.rawp("NTEVAL", step_payload(Xte, yte),
                                        timeout=300)
                text = d.decode("ascii", "replace")
                print(f"final EVAL[{n.ip}] {text}")
                if final_acc is None:
                    for tok in text.split():
                        if tok.startswith("acc="):
                            try:
                                final_acc = float(tok.split("=", 1)[1])
                            except ValueError:
                                pass
        if args.export:
            evaluator = next(n for n in nodes if n.alive)
            st, d = await evaluator.ai.raw(f"NTEXPORT {args.export}")
            print(f"export[{evaluator.ip}]: {d.decode('ascii', 'replace')}")
        total_secs = time.time() - t0
        print(f"total_secs={total_secs:.0f} steps={gstep} "
              f"mean_allreduce_ms={np.mean(allreduce_ms):.0f}")

        bus.mark_done(run_id, status="completed")
        try:
            ai_metrics.log_run(
                ips[0], model_name, "train", nodes[0].backend, "f32",
                {"final_acc": final_acc, "total_secs": round(total_secs, 1),
                 "steps": gstep, "mean_allreduce_ms": round(float(np.mean(allreduce_ms)), 1),
                 "nodes": len(nodes)},
                settings={"mode": "distributed", "ips": ips, "arch": args.arch,
                          "epochs": args.epochs, "global_batch": args.global_batch,
                          "lr": args.lr, "momentum": args.momentum},
                dataset="mnist-train", notes="dp-train fleet run")
        except Exception as e:
            print(f"(DB log failed: {e})")
    except Exception as e:
        bus.mark_done(run_id, status="failed", error=str(e)[:500])
        raise
    finally:
        for n in nodes:
            try:
                await n.ai.raw("NTFREE")
            except Exception:
                pass
            await n.close()


async def infer_node_worker(node, idxs, imgs, labels, model, run_id):
    """Sequentially run INFER_RUN over this node's shard of sample indices,
    publishing live per-node throughput every 10 samples. Returns
    (correct, n, latencies_ms). Any exception marks the node dead in the bus
    and propagates so the caller can drop its shard from the aggregate."""
    correct = 0
    lat = []
    t_node0 = time.time()
    for k, i in enumerate(idxs):
        t0 = time.time()
        logits = await node.ai.infer_run(model, imgs[i * 784:(i + 1) * 784])
        lat.append((time.time() - t0) * 1000)
        if int(logits.argmax()) == labels[i]:
            correct += 1
        if (k + 1) % 10 == 0 or k + 1 == len(idxs):
            elapsed = time.time() - t_node0
            sps = (k + 1) / elapsed if elapsed > 0 else None
            bus.publish(run_id, nodes={node.ip: {
                "alive": True, "backend": node.backend, "precision": "i8",
                "samples_per_sec": round(sps, 1) if sps else None,
                "last_step_ms": round(lat[-1], 1), "last_error": None}},
                progress={"step": k + 1})
    return correct, len(idxs), lat


async def dp_infer(args):
    """Data-parallel distributed inference: shard a batch of samples across
    live fleet nodes concurrently (no gradient averaging, unlike dp-train —
    each sample is independent, so this just gathers predictions +
    aggregates throughput). Mirrors dp_train's Node/sharding pattern."""
    ips = args.ips.split(",")
    nodes = [Node(ip) for ip in ips]
    for n in nodes:
        await n.connect()
        print(f"[{n.ip}] backend={n.backend}")

    imgs = open(f"{OUT}/mnist_test_1000.images.bin", "rb").read()
    labels = open(f"{OUT}/mnist_test_1000.labels.bin", "rb").read()
    n = min(args.n, len(labels))

    run_id = bus.new_run(
        "dp-infer", model=args.model, phase="infer_dist", precision="i8",
        command=" ".join(sys.argv), nodes=ips)
    bus.publish(run_id, status="running",
                progress={"total_steps": n},
                fleet={"nodes_total": len(nodes)})

    shards = np.array_split(np.arange(n), len(nodes))
    t0 = time.time()
    results = await asyncio.gather(
        *[infer_node_worker(nd, shard, imgs, labels, args.model, run_id)
          for nd, shard in zip(nodes, shards) if len(shard)],
        return_exceptions=True)

    total_correct = total_n = 0
    alive_nodes = []
    for nd, r in zip([nd for nd, s in zip(nodes, shards) if len(s)], results):
        if isinstance(r, Exception):
            print(f"[{nd.ip}] shard failed: {r}")
            bus.publish(run_id, nodes={nd.ip: {
                "alive": False, "last_error": str(r)[:200]}})
        else:
            correct, cnt, _lat = r
            total_correct += correct
            total_n += cnt
            alive_nodes.append(nd)

    secs = time.time() - t0
    top1 = total_correct / total_n if total_n else 0.0
    ips_total = total_n / secs if secs > 0 else 0.0
    print(f"dp-infer[{','.join(n.ip for n in alive_nodes)}] {args.model}: "
          f"top1={top1:.4f} n={total_n} {ips_total:.1f} img/s (fleet, incl RTT) "
          f"in {secs:.1f}s")

    bus.mark_done(run_id, status="completed")
    bus.publish(run_id, metrics={"top1": round(top1, 4), "n": total_n,
                                 "img_per_sec_total": round(ips_total, 1)})
    try:
        ai_metrics.log_run(
            alive_nodes[0].ip if alive_nodes else ips[0], args.model, "infer",
            alive_nodes[0].backend if alive_nodes else "?", "i8",
            {"top1": round(top1, 4), "n": total_n,
             "img_per_sec_total": round(ips_total, 2),
             "nodes": len(alive_nodes)},
            settings={"mode": "distributed", "ips": ips}, dataset="mnist-test",
            notes="dp-infer fleet run (includes network round-trip)")
    except Exception as e:
        print(f"(DB log failed: {e})")

    for nd in nodes:
        await nd.close()


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

    di = sub.add_parser("dp-infer")
    di.add_argument("--ips", required=True)
    di.add_argument("--model", required=True)
    di.add_argument("--n", type=int, default=200)

    args = ap.parse_args()
    if args.cmd == "dp-train":
        asyncio.run(dp_train(args))
    elif args.cmd == "dp-infer":
        asyncio.run(dp_infer(args))


if __name__ == "__main__":
    main()
