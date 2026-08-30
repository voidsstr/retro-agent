#!/usr/bin/env python3
r"""Render docs/fleet-inventory.md from the hardware records the boxes publish.

WHY THIS EXISTS. The fleet's machine documentation was hand-maintained and it
was wrong about most of the fleet. TWICE a box's graphics card was swapped
without the docs noticing: .124's Voodoo 3 came out in August and the stale
claim survived for weeks, and .133's Voodoo5 6000 is physically gone while
three documents still named the box by it. Three machines carried a
DX9-or-better GPU nothing mentioned, which would have wrongly refused 2004-era
titles on five of eight boxes.

A document updated by hand after a screwdriver goes into a case will be wrong.
So the machine reports itself: agent/src/hwpublish.c writes each box's
HWPROFILE JSON to the share on every startup, and this renders those.

THE DESIGN CONSEQUENCE THAT MATTERS MOST: generating a document and LEAVING the
hand-written table in place solves nothing - it creates a second thing to go
stale. The generated file is the single source of truth for every measured
field. CLAUDE.md keeps only the per-box traps a probe cannot discover, and
points here.

THREE STATES, NEVER TWO. This project's rule is that "not installed" and
"crashed" must never render the same, and the same applies here:

  current       a record, measured recently
  stale         a record, but old enough that it may no longer describe the box
  never seen    no record at all - the box has never published one
  unreadable    a record that will not parse (a torn copy, a truncated write)

None of these is a fault on its own. THE FLEET IS POWERED ON DEMAND: the retro
machines are deliberately kept off, so at any moment several boxes legitimately
carry old data. "stale" means "re-measure before trusting this", not "broken" -
which is why every record is stamped with when it was measured.

TWO CLOCKS. Staleness is judged by the file's mtime on the share (this host's
clock, which is right) and NOT by the timestamp inside the record (the retro
box's own clock, which on machines this old is frequently years out). Both are
shown, and a disagreement is reported as clock skew rather than silently
changing the answer.

    python3 scripts/fleet/inventory.py                 # write docs/fleet-inventory.md
    python3 scripts/fleet/inventory.py --stdout        # print instead
    python3 scripts/fleet/inventory.py --json          # for tooling
    python3 scripts/fleet/inventory.py --check         # exit 1 if any box is not current

Exit 0 = rendered. With --check, exit 1 when a rostered box is missing or stale.
"""
import argparse
import datetime as _dt
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

DEFAULT_DIR = "/mnt/retro-share/Utility/Retro Automation/fleet-inventory"
DEFAULT_ROSTER = os.path.join(HERE, "fleet-roster.txt")
DEFAULT_OUT = os.path.join(REPO, "docs", "fleet-inventory.md")

# A record older than this may no longer describe the machine. Generous on
# purpose: the fleet is off most of the time, and a box that has been powered
# down for a fortnight has not changed.
DEFAULT_STALE_DAYS = 14

STATE_CURRENT = "current"
STATE_STALE = "stale"
STATE_NEVER = "never seen"
STATE_UNREADABLE = "unreadable"


# --------------------------------------------------------------------------
# roster
# --------------------------------------------------------------------------

def load_roster(path):
    """[(ip, hostname, note)] in file order. A missing roster is empty, not fatal:
    the records on the share still render, just with nothing to call missing."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = re.split(r"\t+|  +", line.strip(), maxsplit=2)
            ip = parts[0].strip()
            host = parts[1].strip() if len(parts) > 1 else ""
            note = parts[2].strip() if len(parts) > 2 else ""
            out.append((ip, host, note))
    return out


# --------------------------------------------------------------------------
# records
# --------------------------------------------------------------------------

def load_records(directory):
    """Every *.json in the publish directory.

    A file that will not parse becomes a record in state 'unreadable' rather
    than an exception: one torn copy must not take the whole document down, and
    an unreadable record is itself worth reporting - it is how you find out a
    box is writing garbage.
    """
    records = []
    if not os.path.isdir(directory):
        return records
    for name in sorted(os.listdir(directory)):
        if not name.lower().endswith(".json"):
            continue
        path = os.path.join(directory, name)
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            mtime = None
        rec = {"file": name, "path": path, "mtime": mtime, "data": None,
               "error": None}
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                raw = fh.read()
            rec["data"] = json.loads(raw)
            if not isinstance(rec["data"], dict):
                rec["data"] = None
                rec["error"] = "top-level JSON is not an object"
        except json.JSONDecodeError as exc:
            rec["error"] = "unparseable JSON: %s" % exc
        except OSError as exc:
            rec["error"] = "unreadable: %s" % exc
        records.append(rec)
    return records


def record_ips(data):
    ips = []
    net = (data or {}).get("network") or {}
    for iface in net.get("interfaces") or []:
        for ip in iface.get("ipv4") or []:
            if ip and ip not in ips:
                ips.append(ip)
    return ips


def record_macs(data):
    macs = []
    net = (data or {}).get("network") or {}
    for iface in net.get("interfaces") or []:
        mac = (iface.get("mac") or "").strip()
        if mac and mac not in macs:
            macs.append(mac)
    return macs


def match_records(roster, records):
    """Pair roster entries with records, by IP first and hostname second.

    IP FIRST, DELIBERATELY. The file is named after the box's computer name,
    and a computer name is not an identity: two boxes on this fleet have been
    renamed, and .124 answers to two names in different documents. The record
    carries the addresses it was published from, so that is the reliable key.
    Hostname is the fallback for a box whose network probe found nothing.
    """
    by_entry = {}
    used = set()
    for ip, host, _note in roster:
        hit = None
        for idx, rec in enumerate(records):
            if idx in used or rec["data"] is None:
                continue
            if ip and ip in record_ips(rec["data"]):
                hit = idx
                break
        if hit is None:
            for idx, rec in enumerate(records):
                if idx in used or rec["data"] is None:
                    continue
                rhost = (rec["data"].get("hostname") or "").strip().lower()
                if host and rhost == host.strip().lower():
                    hit = idx
                    break
        if hit is None:
            # An unreadable record still belongs to whoever it is named after -
            # otherwise a box writing garbage renders as "never seen", which is
            # the wrong call to action entirely.
            for idx, rec in enumerate(records):
                if idx in used or rec["data"] is not None:
                    continue
                stem = os.path.splitext(rec["file"])[0].strip().lower()
                if host and stem == host.strip().lower():
                    hit = idx
                    break
        if hit is not None:
            used.add(hit)
            by_entry[ip] = records[hit]
    unrostered = [r for i, r in enumerate(records) if i not in used]
    return by_entry, unrostered


def classify(rec, now, stale_days):
    """The state of one roster entry. Never two states where there are four."""
    if rec is None:
        return STATE_NEVER, None
    if rec["data"] is None:
        return STATE_UNREADABLE, rec["mtime"]
    if rec["mtime"] is None:
        return STATE_STALE, None
    age_days = (now - rec["mtime"]) / 86400.0
    if age_days > stale_days:
        return STATE_STALE, rec["mtime"]
    return STATE_CURRENT, rec["mtime"]


# --------------------------------------------------------------------------
# formatting
# --------------------------------------------------------------------------

def fmt_when(ts):
    if not ts:
        return "unknown"
    return _dt.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M")


def fmt_age(ts, now):
    if not ts:
        return "unknown"
    secs = max(0.0, now - ts)
    if secs < 3600:
        return "%d min ago" % int(secs // 60)
    if secs < 86400:
        return "%d h ago" % int(secs // 3600)
    return "%d days ago" % int(secs // 86400)


def clock_skew(rec):
    """Minutes between the box's own timestamp and when the file landed here.

    Reported, never acted on. A retro box's RTC is often wrong by years, and a
    tool that judged staleness by the record's own clock would call a machine
    that published thirty seconds ago 'last seen in 2003'.
    """
    if not rec or rec["data"] is None or not rec["mtime"]:
        return None
    stamp = rec["data"].get("reported_at")
    if not stamp:
        return None
    for pattern in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S"):
        try:
            box = _dt.datetime.strptime(stamp[:19], pattern)
        except ValueError:
            continue
        return (box.timestamp() - rec["mtime"]) / 60.0
    return None


def gpu_line(data):
    gpu = data.get("gpu") or {}
    name = gpu.get("name") or "unknown"
    ven, dev = gpu.get("pci_ven") or "", gpu.get("pci_dev") or ""
    pci = ""
    if ven and dev:
        pci = " (`%s:%s`)" % (ven.replace("0x", ""), dev.replace("0x", ""))
    vram = gpu.get("vram_mb")
    extra = ", %s MB" % vram if vram else ""
    level = gpu.get("feature_level")
    if level:
        extra += ", %s" % level
    return "%s%s%s" % (name, pci, extra)


def cpu_line(data):
    cpu = data.get("cpu") or {}
    brand = (cpu.get("brand") or "").strip() or cpu.get("vendor") or "unknown"
    mhz = cpu.get("mhz")
    count = cpu.get("count") or 1
    bits = "%s" % brand
    if mhz:
        bits += ", %s MHz" % mhz
    if count and int(count) > 1:
        bits += " x%s" % count
    return bits


def disk_line(data):
    rows = data.get("disk") or []
    parts = []
    for d in rows:
        root = d.get("root") or "?"
        free = d.get("free_mb")
        total = d.get("total_mb")
        if free is None:
            continue
        if total:
            parts.append("%s %.0f/%.0f GB free" %
                         (root.rstrip("\\"), free / 1024.0, total / 1024.0))
        else:
            parts.append("%s %.0f GB free" % (root.rstrip("\\"), free / 1024.0))
    return ", ".join(parts) if parts else "unknown"


def display_line(data):
    d = data.get("display") or {}
    live = "%sx%sx%s" % (d.get("width"), d.get("height"), d.get("bpp"))
    target = ""
    if d.get("panel_w") and d.get("panel_h"):
        target = "%sx%s" % (d["panel_w"], d["panel_h"])
        src = d.get("panel_source")
        if src:
            target += " (%s)" % src
    if target and target.split(" ")[0] != "%sx%s" % (d.get("width"),
                                                     d.get("height")):
        # The live mode and the persisted mode disagree, which is the exact
        # situation a game leaves behind when it exits without restoring. Say
        # both; the persisted one is what the machine is configured to be.
        return "persisted **%s**, currently %s" % (target, live)
    return "%s" % (target or live)


# --------------------------------------------------------------------------
# document
# --------------------------------------------------------------------------

HEADER = """<!-- GENERATED FILE - DO NOT EDIT BY HAND -->
# Fleet hardware inventory

**This file is generated. Do not hand-edit it** - every change is overwritten
the next time `scripts/fleet/inventory.py` runs. Each machine publishes its own
record (`agent/src/hwpublish.c`, on every agent startup) to
`\\\\192.168.1.122\\files\\Utility\\Retro Automation\\fleet-inventory\\<host>.json`,
and this document is nothing but those records rendered.

To correct something here, fix the machine or the probe, not the file. Per-box
prose that a probe cannot discover - the traps, the dual-boot layouts, "this
box answers slowly" - lives in `scripts/fleet/fleet-roster.txt` and in
CLAUDE.md's *Known Machines* section, which deliberately holds **no** measured
field.

Regenerate with:

```bash
python3 scripts/fleet/inventory.py
```

*Rendered {rendered} from records in `{directory}`.*

> **A stale or missing record is not an outage.** The fleet is powered on
> demand - the retro machines are deliberately kept off - so at any moment
> several boxes legitimately carry old data. `stale` means *re-measure before
> trusting this*; `never seen` means that box has never published at all.
> Staleness is judged by when the record landed on this host, not by the retro
> box's own clock, which on hardware this old is frequently years out.
"""


def render(roster, matched, unrostered, now, stale_days, directory):
    lines = [HEADER.format(
        rendered=_dt.datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M"),
        directory=directory)]

    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| IP | Hostname | State | Measured | CPU | RAM | Display GPU | OS |")
    lines.append("|----|----------|-------|----------|-----|-----|-------------|----|")
    for ip, host, _note in roster:
        rec = matched.get(ip)
        state, ts = classify(rec, now, stale_days)
        if state in (STATE_NEVER, STATE_UNREADABLE):
            lines.append("| %s | %s | **%s** | %s | - | - | - | - |" % (
                ip, host or "-", state,
                fmt_age(ts, now) if ts else "-"))
            continue
        data = rec["data"]
        mark = "current" if state == STATE_CURRENT else "**stale**"
        lines.append("| %s | %s | %s | %s | %s | %s MB | %s | %s |" % (
            ip,
            data.get("hostname") or host or "-",
            mark,
            fmt_age(ts, now),
            cpu_line(data),
            data.get("ram_mb") or "?",
            gpu_line(data),
            ((data.get("os") or {}).get("product") or "?"),
        ))
    lines.append("")

    for ip, host, note in roster:
        rec = matched.get(ip)
        state, ts = classify(rec, now, stale_days)
        lines.append("---")
        lines.append("")
        lines.append("## %s - %s" % (ip, host or "unnamed"))
        lines.append("")
        if state == STATE_NEVER:
            lines.append("**never seen.** This box is on the roster but has "
                         "never published a record to `%s`." % directory)
            lines.append("")
            lines.append("That is not by itself a fault: the fleet is powered "
                         "on demand, and a machine that has not been switched "
                         "on since the publisher shipped has nothing to say "
                         "yet. It becomes a fault if the box has booted since "
                         "- check the agent's log for `HWPUBLISH`, and run "
                         "`HWPUBLISH` by hand to see the reason.")
            if note:
                lines.append("")
                lines.append("> %s" % note)
            lines.append("")
            continue
        if state == STATE_UNREADABLE:
            lines.append("**unreadable record.** `%s` exists (%s) but will not "
                         "parse: %s" % (rec["file"], fmt_when(ts),
                                        rec["error"]))
            lines.append("")
            lines.append("A torn copy is the likely cause - the record is "
                         "written locally and copied in one go, but a reader "
                         "can still catch the copy in flight. If it persists, "
                         "the box is writing garbage and that is a real fault.")
            if note:
                lines.append("")
                lines.append("> %s" % note)
            lines.append("")
            continue

        data = rec["data"]
        skew = clock_skew(rec)
        if state == STATE_STALE:
            lines.append("**stale, last seen %s (%s).** Older than the %d-day "
                         "threshold, so re-measure before trusting it - this "
                         "is normal for a box that has been powered off." %
                         (fmt_age(ts, now), fmt_when(ts), stale_days))
        else:
            lines.append("Measured %s (%s)." % (fmt_when(ts), fmt_age(ts, now)))
        lines.append("")
        if note:
            lines.append("> %s" % note)
            lines.append("")

        os_ = data.get("os") or {}
        dx = data.get("directx") or {}
        rows = [
            ("CPU", cpu_line(data)),
            ("CPU id", "family %s model %s stepping %s, vendor `%s`" % (
                (data.get("cpu") or {}).get("family"),
                (data.get("cpu") or {}).get("model"),
                (data.get("cpu") or {}).get("stepping"),
                (data.get("cpu") or {}).get("vendor"))),
            ("Instruction set", ", ".join((data.get("cpu") or {}).get("features")
                                          or []) or "unknown"),
            ("RAM", "%s MB" % (data.get("ram_mb") or "?")),
            ("Display GPU", gpu_line(data)),
            ("GPU driver", "%s (%s)" % (
                (data.get("gpu") or {}).get("driver_version") or "?",
                (data.get("gpu") or {}).get("driver_date") or "?")),
            ("Display mode", display_line(data)),
            ("OS", "%s %s %s" % (os_.get("product") or "?",
                                 os_.get("service_pack") or "",
                                 "(%s)" % os_.get("version")
                                 if os_.get("version") else "")),
            ("DirectX", dx.get("version") or "?"),
            ("Disks", disk_line(data)),
            ("Agent", data.get("agent_version") or "?"),
            ("Profile hash", "`%s`" % (data.get("profile_hash") or "?")),
        ]
        lines.append("| field | value |")
        lines.append("|-------|-------|")
        for k, v in rows:
            lines.append("| %s | %s |" % (k, str(v).strip()))
        lines.append("")

        cards = data.get("video_cards") or []
        if len(cards) > 1 or (cards and not any(
                c.get("attached_to_desktop") for c in cards)):
            lines.append("**Video adapters on this box** - the one attached to "
                         "the desktop is the one games run on; the others are "
                         "fitted, or are stale class keys for cards that are "
                         "not:")
            lines.append("")
            lines.append("| instance | adapter | PCI | driver | attached to desktop |")
            lines.append("|----------|---------|-----|--------|---------------------|")
            for c in cards:
                lines.append("| %s | %s | `%s:%s` | %s | %s |" % (
                    c.get("instance") or "?",
                    c.get("name") or "?",
                    (c.get("pci_ven") or "").replace("0x", ""),
                    (c.get("pci_dev") or "").replace("0x", ""),
                    c.get("driver_version") or "?",
                    "**yes**" if c.get("attached_to_desktop") else "no"))
            lines.append("")

        glide = data.get("glide_cards") or []
        if glide:
            total = sum(int(g.get("count") or 1) for g in glide)
            lines.append("**3dfx silicon: %d card%s.** From the PCI enumerator "
                         "(`Enum\\PCI`, `VEN_121A`), which is the only source a "
                         "`Class=MEDIA` Voodoo 2 cannot hide from - it appears "
                         "in no display-class scan at all." %
                         (total, "" if total == 1 else "s"))
            lines.append("")
            for g in glide:
                lines.append("- `%s` - %s (%d instance%s)" % (
                    g.get("device_key") or "?",
                    g.get("description") or "no DeviceDesc",
                    int(g.get("count") or 1),
                    "" if int(g.get("count") or 1) == 1 else "s"))
            lines.append("")
        elif "glide_cards" in data:
            lines.append("**No 3dfx silicon.** `Enum\\PCI` carries no "
                         "`VEN_121A` key, and a physically fitted card "
                         "enumerates there even with no driver bound - so this "
                         "is the decisive read, not merely \"undriven\".")
            lines.append("")

        macs = record_macs(data)
        ips = record_ips(data)
        if ips or macs:
            lines.append("Network: %s%s" % (
                ", ".join(ips) or "no address",
                " - MAC %s" % ", ".join(macs) if macs else ""))
            lines.append("")

        if skew is not None and abs(skew) > 60:
            lines.append("> **Clock skew:** this box's own clock reads %s, "
                         "%.0f minutes %s the time the record landed here. "
                         "Staleness above is judged by this host's clock, not "
                         "the box's." % (
                             data.get("reported_at"), abs(skew),
                             "ahead of" if skew > 0 else "behind"))
            lines.append("")

    if unrostered:
        lines.append("---")
        lines.append("")
        lines.append("## Records with no roster entry")
        lines.append("")
        lines.append("These machines published a record but are not in "
                     "`scripts/fleet/fleet-roster.txt`. That is information, "
                     "not an error - a new box has appeared. Add it to the "
                     "roster so it can ever be reported missing.")
        lines.append("")
        for rec in unrostered:
            data = rec["data"] or {}
            lines.append("- `%s` - %s, %s, last seen %s" % (
                rec["file"],
                data.get("hostname") or "unparseable",
                ", ".join(record_ips(data)) or "no address",
                fmt_when(rec["mtime"])))
        lines.append("")

    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------

def build(directory, roster_path, stale_days, now=None):
    now = now if now is not None else _dt.datetime.now().timestamp()
    roster = load_roster(roster_path)
    records = load_records(directory)
    matched, unrostered = match_records(roster, records)
    states = {}
    for ip, _host, _note in roster:
        state, ts = classify(matched.get(ip), now, stale_days)
        states[ip] = {"state": state, "measured": ts,
                      "measured_text": fmt_when(ts)}
    return {
        "roster": roster,
        "matched": matched,
        "unrostered": unrostered,
        "states": states,
        "now": now,
        "directory": directory,
        "stale_days": stale_days,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR,
                    help="directory of published records (default: %(default)s)")
    ap.add_argument("--roster", default=DEFAULT_ROSTER)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--stale-days", type=float, default=DEFAULT_STALE_DAYS)
    ap.add_argument("--stdout", action="store_true",
                    help="print the document instead of writing it")
    ap.add_argument("--json", action="store_true",
                    help="emit the per-box states as JSON, for tooling")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if any rostered box is not current")
    args = ap.parse_args(argv)

    ctx = build(args.dir, args.roster, args.stale_days)

    if args.json:
        print(json.dumps({
            "directory": ctx["directory"],
            "stale_days": ctx["stale_days"],
            "hosts": ctx["states"],
            "unrostered": [r["file"] for r in ctx["unrostered"]],
        }, indent=2))
    else:
        doc = render(ctx["roster"], ctx["matched"], ctx["unrostered"],
                     ctx["now"], ctx["stale_days"], ctx["directory"])
        if args.stdout:
            sys.stdout.write(doc)
        else:
            os.makedirs(os.path.dirname(args.out), exist_ok=True)
            with open(args.out, "w", encoding="utf-8") as fh:
                fh.write(doc)
            bad = [ip for ip, s in ctx["states"].items()
                   if s["state"] != STATE_CURRENT]
            print("wrote %s (%d boxes, %d not current)" %
                  (args.out, len(ctx["roster"]), len(bad)))
            for ip in bad:
                print("  %-16s %s" % (ip, ctx["states"][ip]["state"]))

    if args.check:
        bad = [ip for ip, s in ctx["states"].items()
               if s["state"] != STATE_CURRENT]
        if bad:
            sys.stderr.write("not current: %s\n" % ", ".join(sorted(bad)))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
