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
register - except the steps whose VALUE is the point (vcr_sli_phase_b, kernels
from 2026-09-27 on, marked by b bit 23): SET_DONE / OFF_DONE carry the
VCR_SLI_W_* warn mask, CLOCK_6K the clock hook's result, NOMUX its flags, and
REFUSED the reason (in the chip byte) plus the request's shape {chips, sli, aa,
sampleHigh, analog}. A record without bit 23 predates that and says so rather
than showing "warn 0". HWC_SLIAA (the request as it arrived) and
SLI_POKE_REFUSED (a PCI_OP write to an SLI/AA register the kernel refused) are
decoded too.
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


def sli_defines(prefix):
    """{value: NAME} of the `#define VCR_SLI_<prefix>_NAME value` lines of vcr_sli.h
    (W = the warn bits, R = the refusal reasons)."""
    text = (KMD / "include" / "vcr_sli.h").read_text()
    return {int(v, 0): n for n, v in
            re.findall(rf"#define VCR_SLI_{prefix}_(\w+)\s+(0x[0-9a-fA-F]+|\d+)\b", text)}


PB_VALUE = 0x00800000          # vcr_sli.h VCR_SLI_PB_VALUE
VALUE_STEPS = ("SET_DONE", "OFF_DONE", "CLOCK_6K", "NOMUX", "REFUSED")


def shape(t):
    """A VCR_SLI_TUPLE, as {chips,sli,aa,sampleHigh,analog}."""
    return "{%d,%d,%d,%d,%d}" % ((t >> 16) & 0xf, (t >> 12) & 0xf, (t >> 8) & 0xf,
                                 (t >> 4) & 0xf, t & 0xf)


def warn_names(mask, warns):
    if not mask:
        return "0 (clean)"
    names = [n for bit, n in sorted(warns.items()) if mask & bit]
    rest = mask & ~sum(b for b in warns if mask & b)
    if rest:
        names.append(f"{rest:#x}")
    return f"{mask:#x} ({'|'.join(names)})"


def sli_step_text(a, b, steps, warns, reasons):
    s = steps.get(a)
    name = s[0][10:] if s else str(a)
    if name not in VALUE_STEPS:
        return f"{name} chip {b >> 24} reg {b & 0xffffff:#x}"
    if not b & PB_VALUE:
        if name == "REFUSED":
            r = b & 0xffffff
            return f"REFUSED reason {reasons.get(r, r)} (value not recorded: kernel before 2026-09-27)"
        return f"{name} chip {b >> 24} (value not recorded: kernel before 2026-09-27)"
    val = b & 0x7fffff
    signed = val - 0x800000 if val & 0x400000 else val
    top = b >> 24
    if name in ("SET_DONE", "OFF_DONE"):
        return f"{name} chips {top} warn {warn_names(val, warns)}"
    if name == "CLOCK_6K":
        return f"CLOCK_6K result {signed} ({'programmed' if signed == 0 else 'NOT programmed'})"
    if name == "NOMUX":
        return (f"NOMUX chip {top} sli {val & 1} aa {(val >> 1) & 1} analog {(val >> 2) & 1} "
                f"sampleHigh {(val >> 4) & 0xf}")
    why = reasons.get(top, str(top))
    if why in ("COMBO", "AA_OFF"):
        return f"REFUSED {why} shape {shape(val)}"
    return f"REFUSED {why} value {val:#x}"


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
    warns, reasons = sli_defines("W"), sli_defines("R")
    out = []
    for i in range(max(0, count - slots), count):
        ms, code, a, b = struct.unpack_from("<4I", data, (i % slots) * 16)
        name = events.get(code, (f"code {code}",))[0]
        extra = ""
        if name == "SLI_STEP":
            extra = "  " + sli_step_text(a, b, steps, warns, reasons)
        elif name == "HWC_SLIAA":
            # vcrmp_multi.c: a = chips, b = sliEn | aaEn << 1 | sampleHigh << 4 | analog << 8
            t = ((a & 0xf) << 16 | (b & 1) << 12 | ((b >> 1) & 1) << 8 |
                 ((b >> 4) & 0xf) << 4 | ((b >> 8) & 0xf))
            extra = f"  request {shape(t)}"
        elif name == "SLI_POKE_REFUSED":
            extra = f"  chip {a >> 16} cfg {a & 0xffff:#04x} <- {b:#010x} refused"
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
