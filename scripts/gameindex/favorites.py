r"""favorites.py — turn a live server list into the file a game actually reads.

One writer per engine. A writer returns the exact bytes to place at an exact
path on the box, plus a content hash; sync.py compares that hash against what
it last wrote and skips the machine entirely when nothing changed.

Two rules learned the hard way and encoded here (fleetbook recipe
`populate-quake-iii-arena-favorites-fleet-wide-with-live-inte`):

  * Write autoexec.cfg, NEVER the game's own config. Quake III rewrites
    q3config.cfg from memory on exit, so an edit made while the game is
    running is silently undone. Q3's init order is default.cfg ->
    q3config.cfg -> autoexec.cfg, so autoexec wins, and the game never writes
    it back.
  * MERGE, do not overwrite. An existing autoexec.cfg usually carries
    r_mode/com_maxfps settings that someone tuned. Strip only our own marked
    block and any stray `seta serverN` lines, keep the rest.

Engines with no writer are declared unsupported WITH A REASON. A missing
writer and "there were no servers" must not look the same to the caller.

Where each engine keeps its favourites, and how we know
-------------------------------------------------------
None of this was assumed from the Quake pattern; every mechanism below was
read out of the game's own files in the staged library
(``\\192.168.1.122\files\Files\Games-Library``):

  * **Quake III** - ``<dir>\baseq3\autoexec.cfg``, ``seta server1..16``.
  * **Quake II** - ``<dir>\baseq2\autoexec.cfg``, ``set adr0..8``.
  * **Unreal engine 1** (UT99, Unreal Gold, Deus Ex) - the UBrowser package's
    own bytecode carries the format as a comment::

        class UBrowserFavoritesFact extends UBrowserServerListFactory;
        var config int FavoriteCount;
        var config string Favorites[100];
        /* eg Favorites[0]=Host Name\10.0.0.1\7778\True */

    so the section is ``[UBrowser.UBrowserFavoritesFact]`` and the third field
    is the **query port**, not the game port - ``Query()`` calls
    ``FoundServer(ParseOption(..,1), Int(ParseOption(..,2)), ...)``. Each game
    keeps it in its OWN ini, which is why the target is chosen per title.
  * **UT2004** - ``XInterface.u`` declares
    ``struct ServerFavorite { int ServerID; string IP; int Port; int QueryPort;
    string ServerName; }`` and ``var() protected config array<ServerFavorite>
    Favorites;`` on ``class ExtendedConsole``, so the lines are
    ``Favorites=(...)`` under ``[XInterface.ExtendedConsole]`` in UT2004.ini.
  * **GoldSrc** - the staged CS 1.6 tree's ``revSrvBrowser.dll`` contains the
    exact template it writes into ``config\ServerBrowser.vdf``::

        "filters"
                "favorites"
                "history"
                        "%d"
                        {
                                "name"          "%s"
                                "address"       "%s:%d"
                                "lastplayed"    "%u"
                                "appID"         "%u"
                        }

Two engine-level facts that the *engine* alone cannot express, so titles are
keyed individually below: Soldier of Fortune II and Jedi Academy are Quake III
engine but their game directory is ``base``, not ``baseq3``; and the staged
Half-Life tree is WON protocol 46 while every fleet GoldSrc server answers
protocol 48, so Half-Life must not be pointed at them.
"""
import hashlib
import re

BEGIN = "// --- BEGIN retro-fleet favorites (managed, do not edit) ---"
END = "// --- END retro-fleet favorites ---"


def content_hash(text):
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()[:16]


def _strip_block(existing, seta_re):
    """Remove our managed block and any loose favorite lines, keep the rest."""
    out, skipping = [], False
    for line in existing.splitlines():
        if line.strip() == BEGIN:
            skipping = True
            continue
        if line.strip() == END:
            skipping = False
            continue
        if skipping:
            continue
        if seta_re.match(line.strip()):
            continue
        out.append(line)
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out)


# --- Quake III family --------------------------------------------------------

_Q3_SETA = re.compile(r"^seta\s+server\d+\s", re.IGNORECASE)


def q3_favorites(servers, existing="", slots=16):
    """Q3 favourites are cvars server1..server16, written as seta serverN "ip:port"."""
    body = [BEGIN]
    for i, s in enumerate(servers[:slots], start=1):
        label = (s["hostname"] or s["addr"]).replace("\n", " ")[:60]
        body.append(f'seta server{i} "{s["addr"]}"        // {label}')
    # Blank any slot we are not using, or a stale address from a previous run
    # keeps showing up in the in-game favourites list forever.
    for i in range(len(servers[:slots]) + 1, slots + 1):
        body.append(f'seta server{i} ""')
    body.append(END)
    kept = _strip_block(existing, _Q3_SETA)
    text = (kept + "\n\n" if kept.strip() else "") + "\n".join(body) + "\n"
    return text


# --- Quake II ----------------------------------------------------------------
# Q2's address book is adr0..adr8, set from the console or a config.
_Q2_SETA = re.compile(r"^set\s+adr\d+\s", re.IGNORECASE)


def q2_favorites(servers, existing="", slots=9):
    body = [BEGIN]
    for i, s in enumerate(servers[:slots]):
        label = (s["hostname"] or s["addr"]).replace("\n", " ")[:60]
        body.append(f'set adr{i} "{s["addr"]}"        // {label}')
    for i in range(len(servers[:slots]), slots):
        body.append(f'set adr{i} ""')
    body.append(END)
    kept = _strip_block(existing, _Q2_SETA)
    return (kept + "\n\n" if kept.strip() else "") + "\n".join(body) + "\n"




# --- row helpers -------------------------------------------------------------

def _field(row, name, default=""):
    """Read a column that may not be present on every row shape.

    Rows arrive as sqlite3.Row from the DB and as plain dicts from tests and
    from sync.LOCAL_SERVERS, and the two raise different exceptions for a
    missing column. A DB created before `query_port` existed must degrade to
    the default rather than take a whole pass down.
    """
    try:
        v = row[name]
    except (KeyError, IndexError):
        return default
    return default if v is None else v


def _label(row, limit=60):
    text = str(_field(row, "hostname") or _field(row, "addr"))
    return " ".join(text.split())[:limit]


def _split_addr(row):
    addr = str(_field(row, "addr"))
    host, _, port = addr.rpartition(":")
    try:
        return host, int(port)
    except ValueError:
        return addr, 0


def _query_port(row):
    """The port a server answers QUERIES on, which is not always game port + 1.

    The fleet's own two Unreal servers disagree: UT99 is 7797/7798 (+1) but
    UT2004 is 7777/7787 (+10). So this cannot be derived from the game port -
    it is carried on the row by whatever probed the server, and the +1 default
    is only a last resort for a row that predates the column.
    """
    try:
        qp = int(_field(row, "query_port", 0) or 0)
    except (TypeError, ValueError):
        qp = 0
    return qp or (_split_addr(row)[1] + 1)


# --- ini section splicing ----------------------------------------------------

_SECTION_RE = re.compile(r"^\s*\[(?P<name>[^\]]+)\]\s*$")


def _ini_replace_keys(existing, section, own_re, body):
    """Replace only the keys we own inside ONE ini section.

    Everything else - other sections, other keys in the same section, comments,
    ordering, the file's own blank lines - is carried through untouched. That
    matters more here than in the Quake writers: UnrealTournament.ini is a
    700-line file holding the video mode, every key bind and the whole package
    manifest, and it is the same file the game rewrites on exit.
    """
    out, insert_at, in_section, seen = [], None, False, False
    for line in existing.splitlines():
        m = _SECTION_RE.match(line)
        if m:
            if in_section and insert_at is None:
                insert_at = len(out)
            in_section = m.group("name").strip().lower() == section.lower()
            seen = seen or in_section
            out.append(line)
            continue
        if in_section and own_re.match(line.strip()):
            if insert_at is None:
                insert_at = len(out)
            continue
        out.append(line)
    if in_section and insert_at is None:
        insert_at = len(out)

    if not seen:
        while out and not out[-1].strip():
            out.pop()
        if out:
            out.append("")
        out.append("[%s]" % section)
        out.extend(body)
        return "\n".join(out) + "\n"

    out[insert_at:insert_at] = body
    return "\n".join(out) + "\n"


# --- Unreal engine 1: UT99, Unreal Gold, Deus Ex -----------------------------
#
# [UBrowser.UBrowserFavoritesFact]
# FavoriteCount=1
# Favorites[0]=NSC Retro Fleet Arena (UT99)\192.168.1.132\7798\False
#
# Field order and meaning are from UBrowser.u itself, not from folklore:
# Query() reads option 1 as the IP and option 2 as the QUERY port, and
# SaveFavorites() writes `HostName\IP\QueryPort\bKeepDescription`.

_UNREAL_OWN = re.compile(r"^(FavoriteCount|Favorites\[\d+\])\s*=", re.IGNORECASE)
UNREAL_SECTION = "UBrowser.UBrowserFavoritesFact"


def unreal_favorites(servers, existing="", slots=16):
    picked = servers[:slots]
    body = ["FavoriteCount=%d" % len(picked)]
    for i, s in enumerate(picked):
        host = _split_addr(s)[0]
        # The game splits this line on backslashes, so one inside a server
        # name would silently shift every field after it.
        name = _label(s).replace("\\", "/")
        body.append("Favorites[%d]=%s\\%s\\%d\\False"
                    % (i, name, host, _query_port(s)))
    # SaveFavorites() terminates the list with one empty entry. Query() stops
    # at FavoriteCount either way, but matching what the game itself writes
    # means a hand-saved file and ours are the same shape.
    body.append("Favorites[%d]=" % len(picked))
    return _ini_replace_keys(existing, UNREAL_SECTION, _UNREAL_OWN, body)


# --- UT2004 ------------------------------------------------------------------
#
# [XInterface.ExtendedConsole]
# Favorites=(ServerID=0,IP="192.168.1.132",Port=7777,QueryPort=7778,ServerName="...")
#
# From XInterface.u: `struct ServerFavorite { int ServerID; string IP;
# int Port; int QueryPort; string ServerName; }` and
# `var() protected config array<ServerFavorite> Favorites;` on ExtendedConsole,
# which UT2004.ini names as the Console class.

_UT2K4_OWN = re.compile(r"^Favorites\s*=", re.IGNORECASE)
UT2K4_SECTION = "XInterface.ExtendedConsole"


def _ut2k4_query_port(row):
    r"""UT2004's CLIENT asks on a DIFFERENT PORT, in a DIFFERENT PROTOCOL, than
    the port our own health probe uses -- and the row carries only the latter.

    A UT2004 server opens TWO query listeners:

      game port + 1   `IpServer.UdpServerQuery`, Epic's own binary protocol.
                      THIS is what the in-game server browser speaks, and the
                      only one it will ever speak.
      OldQueryPortNumber   the legacy GameSpy `\status\` text protocol,
                      default game port + 10. Third-party tools use it -- ours
                      included: masters.py `_gamespy_probe` is a GameSpy client.

    `_query_port()` returns the port the row was PROBED on, so for the fleet's
    own server it returns 7787. Writing that into a favourite made the client
    send its binary query to the GameSpy listener, which never answers it, and
    the browser showed the server with **Ping N/A** and no map or player count
    -- while every host-side health check said the server was up in 49 ms.

    MEASURED on box .240, 2026-08-30, with three favourites differing only in
    QueryPort and a UDP sink on the third:

        QueryPort=7778   -> name resolved to "NSC Retro Fleet Arena",
                            map DM-Rankin, 0/12 players, ping 54
        QueryPort=7787   -> N/A
        QueryPort=29000  -> N/A, and the sink logged the client's actual
                            query: b"\x80\x00\x00\x00\x00" -- i.e. the
                            binary UdpServerQuery, NOT `\status\`.

    So the client honours QueryPort verbatim and speaks only the binary
    protocol; game port + 1 is where that lives, by the engine's own default.

    UT99 is deliberately NOT changed: its browser speaks the same GameSpy
    protocol our probe does, so there the probed port is the right one, and it
    is 7798 = port + 1 anyway. That coincidence is exactly why this stayed
    invisible for so long.
    """
    return _split_addr(row)[1] + 1


def ut2k4_favorites(servers, existing="", slots=16):
    body = []
    for i, s in enumerate(servers[:slots]):
        host, port = _split_addr(s)
        name = _label(s).replace('"', "'")
        body.append('Favorites=(ServerID=%d,IP="%s",Port=%d,QueryPort=%d,'
                    'ServerName="%s")'
                    % (i, host, port, _ut2k4_query_port(s), name))
    if not body:
        # An empty array config is expressed by writing no lines at all; the
        # dropped ones are already gone, which is how a favourite is removed.
        body = []
    return _ini_replace_keys(existing, UT2K4_SECTION, _UT2K4_OWN, body)


# --- GoldSrc: Counter-Strike 1.6 and friends ---------------------------------
#
# config\ServerBrowser.vdf, whose exact shape is the format string inside the
# staged tree's own revSrvBrowser.dll. The file also holds a "history" block
# that is none of our business, so this parses the document, replaces only the
# "favorites" subtree, and re-serialises - rather than printing a whole file
# from scratch and hoping nothing else was in it.

class _VdfError(ValueError):
    pass


def _vdf_parse(text):
    """Parse the small subset of VDF the GoldSrc server browser writes.

    Deliberately strict. A file we cannot read back exactly is a file we must
    not replace, so anything unexpected raises rather than being skipped - the
    caller turns that into "refuse to write".
    """
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
            continue
        if c == "/" and text[i:i + 2] == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if c in "{}":
            toks.append(c)
            i += 1
            continue
        if c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                raise _VdfError("unterminated string")
            toks.append(text[i + 1:j])
            i = j + 1
            continue
        raise _VdfError("unexpected character %r at %d" % (c, i))

    pos = [0]

    def block():
        out = []
        while pos[0] < len(toks):
            t = toks[pos[0]]
            if t == "}":
                pos[0] += 1
                return out
            if t == "{":
                raise _VdfError("value where a key was expected")
            key = t
            pos[0] += 1
            if pos[0] >= len(toks):
                raise _VdfError("key %r with no value" % key)
            nxt = toks[pos[0]]
            if nxt == "{":
                pos[0] += 1
                out.append((key, block()))
            elif nxt == "}":
                raise _VdfError("key %r with no value" % key)
            else:
                pos[0] += 1
                out.append((key, nxt))
        return out

    doc = block()
    if pos[0] != len(toks):
        raise _VdfError("trailing tokens")
    return doc


def _vdf_dump(doc, depth=0):
    """Serialise back out in revSrvBrowser.dll's own layout."""
    pad = "\t" * depth
    lines = []
    for key, val in doc:
        if isinstance(val, list):
            lines.append('%s"%s"' % (pad, key))
            lines.append("%s{" % pad)
            lines.extend(_vdf_dump(val, depth + 1))
            lines.append("%s}" % pad)
            lines.append("")
        else:
            gap = "\t\t" if len(key) < 8 else "\t"
            lines.append('%s"%s"%s"%s"' % (pad, key, gap, val))
    return lines


# Steam application ids, as the browser records them next to each favourite.
GOLDSRC_APPID = {"cs16": 10, "halflife": 70, "tfc": 20, "dod": 30,
                 "dmc": 40, "ts": 70}


def goldsrc_favorites(servers, existing="", slots=16, appid=10):
    try:
        doc = _vdf_parse(existing) if existing.strip() else [("filters", [])]
    except _VdfError as exc:
        raise WouldClobber(
            "goldsrc: %s is not VDF we can parse back (%s) - refusing to "
            "rewrite it" % ("config\\serverbrowser.vdf", exc))

    entries = []
    for i, s in enumerate(servers[:slots]):
        entries.append((str(i), [
            ("name", _label(s, 120).replace('"', "'")),
            ("address", str(_field(s, "addr"))),
            # A real timestamp here would change the file's hash every pass and
            # rewrite every box every five minutes for no reason.
            ("lastplayed", "0"),
            ("appID", str(appid)),
        ]))

    replaced = False
    for idx, (key, val) in enumerate(doc):
        if key.lower() != "filters" or not isinstance(val, list):
            continue
        inner = []
        for k2, v2 in val:
            if k2.lower() == "favorites":
                inner.append(("favorites", entries))
                replaced = True
            else:
                inner.append((k2, v2))
        if not replaced:
            inner.insert(0, ("favorites", entries))
            replaced = True
        doc[idx] = (key, inner)
        break
    if not replaced:
        doc.append(("filters", [("favorites", entries), ("history", [])]))

    return "\n".join(_vdf_dump(doc)).rstrip("\n") + "\n"


# The strip patterns, by engine, so the safety check in render() uses exactly
# the same rule the writer does rather than a second copy that can drift.
_SETA_RE = {"q3": _Q3_SETA, "q2": _Q2_SETA,
            "unreal": _UNREAL_OWN, "ut2k4": _UT2K4_OWN}

# Engines whose writer rewrites a whole STRUCTURED document rather than
# editing lines. A line-by-line "did we drop anything" check is meaningless
# against a re-serialised file - the writer does its own preservation check
# and raises WouldClobber itself.
_STRUCTURAL = {"goldsrc"}


# --- registry ----------------------------------------------------------------
#
# `subdir` is where the game's config lives relative to the install dir.
# `slots` is how many favourites the engine actually exposes.
WRITERS = {
    "q3": dict(fn=q3_favorites, subdir="baseq3", filename="autoexec.cfg",
               slots=16, supported=True),
    "q2": dict(fn=q2_favorites, subdir="baseq2", filename="autoexec.cfg",
               slots=9, supported=True),
    # 24, not 16: UBrowserFavoritesFact declares `Favorites[100]`, and the
    # curated seed list is 18 entries. Cutting 17 candidates down to 16 puts
    # the boundary right where the list is, so one server emptying reshuffles
    # the file and rewrites every box. Give the whole curated list room and
    # membership changes only when a server actually dies.
    "unreal": dict(fn=unreal_favorites, subdir="System",
                   filename="UnrealTournament.ini", slots=24, supported=True),
    "ut2k4": dict(fn=ut2k4_favorites, subdir="System",
                  filename="UT2004.ini", slots=16, supported=True),
    "goldsrc": dict(fn=goldsrc_favorites, subdir="config",
                    filename="serverbrowser.vdf", slots=16, supported=True),
    # Deliberately not implemented. Each needs a per-build answer we have not
    # verified on the fleet's actual installs, and writing a guess into a
    # game's config is worse than leaving it alone.
    "qw": dict(supported=False,
               why="classic QW has no favourites store; ezQuake's differs per build"),
    "t2": dict(supported=False, why="no writer implemented"),
    "rtcw": dict(supported=False, why="no writer implemented"),
    "nq": dict(supported=False, why="NetQuake has no favourites store"),
    "-": dict(supported=False, why="game has no server browser"),
}


# --- per-title policy --------------------------------------------------------
#
# Keyed on the agent's GAMEINDEX game key, because the ENGINE alone cannot say
# where a title keeps its favourites or which of our servers it can join:
#
#   * Soldier of Fortune II, Jedi Academy and Jedi Knight II are Quake III
#     engine but their game directory is `base`, not `baseq3`. Writing
#     baseq3\autoexec.cfg into them created a directory the game never reads -
#     a favourites file that could never have had any effect.
#   * Counter-Strike, The Specialists and Half-Life are all hl.exe. A CS
#     client pointed at the Specialists server gets a mod mismatch, and the
#     staged Half-Life tree is WON protocol 46 while every fleet GoldSrc
#     server answers protocol 48 - it cannot join them at all.
#   * UT99, Unreal Gold and Deus Ex share one favourites mechanism but each
#     keeps it in its own ini.
#
# `accepts` filters OUR OWN servers by the gamename they report, so a title is
# never given an address it cannot actually join. It is not applied to
# internet servers: we know exactly what runs on .132, and we have no reliable
# mod taxonomy for the rest of the world.
#
# A title absent from this table is reported unsupported WITH a reason rather
# than written with a guess.
TITLES = {
    "quake3":    dict(engine="q3", subdir="baseq3", accepts={"baseq3"}),
    "ioquake3":  dict(engine="q3", subdir="baseq3", accepts={"baseq3"}),
    "openarena": dict(engine="q3", subdir="baseoa", accepts={"baseoa"}),
    "quake2":    dict(engine="q2", subdir="baseq2", accepts={"baseq2"}),
    "q2pro":     dict(engine="q2", subdir="baseq2", accepts={"baseq2"}),
    "yquake2":   dict(engine="q2", subdir="baseq2", accepts={"baseq2"}),

    # The Unreal family's ini sits in System\ NEXT TO the exe, and the agent
    # already reports `dir` as that System directory - it indexes a game by
    # where it found the executable. `subdir` is therefore appended only when
    # the directory is not already inside it (see target_path), which is right
    # either way round and is not specific to one agent version.
    "ut99":      dict(engine="unreal", subdir="System", create=False,
                      filename="UnrealTournament.ini", accepts={"ut"}),
    "unreal":    dict(engine="unreal", subdir="System", create=False,
                      filename="Unreal.ini", accepts={"unreal"}),
    "deusex":    dict(engine="unreal", subdir="System", create=False,
                      filename="DeusEx.ini", accepts={"deusex"}),
    "ut2004":    dict(engine="ut2k4", subdir="System", create=False,
                      filename="UT2004.ini", accepts={"ut2004"}),
    "ut2003":    dict(engine="ut2k4", subdir="System", create=False,
                      filename="UT2003.ini", accepts={"ut2003"}),

    "cs16":      dict(engine="goldsrc", subdir="config", create=False,
                      filename="serverbrowser.vdf", accepts={"cstrike"},
                      appid=10),
    "ts":        dict(engine="goldsrc", subdir="config", create=False,
                      filename="serverbrowser.vdf", accepts={"ts"}, appid=70),
    "dod":       dict(engine="goldsrc", subdir="config", create=False,
                      filename="serverbrowser.vdf", accepts={"dod"}, appid=30),
    "tfc":       dict(engine="goldsrc", subdir="config", create=False,
                      filename="serverbrowser.vdf", accepts={"tfc"}, appid=20),
}

# `create=False` above means: update this file if it is there, never bring it
# into existence. It is the cheapest possible test for "does this build even
# use this mechanism", and it costs nothing.
#
#   * A WON-era Half-Life at C:\Sierra\Half-Life has no revSrvBrowser and no
#     config\serverbrowser.vdf; creating one writes a file nothing reads.
#   * An .ini for an Unreal-engine game always exists in a real install, and
#     one containing nothing but a favourites section would be worse than
#     none at all.
#
# autoexec.cfg is the opposite case - the Quake writers create it on purpose,
# because not existing is its normal state.

# Directories a favourites file has no business being written into.
# The benchmark harnesses are a real copy of Quake III whose whole value is
# that nothing changes underneath them; the user stopped this service
# precisely because a foreign write mid-test makes a result unattributable.
SKIP_DIRS = re.compile(r"(^|[\\/])(q3bench|[^\\/]*bench(mark)?s?)([\\/]|$)",
                       re.IGNORECASE)

# Titles we can DETECT and deliberately do not write, each with the reason.
# This is the difference between "the favourites agent does not cover this"
# and "the favourites agent has nothing it could honestly put there", which
# are answers to different questions.
UNWRITABLE = {
    "halflife":
        "the staged Half-Life tree is WON protocol 46 and every fleet GoldSrc "
        "server answers protocol 48 - it could not join them, so listing them "
        "would be a favourites list of dead entries. Half-Life is box-to-box "
        "LAN only here",
    "sof2":
        "no Soldier of Fortune II server on the fleet and no live SoF2 master "
        "we have verified; its Quake III lineage does NOT mean it can join a "
        "Quake III server",
    "jka":  "no Jedi Academy server on the fleet and no live JKA master",
    "jk2":  "no Jedi Knight II server on the fleet and no live JK2 master",
    "et":   "no Enemy Territory server on the fleet and no live ET master",
    "mohaa":
        "no MOHAA server on the fleet; note MOHAA also needs the framed "
        "\\xff\\xff\\xff\\xff\\x02getinfo\\x00 query, not Quake III's getstatus",
    "wolfmp": "no RtCW server on the fleet and wolfmaster.idsoftware.com is dead",
    "quake":  "NetQuake has no favourites store, and the fleet's Quake server "
              "is QuakeWorld, which a NetQuake client cannot join",
    "quakeworld": "classic QW keeps no favourites; the fleet server is reached "
                  "from the console with `connect 192.168.1.132:27502`",
    "ezquake": "ezQuake's favourites file differs per build; unverified here",
    "tribes2": "TribesNext encrypts the info response and Tribes 2 keeps no "
               "favourites file we have verified",
    # LAN-only titles. These are staged for multiplayer and it WORKS, but the
    # mechanism is a broadcast or a typed-in address, so there is no list for
    # this agent to populate. Saying so is the useful answer.
    "ra2":     "Red Alert 2 LAN play is UDP broadcast on this subnet - there "
               "is no server list to populate (proven two-box, .123 hosting)",
    "ra2yr":   "Yuri's Revenge LAN play is UDP broadcast - no server list",
    "tibsun":  "Tiberian Sun is IPX over IPXWrapper, LAN-only - no server list "
               "and no internet master",
    "descent": "Descent 1 is DOSBox IPX; the tunnel is `ipxnet connect <ip>` "
               "in the conf, not a favourites file",
    "descent2": "Descent 2 is DOSBox/IPX, same as Descent 1",
    "descent3": "Descent 3 is TCP/IP native but joins by typed address; its "
                "tracker is long dead and there is no fleet D3 server",
    "redfaction": "Red Faction's tracker is dead; LAN games are found by "
                  "broadcast, with no favourites file",
    "shogo":   "Shogo joins by typed address; no fleet server",
    "avp":     "Aliens versus Predator uses its own dead lobby; LAN by broadcast",
    "starcraft": "StarCraft LAN is UDP broadcast - no server list",
    "sshock2": "System Shock 2 co-op joins by typed address",
    "carmageddon": "Carmageddon 1 is DOSBox/IPX - no server list",
    "carmageddon2": "Carmageddon 2 uses IPXWrapper over the LAN - no server list",
    "redneck": "Redneck Rampage is DOSBox/IPX - no server list",
    "hexen2":  "Hexen II LAN play is the NetQuake broadcast; no favourites store",
    "hd":      "Hidden & Dangerous joins by typed address; no fleet server",
    "jk":      "Jedi Knight: Dark Forces II is DirectPlay LAN - no server list",
    "jkmots":  "Mysteries of the Sith is DirectPlay LAN - no server list",
    # Quake II engine, but that buys nothing without a server of their own:
    # a SiN or SoF client cannot join a Quake II server.
    "sin":     "SiN is Quake II engine but speaks its own game; no fleet SiN "
               "server and no live master",
    "sof":     "Soldier of Fortune is Quake II engine but speaks its own game; "
               "no fleet SoF server and no live master",
    # Single-player titles that reached the library for other reasons.
    "thief":   "Thief: The Dark Project is single-player - there is no "
               "multiplayer to have favourites for",
    "thief2":  "Thief II is single-player - there is no multiplayer to have "
               "favourites for",
    "sshock":  "System Shock 1 is single-player - there is no multiplayer "
               "to have favourites for",
    "hl2":     "no Half-Life 2 / Source server on the fleet",
    "diablo2": "Diablo II LAN is UDP broadcast; battle.net is not ours",
    "wolfsp":  "the RtCW single-player executable - the multiplayer one is "
               "WolfMP.exe, and there is no RtCW server on the fleet either",
}


def engines_for_keys(keys):
    """The engines these installed titles need server lists for.

    The agent reports Deus Ex (and a few others) with engine "-", because from
    the box's point of view it has no server browser worth naming. The host
    knows better, and this is what stops a box whose only Unreal-engine title
    is Deus Ex from never having any Unreal servers fetched for it.
    """
    return sorted({TITLES[k]["engine"] for k in keys if k in TITLES})


def writer_for(engine):
    return WRITERS.get(engine, {"supported": False, "why": "unknown engine"})


def policy_for(key, engine=""):
    """What we will do for ONE title, and why.

    Returns a dict with `supported` plus, when supported, the engine, the file
    to write and the local-server gamenames it may be given.
    """
    if key in UNWRITABLE:
        return {"supported": False, "why": UNWRITABLE[key]}
    t = TITLES.get(key)
    if t is None:
        return {"supported": False,
                "why": "no verified favourites mechanism for '%s' - it is "
                       "detected but nothing is written" % (key or "?")}
    spec = writer_for(t["engine"])
    if not spec.get("supported"):
        return {"supported": False, "why": spec.get("why", "unsupported")}
    out = dict(t)
    out["supported"] = True
    out["slots"] = spec["slots"]
    out.setdefault("filename", spec["filename"])
    out.setdefault("create", True)
    return out


class WouldClobber(Exception):
    """Rendering would drop a line the file already had.

    Raised rather than returned because there is no sensible way to continue:
    the only safe action is to leave the file alone. A caller that swallowed
    this and wrote anyway would be doing the exact thing the exception exists
    to prevent.
    """


def dropped_lines(engine, existing, text):
    """Lines present in `existing` that our output does not carry.

    Our block and stray favourite lines are *supposed* to disappear - they are
    ours to replace. Anything else going missing is a bug.

    This deliberately does NOT call `_strip_block`. Reusing it would make the
    check agree with the very function it is checking: if the strip rule ever
    became too greedy, both sides would drop the same lines and the safety net
    would report nothing. So "ours" is recomputed here from first principles -
    inside the markers, or matching the engine's favourite-line pattern.
    """
    spec = writer_for(engine)
    if not spec.get("supported") or engine in _STRUCTURAL:
        return []
    seta_re = _SETA_RE.get(engine)
    have = set(text.splitlines())
    lost, inside = [], False
    for line in existing.splitlines():
        stripped = line.strip()
        if stripped == BEGIN:
            inside = True
            continue
        if stripped == END:
            inside = False
            continue
        if inside or not stripped:
            continue
        if seta_re and seta_re.match(stripped):
            continue                      # a favourite line: ours to replace
        if line not in have:
            lost.append(line)
    return lost


def render(engine, servers, existing="", key=None):
    """Return (text, hash) for this engine, or (None, reason) if unsupported.

    Raises WouldClobber if the merge would lose something the file already
    had. This is a belt-and-braces check on top of the caller refusing to
    write when it could not read: the destructive outcome is losing somebody
    else's settings, and that is worth checking twice rather than trusting the
    strip regex to stay correct forever.
    """
    spec = writer_for(engine)
    if not spec.get("supported"):
        return None, spec.get("why", "unsupported")
    kwargs = {}
    if engine == "goldsrc":
        kwargs["appid"] = GOLDSRC_APPID.get(key or "", 10)
    text = spec["fn"](servers, existing, spec["slots"], **kwargs)
    lost = dropped_lines(engine, existing, text)
    if lost:
        raise WouldClobber(
            f"{engine}: merge would drop {len(lost)} line(s) that are not "
            f"ours, e.g. {lost[0].strip()!r} - refusing to render")
    if engine in _STRUCTURAL:
        _assert_structure_kept(engine, existing, text)
    return text, content_hash(text)


def _assert_structure_kept(engine, existing, text):
    """For a whole-document writer, prove nothing but `favorites` moved.

    The line-based check cannot see this, and the "history" block belongs to
    the person using the box, not to us.
    """
    if not existing.strip():
        return
    try:
        before = _vdf_parse(existing)
        after = _vdf_parse(text)
    except _VdfError as exc:
        raise WouldClobber("%s: could not verify the rewrite (%s)" % (engine, exc))

    def strip_favorites(doc):
        out = []
        for k, v in doc:
            if isinstance(v, list):
                out.append((k.lower(),
                            strip_favorites([(a, b) for a, b in v
                                             if a.lower() != "favorites"])))
            else:
                out.append((k.lower(), v))
        return out

    if strip_favorites(before) != strip_favorites(after):
        raise WouldClobber(
            "%s: the rewrite changed something outside the favourites block" % engine)


def target_path(engine, game_dir, key=None):
    """Where the rendered file goes on the box (Windows path).

    Per TITLE, not per engine: Soldier of Fortune II is a Quake III engine
    game whose directory is `base`, and every Unreal-engine game keeps its
    favourites in an ini named after itself.
    """
    pol = policy_for(key, engine) if key is not None else None
    if pol is not None:
        if not pol.get("supported"):
            return None
        subdir, filename = pol["subdir"], pol["filename"]
    else:
        spec = writer_for(engine)
        if not spec.get("supported"):
            return None
        subdir, filename = spec["subdir"], spec["filename"]
    d = game_dir.rstrip("\\/")
    tail = d.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
    if subdir and tail.lower() == subdir.lower():
        # Already inside it. The agent indexes a game by where it found the
        # executable, and for every Unreal-engine title that is System\ --
        # the same directory the ini lives in. Appending blindly produced
        # ...\System\System\UnrealTournament.ini, a path that cannot exist.
        return f"{d}\\{filename}"
    return f"{d}\\{subdir}\\{filename}" if subdir else f"{d}\\{filename}"
