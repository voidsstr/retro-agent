#!/usr/bin/env python3
"""GPU serving runtime — the custom model, made servable.

Turning a `nn.Module` into something a game server can call every frame needs
three things that the model itself does not provide, and each of them is a
correctness issue rather than an optimisation:

**Per-bot recurrent state.** The policy has a GRU, so every bot carries a hidden
vector between frames. Serving them as one batch means gathering those vectors
in request order and scattering them back afterwards. Get it wrong and bots
inherit each other's memory -- which does not crash, it just produces bots that
behave strangely for reasons nothing logs.

**State resets.** A bot that dies and respawns is a new episode; its memory of
the fight it just lost is worse than useless. The runtime watches the `alive`
flag and zeroes the hidden state on the transition, and drops state entirely
for bots that stop appearing (map change, disconnect) so a long-lived server
does not accumulate hidden vectors forever.

**Fixed shapes, for CUDA graphs.** Measured on this host: eager mode costs
~0.44 ms per call *regardless of batch size* -- it is kernel-launch bound, not
compute bound, so a 32-bot server pays the same as a 1024-bot one. Capturing
the forward pass into a CUDA graph cuts that to 0.09-0.15 ms, a 3-5x win.
Graphs require static shapes, so requests are padded up to the next bucket
(8/16/32/...); the wasted compute is free on a card this idle, and the latency
saved is not.

    python3 runtime.py --bench          # measure the full serving path
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema   # noqa: E402
import model    # noqa: E402

# Batch buckets for CUDA-graph capture. Powers of two up to a whole big server;
# a request is padded to the next one up. More buckets means less padding waste
# but more graphs to capture and more VRAM held for static buffers.
BUCKETS = (8, 16, 32, 64, 128, 256, 512, 1024)

# How many consecutive frames a bot can be absent before we forget its state.
# Generous: a bot missing for one frame is normal (respawn, spectator), a bot
# missing for a hundred has left.
STATE_TTL_FRAMES = 300


def pick_bucket(n):
    for b in BUCKETS:
        if n <= b:
            return b
    return ((n + BUCKETS[-1] - 1) // BUCKETS[-1]) * BUCKETS[-1]


class GpuPolicy:
    """Serves the custom policy on the GPU, with per-bot memory.

    Deliberately NOT a subclass of policyd.Policy at import time: policyd must
    remain importable on a host with no torch (the Phase 0 harness does), so
    the dependency points this way and policyd looks this class up lazily.
    """

    name = "gpu"

    def __init__(self, weights=None, device="cuda", dtype="fp16",
                 use_graphs=True, hidden=256, state_capacity=4096,
                 prewarm=True):
        import torch
        self.torch = torch
        if device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA is not available; pass --device cpu to "
                               "run the model without a GPU (much slower)")
        self.device = torch.device(device)
        self.dtype = {"fp16": torch.float16, "bf16": torch.bfloat16,
                      "fp32": torch.float32}[dtype]
        self.net = model.build(hidden=hidden).to(self.device).to(self.dtype).eval()
        self.weights_path = weights
        self.loaded_version = "untrained"
        if weights:
            self.load(weights)
        self.use_graphs = use_graphs and self.device.type == "cuda"
        self._graphs = {}

        import numpy as np
        self.np = np
        # One preallocated block of hidden state; rows are handed out to bots
        # and reclaimed. Capacity is generous — 4096 rows of 256 fp16 is 2 MB,
        # which is nothing next to a 32 GB card, and running out would mean
        # evicting a live bot's memory mid-fight.
        self.state_capacity = state_capacity
        self._state_buf = torch.zeros(state_capacity, self.net.gru_hidden,
                                      device=self.device, dtype=self.dtype)
        self._slot = {}
        self._free = list(range(state_capacity))
        self._seen = [0] * state_capacity
        self._alive_prev = [False] * state_capacity
        self._alive_off = next(f[2] for f in schema.FIELD_TABLE
                               if f[1] == "alive")
        self._frame = 0
        self.gpu_ms_total = 0.0
        self.calls = 0
        if self.use_graphs and prewarm:
            self.prewarm()

    def prewarm(self, buckets=BUCKETS):
        """Capture every CUDA graph before serving anything.

        Capture is expensive — tens of milliseconds — and lazily capturing on
        first use means the FIRST request at each batch size blows the frame
        budget, the adapter times out, and (because it then backs off) the
        bots stay on the engine's built-in AI for seconds. That is exactly what
        happened the first time the C adapter was pointed at this server, and
        it looks like a broken policy server rather than a warmup cost.

        Paying it once at startup costs about a second and makes every served
        frame the same speed as every other.
        """
        t0 = time.perf_counter()
        for b in buckets:
            self._graph_for(b)
        # Capturing the graph is not enough. The code AROUND it — the host-to
        # -device copy, index_select/index_copy_, the stack, the device-to-host
        # transfer — allocates and initialises on first use too, and that also
        # lands in a game frame. So run the real serving function once per
        # bucket and throw the answer away.
        obs = self.np.zeros((max(buckets), schema.OBS_DIM), dtype=self.np.float32)
        for b in buckets:
            ids = self.np.arange(b, dtype=self.np.uint16)
            self.act_arrays(0, 0, ids, obs[:b], conn_key=-1)
        # Those were not real bots; drop the state rows they claimed.
        for key in [k for k in self._slot if k[0] == -1]:
            self._free.append(self._slot.pop(key))
        self.gpu_ms_total = 0.0
        self.calls = 0
        self._frame = 0
        return time.perf_counter() - t0

    # -- weights ----------------------------------------------------------

    def load(self, path):
        """Hot-loadable weights, so a training run can be promoted without
        restarting a single game server."""
        torch = self.torch
        blob = torch.load(path, map_location=self.device, weights_only=False)
        state = blob.get("state_dict", blob) if isinstance(blob, dict) else blob
        got = blob.get("schema_hash") if isinstance(blob, dict) else None
        if got is not None and got != schema.SCHEMA_HASH:
            # Same discipline as the wire hash: a checkpoint trained against a
            # different observation layout produces a confidently wrong bot,
            # never an error.
            raise ValueError(
                f"checkpoint {path} was trained against schema {got:#010x}, "
                f"this build is {schema.SCHEMA_HASH:#010x} — retrain or "
                f"convert; loading it would silently misread every feature")
        self.net.load_state_dict(state)
        self.net.to(self.dtype).eval()
        self.loaded_version = (blob.get("version") if isinstance(blob, dict)
                               else None) or os.path.basename(path)
        # Captured graphs hold the OLD weights baked in, so they must go — and
        # be recaptured now rather than on the next game frame.
        self._graphs.clear()
        if getattr(self, "use_graphs", False):
            self.prewarm()
        return self.loaded_version

    # -- recurrent state --------------------------------------------------
    #
    # Hidden vectors live in ONE preallocated GPU tensor, with a dict mapping
    # (connection, bot_id) -> row. Gathering is then a single index_select and
    # scattering a single index_copy_, instead of a Python loop that touches
    # the GPU once per bot -- which at 512 bots was costing more than the
    # forward pass it was feeding.

    def _rows_for(self, keys):
        torch = self.torch
        rows = []
        for key in keys:
            row = self._slot.get(key)
            if row is None:
                if not self._free:
                    # Reclaim the least recently seen bot rather than growing
                    # without bound; a server that cycles maps forever would
                    # otherwise leak a row per bot per map.
                    victim = min(self._slot, key=lambda k: self._seen[self._slot[k]])
                    row = self._slot.pop(victim)
                else:
                    row = self._free.pop()
                self._slot[key] = row
                self._state_buf[row].zero_()
                self._alive_prev[row] = False
            rows.append(row)
        return rows

    def _evict(self):
        stale = [k for k, row in self._slot.items()
                 if self._frame - self._seen[row] > STATE_TTL_FRAMES]
        for k in stale:
            row = self._slot.pop(k)
            self._free.append(row)
        return len(stale)

    # -- inference --------------------------------------------------------

    def _graph_for(self, size):
        """Capture (once) a CUDA graph for this padded batch size."""
        torch = self.torch
        if size in self._graphs:
            return self._graphs[size]
        obs = torch.zeros(size, schema.OBS_DIM, device=self.device,
                          dtype=self.dtype)
        hx = torch.zeros(size, self.net.gru_hidden, device=self.device,
                         dtype=self.dtype)
        s = torch.cuda.Stream()
        s.wait_stream(torch.cuda.current_stream())
        with torch.cuda.stream(s):
            for _ in range(3):
                self.net.act(obs, hx)
        torch.cuda.current_stream().wait_stream(s)
        g = torch.cuda.CUDAGraph()
        with torch.cuda.graph(g):
            out, new_hx = self.net.act(obs, hx)
        self._graphs[size] = (g, obs, hx, out, new_hx)
        return self._graphs[size]

    def act_arrays(self, tick, flags, bot_ids, obs, conn_key=0):
        """The serving path. Arrays in, arrays out, no per-bot Python.

        Returns (buttons, pitch, yaw, forward, side, weapon) as numpy arrays,
        already clamped, ready for pack_response_fast.
        """
        torch, np = self.torch, self.np
        n = len(bot_ids)
        if n == 0:
            empty = np.zeros(0, dtype=np.float32)
            return (np.zeros(0, np.uint16), empty, empty, empty, empty,
                    np.zeros(0, np.uint8))
        self._frame += 1

        alive = obs[:, self._alive_off] >= 0.5
        keys = [(conn_key, int(b)) for b in bot_ids]
        rows = self._rows_for(keys)
        idx = torch.as_tensor(rows, dtype=torch.long, device=self.device)

        # A bot that was dead last frame and is alive now has respawned: its
        # memory of the fight it just lost is worse than nothing.
        respawned = alive & ~self._alive_prev_np(rows)
        if respawned.any():
            reset_idx = torch.as_tensor(
                [r for r, rs in zip(rows, respawned) if rs],
                dtype=torch.long, device=self.device)
            self._state_buf.index_fill_(0, reset_idx, 0.0)

        t0 = time.perf_counter()
        obs_t = torch.from_numpy(obs).to(self.device, self.dtype,
                                         non_blocking=True)
        hx_t = self._state_buf.index_select(0, idx)

        if self.use_graphs:
            size = pick_bucket(n)
            g, s_obs, s_hx, s_out, s_hx_new = self._graph_for(size)
            if n < size:
                s_obs[n:].zero_()
                s_hx[n:].zero_()
            s_obs[:n] = obs_t
            s_hx[:n] = hx_t
            g.replay()
            out = s_out
            new_hx = s_hx_new[:n]
        else:
            out, new_hx = self.net.act(obs_t, hx_t)

        self._state_buf.index_copy_(0, idx, new_hx.to(self.dtype))

        # One transfer for every head, stacked, rather than six separate
        # device-to-host syncs.
        cont = torch.stack([out["pitch"][:n], out["yaw"][:n],
                            out["forward"][:n], out["side"][:n]]).float()
        btn_bits = out["buttons"][:n].to(torch.uint8)
        weights = torch.tensor([1 << i for i in range(btn_bits.shape[1])],
                               device=self.device, dtype=torch.int32)
        btn = (btn_bits.to(torch.int32) * weights).sum(dim=1).to(torch.int32)
        wpn = out["weapon"][:n].to(torch.uint8)

        cont_np = cont.cpu().numpy()
        buttons = btn.cpu().numpy().astype(self.np.uint16)
        weapon = wpn.cpu().numpy()
        self.gpu_ms_total += (time.perf_counter() - t0) * 1000
        self.calls += 1

        for r in rows:
            self._seen[r] = self._frame
        self._set_alive_prev(rows, alive)
        if self._frame % 600 == 0:
            self._evict()

        pitch, yaw, fwd, side = (cont_np[0].copy(), cont_np[1].copy(),
                                 cont_np[2].copy(), cont_np[3].copy())
        schema.clamp_actions_inplace(pitch, yaw, fwd, side)
        return buttons, pitch, yaw, fwd, side, weapon

    def _alive_prev_np(self, rows):
        return self.np.array([self._alive_prev[r] for r in rows], dtype=bool)

    def _set_alive_prev(self, rows, alive):
        for r, a in zip(rows, alive):
            self._alive_prev[r] = bool(a)

    def act(self, tick, flags, entries, conn_key=0):
        """Slow path kept for parity testing and for callers without numpy."""
        if not entries:
            return []
        np = self.np
        bot_ids = np.array([b for b, _o in entries], dtype=np.uint16)
        obs = np.ascontiguousarray(
            np.array([o for _b, o in entries], dtype=np.float32))
        buttons, pitch, yaw, fwd, side, weapon = self.act_arrays(
            tick, flags, bot_ids, obs, conn_key)
        return [(int(bot_ids[i]), int(buttons[i]), float(pitch[i]),
                 float(yaw[i]), float(fwd[i]), float(side[i]), int(weapon[i]))
                for i in range(len(entries))]

    def describe(self):
        avg = (self.gpu_ms_total / self.calls) if self.calls else 0.0
        return (f"gpu[{self.loaded_version}] {str(self.dtype).split('.')[-1]}"
                f"{' graphs' if self.use_graphs else ''} "
                f"{avg:.3f}ms/call {len(self._slot)} live states")

    def stats(self):
        return {
            "policy": "gpu",
            "weights": self.loaded_version,
            "dtype": str(self.dtype).split(".")[-1],
            "cuda_graphs": self.use_graphs,
            "graphs_captured": sorted(self._graphs),
            "live_bot_states": len(self._slot),
            "gpu_ms_mean": round(self.gpu_ms_total / self.calls, 4)
            if self.calls else None,
            "params": sum(p.numel() for p in self.net.parameters()),
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--dtype", default="fp16", choices=("fp16", "bf16", "fp32"))
    ap.add_argument("--no-graphs", action="store_true")
    ap.add_argument("--iters", type=int, default=300)
    args = ap.parse_args()

    p = GpuPolicy(dtype=args.dtype, use_graphs=not args.no_graphs)
    print(f"{p.describe()}   params={p.stats()['params']:,}")
    if not args.bench:
        return 0

    # Measure what the SERVER actually does: bytes off a socket -> numpy view
    # -> GPU -> response bytes. Building numpy arrays from Python lists in the
    # benchmark would measure the harness again, which is the mistake Phase 0
    # already made once.
    print(f"\nreal serving path (request bytes -> GPU -> response bytes), "
          f"{'CUDA graphs' if p.use_graphs else 'eager'}, {args.dtype}")
    print(f"{'bots':>6} {'ms/req':>9} {'us/bot':>8} {'decisions/s':>13} "
          f"{'% of 10ms':>10} {'% of 50ms':>10} {'gpu ms':>8}")
    for n in (4, 16, 32, 64, 128, 256, 512):
        entries = [(i, [0.05 * ((i + j) % 20) for j in range(schema.OBS_DIM)])
                   for i in range(n)]
        req = schema.pack_request(0, entries)

        def one(k):
            tick, flags, ids, obs = schema.unpack_request_fast(req)
            btn, pitch, yaw, fwd, side, wpn = p.act_arrays(k, flags, ids, obs)
            return schema.pack_response_fast(tick, ids, btn, pitch, yaw,
                                             fwd, side, wpn)

        for k in range(20):
            one(k)
        gpu0, calls0 = p.gpu_ms_total, p.calls
        t0 = time.perf_counter()
        for k in range(args.iters):
            one(k)
        ms = (time.perf_counter() - t0) / args.iters * 1000
        gpu_ms = (p.gpu_ms_total - gpu0) / max(1, p.calls - calls0)
        print(f"{n:>6} {ms:>9.3f} {ms*1000/n:>8.2f} {n/(ms/1000):>13,.0f} "
              f"{ms/10*100:>9.2f}% {ms/50*100:>9.2f}% {gpu_ms:>8.3f}")
    print(f"\n{p.describe()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
