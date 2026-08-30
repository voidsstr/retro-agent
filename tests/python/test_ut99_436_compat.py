"""The non-SSE2 boxes' only route to UT99 multiplayer must stay open.

WHY THIS EXISTS
---------------
UT99 469e is compiled with SSE2 throughout, so `.124`, `.133` and `.143` (and
the Win98 Pentium boxes as they arrive) cannot execute a single frame of it.
Their ONLY route to UT99 multiplayer is the staged retail **436** tree joining
the fleet's 469e server at 192.168.1.132:7797.

That route was believed to be closed for a whole day. FINDINGS.md, both
`requires.json` notes and a task brief all stated that "a retail 436 client
cannot join a 469e server at all" and concluded that a second, 436-compatible
dedicated server had to be stood up. **Nobody had measured it.** It was an
inference from two version numbers, and it was wrong:

  * OldUnreal's own release notes say 469e is "completely network compatible
    with all previous public releases of UT (down to 432)";
  * the live server's ini already carried ``MinClientVersion=432``;
  * measured on hardware 2026-08-30, `.143` (Athlon K7, **no SSE at all**) and
    `.133` (dual Pentium III, SSE1 only) were in ONE match together, the server
    logging ``HELLO REVISION=0 MINVER=400 VER=436`` then ``Join succeeded`` for
    each, and the 436 client's own LAN Servers browser reported the server's
    rules as ``Game Version 469 / Min. Compatible Version 432``.

UT99's join is a VERSION handshake with a floor -- not a package-hash or
ServerPackages check. So this test pins the three things that would silently
re-close the route, each of which looks harmless in isolation:

1. ``UnrealTournament436`` must NOT declare an sse2 requirement. The gate
   enforces ``cpu_features`` as a hard NO, so one copied line would stop the
   436 tree reaching the exact boxes it exists for -- and a gated title is
   simply absent on the box, with nothing saying why.
2. ``UnrealTournament`` (469e) must KEEP its sse2 requirement, or the gate goes
   back to approving a title that dies with 0xC000001D on those boxes.
3. The staged 436 tree's favourites must carry the fleet server, on its QUERY
   port. GAMESYNC re-copies the staged ini over whatever the favourites agent
   wrote -- that really happened on `.171` and again on `.143` -- so the
   library, not the agent, is what has to hold this entry for it to survive.

And it forbids the false claim itself from coming back into the notes, because
the claim is what caused the wasted work: a phantom defect sends people to
build something nobody needed.

SKIPS LOUDLY when the share is not mounted. A dev host without the SMB mount
must not fail the suite, but a silent skip would let the library rot.
"""
import json
import os
import re

import pytest

LIB = "/mnt/retro-share/Files/Games-Library"
T436 = os.path.join(LIB, "UnrealTournament436")
T469 = os.path.join(LIB, "UnrealTournament")

FLEET_HOST = "192.168.1.132"
FLEET_QUERY_PORT = "7798"          # UT99 answers GameSpy on game port + 1
FLEET_GAME_PORT = 7797

# The engine's own floor, and the value the live server's ini sets. A 436
# client clears it; this is the number the whole route rests on.
MIN_CLIENT_VERSION = 432
RETAIL_CLIENT_VERSION = 436

pytestmark = pytest.mark.skipif(
    not os.path.isdir(LIB),
    reason="SKIP (LOUD): %s not mounted - the UT99 436 multiplayer route was "
           "NOT checked. Run this on a host with the share mounted." % LIB,
)


def _requires(tree):
    path = os.path.join(tree, "requires.json")
    assert os.path.isfile(path), "%s is missing" % path
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _cpu_features(doc):
    return [str(f).lower() for f in (doc.get("cpu_features") or [])]


def test_436_tree_is_not_gated_off_the_non_sse2_boxes():
    """The 436 tree exists FOR the non-SSE2 boxes; it must not require sse2."""
    doc = _requires(T436)
    feats = _cpu_features(doc)
    assert "sse2" not in feats, (
        "UnrealTournament436 declares cpu_features=%r. The gate enforces that "
        "as a hard NO, which would keep the 436 tree off .124/.133/.143 - the "
        "only boxes it exists for - and a gated title is simply absent with "
        "nothing on the box saying why." % (feats,)
    )


def test_469e_tree_keeps_its_sse2_requirement():
    """The inverse: 469e really does need SSE2 and must stay gated."""
    doc = _requires(T469)
    feats = _cpu_features(doc)
    assert "sse2" in feats, (
        "UnrealTournament (469e) no longer declares cpu_features sse2. Measured "
        "2026-08-30: Core.dll carries 1083 SSE2-class instructions in ordinary "
        "compiler prologue code (retail 436 is the control, at zero), so on "
        ".124/.133/.143 the process dies with 0xC000001D/0xC000001E before "
        "writing a line of its own log."
    )


def test_both_trees_are_deployable_to_the_same_boxes_for_different_reasons():
    """The two trees must not BOTH be gated off a non-SSE2 box.

    A box that can run neither has no UT99 at all, which is the state this
    whole exercise existed to get out of.
    """
    assert "sse2" not in _cpu_features(_requires(T436))
    assert "sse2" in _cpu_features(_requires(T469))


def _favourites(tree):
    path = os.path.join(tree, "System", "UnrealTournament.ini")
    assert os.path.isfile(path), "%s is missing" % path
    with open(path, encoding="latin-1") as fh:
        text = fh.read()
    block = re.search(
        r"\[UBrowser\.UBrowserFavoritesFact\]\s*\r?\n"
        r"((?:Favorite[^\r\n]*\r?\n)*)", text)
    assert block, "%s has no [UBrowser.UBrowserFavoritesFact] section" % path
    return [ln.split("=", 1)[1]
            for ln in block.group(1).splitlines()
            if ln.startswith("Favorites[")]


def test_staged_436_favourites_carry_the_fleet_server():
    """GAMESYNC re-copies this file, so the LIBRARY has to hold the entry.

    The favourites agent's write is not durable here: a sync restores the
    staged ini and the fleet server vanishes from the Favorites tab. Measured
    on .143 on 2026-08-30 -- its list was back to the three staged internet
    servers while .124 still had the agent's seventeen.
    """
    entries = [e for e in _favourites(T436) if e.strip()]
    assert entries, "the staged 436 tree ships an EMPTY favourites list"

    ours = [e for e in entries if FLEET_HOST in e]
    assert ours, (
        "the staged UnrealTournament436 favourites do not mention %s. Every "
        "box that syncs this tree then has no fleet server in its Favorites "
        "tab, and a GAMESYNC silently reverts whatever the favourites agent "
        "wrote. Entries found: %r" % (FLEET_HOST, entries)
    )

    # Shape: <name>\<host>\<query port>\<bool>. The game splits on backslashes,
    # so a name containing one would shift every field after it.
    for entry in ours:
        fields = entry.split("\\")
        assert len(fields) == 4, (
            "favourite %r has %d backslash-separated fields, expected 4 "
            "(name\\host\\queryport\\bool)" % (entry, len(fields))
        )
        assert fields[1] == FLEET_HOST
        assert fields[2] == FLEET_QUERY_PORT, (
            "favourite %r uses port %s. UT99 answers its GameSpy query on the "
            "GAME port + 1, i.e. %s for a server on %d - the browser shows a "
            "permanently unresponsive entry if this is the game port."
            % (entry, fields[2], FLEET_QUERY_PORT, FLEET_GAME_PORT)
        )


def test_the_fleet_server_is_first_in_the_staged_favourites():
    """Ours is the one that is always up and always joinable; it leads."""
    entries = [e for e in _favourites(T436) if e.strip()]
    assert FLEET_HOST in entries[0], (
        "the first staged favourite is %r, not the fleet server. The LAN "
        "server is the only one guaranteed reachable with no internet."
        % (entries[0],)
    )


# --- the claim that caused the wasted work must not come back ----------------

_FALSE_CLAIMS = [
    re.compile(r"436\s+client\s+cannot\s+join", re.IGNORECASE),
    re.compile(r"cannot\s+join\s+(?:it|the\s+fleet's\s+469e\s+server)\s+at\s+all",
               re.IGNORECASE),
    re.compile(r"no\s+route\s+to\s+UT99\s+multiplayer", re.IGNORECASE),
]

# The correction itself has to be allowed to quote the claim it is refuting,
# so a line is only a violation when it is NOT adjacent to a refutation.
_REFUTES = re.compile(
    r"was\s+wrong|INFERENCE|verified|VERIFIED|it\s+DOES\s+join|JOINS",
)


@pytest.mark.parametrize("tree", [T436, T469])
def test_requires_notes_do_not_restate_the_false_claim(tree):
    notes = _requires(tree).get("notes", "")
    for pattern in _FALSE_CLAIMS:
        for match in pattern.finditer(notes):
            window = notes[max(0, match.start() - 320):match.end() + 320]
            assert _REFUTES.search(window), (
                "%s/requires.json states %r without the measurement that "
                "refutes it. A 436 client DOES join the fleet's 469e server "
                "(two-box hardware proof, 2026-08-30); repeating the old claim "
                "is what sent a session off to build a second server nobody "
                "needed." % (os.path.basename(tree), match.group(0))
            )


def test_version_floor_arithmetic_is_the_whole_argument():
    """Guards the reasoning, not a file: 436 must clear the server's floor.

    Deliberately asserts BOTH the true relation and the false one, so a future
    edit that 'fixes' the constants cannot quietly invert the conclusion.
    """
    assert RETAIL_CLIENT_VERSION >= MIN_CLIENT_VERSION, (
        "436 must be at or above the server's MinClientVersion of %d - this is "
        "the entire reason the non-SSE2 boxes can play." % MIN_CLIENT_VERSION
    )
    assert not RETAIL_CLIENT_VERSION < MIN_CLIENT_VERSION
