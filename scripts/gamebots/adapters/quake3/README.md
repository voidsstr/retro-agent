# Quake III engine adapter

`qagame_ai.so` — ioquake3's game module with the gamebots adapter compiled in.
A drop-in replacement for `baseq3/qagame.so` that lets the policy server drive
the bots.

**It is inert until `gb_enable 1`.** Installing it changes nothing; a server has
to opt in. Dropping a new brain into a game people play on should require
someone to say so.

## Build

```bash
git clone --depth 1 https://github.com/ioquake/ioq3.git build/ioq3
./build.sh                       # -> out/qagame_ai.so
```

The clone and `out/` are gitignored — third-party source is never committed.
`build.sh` regenerates `gamebots_schema.h` from `schema.py` first, so the
adapter can never be compiled against a stale layout.

## Run

```bash
# 1. a policy server
~/.venvs/gamebots/bin/python ../../policyd.py --policy gpu

# 2. a server with the module (use a SEPARATE instance, not the live one)
cp out/qagame_ai.so <homepath>/baseq3/qagame.so
ioq3ded +set fs_homepath <homepath> +set fs_basepath /usr/lib/ioquake3 \
        +set net_port 27999 +set sv_pure 0 +set vm_game 0 +exec test.cfg

# 3. opt in
rcon gb_enable 1        # gb_debug 1 for per-bot action logging
```

`vm_game 0` is required. Without it the engine prefers `vm/qagame.qvm` out of
`pak8.pk3` and never looks at the native module — it does not warn, it just
loads the stock game.

## How it hooks in

Three lines in `ai_main.c` (recorded as `ai_main.patch`, applied by `build.sh`)
and nothing else:

```c
GB_FrameBegin(time);                    // pack every bot, one exchange
...
    BotUpdateInput(botstates[i], ...);  // botlib fills lastucmd
    GB_ApplyAction(i, ..., &lastucmd);  // we overwrite it, or we do not
    trap_BotUserCommand(...);
```

**The ordering is the fallback design.** botlib still runs and still produces a
complete usercmd, so "the policy did not answer" degrades to the stock bot
rather than to a bot standing still. The fallback is not a path we maintain —
it is the code that was already there.

Keeping the upstream change as a *patch* rather than a forked `ai_main.c` means
an ioq3 update shows up as a patch conflict instead of silently reverting the
hooks.

## Verified on a real server

An isolated `ioq3ded` on :27999 with three bots, same server and same bots
throughout:

| | frags/min |
|---|---|
| our **no-op** policy driving them | **0.00** — all three frozen |
| our policy **off** (`gb_enable 0`, botlib) | 0.33 mean, 1.00 best |

The control is the point. "0.00 frags" is also exactly what a *broken* server
reads — a stuck-in-intermission Quake III server produced that same number
earlier in this project — so freezing the bots only proves something when
turning the policy off makes them fight again.

Server-side, the policy server measured **p50 42 µs, p99 81 µs** serving this
game server's frames.

## What the observation actually contains

Honest about what is filled and what is not:

| group | status |
|---|---|
| health, armour, ammo, weapon, velocity (ego frame), speed, pitch, on-ground, crouching, in-water, alive | **filled** |
| 16 horizontal raycasts + up + down (`trap_Trace`, `MASK_PLAYERSOLID`) | **filled** |
| entity slots: present, teammate, direction (ego frame), distance, relative velocity, health, visibility (`MASK_SHOT` trace) | **filled**, sorted visible-enemies-first then by distance |
| took damage, damage direction, killed someone, died | **filled** (tracked frame to frame; the engine hands us no deltas) |
| round time, score difference, teammates/enemies alive | **filled** |
| `ammo_reserve_frac`, `reloading` | **always zero — Quake III has neither.** Left zero rather than invented, because a policy will learn from whatever it is given |
| `objective` | zero in deathmatch; CTF/flag state is not wired yet |
| `intent` | zero here by design — the policy server injects the planner's vector |

Entity slots hold **players only**. Items, projectiles and powerups are not in
there yet; a bot driven by this cannot see a rocket coming.

## Gotchas found building this

- **`vm_game 0`, or the QVM wins silently.** The engine tries the native module
  only when told to, and says nothing when it falls back.
- **`q_math.c` and `q_shared.c` are not in `basegame.cmake`'s `GAME_SOURCES`.**
  Omitting them produces a `.so` that links cleanly and fails `dlopen` with
  `undefined symbol: vec3_origin` — after which the engine loads the QVM and
  the bots look completely normal. `build.sh` now dlopens the module itself so
  a build that would be ignored fails at build time.
- **The observation is ego-centric on YAW only.** Folding pitch into the body
  frame would make "an enemy above me" and "an enemy ahead while I look up"
  identical.
