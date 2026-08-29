# Quake II engine adapter

`game_ai.so` — Yamagi Quake II's baseq2 game module with the gamebots
adapter compiled in. A drop-in replacement for `baseq2/game.so` that lets
the policy server drive bots.

**It is inert until BOTH `gb_bots` and `gb_enable` are set.** `gb_bots 0`
(default) means the module never touches a client slot at all — a server
with this module installed behaves exactly like stock baseq2. `gb_enable 0`
(default) means any bots that *are* spawned run a tiny built-in fallback
instead of the policy. Installing this changes nothing until someone opts
in, twice over.

## The one big difference from the Quake III adapter, up front

**Quake II's baseq2 ships no bot AI at all.** No botlib, no `SVF_BOT`, no
concept of a fake client anywhere in the source. `docs/game-ai-bots-plan.md`
filed Quake II as "same shape as Q3" before anyone had read its source; that
turned out to be wrong, and finding out was most of the work in this
adapter. Concretely, this adapter has to do two jobs the Quake III one
didn't:

1. **Create the bots.** `gb_bots N` occupies N otherwise-free client slots by
   calling `ClientConnect()` then `ClientBegin()` — the exact same two
   functions the engine calls for a real network connection — directly from
   the game module. There is no separate "spawn a bot" API; this **is** the
   technique every Quake II bot mod back to ACE/Eraser used (confirmed by
   reading ACE's spawn code, `vrgamex86/acesrc/acebot_spawn.c`, elsewhere).
2. **Drive them.** A real client's `usercmd_t` is produced by the network
   layer and consumed by the engine calling `ge->ClientThink()`. Neither
   exists for a fake client, so once a frame, for every bot, `GB_RunFrame()`
   builds a `usercmd_t` itself — from the policy if one answered, otherwise
   from a tiny built-in wander-and-shoot-nothing fallback — and calls
   `ClientThink()` directly.

"Apply your action after the engine's own AI has filled it" becomes "apply
your action after **our own** fallback AI has filled it," because there is
no engine AI to defer to. See "The fallback AI" below for why that
substitution is honest rather than a shortcut, and what it actually does.

## Build

```bash
git clone --depth 1 --branch QUAKE2_8_60 https://github.com/yquake2/yquake2.git build/yquake2
./build.sh                       # -> out/game_ai.so AND out/q2ded_gamebots
```

`QUAKE2_8_60` is the tag matching the installed `yamagi-quake2-core`
`8.60+dfsg-1` Debian package (`New upstream release 8.60` in its
changelog). Unlike ioquake3/Quake III, Yamagi keeps engine and base game in
**one repository** — `src/game/` is the whole of baseq2, there is no
separate `yquake2-baseq2` repo to clone.

The clone and `out/` are gitignored — third-party source is never
committed. `build.sh` regenerates `gamebots_schema.h` from `schema.py`
first, so the adapter can never be compiled against a stale layout.

**Two upstream patches, two artifacts** (see "The crash" below for why
there are two):

| patch | touches | why | output |
|---|---|---|---|
| `g_main.patch` | `src/game/g_main.c` (4 lines: an include, `GB_Init()`, `GB_Shutdown()`, `GB_RunFrame()`) | the usercmd hook | `out/game_ai.so` |
| `sv_fakeclient_safety.patch` | `src/server/sv_send.c`, `src/server/sv_game.c` (one guard each) | **stops a fake client from crashing the server** | `out/q2ded_gamebots` |

Both are applied idempotently (a fresh clone gets patched, an
already-patched tree is left alone) so an upstream update surfaces as a
patch conflict, never a silently reverted hook.

`build.sh` also, in order:
- checks yquake2's `Makefile` `GAME_OBJS_` list against the source list it
  compiles, so an upstream file addition fails the build instead of linking
  a module quietly missing a translation unit;
- `dlopen()`s the finished `game_ai.so` and greps for the exported
  `GetGameAPI` symbol — a module that "builds" and then can't be loaded is
  the worst failure mode, because nothing says so;
- builds `q2ded_gamebots` via yquake2's own Makefile (`make release/q2ded`)
  rather than hand-rolling its source list the way `game_ai.so`'s is —
  the dedicated server pulls in enough of the engine (filesystem, netchan,
  collision) that reimplementing its link line isn't worth it — and checks
  that binary actually runs.

## Run

```bash
# 1. a policy server
~/.venvs/gamebots/bin/python ../../policyd.py --policy noop

# 2. a SEPARATE, isolated server -- see "Gotcha: -datadir does not do what
#    you'd guess" below for why HOME has to be overridden too
HOME=/some/scratch/fakehome build/yquake2/release/q2ded \
    -datadir /some/scratch/q2test +set port 27911 +exec test.cfg

# 3. opt in, from rcon or the server console
gb_bots 4          # spawn 4 fake-client bots
gb_enable 1        # let the policy drive them (gb_debug 1 for per-bot logging)
```

**Use `q2ded_gamebots`, never the systemwide `q2ded`, for anything with
`gb_bots` above 0.** The systemwide binary has none of the crash fix below.

## Verified on a real server

An isolated `q2ded_gamebots` on :27911, `q2dm1`, 4 fake-client bots, same
server throughout, `gb_debug 1` logging every 2 seconds:

| state | bot behaviour (from the live log) |
|---|---|
| `gb_bots 4`, `gb_enable 0` (fallback) | `fwd+400 side+0 up+0 btn0x00 (fallback)` — all four walking |
| `gb_enable 1`, our **noop** policy driving them | `fwd+0 side+0 up+0 btn0x00 (policy)` — all four **frozen** |
| `gb_enable 0` again | `fwd+400 side+0 up+0 btn0x00 (fallback)` — all four **resume** walking |

The control is the point, same as the Quake III adapter's: "0.00 fwd" is
also what a policy server that was never reached looks like. Turning the
adapter off and watching the bots move again is what makes "they froze"
mean the policy answered, not that something upstream was broken —
confirmed here by `policyd`'s own log, which shows the adapter connecting
and an evenly climbing request count across the whole run:

```
[16:36:23] policyd: 121 req, 1 bot-decisions/s, p50 30.8us p99 58.1us, 1 adapter(s)
[16:36:53] policyd: 281 req, 2 bot-decisions/s, p50 31.2us p99 58.1us, 1 adapter(s)
```

`gb_bots 0` cleanly disconnects every bot (`ClientDisconnect()`, the same
call a real player's quit takes) and the server keeps running with zero
players. The whole sequence — spawn 4, run the fallback ~40s through
several drown-and-respawn cycles, enable/disable the policy twice, despawn
to 0 — ran on one server process without a restart.

## The crash, and why there are two patches

**The first version of this adapter crashed the test server within about 20
seconds of the first bot dying**, with:

```
gb_bot_2 sank like a rock.
==== ShutdownGame ====
Error: SZ_GetSpace: overflow without allowoverflow set
```

Root cause, traced through the engine source: a fake client spawned via
`ClientConnect()`/`ClientBegin()` never goes through the network handshake,
so its `client_t.state` (the ENGINE's per-connection struct, `svs.clients[]`
— entirely separate from and invisible to the game module's own
`gclient_t`/`game.clients[]`) sits at `cs_free` forever.
`SV_SendClientMessages()` already knows to skip `cs_free` clients when
flushing per-client buffers each frame — but two other functions write into
those buffers with **no state check at all**:

- `PF_Unicast()` (`sv_game.c`, backs `gi.unicast()` — used by
  `gi.centerprintf()` and directly by game code) writes straight into
  `client->netchan.message` or `client->datagram`.
- `SV_ClientPrintf()` (`sv_send.c`, backs `gi.cprintf()` directly, bypassing
  `PF_Unicast` entirely) writes straight into `cl->netchan.message`.

Stock baseq2 calls `gi.cprintf()` for **every item pickup** and
`gi.centerprintf()` for things like the death/respawn flow. None of that is
bot-specific — a fake client doing anything a normal player does (walking
over a shell, dying, respawning) queues a message that
`SV_SendClientMessages()` will never send and therefore never clears. It
accumulates, frame after frame, until `SZ_GetSpace()` hits its hard
`ERR_FATAL` and the whole server goes down. Note that `gi.bprintf()`
(`SV_BroadcastPrintf`) and the general `gi.multicast()` (`SV_Multicast`)
were already checked against `cs_free`/`cs_spawned` in the stock source —
only these two direct, per-client writers were missing the guard.

**This means a fake-client bot WILL eventually crash a stock Quake II
server, no matter how careful the game-side AI is** — it is not a bug in
this adapter's logic, it is a gap in the 2001-era engine that nothing
running bots through the front door (a real, if automated, network client)
would ever hit. `sv_fakeclient_safety.patch` adds a one-line guard to each
function (skip if `state == cs_free`) and was verified to fix it: the same
4-bot, drown-and-respawn test that crashed the server in under 20 seconds
ran for 40+ seconds through multiple deaths with the patch applied, with no
crash, right up until bots were deliberately despawned.

Because the fix has to live in the **server**, not the game module, it
cannot ship as part of `game_ai.so` the way the usercmd hook does — Quake
II's engine binary needs patching too. That is `q2ded_gamebots`. **Nothing
about this fix is required to build or load `game_ai.so`** — a server
running the stock `q2ded` will load the module, print its banner, and (with
`gb_bots 0`) behave identically to vanilla baseq2. The fix only matters the
moment `gb_bots` goes above zero, at which point the stock engine is not
safe to use.

## What the observation actually contains

Same honesty rule as the Quake III adapter: filled where the engine
genuinely gives us something, zero and documented where it does not.

| group | status |
|---|---|
| health, weapon, velocity (ego frame), speed, pitch, on-ground, crouching, in-water, alive | **filled** |
| armour (`ArmorIndex()` — whichever of jacket/combat/body armour is currently held) | **filled** |
| ammo (current weapon's ammo pool via `client->ammo_index`) | **filled** |
| 16 horizontal raycasts + up + down (`gi.trace`, `MASK_PLAYERSOLID`) | **filled** |
| entity slots: present, direction (ego frame), distance, relative velocity, health, visibility (`gi.trace`, `MASK_SHOT`) | **filled**, sorted visible-first then by distance |
| took damage, killed someone, died | **filled** (tracked frame to frame; the engine hands us no deltas, same as Quake III) |
| round time, score difference, enemies alive | **filled** |
| `ammo_reserve_frac`, `reloading` | **always zero.** Quake II has one ammo pool per weapon and no reload, same reasoning the Quake III adapter gives for the same missing mechanic |
| `is_teammate`, `teammates_alive_frac` | **always zero.** Vanilla baseq2 deathmatch (the module this server runs) has no team mode at all — that needs the separate `ctf` mod, which is not what's installed |
| `damage_dir` | **always zero.** Quake II accumulates the point of impact in `client->damage_from` for the screen-flash effect, but `p_view.c`'s `ClientEndServerFrame()` clears it at the tail of the SAME `G_RunFrame()` our hook starts — one statement before our hook runs again next frame. Reading it earlier would mean a second patch site inside combat code, out of scope for a "hook the usercmd" patch. Quake II also has no persistent last-attacker field for *player* targets — only monsters get one (`ent->enemy`, set in `g_combat.c`'s `M_ReactToDamage`) — so there is nothing to fill this with even if the timing worked out |
| `objective` | zero — vanilla baseq2 has no CTF/bomb mode |
| `intent` | zero here by design — the policy server injects the planner's vector |

Entity slots hold **players only** (same as Quake III). Items and
projectiles are not in there; a bot driven by this cannot see a rocket
coming, and does not know an armour shard is nearby except by tripping over
it.

## The fallback AI, and why respawning is handled outside the policy

When no policy has answered for a bot (either `gb_enable 0`, or the policy
server is unreachable/times out), `GB_FallbackCmd()` walks the bot forward
in a slow circle and does nothing else. **This is not a real bot AI** —
Quake II ships none to fall back to — it exists purely so a disabled or
unreachable policy does not leave a bot standing dead still, and so the
on/off control experiment above has something to show for "off". Do not
read anything into its win rate; it has none.

Both the fallback and the policy path independently press `attack` once a
dead bot is past `client->respawn_time` — this is deliberately **not** left
for the policy to learn. Nothing else drives a fake client's input, so
without this a dead bot (fallback OR an unanswering policy) would sit at
the death screen forever, the way no real human would. This is boilerplate
in the same sense Quake III's botlib-drives-the-fallback is boilerplate:
housekeeping the adapter owns, not something worth spending the policy's
capacity on.

## Only three buttons do anything in vanilla baseq2

`GB_BTN_ATTACK`, `GB_BTN_JUMP` and `GB_BTN_CROUCH` map onto real
`usercmd_t` behaviour (`BUTTON_ATTACK`, and `upmove` above/below the
±10 jump/duck thresholds `common/pmove.c` checks). `GB_BTN_ATTACK2`,
`GB_BTN_RELOAD`, `GB_BTN_WALK` and `GB_BTN_ZOOM` have **no representation at
all** in Quake II's `usercmd_t` or stock weapon code — no secondary fire, no
reload, no walk-toggle bit, no zoom in the base weapon set. `GB_BTN_USE` is
forwarded to `BUTTON_USE`, which exists on the wire but — confirmed by
grepping the whole baseq2 source — **is never read anywhere in vanilla
baseq2**; it is reserved for mods. None of this is invented behaviour: the
schema's action space is shared across engines on purpose, and Quake II
simply doesn't use all of it.

## Other gotchas found building this

- **`-datadir` does not put your files ahead of the systemwide install.**
  `FS_BuildRawPath()`'s search order is `$HOME/.yq2/baseq2` →
  `Sys_GetBinaryDir()` → `-datadir` → `SYSTEMDIR` (`/usr/lib/yamagi-quake2`,
  compiled in via `WITH_SYSTEMWIDE`). A `game.so` placed in `-datadir`'s
  `baseq2/` is checked **after** the systemwide one, which the packaged
  `quake2-server` unit also uses — so a `-datadir`-only test server quietly
  loads the production engine's stock `game.so`, not yours, and nothing
  says so. Confirmed with the engine's own `rcon path` command. The fix
  used here: run the test server with `HOME` pointed at a private scratch
  directory and put `game.so` in `$FAKE_HOME/.yq2/baseq2/` — homedir is
  checked *first*, and overriding `HOME` for one test process cannot touch
  the real `~/.yq2` the live server's `quake2-server` unit shares. **Never**
  place a test `game.so` under the real `~/.yq2` or under
  `/usr/lib/yamagi-quake2` — both are shared with the live server.
- **`rcon status`/`players` will never show a fake-client bot.** Those
  commands read the ENGINE's `svs.clients[]` (network state), and a fake
  client never leaves `cs_free` there — it is entirely a `game.so`-side
  citizen (`game.clients[]`/`g_edicts[]`). Use `gb_debug 1`'s per-frame log
  line, or the server's own broadcast text (`gb_bot_N connected` /
  `entered the game`, printed by the exact same `ClientConnect()`/
  `ClientBegin()` a real player triggers), to confirm a bot exists.
- **`world` is a macro** (`#define world (&g_edicts[0])`,
  `header/local.h`) — a parameter or local variable named `world` fails to
  compile with a baffling error pointing at the macro body, not your code.
  `GB_ToLocal`'s third vector argument is named `worldvec` for this reason.
- **`savegame.c` `#error`s out without `-DYQ2OSTYPE`/`-DYQ2ARCH`** — the
  cross-platform-savegame check the real Makefile normally supplies via
  `uname`. `build.sh` computes and passes both.
- **`sv_fps` is not a pre-existing engine cvar** on this build — querying it
  before anything creates it returns empty over rcon. `gi.cvar()` creates it
  with the given default if missing (same as any other `Cvar_Get`), so
  `GB_Init()`'s `gi.cvar("sv_fps", "10", 0)` is safe either way; `usercmd_t`
  msec is derived from whatever it resolves to.
