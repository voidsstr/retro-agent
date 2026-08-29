#!/usr/bin/env python3
"""Observation/action schema for neural game bots — the single source of truth.

Every engine adapter and every policy speaks THIS, and nothing else. The whole
point of the design is that a policy trained against Quake III can be pointed
at a GoldSrc server without retraining the input layer, so the schema is
defined once, here, and the C header the adapters compile against is
*generated* from this file rather than written alongside it.

Two rules make that stick:

  1. **The layout is hashed, and the hash is on the wire.** Every request
     carries it; the policy server rejects a mismatch loudly. A schema change
     that reaches half the system is otherwise silent -- the floats still
     unpack, they just mean different things, and the bot walks into a wall
     for reasons nobody can see. (Same lesson as retro-infer's .rim container
     refusing a mismatched model.)

  2. **The C header is generated, never hand-maintained.** `--emit-header`
     writes it; a test regenerates and diffs. Two hand-written copies of a
     field table drift, and this repo has been bitten by exactly that before
     (agent/shared/ exists for the same reason).

Everything is ego-centric and rotation-normalised: positions are relative to
the bot and expressed in its own frame, distances are normalised. A policy
that only ever sees "enemy 0.3 ahead and 0.1 right" transfers between maps and
between engines; one that sees world coordinates memorises q3dm7.

Usage:
    python3 schema.py --emit-header > gamebots_schema.h
    python3 schema.py --describe          # human-readable field table
    python3 schema.py --hash              # just the schema hash
"""

import argparse
import hashlib
import struct
import sys

SCHEMA_VERSION = 1

# Number of other-entity slots the bot can see at once. 8 is a deliberate
# compromise: enough for a 4v4 plus a couple of projectiles, small enough that
# the observation stays under one cache line per bot on the C side.
MAX_ENTITIES = 8

# Raycasts around the bot for local geometry. 16 horizontal (every 22.5°) plus
# up and down. This is the cheap stand-in for a nav mesh: it tells the policy
# "wall on my left" without either side needing to agree on map topology.
NUM_RAYS_H = 16

# Width of the intent vector the LLM planner conditions the policy with (see
# docs/game-ai-bots-plan.md §4.3). Reserved from the start even though nothing
# writes it until Phase 4 -- adding it later would change the layout hash and
# invalidate every recorded demonstration.
INTENT_DIM = 16


def _fields():
    """(group, name, count, doc) — the canonical layout, in wire order."""
    f = []

    # --- the bot itself -----------------------------------------------------
    f += [
        ("self", "health_frac", 1, "health / max health, 0..1"),
        ("self", "armor_frac", 1, "armour / max armour, 0..1"),
        ("self", "ammo_frac", 1, "ammo in clip / clip size, 0..1"),
        ("self", "ammo_reserve_frac", 1, "reserve ammo, 0..1"),
        ("self", "weapon_id_norm", 1, "weapon index / max weapon index"),
        ("self", "vel_local", 3, "velocity in the bot's own frame (fwd,right,up), / max speed"),
        ("self", "speed_frac", 1, "|velocity| / max ground speed"),
        ("self", "pitch_norm", 1, "view pitch / 90deg, -1..1"),
        ("self", "on_ground", 1, "1 if standing on something"),
        ("self", "crouching", 1, "1 if ducked"),
        ("self", "in_water", 1, "1 if submerged"),
        ("self", "reloading", 1, "1 if mid-reload"),
        ("self", "alive", 1, "1 if alive (0 during respawn wait)"),
    ]

    # --- local geometry -----------------------------------------------------
    f += [
        ("geom", "ray_h", NUM_RAYS_H,
         f"{NUM_RAYS_H} horizontal raycast distances / far plane, "
         "starting straight ahead and going clockwise"),
        ("geom", "ray_up", 1, "distance to ceiling / far plane"),
        ("geom", "ray_down", 1, "distance to floor / far plane (step/ledge sensing)"),
    ]

    # --- other entities -----------------------------------------------------
    # Sorted by threat (nearest visible enemy first) by the ADAPTER, not the
    # policy: a stable, meaningful slot order is what lets a small net learn
    # "slot 0 is the thing about to kill me" instead of burning capacity on
    # permutation invariance.
    for i in range(MAX_ENTITIES):
        f += [
            ("ent", f"e{i}_present", 1, "1 if this slot holds an entity"),
            ("ent", f"e{i}_is_teammate", 1, "1 teammate, 0 enemy/neutral"),
            ("ent", f"e{i}_dir", 3, "unit vector to it in the bot's frame"),
            ("ent", f"e{i}_dist_norm", 1, "distance / far plane"),
            ("ent", f"e{i}_rel_vel", 2, "its velocity relative to us (fwd,right) / max speed"),
            ("ent", f"e{i}_health_frac", 1, "its health 0..1 (0 if unknown)"),
            ("ent", f"e{i}_visible", 1, "1 if we currently have line of sight"),
        ]

    # --- what just happened to us ------------------------------------------
    f += [
        ("event", "took_damage", 1, "damage taken last tick / max health"),
        ("event", "damage_dir", 2, "unit direction the damage came from (fwd,right)"),
        ("event", "killed_someone", 1, "1 if we got a kill last tick"),
        ("event", "died", 1, "1 if we died last tick"),
    ]

    # --- match context ------------------------------------------------------
    f += [
        ("game", "round_time_frac", 1, "elapsed / round length, 0..1"),
        ("game", "score_diff_norm", 1, "(our score - theirs) / fraglimit, clamped -1..1"),
        ("game", "teammates_alive_frac", 1, "alive teammates / team size"),
        ("game", "enemies_alive_frac", 1, "alive enemies / team size"),
        ("game", "objective", 2, "mode-specific (bomb planted, flag carried, ...)"),
    ]

    # --- planner conditioning ----------------------------------------------
    f += [("intent", "intent", INTENT_DIM,
           "intent vector from the LLM planner (all zeros = no plan)")]

    return f


FIELDS = _fields()


def _offsets():
    off, out = 0, []
    for group, name, count, doc in FIELDS:
        out.append((group, name, off, count, doc))
        off += count
    return out, off


FIELD_TABLE, _RAW_OBS_DIM = _offsets()

# Pad to a multiple of 8 floats. Costs 32 bytes per bot and keeps every
# observation 32-byte aligned for whatever SIMD or tensor core eventually
# reads it; unpadded dims have a way of becoming permanent.
OBS_DIM = (_RAW_OBS_DIM + 7) // 8 * 8
OBS_PAD = OBS_DIM - _RAW_OBS_DIM


# --------------------------------------------------------------------------
# actions
# --------------------------------------------------------------------------
#
# Deliberately shaped to drop straight into both GoldSrc's and Quake's
# usercmd_t -- they share ancestry, which is why one action space can drive
# both. View movement is a DELTA, not an absolute angle: absolute aim would let
# a policy teleport its crosshair, which is both unfair and unlearnable-looking.

BTN_ATTACK = 1 << 0
BTN_ATTACK2 = 1 << 1
BTN_JUMP = 1 << 2
BTN_CROUCH = 1 << 3
BTN_RELOAD = 1 << 4
BTN_USE = 1 << 5
BTN_WALK = 1 << 6      # move quietly / slowly
BTN_ZOOM = 1 << 7

BUTTON_NAMES = ["attack", "attack2", "jump", "crouch",
                "reload", "use", "walk", "zoom"]

# Per-tick view change is capped so a policy cannot produce a physically
# impossible flick. Tuned to be generous (a fast human flick is ~30°/tick at
# 30Hz) while still bounding the worst case.
MAX_PITCH_DELTA_DEG = 25.0
MAX_YAW_DELTA_DEG = 40.0

# bot_id u16, buttons u16, pitch/yaw/fwd/side f32, weapon u8, pad u8, reserved u16
ACTION_STRUCT = "<HHffffBBH"
ACTION_SIZE = struct.calcsize(ACTION_STRUCT)


# --------------------------------------------------------------------------
# wire protocol
# --------------------------------------------------------------------------

REQ_MAGIC = b"GBQ1"    # game-bot query
RESP_MAGIC = b"GBA1"   # game-bot action

# magic, schema hash, bot count, flags, tick
HEADER_STRUCT = "<4sIHHI"
HEADER_SIZE = struct.calcsize(HEADER_STRUCT)

# bot_id u16, pad u16, then OBS_DIM floats
OBS_ENTRY_STRUCT = f"<HH{OBS_DIM}f"
OBS_ENTRY_SIZE = struct.calcsize(OBS_ENTRY_STRUCT)

FLAG_NONE = 0
FLAG_TRAINING = 1 << 0     # adapter is recording; policy may explore
FLAG_PAUSED = 1 << 1       # server paused/warmup; hold still


def schema_hash():
    """Stable 32-bit hash of the layout. On the wire in every request.

    Covers field names, widths and order, plus the action encoding -- anything
    whose change would silently reinterpret bytes. Deliberately NOT covering
    doc strings, so improving a comment does not invalidate a dataset.
    """
    h = hashlib.sha256()
    h.update(f"gamebots-schema-v{SCHEMA_VERSION}\n".encode())
    for group, name, off, count, _doc in FIELD_TABLE:
        h.update(f"{group}.{name}@{off}x{count}\n".encode())
    h.update(f"obs_dim={OBS_DIM}\n".encode())
    h.update(f"action={ACTION_STRUCT}\n".encode())
    h.update(("buttons=" + ",".join(BUTTON_NAMES) + "\n").encode())
    return int.from_bytes(h.digest()[:4], "little")


SCHEMA_HASH = schema_hash()


# --------------------------------------------------------------------------
# pack / unpack
# --------------------------------------------------------------------------

def pack_request(tick, entries, flags=FLAG_NONE):
    """entries: iterable of (bot_id, obs sequence of OBS_DIM floats)."""
    entries = list(entries)
    out = bytearray(struct.pack(HEADER_STRUCT, REQ_MAGIC, SCHEMA_HASH,
                                len(entries), flags, tick & 0xFFFFFFFF))
    for bot_id, obs in entries:
        if len(obs) != OBS_DIM:
            raise ValueError(f"obs for bot {bot_id} has {len(obs)} floats, "
                             f"expected {OBS_DIM}")
        out += struct.pack(OBS_ENTRY_STRUCT, bot_id, 0, *obs)
    return bytes(out)


def unpack_request(buf):
    """-> (tick, flags, [(bot_id, [floats]), ...]). Raises on any mismatch."""
    if len(buf) < HEADER_SIZE:
        raise ValueError(f"short request: {len(buf)} bytes")
    magic, hsh, n, flags, tick = struct.unpack_from(HEADER_STRUCT, buf, 0)
    if magic != REQ_MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {REQ_MAGIC!r}")
    if hsh != SCHEMA_HASH:
        # The single most valuable error message in the system: an adapter
        # built against a different field table is otherwise undetectable.
        raise ValueError(
            f"schema hash mismatch: adapter sent {hsh:#010x}, "
            f"policy server has {SCHEMA_HASH:#010x} — rebuild the adapter "
            f"against the current gamebots_schema.h")
    want = HEADER_SIZE + n * OBS_ENTRY_SIZE
    if len(buf) != want:
        raise ValueError(f"request is {len(buf)} bytes, expected {want} "
                         f"for {n} bots")
    entries = []
    off = HEADER_SIZE
    for _ in range(n):
        vals = struct.unpack_from(OBS_ENTRY_STRUCT, buf, off)
        entries.append((vals[0], list(vals[2:])))
        off += OBS_ENTRY_SIZE
    return tick, flags, entries


def pack_response(tick, actions, flags=FLAG_NONE):
    """actions: iterable of (bot_id, buttons, pitch, yaw, fwd, side, weapon)."""
    actions = list(actions)
    out = bytearray(struct.pack(HEADER_STRUCT, RESP_MAGIC, SCHEMA_HASH,
                                len(actions), flags, tick & 0xFFFFFFFF))
    for bot_id, buttons, pitch, yaw, fwd, side, weapon in actions:
        out += struct.pack(ACTION_STRUCT, bot_id, buttons,
                           pitch, yaw, fwd, side, weapon, 0, 0)
    return bytes(out)


def unpack_response(buf):
    if len(buf) < HEADER_SIZE:
        raise ValueError(f"short response: {len(buf)} bytes")
    magic, hsh, n, flags, tick = struct.unpack_from(HEADER_STRUCT, buf, 0)
    if magic != RESP_MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {RESP_MAGIC!r}")
    if hsh != SCHEMA_HASH:
        raise ValueError(f"schema hash mismatch in response: {hsh:#010x}")
    out, off = [], HEADER_SIZE
    for _ in range(n):
        (bot_id, buttons, pitch, yaw, fwd, side,
         weapon, _p, _r) = struct.unpack_from(ACTION_STRUCT, buf, off)
        out.append((bot_id, buttons, pitch, yaw, fwd, side, weapon))
        off += ACTION_SIZE
    return tick, flags, out


def clamp_action(pitch, yaw, fwd, side):
    """Bound an action to what a body can actually do.

    Applied on the SERVER side, not trusted from the policy: a half-trained
    net emits NaN and garbage long before it emits good play, and an adapter
    that passes that through will make the game server do something strange.
    """
    def fin(v, lo, hi):
        # NaN fails every comparison, so test for it explicitly rather than
        # relying on min/max to filter it.
        if v != v:
            return 0.0
        return lo if v < lo else (hi if v > hi else v)
    return (fin(pitch, -MAX_PITCH_DELTA_DEG, MAX_PITCH_DELTA_DEG),
            fin(yaw, -MAX_YAW_DELTA_DEG, MAX_YAW_DELTA_DEG),
            fin(fwd, -1.0, 1.0),
            fin(side, -1.0, 1.0))


# --------------------------------------------------------------------------
# numpy fast path
# --------------------------------------------------------------------------
#
# The pure-Python struct path costs ~2.8 us/bot, which at 512 bots is 1.4 ms of
# a frame -- an order of magnitude more than the GPU forward pass it exists to
# feed. These read and write the whole batch as one typed memory view instead,
# which is a memcpy and a reinterpret.
#
# numpy is OPTIONAL on purpose: the Phase 0 harness and the tests must keep
# working on a host with no ML stack at all, so this degrades to the struct
# path rather than becoming a hard dependency.

try:
    import numpy as _np

    OBS_ENTRY_DTYPE = _np.dtype([
        ("bot_id", "<u2"), ("pad", "<u2"), ("obs", "<f4", (OBS_DIM,)),
    ])
    ACTION_DTYPE = _np.dtype([
        ("bot_id", "<u2"), ("buttons", "<u2"),
        ("pitch", "<f4"), ("yaw", "<f4"), ("fwd", "<f4"), ("side", "<f4"),
        ("weapon", "u1"), ("pad0", "u1"), ("reserved", "<u2"),
    ])
    HAVE_NUMPY = True

    # If these ever disagree with the struct formats the two paths would write
    # different bytes for the same batch, which is the kind of divergence that
    # only shows up as a bot behaving oddly on one code path.
    assert OBS_ENTRY_DTYPE.itemsize == OBS_ENTRY_SIZE, (
        f"numpy obs entry is {OBS_ENTRY_DTYPE.itemsize} B, "
        f"struct says {OBS_ENTRY_SIZE} B")
    assert ACTION_DTYPE.itemsize == ACTION_SIZE, (
        f"numpy action is {ACTION_DTYPE.itemsize} B, "
        f"struct says {ACTION_SIZE} B")

except ImportError:  # pragma: no cover - exercised on hosts without numpy
    _np = None
    HAVE_NUMPY = False
    OBS_ENTRY_DTYPE = ACTION_DTYPE = None


def unpack_request_fast(buf):
    """-> (tick, flags, bot_ids ndarray, obs ndarray (n, OBS_DIM) float32).

    Validates exactly what the slow path validates -- magic, schema hash and
    length -- because a fast path that skips checks is how a stale adapter gets
    to feed a model silently misaligned floats.
    """
    if not HAVE_NUMPY:
        raise RuntimeError("numpy is not installed")
    if len(buf) < HEADER_SIZE:
        raise ValueError(f"short request: {len(buf)} bytes")
    magic, hsh, n, flags, tick = struct.unpack_from(HEADER_STRUCT, buf, 0)
    if magic != REQ_MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {REQ_MAGIC!r}")
    if hsh != SCHEMA_HASH:
        raise ValueError(
            f"schema hash mismatch: adapter sent {hsh:#010x}, "
            f"policy server has {SCHEMA_HASH:#010x} — rebuild the adapter "
            f"against the current gamebots_schema.h")
    want = HEADER_SIZE + n * OBS_ENTRY_SIZE
    if len(buf) != want:
        raise ValueError(f"request is {len(buf)} bytes, expected {want} "
                         f"for {n} bots")
    arr = _np.frombuffer(buf, dtype=OBS_ENTRY_DTYPE, count=n,
                         offset=HEADER_SIZE)
    # `arr["obs"]` is a strided view over the 580-byte records; make it
    # contiguous once so the tensor wrapping it is a straight memcpy.
    return tick, flags, arr["bot_id"], _np.ascontiguousarray(arr["obs"])


def pack_response_fast(tick, bot_ids, buttons, pitch, yaw, fwd, side, weapon,
                       flags=FLAG_NONE):
    """Build a response from parallel arrays, one buffer, no per-bot Python."""
    if not HAVE_NUMPY:
        raise RuntimeError("numpy is not installed")
    n = len(bot_ids)
    out = _np.empty(n, dtype=ACTION_DTYPE)
    out["bot_id"] = bot_ids
    out["buttons"] = buttons
    out["pitch"] = pitch
    out["yaw"] = yaw
    out["fwd"] = fwd
    out["side"] = side
    out["weapon"] = weapon
    out["pad0"] = 0
    out["reserved"] = 0
    return struct.pack(HEADER_STRUCT, RESP_MAGIC, SCHEMA_HASH, n, flags,
                       tick & 0xFFFFFFFF) + out.tobytes()


def clamp_actions_inplace(pitch, yaw, fwd, side):
    """Vectorised clamp_action, including the NaN handling.

    `np.clip` propagates NaN, so the non-finite values have to be replaced
    first -- exactly the trap the scalar version documents.
    """
    for arr, lo, hi in ((pitch, -MAX_PITCH_DELTA_DEG, MAX_PITCH_DELTA_DEG),
                        (yaw, -MAX_YAW_DELTA_DEG, MAX_YAW_DELTA_DEG),
                        (fwd, -1.0, 1.0), (side, -1.0, 1.0)):
        _np.nan_to_num(arr, copy=False, nan=0.0, posinf=hi, neginf=lo)
        _np.clip(arr, lo, hi, out=arr)
    return pitch, yaw, fwd, side


# --------------------------------------------------------------------------
# C header generation
# --------------------------------------------------------------------------

def emit_header():
    L = []
    a = L.append
    a("/* gamebots_schema.h — GENERATED by scripts/gamebots/schema.py.")
    a(" *")
    a(" * Do not edit. Regenerate with:")
    a(" *     python3 scripts/gamebots/schema.py --emit-header \\")
    a(" *         > scripts/gamebots/gamebots_schema.h")
    a(" *")
    a(" * Engine adapters compile against this. The schema hash below goes out")
    a(" * in every request and the policy server refuses a mismatch, so an")
    a(" * adapter built against a stale header fails loudly at the first tick")
    a(" * instead of quietly feeding the policy misaligned floats.")
    a(" */")
    a("#ifndef GAMEBOTS_SCHEMA_H")
    a("#define GAMEBOTS_SCHEMA_H")
    a("")
    a("#include <stdint.h>")
    a("")
    a(f"#define GB_SCHEMA_VERSION {SCHEMA_VERSION}")
    a(f"#define GB_SCHEMA_HASH    0x{SCHEMA_HASH:08x}u")
    a(f"#define GB_OBS_DIM        {OBS_DIM}")
    a(f"#define GB_MAX_ENTITIES   {MAX_ENTITIES}")
    a(f"#define GB_NUM_RAYS_H     {NUM_RAYS_H}")
    a(f"#define GB_INTENT_DIM     {INTENT_DIM}")
    a("")
    a('#define GB_REQ_MAGIC  "GBQ1"')
    a('#define GB_RESP_MAGIC "GBA1"')
    a("")
    a("/* --- buttons --- */")
    for i, name in enumerate(BUTTON_NAMES):
        a(f"#define GB_BTN_{name.upper():<8} (1u << {i})")
    a("")
    a("/* --- request flags --- */")
    a("#define GB_FLAG_NONE     0u")
    a(f"#define GB_FLAG_TRAINING (1u << 0)")
    a(f"#define GB_FLAG_PAUSED   (1u << 1)")
    a("")
    a("/* --- action bounds (enforce on the server, never trust the policy) --- */")
    a(f"#define GB_MAX_PITCH_DELTA_DEG {MAX_PITCH_DELTA_DEG}f")
    a(f"#define GB_MAX_YAW_DELTA_DEG   {MAX_YAW_DELTA_DEG}f")
    a("")
    a("/* --- observation field offsets --- */")
    group = None
    for g, name, off, count, doc in FIELD_TABLE:
        if g != group:
            a(f"/* {g} */")
            group = g
        macro = f"GB_OBS_{name.upper()}"
        a(f"#define {macro:<28} {off:>4}  /* x{count}: {doc} */")
    if OBS_PAD:
        a(f"#define {'GB_OBS_PAD':<28} {_RAW_OBS_DIM:>4}  /* x{OBS_PAD}: "
          f"alignment padding, always zero */")
    a("")
    a("#pragma pack(push, 1)")
    a("typedef struct {")
    a("    char     magic[4];")
    a("    uint32_t schema_hash;")
    a("    uint16_t n_bots;")
    a("    uint16_t flags;")
    a("    uint32_t tick;")
    a("} gb_header_t;")
    a("")
    a("typedef struct {")
    a("    uint16_t bot_id;")
    a("    uint16_t pad;")
    a("    float    obs[GB_OBS_DIM];")
    a("} gb_obs_entry_t;")
    a("")
    a("typedef struct {")
    a("    uint16_t bot_id;")
    a("    uint16_t buttons;")
    a("    float    pitch_delta;   /* degrees, clamped */")
    a("    float    yaw_delta;     /* degrees, clamped */")
    a("    float    forward;       /* -1..1 */")
    a("    float    side;          /* -1..1 */")
    a("    uint8_t  weapon;        /* 0 = no change */")
    a("    uint8_t  pad0;")
    a("    uint16_t reserved;")
    a("} gb_action_t;")
    a("#pragma pack(pop)")
    a("")
    a(f"/* sanity: these must match the Python side exactly */")
    a("typedef char gb_static_assert_header"
      f"[(sizeof(gb_header_t) == {HEADER_SIZE}) ? 1 : -1];")
    a("typedef char gb_static_assert_obs"
      f"[(sizeof(gb_obs_entry_t) == {OBS_ENTRY_SIZE}) ? 1 : -1];")
    a("typedef char gb_static_assert_action"
      f"[(sizeof(gb_action_t) == {ACTION_SIZE}) ? 1 : -1];")
    a("")
    a("#endif /* GAMEBOTS_SCHEMA_H */")
    return "\n".join(L) + "\n"


def describe():
    L = [f"gamebots schema v{SCHEMA_VERSION}  hash={SCHEMA_HASH:#010x}",
         f"observation: {_RAW_OBS_DIM} fields + {OBS_PAD} pad = {OBS_DIM} floats "
         f"({OBS_DIM * 4} bytes)",
         f"action: {ACTION_SIZE} bytes   header: {HEADER_SIZE} bytes",
         f"per-bot on the wire: {OBS_ENTRY_SIZE} B request / {ACTION_SIZE} B response",
         ""]
    group = None
    for g, name, off, count, doc in FIELD_TABLE:
        if g != group:
            L.append(f"[{g}]")
            group = g
        L.append(f"  {off:>4} +{count:<3} {name:<22} {doc}")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--emit-header", action="store_true")
    ap.add_argument("--describe", action="store_true")
    ap.add_argument("--hash", action="store_true")
    args = ap.parse_args()
    if args.emit_header:
        sys.stdout.write(emit_header())
    elif args.hash:
        print(f"{SCHEMA_HASH:#010x}")
    else:
        print(describe())
    return 0


if __name__ == "__main__":
    sys.exit(main())
