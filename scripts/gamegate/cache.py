"""Verdict cache - SQLite at ~/.retro-fleet/gamegate.db.

WHY IT IS KEYED THE WAY IT IS

    (profile_hash, title, shortcut, requirements_version, model)

* **profile_hash** rather than an IP or a hostname. Two boxes built the same
  share one entry, a re-imaged box keeps its verdicts, and a box that gets a new
  graphics card correctly loses them. The agent computes it from HARDWARE fields
  only (gg_profile_hash in agent/shared/gamegate.h), with the clock bucketed to
  25 MHz and RAM to 16 MB, because a measured clock wobbles a few MHz between
  polls and a hash that moved on that would miss on every single lookup - which
  is the same as having no cache.
* **requirements_version** so correcting a title's numbers invalidates its
  cached verdicts rather than letting them outlive the correction.
* **model** because a verdict is only as good as who gave it. Swapping models
  must not silently inherit the previous one's opinions.

AND WHY `decided_by` IS STORED

A rule verdict is reproducible arithmetic; an LLM verdict is an opinion that may
want revisiting. Recording which is which is what lets `--refresh-llm` drop
every model opinion and keep every rule result, and it is what makes the cache
auditable at all - without it, "why is Doom 3 not on that box" has no answer.
"""

from __future__ import annotations

import os
import sqlite3
import time
from pathlib import Path

DEFAULT_DB = Path(os.path.expanduser("~/.retro-fleet/gamegate.db"))

SCHEMA = """
CREATE TABLE IF NOT EXISTS verdicts (
    profile_hash  TEXT NOT NULL,
    title         TEXT NOT NULL,
    shortcut      TEXT NOT NULL DEFAULT '',
    req_version   INTEGER NOT NULL DEFAULT 0,
    model         TEXT NOT NULL DEFAULT '',
    verdict       TEXT NOT NULL,
    limiting      TEXT NOT NULL DEFAULT '',
    reason        TEXT NOT NULL DEFAULT '',
    missing_caps  INTEGER NOT NULL DEFAULT 0,
    decided_by    TEXT NOT NULL,
    confidence    REAL NOT NULL DEFAULT 1.0,
    created       INTEGER NOT NULL,
    PRIMARY KEY (profile_hash, title, shortcut, req_version, model)
);
CREATE TABLE IF NOT EXISTS profiles (
    profile_hash  TEXT PRIMARY KEY,
    hostname      TEXT NOT NULL DEFAULT '',
    ip            TEXT NOT NULL DEFAULT '',
    summary       TEXT NOT NULL DEFAULT '',
    raw           TEXT NOT NULL DEFAULT '',
    seen          INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_verdict_title ON verdicts(title);
CREATE INDEX IF NOT EXISTS idx_verdict_by ON verdicts(decided_by);
"""


class Cache:
    def __init__(self, path=None):
        self.path = Path(path) if path else DEFAULT_DB
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(self.path))
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.commit()
        self.hits = 0
        self.misses = 0

    # -- verdicts ---------------------------------------------------------
    def get(self, profile_hash, title, shortcut, req_version, model):
        """A RULE verdict is model-independent, so it is looked up under the
        empty model first: changing models must not throw away arithmetic."""
        cur = self.db.execute(
            "SELECT * FROM verdicts WHERE profile_hash=? AND title=? AND "
            "shortcut=? AND req_version=? AND model IN ('', ?)"
            " ORDER BY CASE WHEN model='' THEN 0 ELSE 1 END",
            (profile_hash, title, shortcut, req_version, model))
        row = cur.fetchone()
        if row is None:
            self.misses += 1
            return None
        self.hits += 1
        return row

    def put(self, profile_hash, title, shortcut, req_version, model, decision):
        # Rule verdicts are stored under the empty model - they do not belong
        # to one. Only an LLM verdict is attributed.
        key_model = "" if decision.decided_by == "rule" else model
        self.db.execute(
            "INSERT OR REPLACE INTO verdicts (profile_hash, title, shortcut, "
            "req_version, model, verdict, limiting, reason, missing_caps, "
            "decided_by, confidence, created) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (profile_hash, title, shortcut, req_version, key_model,
             decision.name, decision.limiting, decision.reason,
             decision.missing_caps, decision.decided_by, decision.confidence,
             int(time.time())))
        self.db.commit()

    def forget(self, profile_hash=None, title=None, only_llm=False):
        """Drop cached verdicts. `only_llm` keeps every rule result, which is
        the point of storing decided_by: a model swap or a prompt change should
        not throw away reproducible arithmetic."""
        sql = "DELETE FROM verdicts WHERE 1=1"
        args = []
        if profile_hash:
            sql += " AND profile_hash=?"
            args.append(profile_hash)
        if title:
            sql += " AND title=?"
            args.append(title)
        if only_llm:
            sql += " AND decided_by='llm'"
        cur = self.db.execute(sql, args)
        self.db.commit()
        return cur.rowcount

    # -- profiles ---------------------------------------------------------
    def remember_profile(self, profile, raw_json):
        self.db.execute(
            "INSERT OR REPLACE INTO profiles (profile_hash, hostname, ip, "
            "summary, raw, seen) VALUES (?,?,?,?,?,?)",
            (profile.profile_hash, profile.hostname, profile.ip,
             profile.describe(), raw_json, int(time.time())))
        self.db.commit()

    def profiles(self):
        return list(self.db.execute(
            "SELECT * FROM profiles ORDER BY seen DESC"))

    def stats(self):
        row = self.db.execute(
            "SELECT COUNT(*) n, SUM(decided_by='llm') llm, "
            "SUM(decided_by='rule') rule FROM verdicts").fetchone()
        return {"entries": row["n"] or 0, "llm": row["llm"] or 0,
                "rule": row["rule"] or 0,
                "hits": self.hits, "misses": self.misses}

    def close(self):
        self.db.close()
