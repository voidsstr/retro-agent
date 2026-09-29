#!/usr/bin/env python3
"""triage32.py - what an XP 32-bit TRIAGE (mini) kernel dump DOES hold.

A minidump never contains vcr-kmd's flight recorder (no pool pages), so
vcrdump.py used to stop at "no vcr-kmd flight recorder found". It still holds
the crashing thread's context, a slice of its stack and the loaded-driver
list - enough to answer the first question about any crash on a vcr-kmd box:
WAS vcr-kmd EVEN LOADED, and whose code is on the stack?

Measured 2026-09-28 on .124: the only minidump on the box was a 0xEA
(THREAD_STUCK_IN_DEVICE_DRIVER) with 3dfxvs.dll - the VINTAGE display driver -
on its stack and no vcrmp.sys/vcrdd.dll loaded at all. It predated vcr-kmd, and
it was dated 2003 because the box's RTC resets and Windows writes the Save
Dump event before the agent's clockfix runs. Read this before blaming a dump on
today's driver.

Layouts: DUMP_HEADER32 (0x1000 bytes; context record at 0x320) followed by
TRIAGE_DUMP32; driver entries are DUMP_DRIVER_ENTRY32 = name offset + an
LDR_DATA_TABLE_ENTRY32 (DllBase +0x18, SizeOfImage +0x20), 0x4c bytes apart on
XP SP3; names are DUMP_STRINGs (ULONG length in WCHARs, UTF-16 text).

    triage32.py Mini*.dmp
"""
import struct
import sys

TRIAGE_FIELDS = ('ServicePackBuild', 'SizeOfDump', 'ValidOffset', 'ContextOffset',
                 'ExceptionOffset', 'MmOffset', 'UnloadedDriversOffset', 'PrcbOffset',
                 'ProcessOffset', 'ThreadOffset', 'CallStackOffset', 'SizeOfCallStack',
                 'DriverListOffset', 'DriverCount', 'StringPoolOffset', 'StringPoolSize',
                 'BrokenDriverOffset', 'TriageOptions', 'TopOfStack')
DRIVER_STRIDES = (0x4c, 0x54, 0x58, 0x50, 0x5c, 0x60)
VCR_KMD = ('vcrmp.sys', 'vcrdd.dll')
DISPLAY_HINTS = ('vcr', '3dfx', 'nv4', 'ati', 'ialm', 'vga', 'dxg', 'win32k', 'videoprt')


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _dump_string(d, off):
    n = _u32(d, off)
    if n > 260 or off + 4 + 2 * n > len(d):
        return None
    try:
        return d[off + 4: off + 4 + 2 * n].decode('utf-16-le')
    except UnicodeDecodeError:
        return None


def parse(d):
    """{bugcheck, dump_type, triage, context, modules[(base,size,name)],
    stack_base, stack} of a 32-bit triage dump, or raise ValueError."""
    if d[:8] != b'PAGEDUMP':
        raise ValueError('not a 32-bit kernel dump (no PAGEDUMP signature)')
    dump_type = _u32(d, 0xf88)
    if dump_type != 4:
        raise ValueError('dump type %d is not a triage (mini) dump' % dump_type)
    bc = tuple(_u32(d, 0x28 + 4 * i) for i in range(5))
    tri = {n: _u32(d, 0x1000 + 4 * i) for i, n in enumerate(TRIAGE_FIELDS)}
    co = tri['ContextOffset']
    ctx = {'eip': _u32(d, co + 0xb8), 'esp': _u32(d, co + 0xc4), 'ebp': _u32(d, co + 0xb4)}
    sp0 = tri['StringPoolOffset']
    sp1 = sp0 + tri['StringPoolSize']
    mods = []
    for stride in DRIVER_STRIDES:
        got = []
        for i in range(tri['DriverCount']):
            e = tri['DriverListOffset'] + i * stride
            if e + 4 + 0x24 > len(d):
                break
            noff = _u32(d, e)
            name = _dump_string(d, noff) if sp0 <= noff < sp1 else None
            if name is None:
                break
            got.append((_u32(d, e + 4 + 0x18), _u32(d, e + 4 + 0x20), name))
        if got and len(got) == tri['DriverCount']:
            mods = got
            break
    cs, cn = tri['CallStackOffset'], tri['SizeOfCallStack']
    return {'bugcheck': bc, 'dump_type': dump_type, 'triage': tri, 'context': ctx,
            'modules': mods, 'stack_base': tri['TopOfStack'], 'stack': d[cs:cs + cn]}


def where(mods, addr):
    for base, size, name in mods:
        if base <= addr < base + size:
            return name, addr - base
    return None


def stack_hits(t, limit=64):
    """(va, value, module, offset) for every stack dword that points into a
    loaded module - a symbol-free stack walk, return addresses and all."""
    out = []
    s = t['stack']
    for i in range(0, len(s) - 3, 4):
        v = struct.unpack_from('<I', s, i)[0]
        w = where(t['modules'], v)
        if w:
            out.append((t['stack_base'] + i, v, w[0], w[1]))
            if len(out) >= limit:
                break
    return out


def verdict(t):
    """What this dump says about vcr-kmd - the line a reader needs first."""
    names = {n.lower() for _b, _s, n in t['modules']}
    loaded = [m for m in VCR_KMD if m in names]
    display = sorted({h[2] for h in stack_hits(t, 400)
                      if any(k in h[2].lower() for k in ('vcr', '3dfx', 'nv4', 'ati', 'ialm'))})
    if not t['modules']:
        return 'driver list unreadable - cannot say whether vcr-kmd was loaded'
    if not loaded:
        return ('vcr-kmd was NOT loaded (%s on the stack) - this crash is not vcr-kmd\'s'
                % (', '.join(display) or 'no display driver'))
    return 'vcr-kmd loaded (%s); display code on the stack: %s' % (
        ', '.join(loaded), ', '.join(display) or 'none')


def summary(d, out=sys.stdout):
    t = parse(d)
    out.write('bugcheck 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)\n' % t['bugcheck'])
    out.write('%s\n' % verdict(t))
    w = where(t['modules'], t['context']['eip'])
    out.write('EIP 0x%08x -> %s\n' % (t['context']['eip'],
                                        '%s+0x%x' % w if w else '?'))
    for base, size, name in sorted(t['modules']):
        if any(k in name.lower() for k in DISPLAY_HINTS):
            out.write('  %08x %08x %s\n' % (base, base + size, name))
    for va, v, name, off in stack_hits(t):
        out.write('  [%08x] %08x  %s+0x%x\n' % (va, v, name, off))
    return t


if __name__ == '__main__':
    summary(open(sys.argv[1], 'rb').read())
