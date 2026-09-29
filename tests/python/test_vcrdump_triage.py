"""vcrdump.py on a MINIDUMP: say whether vcr-kmd was even loaded (2026-09-28).

A minidump never holds vcr-kmd's flight recorder, and vcrdump.py used to stop at
"no vcr-kmd flight recorder found". On .124 the only dump on the box was a 0xEA
with 3dfxvs.dll - the VINTAGE display driver - on its stack and no vcr-kmd
module loaded: a crash from before vcr-kmd, dated 2003 by a resetting RTC, that
read at first glance like tonight's. voodoo-cleanroom/vcr-kmd/tools/triage32.py
reads what a triage dump does hold. These tests build one byte by byte.
"""
import io
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', '..', 'voodoo-cleanroom', 'vcr-kmd', 'tools')
sys.path.insert(0, os.path.normpath(TOOLS))
import triage32  # noqa: E402

TOP = 0xf2280000


def _dump(modules, stack, eip=0x804d7100, dump_type=4, stride=0x4c):
    d = bytearray(0x6000)
    d[0:8] = b'PAGEDUMP'
    struct.pack_into('<5I', d, 0x28, 0x100000EA, 0x8142EB30, 0x8149FAC8, 0xF9C47CB4, 1)
    struct.pack_into('<I', d, 0xf88, dump_type)
    struct.pack_into('<I', d, 0x320 + 0xb8, eip)
    struct.pack_into('<I', d, 0x320 + 0xc4, TOP)
    pool, lst, cs = 0x5000, 0x3000, 0x2000
    names, off = [], pool
    for _b, _s, n in modules:
        names.append(off)
        struct.pack_into('<I', d, off, len(n))
        enc = n.encode('utf-16-le')
        d[off + 4: off + 4 + len(enc)] = enc
        off += 4 + len(enc) + 4
    for i, (b, s, _n) in enumerate(modules):
        e = lst + i * stride
        struct.pack_into('<I', d, e, names[i])
        struct.pack_into('<I', d, e + 4 + 0x18, b)
        struct.pack_into('<I', d, e + 4 + 0x20, s)
    for i, v in enumerate(stack):
        struct.pack_into('<I', d, cs + 4 * i, v)
    tri = dict(ContextOffset=0x320, CallStackOffset=cs, SizeOfCallStack=4 * len(stack),
               DriverListOffset=lst, DriverCount=len(modules), StringPoolOffset=pool,
               StringPoolSize=off - pool, TopOfStack=TOP)
    for i, f in enumerate(triage32.TRIAGE_FIELDS):
        struct.pack_into('<I', d, 0x1000 + 4 * i, tri.get(f, 0))
    return bytes(d)


NT = (0x804d7000, 0x216680, 'ntoskrnl.exe')
VINTAGE = (0xbf012000, 0x94fc0, '3dfxvs.dll')
OURS = (0xbf012000, 0x40000, 'vcrdd.dll')
MP = (0xf904f000, 0x2aa80, 'vcrmp.sys')


def test_the_124_dump_reads_as_the_vintage_driver_not_vcr_kmd():
    t = triage32.parse(_dump([NT, VINTAGE], [0x12345678, 0xbf022106, 0x804e3963]))
    assert [n for _b, _s, n in t['modules']] == ['ntoskrnl.exe', '3dfxvs.dll']
    assert triage32.where(t['modules'], t['context']['eip']) == ('ntoskrnl.exe', 0x100)
    hits = triage32.stack_hits(t)
    assert (TOP + 4, 0xbf022106, '3dfxvs.dll', 0x10106) in hits
    v = triage32.verdict(t)
    assert 'NOT loaded' in v and '3dfxvs.dll' in v


def test_a_vcr_kmd_dump_says_so():
    t = triage32.parse(_dump([NT, OURS, MP], [0xbf012345]))
    v = triage32.verdict(t)
    assert v.startswith('vcr-kmd loaded') and 'vcrdd.dll' in v and 'vcrmp.sys' in v


def test_the_driver_entry_stride_is_found_not_assumed():
    t = triage32.parse(_dump([NT, VINTAGE], [], stride=0x54))
    assert len(t['modules']) == 2


def test_only_a_triage_dump_is_parsed():
    with pytest.raises(ValueError):
        triage32.parse(_dump([NT], [], dump_type=2))
    with pytest.raises(ValueError):
        triage32.parse(b'MDMP' + bytes(0x2000))


def test_summary_prints_the_verdict_first():
    out = io.StringIO()
    triage32.summary(_dump([NT, VINTAGE], [0xbf022106]), out)
    lines = out.getvalue().splitlines()
    assert lines[0].startswith('bugcheck 0x100000EA')
    assert 'NOT loaded' in lines[1]
