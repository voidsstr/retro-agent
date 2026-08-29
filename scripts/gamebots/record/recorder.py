#!/usr/bin/env python3
"""DemoRecorder — the opt-in hook policyd calls on the array serving path to
persist (observation, action) pairs as it answers real bots.

Split out from shard.py deliberately: shard.py owns the container FORMAT and
knows nothing about bots or episodes; this module owns the EPISODE
bookkeeping (per (connection, bot_id) alive-transition tracking -> episode_id
and done) and nothing about file layout. Mixing the two would make either one
harder to test in isolation.

Cheap by construction: `record()` takes the exact numpy arrays policyd's
`_serve_fast` already built for the response (bot_ids, obs, buttons, pitch,
yaw, fwd, side, weapon) -- no extra marshalling -- and adds one vectorised
episode-bookkeeping pass plus one `ShardWriter.write_batch`, a single
`tobytes()` memcpy of the whole batch.

The first version of the episode bookkeeping was a plain Python dict-per-bot
loop, on the theory that a dead/alive transition is inherently sequential so
"it cannot be vectorised away". That was true per bot, not true across bots:
at 512 bots that loop alone measured ~0.12 ms of the ~0.16 ms `record()` cost
-- the exact "per-bot Python marshalling dominates" mistake Phase 0's own
README documents for the wire protocol, showing up again one layer up.
Fixed the same way: each connection gets three small numpy arrays indexed
DIRECTLY by bot_id (a u16, so 65536 slots, ~384 KB per connection) --
`seen`/`prev_alive`/`episode` -- and a whole batch is gathered, transitioned
and scattered back in a handful of vectorised ops, no per-bot Python at all.

**The actual disk write is off the serving path entirely.** Even a vectorised
`ShardWriter.write_batch` still measured as the single largest remaining cost
at 512 bots -- it is a real `write(2)` of ~300 KB, and no amount of batching
removes that it is I/O. `record()` therefore only builds the row array (pure
memory, no syscall) and hands it to a bounded queue; a single background
writer thread owns the actual file and drains it. If that queue is ever full
-- the writer falling behind a burst -- `record()` DROPS the batch rather than
blocking: a policy server stalling a game server to protect a training
recording would be exactly backwards (see policyd.py's own module docstring:
"the server never blocks a game server"). Dropped batches are counted in
`stats()` so a stall is visible, not silent.
"""

import os
import queue
import sys
import threading

_HERE = os.path.dirname(os.path.abspath(__file__))
_GB = os.path.dirname(_HERE)
if _GB not in sys.path:
    sys.path.insert(0, _GB)
import schema  # noqa: E402

if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import shard  # noqa: E402

try:
    import numpy as np
    HAVE_NUMPY = True
except ImportError:  # pragma: no cover - exercised on hosts without numpy
    np = None
    HAVE_NUMPY = False

_ALIVE_OFF = next(f[2] for f in schema.FIELD_TABLE if f[1] == "alive")

# bot_id is a u16 on the wire (schema.OBS_ENTRY_STRUCT / ACTION_STRUCT), so
# every possible id fits in one flat array indexed directly by value -- no
# hashing, no dict, just a gather/scatter.
_BOT_ID_SLOTS = 1 << 16


class DemoRecorder:
    """Wraps a shard.ShardWriter with per-bot episode/done tracking and an
    async writer thread.

    `record()` itself is called from policyd's single serving thread (see its
    module docstring) and must stay cheap and non-blocking; the actual file
    I/O runs on ONE dedicated background thread that owns `self.writer`
    exclusively, so `ShardWriter` never needs its own locking.
    """

    def __init__(self, out_dir, policy_name="unknown",
                max_records_per_shard=200_000, flush_every=500,
                queue_maxsize=256, async_write=True):
        if not HAVE_NUMPY:
            raise RuntimeError(
                "numpy is required to record demonstrations (the recorder "
                "reuses the arrays the array serving path already built)")
        self.writer = shard.ShardWriter(out_dir, policy_name=policy_name,
                                        max_records_per_shard=max_records_per_shard)
        self.flush_every = flush_every
        # conn_key -> (seen, prev_alive, episode) arrays, each _BOT_ID_SLOTS
        # long, allocated lazily so a recorder that only ever sees one
        # connection (the common case) pays for one set, not one per possible
        # server. A bot going dead->alive is a respawn: new episode. First
        # sighting starts at episode 0, tracked via `seen` rather than a
        # sentinel, since arrays start pre-zeroed.
        self._conn = {}

        self.dropped_batches = 0
        self.dropped_records = 0
        self._async = bool(async_write)
        if self._async:
            self._queue = queue.Queue(maxsize=queue_maxsize)
            self._thread = threading.Thread(
                target=self._writer_loop, name="gamebots-demo-writer",
                daemon=True)
            self._thread.start()
        else:
            self._since_flush = 0

    def _state_for(self, conn_key):
        st = self._conn.get(conn_key)
        if st is None:
            st = (np.zeros(_BOT_ID_SLOTS, dtype=bool),
                 np.zeros(_BOT_ID_SLOTS, dtype=bool),
                 np.zeros(_BOT_ID_SLOTS, dtype=np.uint32))
            self._conn[conn_key] = st
        return st

    def _episode_ids_and_done(self, conn_key, bot_ids, alive):
        seen, prev_alive, episode = self._state_for(conn_key)
        idx = bot_ids.astype(np.intp, copy=False)

        was_seen = seen[idx]
        was_alive = prev_alive[idx]
        respawned = was_seen & ~was_alive & alive
        if respawned.any():
            episode[idx[respawned]] += 1     # unique per tick: safe as a plain scatter-add

        eids = episode[idx].copy()
        done = (~alive).astype(np.uint8)

        seen[idx] = True
        prev_alive[idx] = alive
        return eids, done

    def record(self, tick, conn_key, bot_ids, obs, buttons, pitch, yaw, fwd,
              side, weapon):
        """Everything here is already a numpy array from the caller's own
        serving path -- this builds one row array (memory only, no I/O) and
        either queues it for the writer thread or (async_write=False, mainly
        for tests) writes it inline."""
        n = len(bot_ids)
        if n == 0:
            return
        alive = obs[:, _ALIVE_OFF] >= 0.5
        eids, done = self._episode_ids_and_done(conn_key, bot_ids, alive)

        rows = shard.record_array(n)
        rows["bot_id"] = bot_ids
        rows["episode_id"] = eids
        rows["tick"] = tick
        rows["done"] = done
        rows["obs"] = obs
        rows["buttons"] = buttons
        rows["pitch"] = pitch
        rows["yaw"] = yaw
        rows["fwd"] = fwd
        rows["side"] = side
        rows["weapon"] = weapon

        if self._async:
            try:
                self._queue.put_nowait(rows)
            except queue.Full:
                # The writer thread is falling behind (slow disk, huge
                # backlog). Dropping here is deliberate: blocking would make
                # a training recording able to stall the game server it is
                # supposedly just watching, which is exactly backwards.
                self.dropped_batches += 1
                self.dropped_records += n
        else:
            self.writer.write_batch(rows)
            self._since_flush += n
            if self._since_flush >= self.flush_every:
                self.writer.flush()
                self._since_flush = 0

    def _writer_loop(self):
        since_flush = 0
        while True:
            rows = self._queue.get()
            if rows is None:            # close() sentinel
                self.writer.flush()
                self._queue.task_done()
                return
            self.writer.write_batch(rows)
            since_flush += len(rows)
            if since_flush >= self.flush_every:
                self.writer.flush()
                since_flush = 0
            self._queue.task_done()

    def stats(self):
        tracked = sum(int(seen.sum()) for seen, _pa, _ep in self._conn.values())
        s = {
            "record_dir": self.writer.directory,
            "record_total_written": self.writer.total_written,
            "record_current_shard": self.writer.current_path,
            "record_bots_tracked": tracked,
            "record_dropped_batches": self.dropped_batches,
            "record_dropped_records": self.dropped_records,
        }
        if self._async:
            s["record_queue_depth"] = self._queue.qsize()
        return s

    def close(self, timeout=30.0):
        if self._async:
            self._queue.put(None)
            self._thread.join(timeout=timeout)
        self.writer.close()
