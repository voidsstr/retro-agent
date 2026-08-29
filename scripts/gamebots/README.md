# gamebots — neural bots for the fleet game servers

Phase 0 of [`docs/game-ai-bots-plan.md`](../../docs/game-ai-bots-plan.md):
the harness, the schema, and the measurements that decide whether the rest of
the plan is worth building. **No model, deliberately.** The thing being proven
first is the loop — that a game server can ask an external process what its
bots should do, inside a frame, at the scale we intend to run.

```
  engine adapter (C, in the game server)          policyd (this directory)
  ┌────────────────────────────┐   UDS batch    ┌──────────────────────────┐
  │ observations for every bot │ ──────────────►│ policy.act(batch)        │
  │ apply actions to usercmds  │ ◄──────────────│ one call, all bots        │
  │ fall back to built-in AI   │   actions      └──────────────────────────┘
  └────────────────────────────┘
```

## What's here

| path | what it is |
|---|---|
| `schema.py` | **The single source of truth.** Observation/action layout, wire format, layout hash. Generates the C header |
| `gamebots_schema.h` | **Generated** — what engine adapters compile against. Never edit |
| `policyd.py` | The policy server: one process, holds the model, answers every bot on every server |
| `loadgen.py` | Stands in for the adapters; produces the measurement table below |
| `baseline.py` | Records how good the bots we *already* have are, so later claims mean something |
| `baselines/` | Captured baselines, committed as reference points |

## Quick start

```bash
python3 schema.py --describe            # the field table
python3 schema.py --hash                # 0x468be61b

python3 policyd.py --policy scripted &  # or --policy noop
python3 loadgen.py --sweep              # the measurement table
python3 baseline.py --port 27961 --seconds 300
```

Regenerate the header after any schema change — a test fails if you forget:

```bash
python3 schema.py --emit-header > gamebots_schema.h
```

## Phase 0 results

Measured on this host (Ultra 9 285K, 24 cores, RTX 5090), 2026-08-28.

### The loop is ~5% of the tightest frame budget at 512 bots

`loadgen.py --sweep`, scripted policy, "of tick" is p99 against the **tightest**
budget we actually run (GoldSrc 100 tick = 10 ms):

| servers | bots each | total | Hz | decisions/s | p50 µs | p99 µs | of tick |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 32 | 32 | 30 | 960 | 298 | 388 | 3.88% |
| 1 | 64 | 64 | 30 | 1,920 | 370 | 485 | 4.85% |
| 8 | 32 | 256 | 30 | 7,680 | 175 | 409 | 4.09% |
| 8 | 32 | 256 | 100 | 25,598 | 79 | 389 | 3.89% |
| 8 | 64 | **512** | 30 | 15,360 | 248 | 681 | **6.81%** |

Against Quake III's actual 50 ms budget that worst case is **1.4% of a frame**.

### Where the time goes — and why almost none of it is real

The raw socket is 3.6 µs. The rest decomposes cleanly:

| component | cost | note |
|---|---|---|
| Socket round trip | **3.6 µs** | measured separately; irrelevant at this scale |
| Process wakeup when idle between ticks | **~200 µs** | 32 bots: 65 µs hot vs 262 µs sleeping. Disappears in production, where a server calls every frame and stays hot |
| Python `struct` serialisation | **2.8 µs/bot** | dominates everything else |
| The same bytes as `memcpy` | **0.013–0.067 µs/bot** | what the real C adapter pays: **40–200× cheaper** |
| Policy compute (scripted, pure Python) | ~0 by comparison | no-op and scripted measured within noise of each other |

**So the per-bot cost we measured is almost entirely a pure-Python artifact of
this harness, not a property of the design.** The C adapter memcpys the struct;
the policy server should read the whole batch with one `numpy.frombuffer`
rather than 512 `struct.unpack`s. Both are Phase 1 work, and both make these
numbers better.

### Baseline: the bots we already have

`baselines/q3-botlib-q3dm17.json` — Quake III botlib, 5 bots, q3dm17, 2.5 min:

| | frags/min |
|---|---|
| mean across bots | **1.12** |
| best single bot | **3.20** |

That is the control group. A cloned or self-play policy has to beat it.

## Two things Phase 0 found that change the plan

### 1. The Quake III server had been stuck for hours

`baseline.py`'s first run reported **0.00 frags/min for every bot** — the scores
were identical to ones sampled two hours earlier. The server log showed:

```
broadcast: print "Timelimit hit.\n"
Exit: Timelimit hit.
```

It had hit the time limit and **stayed in intermission**, never advancing to
`nextmap`. The bots had been frozen the whole time, and any player joining would
have landed in a dead match. `rcon vstr nextmap` unstuck it (map advanced to
q3dm17, bots immediately resumed fragging), which is how the baseline above got
captured.

This matters beyond the annoyance: **Phase 2 plans to bootstrap a demonstration
corpus by letting the existing bots play overnight.** That silently produces
hours of "everyone standing still" unless the intermission problem is fixed
first. Recording it here because the number that comes out of a broken server
looks exactly like a number that came out of a working one.

The durable fix belongs with `retro-gameservers-watch`, which already restarts
*dead* servers and could equally detect a *stuck* one (map unchanged and no
score movement while players are connected).

### 2. Hand-counted offsets are a silent-wrong-answer machine

Two bugs in this directory, both found by measurement rather than by reading:

- `_ent_offset()` scanned the 140-entry field table **per entity, per bot, per
  tick** — the first sweep measured it at ~1 ms of serve time at 64 bots.
  Offsets now resolve once at import.
- The scripted policy read `visible` at `base + 7`, which is actually the second
  component of `rel_vel`, so it never fired. A test caught it; in a *trained*
  policy it would have produced no error at all, just a quietly worse model.

Both are now regression-tested. Sub-fields within an entity slot are derived
from the schema **by name**, never written as `base + 7`.

## Design decisions worth keeping

**Batch per server, not across servers.** Each request already carries every bot
on that server. Coalescing across servers would buy GPU efficiency we measurably
do not need (256 bots is ~0.01% of a 5090) at the cost of the one thing we do
care about, latency.

**The schema hash is on the wire.** An adapter built against a stale header is
otherwise undetectable — the floats still unpack, they just mean different
things. The server refuses a mismatch and names the fix.

**The C header is generated, not maintained.** Two hand-written copies of a field
table drift; `agent/shared/` exists in this repo for the same reason.

**The policy server never blocks a game server.** A throwing policy still gets a
null action out to every bot; a malformed request is refused and the connection
dropped, never left hanging. The adapter's own fallback to the engine's built-in
AI covers the gap. A stalled game server is worse than a stupid bot.

**Reserve the intent vector now.** Nothing writes it until Phase 4's LLM planner,
but adding it later would change the layout hash and invalidate every
demonstration recorded before then.

## Tests

```bash
pytest tests/python/test_gamebots_schema.py     # 23
pytest tests/python/test_gamebots_policyd.py    # 21
```

Both run in `tests/run_all.sh` section [1]. The schema tests regenerate the C
header and diff it, and compile it with `-Wall -Wextra -Werror` so its static
asserts verify the C and Python struct layouts agree byte for byte — the one
kind of drift the runtime hash cannot catch, because both sides would agree on
the hash while disagreeing on the bytes.

---

# Phase 1 — the custom model on the 5090

## What's here now

| path | what it is |
|---|---|
| `model.py` | **The policy network.** Ours, shaped by the schema |
| `runtime.py` | GPU serving: batching, CUDA graphs, per-bot recurrent state |
| `planner.py` | The strategic layer — squad intent at ~2 Hz, heuristic or LLM |
| `adapters/gb_client.[ch]` | **The C client every engine adapter links** |
| `adapters/gb_bench.c` | Measures what an adapter really pays |

```bash
~/.venvs/gamebots/bin/python model.py --bench            # the network alone
~/.venvs/gamebots/bin/python runtime.py --bench          # the serving path
~/.venvs/gamebots/bin/python policyd.py --policy gpu --planner heuristic
python3 planner.py --demo --backend llm --model Qwen/Qwen2.5-1.5B-Instruct

gcc -O2 -I adapters -o gb_bench adapters/gb_bench.c adapters/gb_client.c
./gb_bench                                               # what the engine pays
```

## The network

Not a flat MLP over 144 floats — the observation has structure, and a flat net
would have to rediscover it from data we do not have much of:

- **Grouped encoders** so "my health" and "the wall behind me" are not mixed in
  the first matmul.
- **A shared per-entity encoder with masked attention pooling.** An enemy is an
  enemy whichever slot it lands in, and absent slots contribute *nothing* —
  averaging them in would make "two enemies" and "eight distant enemies" look
  alike.
- **A GRU**, because an enemy that ducks behind a wall still exists.
- **FiLM conditioning** for planner intent. Concatenated conditioning is easy
  for a net to zero out and never look at again; a multiplicative gate cannot be
  routed around. Initialised to the identity, so no plan means "as trained".
- **Heads matched to the action space**, plus a value head — the same network
  serves cloning now and PPO in Phase 3 without a rebuild.

671,647 parameters, ~1.5 MFLOPs per decision.

## Measured on the 5090 (torch 2.11+cu128, sm_120)

**The forward pass is launch-bound, not compute-bound** — eager mode costs
~0.44 ms *whatever the batch size*, so a 32-bot server pays what a 1024-bot one
does. CUDA graphs fix that:

| batch | eager | CUDA graph | speedup |
|---:|---:|---:|---:|
| 32 | 0.436 ms | **0.089 ms** | 4.9× |
| 512 | 0.444 ms | **0.118 ms** | 3.8× |
| 1024 | 0.438 ms | **0.146 ms** | 3.0× |

**The real serving path** — request bytes in, response bytes out:

| bots | ms/req | µs/bot | decisions/s | of a 10 ms tick | of a 50 ms frame |
|---:|---:|---:|---:|---:|---:|
| 32 | 0.241 | 7.5 | 132,785 | 2.4% | 0.48% |
| 128 | 0.263 | 2.1 | 486,455 | 2.6% | 0.53% |
| 512 | **0.357** | 0.70 | **1,432,419** | 3.6% | **0.71%** |

**Through the real C client** (what an engine adapter actually pays), 9300
frames, zero fallbacks:

| bots | mean | p99 | µs/bot | of a 50 ms frame |
|---:|---:|---:|---:|---:|
| 4 | 342 µs | 667 µs | 85.4 | 0.68% |
| 64 | 401 µs | 712 µs | 6.3 | 0.80% |
| 256 | **517 µs** | 793 µs | 2.0 | **1.03%** |

Getting from 3.9 ms to 0.36 ms at 512 bots took three fixes, all found by
measuring rather than reading:

1. **Per-bot Python marshalling** was 3.5 ms of the original 3.9 — the GPU was
   only 0.36 ms of it. Now one typed memory view (`numpy.frombuffer`) for the
   whole batch, kept strictly optional so the harness still runs without numpy.
2. **Recurrent state was gathered in a Python loop**, touching the GPU once per
   bot. Now one preallocated state tensor with `index_select`/`index_copy_`.
3. **Cold start.** The first request at each batch size paid CUDA-graph capture
   *and* first-touch allocation in the surrounding code — tens of milliseconds,
   over the frame budget, so the adapter timed out and backed off and the bots
   silently stayed on the engine's own AI. Found by pointing the C client at it:
   9300 frames, 9300 fallbacks, one reconnect. Now the whole path runs once per
   bucket at startup (0.6 s) and the first served frame costs what the ten
   thousandth does.

## The strategic layer

The policy is a reflex layer: aim, strafe, peek, thirty times a second. Plans
come from `planner.py`, which runs **per squad at ~2 Hz** — not per bot per
frame — and emits an intent that becomes the 16-float slot the schema reserved
from the start.

It lives *inside* the policy server, which already sees every bot's observation
every frame, so **engine adapters never learn the planner exists**: they send
zeros and the intent is injected before inference.

| backend | needs | latency |
|---|---|---|
| `heuristic` | nothing | **0.01 ms** |
| `llm` (Qwen2.5-1.5B-Instruct) | transformers + GPU | 419 ms / 4 bots · 729 ms / 8 · 1184 ms / 16 |

**The LLM does not hold 2 Hz beyond about four bots** — output length grows with
squad size. That is acceptable and by design: the planner runs on its own
thread, off the serving path, so exceeding its period lowers the *plan rate*,
never drops a frame, and intent is sticky. A 16-bot squad plans at ~0.8 Hz.

Safety is the same shape throughout: **any failure leaves the bots exactly as
they were.** No plan is zeros, FiLM's identity. The LLM falls back to the
heuristic on load error, timeout or nonsense. A strategic layer must never be
able to make the reflex layer worse.

## Engine coverage — the honest table

`gb_client.c` is the shared half, and it is done: one implementation of the
protocol, batching, timeout and fallback, so **adding an engine means writing
observation extraction and action application and nothing else.**

| engine | our servers | hook | status |
|---|---|---|---|
| **shared client** | all | `gb_client.[ch]` | **done, tested (26 checks), benchmarked** |
| Quake III / OpenArena | Q3A, OA | `qagame.so` — the Debian package ships a **native** game module, so it is a real .so we can replace | next |
| GoldSrc | CS 1.6 ×2, The Specialists | Metamod plugin (`FL_FAKECLIENT`, `pfnRunPlayerMove`) — proven by RealBot/Sandbot; **`cs16-noblood` already runs Metamod+AMXX**, vanilla `cs16-server` does not and needs it installed | after Q3 |
| Quake 2 | 1 | game `.so`; NOT "same shape as Q3" as this table used to say -- see `adapters/quake2/README.md` | **done, tested on a live isolated server** — no built-in bot AI (adapter spawns fake clients itself); needed a second, ENGINE-side patch (`sv_fakeclient_safety.patch`) because a fake client left at `cs_free` overflows a reliable-message buffer and crashes the stock server the first time it picks up an item or dies |
| QuakeWorld | 1 | mvdsv/KTX — needs investigation | later |
| UT99 / UT2004 | 1 each | UnrealScript bots, **no native plugin ABI** | deferred |
| Tribes 2 | 1 (docker) | closed Torque binary, TribesNext encrypts even the info reply | **not viable** |

Six of the ten servers are reachable with the two adapters after this one, which
is the order they are listed in.

## Tests

```bash
pytest tests/python/test_gamebots_schema.py     # 23
pytest tests/python/test_gamebots_policyd.py    # 21
pytest tests/python/test_gamebots_planner.py    # 21
~/.venvs/gamebots/bin/python -m pytest \
       tests/python/test_gamebots_model.py      # 18 (GPU cases skip without CUDA)
```

Plus `tests/native/test_gamebots_client.c` — 26 true-source C checks, built and
run by `tests/run_all.sh` section [2]: NaN reaching a view angle, buffer sizing,
the batch cap, an over-long AF_UNIX path, and the fallback/cooldown behaviour.

The model tests need torch, so they are **skipped** rather than failed on a host
without it — the rest of the suite must keep running with no ML stack at all.

## Not done yet

**The engine adapter.** Everything above the adapter boundary is built and
measured; `qagame_ai.so` is the next piece, and until it exists no bot in an
actual game is being driven by any of this. Phase 0's exit criterion — *a bot
that stands still because a Python process told it to* — is met on the
policy-server side and through the C client, but not yet inside a game server.

**Training.** The model is randomly initialised. It runs, it is fast, and it
emits well-formed actions; it does not play. That is Phase 2 (behavioural
cloning) and Phase 3 (self-play), and the corpus they need does not exist yet —
see the plan's §2.2, still the real critical path.
