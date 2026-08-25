"""db.py — the fleet's game + server index (SQLite).

One file at ~/.retro-fleet/gameservers.db, alongside the fleetbook. Four
tables, each answering one question:

  machines         which boxes have we indexed, and what was the index hash
  installed_games  what is installed on each box
  servers          which internet servers are alive right now, per engine
  favorites_state  what we last WROTE to each box, so we can skip a no-op

`favorites_state.applied_hash` is what makes "refresh only if there are
changes" real: the sync pass computes the favorites payload it would write,
hashes it, and writes nothing when the hash matches what is already on the
box. Without it every cycle would rewrite every config file on every machine
every five minutes, which on a Pentium III over SMB is not free -- and would
also clobber a game's config while someone is playing.

WAL mode: the sync timer, an operator on the CLI, and the chat brain all read
this concurrently.
"""
import os
import sqlite3
import time
from pathlib import Path

DB_PATH = os.environ.get(
    "RETRO_GAMEINDEX_DB",
    str(Path.home() / ".retro-fleet" / "gameservers.db"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS machines (
    ip            TEXT PRIMARY KEY,
    hostname      TEXT DEFAULT '',
    os            TEXT DEFAULT '',
    agent_version TEXT DEFAULT '',
    index_hash    TEXT DEFAULT '',
    indexed_at    TEXT,
    last_seen     TEXT,
    note          TEXT DEFAULT ''
);

CREATE TABLE IF NOT EXISTS installed_games (
    ip        TEXT NOT NULL,
    game_key  TEXT NOT NULL,
    dir       TEXT NOT NULL,
    name      TEXT DEFAULT '',
    engine    TEXT DEFAULT '',
    exe       TEXT DEFAULT '',
    launcher  TEXT DEFAULT '',
    source    TEXT DEFAULT '',
    seen_at   TEXT,
    PRIMARY KEY (ip, game_key, dir)
);
CREATE INDEX IF NOT EXISTS idx_games_engine ON installed_games(engine);
CREATE INDEX IF NOT EXISTS idx_games_ip     ON installed_games(ip);

CREATE TABLE IF NOT EXISTS servers (
    engine       TEXT NOT NULL,
    addr         TEXT NOT NULL,           -- "ip:port"
    hostname     TEXT DEFAULT '',
    map          TEXT DEFAULT '',
    players      INTEGER DEFAULT 0,
    maxplayers   INTEGER DEFAULT 0,
    ping_ms      INTEGER DEFAULT 0,
    gamename     TEXT DEFAULT '',
    passworded   INTEGER DEFAULT 0,
    is_local     INTEGER DEFAULT 0,       -- our own server on .132
    source       TEXT DEFAULT '',
    first_seen   TEXT,
    last_seen    TEXT,
    PRIMARY KEY (engine, addr)
);
CREATE INDEX IF NOT EXISTS idx_servers_live
    ON servers(engine, players DESC, ping_ms ASC);

CREATE TABLE IF NOT EXISTS favorites_state (
    ip           TEXT NOT NULL,
    game_key     TEXT NOT NULL,
    dir          TEXT NOT NULL,
    applied_hash TEXT DEFAULT '',
    applied_at   TEXT,
    detail       TEXT DEFAULT '',
    PRIMARY KEY (ip, game_key, dir)
);
"""


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def connect(path=None):
    con = sqlite3.connect(path or DB_PATH, timeout=30)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA busy_timeout=30000")
    con.executescript(SCHEMA)
    return con


# --- machines + installed games ---------------------------------------------

def record_machine(con, ip, hostname="", os_ver="", agent_version="",
                   index_hash=None):
    cur = con.execute("SELECT index_hash FROM machines WHERE ip=?", (ip,))
    row = cur.fetchone()
    if row is None:
        con.execute(
            "INSERT INTO machines(ip,hostname,os,agent_version,index_hash,"
            "indexed_at,last_seen) VALUES(?,?,?,?,?,?,?)",
            (ip, hostname, os_ver, agent_version, index_hash or "",
             now() if index_hash else None, now()))
        return True  # changed
    changed = index_hash is not None and row["index_hash"] != index_hash
    con.execute(
        "UPDATE machines SET hostname=?, os=?, agent_version=?, last_seen=?"
        + (", index_hash=?, indexed_at=?" if changed else "")
        + " WHERE ip=?",
        ((hostname, os_ver, agent_version, now(), index_hash, now(), ip)
         if changed else (hostname, os_ver, agent_version, now(), ip)))
    return changed


def replace_games(con, ip, games):
    """Replace this machine's game list wholesale.

    Wholesale, not upsert: a game that was uninstalled must disappear, and the
    agent's index is the complete truth for that box at that moment.
    """
    con.execute("DELETE FROM installed_games WHERE ip=?", (ip,))
    con.executemany(
        "INSERT OR REPLACE INTO installed_games"
        "(ip,game_key,dir,name,engine,exe,launcher,source,seen_at)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        [(ip, g.get("key", ""), g.get("dir", ""), g.get("name", ""),
          g.get("engine", ""), g.get("exe", ""), g.get("launcher", ""),
          g.get("source", ""), now()) for g in games])


def games_for(con, ip=None, engine=None):
    q = "SELECT * FROM installed_games WHERE 1=1"
    args = []
    if ip:
        q += " AND ip=?"
        args.append(ip)
    if engine:
        q += " AND engine=?"
        args.append(engine)
    return con.execute(q + " ORDER BY ip, game_key", args).fetchall()


def engines_in_use(con):
    """Engines actually present on the fleet -- do not scan masters for the rest."""
    return [r["engine"] for r in con.execute(
        "SELECT DISTINCT engine FROM installed_games "
        "WHERE engine NOT IN ('', '-') ORDER BY engine")]


# --- servers -----------------------------------------------------------------

def upsert_servers(con, engine, rows):
    for r in rows:
        con.execute(
            "INSERT INTO servers(engine,addr,hostname,map,players,maxplayers,"
            "ping_ms,gamename,passworded,is_local,source,first_seen,last_seen)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(engine,addr) DO UPDATE SET"
            " hostname=excluded.hostname, map=excluded.map,"
            " players=excluded.players, maxplayers=excluded.maxplayers,"
            " ping_ms=excluded.ping_ms, gamename=excluded.gamename,"
            " passworded=excluded.passworded, is_local=excluded.is_local,"
            " source=excluded.source, last_seen=excluded.last_seen",
            (engine, r["addr"], r.get("hostname", ""), r.get("map", ""),
             r.get("players", 0), r.get("maxplayers", 0), r.get("ping_ms", 0),
             r.get("gamename", ""), int(r.get("passworded", 0)),
             int(r.get("is_local", 0)), r.get("source", ""), now(), now()))


def prune_servers(con, older_than_s=3600):
    """Drop servers we have not seen in a while, so favorites stay joinable."""
    cutoff = time.strftime("%Y-%m-%d %H:%M:%S",
                           time.localtime(time.time() - older_than_s))
    return con.execute("DELETE FROM servers WHERE last_seen < ? AND is_local=0",
                       (cutoff,)).rowcount


def best_servers(con, engine, limit=16, fresh_s=900):
    """Live servers for an engine, our own first, then busiest-and-closest.

    Deduped by host IP. Big hosts run eight ports of the same server and would
    otherwise eat every favorite slot -- that is a lesson from the Q3 recipe,
    not a hypothetical.
    """
    cutoff = time.strftime("%Y-%m-%d %H:%M:%S",
                           time.localtime(time.time() - fresh_s))
    rows = con.execute(
        "SELECT * FROM servers WHERE engine=? AND passworded=0"
        " AND (is_local=1 OR (last_seen >= ? AND players > 0))"
        " ORDER BY is_local DESC, players DESC, ping_ms ASC",
        (engine, cutoff)).fetchall()
    out, seen_hosts = [], set()
    for r in rows:
        host = r["addr"].rsplit(":", 1)[0]
        if host in seen_hosts:
            continue
        seen_hosts.add(host)
        out.append(r)
        if len(out) >= limit:
            break
    return out


# --- favorites state ---------------------------------------------------------

def applied_hash(con, ip, game_key, dir_):
    row = con.execute(
        "SELECT applied_hash FROM favorites_state WHERE ip=? AND game_key=? AND dir=?",
        (ip, game_key, dir_)).fetchone()
    return row["applied_hash"] if row else None


def record_applied(con, ip, game_key, dir_, h, detail=""):
    con.execute(
        "INSERT INTO favorites_state(ip,game_key,dir,applied_hash,applied_at,detail)"
        " VALUES(?,?,?,?,?,?)"
        " ON CONFLICT(ip,game_key,dir) DO UPDATE SET"
        " applied_hash=excluded.applied_hash, applied_at=excluded.applied_at,"
        " detail=excluded.detail",
        (ip, game_key, dir_, h, now(), detail))
