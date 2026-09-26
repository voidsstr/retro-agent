#!/usr/bin/env python3
"""vcrphases.py - where did the box stop? The FLUSHED phase history.

The flight recorder lives in RAM and dies with a power cycle; the phases do
not. The miniport writes each boot phase and each SLI/AA milestone to
Services\\vcrmp\\Diag (PhaseLog: 64 x {ms, code, a, b}, flushed), and at the
next DriverEntry copies the previous boot's history to Prev* before writing
its own. So after a wedge and a power cycle:

    vcrphases.py 192.168.1.124 --prev     # what the boot that hung got through
    vcrphases.py 192.168.1.124            # this boot

For an SLI_STEP phase, a = the vcr_sli.h step (named here) and b = chip << 24 |
register.
"""
import argparse
import asyncio
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
sys.path.insert(0, str(HERE))
import vcrlog  # noqa: E402

DIAG = r"SYSTEM\CurrentControlSet\Services\vcrmp\Diag"


def sli_steps():
    text = (KMD / "include" / "vcr_sli.h").read_text()
    return {int(c): (n, d) for n, c, d in
            re.findall(r'VCR_SLI_STEP\((VCR_SLI_S_\w+),\s*(\d+),\s*"([^"]*)"\)', text)}


def decode(values, prev):
    pre = "Prev" if prev else ""
    blob = values.get(f"{pre}PhaseLog")
    count = values.get(f"{pre}PhaseCount")
    if blob is None or count is None:
        return [f"no {pre}PhaseLog/{pre}PhaseCount in Diag"]
    data = bytes.fromhex(blob.replace(" ", ""))
    slots = len(data) // 16
    events = vcrlog.load_events()
    steps = sli_steps()
    out = []
    for i in range(max(0, count - slots), count):
        ms, code, a, b = struct.unpack_from("<4I", data, (i % slots) * 16)
        name = events.get(code, (f"code {code}",))[0]
        extra = ""
        if name == "SLI_STEP":
            s = steps.get(a)
            extra = f"  {s[0][10:] if s else a} chip {b >> 24} reg {b & 0xffffff:#x}"
        out.append(f"{i:4d} {ms / 1000:9.3f}s  {name:<16} a={a:#010x} b={b:#010x}{extra}")
    last = values.get(f"{pre}LastPhase")
    out.append(f"{pre}LastPhase {events.get(last, (last,))[0]}, a={values.get(pre + 'LastPhaseA')}"
               f", at {values.get(pre + 'LastPhaseMs')} ms"
               + (f", boot #{values.get('PrevBootCount')}" if prev else ""))
    return out


async def read_diag(host, port=9898):
    sys.path.insert(0, str(KMD.parents[1]))
    from client.retro_protocol import RetroConnection
    c = RetroConnection(host, port)
    await c.connect("retro-agent-secret", timeout=20)
    try:
        st, d = await c.send_command(f"REGREAD HKLM {DIAG}", timeout=30)
    finally:
        await c.close()
    j = json.loads(d.decode("latin1"))
    return {v["name"]: v.get("data") for v in j.get("values", [])}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--prev", action="store_true", help="the previous boot (kept at DriverEntry)")
    ap.add_argument("--port", type=int, default=9898)
    a = ap.parse_args()
    for ln in decode(asyncio.run(read_diag(a.host, a.port)), a.prev):
        print(ln)


if __name__ == "__main__":
    main()
