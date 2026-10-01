#!/usr/bin/env python3
"""Pure helpers for the library files several generators CO-OWN.

A staged title's launch.txt and requires.json are touched by more than one
generator: stage-fleetres.py pins launch.txt's first data line for some titles,
stage-win9x.py adds the Windows 9x launchers, and a person edits the notes. So
none of them may rewrite those files from a template - a template would undo
the others. Every change here is a MERGE that touches only the rows and rules
the caller names, and applying the same merge to its own output changes nothing
(the generators' --check depends on that).

Also here: the COMMAND.COM dialect check every Windows 9x launcher this repo
generates must pass. Win98's COMMAND.COM is not cmd.exe, and every one of these
rules is a launcher that silently did nothing on .243:

  * CRLF line ends, ASCII, every line at most 126 bytes (COMMAND.COM's buffer);
  * no %~dp0 / %~1 (cmd.exe), no `cd /d`, no `start "title"`, no setlocal,
    no `( ... )` blocks, no `&&` / `||`, no `2>` redirection;
  * no < > | in a REM line - COMMAND.COM parses redirection there too, so
    `rem a -> b` creates a file named b - and no % in a REM line either;
  * no parentheses at all (house rule: a generated value never carries one).

Lines inside an NT-ONLY region are exempt from the dialect rules (still CRLF,
ASCII, <= 126 bytes): the region opens at a line
    if not exist %windir%\\system32\\reg.exe goto <label>
and closes at `:<label>`. COMMAND.COM jumps over it on Win98 without parsing
it, which is how one launcher serves .243 and a Voodoo 4/5 XP box (measured:
Quake's and Hexen II's Voodoo launchers, 2026-09-28).
"""
import json
import re

LAUNCH_TXT_READ_LIMIT = 1023          # agent/src/gamesync.c reads sizeof(buf)-1
COMMAND_COM_LINE_MAX = 126
NT_GUARD_RE = re.compile(r'^if not exist %windir%\\system32\\reg\.exe goto (\S+)$', re.I)


# --------------------------------------------------------------------------
# launch.txt
# --------------------------------------------------------------------------

def _nl(text):
    return "\r\n" if "\r\n" in text or not text else "\n"


def is_data_line(line):
    s = line.strip()
    return bool(s) and not s.startswith("#")


def launch_rows(text):
    """[(target, display, icon)] for every data line, in file order."""
    out = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if is_data_line(line):
            parts = line.split("\t")
            parts += [""] * (3 - len(parts))
            out.append(tuple(p.strip() for p in parts[:3]))
    return out


def set_launch_rows(text, rows, drop=(), note=None):
    """launch.txt with `rows` present and the targets in `drop` gone.

    * a row whose TARGET (first column, compared case-insensitively - it is a
      Windows path) is already there is rewritten IN PLACE;
    * a new row goes directly after the last data line, in the order given,
      so the data stays above the comments (the agent reads 1023 bytes);
    * every other line - other generators' rows, every comment - is kept.
    `note` (a list of comment lines) is appended at the end once: its first
    line with any text beyond '#' is its marker, and a file that already
    carries the marker is not given a second copy."""
    nl = _nl(text)
    lines = text.replace("\r\n", "\n").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    drop_l = {d.lower() for d in drop}
    want = {r[0].lower(): "\t".join(r) for r in rows}
    seen = set()
    out = []
    last_data = -1
    for line in lines:
        if is_data_line(line):
            tgt = line.split("\t")[0].strip().lower()
            if tgt in drop_l and tgt not in want:
                continue
            if tgt in want:
                if tgt in seen:
                    continue                    # a duplicate row - keep one
                line = want[tgt]
                seen.add(tgt)
            out.append(line)
            last_data = len(out) - 1
        else:
            out.append(line)
    new = [want[r[0].lower()] for r in rows if r[0].lower() not in seen]
    out[last_data + 1:last_data + 1] = new
    if note:
        marker = next((l.strip() for l in note if l.strip().strip("#").strip()), None)
        if marker is None:
            raise ValueError("a launch.txt note needs a line with text in it")
        if not any(l.strip() == marker for l in out):
            out += list(note)
    return nl.join(out) + nl


def launch_problems(text):
    """Why the agent would lose a shortcut from this launch.txt, or []."""
    probs = []
    pos = last_end = 0
    for line in text.splitlines(True):
        if is_data_line(line):
            last_end = pos + len(line.encode("latin-1"))
        pos += len(line.encode("latin-1"))
    if last_end > LAUNCH_TXT_READ_LIMIT:
        probs.append("a data line ends at byte %d, past the agent's %d-byte read"
                     % (last_end, LAUNCH_TXT_READ_LIMIT))
    for tgt, disp, icon in launch_rows(text):
        if "(" in tgt or ")" in tgt:
            probs.append("%r: parentheses in a target" % tgt)
        bad = [c for c in '\\/:*?"<>|' if c in disp]
        if bad:
            probs.append("%r: display name carries %s" % (disp, "".join(bad)))
        if not icon:
            probs.append("%r: no explicit icon" % tgt)
    return probs


# --------------------------------------------------------------------------
# requires.json
# --------------------------------------------------------------------------

def merge_requires(text, version=None, set_shortcuts=None, patch_shortcuts=None,
                   drop_shortcuts=(), set_top=None, drop_top=(), notes_add=None):
    """requires.json with the named changes and nothing else.

    set_shortcuts   {target: rule}  the rule is REPLACED whole (the caller owns it)
    patch_shortcuts {target: {k: v}} keys merged into an existing rule
                                     (someone else owns the rest of it)
    drop_shortcuts  [target]         rules removed - a rule for a target that is
                                     not in launch.txt can never fire, and
                                     `gamegate.py lint` FAILS it
    set_top/drop_top                 title-level keys
    notes_add                        a sentence appended to "notes" once
    version                          requirements_version (the host verdict
                                     cache is keyed on it - bump it whenever a
                                     rule changes)
    Output is json.dumps(indent=2) + newline, key order preserved."""
    doc = json.loads(text) if text and text.strip() else {}
    if version is not None:
        doc["requirements_version"] = version
    for k, v in (set_top or {}).items():
        doc[k] = v
    for k in drop_top:
        doc.pop(k, None)
    if notes_add:
        notes = doc.get("notes", "")
        if notes_add.strip() not in notes:
            doc["notes"] = (notes.rstrip() + " " + notes_add.strip()).strip()
    sc = doc.get("shortcuts")
    if set_shortcuts or patch_shortcuts or drop_shortcuts:
        sc = dict(sc or {})

        def key_for(name):
            return next((k for k in sc if k.lower() == name.lower()), name)
        for name in drop_shortcuts:
            sc.pop(key_for(name), None)
        for name, rule in (set_shortcuts or {}).items():
            k = key_for(name)
            if k != name:
                sc.pop(k)
            sc[name] = dict(rule)
        for name, keys in (patch_shortcuts or {}).items():
            k = key_for(name)
            cur = dict(sc.get(k) or {})
            cur.update(keys)
            sc[k] = cur
        doc["shortcuts"] = sc
    return json.dumps(doc, indent=2) + "\n"


# --------------------------------------------------------------------------
# COMMAND.COM dialect
# --------------------------------------------------------------------------

def command_com_problems(data):
    """Why this .bat would misbehave under Windows 98's COMMAND.COM, or []."""
    probs = []
    if isinstance(data, str):
        data = data.encode("latin-1")
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        return ["not ASCII"]
    if not text.endswith("\r\n"):
        probs.append("does not end in CRLF")
    if "\n" in text.replace("\r\n", "") or "\r" in text.replace("\r\n", ""):
        probs.append("a bare CR or LF - COMMAND.COM wants CRLF")
    nt_label = None
    for n, line in enumerate(text.split("\r\n")[:-1], 1):
        if len(line) > COMMAND_COM_LINE_MAX:
            probs.append("line %d is %d bytes (max %d)" % (n, len(line), COMMAND_COM_LINE_MAX))
        st = line.strip()
        low = st.lower()
        if nt_label:
            if low == ":" + nt_label.lower():
                nt_label = None
            continue
        m = NT_GUARD_RE.match(st)
        if m:
            nt_label = m.group(1)
            continue
        if "(" in line or ")" in line:
            probs.append("line %d has a parenthesis: %s" % (n, st))
        if low.startswith("rem"):
            for c in "<>|%":
                if c in line:
                    probs.append("line %d is a REM with %r - COMMAND.COM parses it: %s"
                                 % (n, c, st))
            continue
        for bad, why in (("%~", "a cmd.exe %~ modifier"), ("cd /d", "cmd.exe's cd /d"),
                         ('start "', 'start "title" (Win98 takes the title as the program)'),
                         ("setlocal", "setlocal"), ("&&", "&&"), ("||", "||"),
                         ("2>", "2> redirection")):
            if bad in low:
                probs.append("line %d uses %s: %s" % (n, why, st))
    if nt_label:
        probs.append("the NT-only region for %s is never closed" % nt_label)
    return probs
