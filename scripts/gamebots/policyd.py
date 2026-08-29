#!/usr/bin/env python3
"""policyd — the bot policy server.

One process holds the model and answers every bot on every game server over a
Unix socket. Engine adapters send a batch of observations per server frame and
get a batch of actions back.

Phase 0 ships no model at all: the policies here are a no-op and a small
scripted one. That is the point. The thing being proven first is the LOOP --
that a game server can ask an external process what its bots should do, inside
a frame, at the scale we intend to run -- because if that is wrong, no amount
of model work saves it.

Design notes worth keeping:

**Batch per server, not across servers.** Each request already carries every
bot on that server, which is the batching that matters. Coalescing across
servers would mean holding the first arrival while waiting for others, buying
GPU efficiency we measurably do not need (256 bots is ~0.01% of the card) at
the cost of the one resource we do care about, latency. So: answer
immediately, batch what arrives together.

**Single-threaded, on purpose.** A `selectors` loop handles every connection.
At ~4 µs of transport per request this is nowhere near saturated, and when a
real model lands, torch releases the GIL for the GPU call anyway. Threads here
would buy nothing and cost determinism.

**The server never blocks a game server.** Any malformed request is answered
with an error and the connection dropped, never left hanging -- a game server
waiting on a socket is a stalled game server. The adapter's own fallback (use
the engine's built-in bot AI) is what covers the gap.

Usage:
    python3 policyd.py                          # no-op policy, default socket
    python3 policyd.py --policy scripted
    python3 policyd.py --socket /run/user/1000/gamebots/policy.sock
    python3 policyd.py --stats-interval 5
"""

import argparse
import json
import math
import os
import selectors
import signal
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

DEFAULT_SOCKET_NAME = "policy.sock"
DEFAULT_RUNTIME_SUBDIR = "gamebots"


def default_socket_path():
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(runtime, DEFAULT_RUNTIME_SUBDIR, DEFAULT_SOCKET_NAME)


def default_status_path():
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(runtime, DEFAULT_RUNTIME_SUBDIR, "status.json")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------
# policies
# --------------------------------------------------------------------------

class Policy:
    """A policy maps a batch of observations to a batch of actions.

    Batch in, batch out -- never one bot at a time. That shape is what lets a
    real model be a single kernel launch for every bot on the server rather
    than one launch per bot.
    """

    name = "base"

    def act(self, tick, flags, entries):
        """entries: [(bot_id, [obs floats])] -> [(bot_id, buttons, pitch, yaw,
        forward, side, weapon)]"""
        raise NotImplementedError

    def describe(self):
        return self.name


class NoOpPolicy(Policy):
    """Do nothing, deliberately.

    This is Phase 0's exit criterion in code: a bot that stands still *because
    a Python process told it to*. If a bot goes still when this is loaded and
    resumes when the engine's own AI takes over, the whole path -- adapter,
    schema, socket, server -- is proven end to end without a model existing.
    """

    name = "noop"

    def act(self, tick, flags, entries):
        return [(bot_id, 0, 0.0, 0.0, 0.0, 0.0, 0) for bot_id, _obs in entries]


class ScriptedPolicy(Policy):
    """A hand-written policy, for loading the loop with realistic work.

    Not meant to play well. It reads the observation the way a real policy
    will -- nearest visible enemy, wall distances -- so that when we measure
    frame-time impact we are measuring something with the same access pattern
    as the real thing, not an empty function.
    """

    name = "scripted"

    def act(self, tick, flags, entries):
        out = []
        for bot_id, obs in entries:
            if flags & schema.FLAG_PAUSED or obs[_HEALTH_OFF] <= 0.0:
                out.append((bot_id, 0, 0.0, 0.0, 0.0, 0.0, 0))
                continue

            buttons = 0
            pitch = yaw = 0.0
            forward, side = 1.0, 0.0

            # Nearest present enemy across the entity slots. The adapter sorts
            # by threat, so this usually resolves at slot 0 -- but scan anyway,
            # because assuming slot 0 is an enemy is exactly the kind of thing
            # that silently breaks when teammates appear.
            best = None
            for base in _ENT_OFF:
                if obs[base] < 0.5:                      # present
                    continue
                if obs[base + _E_TEAMMATE] >= 0.5:       # never shoot our own
                    continue
                dist = obs[base + _E_DIST]
                if best is None or dist < best[0]:
                    best = (dist, base)

            if best is not None:
                dist, base = best
                dir_f = obs[base + _E_DIR]
                dir_r = obs[base + _E_DIR + 1]
                dir_u = obs[base + _E_DIR + 2]
                visible = obs[base + _E_VISIBLE] >= 0.5
                # Turn toward it: yaw from the lateral component, pitch from
                # the vertical. Gain kept low so it looks like aiming rather
                # than snapping.
                yaw = max(-1.0, min(1.0, dir_r)) * schema.MAX_YAW_DELTA_DEG * 0.5
                pitch = -max(-1.0, min(1.0, dir_u)) * schema.MAX_PITCH_DELTA_DEG * 0.5
                if visible and abs(dir_r) < 0.15 and dist > 0.02:
                    buttons |= schema.BTN_ATTACK
                if dist < 0.05:
                    forward = -1.0                # too close, back off
            else:
                # Nothing to fight: sweep for something, and strafe so a wall
                # in front does not pin us.
                yaw = math.sin(tick * 0.05 + bot_id) * 6.0

            # Wall ahead -> strafe out of it. ray_h[0] is straight ahead.
            ray0 = obs[_RAY_H_OFF]
            if ray0 < 0.08:
                side = 1.0 if (bot_id + tick // 64) % 2 else -1.0
                forward = 0.2
                buttons |= schema.BTN_JUMP if ray0 < 0.03 else 0

            pitch, yaw, forward, side = schema.clamp_action(pitch, yaw, forward, side)
            out.append((bot_id, buttons, pitch, yaw, forward, side, 0))
        return out


# Offsets come from the schema, never hardcoded — a literal here is a silent
# misread the day the layout changes. But they are resolved ONCE at import,
# not per lookup: the first version of this scanned the 140-entry field table
# for every entity slot, for every bot, on every tick, and the Phase 0 sweep
# measured it as ~1 ms of serve time at 64 bots. The schema is the source of
# truth; the hot loop gets a precomputed index.
_OFFSETS = {fname: off for _g, fname, off, _c, _d in schema.FIELD_TABLE}


def _field_offset(name):
    return _OFFSETS[name]


_ENT_OFF = tuple(_OFFSETS[f"e{i}_present"] for i in range(schema.MAX_ENTITIES))
_RAY_H_OFF = _OFFSETS["ray_h"]
_HEALTH_OFF = _OFFSETS["health_frac"]

# Sub-fields WITHIN an entity slot, resolved by name from the schema rather
# than written as `base + 7`. The first version hand-counted them and read
# `visible` out of the second component of rel_vel, so the scripted policy
# never fired — a test caught it, but in a trained policy it would have been a
# silently worse model with no error anywhere. Every slot has the same shape
# (the schema tests enforce that), so slot 0's relative offsets serve for all.
_E_TEAMMATE = _OFFSETS["e0_is_teammate"] - _ENT_OFF[0]
_E_DIR = _OFFSETS["e0_dir"] - _ENT_OFF[0]
_E_DIST = _OFFSETS["e0_dist_norm"] - _ENT_OFF[0]
_E_HEALTH = _OFFSETS["e0_health_frac"] - _ENT_OFF[0]
_E_VISIBLE = _OFFSETS["e0_visible"] - _ENT_OFF[0]


def _gpu_policy(**kw):
    """Imported lazily so policyd still runs on a host with no torch — the
    Phase 0 harness and the whole test suite depend on that."""
    import runtime
    return runtime.GpuPolicy(**kw)


def _make_recorder(out_dir, policy_name, max_records_per_shard=200_000):
    """Imported lazily, same reasoning as _gpu_policy: --record is opt-in, so
    a host with no numpy must still be able to run policyd without it as long
    as --record is not passed."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "record"))
    import recorder
    return recorder.DemoRecorder(out_dir, policy_name=policy_name,
                                 max_records_per_shard=max_records_per_shard)


POLICIES = {
    "noop": NoOpPolicy,
    "scripted": ScriptedPolicy,
    "gpu": _gpu_policy,
}


# --------------------------------------------------------------------------
# metrics
# --------------------------------------------------------------------------

class Metrics:
    """Latency and throughput, kept cheap enough to run always-on.

    Percentiles come from a reservoir of recent samples rather than a full
    histogram: we care about "is it still microseconds" not about the exact
    shape of the tail, and an unbounded list would grow forever in a service
    meant to run for weeks.
    """

    RESERVOIR = 4096

    def __init__(self):
        self.started = time.time()
        self.requests = 0
        self.bots_served = 0
        self.errors = 0
        self.rejects_schema = 0
        self.max_batch = 0
        self._lat = []
        self._i = 0

    def record(self, n_bots, elapsed_us):
        self.requests += 1
        self.bots_served += n_bots
        if n_bots > self.max_batch:
            self.max_batch = n_bots
        if len(self._lat) < self.RESERVOIR:
            self._lat.append(elapsed_us)
        else:
            self._lat[self._i % self.RESERVOIR] = elapsed_us
            self._i += 1

    def snapshot(self):
        lat = sorted(self._lat)
        def pct(p):
            if not lat:
                return None
            return round(lat[min(len(lat) - 1, int(len(lat) * p))], 1)
        uptime = max(1e-6, time.time() - self.started)
        return {
            "uptime_sec": round(uptime, 1),
            "requests": self.requests,
            "bots_served": self.bots_served,
            "max_batch": self.max_batch,
            "errors": self.errors,
            "rejects_schema": self.rejects_schema,
            "req_per_sec": round(self.requests / uptime, 1),
            "bot_decisions_per_sec": round(self.bots_served / uptime, 1),
            "serve_us_p50": pct(0.50),
            "serve_us_p99": pct(0.99),
            "serve_us_max": round(max(lat), 1) if lat else None,
        }


def publish_status(state, path):
    """Atomic, world-readable — same convention as the dashboard collector and
    the game-server watchdog, so this can appear on the login-screen wall
    rather than being another service nobody can see."""
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    try:
        os.chmod(directory, 0o755)
    except OSError:
        pass
    tmp = f"{path}.tmp{os.getpid()}"
    with open(tmp, "w") as fh:
        json.dump(state, fh, separators=(",", ":"))
        fh.flush()
        os.fsync(fh.fileno())
    os.chmod(tmp, 0o644)
    os.replace(tmp, path)


# --------------------------------------------------------------------------
# server
# --------------------------------------------------------------------------

class PolicyServer:
    def __init__(self, policy, sock_path, status_path=None,
                 stats_interval=30.0, planner=None, recorder=None,
                 tcp_listen=None, udp_listen=None):
        self.policy = policy
        self.sock_path = sock_path
        self.status_path = status_path
        self.stats_interval = stats_interval
        self.metrics = Metrics()
        # The array path needs both halves: a policy that can consume arrays,
        # and numpy to make them. Either missing falls back to struct, which
        # still works — just slower.
        self._fast = bool(getattr(policy, "act_arrays", None)) and \
            getattr(schema, "HAVE_NUMPY", False)
        self.planner = planner
        self.tcp_listen = tcp_listen
        self._tcp_srv = None
        self.udp_listen = udp_listen
        self._udp_srv = None
        self._udp_last_error = None
        # Opt-in demonstration recording (--record DIR). Only wired on the
        # array path: struct-path serving doesn't have the batched numpy
        # arrays record.recorder.DemoRecorder.record() expects, and numpy is
        # required to build the shard anyway (see record/shard.py). main()
        # refuses to hand a recorder here unless self._fast is already true.
        self.recorder = recorder
        self._intent_lo = next(f[2] for f in schema.FIELD_TABLE
                               if f[1] == "intent")
        self.sel = selectors.DefaultSelector()
        self.conns = {}
        self._running = True
        self._last_stats = time.time()

    # sockaddr_un.sun_path is a fixed 108-byte array in the kernel ABI, and
    # bind() past it fails with a bare "AF_UNIX path too long" that says
    # nothing about which path or what the limit is. Cheap to check, and the
    # message is the difference between a five-second fix and a confused hour.
    SUN_PATH_MAX = 107

    def check_socket_path(self):
        if len(self.sock_path.encode()) > self.SUN_PATH_MAX:
            raise ValueError(
                f"socket path is {len(self.sock_path.encode())} bytes, but "
                f"AF_UNIX allows {self.SUN_PATH_MAX}:\n  {self.sock_path}\n"
                f"Use a shorter path — the default "
                f"($XDG_RUNTIME_DIR/{DEFAULT_RUNTIME_SUBDIR}/"
                f"{DEFAULT_SOCKET_NAME}) is well inside the limit.")

    def _listen_tcp(self, spec):
        """Also listen on TCP, for engines that cannot use a Unix socket.

        UT99's only outbound networking from UnrealScript is `TcpLink`, so a
        mutator adapter has no way to reach a UDS. The wire format is identical;
        the transport is the only difference.

        Binds to **127.0.0.1 by default**. This endpoint takes observations and
        hands back actions with no authentication -- fine on loopback, not fine
        on a LAN -- so binding it wider has to be a deliberate act.
        """
        host, _, port = spec.rpartition(":")
        host = host or "127.0.0.1"
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, int(port)))
        srv.listen(32)
        srv.setblocking(False)
        self._tcp_srv = srv
        self.sel.register(srv, selectors.EVENT_READ, self._accept)
        log(f"policyd: also listening on tcp://{host}:{port}"
            + ("" if host in ("127.0.0.1", "localhost")
               else "  -- WARNING: reachable off this host, unauthenticated"))

    def _listen_udp(self, spec):
        """Also listen on UDP.

        Added because UT99's `TcpLink` does not complete an outbound connect on
        the OldUnreal 469 Linux dedicated build — verified with `ss` that not
        even a SYN leaves the process. `UdpLink` in the same IpDrv demonstrably
        works: the live server answers a 489-byte GameSpy query on 7798 through
        it, which is proof of both send and receive in that exact process.

        UDP suits this protocol better than TCP anyway. Every exchange is one
        fixed-size request and one fixed-size reply, so **datagram boundaries
        are the framing** — there is no partial-read state to keep, and a lost
        datagram is a dropped frame, which the adapter already handles by
        falling back to the engine's own AI.

        The wire STRUCTURE is unchanged (same header, same per-bot obs/action
        layout as every other transport) but this endpoint additionally
        accepts it ASCII-hex-encoded — see `_udp_readable`'s docstring for
        why: UT99's `UdpLink.ReceivedBinary` does not deliver real payload
        bytes on the build this was built against, while `SendText`/
        `ReceivedText` do. A raw-binary client (loadgen.py, any future
        engine that can do better) is unaffected and gets a raw-binary reply
        back; a hex-text client gets a hex-text reply back, matching what it
        sent.

        Loopback default, for the same reason as TCP: unauthenticated.
        """
        host, _, port = spec.rpartition(":")
        host = host or "127.0.0.1"
        srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, int(port)))
        srv.setblocking(False)
        self._udp_srv = srv
        self.sel.register(srv, selectors.EVENT_READ, self._udp_readable)
        log(f"policyd: also listening on udp://{host}:{port}"
            + ("" if host in ("127.0.0.1", "localhost")
               else "  -- WARNING: reachable off this host, unauthenticated"))

    def _udp_readable(self, srv):
        """One datagram in, one datagram out. No connection, no framing.

        Accepts the payload either raw (every other UDP client) or
        ASCII-hex-encoded (the UT99 adapter). UT99's `UdpLink.SendBinary`
        genuinely sends real datagrams — confirmed byte-for-byte correct on
        a receiving Python socket — but `ReceivedBinary`'s `B` byte-array
        parameter never carries real content back on the OldUnreal 469e
        Linux build the adapter was built against: `Count` correctly reports
        the true datagram size every time, `B` is uninitialised memory
        regardless of size (verified by searching for a known incrementing
        byte pattern across the whole buffer and never finding it). UT99's
        `SendText`/`ReceivedText` round-trip content correctly, so that
        adapter hex-encodes the exact same schema bytes and sends them as
        text instead — the schema and wire STRUCTURE are unchanged, only the
        encoding of the bytes that carry them. A hex request gets a hex
        reply back; a raw request gets a raw reply back.

        A malformed datagram is dropped with a counted error rather than
        closing anything — there is nothing to close, and a peer that sends
        rubbish must not be able to stop us serving everyone else.
        """
        try:
            frame, peer = srv.recvfrom(1 << 16)
        except (BlockingIOError, ConnectionResetError, OSError):
            return
        t0 = time.perf_counter_ns()

        is_hex = False
        payload = frame
        if frame[:len(schema.REQ_MAGIC)] != schema.REQ_MAGIC:
            # Doesn't look like a raw request -- try treating it as the
            # hex-text encoding an engine with a broken binary receive path
            # (UT99) sends instead. Not raw and not valid hex text both fall
            # through to unpack_request() below, which reports its own
            # (already-good) error for a short/garbled request.
            try:
                decoded = bytes.fromhex(frame.decode("ascii"))
            except (ValueError, UnicodeDecodeError):
                decoded = None
            if decoded is not None and decoded[:len(schema.REQ_MAGIC)] == schema.REQ_MAGIC:
                payload = decoded
                is_hex = True

        try:
            tick, flags, entries = schema.unpack_request(payload)
        except ValueError as exc:
            self.metrics.errors += 1
            if "schema hash mismatch" in str(exc):
                self.metrics.rejects_schema += 1
            if self._udp_last_error != str(exc):
                self._udp_last_error = str(exc)
                log(f"policyd: bad UDP request from {peer[0]}:{peer[1]} — {exc}")
            return
        try:
            actions = self.policy.act(tick, flags, entries)
        except Exception as exc:  # noqa: BLE001
            self.metrics.errors += 1
            log(f"policyd: policy raised {type(exc).__name__}: {exc}")
            actions = [(bid, 0, 0.0, 0.0, 0.0, 0.0, 0) for bid, _ in entries]
        reply = schema.pack_response(tick, actions, flags)
        if is_hex:
            reply = reply.hex().encode("ascii")
        try:
            srv.sendto(reply, peer)
        except OSError:
            return
        self.metrics.record(len(entries), (time.perf_counter_ns() - t0) / 1000.0)

    def start(self):
        self.check_socket_path()
        directory = os.path.dirname(self.sock_path)
        if directory:
            os.makedirs(directory, exist_ok=True)
        # A stale socket file from a crashed run makes bind() fail with
        # EADDRINUSE even though nothing is listening.
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.sock_path)
        srv.listen(64)
        srv.setblocking(False)
        os.chmod(self.sock_path, 0o666)
        self.srv = srv
        self.sel.register(srv, selectors.EVENT_READ, self._accept)
        if self.tcp_listen:
            self._listen_tcp(self.tcp_listen)
        if self.udp_listen:
            self._listen_udp(self.udp_listen)
        log(f"policyd: policy={self.policy.describe()} "
            f"schema={schema.SCHEMA_HASH:#010x} obs_dim={schema.OBS_DIM} "
            f"path={'array' if self._fast else 'struct'}")
        log(f"policyd: listening on {self.sock_path}")

    def _accept(self, srv):
        conn, _peer = srv.accept()
        conn.setblocking(False)
        if conn.family == socket.AF_INET:
            # Small replies against a frame deadline: Nagle would hold them
            # waiting for more data and add tens of ms to a 10ms budget.
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.conns[conn.fileno()] = bytearray()
        self.sel.register(conn, selectors.EVENT_READ, self._readable)
        log(f"policyd: adapter connected (fd={conn.fileno()})")

    def _close(self, conn, why=""):
        fd = conn.fileno()
        try:
            self.sel.unregister(conn)
        except Exception:
            pass
        self.conns.pop(fd, None)
        try:
            conn.close()
        except Exception:
            pass
        log(f"policyd: adapter disconnected (fd={fd}){' — ' + why if why else ''}")

    def _readable(self, conn):
        try:
            data = conn.recv(1 << 16)
        except (ConnectionResetError, OSError):
            self._close(conn, "reset")
            return
        if not data:
            self._close(conn)
            return
        buf = self.conns.get(conn.fileno())
        if buf is None:
            return
        buf += data
        self._drain(conn, buf)

    def _drain(self, conn, buf):
        """Consume every complete request in the buffer.

        A game server may pipeline (send frame N+1 before reading N's answer),
        so this must handle several requests per readable event and a partial
        one at the tail.
        """
        while True:
            if len(buf) < schema.HEADER_SIZE:
                return
            try:
                _magic, _hsh, n, _flags, _tick = struct.unpack_from(
                    schema.HEADER_STRUCT, buf, 0)
            except struct.error:
                return
            total = schema.HEADER_SIZE + n * schema.OBS_ENTRY_SIZE
            if len(buf) < total:
                return                       # wait for the rest
            frame = bytes(buf[:total])
            del buf[:total]
            if not self._serve(conn, frame):
                return

    def _inject_intent(self, conn_key, ids, obs):
        """Overwrite the intent slot with whatever the planner last decided.

        Done HERE rather than in the adapter so engine adapters never learn the
        planner exists — they send zeros and the strategic layer is transparent
        to them. A bot with no plan keeps zeros, which FiLM treats as the
        identity, so "no planner" means "behave exactly as trained".
        """
        if self.planner is None:
            return
        lo = self._intent_lo
        hi = lo + schema.INTENT_DIM
        for i, bid in enumerate(ids):
            vec = self.planner.intent_for(conn_key, bid)
            if vec is not None:
                obs[i, lo:hi] = vec

    def _serve_fast(self, conn, frame):
        """Array path: bytes -> numpy view -> policy -> bytes.

        Used when the policy exposes act_arrays() and numpy is present. Avoids
        ~2.8 us/bot of Python struct work, which at 512 bots was an order of
        magnitude more than the GPU forward pass it exists to feed.
        """
        t0 = time.perf_counter_ns()
        try:
            tick, flags, ids, obs = schema.unpack_request_fast(frame)
        except ValueError as exc:
            return self._reject(conn, exc)
        n = len(ids)
        if self.planner is not None:
            self.planner.observe(conn.fileno(), ids, obs)
            self._inject_intent(conn.fileno(), ids, obs)
        try:
            btn, pitch, yaw, fwd, side, wpn = self.policy.act_arrays(
                tick, flags, ids, obs, conn_key=conn.fileno())
        except Exception as exc:
            self.metrics.errors += 1
            log(f"policyd: policy raised {type(exc).__name__}: {exc}")
            import numpy as np
            z = np.zeros(n, dtype=np.float32)
            btn = np.zeros(n, dtype=np.uint16)
            pitch = yaw = fwd = side = z
            wpn = np.zeros(n, dtype=np.uint8)
        try:
            conn.sendall(schema.pack_response_fast(tick, ids, btn, pitch, yaw,
                                                   fwd, side, wpn, flags))
        except (BrokenPipeError, ConnectionResetError, OSError):
            self._close(conn, "write failed")
            return False
        # Recording happens AFTER the response is on the wire, so it never
        # delays the answer a game server is blocked on — only the reported
        # serve_us metric below (which includes it) pays for it, which is the
        # honest place for that cost to show up.
        if self.recorder is not None:
            try:
                self.recorder.record(tick, conn.fileno(), ids, obs, btn,
                                     pitch, yaw, fwd, side, wpn)
            except Exception as exc:
                log(f"policyd: recorder failed, disabling recording: {exc}")
                self.recorder = None
        self.metrics.record(n, (time.perf_counter_ns() - t0) / 1000.0)
        return True

    def _reject(self, conn, exc):
        self.metrics.errors += 1
        if "schema hash mismatch" in str(exc):
            self.metrics.rejects_schema += 1
        log(f"policyd: bad request — {exc}")
        self._close(conn, "bad request")
        return False

    def _serve(self, conn, frame):
        if self._fast:
            return self._serve_fast(conn, frame)
        t0 = time.perf_counter_ns()
        try:
            tick, flags, entries = schema.unpack_request(frame)
        except ValueError as exc:
            # Schema mismatch is the important one: say so, loudly, once, and
            # drop the adapter rather than feeding a model garbage.
            self.metrics.errors += 1
            if "schema hash mismatch" in str(exc):
                self.metrics.rejects_schema += 1
            log(f"policyd: bad request — {exc}")
            self._close(conn, "bad request")
            return False

        try:
            actions = self.policy.act(tick, flags, entries)
        except Exception as exc:
            # A policy that throws must not take the server down; the bots get
            # a null action for this frame and the adapter can fall back.
            self.metrics.errors += 1
            log(f"policyd: policy raised {type(exc).__name__}: {exc}")
            actions = [(bid, 0, 0.0, 0.0, 0.0, 0.0, 0) for bid, _ in entries]

        try:
            conn.sendall(schema.pack_response(tick, actions, flags))
        except (BrokenPipeError, ConnectionResetError, OSError):
            self._close(conn, "write failed")
            return False

        self.metrics.record(len(entries), (time.perf_counter_ns() - t0) / 1000.0)
        return True

    def run(self):
        self.start()
        while self._running:
            for key, _mask in self.sel.select(timeout=1.0):
                key.data(key.fileobj)
            now = time.time()
            if self.stats_interval and now - self._last_stats >= self.stats_interval:
                self._last_stats = now
                self._emit_stats()
        self._shutdown()

    def _emit_stats(self):
        snap = self.metrics.snapshot()
        if hasattr(self.policy, "stats"):
            snap.update(self.policy.stats())
        if self.planner is not None:
            snap.update(self.planner.stats())
        if self.recorder is not None:
            snap.update(self.recorder.stats())
        snap.update({
            "ts": time.time(),
            "policy_desc": self.policy.describe(),
            "schema_hash": f"{schema.SCHEMA_HASH:#010x}",
            "obs_dim": schema.OBS_DIM,
            "adapters_connected": len(self.conns),
            "socket": self.sock_path,
        })
        if self.status_path:
            try:
                publish_status(snap, self.status_path)
            except Exception as exc:
                log(f"policyd: cannot publish status: {exc}")
        if snap["requests"]:
            log(f"policyd: {snap['requests']} req, "
                f"{snap['bot_decisions_per_sec']:.0f} bot-decisions/s, "
                f"p50 {snap['serve_us_p50']}us p99 {snap['serve_us_p99']}us, "
                f"{snap['adapters_connected']} adapter(s)")

    def stop(self, *_a):
        self._running = False

    def _shutdown(self):
        self._emit_stats()
        for fd in list(self.conns):
            pass
        if self.recorder is not None:
            try:
                self.recorder.close()
            except Exception as exc:
                log(f"policyd: error closing recorder: {exc}")
        for sock in (self.srv, self._tcp_srv, self._udp_srv):
            try:
                if sock is not None:
                    sock.close()
            except Exception:
                pass
        if os.path.exists(self.sock_path):
            try:
                os.unlink(self.sock_path)
            except OSError:
                pass
        log("policyd: stopped")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--socket", default=os.environ.get(
        "GAMEBOTS_SOCKET", default_socket_path()))
    ap.add_argument("--policy", default="noop", choices=sorted(POLICIES))
    ap.add_argument("--status-path", default=os.environ.get(
        "GAMEBOTS_STATUS", default_status_path()))
    ap.add_argument("--stats-interval", type=float, default=30.0)
    ap.add_argument("--weights", help="checkpoint for --policy gpu")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--dtype", default="fp16", choices=("fp16", "bf16", "fp32"))
    ap.add_argument("--no-graphs", action="store_true",
                    help="disable CUDA graph capture (debugging)")
    ap.add_argument("--planner", default="none",
                    choices=("none", "heuristic", "llm"),
                    help="strategic layer: assigns intent at ~2Hz per server")
    ap.add_argument("--planner-model", default=os.environ.get(
        "GAMEBOTS_PLANNER_MODEL", "Qwen/Qwen2.5-1.5B-Instruct"))
    ap.add_argument("--tcp-listen", default=os.environ.get("GAMEBOTS_TCP"),
                    metavar="[HOST:]PORT",
                    help="additionally listen on TCP, for engines that cannot "
                         "use a Unix socket (UT99's TcpLink). Defaults to "
                         "127.0.0.1; binding wider is unauthenticated")
    ap.add_argument("--udp-listen", default=os.environ.get("GAMEBOTS_UDP"),
                    metavar="[HOST:]PORT",
                    help="additionally listen on UDP. UT99's TcpLink does not "
                         "connect on the Linux 469 build but UdpLink works; "
                         "datagram boundaries are the framing")
    ap.add_argument("--record", metavar="DIR", default=None,
                    help="opt-in: write (observation, action) demonstration "
                         "shards for every bot served, into DIR "
                         "(record/shard.py format). Off by default. Needs "
                         "numpy and a policy with act_arrays() — the same "
                         "requirements as the fast serving path.")
    ap.add_argument("--record-max-per-shard", type=int, default=200_000)
    args = ap.parse_args()

    if args.policy == "gpu":
        policy = POLICIES["gpu"](weights=args.weights, device=args.device,
                                 dtype=args.dtype,
                                 use_graphs=not args.no_graphs)
    else:
        policy = POLICIES[args.policy]()
    planner_svc = None
    if args.planner != "none":
        import planner as planner_mod
        backend = (planner_mod.LlmPlanner(args.planner_model)
                   if args.planner == "llm" else planner_mod.HeuristicPlanner())
        planner_svc = planner_mod.PlannerService(backend).start()
        log(f"policyd: planner={backend.name} at "
            f"{1.0 / planner_mod.PLAN_PERIOD_SEC:.1f} Hz")

    recorder_svc = None
    if args.record:
        fast = bool(getattr(policy, "act_arrays", None)) and \
            getattr(schema, "HAVE_NUMPY", False)
        if not fast:
            log("policyd: --record requires numpy and a policy with "
               "act_arrays() (the fast serving path) — recording disabled")
        else:
            recorder_svc = _make_recorder(
                args.record, args.policy,
                max_records_per_shard=args.record_max_per_shard)
            log(f"policyd: recording demonstrations to {args.record}")

    server = PolicyServer(policy, args.socket, args.status_path,
                          args.stats_interval, planner=planner_svc,
                          recorder=recorder_svc, tcp_listen=args.tcp_listen,
                          udp_listen=args.udp_listen)
    signal.signal(signal.SIGINT, server.stop)
    signal.signal(signal.SIGTERM, server.stop)
    server.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
