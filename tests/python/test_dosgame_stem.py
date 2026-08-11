"""The DOS game manager's install-directory stem.

The stem is computed independently in two places that MUST agree byte for
byte, because it is the name the DOS client asks the server for:

  * scripts/dosgames/dosgame.c   zip_stem()          (16-bit real mode)
  * scripts/dosgames/serve_dosgames.py  zip_stem()   (resolves /z/<STEM>)

These tests pin the Python side's contract and, crucially, the property that
motivated the change: a plain 8-character truncation is NOT a unique directory
name.  On the real 2,982-entry catalogue it collided for 1,268 rows — all
eleven Duke Nukem titles installed into C:\\GAMES\\DUKE_NUK, unzipping over
each other.  Each test asserts both the fixed value and the old buggy one.

The C side is diffed against this implementation over real catalogue rows by
scripts/dosgames/tests/run_dos_tests.sh (needs Open Watcom + DOSBox).
"""
import collections
import importlib.util
import os
import pathlib

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
SERVER = REPO / "scripts" / "dosgames" / "serve_dosgames.py"
CATALOG = REPO / "scripts" / "dosgames" / "data" / "GAMES.CAT"


def _load_server():
    spec = importlib.util.spec_from_file_location("serve_dosgames", SERVER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


srv = _load_server()


def old_stem(zipname):
    """v0.1's stem: first 12 chars, extension stripped, truncated to 8."""
    s = zipname[:12]
    dot = s.rfind(".")
    if dot >= 0:
        s = s[:dot]
    s = s[:8]
    return "".join("_" if c in " ." else c for c in s)


def catalog_rows():
    if not CATALOG.exists():
        pytest.skip("GAMES.CAT not present")
    rows = []
    with open(CATALOG, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) >= 6:
                rows.append(parts)
    return rows


def test_stem_shape():
    """Always exactly 8 characters, valid as a DOS 8.3 directory name."""
    for name in ["DOOM.ZIP",
                 "Duke Nukem 3D (1996)(3D Realms).zip",
                 "a.zip",
                 "Leisure Suit Larry III Passionate Patti (1989).zip"]:
        stem = srv.zip_stem(name)
        assert len(stem) == 8, (name, stem)
        assert " " not in stem and "." not in stem, (name, stem)
        assert stem == stem.upper(), (name, stem)


def test_stem_is_deterministic():
    a = srv.zip_stem("Duke Nukem 3D (1996)(3D Realms).zip")
    b = srv.zip_stem("Duke Nukem 3D (1996)(3D Realms).zip")
    assert a == b


def test_short_names_are_padded_not_truncated():
    """A 1-char name still yields 5 readable + 3 hash characters."""
    stem = srv.zip_stem("a.zip")
    assert stem[:5] == "A____", stem


def test_no_collisions_across_the_real_catalog():
    """The fix. The old scheme collided for ~43% of the catalogue."""
    rows = catalog_rows()
    zips = [r[1] for r in rows]

    new = collections.Counter(srv.zip_stem(z) for z in zips)
    new_collisions = sum(v for v in new.values() if v > 1)
    assert new_collisions == 0, (
        "%d catalogue rows share an install directory" % new_collisions)

    # ...and prove the old scheme really was broken, so this test fails loudly
    # if someone reverts to a plain truncation.
    old = collections.Counter(old_stem(z).upper() for z in zips)
    old_collisions = sum(v for v in old.values() if v > 1)
    assert old_collisions > 1000, (
        "expected the v0.1 truncation to collide badly, got %d" % old_collisions)


def test_duke_nukem_titles_no_longer_share_a_directory():
    """The concrete case: eleven Duke titles, one directory, in v0.1."""
    rows = catalog_rows()
    duke = [r[1] for r in rows if r[1].upper().startswith("DUKE NUKEM")]
    if len(duke) < 2:
        pytest.skip("catalogue has no Duke Nukem titles")

    assert len({old_stem(z).upper() for z in duke}) == 1, \
        "v0.1 put every Duke Nukem title in one directory - premise of the fix"
    assert len({srv.zip_stem(z) for z in duke}) == len(duke), \
        "each Duke Nukem title must now get its own install directory"


def test_generated_fetch_line_fits_the_dos_command_tail():
    """DOS silently truncates a command tail at 126 bytes.

    v0.1 pasted the URL-encoded zip name onto the URL; 845 of 2,982 rows
    overflowed and 404'd, reported to the user as a network failure.
    """
    DOS_TAIL_MAX = 126
    home = r"C:\DOSGAME"
    url = "http://192.168.1.122:8181/dos"
    rows = catalog_rows()

    worst = 0
    for r in rows:
        stem = srv.zip_stem(r[1])
        tail = r"-o %s\%s.ZIP %s/z/%s" % (home, stem, url, stem)
        worst = max(worst, len(tail))
    assert worst <= DOS_TAIL_MAX, \
        "fetch command tail reaches %d bytes, over the %d-byte DOS limit" % (
            worst, DOS_TAIL_MAX)

    # the old form, for contrast: it must be shown to overflow
    def urlencode(s):
        out = []
        for ch in s:
            out.append(ch if (ch.isalnum() and ord(ch) < 128) or ch in "-_.~"
                       else "%%%02X" % ord(ch))
        return "".join(out)

    overflowed = 0
    for r in rows:
        stem = old_stem(r[1])
        tail = r"-o %s\%s.ZIP %s/%s" % (home, stem, url, urlencode(r[1]))
        if len(tail) > DOS_TAIL_MAX:
            overflowed += 1
    assert overflowed > 500, \
        "expected the v0.1 long-URL form to overflow for many rows, got %d" % \
        overflowed


def test_server_resolves_a_stem_to_the_right_file(tmp_path, monkeypatch):
    """/z/<STEM> must find the archive the client asked for."""
    share = tmp_path / "share"
    share.mkdir()
    names = ["Duke Nukem 3D (1996)(3D Realms).zip",
             "Duke Nukem II (1993)(Apogee).zip",
             "DOOM (1993)(Id Software).zip"]
    for n in names:
        (share / n).write_bytes(b"PK\x03\x04")

    monkeypatch.setattr(srv, "SEARCH_DIRS", [str(share)])
    srv.H._stems = None
    index = srv.H._build_index()

    for n in names:
        stem = srv.zip_stem(n)
        assert index.get(stem) == str(share / n), (n, stem)
