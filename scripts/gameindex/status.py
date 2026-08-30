"""status.py — what the favourites agent did on its last pass, for the wall.

`sync.py --status` prints the DB for a human. This publishes the *service's*
own health for a machine: when it last ran, how long it took, how many boxes
it reached, how many favourites files it actually rewrote, and what went wrong.

The distinction that matters, and the reason this file exists rather than the
dashboard inferring health from a log tail: **"nothing to do" and "did not run"
look identical from outside.** The fleet is powered on demand, so a perfectly
healthy pass across zero live boxes writes nothing and logs almost nothing. If
the wall judged the agent by its output it would show a dead service every time
the retro machines were switched off. So the agent states outright that a pass
completed, at what time, and with what result.
"""

import json
import os
import time

SCHEMA_VERSION = 1


def default_status_path():
    """/run/user/<uid>/retro-gameindex/status.json.

    Same reasoning as the game-server watchdog: per-user tmpfs, readable by the
    root dashboard collector, and NOT /tmp -- the greeter that finally displays
    this runs as a systemd DynamicUser and cannot see /tmp at all.
    """
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(runtime, "retro-gameindex", "status.json")


def summarize_servers(con):
    """Per-engine live-server counts straight from the index DB."""
    out = {}
    try:
        rows = con.execute(
            "SELECT engine, COUNT(*) n, SUM(players) p FROM servers "
            "GROUP BY engine ORDER BY engine")
        for r in rows:
            out[r["engine"]] = {"servers": r["n"], "players": r["p"] or 0}
    except Exception:
        pass
    return out


def summarize_favorites(con):
    """How many favourites files we are currently maintaining, per box."""
    out = {"files": 0, "boxes": 0, "last_write": None}
    try:
        row = con.execute(
            "SELECT COUNT(*) n, COUNT(DISTINCT ip) b, MAX(applied_at) t "
            "FROM favorites_state").fetchone()
        if row:
            out = {"files": row["n"] or 0, "boxes": row["b"] or 0,
                   "last_write": row["t"]}
    except Exception:
        pass
    return out


def publish(state, path=None):
    """Atomic write, world-readable — the reader is the root collector."""
    path = path or default_status_path()
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
    return path


def new_report():
    return {
        "schema": SCHEMA_VERSION,
        "started_at": time.time(),
        "ts": None,
        "duration_sec": None,
        "ok": False,
        "phase": "starting",
        "agents": [],
        "machines": [],
        "engines": [],
        "servers": {},
        "favorites": {},
        # `busy` is deliberately NOT folded into `skipped`. A box we did not
        # attempt because a game was running still needs the next pass to
        # reach it; a title with no writer never will. Collapsing the two
        # makes a fleet that silently never got written look like a fleet
        # that needed nothing.
        "writes": {"wrote": 0, "unchanged": 0, "skipped": 0, "busy": 0,
                   "failed": 0},
        "errors": [],
    }
