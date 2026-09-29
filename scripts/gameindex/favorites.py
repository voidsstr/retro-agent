r"""favorites.py — turn a live server list into the file a game actually reads.

One writer per engine. A writer returns the exact bytes to place at an exact
path on the box, plus a content hash; sync.py compares what it would write
with what the box already holds and leaves the file alone when the GAME would
see no difference.

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

  * **Quake III** (baseq3's id q3_ui) - ``<dir>\baseq3\autoexec.cfg``,
    ``seta server1..16``.
  * **Quake III: Team Arena** - NOT the cvars. Team Arena's UI keeps its
    Favorites in the ENGINE's list, which ioquake3 1.36 saves to
    ``%APPDATA%\Quake3\servercache.dat`` (see the servercache section at the
    bottom; the layout is proven from the staged binary itself).
  * **Quake II** - ``<dir>\baseq2\autoexec.cfg``, ``set adr0..8``.
  * **Unreal engine 1** (UT99, Unreal Gold) - the UBrowser package's own
    bytecode carries the format as a comment::

        class UBrowserFavoritesFact extends UBrowserServerListFactory;
        var config int FavoriteCount;
        var config string Favorites[100];
        /* eg Favorites[0]=Host Name\10.0.0.1\7778\True */

    so the section is ``[UBrowser.UBrowserFavoritesFact]`` and the third field
    is the **query port**, not the game port - ``Query()`` calls
    ``FoundServer(ParseOption(..,1), Int(ParseOption(..,2)), ...)``. Each game
    keeps it in its OWN ini, which is why the target is chosen per title.
    (Deus Ex ships UBrowser.u too but never opens its favourites - see
    UNWRITABLE["deusex"].)
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
Half-Life tree is WON protocol 45 while every fleet GoldSrc server answers
protocol 48, so Half-Life must not be pointed at them.

A settled box reports "unchanged"
---------------------------------
Three things used to rewrite a file on every pass while nothing a player could
see had changed, and each one is now handled here rather than trusted away:

  * **Order.** A server that is already in the file keeps its slot
    (`_assign_slots`) or its place in the list (`_stable_sequence`); only a
    vacancy is filled. One server leaving no longer renumbers fifteen lines.
  * **Labels and whitespace.** `same_favourites` compares what the GAME ends
    up with - the servers, and every line that is not ours - so a hostname
    that changed its colour codes, or revSrvBrowser re-saving the vdf with
    its own tabs and a real `lastplayed`, is not a reason to write.
  * **Membership.** sync.py passes the servers already in the file to
    db.best_servers as incumbents; one stays while it is still alive and
    eligible, and only vacancies are filled from the ranking.

A label is never trusted
------------------------
A server's hostname is chosen by whoever runs that server. Quake II and III
split a config line into commands at every `;` that is outside double quotes -
INSIDE A `//` COMMENT TOO (measured: the staged autoexec's own comment "...on
exit; this is not." prints `Unknown command "this"` at every start). The Q3
and Q2 writers put the hostname in a comment, so one public server named
`x; quit` would have run `quit` - or anything else - on every fleet box at
every start. `clean_label` removes that, and `same_favourites` refuses to call
a file with such a line in it "unchanged".
"""
import hashlib
import re
import struct
import unicodedata
from collections import namedtuple

BEGIN = "// --- BEGIN retro-fleet favorites (managed, do not edit) ---"
END = "// --- END retro-fleet favorites ---"


def content_hash(text):
    data = text if isinstance(text, (bytes, bytearray)) else \
        str(text).encode("utf-8", "replace")
    return hashlib.sha256(bytes(data)).hexdigest()[:16]


# --- label hygiene -----------------------------------------------------------
#
# Every label reaches a file some game parses. Control characters are never
# wanted anywhere (a NUL or a newline in an ini value or a vdf string ends or
# splits it), and for the Quake family `;` and `"` are COMMAND SYNTAX even in
# a comment - see the module docstring. `'` is inert to both Cbufs and their
# tokenizers, so a double quote becomes one rather than vanishing.

_QUAKE_SYNTAX = {";": " ", '"': "'"}


def clean_label(text, limit=60, quake=False):
    """A server's own name, made safe to write where a game will read it."""
    out = []
    for ch in str(text or ""):
        if unicodedata.category(ch).startswith("C"):
            out.append(" ")                 # Cc/Cf/...: controls, NUL, DEL
        elif quake and ch in _QUAKE_SYNTAX:
            out.append(_QUAKE_SYNTAX[ch])
        else:
            out.append(ch)
    return " ".join("".join(out).split())[:limit].rstrip()


def _label_is_clean(text, quake=False):
    for ch in str(text or ""):
        if unicodedata.category(ch).startswith("C"):
            return False
        if quake and ch in _QUAKE_SYNTAX:
            return False
    return True


# What may sit between the quotes of `seta serverN "..."`. An address from a
# master is a dotted quad; ours may be a hostname if RETRO_FLEET_HOST says so.
# Nothing else - no quote, no `;`, no space - ever reaches a Quake config.
_ADDR_OK = re.compile(r"^[A-Za-z0-9.\-]+:\d{1,5}$")


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


# --- stable ordering ---------------------------------------------------------

def _assign_slots(servers, current, numbers, keyfn):
    """slot -> server, keeping every server that already HAS a slot in it.

    `current` is slot -> identity, read from the file on the box. A server
    that is still wanted keeps its exact slot; a new one takes the lowest
    free slot. So one server leaving changes one line, not every line after
    it - which is what used to make a Q3 favourites file look rewritten from
    top to bottom whenever a single internet server emptied.
    """
    want = {}
    for s in servers:
        want.setdefault(keyfn(s), s)
    out, used = {}, set()
    for n in numbers:
        k = current.get(n)
        if k and k in want and k not in used:
            out[n] = want[k]
            used.add(k)
    free = [n for n in numbers if n not in out]
    for k, s in want.items():
        if k in used or not free:
            continue
        out[free.pop(0)] = s
        used.add(k)
    return out


def _stable_sequence(servers, current, keyfn):
    """The list order for engines whose favourites are a packed array.

    Servers already in the file keep their relative order; new ones follow.
    A gap is not allowed here (UBrowser would query an empty entry), so this
    is the order-preserving counterpart of `_assign_slots`.
    """
    want = {}
    for s in servers:
        want.setdefault(keyfn(s), s)
    out, used = [], set()
    for k in current:
        if k in want and k not in used:
            out.append(want[k])
            used.add(k)
    out += [s for k, s in want.items() if k not in used]
    return out


# --- Quake III family --------------------------------------------------------

_Q3_SETA = re.compile(r"^seta\s+server\d+\s", re.IGNORECASE)
# Quake II's address book is adr0..adr8, set from the console or a config.
_Q2_SETA = re.compile(r"^set\s+adr\d+\s", re.IGNORECASE)

# A favourite line exactly as our writer produces it: the slot, the quoted
# value, and whatever follows the closing quote.
_QUAKE_LINE = {
    "q3": re.compile(r'^\s*seta\s+server(\d+)\s+"([^"]*)"(.*)$', re.IGNORECASE),
    "q2": re.compile(r'^\s*set\s+adr(\d+)\s+"([^"]*)"(.*)$', re.IGNORECASE),
}
# ...and any other spelling of one, which a game still executes.
_QUAKE_LOOSE = {
    "q3": re.compile(r"^\s*seta\s+server(\d+)\s+(\S+)", re.IGNORECASE),
    "q2": re.compile(r"^\s*set\s+adr(\d+)\s+(\S+)", re.IGNORECASE),
}
_QUAKE_FIRST_SLOT = {"q3": 1, "q2": 0}      # server1..16, adr0..8
_QUAKE_FORMAT = {"q3": ('seta server%d "%s"        // %s', 'seta server%d ""'),
                 "q2": ('set adr%d "%s"        // %s', 'set adr%d ""')}


def quake_values(engine, text):
    """slot -> value: what each favourite cvar ends up as when the file runs.

    Config lines execute top to bottom, so a later `seta server3` beats an
    earlier one wherever it sits - inside our block or not.
    """
    strict, loose = _QUAKE_LINE[engine], _QUAKE_LOOSE[engine]
    out = {}
    for line in (text or "").splitlines():
        m = strict.match(line)
        if m:
            out[int(m.group(1))] = m.group(2).strip()
            continue
        m = loose.match(line)
        if m:
            out[int(m.group(1))] = m.group(2).strip().strip('"')
    return out


def _quake_favorites(engine, servers, existing, slots):
    first = _QUAKE_FIRST_SLOT[engine]
    numbers = list(range(first, first + slots))
    line_fmt, blank_fmt = _QUAKE_FORMAT[engine]
    usable = [s for s in servers if _ADDR_OK.match(str(_field(s, "addr")))]
    assigned = _assign_slots(usable, quake_values(engine, existing), numbers,
                             lambda s: str(_field(s, "addr")))
    body = [BEGIN]
    for n in numbers:
        s = assigned.get(n)
        if s is None:
            # Blank any slot we are not using, or a stale address from a
            # previous run keeps showing up in the in-game list forever.
            body.append(blank_fmt % n)
            continue
        addr = str(_field(s, "addr"))
        label = clean_label(_field(s, "hostname") or addr, 60, quake=True) or addr
        body.append(line_fmt % (n, addr, label))
    body.append(END)
    kept = _strip_block(existing, _Q3_SETA if engine == "q3" else _Q2_SETA)
    return (kept + "\n\n" if kept.strip() else "") + "\n".join(body) + "\n"


def q3_favorites(servers, existing="", slots=16):
    """Q3 favourites are cvars server1..server16, written as seta serverN "ip:port"."""
    return _quake_favorites("q3", servers, existing, slots)


def q2_favorites(servers, existing="", slots=9):
    return _quake_favorites("q2", servers, existing, slots)


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
    return clean_label(_field(row, "hostname") or _field(row, "addr"), limit)


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


def _ini_walk(text, section):
    """(in_section, line) for every line of an ini."""
    inside = False
    for line in (text or "").splitlines():
        m = _SECTION_RE.match(line)
        if m:
            inside = m.group("name").strip().lower() == section.lower()
            yield None, line
            continue
        yield inside, line


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


# --- Unreal engine 1: UT99, Unreal Gold --------------------------------------
#
# [UBrowser.UBrowserFavoritesFact]
# FavoriteCount=1
# Favorites[0]=NSC Retro Fleet Arena (UT99)\192.168.1.132\7798\False
#
# Field order and meaning are from UBrowser.u itself, not from folklore:
# Query() reads option 1 as the IP and option 2 as the QUERY port, and
# SaveFavorites() writes `HostName\IP\QueryPort\bKeepDescription`.

_UNREAL_OWN = re.compile(r"^(FavoriteCount|Favorites\[\d+\])\s*=", re.IGNORECASE)
_UNREAL_FAV = re.compile(r"^\s*Favorites\[(\d+)\]\s*=(.*)$", re.IGNORECASE)
_UNREAL_COUNT = re.compile(r"^\s*FavoriteCount\s*=\s*(.*?)\s*$", re.IGNORECASE)
UNREAL_SECTION = "UBrowser.UBrowserFavoritesFact"


def _unreal_raw(text):
    """(FavoriteCount or None, {index: raw value}, clean) of the section.

    UE1's config reader takes the FIRST occurrence of a key, so that is what
    is recorded; a duplicate makes the file one we did not write (not clean).
    """
    count, favs, clean, sections = None, {}, True, 0
    for inside, line in _ini_walk(text, UNREAL_SECTION):
        if inside is None:
            sections += _SECTION_RE.match(line).group("name").strip().lower() \
                == UNREAL_SECTION.lower()
            continue
        if not inside:
            continue
        m = _UNREAL_COUNT.match(line)
        if m:
            if count is not None:
                clean = False
                continue
            try:
                count = int(m.group(1))
            except ValueError:
                count, clean = 0, False
            continue
        m = _UNREAL_FAV.match(line)
        if m:
            i = int(m.group(1))
            if i in favs:
                clean = False
            else:
                favs[i] = m.group(2)
    if sections != 1 or count is None:
        clean = False
    return count, favs, clean


def _unreal_entries(text):
    """(host, query port) of each favourite UBrowser will query, in order."""
    count, favs, _ = _unreal_raw(text)
    out = []
    for i in range(max(0, min(count or 0, 100))):
        parts = (favs.get(i) or "").split("\\")
        if len(parts) >= 3:
            out.append((parts[1].strip(), parts[2].strip()))
    return out


def _unreal_key(row):
    return (_split_addr(row)[0], str(_query_port(row)))


def unreal_favorites(servers, existing="", slots=16):
    picked = _stable_sequence(servers[:slots], _unreal_entries(existing),
                              _unreal_key)
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
_UT2K4_FAV = re.compile(
    r'^\s*Favorites\s*=\s*\(ServerID=(-?\d+),IP="([^"]*)",Port=(\d+),'
    r'QueryPort=(\d+),ServerName="([^"]*)"\)\s*$', re.IGNORECASE)
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


def _ut2k4_entries(text):
    """(ip, port, query port) of each favourite, in array order."""
    out = []
    for inside, line in _ini_walk(text, UT2K4_SECTION):
        if inside:
            m = _UT2K4_FAV.match(line)
            if m:
                out.append((m.group(2), m.group(3), m.group(4)))
    return out


def ut2k4_favorites(servers, existing="", slots=16):
    picked = _stable_sequence(
        servers[:slots], [(ip, port) for ip, port, _ in _ut2k4_entries(existing)],
        lambda s: (_split_addr(s)[0], str(_split_addr(s)[1])))
    body = []
    for i, s in enumerate(picked):
        host, port = _split_addr(s)
        name = _label(s).replace('"', "'")
        body.append('Favorites=(ServerID=%d,IP="%s",Port=%d,QueryPort=%d,'
                    'ServerName="%s")'
                    % (i, host, port, _ut2k4_query_port(s), name))
    # An empty array config is expressed by writing no lines at all; the
    # dropped ones are already gone, which is how a favourite is removed.
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


# revSrvBrowser.dll's per-key template: `"name"\t\t`, `"address"\t`,
# `"lastplayed"\t`, `"appID"\t\t`. A length rule alone gave "address" two tabs,
# so every time CS saved the file the next pass saw a difference that was only
# whitespace and rewrote it.
_VDF_GAP = {"address": "\t", "lastplayed": "\t", "name": "\t\t", "appid": "\t\t"}


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
            gap = _VDF_GAP.get(key.lower(), "\t\t" if len(key) < 8 else "\t")
            lines.append('%s"%s"%s"%s"' % (pad, key, gap, val))
    return lines


def _vdf_get(entry, name):
    for k, v in entry:
        if k.lower() == name.lower() and not isinstance(v, list):
            return v
    return None


def _goldsrc_favourite_entries(doc):
    """The raw entries of filters > favorites, in file order."""
    for key, val in doc:
        if key.lower() == "filters" and isinstance(val, list):
            for k2, v2 in val:
                if k2.lower() == "favorites" and isinstance(v2, list):
                    return [e for _, e in v2 if isinstance(e, list)]
    return []


def _goldsrc_entries(text):
    """(address, appID) of each favourite, in order. [] if unparseable."""
    try:
        doc = _vdf_parse(text) if (text or "").strip() else []
    except _VdfError:
        return []
    return [((_vdf_get(e, "address") or "").strip(),
             (_vdf_get(e, "appID") or "").strip())
            for e in _goldsrc_favourite_entries(doc)]


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

    picked = _stable_sequence(servers[:slots],
                              [a for a, _ in _goldsrc_entries(existing)],
                              lambda s: str(_field(s, "addr")))
    # When the game has stamped a favourite it kept, the stamp survives a
    # rewrite: it is the player's record of having played there, not ours.
    played = {}
    for e in _goldsrc_favourite_entries(doc):
        a, lp = _vdf_get(e, "address"), _vdf_get(e, "lastplayed")
        if a and lp and lp.strip().isdigit():
            played.setdefault(a.strip(), lp.strip())
    entries = []
    for i, s in enumerate(picked):
        addr = str(_field(s, "addr"))
        entries.append((str(i), [
            ("name", _label(s, 120).replace('"', "'")),
            ("address", addr),
            # Never a clock reading of OURS: that would change the file every
            # pass and rewrite every box every five minutes for no reason.
            ("lastplayed", played.get(addr, "0")),
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
    # Team Arena's favourites: a BINARY file in the player's profile, written
    # by sync.push_servercache with the sc_* functions at the bottom of this
    # module. `binary` keeps it out of the text pipeline.
    "q3cache": dict(fn=None, subdir="", filename="servercache.dat", slots=16,
                    supported=True, binary=True),
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
#     staged Half-Life tree is WON protocol 45 while every fleet GoldSrc
#     server answers protocol 48 - it cannot join them at all.
#   * UT99 and Unreal Gold share one favourites mechanism but each keeps it in
#     its own ini.
#
# `accepts` filters OUR OWN servers by the gamename they report, so a title is
# never given an address it cannot actually join. It is not applied to
# internet servers unless `strict_accepts` says so: we know exactly what runs
# on .132, and for most titles we have no reliable mod taxonomy for the rest of
# the world. (Team Arena is the exception - its game module reports
# "missionpack", and a Team Arena client cannot join a baseq3 server.)
#
# A title absent from this table is reported unsupported WITH a reason rather
# than written with a guess.
TITLES = {
    "quake3":    dict(engine="q3", subdir="baseq3", accepts={"baseq3"}),
    "ioquake3":  dict(engine="q3", subdir="baseq3", accepts={"baseq3"}),
    "openarena": dict(engine="q3", subdir="baseoa", accepts={"baseoa"}),
    # Quake III: Team Arena. The agent never reports this key: Team Arena is
    # the missionpack\ directory beside an ioquake3 install, so sync.py
    # derives it from every `ioquake3` row whose directory has one
    # (`derived_from`, `requires_subdir`). Its favourites are NOT cvars - the
    # Team Arena UI never reads server1..16 (measured on .123/.195/.240:
    # Favorites empty with them set) - but the ENGINE's favourites list,
    # which ioquake3 1.36 keeps in %APPDATA%\Quake3\servercache.dat, shared by
    # every mod. `servers_from` is the engine bucket the server table files
    # Team Arena servers under.
    "missionpack": dict(engine="q3cache", servers_from="q3", subdir="",
                        filename="servercache.dat", accepts={"missionpack"},
                        strict_accepts=True, derived_from=("ioquake3",),
                        requires_subdir="missionpack"),
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
# because not existing is its normal state. Even then only the FILE is
# created: its folder (baseq3, baseq2) must already exist. A folder that is not
# there means the title is not installed there, and making one is how
# favourites-only baseq3\ trees appeared inside Jedi Academy and SoF2.

# Every Quake III client that reads or rewrites %APPDATA%\Quake3 - the file
# Team Arena's favourites live in is shared by all of them, so ANY of these
# running makes servercache.dat busy, whichever mod it is in.
Q3_FAMILY_EXES = frozenset({"quake3.exe", "ioquake3.exe", "ioquake3.x86.exe",
                            "ioquake3-smp.x86.exe", "ioquake3_smp.x86.exe"})

# Why a running game makes its favourites file off limits - per FILE KIND, so
# the log says the true reason for the file it names.
_BUSY_WHY = {
    "q3": "it execs this file as it starts, so a write can land mid-launch",
    "q2": "it execs this file as it starts, so a write can land mid-launch",
    "unreal": "it rewrites this ini from memory on exit, so a write now would "
              "be lost or would revert what the player just set",
    "ut2k4": "it rewrites this ini from memory on exit, so a write now would "
             "be lost or would revert what the player just set",
    "goldsrc": "revSrvBrowser rewrites ServerBrowser.vdf on exit, so a write "
               "now would be lost",
    "q3cache": "Team Arena's UI rewrites servercache.dat from memory when it "
               "shuts down, so a write now would be lost - or would lose a "
               "favourite the player just added",
}


def busy_why(engine):
    return _BUSY_WHY.get(engine, "it may rewrite this file on exit")


# Directories a favourites file has no business being written into.
# The benchmark harnesses are a real copy of Quake III whose whole value is
# that nothing changes underneath them; the user stopped this service
# precisely because a foreign write mid-test makes a result unattributable.
SKIP_DIRS = re.compile(r"(^|[\\/])(q3bench|[^\\/]*bench(mark)?s?)([\\/]|$)",
                       re.IGNORECASE)

# Titles we can DETECT and deliberately do not write, each with the reason.
# This is the difference between "the favourites agent does not cover this"
# and "the favourites agent has nothing it could honestly put there", which
# are answers to different questions. Every reason here was re-checked on
# 2026-09-29 against the staged library and the fleet's game servers; a reason
# that stops being true (a server appears, a mounter is installed) must be
# rewritten in the same change, or this table starts telling people to give up
# on things that work.
UNWRITABLE = {
    "sam":
        "there ARE fleet Serious Sam servers now (ssam-tfe-server :25600 and "
        "ssam-tse-server :25610 on .132) but Serious Engine 1 keeps no "
        "favourites file - its browser is a live GameSpy/LAN enumeration and "
        "there is nothing on disk to write. The LAN tab finds them by "
        "broadcast; a direct join is Join Game -> type the address",
    "halflife":
        "two trees report this key. The WON Half-Life tree (HalfLife1, hl.exe "
        "1.0.1.4) speaks protocol 45 - its hw.dll compares the server's "
        "protocol with 45 (cmp eax,2Dh) and refuses anything else - and every "
        "fleet GoldSrc server answers protocol 48, so listing them there would "
        "be a list of dead entries. The Half-Life DM mod inside the "
        "CounterStrike16 tree IS protocol 48 and does join hldm-server "
        "(192.168.1.132:27020, two boxes verified 2026-08-31, 'Play Half-Life "
        "Deathmatch.bat'), but that tree has ONE config\\serverbrowser.vdf "
        "shared with Counter-Strike and nobody has verified that revSrvBrowser "
        "keeps an appID 70 entry apart from CS's, so nothing is written for it "
        "either: use the launcher or `connect 192.168.1.132:27020`",
    "sof2":
        "there IS a fleet SoF2 server (sof2-server, 192.168.1.132:20100, "
        "verified two-box 2026-08-31) but SoF2's browser keeps its favourites "
        "in the ENGINE's list - sof2mp.exe names servercache.dat, its MP UI "
        "(base\\mp.pk3 vm/sof2mp_ui.qvm) has addFavorite and no server%d, and "
        "the game writes the file at its tree root on the boxes (none is "
        "staged) - not in cvars a config "
        "could set, and that file is a binary struct dump whose layout nobody "
        "has verified for sof2mp.exe. Writing a guess into it is worse than "
        "this sentence. Join it from the console: connect "
        "192.168.1.132:20100. NB its Quake III lineage still does NOT mean it "
        "can join a Quake III server",
    "jka":  "there IS a fleet Jedi Academy server (jka-server, "
            "192.168.1.132:29070, OpenJK, protocol 26) and the staged tree CAN "
            "join it now: the launcher mounts the staged disc image, and .143 "
            "and .246 joined it together on 2026-08-31 (JediAcademy/"
            "README-FLEET.txt; only a box with no disc mounter is gated off). "
            "Nothing is written because JKA's browser keeps favourites in the "
            "ENGINE's list - jamp.exe names servercache.dat, and its UI has "
            "addFavorite and no server%d cvars - a binary struct dump whose "
            "layout is unverified for jamp.exe. Join from the console: "
            "connect 192.168.1.132:29070 (the menus are relative-mouse; run "
            "windowed to type)",
    "jk2":  "no Jedi Knight II server on the fleet and no live JK2 master",
    "et":   "no Enemy Territory server on the fleet and no live ET master",
    "mohaa":
        "no MOHAA server on the fleet; note MOHAA also needs the framed "
        "\\xff\\xff\\xff\\xff\\x02getinfo\\x00 query, not Quake III's getstatus",
    "quake":  "NetQuake keeps no favourites store - the engine has no such "
              "cvar block, so there is nowhere to write. The fleet DOES have "
              "a NetQuake server as of 2026-08-31 (quake1-server, port 26000, "
              "verified two-box): join it from the console with "
              "`connect 192.168.1.132`. The QuakeWorld server on 27502 remains "
              "unjoinable from a NetQuake client - different protocol",
    "quakeworld": "classic QW keeps no favourites; the fleet server is reached "
                  "from the console with `connect 192.168.1.132:27502`",
    "ezquake": "ezQuake's favourites file differs per build; unverified here",
    # Return to Castle Wolfenstein multiplayer. This key HAD a writer - seta
    # server1..16 into Main\autoexec.cfg - and every pass reported it written.
    # It never reached the game (below), so it is here now instead.
    "wolfmp":
        "RTCW multiplayer's browser keeps its Favorites in the ENGINE's list, "
        "saved to <install>\\servercache.dat: the staged Main\\ui_mp_x86.dll "
        "is Team Arena's UI lineage (addFavorite, createFavorite, "
        "ui_favoriteAddress) and has no 'server%d' anywhere - the server1..16 "
        "cvars it registers are never read. Proven on the boxes: "
        "servercache.dat held favourites=0 after sessions whose config "
        "carried server1=192.168.1.132:27963, so the autoexec writer this key "
        "used to have was a false success. That file's layout is unverified "
        "for WolfMP.exe, so nothing is written. Join the fleet server "
        "(rtcw-server, 192.168.1.132:27963) from the console: connect "
        "192.168.1.132:27963 (the menus are relative-mouse), or from the LAN "
        "tab - the Q3-engine LAN scan covers ports 27960-27963",
    # Deus Ex HAD a writer too ([UBrowser.UBrowserFavoritesFact] in
    # DeusEx.ini), reported "unchanged" on every pass, and read by nothing.
    "deusex":
        "Deus Ex has no favourites screen: its join menus are "
        "MenuScreenJoinInternet (DeusExGSpyLink to the dead GameSpy master) "
        "and MenuScreenJoinLan (DeusExLocalLink broadcast), and no class in "
        "DEUSEX.U references UBrowserFavoritesFact - so the favourites block "
        "this key used to write into DeusEx.ini had no reader. Join the fleet "
        "server (deusex-server, 192.168.1.132:7790) by typing it into the join "
        "screen's IP Address box, or `open 192.168.1.132:7790` at the console",
    "tribes2":
        "Tribes 2 DOES keep favourites - $pref::ServerBrowser::Favorite[N] = "
        "name TAB address, from GameGui.cs in scripts.vl2 - but no writer is "
        "built: nobody has verified on hardware that a favourite is queried "
        "and joinable without the TribesNext master (which also encrypts the "
        "info response), and Torque rewrites its prefs from memory when the "
        "game exits. The fleet server is tribes2-server (docker), "
        "192.168.1.132:28000",
    "farcry":
        "Far Cry keeps favourites (Profiles\\server\\fav_server.cfg, "
        "UI.PageMultiplayer.FavServers[n] = \"ip:port\", FCData\\Scripts.pak) "
        "but joining from its list goes through the Ubi.com client first: "
        "PrepareToJoin returns unless UBIGameServers[ip] exists, and only the "
        "dead Ubisoft master fills that. No writer until a box shows a "
        "favourite can actually be joined. The fleet server is farcry-server "
        "on 192.168.1.132, UDP 49001",
    "doom3":
        "DOOM 3 has no favourites UI - 'favorit' is in neither DOOM3.exe nor "
        "any main or multiplayer menu in the staged paks (case-insensitive) - "
        "so there is nothing to write. The fleet server (doom3-server, "
        "192.168.1.132:27666) is joined from the console: "
        "connect 192.168.1.132:27666",
    "bf1942":
        "Battlefield 1942 does keep favourites (BF1942.exe names "
        "ServerListFavorites.dat and has AddFavorite/RemFavorite; the file's "
        "format is unverified) but there is no fleet BF1942 server to put in "
        "it: LAN games are box to box - 'Host Battlefield 1942 - LAN' on one "
        "machine, 'Join Battlefield 1942 - LAN' on the others (the host's "
        "address comes from C:\\Games\\lanhost.txt or a prompt)",
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
    "descent3": "Descent 3 keeps no favourites store - 'favorit' is in none of "
                "its binaries or connection modules (online\\*.d3c, "
                "case-insensitive) - and PXO, its tracker, is long dead. The "
                "fleet server (descent3-server, 192.168.1.132:2092) is joined "
                "by the staged 'Join Descent 3 - LAN.bat', which uses that "
                "address unless lanhost.txt names another",
    "redfaction": "Red Faction's tracker is dead; LAN games are found by "
                  "broadcast, with no favourites file",
    "shogo":   "there IS a fleet Shogo server now (shogo-server, "
               "192.168.1.132:27888, ShogoSrv 2.2 under Wine, since "
               "2026-09-01) but Shogo keeps no favourites file - its browser "
               "is a live GameSpy/LAN enumeration and there is nothing on "
               "disk to write. Join it from Multiplayer -> the address box",
    "avp":     "Aliens versus Predator uses its own dead lobby; LAN by broadcast",
    "starcraft": "StarCraft LAN is UDP broadcast - no server list",
    "sshock2": "System Shock 2 co-op joins by typed address",
    "carmageddon": "Carmageddon 1 is DOSBox/IPX - no server list",
    "carmageddon2": "Carmageddon 2 uses IPXWrapper over the LAN - no server list",
    "redneck": "Redneck Rampage is DOSBox/IPX - no server list",
    "hexen2":  "Hexen II LAN play is the Quake-family broadcast search and the "
               "engine keeps no favourites store. Verified two-box 2026-08-31: "
               "the staged Host/Join launchers work and the joiner's Search "
               "lists the host. There is no fleet Hexen II server to list "
               "either - uhexen2's h2ded refuses the staged retail 1.03 data",
    "heretic": "Heretic is a DOS Doom-engine game: network play is IPX, serial "
               "or modem, set up before launch - there is no server browser, "
               "nothing on disk that holds favourites, and no Heretic server "
               "on the fleet",
    "hexen":   "Hexen is a DOS Doom-engine game: network play is IPX, serial "
               "or modem, set up before launch - there is no server browser, "
               "nothing on disk that holds favourites, and no Hexen server on "
               "the fleet",
    "hd":      "Hidden & Dangerous joins by typed address; no fleet server",
    "jk":      "Jedi Knight: Dark Forces II is DirectPlay LAN - no server list "
               "and no dedicated server on any platform. Verified two-box "
               "2026-08-31: Host Game on one box, Join Game -> TCP/IP -> blank "
               "Locate Session on the other, and the broadcast finds it",
    "jkmots":  "Mysteries of the Sith is DirectPlay LAN - no server list, same "
               "Sith engine as Dark Forces II. Verified two-box 2026-08-31",
    # Quake II engine, but that buys nothing without a server of their own:
    # a SiN or SoF client cannot join a Quake II server.
    "sin":     "SiN is Quake II engine but speaks its own game and keeps no "
               "favourites store; no fleet SiN server (Ritual never shipped a "
               "Linux one). Verified two-box 2026-08-31 box-to-box: one box "
               "runs ds_deathmatch.bat or a listen server, the rest use "
               "`sin.exe +connect <host ip>`",
    "sof":     "Soldier of Fortune is Quake II engine but speaks its own game; "
               "no fleet SoF server and none is possible - Loki's Linux port "
               "topped out at 1.06a and a standalone sofded never existed. Its "
               "multiplayer also wants the SOF CD in a drive (WON Error!), so "
               "there is nothing an address list could fix",
    # Single-player titles that reached the library for other reasons.
    "thief":   "Thief: The Dark Project is single-player - there is no "
               "multiplayer to have favourites for",
    "thief2":  "Thief II is single-player - there is no multiplayer to have "
               "favourites for",
    "sshock":  "System Shock 1 is single-player - there is no multiplayer "
               "to have favourites for",
    "hl2":     "the fleet's Source server (css-server, 192.168.1.132:27025, "
               "Counter-Strike: Source) is for a modern Steam client on the "
               "LAN - scripts/game-servers/gameservers.py records that no "
               "fleet box can join it (a CS:S client needs Steam, which no "
               "longer runs on XP, Vista or 7) - and no favourites writer has "
               "been verified for any Source build here, so nothing is written",
    "diablo2": "Diablo II LAN is UDP broadcast; battle.net is not ours",
    "wolfsp":  "the RtCW single-player executable. WolfSP.exe has no server "
               "browser and no multiplayer at all; the fleet's RTCW server "
               "(rtcw-server, 192.168.1.132:27963, since 2026-09-01) is for "
               "`wolfmp`, which has its own answer",
}


def engines_for_keys(keys):
    """The server-table engines these installed titles need lists for.

    The agent reports some titles with engine "-", because from the box's
    point of view they have no server browser worth naming. The host knows
    better, and this is what makes sure the server table covers every engine
    a writer will ask it about.
    """
    out = set()
    for k in keys:
        t = TITLES.get(k)
        if t is not None:
            out.add(t.get("servers_from", t["engine"]))
    return sorted(out)


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
    out.setdefault("servers_from", t["engine"])
    out.setdefault("strict_accepts", False)
    out.setdefault("local_only", False)
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
    if not spec.get("supported") or engine in _STRUCTURAL or spec.get("binary"):
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
    if spec.get("binary"):
        return None, "%s is a binary store - sync.push_servercache writes it" \
            % spec.get("filename", engine)
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


def _strip_favourites(doc):
    out = []
    for k, v in doc:
        if isinstance(v, list):
            out.append((k.lower(),
                        _strip_favourites([(a, b) for a, b in v
                                           if a.lower() != "favorites"])))
        else:
            out.append((k.lower(), v))
    return out


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

    if _strip_favourites(before) != _strip_favourites(after):
        raise WouldClobber(
            "%s: the rewrite changed something outside the favourites block" % engine)


def target_path(engine, game_dir, key=None):
    """Where the rendered file goes on the box (Windows path).

    Per TITLE, not per engine: Soldier of Fortune II is a Quake III engine
    game whose directory is `base`, and every Unreal-engine game keeps its
    favourites in an ini named after itself. None for a binary store that
    does not live under the game at all (Team Arena's servercache.dat is in
    the player's profile - sync.push_servercache resolves it on the box).
    """
    pol = policy_for(key, engine) if key is not None else None
    if pol is not None:
        if not pol.get("supported") or writer_for(pol["engine"]).get("binary"):
            return None
        subdir, filename = pol["subdir"], pol["filename"]
    else:
        spec = writer_for(engine)
        if not spec.get("supported") or spec.get("binary"):
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


# --- a settled box reports "unchanged" ---------------------------------------
#
# `identity` is how a server is recognised in a file of each kind - the thing
# that must match for a favourite already on the box to count as "the same
# server". Unreal engine 1 records the QUERY port, so that is its identity.

def identity(engine, row):
    host, port = _split_addr(row)
    if engine == "unreal":
        return "%s:%d" % (host, _query_port(row))
    return "%s:%d" % (host, port)


def incumbents(engine, existing):
    """The identities of the favourites the file on the box already holds."""
    if not existing:
        return set()
    if engine in _QUAKE_LINE:
        return {v for v in quake_values(engine, existing).values() if v}
    if engine == "unreal":
        return {"%s:%s" % (h, q) for h, q in _unreal_entries(existing)}
    if engine == "ut2k4":
        return {"%s:%s" % (ip, port) for ip, port, _ in _ut2k4_entries(existing)}
    if engine == "goldsrc":
        return {a for a, _ in _goldsrc_entries(existing) if a}
    return set()


View = namedtuple("View", "rest entries extra clean")


def _quake_view(engine, text):
    """What the GAME gets from a Quake config: the final favourite values, the
    lines that are not favourites, and whether every favourite line is one
    our writer could have produced (quoted address, clean comment, inside
    exactly one BEGIN/END block)."""
    strict, loose = _QUAKE_LINE[engine], _QUAKE_LOOSE[engine]
    final, rest, clean, inside = {}, [], True, False
    begins = ends = 0
    for line in text.splitlines():
        s = line.strip()
        if s == BEGIN:
            begins += 1
            inside, clean = True, clean and not inside
            continue
        if s == END:
            ends += 1
            clean = clean and inside
            inside = False
            continue
        m = strict.match(line)
        if m:
            slot, value, tail = int(m.group(1)), m.group(2).strip(), m.group(3)
            final[slot] = value
            comment = tail.strip()
            if not inside or (value and not _ADDR_OK.match(value)):
                clean = False
            elif comment and not (comment.startswith("//") and
                                  _label_is_clean(comment[2:], quake=True)):
                clean = False
            continue
        m = loose.match(line)
        if m:
            final[int(m.group(1))] = m.group(2).strip().strip('"')
            clean = False                  # a favourite in a shape we never write
            continue
        if inside:
            if s:
                clean = False              # something foreign inside our block
            continue
        rest.append(line.rstrip())
    while rest and not rest[-1]:
        rest.pop()
    if begins != 1 or ends != 1 or inside:
        clean = False
    entries = tuple(sorted(v for v in final.values() if v))
    return View(tuple(rest), entries, frozenset(final), clean)


def _unreal_view(text):
    count, favs, clean = _unreal_raw(text)
    if count is not None and not 0 <= count <= 100:
        # UBrowserFavoritesFact declares Favorites[100]; anything else is a
        # file we did not write, and not one to reason about entry by entry.
        count, clean = max(0, min(count, 100)), False
    rest = []
    for inside, line in _ini_walk(text, UNREAL_SECTION):
        if inside and _UNREAL_OWN.match(line.strip()):
            continue
        rest.append(line.rstrip())
    while rest and not rest[-1]:
        rest.pop()
    entries = []
    for i in range(max(0, count or 0)):
        raw = favs.get(i)
        parts = (raw or "").split("\\")
        if raw is None or len(parts) != 4 or not _label_is_clean(raw):
            clean = False
            continue
        entries.append((parts[1].strip(), parts[2].strip()))
    return View(tuple(rest), tuple(sorted(entries)), None, clean)


def _ut2k4_view(text):
    rest, entries, clean, sections = [], [], True, 0
    for inside, line in _ini_walk(text, UT2K4_SECTION):
        if inside is None:
            sections += _SECTION_RE.match(line).group("name").strip().lower() \
                == UT2K4_SECTION.lower()
        if inside and _UT2K4_OWN.match(line.strip()):
            m = _UT2K4_FAV.match(line)
            if not m or not _label_is_clean(m.group(5)):
                clean = False
                continue
            entries.append((m.group(2), m.group(3), m.group(4)))
            continue
        rest.append(line.rstrip())
    while rest and not rest[-1]:
        rest.pop()
    if sections != 1:
        clean = False
    return View(tuple(rest), tuple(sorted(entries)), None, clean)


def _goldsrc_view(text):
    doc = _vdf_parse(text)                # raises: not a file we can reason about
    clean, entries = True, []
    for e in _goldsrc_favourite_entries(doc):
        addr, appid = _vdf_get(e, "address"), _vdf_get(e, "appID")
        if addr is None or appid is None or \
                not all(_label_is_clean(v) for _, v in e if not isinstance(v, list)):
            clean = False
            continue
        entries.append((addr.strip(), appid.strip()))
    return View(repr(_strip_favourites(doc)), tuple(sorted(entries)), None, clean)


_VIEWS = {"q3": lambda t: _quake_view("q3", t),
          "q2": lambda t: _quake_view("q2", t),
          "unreal": _unreal_view, "ut2k4": _ut2k4_view, "goldsrc": _goldsrc_view}


def same_favourites(engine, existing, text):
    """Would writing `text` over `existing` change anything the GAME sees?

    False means "write it". True when the two are line-for-line the same, or
    when they hold the same servers and every line that is not a favourite is
    identical, and the file on the box is one our writer could have produced.
    Order, labels, whitespace and a `lastplayed` the game stamped itself are
    not differences worth an upload - they were what rewrote files on every
    pass while nothing a player could see had changed.

    The `clean` requirement is what keeps this from hiding a real problem:
    an unsafe label (a `;` in a Quake comment), a favourite written in some
    other shape, or a file with two of our blocks is always rewritten.
    """
    if (existing or "").splitlines() == text.splitlines():
        return True
    view = _VIEWS.get(engine)
    if view is None or not (existing or "").strip():
        return False
    try:
        a, b = view(existing), view(text)
    except (_VdfError, ValueError):
        return False
    return (a.clean and b.clean and a.rest == b.rest
            and a.entries == b.entries and a.extra == b.extra)


# --- Quake III: Team Arena -- ioquake3 1.36's servercache.dat -----------------
#
# Team Arena's UI (missionpack) never reads the server1..16 cvars. Its Favorites
# tab is the ENGINE's favourites list: _UI_Init calls trap_LAN_LoadCachedServers
# and _UI_Shutdown trap_LAN_SaveCachedServers (ioq3 code/ui/ui_main.c), and the
# engine keeps the list in <fs_homepath>\servercache.dat - for ioquake3 1.36 on
# Windows that is %APPDATA%\Quake3\servercache.dat, one file for every mod.
#
# The layout was PROVEN, not assumed, against both the ioq3 source at the 1.36
# release point and the staged ioquake3.x86.exe itself (md5 12f99bc0...; see
# .claude/evidence-1080p/build-favourites/servercache-layout-verification.txt):
#
#   int numglobalservers; int numfavoriteservers; int size (== 692736)
#   serverInfo_t globalServers[4096]; serverInfo_t favoriteServers[128]
#
#   serverInfo_t (164 bytes)          netadr_t (32 bytes)
#     +0   netadr_t adr                 +0  int   type   (NA_IP == 4)
#     +32  char hostName[32]            +4  byte  ip[4]
#     +64  char mapName[32]             +8  byte  ip6[16]
#     +96  char game[32]                +24 u16   port   (network order)
#     +128 int netType, gameType,       +28 u32   scope_id
#          clients, maxClients,
#          minPing, maxPing, ping       LAN_LoadCachedServers: cmp size,0xa9200
#     +156 qboolean visible             then 0xa4000 + 0x5200 bytes;
#     +160 int punkbuster               CL_SetServerInfo: hostName +0x20 ...
#
# Nothing checks numglobal/numfav on load, so this code refuses any file whose
# counts are out of range rather than hand the engine an out-of-bounds read.
#
# WHICH ENTRIES ARE OURS: an entry we add carries SC_MARK in netadr_t.ip6. For
# an NA_IP address that field is never read - NET_CompareAdr compares type, the
# four ip bytes and the port only (disassembled: `cmp ecx,4` -> repz cmpsb of 4
# bytes), and the engine itself leaves stack garbage there when a player adds a
# favourite (LAN_AddServer's netadr_t is uninitialised). The game copies the
# whole struct when it saves, so the mark survives the game's own rewrite and
# a favourite the PLAYER added (no mark) is never removed by this code.

SC_GLOBAL = 4096                       # MAX_GLOBAL_SERVERS
SC_OTHER = 128                         # MAX_OTHER_SERVERS (the favourites)
SC_REC = 164                           # sizeof(serverInfo_t)
SC_SIZE = (SC_GLOBAL + SC_OTHER) * SC_REC      # 692736, the header's size field
SC_LEN = 12 + SC_SIZE                          # 692748, the whole file
SC_FAV_OFF = 12 + SC_GLOBAL * SC_REC           # 671756
SC_NA_IP = 4
SC_MARK = b"retro-fleet-fav\x00"
assert len(SC_MARK) == 16

_IPV4 = re.compile(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$")


class ServerCacheError(ValueError):
    """The file is not ioquake3 1.36's servercache.dat - do not touch it."""


def sc_parse(data):
    """(numglobal, [favourite records]) of a servercache.dat, or raise."""
    if len(data) != SC_LEN:
        raise ServerCacheError("%d bytes, expected %d" % (len(data), SC_LEN))
    nglobal, nfav, size = struct.unpack_from("<iii", data, 0)
    if size != SC_SIZE:
        raise ServerCacheError("size field %d, expected %d" % (size, SC_SIZE))
    if not 0 <= nglobal <= SC_GLOBAL:
        raise ServerCacheError("numglobalservers %d out of range" % nglobal)
    if not 0 <= nfav <= SC_OTHER:
        raise ServerCacheError("numfavoriteservers %d out of range" % nfav)
    favs = [bytes(data[SC_FAV_OFF + i * SC_REC:SC_FAV_OFF + (i + 1) * SC_REC])
            for i in range(nfav)]
    return nglobal, favs


def sc_addr(rec):
    """'a.b.c.d:port' for an NA_IP favourite, else None."""
    if struct.unpack_from("<i", rec, 0)[0] != SC_NA_IP:
        return None
    port = struct.unpack_from(">H", rec, 24)[0]
    return "%d.%d.%d.%d:%d" % (rec[4], rec[5], rec[6], rec[7], port)


def sc_is_ours(rec):
    return sc_addr(rec) is not None and bytes(rec[8:24]) == SC_MARK


def sc_hostname(rec):
    return bytes(rec[32:64]).split(b"\0", 1)[0].decode("latin-1")


def sc_record(addr, label):
    """One favourite exactly as LAN_AddServer makes it on a zeroed slot -
    address, hostName and visible=qtrue, everything else zero - plus our mark."""
    m = _IPV4.match(str(addr))
    if not m or any(int(x) > 255 for x in m.groups()[:4]) or \
            not 0 < int(m.group(5)) < 65536:
        raise ValueError("not an IPv4 address with a port: %r" % (addr,))
    rec = bytearray(SC_REC)
    struct.pack_into("<i", rec, 0, SC_NA_IP)
    rec[4:8] = bytes(int(x) for x in m.groups()[:4])
    rec[8:24] = SC_MARK
    struct.pack_into(">H", rec, 24, int(m.group(5)))
    name = clean_label(label, 31).encode("latin-1", "replace")[:31]
    rec[32:32 + len(name)] = name
    struct.pack_into("<i", rec, 156, 1)            # visible = qtrue
    return bytes(rec)


def sc_our_addresses(data):
    """Addresses of the favourites WE put in this file (marked)."""
    if not data:
        return set()
    try:
        _, favs = sc_parse(data)
    except ServerCacheError:
        return set()
    return {sc_addr(r) for r in favs if sc_is_ours(r)}


def sc_merge(existing, servers):
    """Put `servers` into the favourites of a servercache.dat, touching nothing else.

    `existing` is the file's bytes, or None when there is no file yet. Returns
    (new bytes, summary) - new is None when the file already says exactly
    this, which is what "unchanged" means here: the same entries, ours marked,
    in the same order - or, with no file yet, when there is nothing of ours to
    put in one. hostName and the ping fields are the GAME's to update, so they
    are never a reason to write. A wanted server that could not be added is
    in summary["full"] / summary["skipped"]; `sc_missing` words it.

    Preserved byte for byte: the header's numglobalservers, all 4096 global
    records, and every favourite we did not add. Ours that are no longer
    wanted are removed; wanted ones not yet present are appended (the player's
    entries keep their places). A server the player already added by hand
    counts as present - it is not duplicated and not marked.
    """
    if existing is None:
        nglobal, favs = 0, []
        globals_blob = bytes(SC_GLOBAL * SC_REC)
    else:
        nglobal, favs = sc_parse(existing)
        globals_blob = bytes(existing[12:SC_FAV_OFF])

    wanted, skipped = [], []
    for s in servers:
        addr = str(_field(s, "addr"))
        m = _IPV4.match(addr)
        # The engine stores four address BYTES; a hostname or a malformed
        # address has no representation here and is reported, not guessed.
        if not m or any(int(x) > 255 for x in m.groups()[:4]) or \
                not 0 < int(m.group(5)) < 65536:
            skipped.append(addr)
            continue
        if addr not in [a for a, _ in wanted]:
            wanted.append((addr, _field(s, "hostname") or addr))
    want_addrs = {a for a, _ in wanted}

    kept, present, dropped = [], set(), []
    for rec in favs:
        a = sc_addr(rec)
        if sc_is_ours(rec) and (a not in want_addrs or a in present):
            dropped.append(a)
            continue
        kept.append(rec)
        if a:
            present.add(a)
    added, full = [], []
    for addr, label in wanted:
        if addr in present:
            continue
        if len(kept) >= SC_OTHER:
            full.append(addr)
            continue
        kept.append(sc_record(addr, label))
        present.add(addr)
        added.append(addr)

    ours = [sc_addr(r) for r in kept if sc_is_ours(r)]
    summary = {
        "favourites": [sc_addr(r) or "(non-IPv4)" for r in kept],
        "wanted": [a for a, _ in wanted],
        "ours": ours, "added": added, "dropped": dropped, "full": full,
        "skipped": skipped, "theirs": len(kept) - len(ours),
        "numglobal": nglobal,
        "hash": content_hash("|".join(
            "%s%s" % ("*" if sc_is_ours(r) else "", sc_addr(r) or r[:32].hex())
            for r in kept)),
    }
    # Nothing to add and nothing to take away: the file on the box (or its
    # absence) already says it. With no file yet that also means NOT creating
    # one - an empty servercache.dat seeded for a list we could not fill (a
    # hostname address, say) would be a write that put nothing of ours there.
    if not added and not dropped:
        return None, summary
    fav_blob = b"".join(kept) + bytes(SC_REC * (SC_OTHER - len(kept)))
    new = struct.pack("<iii", nglobal, len(kept), SC_SIZE) + globals_blob + fav_blob
    assert len(new) == SC_LEN
    return new, summary


def sc_missing(summary):
    """Why a server we wanted is NOT in the list after the merge, or "".

    Never "unchanged" and never a plain "wrote": a list that is full of the
    player's own 128 favourites, or an address this file cannot hold, leaves
    the fleet server out - and a report that says "unchanged" there is the
    project's recurring shape, a tool reporting success while the thing it
    exists to do did not happen.
    """
    out = []
    if summary.get("full"):
        out.append("NOT ADDED %s - the favourites list is full (%d entries, "
                   "the player's own)" % (", ".join(summary["full"]), SC_OTHER))
    if summary.get("skipped"):
        out.append("NOT ADDED %s - not an IPv4 address:port, and that is all "
                   "servercache.dat can hold" % ", ".join(summary["skipped"]))
    return "; ".join(out)


def sc_listing(summary):
    """Which wanted servers the list holds, and whose entry each one is."""
    ours = set(summary.get("ours") or ())
    present = set(summary.get("favourites") or ())
    theirs = [a for a in summary.get("wanted") or () if a in present and a not in ours]
    parts = []
    if summary.get("ours"):
        parts.append("ours: " + ", ".join(summary["ours"]))
    if theirs:
        parts.append("already the player's own: " + ", ".join(theirs))
    return "; ".join(parts) or "none of ours"
