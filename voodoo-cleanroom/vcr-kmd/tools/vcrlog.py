#!/usr/bin/env python3
"""vcrlog.py - read the vcr-kmd flight recorder.

Three sources, one decoder:
  vcrlog.py fetch <host> [--port 9898] [--tool C:\\vcr\\vcrctl.exe] [--after N]
        run `vcrctl log` on a box through the agent and decode it
  vcrlog.py decode <file>     decode saved `vcrctl log` output (TSV)
  vcrlog.py events            list every event code (from include/vcr_events.h)

Event names come from include/vcr_events.h itself (the VCR_EVENT(...) table),
so the C side and this decoder cannot drift.
"""
import argparse
import asyncio
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EVENTS_H = HERE.parent / "include" / "vcr_events.h"
SRC = {1: "mp", 2: "dd", 3: "esc", 4: "tool"}
LEVEL = {0: "ERR", 1: "WRN", 2: "inf", 3: "dbg", 4: "trc"}


def load_events(path=EVENTS_H):
    ev = {}
    for m in re.finditer(r'VCR_EVENT\((VCR_EV_\w+),\s*(\d+),\s*"([^"]*)"\)',
                         Path(path).read_text()):
        ev[int(m.group(2))] = (m.group(1)[len("VCR_EV_"):], m.group(3))
    return ev


def parse_tsv(text):
    """(header dict, [entry dicts]) from `vcrctl log` output."""
    hdr, out = {}, []
    for line in text.splitlines():
        if line.startswith("#vcrlog"):
            for kv in line.split()[1:]:
                k, _, v = kv.partition("=")
                hdr[k] = v
            continue
        f = line.split("\t")
        if len(f) < 10 or not f[0].isdigit():
            continue
        out.append({"seq": int(f[0]), "ms": int(f[1]), "src": int(f[2]), "level": int(f[3]),
                    "code": int(f[4]), "pid": int(f[5]),
                    "a": int(f[6], 16), "b": int(f[7], 16), "c": int(f[8], 16),
                    "d": int(f[9], 16), "msg": f[10] if len(f) > 10 else ""})
    return hdr, out


def format_entry(e, events):
    name = events.get(e["code"], (f"code{e['code']}", ""))[0]
    return (f"{e['seq']:6d} {e['ms'] / 1000:9.3f}s {SRC.get(e['src'], '?'):>4} "
            f"{LEVEL.get(e['level'], '?')} {name:<18} {e['a']:08x} {e['b']:08x} "
            f"{e['c']:08x} {e['d']:08x}  {e['msg']}")


async def fetch(host, port, tool, after):
    sys.path.insert(0, str(HERE.parents[2]))
    from client.retro_protocol import RetroConnection
    c = RetroConnection(host, port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        st, d = await c.send_command(f"EXEC {tool} log {after}", timeout=60)
        return d.decode("ascii", "replace")
    finally:
        await c.close()


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("host")
    f.add_argument("--port", type=int, default=9898)
    f.add_argument("--tool", default=r"C:\vcr\vcrctl.exe")
    f.add_argument("--after", type=int, default=0)
    f.add_argument("--save")
    d = sub.add_parser("decode")
    d.add_argument("file")
    sub.add_parser("events")
    a = ap.parse_args(argv)
    events = load_events()
    if a.cmd == "events":
        for code in sorted(events):
            print(f"{code:4d} {events[code][0]:<18} {events[code][1]}")
        return 0
    text = (asyncio.run(fetch(a.host, a.port, a.tool, a.after)) if a.cmd == "fetch"
            else Path(a.file).read_text())
    if a.cmd == "fetch" and a.save:
        Path(a.save).write_text(text)
    hdr, entries = parse_tsv(text)
    if not hdr and not entries:
        print(text.strip()[:500])
        return 1
    print(f"# boot_count={hdr.get('boot_count')} version={hdr.get('version')} "
          f"next_seq={hdr.get('next_seq')} ({len(entries)} entries)")
    for e in entries:
        print(format_entry(e, events))
    return 0


if __name__ == "__main__":
    sys.exit(main())
