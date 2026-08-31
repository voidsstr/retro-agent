#!/usr/bin/env python3
r"""The fleet-wide compatibility database - which games work on which boxes.

WHERE IT LIVES, AND WHY IT IS NOT A FIFTH DATABASE
==================================================
These tables live in **`~/.retro-fleet/fleetbook.db`**, beside the recipes and
the per-machine change log, because that file is already "the fleet's
persistent memory about the retro agents" and the user's instruction was that
everything about the retro agents belongs in ONE database.  There were four
SQLite files before this and adding a fifth would have been the exact mistake:

  fleetbook.db     recipes + per-machine change log   <- WE EXTEND THIS
  gamegate.db      the capability gate's verdict cache  (an INPUT; we ingest it)
  gameservers.db   live game-server + installed-game state (an INPUT)
  gameindex.db     0 bytes, never populated

`gamegate.db` and `gameservers.db` keep their own files on purpose: they are
**caches owned by other tools that write them concurrently**, and their rows are
derived, cheap to recompute and keyed on things (`profile_hash`) that change
under us.  We READ them on `ingest` and copy what we need in, tagged with where
it came from.  Nothing here writes back into them.

THE SIX DISTINCTIONS THE SCHEMA MUST NOT COLLAPSE
=================================================
Every one of these has already cost this project real time, so each gets its
own column or its own table rather than being folded into a boolean.

1.  **deployed != runs != verified.**  `GAMESYNC` reporting `state=done` means
    files are on the disk.  It is not evidence the game starts, and starting is
    not evidence anybody watched it render.  Three tables: `compat_deploy`,
    `compat_render`, `compat_mp`.

2.  **gated != skipped != failed.**  Three different follow-ups.  Gated = the
    capability gate refused, and the row carries the limiting factor AND BOTH
    NUMBERS (`have`, `need`) because "CPU too slow" without the two figures
    cannot be argued with.  Skipped = it did not fit on the disk.  Failed = the
    copy errored.  Collapsing these into "not deployed" loses the remedy.

3.  **rendering is per box, by construction.**  One staged tree serves eight
    different monitors, so resolution / refresh / fullscreen / renderer
    (Glide, D3D, OpenGL, software, DOSBox) are recorded per `(ip, title)`, and
    per *shortcut* where a title's halves differ - the gate is already
    per-shortcut and BF1942's single-player and LAN launchers need different
    machines.

4.  **multiplayer is not a boolean.**  It is one of eight states, and the
    interesting ones carry a reason.  `no_multiplayer` MEASURED (Max Payne
    imports no winsock at all) is a finding; `untested` is an absence.

5.  **evidence, with a timestamp.**  A verified row with no evidence is an
    opinion.  `compat_evidence` holds the screenshot path or the log line, and
    when it was measured - not when it was typed in.

6.  **never tested != tested and failed != not applicable.**  Three states,
    never two.  This is the standing rule here and the reason the matrix
    materialises the FULL cross product: a missing row renders as the explicit
    string `untested`, never as blank and never as a pass.

HAND-RECORDED BEATS MACHINE-DERIVED, AND NEITHER IS EVER LOST
=============================================================
`origin` is part of the PRIMARY KEY of all three fact tables.  A hand-recorded
verification (`origin='measured'`) and a machine-derived guess
(`origin='derived'`) for the same cell are two rows that coexist forever; the
`v_*` views pick `measured` first, and `v_compat_conflict` lists every cell
where the two disagree so the disagreement is visible rather than resolved
silently.  An `ingest` can therefore be re-run any number of times without ever
destroying something a person watched happen.

CONCURRENCY
===========
Other agents write `fleetbook.db` at the same time (the chat brain logs changes
there).  The file is already WAL.  Every connection here sets a busy timeout and
every schema statement is `IF NOT EXISTS` / additive, so a migration can run
while another writer holds the file.  No table is ever dropped or rewritten.
"""

from __future__ import annotations

import datetime as _dt
import os
import sqlite3
from pathlib import Path

DEFAULT_DB = Path(os.path.expanduser("~/.retro-fleet/fleetbook.db"))
GAMEGATE_DB = Path(os.path.expanduser("~/.retro-fleet/gamegate.db"))
GAMESERVERS_DB = Path(os.path.expanduser("~/.retro-fleet/gameservers.db"))

# A cell measured longer ago than this may no longer describe the box.  The
# fleet is powered ON DEMAND and staged trees are redeployed often, so this is
# deliberately shortish for verifications - unlike hardware records, a "Quake 3
# rendered fullscreen" from six weeks ago has survived several GAMESYNCs.
DEFAULT_STALE_DAYS = 30

# ---------------------------------------------------------------------------
# The enumerations.  Anything not in these lists is a bug, not a new state:
# the whole point is that a reader can enumerate the possibilities.
# ---------------------------------------------------------------------------

#: Is the title's tree ON the box, and if not, WHY not.
DEPLOY_STATES = (
    "deployed",   # files are on the box (GAMESYNC copied it, or we looked)
    "gated",      # the capability gate refused it - see limiting/have/need
    "marginal",   # the gate let it through but flagged it as borderline
    "skipped",    # did not fit on the disk.  Different follow-up from gated.
    "failed",     # the copy errored (failed_files > 0)
    "absent",     # positively looked for and not there
    "untested",   # nobody has asked.  THE DEFAULT FOR A MISSING ROW.
)

#: Does it START, on this box.  Rendering detail hangs off the same row.
RUN_STATES = (
    "verified",   # someone watched it render and there is evidence
    "runs",       # it started; nobody characterised the rendering
    "failed",     # it was launched and did not run
    "untested",
    "n/a",        # cannot apply - the title is not deployed here at all
)

#: How the picture is produced on THIS box.  One staged tree, eight monitors.
RENDERERS = ("glide", "d3d", "opengl", "software", "dosbox", "ddraw", "unknown")

#: Multiplayer is not a boolean.  These are the states the fleet actually has.
MP_STATES = (
    "verified_two_box",     # host on A, join from B, BOTH ends screenshotted
    "verified_server",      # joined a dedicated server hosted on 192.168.1.132
    "peer_unproven",        # peer-hosted by design; the two-box gather not done
    "needs_human",          # one manual step blocks it - see blocker/remedy
    "no_multiplayer",       # MEASURED to have none (not "we did not find any")
    "blocked",              # a real defect stops it - see blocker
    "untested",
    "n/a",
)

#: How multiplayer reaches the wire.  Recorded because the remedy differs.
TRANSPORTS = ("goldsrc", "idtech2", "idtech3", "idtech4", "ue1", "ue2",
              "directplay", "ipx-tunnel", "udp-native", "westwood-peer",
              "quake-control", "other", "unknown")

#: WHO said so.  A hand-recorded verification outranks a machine-derived guess
#: and the two are never merged - see the module docstring.
ORIGINS = ("measured", "derived")

#: What produced the row.  Free-ish, but these are the ones ingest writes.
SOURCES = ("gamegate.db", "gamesync", "lan-doc", "fleet-inventory",
           "gameservers.db", "fleetbook-changes", "manual", "perbox")

SCHEMA = r"""
CREATE TABLE IF NOT EXISTS compat_box (
    ip            TEXT PRIMARY KEY,
    hostname      TEXT NOT NULL DEFAULT '',
    profile_hash  TEXT NOT NULL DEFAULT '',
    os            TEXT NOT NULL DEFAULT '',
    cpu           TEXT NOT NULL DEFAULT '',
    cpu_mhz       INTEGER,
    ram_mb        INTEGER,
    gpu           TEXT NOT NULL DEFAULT '',
    gpu_class     TEXT NOT NULL DEFAULT '',
    accelerators  TEXT NOT NULL DEFAULT '',
    display_mode  TEXT NOT NULL DEFAULT '',
    agent_version TEXT NOT NULL DEFAULT '',
    state         TEXT NOT NULL DEFAULT 'unknown',
    note          TEXT NOT NULL DEFAULT '',
    measured_at   TEXT,
    source        TEXT NOT NULL DEFAULT '',
    updated_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS compat_title (
    title        TEXT PRIMARY KEY,
    display_name TEXT NOT NULL DEFAULT '',
    engine       TEXT NOT NULL DEFAULT '',
    parent       TEXT NOT NULL DEFAULT '',
    kind         TEXT NOT NULL DEFAULT 'title',
    mp_design    TEXT NOT NULL DEFAULT 'unknown',
    in_library   INTEGER NOT NULL DEFAULT 1,
    note         TEXT NOT NULL DEFAULT '',
    updated_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS compat_deploy (
    ip          TEXT NOT NULL,
    title       TEXT NOT NULL,
    origin      TEXT NOT NULL,
    state       TEXT NOT NULL,
    gate        TEXT NOT NULL DEFAULT '',
    reason      TEXT NOT NULL DEFAULT '',
    limiting    TEXT NOT NULL DEFAULT '',
    have        TEXT NOT NULL DEFAULT '',
    need        TEXT NOT NULL DEFAULT '',
    decided_by  TEXT NOT NULL DEFAULT '',
    confidence  REAL,
    source      TEXT NOT NULL DEFAULT '',
    measured_at TEXT,
    updated_at  TEXT NOT NULL,
    PRIMARY KEY (ip, title, origin)
);

CREATE TABLE IF NOT EXISTS compat_render (
    ip          TEXT NOT NULL,
    title       TEXT NOT NULL,
    shortcut    TEXT NOT NULL DEFAULT '',
    origin      TEXT NOT NULL,
    runs        TEXT NOT NULL,
    renderer    TEXT NOT NULL DEFAULT 'unknown',
    width       INTEGER,
    height      INTEGER,
    refresh_hz  INTEGER,
    fullscreen  TEXT NOT NULL DEFAULT 'unknown',
    detail      TEXT NOT NULL DEFAULT '',
    source      TEXT NOT NULL DEFAULT '',
    measured_at TEXT,
    updated_at  TEXT NOT NULL,
    PRIMARY KEY (ip, title, shortcut, origin)
);

CREATE TABLE IF NOT EXISTS compat_mp (
    ip          TEXT NOT NULL,
    title       TEXT NOT NULL,
    origin      TEXT NOT NULL,
    status      TEXT NOT NULL,
    partner_ip  TEXT NOT NULL DEFAULT '',
    role        TEXT NOT NULL DEFAULT '',
    transport   TEXT NOT NULL DEFAULT 'unknown',
    blocker     TEXT NOT NULL DEFAULT '',
    remedy      TEXT NOT NULL DEFAULT '',
    source      TEXT NOT NULL DEFAULT '',
    measured_at TEXT,
    updated_at  TEXT NOT NULL,
    PRIMARY KEY (ip, title, origin)
);

CREATE TABLE IF NOT EXISTS compat_evidence (
    id          INTEGER PRIMARY KEY,
    ip          TEXT NOT NULL,
    title       TEXT NOT NULL,
    axis        TEXT NOT NULL,
    kind        TEXT NOT NULL,
    ref         TEXT NOT NULL,
    note        TEXT NOT NULL DEFAULT '',
    measured_at TEXT NOT NULL,
    created_at  TEXT NOT NULL,
    UNIQUE (ip, title, axis, kind, ref)
);

CREATE TABLE IF NOT EXISTS compat_ingest (
    id           INTEGER PRIMARY KEY,
    ts           TEXT NOT NULL,
    source       TEXT NOT NULL,
    ok           INTEGER NOT NULL,
    rows_in      INTEGER NOT NULL DEFAULT 0,
    rows_written INTEGER NOT NULL DEFAULT 0,
    detail       TEXT NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_compat_deploy_title ON compat_deploy(title);
CREATE INDEX IF NOT EXISTS idx_compat_render_title ON compat_render(title);
CREATE INDEX IF NOT EXISTS idx_compat_mp_title     ON compat_mp(title);
CREATE INDEX IF NOT EXISTS idx_compat_ev_cell      ON compat_evidence(ip, title);
"""

# The views resolve `measured` over `derived` in ONE place.  Anything reading
# the database - the CLI, the dashboard export, a test - reads these, so the
# precedence rule cannot drift between consumers.
VIEWS = r"""
DROP VIEW IF EXISTS v_compat_deploy;
CREATE VIEW v_compat_deploy AS
SELECT d.* FROM compat_deploy d
WHERE d.origin = (
    SELECT origin FROM compat_deploy x
    WHERE x.ip = d.ip AND x.title = d.title
    ORDER BY CASE x.origin WHEN 'measured' THEN 0 ELSE 1 END LIMIT 1);

DROP VIEW IF EXISTS v_compat_render;
CREATE VIEW v_compat_render AS
SELECT r.* FROM compat_render r
WHERE r.origin = (
    SELECT origin FROM compat_render x
    WHERE x.ip = r.ip AND x.title = r.title AND x.shortcut = r.shortcut
    ORDER BY CASE x.origin WHEN 'measured' THEN 0 ELSE 1 END LIMIT 1);

DROP VIEW IF EXISTS v_compat_mp;
CREATE VIEW v_compat_mp AS
SELECT m.* FROM compat_mp m
WHERE m.origin = (
    SELECT origin FROM compat_mp x
    WHERE x.ip = m.ip AND x.title = m.title
    ORDER BY CASE x.origin WHEN 'measured' THEN 0 ELSE 1 END LIMIT 1);

-- Every cell where a hand-recorded fact and a machine-derived one disagree.
-- These are NOT resolved silently; they are reported so a person can look.
DROP VIEW IF EXISTS v_compat_conflict;
CREATE VIEW v_compat_conflict AS
SELECT 'deploy' AS axis, a.ip, a.title, '' AS shortcut,
       a.state AS measured, b.state AS derived,
       a.source AS measured_source, b.source AS derived_source,
       a.measured_at AS measured_at
  FROM compat_deploy a JOIN compat_deploy b
    ON a.ip=b.ip AND a.title=b.title
 WHERE a.origin='measured' AND b.origin='derived' AND a.state <> b.state
UNION ALL
SELECT 'render', a.ip, a.title, a.shortcut, a.runs, b.runs,
       a.source, b.source, a.measured_at
  FROM compat_render a JOIN compat_render b
    ON a.ip=b.ip AND a.title=b.title AND a.shortcut=b.shortcut
 WHERE a.origin='measured' AND b.origin='derived' AND a.runs <> b.runs
UNION ALL
SELECT 'mp', a.ip, a.title, '', a.status, b.status,
       a.source, b.source, a.measured_at
  FROM compat_mp a JOIN compat_mp b
    ON a.ip=b.ip AND a.title=b.title
 WHERE a.origin='measured' AND b.origin='derived' AND a.status <> b.status;

-- THE MATRIX.  The full cross product of boxes x titles, so a cell nobody has
-- ever looked at is a ROW saying `untested` rather than a row that is missing.
-- A missing row is what lets a never-tested cell render as a pass, which is
-- the single failure this whole database exists to prevent.
DROP VIEW IF EXISTS v_compat_matrix;
CREATE VIEW v_compat_matrix AS
SELECT b.ip, b.hostname, t.title, t.in_library, t.kind, t.parent,
       t.engine, t.display_name,
       COALESCE(d.state,  'untested') AS deploy,
       COALESCE(d.gate, '')           AS gate,
       COALESCE(d.limiting, '')       AS deploy_limiting,
       COALESCE(d.have, '')           AS deploy_have,
       COALESCE(d.need, '')           AS deploy_need,
       COALESCE(d.reason, '')         AS deploy_reason,
       COALESCE(d.origin, '')         AS deploy_origin,
       COALESCE(r.runs,   'untested') AS runs,
       COALESCE(r.renderer, '')       AS renderer,
       r.width, r.height, r.refresh_hz,
       COALESCE(r.fullscreen, 'unknown') AS fullscreen,
       COALESCE(r.origin, '')         AS render_origin,
       COALESCE(m.status, 'untested') AS mp,
       COALESCE(m.partner_ip, '')     AS mp_partner,
       COALESCE(m.transport, '')      AS mp_transport,
       COALESCE(m.blocker, '')        AS mp_blocker,
       COALESCE(m.remedy, '')         AS mp_remedy,
       COALESCE(m.origin, '')         AS mp_origin,
       COALESCE(r.measured_at, m.measured_at, d.measured_at) AS measured_at,
       (SELECT COUNT(*) FROM compat_evidence e
         WHERE e.ip=b.ip AND e.title=t.title) AS evidence
  FROM compat_box b
  CROSS JOIN compat_title t
  LEFT JOIN v_compat_deploy d ON d.ip=b.ip AND d.title=t.title
  LEFT JOIN v_compat_render r ON r.ip=b.ip AND r.title=t.title AND r.shortcut=''
  LEFT JOIN v_compat_mp     m ON m.ip=b.ip AND m.title=t.title;
"""


def now() -> str:
    return _dt.datetime.now().replace(microsecond=0).isoformat(sep=" ")


def connect(path=None, timeout: float = 30.0) -> sqlite3.Connection:
    """Open the fleet database and make sure the compat tables exist.

    Safe to call while another agent is writing: WAL plus a real busy timeout,
    and every statement in SCHEMA/VIEWS is additive.
    """
    p = Path(path) if path else DEFAULT_DB
    p.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(str(p), timeout=timeout)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA busy_timeout=%d" % int(timeout * 1000))
    con.executescript(SCHEMA)
    con.executescript(VIEWS)
    con.commit()
    return con


class BadState(ValueError):
    """A value outside the declared enumeration.

    Raised rather than stored, because the point of the enumerations is that a
    reader can list the possibilities.  A typo'd state silently accepted would
    render as neither a pass nor a fail and would be invisible in every count.
    """


def _check(value, allowed, what):
    if value not in allowed:
        raise BadState("%s=%r is not one of %s" % (what, value, ", ".join(allowed)))
    return value


# ---------------------------------------------------------------------------
# Upserts.  Each takes an explicit `origin` because that is the one field a
# caller must think about: did a person/agent WATCH this, or did a tool infer it?
# ---------------------------------------------------------------------------

def _upsert(con, table, keycols, keyvals, cols, kw):
    """Insert the key row if absent, then update only the fields given.

    Two statements rather than one ON CONFLICT because the columns are
    `NOT NULL DEFAULT ''` and a single upsert would have to pass NULL for every
    field the caller did not mention.  This shape gives the property that
    actually matters: **a partial update never wipes a field somebody else
    filled in.**  An ingest that knows only the hostname must not blank the GPU
    that a different ingest measured.
    """
    con.execute("INSERT OR IGNORE INTO %s (%s,updated_at) VALUES (%s,?)"
                % (table, ",".join(keycols), ",".join("?" * len(keycols))),
                list(keyvals) + [now()])
    given = [c for c in cols if kw.get(c) is not None]
    if given:
        con.execute("UPDATE %s SET %s, updated_at=? WHERE %s"
                    % (table, ",".join("%s=?" % c for c in given),
                       " AND ".join("%s=?" % c for c in keycols)),
                    [kw[c] for c in given] + [now()] + list(keyvals))
    con.commit()


def put_box(con, ip, **kw):
    _upsert(con, "compat_box", ("ip",), (ip,),
            ("hostname", "profile_hash", "os", "cpu", "cpu_mhz", "ram_mb",
             "gpu", "gpu_class", "accelerators", "display_mode",
             "agent_version", "state", "note", "measured_at", "source"), kw)


def put_title(con, title, **kw):
    _upsert(con, "compat_title", ("title",), (title,),
            ("display_name", "engine", "parent", "kind", "mp_design",
             "in_library", "note"), kw)


def put_deploy(con, ip, title, origin, state, **kw):
    _check(origin, ORIGINS, "origin")
    _check(state, DEPLOY_STATES, "state")
    con.execute(
        "INSERT OR REPLACE INTO compat_deploy (ip,title,origin,state,gate,"
        "reason,limiting,have,need,decided_by,confidence,source,measured_at,"
        "updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (ip, title, origin, state, kw.get("gate", ""),
         kw.get("reason", ""), kw.get("limiting", ""),
         str(kw.get("have", "") or ""), str(kw.get("need", "") or ""),
         kw.get("decided_by", ""), kw.get("confidence"), kw.get("source", ""),
         kw.get("measured_at"), now()))
    con.commit()


def put_render(con, ip, title, origin, runs, shortcut="", **kw):
    _check(origin, ORIGINS, "origin")
    _check(runs, RUN_STATES, "runs")
    renderer = kw.get("renderer", "unknown") or "unknown"
    _check(renderer, RENDERERS, "renderer")
    con.execute(
        "INSERT OR REPLACE INTO compat_render (ip,title,shortcut,origin,runs,"
        "renderer,width,height,refresh_hz,fullscreen,detail,source,"
        "measured_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (ip, title, shortcut, origin, runs, renderer, kw.get("width"),
         kw.get("height"), kw.get("refresh_hz"),
         kw.get("fullscreen", "unknown") or "unknown", kw.get("detail", ""),
         kw.get("source", ""), kw.get("measured_at"), now()))
    con.commit()


def put_mp(con, ip, title, origin, status, **kw):
    _check(origin, ORIGINS, "origin")
    _check(status, MP_STATES, "status")
    transport = kw.get("transport", "unknown") or "unknown"
    _check(transport, TRANSPORTS, "transport")
    con.execute(
        "INSERT OR REPLACE INTO compat_mp (ip,title,origin,status,partner_ip,"
        "role,transport,blocker,remedy,source,measured_at,updated_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        (ip, title, origin, status, kw.get("partner_ip", ""),
         kw.get("role", ""), transport, kw.get("blocker", ""),
         kw.get("remedy", ""), kw.get("source", ""), kw.get("measured_at"),
         now()))
    con.commit()


def put_evidence(con, ip, title, axis, kind, ref, measured_at, note=""):
    if axis not in ("deploy", "render", "mp"):
        raise BadState("axis=%r" % axis)
    con.execute(
        "INSERT OR IGNORE INTO compat_evidence (ip,title,axis,kind,ref,note,"
        "measured_at,created_at) VALUES (?,?,?,?,?,?,?,?)",
        (ip, title, axis, kind, ref, note, measured_at, now()))
    con.commit()


def log_ingest(con, source, ok, rows_in=0, rows_written=0, detail=""):
    con.execute(
        "INSERT INTO compat_ingest (ts,source,ok,rows_in,rows_written,detail)"
        " VALUES (?,?,?,?,?,?)",
        (now(), source, 1 if ok else 0, rows_in, rows_written, detail))
    con.commit()
