#!/usr/bin/env python3
"""The demonstration container format — a self-describing, append-only shard.

Phase 2's whole point is a corpus, so the format gets the same discipline the
rest of this system already applies:

  1. **The schema hash is embedded and checked on read**, same as the wire
     protocol (`schema.py`) and the checkpoint container (`runtime.py`). A
     shard recorded against a different observation layout is refused, not
     silently misread — the alternative is a policy that trains on floats that
     mean something else and no error anywhere.
  2. **Append-only, fixed-size records, no per-record framing.** A shard is
     one small header (`gbdemo1`, magic + schema hash + obs_dim + who recorded
     it) followed by fixed-size records back to back. That means a reader can
     validate a shard from its file size alone — `(size - header) % record_size`
     is the truncation check — and a writer never has to seek back to patch a
     count when it crashes mid-write, because there is no count to patch.
  3. **Two paths, same bytes, same shape as `schema.py`.** A pure Python/struct
     path always works (used by the header, by `write_record`/`load_shard_python`,
     and by every host without numpy). A numpy path (`write_batch`/`load_shard`)
     is the one the hot serving path and the trainer actually use, because a
     Python `struct.pack` per bot at 512 bots/frame is exactly the cost Phase 0
     measured and removed from the wire protocol. An assertion pins the two
     paths to identical byte layouts, so they can never quietly diverge.

One file = one shard = `<prefix>-<pid>-<seq>.gbdemo`. A recording session
rolls to a new shard every `max_records_per_shard` records, so no single file
grows without bound and a crash loses at most one shard's worth of buffering.

Usage:
    python3 shard.py --describe some.gbdemo
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import schema  # noqa: E402

# --------------------------------------------------------------------------
# header
# --------------------------------------------------------------------------

MAGIC = b"GBD1"          # game-bot demo, format v1
FORMAT_VERSION = 1        # the RECORD layout version -- independent of the
                          # observation schema version, so the demo container
                          # can evolve (e.g. add a field) without touching
                          # schema.py, and vice versa.

# magic, format_version, schema_hash, obs_dim, created_ts, reserved, policy_name
HEADER_STRUCT = "<4sIIIdI32s"
HEADER_SIZE = struct.calcsize(HEADER_STRUCT)

POLICY_NAME_MAX = 32


class ShardHeader:
    __slots__ = ("format_version", "schema_hash", "obs_dim", "created_ts",
                 "policy_name")

    def __init__(self, format_version, schema_hash, obs_dim, created_ts,
                 policy_name):
        self.format_version = format_version
        self.schema_hash = schema_hash
        self.obs_dim = obs_dim
        self.created_ts = created_ts
        self.policy_name = policy_name

    def __repr__(self):
        return (f"ShardHeader(format_version={self.format_version}, "
                f"schema_hash={self.schema_hash:#010x}, obs_dim={self.obs_dim}, "
                f"created_ts={self.created_ts:.0f}, "
                f"policy_name={self.policy_name!r})")


def write_header(fh, schema_hash, obs_dim, policy_name="unknown"):
    name_b = policy_name.encode("utf-8")[:POLICY_NAME_MAX]
    name_b = name_b.ljust(POLICY_NAME_MAX, b"\0")
    fh.write(struct.pack(HEADER_STRUCT, MAGIC, FORMAT_VERSION, schema_hash,
                         obs_dim, time.time(), 0, name_b))


def read_header(fh):
    buf = fh.read(HEADER_SIZE)
    if len(buf) < HEADER_SIZE:
        raise ValueError(
            f"shard header is truncated: got {len(buf)} of {HEADER_SIZE} "
            f"bytes -- not a valid .gbdemo shard, or the write crashed before "
            f"the header finished")
    magic, fmt_ver, schema_hash, obs_dim, created_ts, _reserved, name_b = \
        struct.unpack(HEADER_STRUCT, buf)
    if magic != MAGIC:
        raise ValueError(f"bad shard magic {magic!r}, expected {MAGIC!r} -- "
                         f"this is not a gamebots demo shard")
    if fmt_ver != FORMAT_VERSION:
        raise ValueError(f"shard is demo-format v{fmt_ver}, this reader only "
                         f"understands v{FORMAT_VERSION}")
    return ShardHeader(fmt_ver, schema_hash, obs_dim, created_ts,
                       name_b.rstrip(b"\0").decode("utf-8", "replace"))


def _check_schema(header, strict_schema):
    """The single most valuable check in this module: a dataset recorded
    against a different observation layout must never be trained on. Same
    discipline as the wire hash in schema.py and the checkpoint hash in
    runtime.py."""
    if strict_schema and header.schema_hash != schema.SCHEMA_HASH:
        raise ValueError(
            f"shard was recorded against schema {header.schema_hash:#010x}, "
            f"this build is {schema.SCHEMA_HASH:#010x} -- training on it would "
            f"silently learn the wrong meaning for every field. Re-record "
            f"against the current schema, or pass strict_schema=False to "
            f"inspect it anyway.")


# --------------------------------------------------------------------------
# records — struct path (always available)
# --------------------------------------------------------------------------
#
# bot_id u16, episode_id u32, tick u32, done u8, obs[OBS_DIM] f32,
# buttons u16, pitch/yaw/fwd/side f32, weapon u8.
#
# episode_id and done exist so a FUTURE sequence-aware trainer can chunk a
# recording into episodes without changing the format: done=1 marks "this bot
# was not alive when this record was captured" and episode_id increments on
# every dead->alive transition for that bot. The BC trainer in train/bc.py
# does not use them yet (see its docstring for why) -- they are recorded now
# because adding them later would be a format version bump that invalidates
# every shard collected before it, the exact mistake schema.py's INTENT_DIM
# comment warns about.

RECORD_STRUCT = f"<HIIB{schema.OBS_DIM}fHffffB"
RECORD_SIZE = struct.calcsize(RECORD_STRUCT)

RECORD_FIELDS = ("bot_id", "episode_id", "tick", "done", "obs", "buttons",
                 "pitch", "yaw", "fwd", "side", "weapon")


def pack_record(bot_id, episode_id, tick, done, obs, buttons, pitch, yaw,
                fwd, side, weapon):
    if len(obs) != schema.OBS_DIM:
        raise ValueError(f"obs has {len(obs)} floats, expected {schema.OBS_DIM}")
    return struct.pack(RECORD_STRUCT, bot_id, episode_id, tick & 0xFFFFFFFF,
                       1 if done else 0, *obs, buttons, pitch, yaw, fwd, side,
                       weapon)


def unpack_record(buf, offset=0):
    vals = struct.unpack_from(RECORD_STRUCT, buf, offset)
    bot_id, episode_id, tick, done = vals[0:4]
    obs = vals[4:4 + schema.OBS_DIM]
    buttons, pitch, yaw, fwd, side, weapon = vals[4 + schema.OBS_DIM:]
    return {"bot_id": bot_id, "episode_id": episode_id, "tick": tick,
           "done": done, "obs": obs, "buttons": buttons, "pitch": pitch,
           "yaw": yaw, "fwd": fwd, "side": side, "weapon": weapon}


# --------------------------------------------------------------------------
# records — numpy fast path (optional, same reasoning as schema.py)
# --------------------------------------------------------------------------

try:
    import numpy as _np

    RECORD_DTYPE = _np.dtype([
        ("bot_id", "<u2"), ("episode_id", "<u4"), ("tick", "<u4"),
        ("done", "u1"), ("obs", "<f4", (schema.OBS_DIM,)),
        ("buttons", "<u2"), ("pitch", "<f4"), ("yaw", "<f4"), ("fwd", "<f4"),
        ("side", "<f4"), ("weapon", "u1"),
    ], align=False)
    HAVE_NUMPY = True

    # If these ever disagree, the numpy batch path and the struct path would
    # write different bytes for the same logical record -- exactly the
    # divergence schema.py's own dtype assertion exists to catch one layer up.
    assert RECORD_DTYPE.itemsize == RECORD_SIZE, (
        f"numpy record is {RECORD_DTYPE.itemsize} B, struct says "
        f"{RECORD_SIZE} B")

except ImportError:  # pragma: no cover - exercised on hosts without numpy
    _np = None
    RECORD_DTYPE = None
    HAVE_NUMPY = False


def record_array(n):
    """A fresh, uninitialised batch of n records — fill every field before
    writing (write_batch does not zero it for you)."""
    if not HAVE_NUMPY:
        raise RuntimeError("numpy is not installed")
    return _np.empty(n, dtype=RECORD_DTYPE)


# --------------------------------------------------------------------------
# inspection / truncation detection
# --------------------------------------------------------------------------

def inspect_shard(path, strict_schema=True):
    """(header, n_complete_records, trailing_bytes).

    No record is read to compute this -- it comes from the file size alone,
    which is what lets a crash-truncated shard (a process killed mid
    `write_batch`) be detected cheaply and unambiguously: the trailing bytes
    are whatever was left mid-record when the write stopped, always fewer
    than one full record, never mistaken for a complete one.
    """
    size = os.path.getsize(path)
    with open(path, "rb") as fh:
        header = read_header(fh)
    _check_schema(header, strict_schema)
    if header.obs_dim != schema.OBS_DIM:
        # Can only reach here with strict_schema=False and a hash collision,
        # or (should be impossible) a hash match with a different obs_dim.
        # Either way, computing record math with THIS build's RECORD_SIZE
        # against a shard shaped for a different obs_dim would silently
        # misread every record, so refuse rather than guess.
        raise ValueError(
            f"{path}: shard obs_dim={header.obs_dim} does not match this "
            f"build's obs_dim={schema.OBS_DIM} (schema {header.schema_hash:#010x} "
            f"vs {schema.SCHEMA_HASH:#010x}) -- cannot compute its record "
            f"layout safely")
    body = size - HEADER_SIZE
    if body < 0:
        raise ValueError(f"{path}: file ({size} bytes) is smaller than its "
                         f"own header ({HEADER_SIZE} bytes)")
    return header, body // RECORD_SIZE, body % RECORD_SIZE


def load_shard_python(path, strict_schema=True):
    """(header, [record dict, ...], trailing_bytes). Pure struct, no numpy —
    for hosts without it, and for exercising the format's invariants in tests
    that must run on the system Python."""
    header, n_complete, trailing = inspect_shard(path, strict_schema=strict_schema)
    records = []
    with open(path, "rb") as fh:
        fh.seek(HEADER_SIZE)
        for _ in range(n_complete):
            buf = fh.read(RECORD_SIZE)
            records.append(unpack_record(buf))
    return header, records, trailing


def load_shard(path, strict_schema=True):
    """(header, structured ndarray of RECORD_DTYPE, trailing_bytes). The fast
    path: one read, one frombuffer, no per-record Python — what dataset.py
    actually uses."""
    if not HAVE_NUMPY:
        raise RuntimeError("numpy is not installed; use load_shard_python()")
    header, n_complete, trailing = inspect_shard(path, strict_schema=strict_schema)
    with open(path, "rb") as fh:
        fh.seek(HEADER_SIZE)
        buf = fh.read(n_complete * RECORD_SIZE)
    arr = _np.frombuffer(buf, dtype=RECORD_DTYPE, count=n_complete)
    return header, arr, trailing


# --------------------------------------------------------------------------
# writer
# --------------------------------------------------------------------------

class ShardWriter:
    """Appends records to a rolling sequence of shard files in `directory`.

    `write_record` (struct, one record) always works. `write_batch` (numpy,
    a whole array of records) is the one the policyd recording hook and the
    synthetic generator actually use, because it is the same "one memcpy for
    the whole batch" shape as the wire protocol's fast path.
    """

    def __init__(self, directory, prefix="demo", policy_name="unknown",
                max_records_per_shard=200_000, buffering=1 << 20):
        os.makedirs(directory, exist_ok=True)
        self.directory = directory
        self.prefix = prefix
        self.policy_name = policy_name
        self.max_records_per_shard = max_records_per_shard
        self._buffering = buffering
        self._fh = None
        self._count_in_shard = 0
        self._seq = 0
        self.current_path = None
        self.total_written = 0
        self.shards_written = []

    def _roll(self):
        if self._fh is not None:
            self._fh.close()
        self._seq += 1
        name = f"{self.prefix}-{os.getpid()}-{self._seq:04d}.gbdemo"
        self.current_path = os.path.join(self.directory, name)
        self._fh = open(self.current_path, "wb", buffering=self._buffering)
        write_header(self._fh, schema.SCHEMA_HASH, schema.OBS_DIM,
                    self.policy_name)
        self._count_in_shard = 0
        self.shards_written.append(self.current_path)

    def _ensure_open(self):
        if self._fh is None or self._count_in_shard >= self.max_records_per_shard:
            self._roll()

    def write_record(self, bot_id, episode_id, tick, done, obs, buttons,
                     pitch, yaw, fwd, side, weapon):
        self._ensure_open()
        self._fh.write(pack_record(bot_id, episode_id, tick, done, obs,
                                   buttons, pitch, yaw, fwd, side, weapon))
        self._count_in_shard += 1
        self.total_written += 1

    def write_batch(self, rows):
        """rows: a numpy structured array with RECORD_DTYPE's field names
        (need not be the exact dtype -- values are cast field by field, so
        e.g. float64 obs from a generator that didn't bother with dtype=f4
        still works)."""
        if not HAVE_NUMPY:
            raise RuntimeError("numpy is not installed; use write_record()")
        n = len(rows)
        if n == 0:
            return
        self._ensure_open()
        if rows.dtype != RECORD_DTYPE:
            cast = _np.empty(n, dtype=RECORD_DTYPE)
            for name in RECORD_DTYPE.names:
                cast[name] = rows[name]
            rows = cast
        self._fh.write(rows.tobytes())
        self._count_in_shard += n
        self.total_written += n

    def flush(self):
        if self._fh is not None:
            self._fh.flush()

    def close(self):
        if self._fh is not None:
            self._fh.close()
            self._fh = None


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def describe(path):
    header, n_complete, trailing = inspect_shard(path, strict_schema=False)
    match = "OK" if header.schema_hash == schema.SCHEMA_HASH else "MISMATCH"
    lines = [
        f"{path}",
        f"  format v{header.format_version}  schema {header.schema_hash:#010x} "
        f"[{match} against this build's {schema.SCHEMA_HASH:#010x}]",
        f"  obs_dim={header.obs_dim}  policy={header.policy_name!r}  "
        f"recorded={time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(header.created_ts))}",
        f"  {n_complete:,} complete records ({n_complete * RECORD_SIZE:,} bytes)"
        + (f"  -- {trailing} trailing byte(s) after the last complete record "
           f"(truncated write)" if trailing else ""),
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--describe", metavar="SHARD", help="print a shard's header + record count")
    args = ap.parse_args()
    if args.describe:
        print(describe(args.describe))
        return 0
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
