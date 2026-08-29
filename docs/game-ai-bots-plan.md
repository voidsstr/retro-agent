# Neural game bots on the fleet servers — feasibility and plan

**Question:** can we train custom AI models that drive bots on our game servers,
run them on this host's RTX 5090, and host many bots at once?

**Answer: yes, and by a wide margin on the parts people usually worry about —
but the GPU was never the constraint, and the thing that *is* hard is not
compute at all. It is data and integration.** A from-scratch self-play agent of
the DeepMind FTW kind is firmly out of reach; a genuinely strong, human-feeling
bot built on behavioural cloning plus a hierarchical LLM planner is well within
reach.

Everything numeric below was measured on this host on 2026-08-28, not estimated.

---

## 1. What the measurements say

### The GPU is over-provisioned by three orders of magnitude

A control policy for a state-based FPS bot is small — an MLP or a small
transformer over a few hundred features, on the order of **1–5 MFLOPs per
decision**.

| | |
|---|---|
| 64 bots at 30 Hz | 1,920 decisions/s → **~4 GFLOP/s** |
| 256 bots at 30 Hz | 7,680 decisions/s → **~15 GFLOP/s** |
| RTX 5090 (dense fp16, this host) | **~100+ TFLOP/s**, 32 GB VRAM |

That is **~0.01% of the card** for 256 bots. Even a deliberately extravagant
100M-parameter policy at ~200 MFLOPs/step leaves the GPU ~98% idle. The 5090 is
not the limit; it is so far from the limit that the interesting question is what
*else* to spend it on (see the LLM planner, §4.3).

### Round-trip latency is 0.04% of the frame budget

The architecture below puts the policy in a separate process from the game
server, so the round trip has to fit inside one server frame. Measured on this
host with a Unix-domain socket:

| transport | state payload | mean | p50 | p99 |
|---|---|---|---|---|
| SOCK_STREAM | 256 B | 3.6 µs | 3.6 µs | 5.9 µs |
| SOCK_STREAM | 4096 B | 3.9 µs | 3.6 µs | 5.3 µs |
| SOCK_DGRAM | 4096 B | 4.5 µs | 4.0 µs | 7.5 µs |

Against the frame budgets we actually run at:

| engine | frame budget |
|---|---|
| Quake III at `sv_fps 20` (our current setting) | 50.0 ms |
| Quake III at `sv_fps 40` | 25.0 ms |
| GoldSrc usercmd ~30 Hz | 33.3 ms |
| GoldSrc 100 tick | 10.0 ms |

**~4 µs against a 10 ms worst case.** Even batching a whole server's bots into
one call and adding GPU time, we are three orders of magnitude inside budget.
Out-of-process inference is safe; we do not need to embed a runtime in the game
server, and we should not.

### Headless servers are cheap, so training environments are cheap

Measured over an 11-hour window on this host:

| server | CPU | RSS |
|---|---|---|
| `quake3-server` (ioq3ded, 4 bots active) | **1.7%** of one core | 32 MB |
| `openarena-server` | **3.5%** of one core | 32 MB |
| `cs16-server` (hlds_linux) | **1.5%** of one core | 50 MB |

24 cores, load average 1.1. Even assuming a **10× increase** under a full server
of bots at speed, that is ~0.3 core each — so **40–60 concurrent headless
instances** is realistic for training, on this box alone.

This matters more than it looks. The expensive part of DMLab/ViZDoom-style
research is *rendering pixels*. We are driving bots **server-side from game
state**, so there is no renderer in the loop at all. We get the sample
throughput that normally requires a GPU render farm, from CPU cores we already
have idle.

---

## 2. What is actually hard

Three things, in order.

### 2.1 We cannot replicate FTW, and should not try

DeepMind's *Human-level performance in first-person multiplayer games with
population-based deep RL* (the Quake III CTF "FTW" agent) trained a **population
of agents across thousands of parallel matches**. OpenAI Five, for scale, used
**256 GPUs and 128,000 CPU cores**. We have one GPU and 24 cores.

Pure from-scratch self-play to human level is **not feasible here** and any plan
that implies otherwise is dishonest. The good news is that it is also not
necessary: FTW was solving "learn the game from nothing". We already have
decades of hand-built bot navigation, and we can supervise from demonstrations.

### 2.2 We have no demonstration corpus

Behavioural cloning is the highest-value-per-watt approach available to us
(Pearce's *Counter-Strike Deathmatch with Large-Scale Behavioural Cloning*
reached built-in-medium-bot standard from **4M frames** of scraped human play,
and it did that from *pixels*, which is much harder than what we need). But that
dataset does not exist for our servers.

We have to make it, and that is the single biggest schedule risk. Three sources,
in increasing quality:

1. **Existing bots.** Q3's botlib and CS's RealBot/CZ bots already play. Cloning
   them gets a working pipeline end-to-end and a baseline to beat, but caps
   quality at "the bot we already have".
2. **Our own play**, recorded server-side. Highest quality per frame, lowest
   volume — this fleet has a handful of players, not a public server.
3. **Self-play from a cloned start.** Once (1) gives a policy that can move and
   shoot, self-play *improves* it rather than starting from noise. This is where
   the idle CPU cores earn their keep.

The recording harness is therefore **the first thing to build**, before any
model work, because every later phase consumes it.

### 2.3 Each engine needs its own adapter, and they are not equally friendly

| engine | our servers | bot hook | difficulty |
|---|---|---|---|
| **GoldSrc** | CS 1.6 ×2, The Specialists | Metamod plugin; `pfnRunPlayerMove`, `FL_FAKECLIENT`. Proven by RealBot, Sandbot, HPB template | **Low** — and `cs16-noblood` already runs Metamod+AMXX on this host |
| **Quake III** | Q3A, OpenArena | Custom `qagame` module; ioq3 fully open source | **Low–medium** |
| **Quake 2 / QuakeWorld** | 1 each | Open source, same shape as Q3 | Medium |
| **UT99 / UT2004** | 1 each | UnrealScript bots, no native plugin ABI | **High — defer** |
| **Tribes 2** | 1 (docker) | Closed Torque binary, TribesNext encrypts even its info response | **Not viable — exclude** |

Note that vanilla `cs16-server` has **no Metamod** installed while
`cs16-noblood` does — adding it is a prerequisite, not an assumption.

---

## 3. Reference landscape

What exists, and what we take from each:

| project | relevance | what we use |
|---|---|---|
| [google-deepmind/lab](https://github.com/google-deepmind/lab) | 3D RL environment **built on ioquake3** — the same engine as our Q3 server | Precedent that this engine is a viable RL substrate; their map/task tooling |
| [alex-petrenko/sample-factory](https://github.com/alex-petrenko/sample-factory) | Fastest open-source single-machine RL (APPO); ViZDoom + DMLab wrappers; self-play, multiple policies on one GPU | The training backbone — designed for exactly our shape of problem |
| [TeaPearce/Counter-Strike_Behavioural_Cloning](https://github.com/TeaPearce/Counter-Strike_Behavioural_Cloning) | BC agent matching medium built-in AI on CSGO deathmatch, [paper](https://arxiv.org/abs/2104.04258), [dataset](https://huggingface.co/datasets/TeaPearce/CounterStrike_Deathmatch) | The BC recipe and action-space design; proof BC alone is enough to be fun |
| [Fundynamic/RealBot](https://github.com/Fundynamic/RealBot), [Bots-United/HPB-bot](https://github.com/Bots-United/HPB-bot), [tschumann/sandbot](https://github.com/tschumann/sandbot) | Working GoldSrc server-side bots | The *body*: fakeclient plumbing, nav meshes, waypointing. We replace the brain, not the skeleton |
| [Hierarchical Control in Multi-Agent Games: LLM Planning + RL Execution](https://arxiv.org/abs/2606.20014v1) | LLM picks among pretrained RL skills; **statistically tied with a hand-built behaviour tree (46.4% vs 51.5%, p=0.103)** while 60% of a user study found it the most human-like (p=0.027) | The architecture for §4.3 — and a calibrated expectation of what it buys |

**A deliberate finding:** no open-source project drives CS 1.6 or Quake III bots
from an external neural policy server. The pieces all exist; the assembly does
not. We are building, not integrating.

---

## 4. Architecture

```
  game servers (headless, this host)                policy server (GPU)
  ┌──────────────────────────────┐                 ┌────────────────────────┐
  │ hlds_linux  + retrobot.so    │  UDS, ~4 µs     │  batcher (per tick)    │
  │ ioq3ded     + qagame_ai.so   │ ───────────────►│  ↓                     │
  │  · fakeclient bodies         │   obs batch     │  policy net (fp16)     │
  │  · nav mesh, physics, hitreg │ ◄───────────────│  ↓                     │
  └──────────────────────────────┘   action batch  │  action decode         │
              ▲                                    └────────────────────────┘
              │ 2 Hz strategy                                 ▲
  ┌───────────┴──────────────────┐                            │ shared VRAM
  │ LLM planner (squad tactics)  │────────────────────────────┘
  └──────────────────────────────┘
```

### 4.1 Engine-agnostic contract

The one design decision that matters most: **define the observation and action
schema once, engine-independently**, and write thin adapters. Otherwise we build
the same bot four times.

- **Observation** (~200–400 floats): self state (position, velocity, view
  angles, health, armour, weapon, ammo); ray-cast/nav features to nearby
  geometry; up to *k* visible entities (relative position, velocity, team,
  class); recent damage; game-mode features (bomb, flag, round timer).
  Ego-centric and rotation-normalised, so a policy transfers between maps.
- **Action**: continuous view delta (pitch/yaw), discrete movement (forward /
  side / jump / crouch), fire / secondary / reload / use, weapon select.
  This maps cleanly onto GoldSrc's `usercmd_t` and Q3's `usercmd_t` alike —
  which is not a coincidence, they share ancestry.

Version the schema and hash it into the model file so a policy can never be
loaded against a mismatched adapter. (Same lesson as the `.rim` container in
`retro-infer/`.)

### 4.2 Policy server

- One process, holds the GPU, serves all engines over a Unix socket.
- **Batches across every bot on every server per tick** — this is what makes
  hundreds of bots free. One kernel launch for 256 bots, not 256 launches.
- fp16/bf16; TensorRT or `torch.compile` once the architecture settles.
- Hot-reload of weights, so a training run can be promoted without restarting
  game servers.
- **Fails safe:** if the policy server is unreachable or slow, the adapter falls
  back to the engine's built-in bot AI for that frame. A dead policy server must
  degrade the bots, never hang or crash the game server. (This is the same
  discipline as the login-screen dashboard: a stall in the wrong process is
  worse than a wrong answer.)

### 4.3 Hierarchical LLM planner — the "very advanced" part

This is where the spare 5090 actually goes, and where the bots stop feeling like
bots. Following the hierarchical-control result above:

- The **policy net** handles the 30 Hz reflex layer: aim, strafe, peek, reload.
- An **LLM runs at ~2 Hz per squad** (not per bot per frame) and emits
  *intent*: `push B`, `hold long`, `rotate`, `save`, `bait`, plus per-bot
  role assignment and target priority.
- Intent is a small conditioning vector appended to every bot's observation, so
  one policy serves all roles.
- 2 Hz × a few squads is a handful of LLM calls per second — comfortably a
  7–14B model quantised in the 5090's 32 GB, alongside the policy.

Two things this buys that RL alone does not: **team coordination** without a
combinatorial multi-agent training problem, and **legible personality** — bot
"characters" become prompts, not retrained networks. It also plugs straight into
infrastructure we already run: `retro-chat-brain` is already a Claude Agent SDK
service on this host, so **bots can trash-talk in-game chat with the same brain
that answers the fleet chat**.

Calibration, honestly: the cited study found LLM+RL *statistically tied* with a
well-built behaviour tree on win rate. **The win is human-likeness and
authorability, not raw strength.** If we only wanted a hard bot, a good behaviour
tree is cheaper. We want bots that are fun and feel alive — that is exactly what
this architecture is good at.

---

## 5. Phased plan

Each phase is independently useful and independently abandonable.

### Phase 0 — Harness and honest baselines — **DONE (2026-08-28)**
Delivered in `scripts/gamebots/` — see [its README](../scripts/gamebots/README.md)
for the full result tables.

- Schema v1 with a layout hash on the wire, generating the C header adapters
  compile against.
- `policyd` (batching policy server, no-op + scripted policies) and `loadgen`.
- Measured: **512 bots across 8 servers at 30 Hz costs 681 µs p99 — 6.8% of the
  tightest frame budget we run, 1.4% of Quake III's.** The dominant cost is
  Python `struct` serialisation at 2.8 µs/bot, against 0.013–0.067 µs/bot for
  the `memcpy` a C adapter actually pays; the socket itself is 3.6 µs.
- Baseline captured: **Quake III botlib bots manage 1.12 frags/min mean, 3.20
  best** (`scripts/gamebots/baselines/q3-botlib-q3dm17.json`). That is the
  control group Phase 2 has to beat.
- **Exit criterion met** on the policy-server side. The remaining half — a bot
  standing still *inside a game server* — needs the engine adapter, which is
  Phase 1.

**Phase 0 changed two things in this plan.** See §6.

### Phase 1 — One engine, end to end *(medium)*
Start with **Quake III**: fully open source, we control `qagame`, DeepMind Lab
proves the engine works as an RL substrate, and our server already runs bots as
a control group.
- `qagame_ai.so` adapter: extract observations, apply actions, fall back to
  botlib on policy-server failure.
- Policy server with a hand-written scripted policy (no learning yet) to prove
  the loop under load: **32 bots on one server**.
- **Exit criterion:** 32 scripted-policy bots playing a full match, server frame
  time unchanged within noise.

### Phase 2 — Recording and behavioural cloning *(medium–large)*
- Server-side demo recorder writing `(obs, action)` at tick rate — captures
  humans *and* existing bots, same format.
- Bootstrap corpus from existing bots (millions of frames overnight, free).
- Train a BC policy; evaluate head-to-head against botlib.
- **Exit criterion:** the BC policy beats `bot_minplayers` botlib bots at equal
  numbers, and a human says it feels less robotic.

### Phase 3 — Self-play improvement *(large)*
- Sample Factory harness driving **N headless servers** (measured: 40–60 fit on
  this host) with the BC policy as initialisation.
- League/population play to avoid the classic self-play collapse into one
  degenerate strategy.
- **Exit criterion:** self-play policy beats the BC policy over a large match
  sample, with skill tiers extractable from population checkpoints.

### Phase 4 — Hierarchical LLM planner *(medium)*
- Intent vocabulary + conditioning vector; planner service at 2 Hz.
- Bot personalities as prompts; in-game chat via `retro-chat-brain`.
- **Exit criterion:** a blind A/B where players judge which team is human-like.

### Phase 5 — Fleet integration *(small–medium)*
- GoldSrc adapter (`retrobot.so` Metamod plugin) reusing the same schema —
  this is where the engine-agnostic contract pays for itself.
- Install Metamod on vanilla `cs16-server` (prerequisite; `cs16-noblood`
  already has it).
- Difficulty tiers from population checkpoints, so the bots are fun for us and
  not just strong.
- Register with `retro-gameservers-watch` so bot health shows on the login-screen
  status wall alongside the servers themselves.

**Sequencing note:** Phases 0–2 are the ones that prove the concept. Phase 3 is
the expensive one and should not start until Phase 2 has produced a policy worth
improving.

---

## 6. Risks, and what would kill this

| risk | severity | mitigation |
|---|---|---|
| **Demonstration data is too thin** | **High** — this is the real one | Bootstrap from existing bots first; self-play does the heavy lifting. Do not gate Phase 1 on human data |
| **A stuck game server produces hours of useless "demonstrations"** | **High — observed** | Found in Phase 0: the Q3 server had sat in intermission after "Timelimit hit" for hours with every bot frozen, and a scoreboard from a broken server looks exactly like one from a working server. Fix intermission advance and add stuck-detection to `retro-gameservers-watch` **before** any overnight capture run |
| Hand-counted offsets into the observation | Medium — **two found in Phase 0** | Derive every offset from the schema by name. A wrong offset in a *trained* policy produces no error at all, just a quietly worse model |
| Self-play collapses to a degenerate strategy (spawn camping, one weapon) | High | Population/league play, opponent sampling, reward shaping against exploits — the FTW paper's central lesson |
| Policy server becomes a game-server dependency | Medium | Fall back to built-in AI on any failure; never block a server frame |
| Server-frame regression under load | Medium | Batch per tick; measured budget is 3 orders of magnitude of headroom, but measure it in Phase 0 and again in Phase 1 |
| Scope sprawl across six engines | Medium | One engine to working, then port. UT99/UT2004 deferred, Tribes 2 excluded outright |
| Bots that are strong but no fun | Medium | Difficulty tiers from checkpoints; judge on blind human A/B, not win rate |
| Overfitting to `q3dm7` | Low | Ego-centric, rotation-normalised observations; train across the map rotation |

**What would make me stop:** if Phase 1 shows meaningful server frame-time
regression that batching cannot fix, the out-of-process design is wrong and the
whole plan needs rethinking before any model work. That is why Phase 0's exit
criterion is a measurement, not a demo.

---

## 7. Where it lives

```
scripts/gamebots/
  schema/            observation + action schema, versioned, one source of truth
  policyd/           the GPU policy server (batching, hot-reload, fallback)
  record/            demo recorder + dataset tooling
  train/             BC and self-play (Sample Factory harness)
  adapters/
    quake3/          qagame_ai.so
    goldsrc/         retrobot.so (Metamod)
  eval/              head-to-head match runner, skill tracking
docs/game-ai-bots-plan.md    this document
tests/python/test_gamebots_*.py
tests/native/test_gamebots_*.c   schema/packing invariants
```

Conventions this repo already has and this work should inherit: version the
model container and refuse a mismatched load (`retro-infer`'s `.rim`); atomic
publish for anything another process reads (`publish_json`, the dashboard
collector); a regression test per verified fix; and a status file so the thing
shows up on the login-screen wall rather than being invisible.

---

## 8. Bottom line

- **Hosting many bots on the 5090: not in question.** Hundreds of bots is
  ~0.01% of the card, and the IPC is 4 µs against a 10 ms budget. The
  interesting design question is what to do with the *other* 99%, and the answer
  is the LLM planner.
- **A genuinely advanced bot: yes**, via behavioural cloning → self-play →
  hierarchical LLM planning. That is a real architecture with published results
  behind each stage, not a wish.
- **A from-scratch FTW-class agent: no.** That needed thousands of parallel
  matches; we have one box. Saying otherwise would waste months.
- **The critical path is data and integration, not compute** — which is unusual,
  and is the single most important thing to internalise before starting.

The first thing to build is the recorder and the schema, because everything
downstream eats them.
