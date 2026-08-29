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

## Not done yet

Phase 0 deliberately stops before the engine adapter. The exit criterion — *a
bot that stands still because a Python process told it to* — is met on the
policy-server side (`--policy noop` answers every bot with a null action) but
**not yet inside a game server**: that is `qagame_ai.so`, and it is Phase 1.
