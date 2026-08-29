# UT99 engine adapter

`GameBots.u` — an UnrealScript mutator + pawn subclass for OldUnreal 469e
(Unreal Tournament 1999) that lets the policy server drive bots, the same
contract every other `scripts/gamebots/adapters/*` engine adapter honours.

**Read the verdict section before building anything on top of this.** The
adapter compiles clean, spawns bots, and correctly falls back to the built-in
AI — but the actual network connection to the policy server does not work on
the OldUnreal 469e Linux dedicated server this was built and tested against,
and that appears to be a real limitation of this build, not a bug in this
adapter. See [What works / what doesn't](#what-works--what-doesnt) below.

## Why this adapter looks different from the others

UT99 has **no native plugin ABI** — no `qagame.so`, no Metamod `.so`. Its bot
AI is itself UnrealScript (`Botpack.u`), which is exactly what makes this
adapter possible at all: everything here is `.uc` source, compiled by `ucc
make` into `GameBots.u`, and loaded via a mutator, not linked into a native
module.

That has two consequences the other adapters don't have to deal with:

- **No C client.** `gb_client.c` (AF_UNIX, `memcpy`-based marshalling) is not
  usable from UnrealScript. This adapter speaks the identical wire protocol
  over **TCP** instead, to `policyd.py --tcp-listen`, added to policyd for
  exactly this purpose (commit `66da9b4`) — same header, same per-bot floats,
  same schema hash. There is no second protocol.
- **No bit-cast.** UnrealScript has no union, no pointer cast, no
  `FloatAsInt`-style intrinsic. Marshalling floats to/from the wire's IEEE-754
  bytes is done in software — see [GBMath.uc](#the-float-marshalling-problem)
  below.

## What's here

| path | what it is |
|---|---|
| `GameBots/Classes/GBSchema.uc` | **Generated** by `gen_gbschema.py` from `schema.py`. Never edit. |
| `GameBots/Classes/GBMath.uc` | Software IEEE-754 float↔int codec (see below) |
| `GameBots/Classes/GBLink.uc` | The policy-server client — `TcpLink` subclass, chunked framing |
| `GameBots/Classes/GBBot.uc` | `Bot` subclass — the fallback/override mechanism |
| `GameBots/Classes/GBMutator.uc` | The mutator: spawns bots, builds observations, applies actions |
| `gen_gbschema.py` | Regenerates `GBSchema.uc` from `scripts/gamebots/schema.py` |
| `build.sh` | Regenerate + compile against a UT99 install (not the live one — see below) |

## One-time setup, then build

`ucc`'s compiler needs a `UnrealTournament.ini` to already exist under
`$HOME/.utpg/System/` before it will compile anything (see `build.sh`'s
header comment for exactly what that path resolution does and why the
compiled output ends up somewhere other than where you'd expect). The
simplest way to get one: copy an existing server's, e.g.

```bash
mkdir -p build/ucc-home/.utpg/System
cp ~/ut99-server/../wherever/UnrealTournament.ini build/ucc-home/.utpg/System/
cp ~/ut99-server/../wherever/User.ini            build/ucc-home/.utpg/System/
```

**Never point this at the live `ut99-server` unit's own `$HOME`** — compiling
there is harmless (it only writes `GameBots.u`) but do it on a copy anyway;
the point of this adapter is a separate instance end to end. Set up a spare
UT99 install (`cp -r` the System/System64 trees, symlink the large read-only
asset dirs — Maps/Music/Sounds/Textures/SystemLocalized/Help — to save space
and time) on a spare port (7900+, never 7797).

Then:

```bash
./build.sh /path/to/spare-ut99-install [ucc-home-dir]
# -> /path/to/spare-ut99-install/System/GameBots.u
```

Add `EditPackages=GameBots` under `[Editor.EditorEngine]` and
`ServerPackages=GameBots` under `[Engine.GameEngine]` in that install's own
`UnrealTournament.ini` (both are one-time, not something `build.sh` does for
you, since it would otherwise have to parse and rewrite an ini it doesn't
own). Also set `MinPlayers=0` and `bAutoNumBots=False` under
`[Botpack.DeathMatchPlus]` if you want ONLY this adapter's bots in the match
(see [Known incompleteness](#known-incompleteness-worth-fixing-next) about
why that matters).

Launch (a **separate** instance, never the live `ut99-server` systemd unit):

```bash
cd /path/to/spare-ut99-install/System64
HOME=<ucc-home-dir> ./ucc-bin-amd64 server \
    "DM-Deck16][.unr?game=Botpack.DeathMatchPlus?mutator=GameBots.GBMutator" \
    -port=7900
```

**Off by default.** Loading the mutator changes nothing until you either edit
`<ucc-home-dir>/.utpg/System/GameBots.ini` (`bEnabled=True`, generated on
first run from `GBMutator`'s `defaultproperties`) or type `mutate gb_enable 1`
at the server console (`mutate gb_enable 0` to stop, `mutate gb_status` to
check). This mirrors the Quake III adapter's `gb_enable 0` cvar as closely as
UT99's admin surface allows — UnrealScript mutators don't have cvars, but
`Mutate()` is the console-command hook every UT99 admin already knows.

## How it hooks in

**A mutator, not a `GameInfo` subclass**, and it spawns its **own** roster of
bots rather than trying to reclassify the server's `addbot`/auto-added ones:

- `GBMutator.PostBeginPlay()` (if enabled) spawns `NumBots` (config, default
  4, capped at `MAX_BOTS`=16) `GBBot` pawns directly via
  `Spawn(class'GBBot', ...)` + `Level.Game.RestartPlayer(NewBot)`, using
  `Level.Game.FindPlayerStart(None)` for each spawn point.
- Every `TickRate` Hz (config, default 10 — **not** the server's own tick
  rate; see [Why 10 Hz](#why-10-hz-not-the-servers-own-tick-rate)),
  `GBMutator` builds one observation per bot and sends the batch to
  `GBLink`, which frames it onto the wire exactly like `gb_client.c` does.
- `GBBot.Tick()` calls `Super.Tick(DeltaTime)` **first** — this runs
  Botpack's own bot AI unconditionally, exactly as it would with no adapter
  at all — then calls back into `GBMutator.ApplyAction()`, which overwrites
  `Acceleration`/`ViewRotation`/`bFire`/`bAltFire`/`bDuck` **only if** a fresh
  policy answer exists for that bot this cycle. No answer (server down,
  still connecting, exchange timed out, or the mutator disabled) means
  `Super.Tick()`'s own output stands untouched.

This is the same fallback discipline as `gb_adapter.c`'s "botlib still runs
and still produces a complete usercmd" — the ordering guarantee comes from
calling `Super.Tick()` unconditionally before ever touching the pawn's
output, not from any timing assumption between actors.

## The float-marshalling problem

UnrealScript (this OldUnreal 469e build) has no operator that reinterprets a
float's bit pattern as an int. Confirmed absent, not merely undiscovered:
`ucc packagedump Core`/`Engine` list every native function the engine
exposes, and none of `GetAxes`/`Normal`/`VSize`/`Rotator`/`Vector` — the ones
this adapter *does* use for vector math — is a bit-cast, nor is anything else
in either package.

**It IS practical, with software arithmetic**, so this adapter does not fall
back to a text framing (the alternative the task anticipated for this exact
problem): `GBMath.FloatToBits`/`BitsToFloat` implement the textbook software
IEEE-754 single-precision codec — normalise the magnitude into `[1,2)` by
repeated multiply/divide by 2 (at most ~11 iterations for the value ranges
this schema uses), then assemble sign/exponent/23-bit-mantissa with integer
bitwise operators, which UnrealScript **does** have (`<<`, `>>>`, `&`, `|` —
confirmed via `ucc packagedump Core`'s operator table, not assumed). NaN/Inf
are not specially encoded on the way out (nothing this adapter sends is ever
non-finite) or specially decoded on the way in (a garbage exponent from a
half-trained policy decodes to *some* finite float, which `GBMutator.OnAction`
clamps immediately after — the same distrust `gb_client.c`'s `gb_clamp()`
applies on every other engine).

This compiled and ran correctly in isolation (the codec has no dependency on
anything network-related), but see the verdict below for why it was never
exercised against a real round trip end to end.

## The cross-class `const` compiler quirk

A real, load-bearing finding, not a style choice: a bare cross-class constant
reference (`GBSchema.SOME_CONST`, `SOME_CONST` a plain `const NAME = value;`
in another class) compiles in some syntactic positions on this `ucc` and not
others, with **no discoverable pattern** — reproduced across ints, an
unsuffixed bit-flag, a plain loop bound identical in shape to ones that
compiled fine two lines away, and float constants (which never failed).
Positions that failed: a bare function-call argument, the whole RHS of an
assignment, one operand of `&`/`&&`/`>=` combined with the constant, a
ternary condition. Positions that worked: an array subscript, most of the
time. The exact same construct sometimes moved position when a *different*
line was rewritten — a parser-desync signature, not something tied to any one
constant.

**The fix: every value in `GBSchema.uc` is a static function
(`class'GBSchema'.static.Name()`), not a `const`.** A cross-class function
call goes through the same call machinery as any other (`VSize()`,
`FClamp()`, ...) and hit none of these failures in any position tried.
`gen_gbschema.py` emits `static final function int NAME() { return value; }`
for every schema value; call sites are the mechanical rewrite
`GBSchema.NAME` → `class'GBSchema'.static.NAME()`.

A second, narrower version of the same family: a **ternary whose condition is
not itself a parenthesised comparison** (`X ? A : B` where `X` is a bare
bool/byte identifier or array element) also intermittently failed
("Type mismatch in '='"), including cases where both branches and the
destination were unambiguously the same type. Every ternary in this codebase
was rewritten to `if/else` rather than chased individually — cheaper than
continuing to isolate a second parser bug in a closed-source, 25-year-old
compiler.

## Observation — what's filled, what's zero and why

Mirrors the Quake III adapter's honesty table:

| group | status |
|---|---|
| health, alive, on-ground, crouching, in-water, velocity (ego frame), speed, pitch, view-relative geometry rays (`Actor.Trace`, `MASK`-free — `bTraceActors=True`) | **filled** |
| entity slots: present, teammate, direction (ego frame), distance, relative velocity, health, visibility, sorted visible-enemies-first then nearest (`Pawn.VisibleCollidingActors`) | **filled** |
| took damage, killed someone, died (tracked frame-to-frame, same reason as `gb_adapter.c`: the engine hands us no deltas) | **filled** |
| round time / score diff (only for `Botpack.DeathMatchPlus` and subclasses — `TimeLimit`/`FragLimit` live there, not the more generic `TournamentGameInfo`, confirmed via `ucc packagedump Botpack`) | **filled**, zero for other gametypes |
| `ammo_frac`, `ammo_reserve_frac`, `reloading` | **always zero.** The ammo count lives on a separate `Ammo` inventory actor (`AmmoAmount` is an `Ammo` property, not a `Weapon` function — confirmed via packagedump), reachable via `Pawn.FindInventoryType(class<Inventory>)`; that call's parameter shape did not compile here ("type mismatch in parameter 1") within the time available. Left zero and documented, not guessed. |
| `weapon_id_norm` | **always zero** — no stable, cross-map weapon index is exposed on the base `Pawn`/`Weapon` API this adapter reads |
| `damage_dir` | **always zero** — UT99's `Pawn` does not expose "who last hurt me" the way Quake III's `client->lasthurt_client` does on the API this adapter uses |
| `objective` (bomb/flag state) | **zero** — this adapter reads the generic `DeathMatchPlus` surface only, not CTF/Domination specifics |
| `intent` | zero by design — the policy server injects the planner's vector, same contract as every other engine adapter |

`EYE_Z` (44.0 units, used for ray/entity eye-height offsets) is a **documented
estimate**, not a measured engine constant — there is no
`DEFAULT_VIEWHEIGHT`-equivalent exposed to script the way Quake III's
`Pawn.h` constant is.

## Action application

- **View**: the policy's `pitch_delta`/`yaw_delta` (degrees) are converted to
  UnrealScript's 0..65535-per-turn rotator units and added to
  `Pawn.ViewRotation`; `DesiredRotation` is set to match so movement follows
  the same heading. Absolute aim is never accepted from the policy, same
  reasoning as every other adapter: a policy that could teleport its
  crosshair would be both unfair and unlearnable-looking.
- **Movement**: `forward`/`side` combine with the (yaw-only) view basis via
  `GetAxes()` into a wish direction, scaled by `Pawn.AccelRate` and written to
  `Pawn.Acceleration` — the same field the built-in AI writes, so whichever
  wrote it last wins for that tick, which is exactly the override semantics
  wanted.
- **Buttons**: `bFire`/`bAltFire`/`bDuck` are **byte** properties on `Pawn`
  despite the `b`-prefix naming convention (confirmed via packagedump — this
  cost real debugging time, see `GBMutator.HasButton`'s comment), set
  directly from the action's button bitmask.
- **Clamping**: `GBMutator.OnAction` clamps every float (pitch/yaw delta to
  the schema's bounds, forward/side to ±1) and explicitly tests for NaN
  (`v != v`) before anything else touches it — same distrust
  `gb_client.c`'s `gb_clamp()` applies on every other engine, and the thing
  that makes `GBMath`'s un-decoded NaN case (see above) safe.

## Why 10 Hz, not the server's own tick rate

`TcpLink` is asynchronous by construction — `Open()`/`SendBinary()` never
block, and a response arrives later as a `ReceivedBinary` event whenever the
engine's network tick delivers it. There is no call in UnrealScript that
"sends and waits", so `GBMutator` cannot poll the policy server every server
frame the way a synchronous C client can afford to; it paces itself to a
configurable `TickRate` (10 Hz default, matching Quake III's `sv_fps 20`-ish
order of magnitude) and, if the previous exchange has not completed by the
next tick, simply skips sending a new one rather than piling up requests on
a stream protocol that has no request-ID to disambiguate them.

## What works / what doesn't

**Verified, on a real OldUnreal 469e dedicated server (a separate instance on
port 7900, never the live `ut99-server` unit):**

- `GameBots.u` compiles clean (`ucc make`, 0 errors, 0 warnings).
- **Off by default**, verified with a live control: with `bEnabled=False` the
  mutator loads (`Add mutator GameBots.GBMutator` in the server log) and does
  precisely nothing — no `gamebots(ut99):` log line, `\status\` UDP query
  reports `numplayers\0`.
- With `bEnabled=True`, the mutator spawns its configured bot count
  (`gamebots(ut99): spawned 3/3 GBBot(s)`), the server keeps running (no
  crash, no `PlayAnim`/state errors beyond a single cosmetic "No mesh"
  warning per bot at spawn), and Botpack's own bot AI runs via `GBBot`'s
  `Super.Tick()` — this is genuinely the same code path a stock `Bot` uses.
- The fallback path is proven **by construction and by log evidence**: every
  run above shows `gamebots(ut99): policy server unavailable -- bots are on
  their own AI` and the server stays healthy. A dead/unreachable policy
  server degrades cleanly; this was true on every single run, because of the
  next finding.

**Not verified — the connectivity blocker.** Despite `IpAddr`/port resolving
correctly (`StringToIpAddr`, and separately `Resolve()`/`Resolved()`, both
confirmed with the correct `127.0.0.1:27200` and `192.168.1.132:27200`),
`BindPort()` succeeding, `bAlwaysTick=True` on `GBLink` (its own `Tick()` is
where TcpLink's native socket polling lives — a real, separate bug found and
fixed during development, since `GBMutator` being always-ticked does not
imply `GBLink` is), and `Open()` reporting success every time — **no TCP
connection to `policyd.py`'s listening socket was ever observed to complete,
on this build.** Verified with `ss -tn` watched continuously across the
`RECONNECT_COOLDOWN` retry cycle: not even a SYN packet leaves the process,
toward loopback or the LAN interface, and `policyd.py`'s own log (which does
log "adapter connected" on every other engine) never shows an incoming
connection. Every standard TcpLink client pattern documented for this engine
family was tried:

| tried | result |
|---|---|
| `StringToIpAddr()` + `Open()` | Open() returns true, `err=11` (EAGAIN) from `GetLastError()`, zero packets on the wire |
| `Resolve()` + `event Resolved(Addr)` + `Open()` | `Resolved()` fires with the correct address, `Open()` again reports true, still zero packets |
| `BindPort()` before either of the above | No change |
| `bAlwaysTick=True` on the `TcpLink` subclass itself | No change (this WAS necessary and is kept — it's a real, separate requirement, just not sufficient) |
| Loopback (`127.0.0.1`) vs the host's LAN IP (`192.168.1.132`) | No difference — rules out a loopback-specific quirk |
| A long-lived server (45+ seconds, retrying every 2s per `RECONNECT_COOLDOWN`) | No connection ever completes at any point |

One data point that muddies rather than clarifies: at least once, `IsConnected()`
returned true long enough for `GBMutator` to send a request (visible as
`gamebots(ut99): exchange timed out after 0.5s`, which only happens after
`SendObservations()` runs) — meaning the engine's own connection-state
bookkeeping is, at least transiently, **inconsistent with the OS-level socket
state** `ss` reports. This is consistent with a genuine bug in this specific
build's Linux `TcpLink` implementation (an optimistic "connecting" state that
never resolves to a true completion or a reported failure) rather than a
missing step on the script side — but without engine source or a debugger
attached to `Engine.so`/`IpDrv.so`, that could not be confirmed further within
the time available.

## The honest verdict

**UnrealScript + `TcpLink` is not a dead end as a *design*** — the schema,
the framing, the software float codec, the observation/action mapping, and
the fallback discipline are all sound, compile cleanly, and (short of the
actual network round trip) behave exactly as intended on a live server. If
this were running on a build where `TcpLink` client connections work, this
adapter should work with no further changes.

**It is a dead end on *this specific build*** — OldUnreal 469e's Linux
dedicated server — for the TCP transport specifically, and that could not be
worked around from script. Recommended next steps, in order of effort:

1. **Test on the Windows build of the same OldUnreal 469e**, or an official
   469b/469c release, before assuming the whole engine generation is
   affected — this may be a Linux-port-specific regression in `IpDrv.so`'s
   socket handling rather than something universal to UT99.
2. **File/search OldUnreal's issue tracker** for known `TcpLink` client-mode
   problems on Linux dedicated servers; a fix or workaround may already be
   documented given how long this codebase has existed.
3. **If TCP genuinely cannot be made to work**, the schema/framing is
   transport-agnostic, so the honest fallback the task anticipated for a
   different problem (binary marshalling, which this adapter solved) applies
   here instead: policyd already supports being taught an additional framing
   with modest effort (its request/response packing lives in one file,
   `schema.py`), and `UdpLink` is a sibling class in the same `IpDrv` package
   this adapter never got to test — worth trying before concluding UDP is
   affected the same way, since it is a materially different code path
   (native engine actors already use `UdpLink`-family classes for the master
   server pings visible in every server's own startup log).

## Known incompleteness worth fixing next

- **Bot player-count registration.** `\status\` UDP queries report
  `numplayers\0` even with 3 `GBBot`s alive and running Botpack AI. This
  adapter spawns bots via `Spawn(class'GBBot', ...)` + `RestartPlayer()`
  directly, bypassing `GameInfo.AddBot()`'s own bookkeeping — plausibly this
  means `PlayerReplicationInfo` isn't fully wired the way a "real" player's
  is, which would also silently zero the `score_diff_norm`/teammate-detection
  paths in `BuildGameContext`/`BuildEntities` for these bots (both already
  guard on `PlayerReplicationInfo != None`, so they fail safe to zero rather
  than crash, but the *feature* is likely not working). Not chased further
  given the connectivity blocker made it moot for this pass; worth revisiting
  if TCP starts working.

## Tests

```bash
python3 -m pytest tests/python/test_gamebots_ut99.py
```

Source-level checks only (no `ucc` in CI): schema constants match
`schema.py` exactly, `GBMutator` defaults to disabled, the fallback call
(`Super.Tick()` before any override) is present in `GBBot.uc`, and
`GBSchema.uc` is byte-for-byte what `gen_gbschema.py` would emit right now
(the same "generated file, diffed" discipline as the C header's test).
