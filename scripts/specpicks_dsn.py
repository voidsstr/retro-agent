"""Resolve the specpicks Postgres DSN without hardcoding the credential.

Order: SPECPICKS_DATABASE_URL env var, then ~/.specpicks_dsn (0600), then
~/.config/specpicks/db_url. Raises if none is found — the password is no
longer committed to the repo, so set one of these. See MAINTENANCE.md.
"""
import os
import pathlib


def resolve_dsn() -> str:
    v = os.environ.get("SPECPICKS_DATABASE_URL")
    if v:
        return v.strip()
    for p in (pathlib.Path.home() / ".specpicks_dsn",
              pathlib.Path.home() / ".config" / "specpicks" / "db_url"):
        try:
            t = p.read_text().strip()
            if t:
                return t
        except OSError:
            pass
    raise SystemExit(
        "specpicks DSN not found. The DB credential is no longer hardcoded.\n"
        "Set it one of these ways:\n"
        "  export SPECPICKS_DATABASE_URL='postgresql://...'\n"
        "  echo 'postgresql://...' > ~/.specpicks_dsn && chmod 600 ~/.specpicks_dsn")
