#!/usr/bin/env python3
"""Rebuild the stage_win9x_dos.py spec of every Windows 9x DOS title from the
LIBRARY itself - the one record of them that survives.

    python3 scripts/dosgames/spec_from_library.py > /tmp/rebuilt.json

The committed spec is scripts/dosgames/specs/win9x-dos.json - rebuilt this way
once, then EDITED as titles are fixed ("files", new launch lines). Whether the
library matches it is answered by the generator itself:

    python3 scripts/dosgames/stage_win9x_dos.py --spec scripts/dosgames/specs/win9x-dos.json --update --dry-run

WHY (2026-09-30). The 54 Flight-* and DOS-* titles were staged from specs and
prepared trees that lived in a session scratchpad, which the host's reboots
wiped. The staged trees on the share are intact, but the spec a fix must go
through ("fix it at the GENERATOR", CLAUDE.md) was gone. Everything the
generator derives a title's small files from can be read back out of what it
generated:

  launch.txt     -> the display name (== clean_name(title)) and the icon file
  Play <n>.bat   -> the launch lines (between its fixed header and CLS), or for
                    a real-DOS title RDGAME.BAT's lines
  requires.json  -> year and the title's own notes (after the fixed prefix)

"title" is the display name: clean_name() is idempotent, so regenerating from
it reproduces the same file names. The tree itself is not copied anywhere -
"tree" names the library directory, which IS the record of the game.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stage_win9x_dos as st  # noqa: E402

MARK = "staged for Windows 9x by scripts/dosgames/stage_win9x_dos.py"
PREFIX = ("DOS game staged for Windows 9x boxes: its desktop shortcut runs it natively in a "
          "Win9x DOS box, full screen. NT boxes do not get it - no DOSBox wrapper is staged. "
          "Sound: BLASTER from the box's AUTOEXEC.BAT; if a game is silent, run its own setup "
          "once. ")


def bat_lines(raw):
    return raw.decode("latin-1").replace("\r\n", "\n").rstrip("\n").split("\n")


def launch_from_play_bat(lines):
    """The generated header is '@echo off' + rem lines; the tail is the CLS
    comment + 'cls'. What is between them is the spec's launch list."""
    i = 1
    while i < len(lines) and lines[i].lower().startswith("rem "):
        i += 1
    body = lines[i:]
    if body[-2:] != ["rem CLS last: a DOS box closes on exit only if its screen is empty.", "cls"]:
        raise ValueError("not a generated Play bat (no CLS tail)")
    return body[:-2]


def one(lib_dir):
    lib = os.path.basename(lib_dir)
    lt = open(os.path.join(lib_dir, "launch.txt"), "rb").read().decode("latin-1")
    if MARK not in lt:
        return None
    first = lt.split("\r\n")[0].split("\t")
    bat, disp, icon = first[0], first[1], first[2]
    req = json.load(open(os.path.join(lib_dir, "requires.json")))
    notes = req.get("notes", "")
    if (notes + " ").startswith(PREFIX):       # the generator strip()s the whole string
        notes = (notes + " ")[len(PREFIX):].rstrip()
    else:
        raise ValueError("%s: requires.json notes lack the generated prefix" % lib)
    t = {"lib": lib, "title": disp, "year": req.get("year"), "tree": lib_dir,
         "icon": os.path.join(lib_dir, icon)}
    if icon.upper() != st.icon_file_name({"lib": lib}):
        t["icon_name"] = icon.upper()
    src = re.search(r"Source: (.*?)\r\n", lt)
    if src and src.group(1) != lib:
        t["source_label"] = src.group(1)
    m = re.match(r"REAL DOS ONLY: this game cannot run inside Windows; its launcher copies it to "
                 r"C:\\GAMES\\([A-Z0-9_]+) and restarts the PC into real DOS through the rundos line "
                 r"in AUTOEXEC\.BAT( - it needs EMS: boot menu 2, With EMS)?\.(?: |$)", notes)
    if m:
        t["real_dos"] = {"dir": m.group(1)}
        if m.group(2):
            t["real_dos"]["ems"] = True
        notes = notes[m.end():]
        run = bat_lines(open(os.path.join(lib_dir, "RDGAME.BAT"), "rb").read())
        body = run[run.index("cd \\GAMES\\%s" % m.group(1)) + 1:]
        if body and body[0].startswith('if "%CONFIG%"=="EMS"'):
            body = body[body.index(":run") + 1:]
        t["launch"] = body[:body.index(":done")]
    else:
        t["launch"] = launch_from_play_bat(bat_lines(open(os.path.join(lib_dir, bat), "rb").read()))
    t["notes"] = notes
    if bat != "Play %s.bat" % st.clean_name(disp):
        raise ValueError("%s: launcher %r is not what the generator would name it" % (lib, bat))
    return t


def build(root=st.LIB_MNT):
    out = []
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        if name.startswith("_") or not os.path.isfile(os.path.join(d, "launch.txt")):
            continue
        t = one(d)
        if t:
            out.append(t)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=st.LIB_MNT)
    a = ap.parse_args()
    json.dump(build(a.root), sys.stdout, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
