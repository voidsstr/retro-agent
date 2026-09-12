#!/usr/bin/env python3
"""
v56k_report.py - render a v56k_bench.py results.csv into article tables.

Reads the CSV that v56k_bench.py appends to and prints the tables the
specpicks.com writeup needs, plus the things a benchmark table normally hides:

  * every non-measured cell, named with WHY (a mode the card refused, a GL init
    that hung, a title blocked because an unsafe DLL sat beside its exe).  A
    blank cell that reads as a pass is how a matrix starts lying, so nothing is
    silently dropped.
  * the GL_RENDERER actually recorded per row, and a loud complaint if one grid
    mixes renderers - that is the difference between measuring a driver and
    measuring whatever DLL happened to load.
  * the AA verification column.  A row whose AA config could not be read back
    is not reported as a number.

Usage:
    python3 v56k_report.py [results.csv] [--md]
"""

import argparse
import csv
import sys
from collections import OrderedDict
from pathlib import Path

CHIP_ORDER = [(1, 1), (1, 2), (2, 1), (2, 2), (2, 4), (4, 1), (4, 2), (4, 4), (4, 8)]


def aa_label(chips, samples):
    return f"{chips}chip/{'no' if samples == 1 else str(samples) + 'x'}AA"


def res_key(r):
    try:
        return (int(r["width"]), int(r["height"]))
    except (ValueError, KeyError):
        return (0, 0)


def load(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def grid(rows, title, api, depth):
    """rows -> {res: {(chips,samples): fps}} for one title/api/depth."""
    out = {}
    for r in rows:
        if r["title"] != title or r.get("api", "") != api:
            continue
        if str(r["colordepth"]) != str(depth) or r["status"] != "ok":
            continue
        fps = fnum(r["avg_fps"])
        if fps is None:
            continue
        key = (int(r["chips"]), int(r["samples"]))
        out.setdefault(res_key(r), {})[key] = fps
    return OrderedDict(sorted(out.items()))


def print_grid(g, header, md=False):
    if not g:
        return
    cols = [c for c in CHIP_ORDER if any(c in v for v in g.values())]
    print(f"\n### {header}\n")
    head = ["resolution"] + [aa_label(*c) for c in cols]
    if md:
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join(["---"] * len(head)) + "|")
    else:
        print("  " + "".join(f"{h:>14}" for h in head))
    for (w, h), cells in g.items():
        vals = [f"{cells[c]:.1f}" if c in cells else "-" for c in cols]
        if md:
            print(f"| {w}x{h} | " + " | ".join(vals) + " |")
        else:
            print(f"  {w}x{h:<8}".ljust(2) + "".join(f"{v:>14}" for v in [f"{w}x{h}"] + vals)[14:])


def print_scaling(g, md=False):
    """How much does a second and a fourth chip actually buy, per resolution?"""
    have = [r for r in g if (1, 1) in g[r]]
    if not have:
        return
    print("\n### SLI scaling, no AA  (x relative to one chip)\n")
    head = ["resolution", "1 chip", "2 chips", "4 chips", "2x", "4x"]
    if md:
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join(["---"] * len(head)) + "|")
    for r in have:
        c1, c2, c4 = g[r].get((1, 1)), g[r].get((2, 1)), g[r].get((4, 1))
        row = [f"{r[0]}x{r[1]}",
               f"{c1:.1f}" if c1 else "-",
               f"{c2:.1f}" if c2 else "-",
               f"{c4:.1f}" if c4 else "-",
               f"{c2 / c1:.2f}x" if c1 and c2 else "-",
               f"{c4 / c1:.2f}x" if c1 and c4 else "-"]
        if md:
            print("| " + " | ".join(row) + " |")
        else:
            print("  " + "".join(f"{v:>13}" for v in row))


def print_aa_cost(g, md=False):
    """What does anti-aliasing cost on four chips? The card's whole argument."""
    have = [r for r in g if (4, 1) in g[r]]
    if not have:
        return
    print("\n### Cost of AA on four chips  (% of the no-AA figure retained)\n")
    head = ["resolution", "no AA", "2x", "4x", "8x"]
    if md:
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join(["---"] * len(head)) + "|")
    for r in have:
        base = g[r][(4, 1)]
        row = [f"{r[0]}x{r[1]}", f"{base:.1f}"]
        for s in (2, 4, 8):
            v = g[r].get((4, s))
            row.append(f"{v:.1f} ({v / base * 100:.0f}%)" if v else "-")
        if md:
            print("| " + " | ".join(row) + " |")
        else:
            print("  " + "".join(f"{v:>18}" for v in row))


def print_depth(rows, title, api, md=False):
    """16 vs 32-bit at matched settings.

    FINDINGS records that 3dfx's own V5-6000 reference driver showed only a
    1.03x gain for halving colour depth where community drivers got 1.20x, and
    called it a driver defect. This table is that experiment.
    """
    pairs = {}
    for r in rows:
        if r["title"] != title or r.get("api", "") != api or r["status"] != "ok":
            continue
        fps = fnum(r["avg_fps"])
        if fps is None:
            continue
        k = (res_key(r), int(r["chips"]), int(r["samples"]))
        pairs.setdefault(k, {})[str(r["colordepth"])] = fps
    both = {k: v for k, v in pairs.items() if "16" in v and "32" in v}
    if not both:
        return
    print("\n### 16-bit vs 32-bit  (ratio near 1.0x reproduces the documented 3dfx defect)\n")
    head = ["resolution", "config", "16-bit", "32-bit", "16/32"]
    if md:
        print("| " + " | ".join(head) + " |")
        print("|" + "|".join(["---"] * len(head)) + "|")
    for (res, chips, samples), v in sorted(both.items()):
        row = [f"{res[0]}x{res[1]}", aa_label(chips, samples),
               f"{v['16']:.1f}", f"{v['32']:.1f}", f"{v['16'] / v['32']:.2f}x"]
        if md:
            print("| " + " | ".join(row) + " |")
        else:
            print("  " + "".join(f"{x:>15}" for x in row))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="?",
                    default=str(Path(__file__).resolve().parent / "results"
                               / "v56k_192.168.1.191" / "results.csv"))
    ap.add_argument("--md", action="store_true", help="markdown tables")
    a = ap.parse_args()

    rows = load(a.csv)
    ok = [r for r in rows if r["status"] == "ok"]
    print(f"{a.csv}\n{len(rows)} row(s), {len(ok)} measured\n")

    combos = OrderedDict()
    for r in rows:
        combos.setdefault((r["title"], r.get("api", "")), []).append(r)

    for (title, api), sub in combos.items():
        engine = next((r["engine"] for r in sub if r.get("engine")), "")
        print("=" * 74)
        print(f"{title}   api={api or '?'}   {engine}")
        print("=" * 74)
        for depth in ("16", "32"):
            g = grid(rows, title, api, depth)
            if g:
                print_grid(g, f"average fps, {depth}-bit", a.md)
        g16 = grid(rows, title, api, "16")
        print_scaling(g16, a.md)
        print_aa_cost(g16, a.md)
        print_depth(rows, title, api, a.md)

        # attribution: one grid must be one renderer
        rends = {r["gl_renderer"] for r in sub if r.get("gl_renderer")}
        if len(rends) > 1:
            print("\n  !! MIXED RENDERERS in this grid - these rows do not "
                  "describe one driver:")
            for x in sorted(rends):
                print(f"     {x}")
        elif rends:
            print(f"\n  renderer: {rends.pop()}")

        unver = [r for r in sub if r.get("aa_verified", "").startswith("NO")]
        if unver:
            print(f"  !! {len(unver)} row(s) had an UNVERIFIED AA config and "
                  f"carry no number")

    bad = [r for r in rows if r["status"] != "ok"]
    if bad:
        print("\n" + "=" * 74)
        print(f"NOT MEASURED - {len(bad)} cell(s), each with its reason")
        print("=" * 74)
        by = OrderedDict()
        for r in bad:
            by.setdefault(r["status"], []).append(r)
        for status, group in by.items():
            print(f"\n  {status}  ({len(group)})")
            for r in group[:12]:
                note = (r.get("notes") or "")[:70]
                print(f"    {r['title']:<24} {r['res']:>10} {r['colordepth']:>3}bit "
                      f"{aa_label(int(r['chips']), int(r['samples'])):>14}  {note}")
            if len(group) > 12:
                print(f"    ... and {len(group) - 12} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
