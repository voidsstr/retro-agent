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

Diag\\SliAAState, when present, is decoded after the phases: every chip's
SLI/AA config space as the kernel read it back - by config cycles only - right
after the last AA enable (vcr_sli.h vcr_sli_aa_state). It is not per boot: its
own header says which boot wrote it. Compare it with the expected tables in
tests/native/test_vcr_kmd_sli.c.
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
    if name == "SET_BEGIN" and b & 0x100:
        return f"SET_BEGIN chip {b >> 24} reg {b & 0xffffff:#x} (vendor AA recipe)"
    if name == "AA_STATE":
        return (f"AA_STATE read back ({b >> 24} chips)" if b & 1
                else f"AA_STATE reading chip {b >> 24} (config cycles)")
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
    if why == "MEMINFO":
        return f"REFUSED MEMINFO tileMark {val << 12:#x} (vendor AA recipe)"
    return f"REFUSED {why} value {val:#x}"


# ---- Diag\SliAAState (vcr_sli.h vcr_sli_aa_state) ----------------------------------
STATE_MAGIC = 0x31414153          # VCR_SLI_STATE_MAGIC, "SAA1"
STATE_BYTES = 224                 # VCR_SLI_STATE_BYTES
STATE_HDR = ("magic", "size", "boot", "ms", "tuple", "flags", "result", "nchips",
             "nlines", "bpp", "tile", "total", "col", "dbeg", "dend", "reserved")
STATE_CFG = (0x40, 0x48, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0xac)   # vcrmp_sli.c k_state_cfg
STATE_CFG_NAMES = ("initEn", "pciDec", "vidCtrl0", "vidCtrl1", "vidCtrl2", "sliLfb",
                   "aaDepth", "aaLfb", "sliAAMisc")
F_VENDOR_AA = 0x1                 # VCR_SLI_F_VENDOR_AA


def aalfb_text(v):
    """cfgAALfbCtrl, field by field (vcr_sli.h): base bits 4-25, CPU/dispatch
    write, AA read enable, read format, divide by 4."""
    parts = [f"base {v & 0x03fffff0:#x}"]
    parts += [n for bit, n in ((26, "cpuWr"), (27, "dispWr"), (28, "READ_EN"), (31, "div4"))
              if v >> bit & 1]
    parts.append(("16bpp", "15bpp", "32bpp", "fmt3?")[(v >> 29) & 3])
    return " ".join(parts)


def decode_state(blob_hex, warns=None):
    """Diag\\SliAAState as lines; says so, rather than guessing, when it is not one."""
    data = bytes.fromhex(blob_hex.replace(" ", ""))
    if len(data) < 64:
        return [f"SliAAState: {len(data)} bytes - too short for a vcr-kmd state record"]
    h = dict(zip(STATE_HDR, struct.unpack_from("<16I", data, 0)))
    if h["magic"] != STATE_MAGIC or h["size"] != STATE_BYTES or len(data) < STATE_BYTES:
        return [f"SliAAState: not a vcr-kmd state record (magic {h['magic']:#010x}, "
                f"size {h['size']}, {len(data)} bytes)"]
    warns = warns if warns is not None else sli_defines("W")
    result = h["result"] - (1 << 32) if h["result"] & 0x80000000 else h["result"]
    recipe = "vendor" if h["flags"] & F_VENDOR_AA else "dos_mode"
    res = warn_names(result, warns) if result >= 0 else f"{result} (refused)"
    out = [f"SliAAState: boot #{h['boot']} at {h['ms'] / 1000:.3f}s, request {shape(h['tuple'])} "
           f"nlines {h['nlines']} bpp {h['bpp']}, recipe {recipe}, result {res}",
           f"  memory: tileMark {h['tile']:#010x} total {h['total']:#010x} col {h['col']:#010x} "
           f"depth {h['dbeg']:#010x}-{h['dend']:#010x}",
           "  chip  " + " ".join(f"{n:>10}" for n in STATE_CFG_NAMES) + "   pciInit0"]
    n = min(h["nchips"], 4)
    for c in range(n):
        regs = struct.unpack_from("<10I", data, 64 + c * 40)
        pci0 = (f"{regs[9]:08x} (written)" if h["flags"] & (0x100 << c) else "not recorded")
        out.append(f"  {c:<4}  " + " ".join(f"  {v:08x}" for v in regs[:9]) + f"   {pci0}")
    for c in range(n):
        out.append(f"  chip {c} aaLfbCtrl: {aalfb_text(struct.unpack_from('<I', data, 64 + c * 40 + 28)[0])}")
    return out


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
    values = asyncio.run(read_diag(a.host, a.port))
    for ln in decode(values, a.prev):
        print(ln)
    if values.get("SliAAState"):
        for ln in decode_state(values["SliAAState"]):
            print(ln)


if __name__ == "__main__":
    main()
