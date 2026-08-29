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
three -- plus the two live-test bugs found afterward (see "Live-test
findings" below) -- are pinned by `tests/python/test_gamebots_goldsrc_hooks.py`
(10 checks, source-text assertions, no HLSDK/compiler needed to run).

**Update: it has now run inside a real HLDS+Metamod process, on a throwaway
server, for 8+ minutes across multiple round restarts, with the policy server
receiving live decisions the whole time.** See "Live-test findings" below for
what that found (two real bugs, both fixed) and what is still open (bots do
not yet actually join a team and spawn on this engine build). **Still do not
deploy this to `cs16-server`, `cs16-noblood`, or `specialists-server`** --
the crash described below happened on exactly this kind of throwaway
instance, which is the whole reason to have one.

## Live-test findings (throwaway HLDS, engine build 48/1.1.2.7/Stdio)

A real Half-Life dedicated server with Metamod, running on a spare port, with
one `retrobot_addbot`-created fake client and a real `policyd` instance,
surfaced three things static analysis and header-matching could not:

### 1. `pfnClientPutInServer` never fires for a fakeclient on this engine build

**Symptom:** `retrobot_addbot` created a bot (`status` showed it connected,
`pfnCreateFakeClient` clearly worked), but `retrobot_debug` reported "0
registered bots" indefinitely, and `policyd` logged zero adapter connections
for over a minute. `RB_ClientPutInServer_Post` had an unconditional entry log
added specifically to test this; across multiple bot creations, in multiple
server restarts, it never printed once -- while the identical `UTIL_LogPrintf`
call worked fine from other dllapi hooks (`pfnServerActivate`) in the same
run, ruling out a logging problem, and the `DLL_FUNCTIONS` field order was
re-verified field-by-field against `eiface.h` a second time, ruling out a
table-wiring bug (a table-order mistake could not explain one specific slot
never firing while an adjacent one, three slots later in the same struct,
fires every map load).

**Fix:** register the bot (`rb_register_bot()`) and issue its team/class
auto-join immediately after `pfnCreateFakeClient()` returns, in
`RB_Cmd_AddBot`, rather than waiting for `pfnClientPutInServer`.
`RB_ClientPutInServer_Post` still calls the same function too (it is
idempotent -- a second call for an already-registered edict is a no-op), so
this costs nothing on an engine build where that hook DOES fire.

**Verified fixed:** after this change, `policyd`'s own log showed
`adapter connected` within one second of `retrobot_addbot`, followed by a
steadily climbing decision count (0 to 100,000+ requests, 20-45
bot-decisions/s sustained) for the rest of the session, surviving two forced
round restarts (`mp_restartgame 1`, `sv_restartround 1`) and at least one
natural `mp_roundtime` expiry with no interruption.

### 2. A stale `edict_t*` is a live crash, not a theoretical one

**Symptom (found by the person who ran the throwaway server, not the author of
the fix):** a prior version of this plugin, running one fakeclient that had
never successfully joined a team, crashed the whole HLDS process with a
segfault roughly eight seconds after a `Round_End` server-log line. The likely
mechanism: `g_bots[i].ed` was a bare `edict_t*` cached once at registration and
trusted every frame after (`if (!b->ed || b->ed->free) ...`); GoldSrc's edict
array is static (allocated once at `pfnServerActivate`, never reallocated), so
this is not the classic heap-use-after-free, but a cached pointer can still
describe an edict that no longer belongs to *this* bot if `g_bots[]` and
engine reality ever desync (a disconnect this plugin's `ClientDisconnect` hook
missed, for instance) -- and dereferencing `->v` on the wrong entity from
inside a per-frame server hook is exactly the shape of bug that takes a live
game server down with it.

**Fix:** `rb_resolve_bot_edict()` re-resolves the edict fresh from its stable
index (`pfnPEntityOfEntIndex`) every time it is used, validates it is still
non-free and still `FL_FAKECLIENT`, and drops the bot's registration outright
(rather than merely skipping one frame) if the check fails -- so a desync
cannot resurface next frame with the same stale data. Applied everywhere a
bot's edict is touched: `RB_StartFrame_Post`'s per-bot loop and
`RB_Cmd_Debug`.

**Verified fixed:** the hardened build ran a fakeclient (still not
team-joined -- see finding 3) for 8+ minutes across two forced round restarts
and a natural round-timer expiry with no crash, where an earlier build in the
same scenario had crashed within eight seconds of a round ending. This is not
a proof the exact mechanism was this one and no other -- there is no core
dump analysis backing that claim, only that the hardened build survived
substantially longer under the same conditions -- but it is the correct
defensive fix regardless of the precise trigger.

### 3. `pfnClientCommand` does not dispatch to the game DLL for a fakeclient on this engine build (OPEN)

**Symptom:** even after fix #1, a registered bot's `pev->team` stayed `0`
(unassigned) and its `health` stayed `0.0` indefinitely -- it never actually
joined a team, spawned, or received a loadout, despite `rb_register_bot()`
issuing `jointeam 5` / `joinclass 5` via `pfnClientCommand()` right after
creation. To isolate whether this was "wrong command for this CS build" versus
"the mechanism doesn't work for a fakeclient at all", a temporary diagnostic
hook logged every `DLL_FUNCTIONS.pfnClientCommand` call the game DLL received,
and a temporary console command fired arbitrary `pfnClientCommand()` calls at
the bot on demand. Sending the universally-recognised `kill` command produced
**zero** log lines from that hook -- the same result as `jointeam`/`joinclass`.
This rules out "wrong command name" and points at this specific WON-era engine
build (`48/1.1.2.7/Stdio`) not dispatching `pfnClientCommand()` through to the
game DLL's `ClientCommand` callback for a fakeclient at all, whatever the
command text.

**Status: not fixed, left as a known, documented gap.** The `jointeam`/
`joinclass` calls remain in `rb_register_bot()` (harmless no-op on this
engine build, and this is still the standard, documented mechanism that may
work on other CS 1.6 engine builds -- WON-era HLDS versions are known to
differ significantly from later Steam-era ones). What IS handled: a bot stuck
at `team == 0` for more than `RB_STUCK_UNASSIGNED_FRAMES` (~a few seconds)
after registration now logs a one-time `WARNING` (`rb_maybe_report_stuck()`)
instead of failing silently forever -- this is the direct fix for "a bot that
exists but is skipped every frame should say so once", applied to the exact
failure this investigation hit. **A properly team-joined, spawned bot with
weapons has never been observed** on this adapter; everything verified above
(policy connection, decisions, action application, crash survival) was
observed with the bot in this unassigned/observer-like state. The next
concrete step for whoever picks this up: try a different (later, Steam-era)
CS 1.6 engine build, or find the actual mechanism this WON build expects
(possibly `menuselect N` against a server-sent menu rather than a literal
`jointeam`/`joinclass` command -- not verified either way here).

### The transition-only reporting this all led to

Per the explicit ask that came out of this investigation ("a bot that exists
but is skipped every frame should say so once, not per frame -- the same
pattern the Quake III adapter uses"): `RB_StartFrame_Post` now has three
report-on-transition latches, all following `adapters/quake3/gb_adapter.c`'s
`gb_reported_state` pattern exactly (log only on a state CHANGE, never once
per frame):

- `g_gb_reported_state` -- the policy-server-reachable/unreachable transition
  for the whole exchange.
- `reported_alive` (per bot) -- the alive/dead transition that gates
  participation in the policy loop.
- `reported_stuck_unassigned` (per bot) -- the one-shot "still team 0 after a
  grace period" warning from finding #3 above.

All three are pinned in `tests/python/test_gamebots_goldsrc_hooks.py`
(source-text assertions -- see "Testing" below).

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

Plus `tests/python/test_gamebots_goldsrc_hooks.py` (10 checks, source-text
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

Two server console commands, registered in `Meta_Attach`:

```
retrobot_addbot [count]     # default count = 1
retrobot_debug               # dump every registered bot's raw engine state
```

Each `retrobot_addbot` call creates one `pfnCreateFakeClient` bot and
registers it (`rb_register_bot()`) immediately — **not** waiting for
`pfnClientPutInServer`, which live testing found never fires for a fakeclient
on at least one real engine build; see "Live-test findings" above.
Registration issues the CS 1.6 "auto-assign" team/class join (`jointeam 5` /
`joinclass 5`, the same client commands a real player's menu sends) — **this
currently does not take effect** on the engine build this was tested against
(same section). `retrobot_debug` is the standing tool for checking a bot's
actual `deadflag`/`team`/`health`/`origin` without guessing. Bot removal isn't
wired up to a command yet — disconnect the bot the normal server-admin way
(`kick`) and `RB_ClientDisconnect_Post` frees its registry slot.

## Testing

**What runs today via `tests/run_all.sh`, no HLSDK/32-bit toolchain needed:**

```bash
bash tests/run_all.sh
# or directly:
gcc -std=c11 -O0 -g -Wall -I tests/native tests/native/test_gamebots_goldsrc.c -lm \
    -o /tmp/test_gamebots_goldsrc && /tmp/test_gamebots_goldsrc   # 66 checks
pytest tests/python/test_gamebots_goldsrc_hooks.py                # 10 checks
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

### Testing on a throwaway server (this has now been done)

Per the task brief: **never deploy to `cs16-server`, `cs16-noblood`, or
`specialists-server`** — people play on those. A throwaway `hlds_linux`
instance on a spare port, with Metamod and this plugin, was stood up and
torn down again for this session's live testing — see "Live-test findings"
above for what it found. To repeat or extend that testing:

1. Get the 32-bit toolchain (`sudo apt-get install gcc-multilib g++-multilib
   libc6-dev-i386`, or the no-root recipe above) and run `make` here until
   `build/retrobot.so` exists.
2. Stand up a **separate** HLDS instance on a spare UDP port (not 27015/27016,
   whatever `cs16-server`/`cs16-noblood` use — check
   `scripts/game-servers/README.md`), with Metamod installed (copy the
   pattern `cs16-noblood` already uses, per the task brief).
3. Drop `retrobot.so` into that instance's `addons/metamod/dlls/`, register
   it in `metamod`'s `plugins.ini`.
4. **If any of the instance's files are symlinked back to a live server's
   directory tree (as a from-scratch throwaway rig set up by symlinking a
   vanilla install often is), check `cstrike/logs/` specifically.** It is
   easy to leave it as a symlink to the live server's own `logs/` dir by
   omission, and `rcon log on` on the throwaway instance then writes its
   test session's log lines straight into the live server's log history —
   this happened once during this session's testing (one stray file,
   `L0829029.log`, appeared in the live `cs16-server`'s `cstrike/logs/`
   and was deleted immediately). Fix: `rm` the symlink and `mkdir` a real,
   private `logs/` directory for the throwaway instance before turning on
   logging.
5. Start `policyd.py --policy scripted` (or `noop`) pointed at the default
   socket, `rcon retrobot_addbot 1`, then `rcon retrobot_debug` and watch
   `policyd`'s own log for `adapter connected` / a climbing decision count
   — that combination is the actual bar, not just `status` showing a
   connected bot (which was true even before the registration fix, while
   `policyd` saw nothing at all).
6. **Still open, next step for whoever picks this up:** get a bot to
   actually leave `team == 0` and spawn. Finding #3 above rules out
   "wrong jointeam/joinclass id" — the whole `pfnClientCommand()` dispatch
   path was shown not to reach the game DLL for a fakeclient on this
   engine build (`48/1.1.2.7/Stdio`). Try: (a) a different, later CS 1.6
   engine build (WON-era builds are known to differ a lot from Steam-era
   ones), or (b) whatever mechanism a server-sent team-select MENU
   actually expects back from a client on this build (possibly
   `menuselect N` rather than a literal `jointeam`/`joinclass` command —
   not verified either way here). `retrobot_debug`'s `team=`/`health=`
   fields are the check for "did it work": a genuinely spawned player
   reads `team != 0` and `health > 0`.
7. Only after a bot can actually play does deploying to a real server make
   sense, and even then: a separate decision, not implied by this adapter
   existing.


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
