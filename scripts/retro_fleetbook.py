#!/usr/bin/env python3
"""retro_fleetbook.py — the fleet's persistent memory of solved problems.

A plain-SQLite knowledge base (default ~/.retro-fleet/fleetbook.db) holding:

  recipes  — reusable fixes: the problem, its symptoms, and the exact steps
             (the "recipe") that solved it, with tags and usage counts.
  changes  — a per-machine change log: what was changed on which computer,
             when, and (optionally) which recipe was applied.

The retro chat brain is instructed to SEARCH this before diagnosing anything
("have we solved this before?") and to RECORD a change (+recipe, when the fix
is reusable) after completing work on a box. Humans use the same CLI.

Usage:
  retro_fleetbook.py search <query...>            # FTS over recipes
  retro_fleetbook.py show <id|slug>               # full recipe + where applied
  retro_fleetbook.py add --title T --problem P --recipe R
                         [--symptoms S] [--tags a,b] [--source SRC]
  retro_fleetbook.py log --host IP --summary S [--detail D] [--recipe id|slug]
  retro_fleetbook.py history [--host IP] [-n N]   # recent changes
  retro_fleetbook.py stats

`log --recipe X` also bumps X's usage counters, so "applied recipe on box"
is a single call. All writes are safe under concurrent callers (WAL mode).

DB override for tests: RETRO_FLEETBOOK_DB=/path/to.db
"""
import argparse
import datetime
import os
import re
import sqlite3
import sys

DB_PATH = os.environ.get(
    "RETRO_FLEETBOOK_DB",
    os.path.join(os.path.expanduser("~"), ".retro-fleet", "fleetbook.db"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS recipes (
    id          INTEGER PRIMARY KEY,
    slug        TEXT UNIQUE NOT NULL,
    title       TEXT NOT NULL,
    problem     TEXT NOT NULL,
    symptoms    TEXT DEFAULT '',
    recipe      TEXT NOT NULL,
    tags        TEXT DEFAULT '',
    source      TEXT DEFAULT '',
    created_at  TEXT NOT NULL,
    updated_at  TEXT NOT NULL,
    times_used  INTEGER DEFAULT 0,
    last_used_at TEXT
);
CREATE TABLE IF NOT EXISTS changes (
    id          INTEGER PRIMARY KEY,
    ts          TEXT NOT NULL,
    host        TEXT NOT NULL,
    summary     TEXT NOT NULL,
    detail      TEXT DEFAULT '',
    recipe_id   INTEGER REFERENCES recipes(id)
);
CREATE INDEX IF NOT EXISTS idx_changes_host ON changes(host);
CREATE VIRTUAL TABLE IF NOT EXISTS recipes_fts USING fts5(
    title, problem, symptoms, recipe, tags,
    content='recipes', content_rowid='id');
CREATE TRIGGER IF NOT EXISTS recipes_ai AFTER INSERT ON recipes BEGIN
    INSERT INTO recipes_fts(rowid, title, problem, symptoms, recipe, tags)
    VALUES (new.id, new.title, new.problem, new.symptoms, new.recipe, new.tags);
END;
CREATE TRIGGER IF NOT EXISTS recipes_ad AFTER DELETE ON recipes BEGIN
    INSERT INTO recipes_fts(recipes_fts, rowid, title, problem, symptoms, recipe, tags)
    VALUES ('delete', old.id, old.title, old.problem, old.symptoms, old.recipe, old.tags);
END;
CREATE TRIGGER IF NOT EXISTS recipes_au AFTER UPDATE ON recipes BEGIN
    INSERT INTO recipes_fts(recipes_fts, rowid, title, problem, symptoms, recipe, tags)
    VALUES ('delete', old.id, old.title, old.problem, old.symptoms, old.recipe, old.tags);
    INSERT INTO recipes_fts(rowid, title, problem, symptoms, recipe, tags)
    VALUES (new.id, new.title, new.problem, new.symptoms, new.recipe, new.tags);
END;
"""


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def connect():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    con = sqlite3.connect(DB_PATH, timeout=15)
    con.execute("PRAGMA journal_mode=WAL")
    con.executescript(SCHEMA)
    con.row_factory = sqlite3.Row
    return con


def slugify(title):
    s = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
    return s[:60] or "recipe"


def resolve_recipe(con, ref):
    """ref = numeric id or slug; returns row or None."""
    if str(ref).isdigit():
        r = con.execute("SELECT * FROM recipes WHERE id=?", (int(ref),)).fetchone()
        if r:
            return r
    return con.execute("SELECT * FROM recipes WHERE slug=?", (str(ref),)).fetchone()


# ---- commands ----

def cmd_add(con, a):
    slug = a.slug or slugify(a.title)
    base, n = slug, 2
    while con.execute("SELECT 1 FROM recipes WHERE slug=?", (slug,)).fetchone():
        slug = "%s-%d" % (base, n)
        n += 1
    t = now()
    cur = con.execute(
        "INSERT INTO recipes (slug,title,problem,symptoms,recipe,tags,source,"
        "created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
        (slug, a.title, a.problem, a.symptoms or "", a.recipe,
         a.tags or "", a.source or "", t, t))
    con.commit()
    print("added recipe #%d  %s" % (cur.lastrowid, slug))


def cmd_update(con, a):
    r = resolve_recipe(con, a.ref)
    if not r:
        sys.exit("no such recipe: %s" % a.ref)
    fields = {}
    for f in ("title", "problem", "symptoms", "recipe", "tags", "source"):
        v = getattr(a, f, None)
        if v is not None:
            fields[f] = v
    if not fields:
        sys.exit("nothing to update (pass --title/--problem/--recipe/...)")
    fields["updated_at"] = now()
    con.execute("UPDATE recipes SET " + ",".join("%s=?" % k for k in fields)
                + " WHERE id=?", list(fields.values()) + [r["id"]])
    con.commit()
    print("updated recipe #%d  %s" % (r["id"], r["slug"]))


def fts_query(q):
    """Turn free text into a safe FTS5 query (AND of quoted tokens)."""
    toks = re.findall(r"[A-Za-z0-9_.\\-]+", q)
    return " ".join('"%s"' % t for t in toks) if toks else '""'


def cmd_search(con, a):
    q = " ".join(a.query)
    rows = con.execute(
        "SELECT r.*, bm25(recipes_fts) AS rank FROM recipes_fts "
        "JOIN recipes r ON r.id = recipes_fts.rowid "
        "WHERE recipes_fts MATCH ? ORDER BY rank LIMIT ?",
        (fts_query(q), a.limit)).fetchall()
    if not rows:
        # fallback: any-token OR match, then LIKE
        toks = re.findall(r"[A-Za-z0-9_.\\-]+", q)
        if toks:
            rows = con.execute(
                "SELECT r.*, bm25(recipes_fts) AS rank FROM recipes_fts "
                "JOIN recipes r ON r.id = recipes_fts.rowid "
                "WHERE recipes_fts MATCH ? ORDER BY rank LIMIT ?",
                (" OR ".join('"%s"' % t for t in toks), a.limit)).fetchall()
    if not rows:
        print("no recipes match: %s" % q)
        return
    for r in rows:
        print("#%-4d %-45s used %dx  [%s]" %
              (r["id"], r["slug"], r["times_used"], r["tags"]))
        print("      %s" % r["title"])


def cmd_show(con, a):
    r = resolve_recipe(con, a.ref)
    if not r:
        sys.exit("no such recipe: %s" % a.ref)
    print("recipe #%d  %s  (used %dx, last %s)" %
          (r["id"], r["slug"], r["times_used"], r["last_used_at"] or "never"))
    print("title:    %s" % r["title"])
    print("tags:     %s" % r["tags"])
    if r["source"]:
        print("source:   %s" % r["source"])
    print("problem:  %s" % r["problem"])
    if r["symptoms"]:
        print("symptoms: %s" % r["symptoms"])
    print("--- recipe ---")
    print(r["recipe"])
    apps = con.execute(
        "SELECT ts, host, summary FROM changes WHERE recipe_id=? "
        "ORDER BY ts DESC LIMIT 10", (r["id"],)).fetchall()
    if apps:
        print("--- applied on ---")
        for c in apps:
            print("%s  %-15s %s" % (c["ts"], c["host"], c["summary"]))


def cmd_log(con, a):
    rid = None
    if a.recipe:
        r = resolve_recipe(con, a.recipe)
        if not r:
            sys.exit("no such recipe: %s" % a.recipe)
        rid = r["id"]
        con.execute("UPDATE recipes SET times_used=times_used+1, last_used_at=? "
                    "WHERE id=?", (now(), rid))
    cur = con.execute(
        "INSERT INTO changes (ts,host,summary,detail,recipe_id) VALUES (?,?,?,?,?)",
        (now(), a.host, a.summary, a.detail or "", rid))
    con.commit()
    print("logged change #%d on %s%s" %
          (cur.lastrowid, a.host, (" (recipe #%d)" % rid) if rid else ""))


def cmd_history(con, a):
    q = ("SELECT c.*, r.slug AS rslug FROM changes c "
         "LEFT JOIN recipes r ON r.id = c.recipe_id ")
    args = []
    if a.host:
        q += "WHERE c.host=? "
        args.append(a.host)
    q += "ORDER BY c.ts DESC LIMIT ?"
    args.append(a.n)
    rows = con.execute(q, args).fetchall()
    if not rows:
        print("no changes logged%s" % ((" for " + a.host) if a.host else ""))
        return
    for c in rows:
        ref = ("  [%s]" % c["rslug"]) if c["rslug"] else ""
        print("%s  %-15s %s%s" % (c["ts"], c["host"], c["summary"], ref))
        if a.verbose and c["detail"]:
            for ln in c["detail"].splitlines():
                print("    %s" % ln)


def cmd_stats(con, a):
    nr = con.execute("SELECT COUNT(*) c FROM recipes").fetchone()["c"]
    nc = con.execute("SELECT COUNT(*) c FROM changes").fetchone()["c"]
    hosts = con.execute("SELECT COUNT(DISTINCT host) c FROM changes").fetchone()["c"]
    print("%d recipes, %d changes across %d hosts  (%s)" % (nr, nc, hosts, DB_PATH))
    top = con.execute("SELECT slug, times_used FROM recipes "
                      "WHERE times_used > 0 ORDER BY times_used DESC LIMIT 5").fetchall()
    for r in top:
        print("  %dx %s" % (r["times_used"], r["slug"]))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("add", help="add a reusable fix recipe")
    s.add_argument("--title", required=True)
    s.add_argument("--problem", required=True)
    s.add_argument("--symptoms")
    s.add_argument("--recipe", required=True, help="the steps that fix it")
    s.add_argument("--tags", help="comma-separated")
    s.add_argument("--source", help="where this was learned (doc, session, box)")
    s.add_argument("--slug")

    s = sub.add_parser("update", help="update fields of an existing recipe")
    s.add_argument("ref")
    s.add_argument("--title"); s.add_argument("--problem")
    s.add_argument("--symptoms"); s.add_argument("--recipe")
    s.add_argument("--tags"); s.add_argument("--source")

    s = sub.add_parser("search", help="full-text search recipes")
    s.add_argument("query", nargs="+")
    s.add_argument("--limit", type=int, default=8)

    s = sub.add_parser("show", help="show one recipe (+ where it was applied)")
    s.add_argument("ref")

    s = sub.add_parser("log", help="log a change made on a computer")
    s.add_argument("--host", required=True)
    s.add_argument("--summary", required=True)
    s.add_argument("--detail")
    s.add_argument("--recipe", help="recipe id/slug that was applied (bumps usage)")

    s = sub.add_parser("history", help="recent changes (optionally one host)")
    s.add_argument("--host")
    s.add_argument("-n", type=int, default=20)
    s.add_argument("-v", "--verbose", action="store_true")

    sub.add_parser("stats", help="counts + most-used recipes")

    a = p.parse_args(argv)
    con = connect()
    try:
        {"add": cmd_add, "update": cmd_update, "search": cmd_search,
         "show": cmd_show, "log": cmd_log, "history": cmd_history,
         "stats": cmd_stats}[a.cmd](con, a)
    finally:
        con.close()


if __name__ == "__main__":
    main()
