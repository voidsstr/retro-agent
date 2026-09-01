#!/usr/bin/env python3
"""Generate docs/staged-library.md - what is staged, where it went, was it tested.

WHY THIS IS GENERATED AND NOT WRITTEN BY HAND
---------------------------------------------
The user asked for three things that are really one thing:

    "the full list of staged games is somewhere as well - make sure that is
     documented ... in the end i also want to know which games have been
     deployed to which retro computers and if it has been tested"

All of it already exists in `~/.retro-fleet/fleetbook.db`, and none of it was
readable without running a query. A hand-written list would be wrong within a
day: this library went 38 -> 46 titles in one session, two graphics cards were
swapped mid-session, and eight boxes are powered on and off continuously.

The same argument settled `docs/fleet-inventory.md`, whose hand-maintained
predecessor was wrong about most of the fleet - twice a card was changed without
the docs noticing. **Generate it, or it lies.**

THE DISTINCTIONS THIS DOCUMENT KEEPS, because each has cost real time here:

  * **deployed** (files are on the box) is not **runs** (it starts) is not
    **verified** (someone SAW it render and kept the screenshot). `state=done`
    from GAMESYNC is not evidence a game works.
  * **gated** (the machine cannot run it, with the limiting number) is not
    **skipped** (it did not fit on the disk) is not **failed**. Three different
    follow-ups; conflating the first two told an operator a Pentium 1 "cannot
    run" a game it merely had no room for.
  * **untested** is never rendered as anything else. A blank cell that reads as
    a pass is how a matrix starts lying.

Usage:
    python3 scripts/fleet/gen-staged-library.py            # write the doc
    python3 scripts/fleet/gen-staged-library.py --check    # non-zero if stale
"""
import argparse
import os
import sqlite3
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DB = os.path.expanduser("~/.retro-fleet/fleetbook.db")
OUT = os.path.join(REPO, "docs", "staged-library.md")

RUN_MARK = {"verified": "V", "runs": "r", "failed": "X", "untested": ".", "n/a": "-"}
DEP_MARK = {"deployed": "+", "gated": "G", "marginal": "~", "skipped": "s",
            "absent": "-", "failed": "X", "untested": "."}


def load():
    if not os.path.exists(DB):
        sys.exit("no compat database at %s - run `compat.py ingest` first" % DB)
    c = sqlite3.connect(DB)
    c.row_factory = sqlite3.Row
    boxes = [(r["ip"], r["hostname"], r["cpu_mhz"], r["ram_mb"], r["gpu"], r["os"])
             for r in c.execute("SELECT * FROM compat_box ORDER BY ip")]
    # compat_title as well, NOT just the two fact tables. A title that is in
    # the library but has never been deployed anywhere appears in neither
    # compat_deploy nor compat_render, so it was INVISIBLE here - the doc that
    # answers "which games are staged and where did they go" silently omitted
    # the newest ones. Rainbow Six was staged on 2026-09-01 while the whole
    # fleet was powered down and did not appear at all. Such a title now
    # renders as a full row of `-.`: absent from every box, untested
    # everywhere - which is the honest reading, and visibly different from
    # not being staged.
    titles = sorted({r[0] for r in c.execute("SELECT title FROM compat_deploy")}
                    | {r[0] for r in c.execute("SELECT title FROM compat_render")}
                    | {r[0] for r in c.execute("SELECT title FROM compat_title")})
    dep, run, mp = {}, {}, {}
    for r in c.execute("SELECT ip,title,state,limiting,have,need FROM compat_deploy"):
        dep[(r["ip"], r["title"])] = r
    for r in c.execute("SELECT ip,title,runs,renderer,width,height,fullscreen "
                       "FROM compat_render WHERE shortcut=''"):
        run[(r["ip"], r["title"])] = r
    for r in c.execute("SELECT ip,title,status,blocker FROM compat_mp"):
        mp[(r["ip"], r["title"])] = r
    c.close()
    return boxes, titles, dep, run, mp


def render():
    boxes, titles, dep, run, mp = load()
    ips = [b[0] for b in boxes]
    short = [ip.split(".")[-1] for ip in ips]
    L = []
    A = L.append
    A("# Staged library — what is staged, where it went, and whether it was tested")
    A("")
    A("**GENERATED — do not edit by hand.** Regenerate with")
    A("`python3 scripts/fleet/gen-staged-library.py`; `--check` fails if it is stale.")
    A("")
    A("A hand-written version of this was never going to survive: the library went")
    A("38 → %d titles in a single session, two graphics cards were swapped mid-session,"
      % len(titles))
    A("and the machines are powered on and off continuously. The same argument settled")
    A("`docs/fleet-inventory.md`, whose hand-maintained predecessor was wrong about most")
    A("of the fleet.")
    A("")
    A("Source of truth is `~/.retro-fleet/fleetbook.db`. Query it directly with")
    A("`scripts/fleet/compat.py` (`matrix`, `status --box .143`, `gaps`, `summary`).")
    A("")
    A("Generated %s." % time.strftime("%Y-%m-%d %H:%M"))
    A("")
    A("## The machines")
    A("")
    A("| box | host | CPU | RAM | GPU | OS |")
    A("|---|---|---|---|---|---|")
    for ip, host, mhz, ram, gpu, osv in boxes:
        A("| `%s` | %s | %s MHz | %s MB | %s | %s |"
          % (ip, host or "?", mhz or "?", ram or "?", (gpu or "?")[:34], osv or "?"))
    A("")
    A("## Deployment and test state")
    A("")
    A("Each cell is **deploy / runs**:")
    A("")
    A("| | deploy | | runs |")
    A("|---|---|---|---|")
    A("| `+` | deployed | `V` | **verified** — seen rendering fullscreen, screenshot kept |")
    A("| `G` | gated — the box cannot run it | `r` | starts; rendering not characterised |")
    A("| `s` | skipped — did not fit on the disk | `X` | failed |")
    A("| `~` | marginal (allowed) | `.` | **untested — nobody has looked** |")
    A("| `-` | absent | `-` | not applicable |")
    A("| `.` | **nobody has looked** — no deploy record for this box at all | | |")
    A("")
    A("A row that is `..` all the way across is a title that IS in the library and")
    A("has never been deployed to anything — staged, but nowhere yet. That is a")
    A("different fact from `-` (absent: we looked, it is not there) and from `G`")
    A("(gated: the hardware cannot run it), and it is what a title staged while the")
    A("fleet was powered down looks like.")
    A("")
    A("**`gated` and `skipped` are different facts.** The first means the hardware")
    A("cannot run it and carries the limiting number; the second means there was no")
    A("room. Conflating them once told an operator a Pentium 1 \"cannot run\" a game it")
    A("merely had no space for.")
    A("")
    A("| title | " + " | ".join("`.%s`" % s for s in short) + " | verified |")
    A("|---|" + "---|" * (len(short) + 1))
    for t in titles:
        cells = []
        nver = 0
        for ip in ips:
            d = dep.get((ip, t))
            r = run.get((ip, t))
            dm = DEP_MARK.get(d["state"] if d else "untested", "?")
            rm = RUN_MARK.get(r["runs"] if r else "untested", "?")
            if rm == "V":
                nver += 1
            cells.append("%s%s" % (dm, rm))
        A("| %s | %s | %d |" % (t, " | ".join(cells), nver))
    A("")
    tot = len(titles) * len(ips)
    def runs_of(ip, t):
        r = run.get((ip, t))
        return r["runs"] if r else "untested"

    nv = sum(1 for t in titles for ip in ips if runs_of(ip, t) == "verified")
    nu = sum(1 for t in titles for ip in ips if runs_of(ip, t) == "untested")
    A("**%d titles × %d machines = %d cells — %d verified, %d untested.**"
      % (len(titles), len(ips), tot, nv, nu))
    A("")
    A("## Titles with a blocker recorded")
    A("")
    A("| title | box | blocker |")
    A("|---|---|---|")
    seen = 0
    for (ip, t), r in sorted(mp.items()):
        if r["blocker"]:
            A("| %s | `%s` | %s |" % (t, ip, r["blocker"][:96]))
            seen += 1
    if not seen:
        A("| _none recorded_ | | |")
    A("")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the doc is stale, write nothing")
    a = ap.parse_args()
    body = render()
    old = ""
    if os.path.exists(OUT):
        with open(OUT, encoding="utf-8") as f:
            old = f.read()

    def strip_stamp(s):
        return "\n".join(l for l in s.split("\n") if not l.startswith("Generated "))

    if a.check:
        if strip_stamp(old) != strip_stamp(body):
            print("docs/staged-library.md is STALE - regenerate it", file=sys.stderr)
            return 1
        print("docs/staged-library.md is current")
        return 0
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(body)
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
