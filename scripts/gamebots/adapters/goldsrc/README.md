# GoldSrc adapter (`retrobot.so`)

The Metamod plugin that drives fakeclient bots on our CS 1.6 ×2 and The
Specialists servers from the neural-bot policy server. Part of
[`docs/game-ai-bots-plan.md`](../../../../docs/game-ai-bots-plan.md) /
[`scripts/gamebots/README.md`](../../README.md) — read those first; this file
covers only the GoldSrc-specific half.

## Build status — READ THIS BEFORE TRUSTING ANYTHING ELSE HERE

**The engine-independent core (`retrobot_core.c`) is written, built, and
tested — 66/66 checks pass, no HLSDK, no 32-bit toolchain needed.**

**The Metamod glue (`retrobot_engine.cpp`) is written but has never been
compiled, linked, or run.** This dev host has `gcc`/`g++` but not the 32-bit
multilib package (`libc6-dev-i386` / `gcc-multilib`) GoldSrc needs — it's a
32-bit-only engine, no exceptions. Confirmed with `apt-cache policy
libc6-dev-i386`: candidate exists, not installed, and `sudo apt-get install`
needs interactive auth this session did not have (`sudo: interactive
authentication is required`). `apt-get download` (no root needed) pulled the
`.deb`, but the actual blocker is `gcc-multilib`'s 32-bit crt objects and
`libgcc`, not just the header package, and hand-assembling a multilib sysroot
from individually downloaded `.deb`s was judged not worth doing in place of
the honest thing: say so, and leave a `make check-toolchain` target that
tells the next person exactly what's missing:

```bash
cd scripts/gamebots/adapters/goldsrc
make check-toolchain
#   MISSING: cc -m32 cannot build a 32-bit binary on this host. ...
#   Fix (Debian/Ubuntu, needs sudo):
#       sudo apt-get install gcc-multilib g++-multilib libc6-dev-i386
```

That command's failure output (`cannot find Scrt1.o`, `cannot find crti.o`,
`cannot find -lgcc`) is captured verbatim in this repo's history — it is the
exact wall this session hit.

**What that means concretely:**
- Every HLSDK/Metamod API used in `retrobot_engine.cpp` (struct field names,
  function signatures, `DLL_FUNCTIONS`/`META_FUNCTIONS` field ORDER) was
  checked against the actual cloned `build/hlsdk` and `build/metamod-hl1`
  headers and, where possible, against the reference implementation in
  `build/hlsdk/dlls/client.cpp` (e.g. `GetWeaponData`/`UpdateClientData`) —
  not written from memory. Matching the header is necessary, not sufficient.
- It has not been type-checked by a compiler. There will very likely be
  small mistakes (a missing cast, an off-by-one in a struct initializer) that
  only a real build finds. One such mistake (a completely missing
  `GiveFnptrsToDll`/`g_engfuncs` definition — the plugin would have failed to
  link) was caught during writing by re-reading against
  `metamod-hl1/stub_plugin/h_export.cpp`; there may be others like it that
  weren't.
- **Do not deploy this to `cs16-server`, `cs16-noblood`, or
  `specialists-server`** until it has been built and tested against a
  throwaway HLDS instance on a spare port (see "Testing on a throwaway
  server" below). This file's job is to make that easy for whoever has (or
  installs) the 32-bit toolchain.

## What's here

| file | what it is | tested? |
|---|---|---|
| `retrobot_core.h` / `.c` | Engine-independent observation packing: world→ego-centric rotation, threat-sorted entity slots, normalisation/clamping, schema packing via the `GB_OBS_*` macros. Zero HLSDK dependency by design. | **Yes** — `tests/native/test_gamebots_goldsrc.c`, 66 checks, plain `gcc -std=c11` |
| `retrobot_engine.cpp` | The Metamod plugin: `Meta_Query`/`Meta_Attach`, fakeclient creation, the `pfnStartFrame` POST hook that drives the per-tick observe→exchange→act loop, `TraceLine`-based raycasting and visibility, action application via `pfnRunPlayerMove`. | **No** — see "Build status" |
| `Makefile` | Clones HLSDK + metamod-hl1 into `build/` (gitignored), builds `build/retrobot.so`. `make check-toolchain` first. | N/A |
| `build/` | Gitignored. `hlsdk/` (ValveSoftware/halflife) and `metamod-hl1/` (alliedmodders/metamod-hl1), cloned by `make deps`, plus build output. **Never commit anything in here.** | — |

## Building

```bash
cd scripts/gamebots/adapters/goldsrc
make check-toolchain    # fails fast with the apt-get line if you don't have gcc-multilib
make deps               # clones build/hlsdk and build/metamod-hl1 (~50MB, one-time)
make                     # builds build/retrobot.so
```

`make deps` needs network access to GitHub; both repos were reachable and
cloned successfully during this session (`git ls-remote` and `git clone
--depth 1` both worked, ~48MB for HLSDK). Only the *compile* step is blocked
here, not the fetch.

## What's honestly extracted, and what's zero-filled

Every schema field either comes from real, verified HLSDK/engine state, or is
deliberately left at `0.0` with a comment explaining why — never a guess.
Full field-by-field provenance is in the comment above
`rb_build_raw_obs()` in `retrobot_engine.cpp`; summary:

**Real, generic, verified against `progdefs.h`/`const.h`:**
health, armor, position, velocity, view angles, on-ground, crouching, alive
(`deadflag`), team (`entvars_t.team` — see caveat below), other-entity
health/position/velocity, `waterlevel`/`maxspeed` (confirmed these ARE plain
`entvars_t` fields, not mod-private — see `dlls/client.cpp`'s own
`UpdateClientData`, which does nothing but `cd->waterlevel = pev->waterlevel`).

**Real, but depends on an unverified engine behaviour (see below):**
current weapon id and reload flag, via `pfnGetWeaponData`/`pfnUpdateClientData`
POST hooks. The *values* these hooks report are confirmed correct against
`dlls/client.cpp`'s implementation (`item->m_iClip = gun->m_iClip`,
`item->m_fInReload = gun->m_fInReload`, `cd->m_iId = II.iId`) — what's
**unverified** is whether the engine calls these hooks for a fakeclient at
all. They exist to fill network snapshots for a real client's connection;
whether the engine bothers for a bot with nobody to send a snapshot to is
something only a live test can answer. If it doesn't, this degrades exactly
the way "unknown" is supposed to: the cache stays invalid, `weapon_id_max`
stays 0, `rb_norm01()` reads the fraction as 0 (not NaN), and `reloading`
stays 0. **This is the single most important thing to check on a first live
test** — see the checklist below.

**Zero-filled, and why (not available through any generic public API):**
- `ammo_clip_frac` denominator (clip *size*), `ammo_reserve_frac` (both
  numerator and denominator) — CS's backup ammo lives in `CBasePlayer`'s
  private `m_rgAmmo[]`, which needs the mod's own SDK headers. Those were
  deliberately not fetched (this task's brief: don't grab dubious/leaked
  sources for a guess when honest zero is available).
- `damage_dir` — "who hit me from where" needs a `TakeDamage`/`TraceAttack`
  interception this plugin doesn't install. `took_damage` itself IS real
  (computed from the frame-to-frame health delta, which needs no hook at
  all), only the direction is missing.
- `round_time_frac`, `score_diff_norm`, `objective` — CS's round timer/score/
  bomb state live in `CMultiplayGamerules`, privately, with no generic
  dllapi/engine accessor.
- `teammates_alive_frac` / `enemies_alive_frac` **are** filled — computed by
  scanning connected players by `entvars_t.team` each frame, which needs no
  private state.

**Entity sort order:** implemented exactly per the schema's contract —
visible enemies first (nearest first), then visible teammates, then hidden
enemies, then hidden teammates — in `retrobot_core.c:rb_sort_entities_by_threat()`,
tested directly (ties, invalid slots, NaN positions, `n` out of range).

**The `entvars_t.team` caveat:** this is a real, generic HLSDK field, but
whether CS 1.6's game DLL keeps it synced with its own team concept is
mod-specific and not something this session could verify without a live
server. If a live test shows bots treating teammates as enemies (or
vice versa), that's the first thing to check — not evidence the field
extraction is broken.

## The fallback: no engine bot AI exists to fall back to

On `GB_FALLBACK` (no policy server, timeout, schema mismatch — see
`gb_client.h`), a bot **holds its current view angle and stands still**
(`rb_apply_fallback_hold_still()`). This is deliberate, not a stub for a
missing feature: vanilla HLSDK/CS ships **no bot AI of its own**. RealBot,
HPB-bot and Sandbot bring their own navigation/decision code; this plugin
does not vendor any of theirs (they were consulted for the
fakeclient/`pfnRunPlayerMove` mechanics per the task brief, not copied). A
bot that stands still and does nothing is honest and safe; a bot that "falls
back" to code that was never written would be neither.

## Weapon selection is not implemented

`gb_action_t.weapon` (weapon-switch request) is read from the wire but never
applied. Switching weapons in CS needs either a `weapon_<name>` client
command (needs a weapon-id→name table, which is exactly the kind of "public
knowledge, but I can't verify it without hardware" guess this file avoids
making) or private `CBasePlayer` access (see the ammo section above). Left as
a documented no-op rather than a guessed table.

## Bot creation

A server console command, registered in `Meta_Attach`:

```
retrobot_addbot [count]     # default count = 1
```

Each call creates one `pfnCreateFakeClient` bot and, once it's fully spawned
(`pfnClientPutInServer` POST), auto-joins it (`jointeam 5` / `joinclass 5` —
CS 1.6's "auto-assign" convention, the same client commands a real player's
menu sends; this is documented public knowledge from how every CS bot plugin
does it, not from a leaked SDK). Removal isn't wired up to a command yet —
disconnect the bot the normal server-admin way (`kick`) and
`RB_ClientDisconnect_Post` frees its registry slot.

## Testing

**What runs today, on this host, no HLSDK/32-bit toolchain needed:**

```bash
bash tests/run_all.sh
# or directly:
gcc -std=c11 -O0 -g -Wall -I tests/native tests/native/test_gamebots_goldsrc.c -lm \
    -o /tmp/test_gamebots_goldsrc && /tmp/test_gamebots_goldsrc
```

This exercises `retrobot_core.c`'s NaN-safety, the threat-sort (visibility,
distance, teammate/enemy grouping, ties, invalid/out-of-range input), the
frame rotation, and full `rb_build_observation()` round trips including the
reserved intent slot and alignment padding staying zero. It does **not**
exercise `retrobot_engine.cpp` — there is no way to unit-test Metamod glue
without an HLDS process.

### Testing on a throwaway server (required before ANY live deploy)

Per the task brief: **never deploy to `cs16-server`, `cs16-noblood`, or
`specialists-server`** — people play on those. To test for real:

1. Get the 32-bit toolchain (`sudo apt-get install gcc-multilib g++-multilib
   libc6-dev-i386`) and run `make` here until `build/retrobot.so` exists.
2. Stand up a **separate** HLDS instance on a spare UDP port (not 27015/27016,
   whatever `cs16-server`/`cs16-noblood` use — check
   `scripts/game-servers/README.md`), with Metamod installed (copy the
   pattern `cs16-noblood` already uses, per the task brief).
3. Drop `retrobot.so` into that instance's `addons/metamod/dlls/`, register
   it in `metamod`'s `plugins.ini`.
4. Start `policyd.py --policy scripted` (or `noop`) pointed at the default
   socket, `rcon retrobot_addbot 1`, and watch:
   - does the bot appear as a connected player (`status`) and join a team?
   - does `pfnStartFrame`'s loop actually run — add a temporary
     `UTIL_LogPrintf` if needed to confirm `gb_exchange()` returns `GB_OK` and
     not `GB_FALLBACK` every frame;
   - **the weapon-cache question above**: does `weapon_id_norm` read nonzero
     for the bot, or does it stay 0 forever (meaning the engine never called
     `pfnUpdateClientData` for a fakeclient)?
   - does the bot visibly move/aim under a scripted policy, and does
     `retrobot_addbot`'s jointeam/joinclass sequence actually work on CS 1.6
     specifically (it's confirmed elsewhere as a "The Specialists" quirk that
     mods sometimes rename/renumber these — recheck against whichever mod
     the throwaway instance is running before trusting the constant).
5. Only after that passes does deploying to a real server make sense, and
   even then: a separate decision, not implied by this adapter existing.

## Provenance

`build/hlsdk` = `https://github.com/ValveSoftware/halflife` (Valve's official
HLSDK release, non-commercial license per its own `LICENSE`).
`build/metamod-hl1` = `https://github.com/alliedmodders/metamod-hl1` (GPLv2 +
the HL-engine linking exception, see its `GPL.txt`). Neither is committed to
this repo (see `.gitignore`); `make deps` clones them fresh. `retrobot_core.c`
and `retrobot_engine.cpp` are original code written for this task, not derived
from either SDK beyond calling their public interfaces — the "study
RealBot/HPB-bot/Sandbot" instruction in the task brief was read as "learn the
`pfnRunPlayerMove`/fakeclient mechanics", which are documented interface
usage, not as "borrow their source".
