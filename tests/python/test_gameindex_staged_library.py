"""GAMEINDEX must recognise every title in the fleet's staged library.

Why this test exists
--------------------
On 2026-08-29 only ten of the twenty-nine titles in
``\\\\192.168.1.122\\files\\Files\\Games-Library`` were recognised by the agent's
``GAMEINDEX``. The other nineteen were installed, had desktop shortcuts and were
being played, and the host still could not see them: ``gameindex.c`` detects a
game purely by finding a known executable name in a directory, and those
nineteen executables were simply not in ``g_sigs[]``.

That failure is invisible from the host - an unlisted game looks exactly like an
uninstalled one - so it needs a test rather than a memory. This asserts the
signature table against the staged library's real executable names.

The pairs below are the file the SIGNATURE matches on, which is deliberately not
always the file the desktop shortcut runs:

  * distinctive over convenient - Tiberian Sun is matched on ``SUN.EXE`` and not
    on its ``GAME.EXE``, Red Alert 2 on ``Ra2.exe``/``gamemd.exe`` and not on its
    ``game.exe``, Descent 3 on ``Descent 3.exe`` and not on ``main.exe``. Those
    three generic names would collide with unrelated software during the
    depth-limited walk of every fixed drive.
  * never a launcher of ours - several titles are started by a ``Play *.bat`` we
    wrote, and a signature that depended on it would break the moment the
    library renamed one.

If a title is legitimately renamed or dropped from the library, update this list
in the same commit as the signature change.
"""
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
GAMEINDEX_C = REPO / "agent" / "src" / "gameindex.c"

# title in Games-Library  ->  the executable its signature matches on
STAGED_LIBRARY = {
    "AliensVsPredator":  "avp.exe",
    "Carmageddon1":      "MAINPROG.EXE",
    "Carmageddon2":      "carma2.exe",
    "CounterStrike16":   "hl.exe",              # + moddir cstrike
    "Descent1":          "DESCENTR.EXE",
    "Descent2":          "DESCENTW.EXE",
    "Descent3":          "Descent 3.exe",
    "DeusEx":            "DeusEx.exe",
    "HalfLife1":         "hl.exe",              # + moddir valve
    "JediAcademy":       "jamp.exe",
    "JediKnightDF2":     "JK.EXE",
    "JediKnightMotS":    "JKM.EXE",
    "Quake1":            "glquake.exe",
    "Quake2Complete":    "quake2.exe",
    "Quake3-TeamArena":  "quake3.exe",
    "RedAlert2":         "Ra2.exe",
    "RedFaction":        "rf.exe",
    "RedneckRampage":    "RR.EXE",
    "Shogo":             "Shogo.exe",
    "SiNGold":           "sin.exe",
    "SoldierOfFortune":  "SoF.exe",
    "StarCraft":         "StarCraft.exe",
    "SystemShock1":      "sshock.exe",
    "SystemShock2":      "shock2.exe",
    "Thief2":            "Thief2.exe",
    "ThiefGold":         "THIEF.EXE",
    "TiberianSun":       "SUN.EXE",
    "UnrealGold":        "Unreal.exe",
    "UnrealTournament":  "UnrealTournament.exe",
    # approved for staging 2026-08-29; the fleet runs a ut2004-server on :7777
    "UT2004":            "UT2004.exe",
}

# Names generic enough that a signature on them would fire on unrelated
# software during the depth-3 walk of every fixed drive.
FORBIDDEN_EXES = {"game.exe", "main.exe", "setup.exe", "launcher.exe",
                  "start.exe", "play.exe", "run.exe", "install.exe"}

_ROW = re.compile(
    r'\{\s*"(?P<key>[^"]*)"\s*,\s*"(?P<name>[^"]*)"\s*,\s*'
    r'"(?P<exe>[^"]*)"\s*,\s*(?P<mod>NULL|"[^"]*")\s*,\s*"(?P<engine>[^"]*)"\s*\}')


def signature_rows():
    src = GAMEINDEX_C.read_text(encoding="utf-8", errors="replace")
    start = src.index("static const game_sig_t g_sigs[]")
    end = src.index("};", start)
    rows = []
    for m in _ROW.finditer(src[start:end]):
        rows.append({
            "key": m.group("key"),
            "name": m.group("name"),
            "exe": m.group("exe"),
            "moddir": None if m.group("mod") == "NULL" else m.group("mod").strip('"'),
            "engine": m.group("engine"),
        })
    return rows


def test_the_signature_table_actually_parses():
    rows = signature_rows()
    # A regex that silently matched nothing would make every other assertion
    # here pass vacuously.
    assert len(rows) > 40, f"only parsed {len(rows)} signatures"


def test_every_staged_library_title_has_a_signature():
    have = {r["exe"].lower() for r in signature_rows()}
    missing = sorted(t for t, exe in STAGED_LIBRARY.items()
                     if exe.lower() not in have)
    assert not missing, (
        "GAMEINDEX cannot see these staged titles: " + ", ".join(missing))


def test_no_signature_matches_a_dangerously_generic_exe_name():
    bad = sorted({r["exe"] for r in signature_rows()
                  if r["exe"].lower() in FORBIDDEN_EXES})
    assert not bad, (
        "these exe names are too generic to identify a game: " + ", ".join(bad))


def test_the_goldsrc_family_is_still_split_by_moddir():
    # hl.exe alone cannot tell Half-Life from Counter-Strike; the mod directory
    # is the only thing that does, so every hl.exe row must carry one.
    rows = [r for r in signature_rows() if r["exe"].lower() == "hl.exe"]
    assert rows, "no hl.exe signature at all"
    assert all(r["moddir"] for r in rows), \
        "an hl.exe signature with no moddir claims every GoldSrc mod at once"


def test_ioquake3_is_matched_under_the_name_the_library_actually_ships():
    # The staged Quake3-TeamArena tree ships upstream's ioquake3.x86.exe, not
    # ioquake3.exe, so the plain name alone left the ioq3 build undetected.
    exes = {r["exe"].lower() for r in signature_rows() if r["key"] == "ioquake3"}
    assert "ioquake3.x86.exe" in exes
    assert "ioquake3.exe" in exes, "the plain name must keep working too"


def test_keys_are_unique_per_game_family():
    # Several signatures may share a key (two spellings of one game), but two
    # DIFFERENT games sharing a key would merge them in the index.
    by_key = {}
    for r in signature_rows():
        by_key.setdefault(r["key"], set()).add(r["name"])
    clashes = {k: sorted(v) for k, v in by_key.items() if len(v) > 1}
    assert not clashes, f"one key used for different games: {clashes}"
