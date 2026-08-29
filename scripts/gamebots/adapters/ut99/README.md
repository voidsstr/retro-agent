# UT99 engine adapter

`GameBots.u` — an UnrealScript mutator + pawn subclass for OldUnreal 469e
(Unreal Tournament 1999) that lets the policy server drive bots, the same
contract every other `scripts/gamebots/adapters/*` engine adapter honours.

**Working end to end, verified live.** The full round trip — build an
observation, send it, get an action back, apply it, and fall back cleanly
when nothing answers — runs on a real dedicated server. Two dead ends were
found and worked around on the way there (TCP, then raw binary over UDP);
both are kept below as history because the investigation is worth as much as
the result.

## Why this adapter looks different from the others

UT99 has **no native plugin ABI** — no `qagame.so`, no Metamod `.so`. Its bot
AI is itself UnrealScript (`Botpack.u`), which is exactly what makes this
adapter possible at all: everything here is `.uc` source, compiled by `ucc
make` into `GameBots.u`, and loaded via a mutator, not linked into a native
module.

That has two consequences the other adapters don't have to deal with:

- **No C client.** `gb_client.c` (AF_UNIX, `memcpy`-based marshalling) is not
  usable from UnrealScript. This adapter speaks the identical wire protocol
  over **UDP** instead, to `policyd.py --udp-listen`, hex-text encoded (see
  below for why) — same header, same per-bot floats, same schema hash. There
  is no second protocol, only a second *encoding* of the same one.
- **No bit-cast.** UnrealScript has no union, no pointer cast, no
  `FloatAsInt`-style intrinsic. Marshalling floats to/from the wire's IEEE-754
  bytes is done in software — see [GBMath.uc](#the-float-marshalling-problem)
  below.

## What's here

| path | what it is |
|---|---|
| `GameBots/Classes/GBSchema.uc` | **Generated** by `gen_gbschema.py` from `schema.py`. Never edit. |
| `GameBots/Classes/GBMath.uc` | Software IEEE-754 float↔int codec, and hex nibble↔char codec |
| `GameBots/Classes/GBLink.uc` | The policy-server client — `UdpLink` subclass, hex-text framed |
| `GameBots/Classes/GBBot.uc` | `Bot` subclass — the fallback/override mechanism |
| `GameBots/Classes/GBMutator.uc` | The mutator: spawns bots, builds observations, applies actions |
| `gen_gbschema.py` | Regenerates `GBSchema.uc` from `scripts/gamebots/schema.py` |
| `build.sh` | Regenerate + compile against a UT99 install (not the live one — see below) |

## One-time setup, then build

`ucc`'s compiler needs a `UnrealTournament.ini` to already exist under
`$HOME/.utpg/System/` before it will compile anything (see `build.sh`'s
header comment for exactly what that path resolution does and why the
compiled output ends up somewhere other than where you'd expect, and why a
*second* build needs both copies of the old `.u` removed first). The
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
`[Botpack.DeathMatchPlus]` so ONLY this adapter's bots are in the match — it
spawns its own roster and does not touch the server's own `addbot`/
auto-added bots (see [How it hooks in](#how-it-hooks-in)).

Launch (a **separate** instance, never the live `ut99-server` systemd unit):

```bash
cd /path/to/spare-ut99-install/System64
HOME=<ucc-home-dir> ./ucc-bin-amd64 server \
    "DM-Deck16][.unr?game=Botpack.DeathMatchPlus?mutator=GameBots.GBMutator" \
    -port=7900
```

Run a policy server with the UDP endpoint (added to `policyd.py` for exactly
this adapter):

```bash
~/.venvs/gamebots/bin/python scripts/gamebots/policyd.py \
    --policy noop --udp-listen 127.0.0.1:27300
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
  `GBLink`, which frames it onto the wire.
- `GBBot.Tick()` calls `Super.Tick(DeltaTime)` **first** — this runs
  Botpack's own bot AI unconditionally, exactly as it would with no adapter
  at all — then calls back into `GBMutator.ApplyAction()`, which overwrites
  `Acceleration`/`ViewRotation`/`bFire`/`bAltFire`/`bDuck` **only if** a fresh
  policy answer exists for that bot this cycle. No answer (server down,
  reply not yet arrived, or the mutator disabled) means `Super.Tick()`'s own
  output stands untouched.
- **`ApplyAction` also clears `Pawn.MoveTarget`** whenever it has a fresh
  answer to apply. This was not in the first version and had to be added
  after live testing showed why: Botpack's bot AI does not move a Pawn purely
  by physics integrating `Acceleration` the way a human-controlled Pawn does
  — it walks toward `MoveTarget` (a `NavigationPoint`) using its own native
  pathing, independent of what `Acceleration` says. Overwriting `Acceleration`
  alone left one bot settling to a stop while another kept walking its patrol
  route at full speed despite receiving continuous zero actions. Clearing
  `MoveTarget` every tick removes the thing that pathing was walking toward,
  after which `Acceleration` is what's actually left driving movement — and
  it froze cleanly. See [the honest verdict](#the-honest-verdict) for the one
  side effect this has.

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

**It IS practical, with software arithmetic.** `GBMath.FloatToBits`/
`BitsToFloat` implement the textbook software IEEE-754 single-precision codec
— normalise the magnitude into `[1,2)` by repeated multiply/divide by 2 (at
most ~11 iterations for the value ranges this schema uses), then assemble
sign/exponent/23-bit-mantissa with integer bitwise operators, which
UnrealScript **does** have (`<<`, `>>>`, `&`, `|` — confirmed via `ucc
packagedump Core`'s operator table, not assumed). NaN/Inf are not specially
encoded on the way out (nothing this adapter sends is ever non-finite) or
specially decoded on the way in (a garbage exponent from a half-trained
policy decodes to *some* finite float, which `GBMutator.OnAction` clamps
immediately after — the same distrust `gb_client.c`'s `gb_clamp()` applies on
every other engine).

This has now been exercised through a real, live, bidirectional round trip
(see [What's verified](#whats-verified-live)), not just compiled in
isolation.

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

## History: TCP, then raw binary UDP, both dead ends on this build

Kept in full because someone will otherwise repeat the investigation.

### 1. TcpLink never connects

`TcpLink` (the obvious first choice, since the wire protocol was originally
TCP-only) never completes an outbound connect on this OldUnreal 469e Linux
dedicated server. `ss -tn` watched continuously across a live server's
`RECONNECT_COOLDOWN` retry cycle showed **not even a SYN packet** leaving the
process, toward loopback or the LAN interface, across every documented
client pattern tried: `StringToIpAddr()`+`Open()`, `Resolve()`+
`event Resolved()`+`Open()`, with and without `BindPort()` first, with and
without `bAlwaysTick=True` on the link actor. `Open()` reported success every
time and `IsConnected()` even transiently returned true long enough to start
an exchange, but the destination (`policyd.py`, confirmed listening) never
saw a connection attempt at the OS level.

The decisive counter-evidence that ruled out "UT99 can't do networking" as
the explanation: **the live `ut99-server` itself answers a 489-byte GameSpy
query on UDP 7798**, through `IpDrv`'s *other* link class. Real send-and-real
receive, inside this exact process — so the blocker was `TcpLink`
specifically, and `UdpLink` was the obvious next thing to try.

### 2. UdpLink sends real datagrams, but `ReceivedBinary` never delivers content

`UdpLink.SendBinary`/`SendText` both genuinely put bytes on the wire —
confirmed byte-for-byte correct on a receiving Python socket, up to and
including the exact byte pattern sent. But `UdpLink.ReceivedBinary`'s `B`
byte-array parameter **never carries real payload content** on this build:

- `Count` correctly reports the true datagram size on every single reply
  (measured for reply sizes 64, 200, 254, 255, 256, 300, and 500 bytes,
  clipping to 255 as expected once the payload exceeds the array size — but
  the size tracking itself is always right).
- `B` is uninitialised memory regardless of size — verified by having a
  Python peer reply with a known, easily recognised incrementing byte
  pattern (`byte[i] = i % 256`) and searching the *entire* 255-byte buffer
  for that pattern on the UnrealScript side: it never appears, at any offset,
  for any tested reply size, including ones well under the 255-byte cap
  where truncation cannot be the explanation. What comes back instead looks
  like stack/heap addresses (`...fd7f0000...`, a classic x86-64 pointer
  shape), not payload.
- Calling `ReadBinary()` **manually, immediately, from inside the
  `ReceivedBinary` event** — in case the event was only a "data is ready"
  notification and the real bytes needed a separate pull — also returned
  nothing (`n=0`, buffer unchanged).
- Switching `ReceiveMode` to `RMODE_Manual` and polling `ReadBinary()` from
  `Tick()` instead of relying on the event at all: same result, `n` never
  became positive even though the Python peer's reply had genuinely arrived
  (confirmed on the Python side).

`SendText`/`ReceivedText`, tried next, round-trip content **correctly** in
both directions — this is the one that actually works, and this adapter uses
it. `SendText` delivered at least 25,000 characters intact in one call
(measured); `ReceivedText` truncates to exactly **4095** characters no matter
how much more was sent (also measured, not assumed) — comfortably above this
adapter's largest reply (a 16-bot action batch is 800 hex characters) and
irrelevant to the request direction, since `SendText` has no such ceiling
this adapter would ever reach (its largest request, MAX_BOTS=16, is 18,592
hex characters).

So the wire is: pack the request exactly as `schema.py`/`gb_client.c` define
it, hex-encode every byte to two ASCII characters, and send the whole thing
in one `SendText` call — no chunking, since a datagram is the frame either
way and this build's practical string-length ceiling for a *reply* (4095
chars) is what actually constrains batch size, not the schema or the
transport. `policyd.py --udp-listen` now accepts either raw binary (every
other UDP client, unaffected) or this hex-text encoding, and replies in
whichever encoding the request arrived in.

## What's verified live

On a real OldUnreal 469e dedicated server (a separate instance on port 7900,
never the live `ut99-server` unit), against a real `policyd.py --udp-listen`:

- `GameBots.u` compiles clean (`ucc make`, 0 errors, 0 warnings).
- **Off by default**, verified with a live control: with `bEnabled=False` the
  mutator loads (`Add mutator GameBots.GBMutator` in the server log) and does
  precisely nothing — no `gamebots(ut99):` log line, `\status\` UDP query
  reports `numplayers\0`.
- **The full round trip works.** With `bEnabled=True` and a `--policy noop`
  policyd running, the server log shows the real transition:
  `gamebots(ut99): spawned 3/3 GBBot(s)` →
  `gamebots(ut99): policy server answering, driving 3 bot(s)` — and stays in
  that state, with per-bot debug lines (`bot 0 fwd=0.000000 side=0.000000
  buttons=0`) showing the exact zero action a no-op policy is supposed to
  answer with. `policyd.py`'s own periodic stats line shows a **climbing
  decision count** across the run (14 → 64 → 114 → 135 requests, then a
  second run climbing again 135 → 178 → 228 → 270) — real, continuous,
  bidirectional traffic, not a one-off handshake.
- **The control experiment, both halves, on the same three bots:** a
  TEST-ONLY harness (not committed — it directly calls
  `GBMutator.Mutate("gb_enable 0", None)`, the same function the real
  `mutate` console command dispatches to, so the toggle could be scripted
  without an attached console) logged each bot's `Location`/`Velocity` once
  a second, enabled the whole time, and flipped `gb_enable` off partway
  through:
    - **Frozen (noop policy driving, `MoveTarget` cleared):** all three bots
      stopped moving within ~5 seconds of being driven — two settled to an
      exact fixed point and stayed there (position and velocity essentially
      unchanged) for the remaining ~10 seconds of that phase; the third
      stopped instantly (velocity `0.000000` from the first sampled tick
      onward).
    - **Moving again (mutator disabled):** two of the three bots resumed
      active, full-speed (`vel≈400`) patrol movement within one sampled tick
      of the toggle firing, and kept moving in a normal patrol pattern for
      the rest of the test.
    - **The one honest wrinkle:** the third bot did *not* resume moving
      within the observed window (up to 40+ seconds after being released).
      Control was genuinely relinquished — `ActHave` was cleared and
      `Brain` was set to `None`, exactly as disabling does for every bot —
      but that bot had been walking with no `MoveTarget` for 15 seconds
      under the freeze and evidently ended up somewhere Botpack's own
      pathing did not visibly recover a route from in the time observed.
      This reads as a real, explainable side effect of repeatedly clearing
      `MoveTarget` — a bot with no target for that long is not guaranteed to
      end up somewhere convenient for its own AI to resume from — rather
      than a sign that "disabled" fails to relinquish control. Worth
      knowing before relying on this for anything beyond that same 5-15
      second class of test.

This is the bar the Quake III and Quake 2 adapters both met, on the transport
that turned out to actually work on this build.

## Why 10 Hz, not the server's own tick rate

`UdpLink` is asynchronous — `SendText()` never blocks, and a reply arrives
later as a `ReceivedText` event whenever the engine's network tick delivers
it. There is no call in UnrealScript that "sends and waits", so `GBMutator`
paces itself to a configurable `TickRate` (10 Hz default, matching Quake
III's `sv_fps 20`-ish order of magnitude) rather than trying to exchange on
every server frame. UDP being connectionless removes the reconnect/backoff
question the TCP design needed: there is nothing to reconnect. "Are we being
answered" is judged purely from how long it has been since
`GBLink.LastGoodReplyTime` — if that exceeds `ResponseTimeout`, every bot's
`ActHave` is cleared (relinquishing control back to `Super.Tick()`) and the
fallback state is reported.

## Observation — what's filled, what's zero and why

Mirrors the Quake III adapter's honesty table:

| group | status |
|---|---|
| health, alive, on-ground, crouching, in-water, velocity (ego frame), speed, pitch, view-relative geometry rays (`Actor.Trace`, `bTraceActors=True`) | **filled** |
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
- **Movement**: `Pawn.MoveTarget` is cleared (see
  [How it hooks in](#how-it-hooks-in) for why), then `forward`/`side` combine
  with the (yaw-only) view basis via `GetAxes()` into a wish direction,
  scaled by `Pawn.AccelRate` and written to `Pawn.Acceleration`.
- **Buttons**: `bFire`/`bAltFire`/`bDuck` are **byte** properties on `Pawn`
  despite the `b`-prefix naming convention (confirmed via packagedump — this
  cost real debugging time, see `GBMutator.HasButton`'s comment), set
  directly from the action's button bitmask.
- **Clamping**: `GBMutator.OnAction` clamps every float (pitch/yaw delta to
  the schema's bounds, forward/side to ±1) and explicitly tests for NaN
  (`v != v`) before anything else touches it — same distrust
  `gb_client.c`'s `gb_clamp()` applies on every other engine, and the thing
  that makes `GBMath`'s un-decoded NaN case safe.

## The honest verdict

**UnrealScript is a viable path for UT99 bots, not a dead end** — with the
right transport. `TcpLink` was a dead end on this build; raw binary over
`UdpLink` was a dead end on this build for a completely different reason;
hex-text over `UdpLink` works, verified end to end, live, with a real policy
server on the other end and a real control experiment on real bots.

The one open item is the `MoveTarget`-clearing side effect documented above
— worth understanding before extending the freeze window much past what was
tested here, or before relying on "disabled always means every bot resumes
instantly." A more targeted fix (e.g. handing the bot a fresh, nearby
`NavigationPoint` on release instead of leaving it to reacquire one on its
own) is a reasonable next step if that matters for a given use, but wasn't
needed to prove the design.

## Known incompleteness worth fixing next

- **Bot player-count registration.** `\status\` UDP queries report
  `numplayers\0` even with 3 `GBBot`s alive and running Botpack AI. This
  adapter spawns bots via `Spawn(class'GBBot', ...)` + `RestartPlayer()`
  directly, bypassing `GameInfo.AddBot()`'s own bookkeeping — plausibly this
  means `PlayerReplicationInfo` isn't fully wired the way a "real" player's
  is, which would also silently zero the `score_diff_norm`/teammate-detection
  paths in `BuildGameContext`/`BuildEntities` for these bots (both already
  guard on `PlayerReplicationInfo != None`, so they fail safe to zero rather
  than crash, but the *feature* is likely not working).
- **The `MoveTarget` release side effect** described above.
- **`ammo_frac`** left at zero (see the observation table) — worth another
  attempt at `FindInventoryType`'s real parameter shape if ammo awareness
  ever matters for a trained policy.

## Tests

```bash
python3 -m pytest tests/python/test_gamebots_ut99.py
python3 -m pytest tests/python/test_gamebots_policyd.py -k udp
```

Source-level checks only (no `ucc` in CI): schema constants match
`schema.py` exactly, `GBMutator` defaults to disabled, the fallback call
(`Super.Tick()` before any override) is present in `GBBot.uc`, the adapter
targets a UDP endpoint via `UdpLink`, and `GBSchema.uc` is byte-for-byte what
`gen_gbschema.py` would emit right now (the same "generated file, diffed"
discipline as the C header's test). The `policyd.py` side has its own tests
for the hex-text UDP encoding (`test_gamebots_policyd.py -k udp`), including
that a raw-binary client and a hex-text client are served independently and
correctly at the same time.
