#!/usr/bin/env python3
"""retro_playerprofile.py - per-player profiles and per-game configurations.

A plain-SQLite store (default ~/.retro-fleet/players.db) that answers
"who plays on this fleet, how do they like their games set up, and is that
setup actually on the box right now?".

  players       - a person: handle, display name, free notes.
  prefs         - cross-game preferences (preferred resolution, mouse DPI,
                  handedness, "always vsync", ...). Key/value, per player.
  profiles      - one per (player, game): the player's config for that title.
  settings      - the cvars/ini keys of a profile, with the console command
                  word preserved (seta/set/setu) so a round-trip is faithful.
  binds         - key -> action, kept apart from settings because binds are
                  what players actually argue about.
  gamedefs      - where a game's config file lives on a box and what format
                  it is. Seeded with the fleet's usual titles; extend with
                  `gamedef`.
  deployments   - the audit trail: every capture from / apply to a machine.

The point of the split is that a profile is authored ONCE and pushed to any
box: capture Ian's Quake 3 config off .133, then apply it to .185 and .124.

Usage:
  retro_playerprofile.py create <handle> [--name N] [--notes T]
  retro_playerprofile.py list
  retro_playerprofile.py show <handle>
  retro_playerprofile.py set <handle> key=value [key=value ...]
  retro_playerprofile.py games
  retro_playerprofile.py gamedef <game> [--path P] [--format quake|ini]
                                        [--cmd seta] [--launch L]
  retro_playerprofile.py game-set <handle> <game> key=value [...] [--cmd set]
  retro_playerprofile.py bind <handle> <game> <key> <action>
  retro_playerprofile.py unset <handle> <game> <key> [--bind]
  retro_playerprofile.py game-show <handle> <game>
  retro_playerprofile.py import <handle> <game> --file F [--source S]
  retro_playerprofile.py render <handle> <game> [--out F]
  retro_playerprofile.py capture <handle> <game> --host IP [--path P]
  retro_playerprofile.py apply <handle> <game> --host IP [--path P] [--dry-run]
  retro_playerprofile.py history [--handle H] [--host IP] [-n N]

`capture` and `apply` talk to a fleet agent through client/retro_protocol.py.
`apply` always downloads and archives the box's existing file first, so a push
is reversible; `--dry-run` prints the config instead of sending it.

DB override for tests: RETRO_PLAYERS_DB=/path/to.db
"""
import argparse
import datetime
import os
import re
import sqlite3
import sys

DB_PATH = os.environ.get(
    "RETRO_PLAYERS_DB",
    os.path.join(os.path.expanduser("~"), ".retro-fleet", "players.db"))

AGENT_SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")

SCHEMA = """
CREATE TABLE IF NOT EXISTS players (
    id           INTEGER PRIMARY KEY,
    handle       TEXT UNIQUE NOT NULL,
    display_name TEXT DEFAULT '',
    notes        TEXT DEFAULT '',
    created_at   TEXT NOT NULL,
    updated_at   TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS prefs (
    player_id  INTEGER NOT NULL REFERENCES players(id),
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY (player_id, key)
);
CREATE TABLE IF NOT EXISTS gamedefs (
    game        TEXT PRIMARY KEY,
    config_path TEXT DEFAULT '',
    format      TEXT DEFAULT 'quake',
    cvar_cmd    TEXT DEFAULT 'seta',
    launch      TEXT DEFAULT '',
    updated_at  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS profiles (
    id         INTEGER PRIMARY KEY,
    player_id  INTEGER NOT NULL REFERENCES players(id),
    game       TEXT NOT NULL,
    notes      TEXT DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE (player_id, game)
);
CREATE TABLE IF NOT EXISTS settings (
    profile_id INTEGER NOT NULL REFERENCES profiles(id),
    key        TEXT NOT NULL,
    value      TEXT NOT NULL,
    cmd        TEXT DEFAULT '',
    updated_at TEXT NOT NULL,
    PRIMARY KEY (profile_id, key)
);
CREATE TABLE IF NOT EXISTS binds (
    profile_id INTEGER NOT NULL REFERENCES profiles(id),
    key        TEXT NOT NULL,
    action     TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    PRIMARY KEY (profile_id, key)
);
CREATE TABLE IF NOT EXISTS deployments (
    id         INTEGER PRIMARY KEY,
    ts         TEXT NOT NULL,
    player_id  INTEGER REFERENCES players(id),
    profile_id INTEGER REFERENCES profiles(id),
    host       TEXT NOT NULL,
    path       TEXT DEFAULT '',
    action     TEXT NOT NULL,
    note       TEXT DEFAULT '',
    backup     TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_dep_host ON deployments(host);
CREATE INDEX IF NOT EXISTS idx_dep_player ON deployments(player_id);
"""

# Seeded game definitions. config_path is where the file lives on a fleet box;
# it is a DEFAULT - override per call with --path, or permanently with `gamedef`
# (dual-boot boxes put games on D:, which is exactly why this is editable).
SEED_GAMEDEFS = [
    ("quake3", r"C:\Quake3\baseq3\q3config.cfg", "quake", "seta",
     r"C:\Quake3\quake3.exe"),
    ("quake2", r"C:\Quake2\baseq2\config.cfg", "quake", "set",
     r"C:\Quake2\quake2.exe"),
    ("quake", r"C:\Quake\id1\config.cfg", "quake", "", r"C:\Quake\winquake.exe"),
    ("cs16", r"C:\Half-Life\cstrike\userconfig.cfg", "quake", "",
     r"C:\Half-Life\hl.exe"),
    ("halflife", r"C:\Half-Life\valve\userconfig.cfg", "quake", "",
     r"C:\Half-Life\hl.exe"),
    ("ut99", r"C:\UnrealTournament\System\UnrealTournament.ini", "ini", "",
     r"C:\UnrealTournament\System\UnrealTournament.exe"),
    ("ut2004", r"C:\UT2004\System\UT2004.ini", "ini", "",
     r"C:\UT2004\System\UT2004.exe"),
    ("openarena", r"C:\OpenArena\baseoa\q3config.cfg", "quake", "seta",
     r"C:\OpenArena\openarena.exe"),
    ("seriousssam", r"C:\Serious Sam\Scripts\PersistentSymbols.ini", "quake",
     "", ""),
    ("deusex", r"C:\DeusEx\System\DeusEx.ini", "ini", "",
     r"C:\DeusEx\System\DeusEx.exe"),
]


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def connect():
    d = os.path.dirname(DB_PATH)
    if d:
        os.makedirs(d, exist_ok=True)
    con = sqlite3.connect(DB_PATH, timeout=15)
    con.execute("PRAGMA journal_mode=WAL")
    con.executescript(SCHEMA)
    con.row_factory = sqlite3.Row
    t = now()
    for g, p, f, c, l in SEED_GAMEDEFS:
        con.execute("INSERT OR IGNORE INTO gamedefs "
                    "(game,config_path,format,cvar_cmd,launch,updated_at) "
                    "VALUES (?,?,?,?,?,?)", (g, p, f, c, l, t))
    con.commit()
    return con


def norm_game(g):
    return re.sub(r"[^a-z0-9]+", "", str(g).lower())


def get_player(con, handle, required=True):
    r = con.execute("SELECT * FROM players WHERE handle=?",
                    (str(handle).lower(),)).fetchone()
    if not r and required:
        sys.exit("no such player: %s  (create it with: create %s)"
                 % (handle, handle))
    return r


def get_gamedef(con, game):
    return con.execute("SELECT * FROM gamedefs WHERE game=?",
                       (norm_game(game),)).fetchone()


def get_profile(con, player_id, game, create=False):
    g = norm_game(game)
    r = con.execute("SELECT * FROM profiles WHERE player_id=? AND game=?",
                    (player_id, g)).fetchone()
    if r or not create:
        return r
    t = now()
    con.execute("INSERT INTO profiles (player_id,game,created_at,updated_at) "
                "VALUES (?,?,?,?)", (player_id, g, t, t))
    con.commit()
    return con.execute("SELECT * FROM profiles WHERE player_id=? AND game=?",
                       (player_id, g)).fetchone()


def split_kv(pairs):
    """['fov=110', 'sensitivity 3'] -> [('fov','110'), ('sensitivity','3')]."""
    out = []
    for p in pairs:
        if "=" in p:
            k, v = p.split("=", 1)
        elif " " in p.strip():
            k, v = p.strip().split(None, 1)
        else:
            sys.exit("setting must be key=value, got: %s" % p)
        out.append((k.strip(), v.strip().strip('"')))
    return out


# ---- config text parse / render ----

CVAR_RE = re.compile(r'^\s*(seta|setu|sets|set)?\s*([A-Za-z_][\w\.]*)\s+'
                     r'("(?:[^"\\]|\\.)*"|\S+)\s*$')
BIND_RE = re.compile(r'^\s*bind\s+("[^"]+"|\S+)\s+("(?:[^"\\]|\\.)*"|\S+)\s*$',
                     re.I)


def unquote(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


def parse_quake(text):
    """Parse a Quake/GoldSrc-family console config. -> (settings, binds).

    settings maps key -> (value, cmd) where cmd is the original command word
    ('seta', 'set', or '' for a bare `name "value"` GoldSrc line). Preserving
    it matters: writing `seta` into a GoldSrc config is a syntax error, and
    writing a bare line into q3config.cfg loses the archive flag.
    """
    settings, binds = {}, {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        m = BIND_RE.match(line)
        if m:
            binds[unquote(m.group(1))] = unquote(m.group(2))
            continue
        m = CVAR_RE.match(line)
        if m:
            cmd, key, val = m.group(1) or "", m.group(2), unquote(m.group(3))
            if key.lower() in ("bind", "unbind", "unbindall", "exec", "echo"):
                continue
            settings[key] = (val, cmd)
    return settings, binds


def render_quake(settings, binds, default_cmd="seta"):
    """settings: list of (key, value, cmd). Emitted with CRLF for DOS/Win."""
    lines = ["// generated by retro_playerprofile.py - do not hand-edit",
             "// edits here are overwritten on the next `apply`"]
    for key, value, cmd in settings:
        c = (cmd or default_cmd).strip()
        lines.append(('%s %s "%s"' % (c, key, value)).strip()
                     if c else '%s "%s"' % (key, value))
    if binds:
        lines.append("")
        for key, action in binds:
            lines.append('bind %s "%s"' % (key, action))
    return "\r\n".join(lines) + "\r\n"


def parse_ini(text):
    """Parse an Unreal-engine style ini. Keys are stored 'Section.Key'."""
    settings, section = {}, ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("//"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            key = "%s.%s" % (section, k.strip()) if section else k.strip()
            settings[key] = (v.strip(), "")
    return settings, {}


def render_ini(settings, binds, default_cmd=""):
    """settings: list of (key, value, cmd) with keys as 'Section.Key'.

    Only the keys the profile owns are emitted, so this is a PATCH, not a
    whole ini - see apply_ini_patch, which merges it into the box's file.
    """
    bysec = {}
    for key, value, _cmd in settings:
        sec, _, k = key.rpartition(".")
        bysec.setdefault(sec, []).append((k, value))
    out = []
    for sec in sorted(bysec):
        if sec:
            out.append("[%s]" % sec)
        for k, v in bysec[sec]:
            out.append("%s=%s" % (k, v))
        out.append("")
    return "\r\n".join(out) + "\r\n"


def apply_ini_patch(original, settings):
    """Merge (key,value,cmd) triples into an existing ini, preserving
    everything else. A game ini holds hundreds of engine keys we do not model;
    overwriting the file with only the profile's keys would break the game."""
    want = {}
    for key, value, _cmd in settings:
        sec, _, k = key.rpartition(".")
        want.setdefault(sec, {})[k.lower()] = (k, value)
    out, section, seen = [], "", {}
    for raw in original.splitlines():
        line = raw.rstrip("\r\n")
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            # flush any unseen keys for the section we are leaving
            for lk, (k, v) in want.get(section, {}).items():
                if lk not in seen.get(section, set()):
                    out.append("%s=%s" % (k, v))
                    seen.setdefault(section, set()).add(lk)
            section = s[1:-1].strip()
            out.append(line)
            continue
        if "=" in s and not s.startswith(";"):
            k = s.split("=", 1)[0].strip()
            hit = want.get(section, {}).get(k.lower())
            if hit:
                out.append("%s=%s" % (hit[0], hit[1]))
                seen.setdefault(section, set()).add(k.lower())
                continue
        out.append(line)
    for lk, (k, v) in want.get(section, {}).items():
        if lk not in seen.get(section, set()):
            out.append("%s=%s" % (k, v))
            seen.setdefault(section, set()).add(lk)
    # sections that were not in the file at all
    for sec, keys in want.items():
        missing = [(k, v) for lk, (k, v) in keys.items()
                   if lk not in seen.get(sec, set())]
        if not missing:
            continue
        out.append("")
        if sec:
            out.append("[%s]" % sec)
        for k, v in missing:
            out.append("%s=%s" % (k, v))
    return "\r\n".join(out) + "\r\n"


def profile_rows(con, profile_id):
    s = con.execute("SELECT key,value,cmd FROM settings WHERE profile_id=? "
                    "ORDER BY key", (profile_id,)).fetchall()
    b = con.execute("SELECT key,action FROM binds WHERE profile_id=? "
                    "ORDER BY key", (profile_id,)).fetchall()
    return ([(r["key"], r["value"], r["cmd"]) for r in s],
            [(r["key"], r["action"]) for r in b])


def render_profile(con, profile, gamedef):
    settings, binds = profile_rows(con, profile["id"])
    fmt = (gamedef["format"] if gamedef else "quake")
    cmd = (gamedef["cvar_cmd"] if gamedef else "seta")
    if fmt == "ini":
        return render_ini(settings, binds, cmd)
    return render_quake(settings, binds, cmd)


def store_parsed(con, profile_id, settings, binds):
    t, n = now(), 0
    for key, (value, cmd) in settings.items():
        con.execute("INSERT INTO settings (profile_id,key,value,cmd,updated_at)"
                    " VALUES (?,?,?,?,?) ON CONFLICT(profile_id,key) DO UPDATE "
                    "SET value=excluded.value, cmd=excluded.cmd, "
                    "updated_at=excluded.updated_at",
                    (profile_id, key, value, cmd, t))
        n += 1
    for key, action in binds.items():
        con.execute("INSERT INTO binds (profile_id,key,action,updated_at) "
                    "VALUES (?,?,?,?) ON CONFLICT(profile_id,key) DO UPDATE "
                    "SET action=excluded.action, updated_at=excluded.updated_at",
                    (profile_id, key, action, t))
    con.execute("UPDATE profiles SET updated_at=? WHERE id=?", (t, profile_id))
    con.commit()
    return n, len(binds)


def parse_config(text, fmt):
    return parse_ini(text) if fmt == "ini" else parse_quake(text)


# ---- fleet agent I/O ----

def _agent_call(host, fn):
    """Run one coroutine against a fleet agent on `host`, closing cleanly.

    The connection is ALWAYS closed in a finally: an abrupt TCP disconnect
    crashes Win98's Winsock and takes the whole box down (see CLAUDE.md).
    """
    import asyncio
    sys.path.insert(0, os.path.abspath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
    from client.retro_protocol import RetroConnection

    async def run():
        con = RetroConnection(host, 9898)
        await con.connect(AGENT_SECRET, timeout=15.0)
        try:
            return await fn(con)
        finally:
            await con.close()

    return asyncio.run(run())


def agent_read_file(host, path):
    """Download a text file from a box. Returns None if it does not exist."""
    async def fn(con):
        status, data = await con.send_command("DOWNLOAD %s" % path)
        if status != 0:
            return None
        return data
    data = _agent_call(host, fn)
    if data is None:
        return None
    return data.decode("latin-1")


def agent_write_file(host, path, text):
    payload = text.encode("latin-1", errors="replace")

    async def fn(con):
        parent = path.rsplit("\\", 1)[0]
        if parent and parent != path:
            await con.send_command("MKDIR %s" % parent)
        status, data = await con.send_command("UPLOAD %s" % path,
                                              binary_payload=payload)
        if status != 0:
            raise RuntimeError("UPLOAD %s failed: %s"
                               % (path, data.decode("ascii", "replace")))
        return True
    return _agent_call(host, fn)


def log_deployment(con, player_id, profile_id, host, path, action,
                   note="", backup=""):
    con.execute("INSERT INTO deployments (ts,player_id,profile_id,host,path,"
                "action,note,backup) VALUES (?,?,?,?,?,?,?,?)",
                (now(), player_id, profile_id, host, path, action, note,
                 backup))
    con.commit()


# ---- commands ----

def cmd_create(con, a):
    h = a.handle.lower()
    if con.execute("SELECT 1 FROM players WHERE handle=?", (h,)).fetchone():
        sys.exit("player already exists: %s" % h)
    t = now()
    con.execute("INSERT INTO players (handle,display_name,notes,created_at,"
                "updated_at) VALUES (?,?,?,?,?)",
                (h, a.name or "", a.notes or "", t, t))
    con.commit()
    print("created player %s" % h)


def cmd_list(con, a):
    rows = con.execute("SELECT * FROM players ORDER BY handle").fetchall()
    if not rows:
        print("no players yet  (create one with: create <handle>)")
        return
    for p in rows:
        n = con.execute("SELECT COUNT(*) c FROM profiles WHERE player_id=?",
                        (p["id"],)).fetchone()["c"]
        print("%-16s %-24s %d game profile%s"
              % (p["handle"], p["display_name"] or "-", n,
                 "" if n == 1 else "s"))


def cmd_show(con, a):
    p = get_player(con, a.handle)
    print("player:   %s" % p["handle"])
    if p["display_name"]:
        print("name:     %s" % p["display_name"])
    if p["notes"]:
        print("notes:    %s" % p["notes"])
    print("created:  %s" % p["created_at"])
    prefs = con.execute("SELECT key,value FROM prefs WHERE player_id=? "
                        "ORDER BY key", (p["id"],)).fetchall()
    print("--- preferences (all games) ---")
    if not prefs:
        print("(none)")
    for r in prefs:
        print("  %-24s %s" % (r["key"], r["value"]))
    profs = con.execute("SELECT * FROM profiles WHERE player_id=? "
                        "ORDER BY game", (p["id"],)).fetchall()
    print("--- game profiles ---")
    if not profs:
        print("(none)")
    for pr in profs:
        ns = con.execute("SELECT COUNT(*) c FROM settings WHERE profile_id=?",
                         (pr["id"],)).fetchone()["c"]
        nb = con.execute("SELECT COUNT(*) c FROM binds WHERE profile_id=?",
                         (pr["id"],)).fetchone()["c"]
        print("  %-14s %3d settings, %3d binds   updated %s"
              % (pr["game"], ns, nb, pr["updated_at"]))
    deps = con.execute("SELECT ts,host,action,path FROM deployments "
                       "WHERE player_id=? ORDER BY ts DESC LIMIT 5",
                       (p["id"],)).fetchall()
    if deps:
        print("--- recent machine activity ---")
        for d in deps:
            print("  %s  %-8s %-15s %s"
                  % (d["ts"], d["action"], d["host"], d["path"]))


def cmd_set(con, a):
    p = get_player(con, a.handle)
    t = now()
    for k, v in split_kv(a.pairs):
        con.execute("INSERT INTO prefs (player_id,key,value,updated_at) "
                    "VALUES (?,?,?,?) ON CONFLICT(player_id,key) DO UPDATE "
                    "SET value=excluded.value, updated_at=excluded.updated_at",
                    (p["id"], k, v, t))
        print("%s: %s = %s" % (p["handle"], k, v))
    con.execute("UPDATE players SET updated_at=? WHERE id=?", (t, p["id"]))
    con.commit()


def cmd_games(con, a):
    for g in con.execute("SELECT * FROM gamedefs ORDER BY game").fetchall():
        print("%-12s %-6s %-6s %s" % (g["game"], g["format"],
                                      g["cvar_cmd"] or "-",
                                      g["config_path"] or "(no path set)"))


def cmd_gamedef(con, a):
    g = norm_game(a.game)
    row = get_gamedef(con, g)
    if not row:
        con.execute("INSERT INTO gamedefs (game,updated_at) VALUES (?,?)",
                    (g, now()))
        con.commit()
        row = get_gamedef(con, g)
    fields = {}
    if a.path is not None:
        fields["config_path"] = a.path
    if a.format is not None:
        fields["format"] = a.format
    if a.cmd is not None:
        fields["cvar_cmd"] = a.cmd
    if a.launch is not None:
        fields["launch"] = a.launch
    if fields:
        fields["updated_at"] = now()
        con.execute("UPDATE gamedefs SET "
                    + ",".join("%s=?" % k for k in fields) + " WHERE game=?",
                    list(fields.values()) + [g])
        con.commit()
        row = get_gamedef(con, g)
    print("%-12s %-6s %-6s %s" % (row["game"], row["format"],
                                  row["cvar_cmd"] or "-",
                                  row["config_path"] or "(no path set)"))


def cmd_game_set(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game, create=True)
    gd = get_gamedef(con, a.game)
    default_cmd = a.cmd if a.cmd is not None else (gd["cvar_cmd"] if gd else "")
    t = now()
    for k, v in split_kv(a.pairs):
        con.execute("INSERT INTO settings (profile_id,key,value,cmd,updated_at)"
                    " VALUES (?,?,?,?,?) ON CONFLICT(profile_id,key) DO UPDATE "
                    "SET value=excluded.value, cmd=excluded.cmd, "
                    "updated_at=excluded.updated_at",
                    (pr["id"], k, v, default_cmd, t))
        print("%s/%s: %s = %s" % (p["handle"], pr["game"], k, v))
    con.execute("UPDATE profiles SET updated_at=? WHERE id=?", (t, pr["id"]))
    con.commit()


def cmd_bind(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game, create=True)
    t = now()
    con.execute("INSERT INTO binds (profile_id,key,action,updated_at) "
                "VALUES (?,?,?,?) ON CONFLICT(profile_id,key) DO UPDATE "
                "SET action=excluded.action, updated_at=excluded.updated_at",
                (pr["id"], a.key, a.action, t))
    con.execute("UPDATE profiles SET updated_at=? WHERE id=?", (t, pr["id"]))
    con.commit()
    print("%s/%s: bind %s -> %s" % (p["handle"], pr["game"], a.key, a.action))


def cmd_unset(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game)
    if not pr:
        sys.exit("no profile: %s/%s" % (p["handle"], norm_game(a.game)))
    table = "binds" if a.bind else "settings"
    cur = con.execute("DELETE FROM %s WHERE profile_id=? AND key=?" % table,
                      (pr["id"], a.key))
    con.commit()
    print("removed %d %s entry for %s" % (cur.rowcount, table, a.key))


def cmd_game_show(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game)
    if not pr:
        sys.exit("no profile: %s/%s" % (p["handle"], norm_game(a.game)))
    gd = get_gamedef(con, pr["game"])
    settings, binds = profile_rows(con, pr["id"])
    print("profile:  %s / %s   (updated %s)"
          % (p["handle"], pr["game"], pr["updated_at"]))
    if gd:
        print("config:   %s  [%s]" % (gd["config_path"] or "(unset)",
                                      gd["format"]))
    if pr["notes"]:
        print("notes:    %s" % pr["notes"])
    print("--- settings (%d) ---" % len(settings))
    for k, v, c in settings:
        print("  %-28s %s" % (k, v))
    print("--- binds (%d) ---" % len(binds))
    for k, act in binds:
        print("  %-12s %s" % (k, act))
    deps = con.execute("SELECT ts,host,action FROM deployments WHERE "
                       "profile_id=? ORDER BY ts DESC LIMIT 5",
                       (pr["id"],)).fetchall()
    if deps:
        print("--- on machines ---")
        for d in deps:
            print("  %s  %-8s %s" % (d["ts"], d["action"], d["host"]))


def cmd_import(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game, create=True)
    gd = get_gamedef(con, pr["game"])
    with open(a.file, "r", encoding="latin-1") as fh:
        text = fh.read()
    settings, binds = parse_config(text, gd["format"] if gd else "quake")
    ns, nb = store_parsed(con, pr["id"], settings, binds)
    log_deployment(con, p["id"], pr["id"], a.source or "(file)", a.file,
                   "import", "%d settings, %d binds" % (ns, nb))
    print("imported %d settings and %d binds into %s/%s"
          % (ns, nb, p["handle"], pr["game"]))


def cmd_render(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game)
    if not pr:
        sys.exit("no profile: %s/%s" % (p["handle"], norm_game(a.game)))
    text = render_profile(con, pr, get_gamedef(con, pr["game"]))
    if a.out:
        with open(a.out, "w", encoding="latin-1", newline="") as fh:
            fh.write(text)
        print("wrote %s (%d bytes)" % (a.out, len(text)))
    else:
        sys.stdout.write(text.replace("\r\n", "\n"))


def resolve_path(con, a, gd):
    path = a.path or (gd["config_path"] if gd else "")
    if not path:
        sys.exit("no config path known for this game - pass --path, or set one "
                 "permanently:  gamedef %s --path 'D:\\Game\\config.cfg'"
                 % norm_game(a.game))
    return path


def cmd_capture(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game, create=True)
    gd = get_gamedef(con, pr["game"])
    path = resolve_path(con, a, gd)
    text = agent_read_file(a.host, path)
    if text is None:
        sys.exit("could not read %s on %s (file missing, or wrong path for "
                 "this box - dual-boot boxes keep games on D:)" % (path, a.host))
    settings, binds = parse_config(text, gd["format"] if gd else "quake")
    ns, nb = store_parsed(con, pr["id"], settings, binds)
    log_deployment(con, p["id"], pr["id"], a.host, path, "capture",
                   "%d settings, %d binds" % (ns, nb), backup=text)
    print("captured %d settings and %d binds from %s:%s into %s/%s"
          % (ns, nb, a.host, path, p["handle"], pr["game"]))


def cmd_apply(con, a):
    p = get_player(con, a.handle)
    pr = get_profile(con, p["id"], a.game)
    if not pr:
        sys.exit("no profile: %s/%s" % (p["handle"], norm_game(a.game)))
    gd = get_gamedef(con, pr["game"])
    path = resolve_path(con, a, gd)
    fmt = gd["format"] if gd else "quake"
    settings, binds = profile_rows(con, pr["id"])
    if not settings and not binds:
        sys.exit("profile %s/%s is empty - nothing to apply"
                 % (p["handle"], pr["game"]))
    if a.dry_run:
        # Do not touch the box at all on a dry run, not even to read.
        text = render_profile(con, pr, gd)
        print("--- would write to %s:%s ---" % (a.host, path))
        sys.stdout.write(text.replace("\r\n", "\n"))
        return
    existing = agent_read_file(a.host, path)
    if fmt == "ini":
        if existing is None:
            sys.exit("%s has no %s - an ini profile is a PATCH and needs the "
                     "game's own ini to merge into. Install the game first."
                     % (a.host, path))
        text = apply_ini_patch(existing, settings)
    else:
        text = render_profile(con, pr, gd)
    agent_write_file(a.host, path, text)
    log_deployment(con, p["id"], pr["id"], a.host, path, "apply",
                   "%d settings, %d binds" % (len(settings), len(binds)),
                   backup=existing or "")
    print("applied %s/%s to %s:%s  (%d settings, %d binds)"
          % (p["handle"], pr["game"], a.host, path, len(settings), len(binds)))
    if existing is not None:
        print("previous file archived in the deployment log "
              "(restore with: history --host %s)" % a.host)


def cmd_history(con, a):
    q = ("SELECT d.*, p.handle, pr.game FROM deployments d "
         "LEFT JOIN players p ON p.id=d.player_id "
         "LEFT JOIN profiles pr ON pr.id=d.profile_id WHERE 1=1")
    args = []
    if a.handle:
        q += " AND p.handle=?"
        args.append(a.handle.lower())
    if a.host:
        q += " AND d.host=?"
        args.append(a.host)
    q += " ORDER BY d.ts DESC LIMIT ?"
    args.append(a.n)
    rows = con.execute(q, args).fetchall()
    if not rows:
        print("no deployment history")
        return
    for r in rows:
        print("#%-4d %s  %-8s %-15s %s/%s"
              % (r["id"], r["ts"], r["action"], r["host"],
                 r["handle"] or "?", r["game"] or "?"))
        print("      %s  %s" % (r["path"], r["note"]))


def cmd_restore(con, a):
    """Write a deployment's archived file back to the box it came from."""
    r = con.execute("SELECT * FROM deployments WHERE id=?", (a.id,)).fetchone()
    if not r:
        sys.exit("no such deployment: %s" % a.id)
    if not r["backup"]:
        sys.exit("deployment #%d has no archived file to restore" % r["id"])
    host = a.host or r["host"]
    agent_write_file(host, r["path"], r["backup"])
    log_deployment(con, r["player_id"], r["profile_id"], host, r["path"],
                   "restore", "from deployment #%d" % r["id"])
    print("restored the file archived at deployment #%d to %s:%s"
          % (r["id"], host, r["path"]))


# ---- CLI ----

def build_parser():
    ap = argparse.ArgumentParser(
        prog="retro_playerprofile.py",
        description="Player profiles and per-game configurations for the "
                    "retro fleet.")
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("create", help="create a player")
    p.add_argument("handle")
    p.add_argument("--name", help="display / real name")
    p.add_argument("--notes")
    p.set_defaults(fn=cmd_create)

    p = sub.add_parser("list", help="list players")
    p.set_defaults(fn=cmd_list)

    p = sub.add_parser("show", help="show a player, prefs and profiles")
    p.add_argument("handle")
    p.set_defaults(fn=cmd_show)

    p = sub.add_parser("set", help="set cross-game preferences")
    p.add_argument("handle")
    p.add_argument("pairs", nargs="+", metavar="key=value")
    p.set_defaults(fn=cmd_set)

    p = sub.add_parser("games", help="list known game definitions")
    p.set_defaults(fn=cmd_games)

    p = sub.add_parser("gamedef", help="add/edit where a game's config lives")
    p.add_argument("game")
    p.add_argument("--path", help="config file path ON THE BOX")
    p.add_argument("--format", choices=["quake", "ini"])
    p.add_argument("--cmd", help="cvar command word: seta, set, or '' ")
    p.add_argument("--launch", help="game exe path, for reference")
    p.set_defaults(fn=cmd_gamedef)

    p = sub.add_parser("game-set", help="set per-game settings")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("pairs", nargs="+", metavar="key=value")
    p.add_argument("--cmd", help="override the cvar command word")
    p.set_defaults(fn=cmd_game_set)

    p = sub.add_parser("bind", help="set a key bind")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("key")
    p.add_argument("action")
    p.set_defaults(fn=cmd_bind)

    p = sub.add_parser("unset", help="remove a setting (or --bind, a bind)")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("key")
    p.add_argument("--bind", action="store_true")
    p.set_defaults(fn=cmd_unset)

    p = sub.add_parser("game-show", help="show one game profile in full")
    p.add_argument("handle")
    p.add_argument("game")
    p.set_defaults(fn=cmd_game_show)

    p = sub.add_parser("import", help="import a config file from disk")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("--file", required=True)
    p.add_argument("--source", help="note where it came from")
    p.set_defaults(fn=cmd_import)

    p = sub.add_parser("render", help="render the profile as a config file")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("--out")
    p.set_defaults(fn=cmd_render)

    p = sub.add_parser("capture", help="pull a game's config off a fleet box")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("--host", required=True)
    p.add_argument("--path")
    p.set_defaults(fn=cmd_capture)

    p = sub.add_parser("apply", help="push a profile onto a fleet box")
    p.add_argument("handle")
    p.add_argument("game")
    p.add_argument("--host", required=True)
    p.add_argument("--path")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_apply)

    p = sub.add_parser("history", help="capture/apply audit trail")
    p.add_argument("--handle")
    p.add_argument("--host")
    p.add_argument("-n", type=int, default=20)
    p.set_defaults(fn=cmd_history)

    p = sub.add_parser("restore", help="put an archived config back on a box")
    p.add_argument("id", type=int, help="deployment id from `history`")
    p.add_argument("--host", help="restore to a different box")
    p.set_defaults(fn=cmd_restore)

    return ap


def main(argv=None):
    ap = build_parser()
    a = ap.parse_args(argv)
    if not getattr(a, "fn", None):
        ap.print_help()
        return 1
    con = connect()
    try:
        a.fn(con, a)
    finally:
        con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
