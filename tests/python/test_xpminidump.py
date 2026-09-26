"""scripts/fleet/xpminidump.py reads XP kernel minidumps with no debugger.

Written 2026-09-26 to read .143's 47 dumps: every crash since the GeForce 6800
went in (08-20..08-31) is 0xEA THREAD_STUCK_IN_DEVICE_DRIVER in nv4_disp.dll,
every July crash was in the Voodoo5's 3dfxv5d.dll. A wrong offset here would
blame the wrong driver, so this builds a synthetic dump with a known answer.
"""
import importlib.util
import os
import struct

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("xpm", os.path.join(REPO, "scripts", "fleet", "xpminidump.py"))
xpm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xpm)


def _dump(tmp_path):
    b = bytearray(0x4000)
    b[0:8] = b"PAGEDUMP"
    struct.pack_into("<I", b, 0x0C, 2600)
    struct.pack_into("<5I", b, 0x28, 0xEA, 0x82e4dda8, 0x82bbd9f0, 0xbf012346, 1)
    ctx, exc, cso, dlo, spo = 0x1100, 0x1300, 0x1400, 0x1800, 0x2000
    struct.pack_into("<19I", b, 0x1000, 0, len(b), 0, ctx, exc, 0, 0, 0, 0, 0,
                     cso, 16, dlo, 2, spo, 0x100, 0, 0, 0)
    for i, (base, size, name) in enumerate(((0x80400000, 0x200000, "ntoskrnl.exe"),
                                             (0xbf012000, 0x100000, "nv4_disp.dll"))):
        e = dlo + i * 0x4C
        noff = spo + i * 0x40
        struct.pack_into("<I", b, e, noff)
        struct.pack_into("<3I", b, e + 4 + 0x18, base, base, size)
        n = name.encode("utf-16-le")
        struct.pack_into("<I", b, noff, len(name))
        b[noff + 4:noff + 4 + len(n)] = n
    struct.pack_into("<I", b, exc, 0x80000003)
    struct.pack_into("<I", b, exc + 0x0C, 0xbf0880e2)
    struct.pack_into("<I", b, ctx + 0xB8, 0xbf0880e2)
    struct.pack_into("<4I", b, cso, 0x80401234, 0xbf0880e2, 0x12345678, 0)
    p = tmp_path / "Mini.dmp"
    p.write_bytes(bytes(b))
    return str(p)


def test_bugcheck_and_faulting_driver(tmp_path):
    r = xpm.parse(_dump(tmp_path))
    assert r["code"] == 0xEA and xpm.BUGCHECKS[0xEA] == "THREAD_STUCK_IN_DEVICE_DRIVER"
    assert r["exc_module"] == "nv4_disp.dll+0x760e2"
    assert r["eip_module"] == "nv4_disp.dll+0x760e2"
    assert r["param_modules"][2] == "nv4_disp.dll+0x346"


def test_stack_names_drivers_but_not_the_kernel(tmp_path):
    assert xpm.parse(_dump(tmp_path))["stack_drivers"] == ["nv4_disp.dll"]


def test_a_non_dump_is_refused_not_misread(tmp_path):
    p = tmp_path / "x.dmp"
    p.write_bytes(b"MDMP" + b"\0" * 100)
    assert "error" in xpm.parse(str(p))
