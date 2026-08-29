"""favorites.py — turn a live server list into the file a game actually reads.

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


# The strip patterns, by engine, so the safety check in render() uses exactly
# the same rule the writer does rather than a second copy that can drift.
_SETA_RE = {"q3": _Q3_SETA, "q2": _Q2_SETA}


# --- registry ----------------------------------------------------------------
#
# `subdir` is where the game's autoexec lives relative to the install dir.
# `slots` is how many favourites the engine actually exposes.
WRITERS = {
    "q3": dict(fn=q3_favorites, subdir="baseq3", filename="autoexec.cfg",
               slots=16, supported=True),
    "q2": dict(fn=q2_favorites, subdir="baseq2", filename="autoexec.cfg",
               slots=9, supported=True),
    # Deliberately not implemented. Each needs a per-build answer we have not
    # verified on the fleet's actual installs, and writing a guess into a
    # game's config is worse than leaving it alone.
    "qw": dict(supported=False,
               why="classic QW has no favourites store; ezQuake's differs per build"),
    "goldsrc": dict(supported=False,
                    why="non-Steam CS 1.6 builds vary in where the server "
                        "browser keeps favourites; needs per-build verification"),
    "unreal": dict(supported=False,
                   why="UnrealTournament.ini is rewritten by a running game"),
    "ut2k4": dict(supported=False,
                  why="UT2004.ini is rewritten by a running game"),
    "t2": dict(supported=False, why="no writer implemented"),
    "rtcw": dict(supported=False, why="no writer implemented"),
    "nq": dict(supported=False, why="NetQuake has no favourites store"),
    "-": dict(supported=False, why="game has no server browser"),
}


def writer_for(engine):
    return WRITERS.get(engine, {"supported": False, "why": "unknown engine"})


class WouldClobber(Exception):
    """Rendering would drop a line the file already had.

    Raised rather than returned because there is no sensible way to continue:
    the only safe action is to leave the file alone. A caller that swallowed
    this and wrote anyway would be doing the exact thing the exception exists
    to prevent.
    """


def dropped_lines(engine, existing, text):
    """Lines present in `existing` that our output does not carry.

    Our block and stray `seta serverN` lines are *supposed* to disappear —
    they are ours to replace. Anything else going missing is a bug.

    This deliberately does NOT call `_strip_block`. Reusing it would make the
    check agree with the very function it is checking: if the strip rule ever
    became too greedy, both sides would drop the same lines and the safety net
    would report nothing. So "ours" is recomputed here from first principles —
    inside the markers, or matching the engine's favourite-line pattern.
    """
    spec = writer_for(engine)
    if not spec.get("supported"):
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


def render(engine, servers, existing=""):
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
    text = spec["fn"](servers, existing, spec["slots"])
    lost = dropped_lines(engine, existing, text)
    if lost:
        raise WouldClobber(
            f"{engine}: merge would drop {len(lost)} line(s) that are not "
            f"ours, e.g. {lost[0].strip()!r} — refusing to render")
    return text, content_hash(text)


def target_path(engine, game_dir):
    """Where the rendered file goes on the box (Windows path)."""
    spec = writer_for(engine)
    if not spec.get("supported"):
        return None
    d = game_dir.rstrip("\\/")
    return f"{d}\\{spec['subdir']}\\{spec['filename']}"
