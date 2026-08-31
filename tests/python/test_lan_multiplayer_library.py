"""The staged library's LAN-multiplayer contract for the id Tech / Quake family.

Every assertion here is a bug that was on hardware and looked like something
else. They share one shape: **the process starts, stays in the process list,
and never opens its socket** -- so a PROCLIST says the host is healthy while
every other box's server browser is empty.

  * Hexen II's GL build dies at the VIDEO MODE CHECK before it loads the map.
    Measured on .240 (1080p panel) 2026-08-31: glh2.exe answers "Specified
    video mode not available" at 1920x1080, 1280x1024 AND 1280x960, and starts
    at 1024x768. The launchers used to pass the desktop's own mode, which is
    fine on a 4:3 CRT and fatal on every 16:9 box -- and for the HOST launcher
    that means no listen server at all.
  * SiN's dedicated server binds UDP 22450 only once a level is loaded, so
    `sin.exe +set dedicated 1` with no `+map` is a server nobody can join.
  * Jedi Knight DF2 and Mysteries of the Sith refuse to host or join with
    "No Valid Characters" until a pilot AND a multiplayer character exist, and
    the retail trees ship player\\ empty.

These are library facts, so the test reads the SHARE, and SKIPS LOUDLY when it
is not mounted -- a silent skip would let the library rot unnoticed, which is
the same reasoning as tests/test_staged_library.py.
"""
import os

import pytest

LIB = os.environ.get("RETRO_GAMES_LIBRARY",
                     "/mnt/retro-share/Files/Files/Games-Library")
if not os.path.isdir(LIB):
    LIB = "/mnt/retro-share/Files/Games-Library"

pytestmark = pytest.mark.skipif(
    not os.path.isdir(LIB),
    reason="staged library not mounted at %s - SKIPPED LOUDLY, not passed" % LIB)


def read(rel):
    with open(os.path.join(LIB, rel), "r", newline="", errors="replace") as fh:
        return fh.read()


# --- Hexen II: a 4:3-only engine must be asked for a mode it has -------------

HEXEN_GL_LAUNCHERS = (
    "HexenII/Host Hexen II - LAN.bat",
    "HexenII/Join Hexen II - LAN.bat",
    "HexenII/Play Hexen II.bat",
)


@pytest.mark.parametrize("rel", HEXEN_GL_LAUNCHERS)
def test_hexen2_gl_launchers_cap_to_a_mode_the_engine_has(rel):
    text = read(rel)
    assert "-cap 1024 768" in text, (
        "%s must call FLEETRES with -cap 1024 768: glh2.exe has a small fixed "
        "mode table and refuses 1920x1080, 1280x1024 and 1280x960" % rel)


@pytest.mark.parametrize("rel", HEXEN_GL_LAUNCHERS)
def test_hexen2_gl_launchers_pass_the_4x3_variables(rel):
    text = read(rel)
    assert "%FR_W43%" in text and "%FR_H43%" in text, rel
    assert "-width %FR_W%" not in text, (
        "%s passes the raw desktop width, which is what killed the listen "
        "server on every 16:9 box" % rel)


def test_hexen2_host_sets_the_hostname_as_a_cvar_not_a_switch():
    """-hostname is not a Hexen II command-line switch. It was silently
    ignored, so the host advertised the Windows machine name."""
    launch = [ln for ln in read("HexenII/Host Hexen II - LAN.bat").splitlines()
              if ln.lstrip().startswith("start ")]
    assert len(launch) == 1, launch
    assert '+hostname "NSC Retro Fleet Hexen II"' in launch[0]
    # The comment block explains the old switch, so only the LAUNCH LINE is
    # checked -- otherwise the test fails on its own documentation.
    assert "-hostname " not in launch[0]


def test_hexen2_host_still_starts_a_listen_server_on_a_map():
    text = read("HexenII/Host Hexen II - LAN.bat")
    assert "-listen 8" in text
    assert "+map demo1" in text


# --- SiN: a dedicated server with no map never opens its socket -------------

@pytest.mark.parametrize("rel", ("SiNGold/ds_deathmatch.bat",
                                 "SiNGold/ds_sinctf.bat"))
def test_sin_dedicated_servers_load_a_map(rel):
    text = read(rel)
    assert "+map " in text, (
        "%s must pass +map: sin.exe with `+set dedicated 1` alone starts, sits "
        "in the process list looking healthy, and never binds UDP 22450" % rel)


def test_sin_deathmatch_server_declares_deathmatch_and_a_name():
    text = read("SiNGold/ds_deathmatch.bat")
    assert "+set deathmatch 1" in text
    assert "+set hostname" in text


# --- Sith engine: the empty player\ directory that reads as a network fault --

@pytest.mark.parametrize("title", ("JediKnightDF2", "JediKnightMotS"))
def test_sith_engine_titles_stage_a_playable_profile(title):
    """Host Game answers "No Valid Characters" without BOTH of these, and the
    message says nothing about a missing directory."""
    base = os.path.join(LIB, title, "player", "fleet")
    for name in ("fleet.plr", "fleet.mpc"):
        path = os.path.join(base, name)
        assert os.path.isfile(path), (
            "%s is missing: %s ships player\\ empty, and without a pilot AND a "
            "multiplayer character the game can neither host nor join"
            % (path, title))
        assert os.path.getsize(path) > 0


@pytest.mark.parametrize("title", ("JediKnightDF2", "JediKnightMotS"))
def test_sith_profile_name_comes_from_the_filename(title):
    """Both files are plain text and carry no name field, which is what makes
    them renameable per box. If that stops being true, staging one shared pair
    stops being safe."""
    path = os.path.join(LIB, title, "player", "fleet", "fleet.plr")
    with open(path, "rb") as fh:
        head = fh.read(16)
    assert head.startswith(b"version "), head
