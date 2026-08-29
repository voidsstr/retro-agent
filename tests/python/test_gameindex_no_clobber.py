"""The favourites agent must never destroy settings it did not write.

Reported from the fleet on 2026-08-29 by another session running a staged-games
pass: on .143 the service appended its block after their `r_fullscreen`/`r_mode`
and lost nothing; on .240 the same settings were **gone**. Same code, same
file, opposite outcomes — the hardest possible shape to debug from outside.

The cause was not the merge. It was the READ:

    existing = ""
    try:
        existing = await c.command_text(f'EXEC cmd /c type "{path}"')
        if "cannot find" in existing.lower(): existing = ""
    except Exception:
        existing = ""

Three separate failures all collapsed into "the file is empty", after which the
merge faithfully wrote only our block:

  * any exception — a timeout, a busy box, a dropped connection;
  * matching English error prose against the file's OWN CONTENT to decide
    whether it exists;
  * a shell round trip that can truncate or mangle what it returns.

"The file is not there" and "I could not read the file" mean opposite things —
one is safe to create, the other must not be written — and collapsing them is
what made the damage intermittent and invisible.

Run: pytest tests/python/test_gameindex_no_clobber.py
"""

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_GI = Path(__file__).resolve().parent.parent.parent / "scripts" / "gameindex"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, _GI / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


favorites = _load("favorites")

FOREIGN = """// staged by library-warden
seta r_fullscreen "1"
seta r_mode "4"
bind F11 "screenshot"
seta com_maxfps "125"
"""

SERVERS = [{"addr": "192.168.1.132:27961", "hostname": "NSC Arena"}]


# --- the merge keeps what is not ours ------------------------------------

def test_foreign_settings_survive_the_merge():
    text, _h = favorites.render("q3", SERVERS, FOREIGN)
    for line in ('seta r_fullscreen "1"', 'seta r_mode "4"',
                 'bind F11 "screenshot"', 'seta com_maxfps "125"'):
        assert line in text, f"{line} was lost"


def test_our_own_block_is_replaced_not_duplicated():
    once, _ = favorites.render("q3", SERVERS, FOREIGN)
    twice, _ = favorites.render("q3", SERVERS, once)
    assert twice.count(favorites.BEGIN) == 1
    assert twice.count('seta r_fullscreen "1"') == 1


def test_stale_favourite_lines_outside_our_block_are_still_stripped():
    """A `seta server3` left by an older run must go, or it haunts the in-game
    list forever — that is the one thing we DO remove."""
    text, _ = favorites.render(
        "q3", SERVERS, FOREIGN + '\nseta server3 "10.0.0.9:27960"\n')
    assert "10.0.0.9" not in text
    assert 'seta r_mode "4"' in text


def test_a_lost_line_raises_rather_than_being_written(monkeypatch):
    """Belt and braces: if the strip rule ever grows too greedy, render must
    refuse rather than hand the caller a file that drops someone's settings."""
    def greedy(existing, seta_re):
        return ""          # pretend the strip ate everything
    monkeypatch.setattr(favorites, "_strip_block", greedy)
    with pytest.raises(favorites.WouldClobber) as e:
        favorites.render("q3", SERVERS, FOREIGN)
    assert "r_fullscreen" in str(e.value) or "drop" in str(e.value)


def test_empty_existing_is_fine():
    """A genuinely new file is not a clobber."""
    text, _ = favorites.render("q3", SERVERS, "")
    assert favorites.BEGIN in text


def test_q2_merge_keeps_foreign_lines_too():
    q2_foreign = 'set vid_fullscreen "1"\nset cl_maxfps "60"\n'
    text, _ = favorites.render("q2", [{"addr": "1.2.3.4:27910",
                                       "hostname": "x"}], q2_foreign)
    assert 'set vid_fullscreen "1"' in text
    assert 'set cl_maxfps "60"' in text


# --- the read must distinguish missing from unreadable -------------------

class _Conn:
    """A stand-in agent connection with scripted outcomes."""

    def __init__(self, download=None, dirlist=None):
        self._download = download
        self._dirlist = dirlist

    async def command_binary(self, cmd, timeout=60):
        if isinstance(self._download, Exception):
            raise self._download
        return self._download

    async def command_text(self, cmd, timeout=30):
        if isinstance(self._dirlist, Exception):
            raise self._dirlist
        return self._dirlist


def _read(conn, path=r"C:\Games\Q3\baseq3\autoexec.cfg"):
    import asyncio
    sync = _load("sync")
    return asyncio.run(sync.read_existing(conn, path))


def test_a_successful_read_returns_the_bytes():
    text, state, _why = _read(_Conn(download=FOREIGN.encode()))
    assert state == "read"
    assert 'r_fullscreen' in text


def test_a_timeout_is_unreadable_not_empty():
    """THE bug. A timeout used to become `existing = ""`, and the next line
    wrote a file containing only our block."""
    conn = _Conn(download=TimeoutError("timed out"),
                 dirlist=json.dumps([{"name": "autoexec.cfg"}]))
    text, state, why = _read(conn)
    assert state == "unreadable"
    assert text == ""
    assert "TimeoutError" in why


def test_a_file_that_exists_but_cannot_be_read_is_never_written():
    conn = _Conn(download=OSError("busy"),
                 dirlist=json.dumps([{"name": "autoexec.cfg"},
                                     {"name": "pak0.pk3"}]))
    _t, state, why = _read(conn)
    assert state == "unreadable"
    assert "exists" in why


def test_a_genuinely_missing_file_is_safe_to_create():
    conn = _Conn(download=OSError("not found"),
                 dirlist=json.dumps([{"name": "pak0.pk3"}]))
    text, state, _why = _read(conn)
    assert state == "missing"
    assert text == ""


def test_an_unparseable_listing_is_not_evidence_of_absence():
    conn = _Conn(download=OSError("nope"), dirlist="<html>error</html>")
    _t, state, why = _read(conn)
    assert state == "unreadable"
    assert "unparseable" in why


def test_a_failed_listing_is_also_unreadable():
    conn = _Conn(download=OSError("nope"), dirlist=OSError("no dir"))
    _t, state, _why = _read(conn)
    assert state == "unreadable"


def test_existence_is_case_insensitive():
    """Windows paths are case-insensitive; AUTOEXEC.CFG is the same file."""
    conn = _Conn(download=OSError("x"),
                 dirlist=json.dumps([{"name": "AUTOEXEC.CFG"}]))
    _t, state, _why = _read(conn)
    assert state == "unreadable", "an existing file was treated as missing"


# --- the caller must honour it -------------------------------------------

def test_push_favorites_refuses_to_write_when_unreadable():
    src = (_GI / "sync.py").read_text()
    block = src[src.index("existing, state, why = await read_existing"):]
    head = block[:1200]
    assert 'state == "unreadable"' in head
    assert "continue" in head, "an unreadable file must skip, not fall through"


def test_the_old_destructive_read_is_gone():
    """Check CODE, not comments -- the docstring explaining the bug quotes
    the old call, and a test that trips on its own documentation is worse
    than no test."""
    marks = ("#", "*", chr(34) * 3, chr(39) * 3)
    code = "\n".join(ln for ln in (_GI / "sync.py").read_text().splitlines()
                     if not ln.lstrip().startswith(marks))
    assert "command_text(f'EXEC cmd /c type" not in code, "shell read is back"
    assert 'if "cannot find" in existing' not in code, \
        "deciding file existence from the file's own content is back"

