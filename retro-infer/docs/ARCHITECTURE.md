# Fleet AI architecture — retro-infer + agent transport + brain

How the pieces fit, where every component lives, and what happens on the wire
for one inference and one training step. Companion docs:
[ALGORITHMS.md](ALGORITHMS.md) (the math), [MODELS.md](MODELS.md) (the zoo),
[TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md) (runbook),
[MAINTENANCE.md](MAINTENANCE.md) (keeping it alive). Milestone history +
acceptance criteria: [`docs/roadmap-fleet-ai.md`](../../docs/roadmap-fleet-ai.md).

## The five layers

```
 (5) metrics      scripts/ai_metrics.py -> ai_runs (specpicks Postgres)
      ^                    every train/infer/bench run, keyed
      |                    model x machine x backend x precision
 (4) brain        scripts/retro_brain_tools.py  (mcp__retro__ai_*)
     orchestration scripts/retro_ai_fleet.py    (data-parallel SGD)
      ^            scripts/retro_ai_gbdt.py     (distributed GBDT)
      |            scripts/retro_ai_pipeline.py (pipeline parallel)
      |            scripts/retro_infer_console.py (ASCII TUI)
      |            client/retro_ai.py           (RetroAI + TNSR codec)
      |  retro-agent TCP :9898 (same framing as every agent command)
 (3) agent        agent/src/ai.c  AI_HELLO/MODEL_LOAD/MODEL_LIST/INFER_RUN/
     transport                    TENSOR/AI_RAW(P)/AI_RESTART
      |  loopback TCP 127.0.0.1:9896, identical [len][status+data] framing
 (2) GPU backends src/gpu/glide_mac.c (3dfx Voodoo, verified)
      ^           src/gpu/nv_gl.c     (GeForce, hardware-pending)
      |
 (1) engine       retro-infer: src/main.c CLI, src/serve.c server,
                  src/exec.c executor, src/ops/* kernels, src/train/* trainers
```

1. **Engine** — one static i586 binary per box
   ([`src/main.c`](../src/main.c), CLI modes at
   [`src/main.c:260`](../src/main.c#L260)). Runtime ISA dispatch:
   [`src/cpuid.c:53`](../src/cpuid.c#L53) reads CPUID feature bits (the binary
   is `-march=i586`, so vectorized code lives in dedicated TUs and is only
   reached through [`src/kernels.c:31`](../src/kernels.c#L31) `kernels_init`).
   The `.rim` executor is [`src/exec.c:317`](../src/exec.c#L317) `model_infer`;
   trainers live in [`src/train/`](../src/train).
2. **GPU backends** — [`src/gpu/glide_mac.c`](../src/gpu/glide_mac.c) turns a
   Voodoo into an exact binary/XNOR MAC array (method in its header comment,
   [`src/gpu/glide_mac.c:1-27`](../src/gpu/glide_mac.c#L1); math in
   [ALGORITHMS.md](ALGORITHMS.md#glide-xnor-gemm)).
   [`src/gpu/nv_gl.c`](../src/gpu/nv_gl.c) mirrors the design for GeForce —
   compile-verified, awaiting hardware (roadmap M6). Both bind their DLLs
   dynamically ([`src/gpu/glide_mac.c:263`](../src/gpu/glide_mac.c#L263)) so
   the engine runs on GPU-less boxes.
3. **Agent transport** — the retro agent proxies AI frames to the engine;
   dispatch table entries at
   [`agent/src/handlers.c:72-80`](../../agent/src/handlers.c#L72), handlers in
   [`agent/src/ai.c`](../../agent/src/ai.c). The UDP discovery beacon carries
   an optional `ai=0/1` field
   ([`agent/src/protocol.c:222`](../../agent/src/protocol.c#L222)) so
   AI-capable boxes are findable without a handshake.
4. **Brain orchestration** — Python coordinators speak the agent protocol via
   [`client/retro_ai.py:79`](../../client/retro_ai.py#L79) `RetroAI` (typed
   wrappers + the `TNSR` tensor codec at
   [`client/retro_ai.py:34`](../../client/retro_ai.py#L34)). The chat brain
   gets `mcp__retro__ai_list/ai_load/ai_infer` from
   [`scripts/retro_brain_tools.py:275`](../../scripts/retro_brain_tools.py#L275).
5. **Metrics** — every run lands in the `ai_runs` table
   ([`scripts/ai_metrics.py:28`](../../scripts/ai_metrics.py#L28) DDL,
   [`scripts/ai_metrics.py:73`](../../scripts/ai_metrics.py#L73) `log_run`,
   leaderboards at [`scripts/ai_metrics.py:93`](../../scripts/ai_metrics.py#L93)).
   DSN comes from the `SPECPICKS_DATABASE_URL` env var.

Per-machine hardware/kernel/driver record (which box uses which path and why):
[`docs/machines/ai-capability-profiles.md`](../../docs/machines/ai-capability-profiles.md).

## Data flow: one remote inference

`mcp__retro__ai_infer` (chat) → agent `INFER_RUN` → engine `INFER` →
executor → f32 logits back up the same path.

```
 chat user          brain (Claude SDK)        agent (fleet box)         engine
    |  "classify #7"     |                        |                        |
    |------------------->| ai_infer tool          |                        |
    |                    | retro_brain_tools.py:368                        |
    |                    |--- INFER_RUN lenet --->| handlers.c:77          |
    |                    |    + input frame       | ai.c:364 handle_infer_run
    |                    |    (784 raw u8 bytes)  |--- "INFER lenet" ----->| serve.c:291
    |                    |                        |    + input frame       | model_infer
    |                    |                        |                        | exec.c:317
    |                    |                        |<-- 0x01 + f32 logits --| serve.c:319
    |                    |<-- frame forwarded ----| ai.c:176 (verbatim)    |
    |<-- "argmax=7" -----|                        |                        |
```

Step by step:

1. The brain tool [`scripts/retro_brain_tools.py:389`](../../scripts/retro_brain_tools.py#L389)
   `ai_infer` slices a sample from a local eval file and sends the two-frame
   `INFER_RUN <name>` (command frame, then raw-input frame) over the agent
   connection — same framing as `UPLOAD`.
2. Agent dispatch ([`agent/src/handlers.c:77`](../../agent/src/handlers.c#L77))
   calls [`agent/src/ai.c:364`](../../agent/src/ai.c#L364) `handle_infer_run`,
   which receives the payload frame and forwards `INFER <name>` + payload to
   the engine via [`agent/src/ai.c:139`](../../agent/src/ai.c#L139)
   `infer_roundtrip`.
3. The engine's `INFER` handler ([`src/serve.c:291`](../src/serve.c#L291))
   looks up the resident model, runs
   [`src/exec.c:317`](../src/exec.c#L317) `model_infer`, and replies
   status `0x01` + `n_classes` f32 LE logits
   ([`src/serve.c:319`](../src/serve.c#L319)). Serve-loaded models skip the
   trailing softmax ([`src/serve.c:246`](../src/serve.c#L246)) so remote logits
   are parity-comparable with `--eval --logits` dumps.
4. The agent forwards the engine reply **verbatim**
   ([`agent/src/ai.c:176`](../../agent/src/ai.c#L176)) — engine reply framing
   is deliberately identical to agent reply framing
   ([`agent/src/ai.c:10-12`](../../agent/src/ai.c#L10)).
5. [`client/retro_ai.py:106`](../../client/retro_ai.py#L106) `infer_run`
   decodes the logits as `<f4`; the brain tool prints argmax + logits.

M4 acceptance: remote logits bit-exact vs local inference and vs the numpy
reference (10/10 on the fleet — commit `22f3f35`).

## Data flow: one fleet training step (data-parallel SGD, M7)

Coordinator: [`scripts/retro_ai_fleet.py:78`](../../scripts/retro_ai_fleet.py#L78)
`dp_train`. Engine session verbs: `NTINIT/NTSTEP/NTAPPLY/NTEVAL/NTEXPORT/NTFREE`
([`src/serve.c:362-504`](../src/serve.c#L362)), implemented by
[`src/train/nn_session.c`](../src/train/nn_session.c). The verbs ride the
agent's generic pass-through `AI_RAW`/`AI_RAWP`
([`agent/src/ai.c:428`](../../agent/src/ai.c#L428)) so new engine verbs need no
agent release.

```
 brain (root)                      node A (.124)            node B (.143)
    | NTINIT arch seed lr mom ------->|------------------------>|   identical
    |   (nn_session.c:42 nns_create)  |  same seed => same W0   |   weights
    | == per step ==                  |                         |
    | shard global batch (fleet.py:104)                         |
    | NTSTEP [B|X|y] --------------- >|  nns_step (:102)        |
    | NTSTEP [B|X|y] ---------------------------------------- >|  fwd+bwd
    |<-- [f32 loss][f32 flat grads] --|<------------------------|
    | weighted-average grads (fleet.py:141-147)                 |
    | NTAPPLY [avg grads] ---------- >|  nns_apply (:216)       |
    | NTAPPLY [avg grads] ----------------------------------- >|  momentum
    |                                 |   same update everywhere => lockstep
```

- Every node `NTINIT`s with the same seed, so initial weights are identical
  ([`src/train/nn_session.c:13-16`](../src/train/nn_session.c#L13)).
- `NTSTEP` payload is `[u32 B][X u8 B*in][y u8 B]`
  ([`src/serve.c:387`](../src/serve.c#L387)); the reply is one f32 loss plus
  the flat gradient vector (`W0,b0,W1,b1,…` row-major,
  [`src/train/nn_session.c:17`](../src/train/nn_session.c#L17)).
- The brain is the allreduce root: it computes the shard-size-weighted average
  ([`scripts/retro_ai_fleet.py:141-147`](../../scripts/retro_ai_fleet.py#L141))
  and broadcasts it with `NTAPPLY`; each node applies the *same* averaged
  gradient with SGD+momentum
  ([`src/train/nn_session.c:216`](../src/train/nn_session.c#L216)), keeping
  weights in lockstep. (With ≥3 nodes the `TENSOR PUT/GET` slots,
  [`src/serve.c:326`](../src/serve.c#L326), support node-to-node ring relay
  instead — see the coordinator docstring.)
- **Failover**: a node that errors mid-step is dropped and its shard is
  re-run on a survivor
  ([`scripts/retro_ai_fleet.py:122-139`](../../scripts/retro_ai_fleet.py#L122));
  `--kill-node/--kill-at-step` injects the failure for the acceptance test.

M7 acceptance: 2-node final eval **bit-identical** on both nodes and vs the
single-node baseline; failover run completes with identical metrics
(commit `c4556e4`).

The other two fleet modes use the same transport: distributed GBDT ships
per-level histograms ([`scripts/retro_ai_gbdt.py`](../../scripts/retro_ai_gbdt.py)
↔ `GB*` verbs, [`src/train/gb_dist.c`](../src/train/gb_dist.c)); pipeline
parallelism streams activations stage-to-stage as `INFER_RUN` outputs
([`scripts/retro_ai_pipeline.py`](../../scripts/retro_ai_pipeline.py)).
Both are covered in [ALGORITHMS.md](ALGORITHMS.md).

## Process model — supervision + crash isolation

The agent **never runs inference in-process**
([`agent/src/ai.c:5-9`](../../agent/src/ai.c#L5)). It supervises a
`retro-infer.exe --serve 9896` child:

- Engine listens on **127.0.0.1:9896 only**
  ([`src/serve.c:644`](../src/serve.c#L644) binds loopback) — it is never
  exposed on the LAN; the agent (port 9898, alt 9897) is the sole doorway.
  9896 was picked because the agent itself owns 9897
  ([`src/main.c:274`](../src/main.c#L274)).
- On the first AI command the agent connects, or spawns the engine from the
  exe staged **next to the agent binary**
  ([`agent/src/ai.c:90`](../../agent/src/ai.c#L90) `infer_spawn`,
  [`agent/src/ai.c:120`](../../agent/src/ai.c#L120) `infer_ensure`,
  8 × 400 ms connect retries).
- Every round-trip retries once through a disconnect+respawn
  ([`agent/src/ai.c:145`](../../agent/src/ai.c#L145)) — a crashed GPU backend
  kills the child, never the agent; the next AI command resurrects it.
- `AI_RESTART` ([`agent/src/ai.c:454`](../../agent/src/ai.c#L454)) is the
  operator's hard-restart for a hung engine (sends `SHUTDOWN`, drops the
  socket, re-ensures).
- At boot, `ai_status_thread`
  ([`agent/src/ai.c:264`](../../agent/src/ai.c#L264), started from
  [`agent/src/main.c:696`](../../agent/src/main.c#L696)) probes/spawns the
  engine and prints an `AI: READY for fleet AI requests` banner (plus host GPU
  + `driver_flag`) on the box's own console and `agent.log`.
- `MODEL_LOAD` stages `.rim` files into `models\<name>.rim` next to the agent
  ([`agent/src/ai.c:335-338`](../../agent/src/ai.c#L335)), then tells the
  engine to `LOAD` from disk — the engine memory-maps nothing; it owns a heap
  copy per resident model ([`src/rim.c:34`](../src/rim.c#L34)).

## Idle resource footprint

Measured directly against the live fleet (2026-07-18), not modeled or estimated.

- **Why it's near-zero by design, not luck.** The agent's only proactive
  engine touch is `ai_status_thread`
  ([`agent/src/ai.c:378-402`](../../agent/src/ai.c#L378)): it fires once,
  `Sleep(3000)` ms after boot ([`:384`](../../agent/src/ai.c#L384)), makes one
  `infer_roundtrip("HELLO", …)` call ([`:388`](../../agent/src/ai.c#L388)),
  and returns ([`:402`](../../agent/src/ai.c#L402)) — there is no periodic
  timer or re-poll loop anywhere in `ai.c`. Once spawned, the engine's own
  main loop ([`src/serve.c:669`](../src/serve.c#L669)) sits in a blocking
  `accept()`/`recv()` between connections
  ([`:672`](../src/serve.c#L672), [`:680`](../src/serve.c#L680)
  `handle_client`) — no busy-wait, no periodic wakeup.
- **CPU: rounds to zero.** WHITEBEAST (`.82`, Ryzen 9950X / RTX 4080 SUPER,
  the fastest box in the fleet) ran 72 minutes idle (started 11:08:44,
  checked 12:20:49) and accumulated `<1s` of CPU time per `tasklist`
  (`CPU Time: 0:00:00`). Every other idle box (.124, .123, .145, .240) showed
  the same `0:00:00` regardless of uptime. `.143` is the one exception — 7s
  accumulated that session — attributable to the dp-train/dp-infer/pipeline
  work actively driven against it, not idle overhead.
- **Memory: 1.6–7 MB resident**, driven more by box RAM headroom than by
  whether a model is loaded:

  | Box | Idle resident memory | Models loaded |
  |---|---|---|
  | .124 (383 MB total RAM) | 1.6 MB | lenet5-int8 |
  | .143 (511 MB total RAM) | 2.9 MB | lenet5-int8 |
  | .240 | 2.0 MB | lenet5-int8 |
  | .123 | 1.66 MB | none |
  | .145 | 1.64 MB | none |
  | .82 (WHITEBEAST) | 6.9 MB | none |

  On the tightest box (.124), 1.6 MB is ~0.4% of total RAM. The agent process
  itself is separately ~1.9 MB resident on .124, for scale — comparable
  overhead (its own 18s of accumulated CPU time there reflects the whole
  session's protocol traffic, not AI-specific cost, so don't read it as an
  AI number).
- **Model residency caveat.** A model staged via `MODEL_LOAD`
  ([`agent/src/ai.c:335-338`](../../agent/src/ai.c#L335)) stays resident in
  the engine's heap until `MODEL_UNLOAD`/`AI_RESTART` — but the table above
  shows this barely moves the needle for a small model like lenet5-int8:
  boxes with a model loaded aren't meaningfully bigger than the zero-model
  boxes (.123/.145), within measurement noise.
- **GPU: zero idle cost.** No Glide/OpenGL context exists until a GPU-backed
  engine command actually runs — and today's live orchestration paths don't
  even reach the GPU backend in the first place; see
  [OPERATIONS.md's Honesty notes](OPERATIONS.md#honesty-notes) for that
  finding.

## Threading & locking

- **Agent side**: the agent is threaded per client, but the engine socket is
  shared state — all AI commands serialize through one lazily-initialized
  critical section
  ([`agent/src/ai.c:29-48`](../../agent/src/ai.c#L29): `g_ai_cs`, init raced
  via `InterlockedCompareExchange`). So AI throughput is one command at a time
  per box, by design.
- **Engine side**: single-threaded, blocking I/O, one client at a time
  ([`src/serve.c:21`](../src/serve.c#L21)) — the agent's serialization
  guarantees that is enough. Resident-model table (8 slots) and tensor slots
  (32) are plain globals ([`src/serve.c:50-66`](../src/serve.c#L50)); one
  NN training session per node ([`src/serve.c:67`](../src/serve.c#L67)).
- **Unrelated agent threads that matter to AI ops**: the watchdog
  ([`agent/src/watchdog.c:97`](../../agent/src/watchdog.c#L97)) recovers the
  box when a command wedges >75 s behind a hung fullscreen Glide game; the
  listen sockets are marked non-inheritable
  ([`agent/src/main.c:604`](../../agent/src/main.c#L604)) so an orphaned
  restart batch can't hold the port. Both are covered in
  [MAINTENANCE.md](MAINTENANCE.md#fleet-gotchas).
- **Determinism note**: single-threaded execution is load-bearing — fixed
  accumulation order is part of the parity contract
  ([`src/ops/gemm_scalar.c:1-5`](../src/ops/gemm_scalar.c#L1), and see
  [ALGORITHMS.md](ALGORITHMS.md#determinism)).
