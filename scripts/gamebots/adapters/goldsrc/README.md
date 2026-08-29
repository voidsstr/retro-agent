# GoldSrc adapter (`retrobot.so`)

The Metamod plugin that drives fakeclient bots on our CS 1.6 ×2 and The
Specialists servers from the neural-bot policy server. Part of
[`docs/game-ai-bots-plan.md`](../../../../docs/game-ai-bots-plan.md) /
[`scripts/gamebots/README.md`](../../README.md) — read those first; this file
covers only the GoldSrc-specific half.

## Build status

**It builds, links, and dlopens cleanly as a 32-bit `.so` with no undefined
symbols, and its Metamod entry points work.** Verified this session:

```
$ file build/retrobot.so
build/retrobot.so: ELF 32-bit LSB shared object, Intel i386, ...
$ nm -D --defined-only build/retrobot.so | grep -E 'Meta_|GiveFnptrs'
... T GetEntityAPI2_Post
... T GiveFnptrsToDll
... T Meta_Attach
... T Meta_Detach
... T Meta_Query
$ ./dlopen_smoke_test build/retrobot.so     # a tiny standalone dlopen()+dlsym() harness
dlopen OK
Meta_Query: found
Meta_Attach: found
Meta_Detach: found
GiveFnptrsToDll: found
GetEntityAPI2_Post: found
$ ./meta_query_smoke_test build/retrobot.so  # calls the real Meta_Query()
Meta_Query returned 1
ifvers=5:13
name=RetroBot GoldSrc Adapter
version=0.1
loadable=1 unloadable=4
```

`retrobot_core.c` (the engine-independent half) was already built and tested
before this — 66/66 checks, `tests/native/test_gamebots_goldsrc.c`, no HLSDK
needed. **What's new is that `retrobot_engine.cpp` (the Metamod glue) now
compiles too**, on a locally-assembled 32-bit toolchain (no root) described
below, and a real compiler found three genuine bugs the header-matching pass
alone could not have caught — see "Bugs a compiler found" just below. All
three are pinned by `tests/python/test_gamebots_goldsrc_hooks.py` (7 checks,
source-text assertions, no HLSDK/compiler needed to run).

**What's still NOT verified:** this was dlopen'd and had `Meta_Query()` called
in a standalone smoke-test harness, never inside a real HLDS+Metamod process.
`Meta_Attach`, the `pfnStartFrame` hook, `pfnCreateFakeClient`, and everything
that touches real engine state (`g_engfuncs`, `gpGlobals`) has not run against
a live server. **Do not deploy this to `cs16-server`, `cs16-noblood`, or
`specialists-server`** until it has been tested against a throwaway HLDS
instance on a spare port (see "Testing on a throwaway server" below).

### Bugs a compiler found (that header-matching alone missed)

Every HLSDK/Metamod API used was checked against the actual cloned headers
before a compiler ever ran — matching the header turned out to be necessary,
not sufficient. Three real bugs only showed up once a compiler looked at it:

1. **`clientdata_t` was never declared.** HLSDK's `dlls/extdll.h` (which
   pulls in `eiface.h`) never includes `common/entity_state.h`, which is the
   only place `clientdata_t` (and `weapon_data_t`, redundantly, since it's
   also reachable via `<weaponinfo.h>`) is declared. The compiler's error
   recovery silently treated the unknown `clientdata_t` as `int`, so
   `RB_UpdateClientData_Post`'s third parameter became `int *cd` instead of
   `clientdata_t *cd` — a hook that, had a more permissive/older compiler let
   it through, would have been registered into metamod's live
   `DLL_FUNCTIONS` table with the wrong pointer type, read every frame for
   every connected player. Fixed by `#include <entity_state.h>`.
2. **`dlls/util.h` has no include guard**, by HLSDK's own design — a `.cpp`
   is meant to include it exactly once, directly. This file did that AND got
   it a second time transitively via `<meta_api.h> -> dllapi.h -> sdk_util.h
   -> <util.h>`, so it was parsed twice in one translation unit, and every
   default-argument and class declaration in it became a hard "redefinition"
   error. Fixed by removing the direct `#include <util.h>` (it still arrives,
   once, via `<meta_api.h>`).
3. **`UTIL_LogPrintf` mismatch, found only at `dlopen()` time.** `dlls/util.h`
   declares it `(char *fmt, ...)` (non-const); the only real implementation —
   `metamod-hl1/metamod/sdk_util.cpp`, which every plugin must compile its own
   copy of (metamod ships no shared runtime for plugins to link against;
   `stub_plugin` lists `sdk_util.cpp` as one of its own sources for exactly
   this reason, and this Makefile now does too) — defines it
   `(const char *fmt, ...)`. Those are two *different* C++ overloads with two
   different mangled names. The mismatch is invisible at build/link time (a
   `.so` is allowed unresolved symbols) and only shows up as `dlopen()`
   failing at runtime with `undefined symbol: _Z14UTIL_LogPrintfPcz`. Fixed
   by redeclaring the real (const) signature in `retrobot_engine.cpp` before
   any call site, so string-literal calls bind to the overload that exists.

All three are exactly the class of mistake the file's original "written, not
built" disclaimer warned about — caught the moment a real compiler ran,
consistent with matching a header being necessary but not sufficient.

### No-root 32-bit toolchain (how this was actually built)

The dev host had `gcc`/`g++` but not the 32-bit multilib package
(`libc6-dev-i386`/`gcc-multilib`), and `sudo apt-get install` needs
interactive auth a Claude Code session doesn't have. **`apt-get download`
works without root** (it just fetches `.deb`s, no install step), so the fix
is to unpack the needed packages into a private prefix and point the
compiler at it with `-B`/`-L`/`-idirafter` instead of `--sysroot` (`--sysroot`
hides the *system* C++ headers, which are still needed since this prefix
only carries the 32-bit halves):

```bash
# one-time setup into a prefix, e.g. $HOME/.local/m32root:
M32=/path/to/your/m32/prefix
mkdir -p "$M32"
cd /tmp && apt-get download gcc-14-multilib lib32gcc-14-dev libx32gcc-14-dev     libc6-dev-i386 libc6-i386 lib32stdc++-14-dev lib32stdc++6     lib32stdc++-15-dev lib32gcc-15-dev gcc-15-multilib
for d in *.deb; do dpkg-deb -x "$d" "$M32"; done

# $M32/usr/lib32/libc.so is a linker script with absolute host paths
# (/usr/lib32/..., /lib/ld-linux.so.2) that don't exist in the prefix --
# rewrite them to point inside $M32, and symlink the dynamic linker:
#   $M32/lib/ld-linux.so.2 -> ../usr/lib32/ld-linux.so.2

M32FLAGS="-B$M32/usr/lib/gcc/x86_64-linux-gnu/15/32/ -B$M32/usr/lib32/ \
  -L$M32/usr/lib32 -L$M32/usr/lib/gcc/x86_64-linux-gnu/15/32 \
  -idirafter $M32/usr/include/x86_64-linux-gnu/c++/15/32 \
  -idirafter $M32/usr/include/x86_64-linux-gnu \
  -idirafter /usr/include/x86_64-linux-gnu"

cd scripts/gamebots/adapters/goldsrc
make CC="gcc $M32FLAGS" CXX="g++ $M32FLAGS"
```

This is documented here as the path that actually worked, but **`make
check-toolchain`'s plain `sudo apt-get install gcc-multilib g++-multilib
libc6-dev-i386` remains the preferred route** on any host where root is
available — it's simpler, and it's what every other developer on a normal
Linux box should just do. The no-root recipe above is for exactly the
situation this session was in: an automated build session with no
interactive sudo.

## What's here

| file | what it is | tested? |
|---|---|---|
| `retrobot_core.h` / `.c` | Engine-independent observation packing: world→ego-centric rotation, threat-sorted entity slots, normalisation/clamping, schema packing via the `GB_OBS_*` macros. Zero HLSDK dependency by design. | **Yes** — `tests/native/test_gamebots_goldsrc.c`, 66 checks, plain `gcc -std=c11` |
| `retrobot_engine.cpp` | The Metamod plugin: `Meta_Query`/`Meta_Attach`, fakeclient creation, the `pfnStartFrame` POST hook that drives the per-tick observe→exchange→act loop, `TraceLine`-based raycasting and visibility, action application via `pfnRunPlayerMove`. | **Builds, links, dlopens; `Meta_Query()` runs correctly. NOT run inside a real HLDS** — see "Build status" |
| `Makefile` | Clones HLSDK + metamod-hl1 into `build/` (gitignored), builds `build/retrobot.so` (now including metamod's own `sdk_util.cpp`, required — see "Bugs a compiler found"). `make check-toolchain` first. | N/A |
| `build/` | Gitignored. `hlsdk/` (ValveSoftware/halflife) and `metamod-hl1/` (alliedmodders/metamod-hl1), cloned by `make deps`, plus build output. **Never commit anything in here.** | — |

Plus `tests/python/test_gamebots_goldsrc_hooks.py` (7 checks, source-text
assertions, no HLSDK/compiler needed) pinning the three bugs above so a later
refactor can't silently reintroduce any of them.

## Building

```bash
cd scripts/gamebots/adapters/goldsrc
make check-toolchain    # fails fast with the apt-get line if you don't have gcc-multilib
make deps               # clones build/hlsdk and build/metamod-hl1 (~50MB, one-time)
make                     # builds build/retrobot.so
```

`make deps` needs network access to GitHub; both repos were reachable and
cloned successfully (`git ls-remote` and `git clone --depth 1` both worked,
~48MB for HLSDK). On a host with `gcc-multilib`/`libc6-dev-i386` already
installed, that's the whole story. On a host without root, see "No-root
32-bit toolchain" above for the `CC=`/`CXX=` override that was actually used
to get this building.

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

**What runs today via `tests/run_all.sh`, no HLSDK/32-bit toolchain needed:**

```bash
bash tests/run_all.sh
# or directly:
gcc -std=c11 -O0 -g -Wall -I tests/native tests/native/test_gamebots_goldsrc.c -lm \
    -o /tmp/test_gamebots_goldsrc && /tmp/test_gamebots_goldsrc   # 66 checks
pytest tests/python/test_gamebots_goldsrc_hooks.py                # 7 checks
```

The first exercises `retrobot_core.c`'s NaN-safety, the threat-sort
(visibility, distance, teammate/enemy grouping, ties, invalid/out-of-range
input), the frame rotation, and full `rb_build_observation()` round trips
including the reserved intent slot and alignment padding staying zero. The
second pins the three real bugs a compiler found in `retrobot_engine.cpp`
(see "Bugs a compiler found") at the source-text level, so a refactor can't
silently reintroduce any of them without a compiler to catch it.

**What ran once, by hand, with the no-root 32-bit toolchain (not part of
`tests/run_all.sh` — needs the toolchain and isn't reproducible on a bare
clone):** `retrobot.so` built, `file` confirmed `ELF 32-bit ... Intel 80386`,
`nm -D --defined-only` confirmed all five Metamod entry points export with
plain (unmangled) C linkage, a standalone `dlopen()`+`dlsym()` harness loaded
it with zero undefined symbols, and a second harness called the real
`Meta_Query()` and got back the correct `plugin_info_t` (name, version,
`ifvers="5:13"` matching `META_INTERFACE_VERSION`, `loadable`/`unloadable`).
Neither harness is committed — they were disposable ~30-line C files built
against the same prefix, not test infrastructure this repo can run without
that prefix.

**What is still NOT verified — everything past `Meta_Query`:** `Meta_Attach`,
`GiveFnptrsToDll`, the `pfnStartFrame` hook, `pfnCreateFakeClient`, and every
line that touches `g_engfuncs`/`gpGlobals` has never run against a real
engine. Those need an actual HLDS+Metamod process, which is the next section.

### Testing on a throwaway server (required before ANY live deploy)

Per the task brief: **never deploy to `cs16-server`, `cs16-noblood`, or
`specialists-server`** — people play on those. To test for real:

1. Get the 32-bit toolchain (`sudo apt-get install gcc-multilib g++-multilib
   libc6-dev-i386`, or the no-root recipe above) and run `make` here until
   `build/retrobot.so` exists.
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
