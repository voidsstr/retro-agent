#!/usr/bin/env python3
"""gameservers_watch.py — keep every fleet game server up, and say so.

Runs as a `systemd --user` service beside the servers themselves, which is the
only place a restart is cheap: the game units are `--user` units, so a watchdog
running as root would have to reach into another manager to touch them.

It does two jobs on one loop:

  1. **Publish** a status blob — up/down, players, bots, map, query RTT, unit
     state — to a file the login-screen dashboard collector reads. The
     collector never probes UDP itself; a stall there is a login screen that
     stops updating.
  2. **Restart** what has actually died, under guardrails that matter more than
     the restarting does:

     * a unit systemd calls `failed`/`inactive` is restarted at once,
     * a unit that is `active` but has not answered its query for
       `PROBE_FAIL_LIMIT` consecutive cycles is restarted -- one silent cycle
       is a map change, three is a wedge,
     * never more often than `COOLDOWN_SEC` for the same unit, and never more
       than `MAX_PER_HOUR` times an hour, so a server that is broken for a
       reason restarting cannot fix (a missing pak, a bad cfg) is left alone
       after a few tries instead of being flapped forever,
     * a unit that is not installed is never touched.

Every decision, including every decision NOT to act, is logged with its reason.
A silent skip and a successful restart must never look the same in the journal.

    python3 gameservers_watch.py                 # run forever (the service)
    python3 gameservers_watch.py --once --stdout # one pass, print, change nothing
    python3 gameservers_watch.py --no-restart    # watch and publish only
"""

import argparse
import collections
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gameservers  # noqa: E402

SCHEMA_VERSION = 1

INTERVAL_SEC = 20.0
PROBE_FAIL_LIMIT = 3     # consecutive mute cycles before we call an active unit wedged
COOLDOWN_SEC = 300.0     # minimum gap between restarts of the same unit
MAX_PER_HOUR = 4         # give up rather than flap a server that cannot start


# The host's `systemd --user` services, reported here rather than by the
# dashboard collector.
#
# The collector runs as root inside a hardened unit, where `systemctl --user`
# means *root's* manager. Reaching the fleet user's manager from there needs a
# privilege hop (`setpriv --reuid ... systemctl --user`), and inside that
# sandbox the hop fails -- `--clear-groups` calls setgroups(), which does not
# survive, so it produced empty output and every fleet service read "unknown":
# indistinguishable from all of them having died.
#
# This process already IS the fleet user and already runs `systemctl --user`
# for the game servers, so it can answer the question directly and put the
# result in the status file the collector is reading anyway. No hop, no
# sandbox interaction, one fewer way for the panel to be wrong.
HOST_USER_SERVICES = [
    "retro-chat-daemon",
    "retro-chat-brain",
    "retro-gameindex",
    "retro-gameservers-watch",
    "retro-dosgames-http",
]


def default_status_path():
    """Where the dashboard collector looks.

    `$XDG_RUNTIME_DIR` is /run/user/<uid>, which is exactly right: it is
    per-user, tmpfs, cleaned up at logout, and readable by root -- and root is
    the dashboard collector. It is NOT /tmp, because the greeter that
    ultimately displays this cannot see /tmp (systemd DynamicUser implies
    PrivateTmp), and keeping both halves out of /tmp stops anyone reintroducing
    that trap later.
    """
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(runtime, "retro-gameservers", "status.json")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Watch:
    def __init__(self, restart=True, interval=INTERVAL_SEC):
        self.restart_enabled = restart
        self.interval = interval
        self.mute_streak = collections.Counter()
        self.last_restart = {}
        self.restart_log = collections.defaultdict(list)  # unit -> [ts, ...]
        self.actions = collections.deque(maxlen=20)
        self.started = time.time()
        self.passes = 0

    # -- restart policy ----------------------------------------------------

    def _recent_restarts(self, unit, now):
        cutoff = now - 3600
        stamps = [t for t in self.restart_log[unit] if t > cutoff]
        self.restart_log[unit] = stamps
        return len(stamps)

    def decide(self, row, now):
        """(should_restart, reason). Reason is recorded either way."""
        unit = row["unit"]
        if not row.get("installed"):
            # Covers both "never installed here" and "could not ask the
            # manager". Restarting on the strength of a failed lookup is how a
            # watchdog starts bouncing services because docker was busy.
            return False, row.get("unavailable") or "not installed"
        if not self.restart_enabled:
            return False, "restarts disabled"

        state = row.get("unit_state")
        if state in ("failed", "inactive"):
            why = f"unit {state}"
        elif row["up"]:
            self.mute_streak[unit] = 0
            return False, None
        else:
            streak = self.mute_streak[unit]
            if streak < PROBE_FAIL_LIMIT:
                # One mute cycle is a map change, not a wedge. Say which.
                return False, (f"mute {streak}/{PROBE_FAIL_LIMIT} "
                               f"(unit {state}) — waiting")
            why = f"active but mute {streak} cycles"

        since = now - self.last_restart.get(unit, 0)
        if since < COOLDOWN_SEC:
            return False, f"{why}; cooling down {int(COOLDOWN_SEC - since)}s"
        if self._recent_restarts(unit, now) >= MAX_PER_HOUR:
            return False, (f"{why}; {MAX_PER_HOUR} restarts this hour already "
                           f"— needs a human")
        return True, why

    # -- one pass ----------------------------------------------------------

    def pass_once(self):
        snap = gameservers.collect()
        now = time.time()
        self.passes += 1

        for row in snap["servers"]:
            if row.get("installed") and not row["up"]:
                self.mute_streak[row["unit"]] += 1

            should, reason = self.decide(row, now)
            row["watchdog"] = reason
            if not should:
                if reason and reason not in ("not installed", "restarts disabled",
                                             "manager unreachable"):
                    log(f"{row['unit']}: {reason}")
                continue

            log(f"{row['unit']}: {reason} — restarting via {row.get('manager', 'systemd')}")
            # Restart through whichever manager owns it: Tribes 2 is a docker
            # container, and `systemctl restart tribes2-server` would fail with
            # "unit not found" forever while the watchdog logged success-shaped
            # attempts against a server nobody was fixing.
            ok, msg = gameservers.restart(row)
            self.last_restart[row["unit"]] = now
            self.restart_log[row["unit"]].append(now)
            self.mute_streak[row["unit"]] = 0
            row["watchdog"] = f"{reason} — {'restarted' if ok else msg}"
            self.actions.appendleft({
                "ts": now, "unit": row["unit"], "reason": reason,
                "ok": ok, "detail": msg,
            })
            log(f"{row['unit']}: {'restarted' if ok else 'RESTART FAILED: ' + msg}")

        for row in snap["servers"]:
            row["restarts_this_hour"] = self._recent_restarts(row["unit"], now)

        # Reported from here because we are the fleet user; see
        # HOST_USER_SERVICES for why the collector cannot ask for itself.
        try:
            snap["host_services"] = gameservers.unit_states(HOST_USER_SERVICES)
        except Exception as exc:  # never let this sink a game-server pass
            snap["host_services_error"] = f"{type(exc).__name__}: {exc}"

        snap["schema"] = SCHEMA_VERSION
        snap["watchdog"] = {
            "enabled": self.restart_enabled,
            "interval_sec": self.interval,
            "started_at": self.started,
            "passes": self.passes,
            "actions": list(self.actions)[:8],
        }
        return snap


def publish(state, path):
    """Atomic, and world-readable — the reader is the root collector."""
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    os.chmod(directory, 0o755)
    tmp = f"{path}.tmp{os.getpid()}"
    with open(tmp, "w") as fh:
        json.dump(state, fh, separators=(",", ":"))
        fh.flush()
        os.fsync(fh.fileno())
    os.chmod(tmp, 0o644)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--status-path", default=os.environ.get(
        "RETRO_GAMESERVERS_STATUS", default_status_path()))
    ap.add_argument("--interval", type=float, default=INTERVAL_SEC)
    ap.add_argument("--no-restart", action="store_true",
                    help="watch and publish, but never restart anything")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--stdout", action="store_true",
                    help="print the blob instead of writing the status file")
    args = ap.parse_args()

    restart = not args.no_restart and not os.environ.get("RETRO_GAMESERVERS_NO_RESTART")
    # --once --stdout is the "tell me what you see" mode; it must be safe to
    # run from a terminal at any time, so it never restarts anything.
    if args.once and args.stdout:
        restart = False

    watch = Watch(restart=restart, interval=args.interval)
    log(f"gameservers watchdog: interval {args.interval}s, "
        f"restarts {'ON' if restart else 'OFF'}, status -> {args.status_path}")

    while True:
        started = time.monotonic()
        try:
            state = watch.pass_once()
            if args.stdout:
                json.dump(state, sys.stdout, indent=2 if args.once else None)
                sys.stdout.write("\n")
                sys.stdout.flush()
            else:
                publish(state, args.status_path)
        except Exception as exc:  # one bad pass must not kill the watchdog
            log(f"pass failed: {exc}")
        if args.once:
            return 0
        time.sleep(max(1.0, args.interval - (time.monotonic() - started)))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
