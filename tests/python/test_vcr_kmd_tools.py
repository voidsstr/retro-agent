"""The vcr-kmd host tools: the flight-recorder decoder, the crash-dump reader
and the XP import check (voodoo-cleanroom/vcr-kmd/tools/).

The dump test is a cross-language contract: a ring WRITTEN by the C code the
miniport runs (common/vcr_log.c, compiled for the host) is embedded in a fake
XP kernel dump and must be FOUND and DECODED by vcrdump.py - wrapped, with a
torn record, behind a PAGEDUMP header carrying a bugcheck.
"""
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
KMD = REPO / "voodoo-cleanroom" / "vcr-kmd"
sys.path.insert(0, str(KMD / "tools"))

import vcrlog  # noqa: E402
import vcrdump  # noqa: E402


def test_every_event_in_the_header_is_decoded():
    events = vcrlog.load_events()
    lines = [ln for ln in (KMD / "include" / "vcr_events.h").read_text().splitlines()
             if "VCR_EVENT(VCR_EV_" in ln]
    assert len(events) == len(lines) > 40
    assert events[300][0] == "MODESET_BEGIN"
    assert events[800][0] == "SAFE_DECLINE"
    assert events[600][0] == "HWC_REQUEST"


def test_codes_are_unique_and_never_renumbered():
    """Old dumps must keep decoding: codes only ever get appended."""
    text = (KMD / "include" / "vcr_events.h").read_text()
    import re
    codes = [int(m) for m in re.findall(r"VCR_EVENT\(VCR_EV_\w+,\s*(\d+),", text)]
    assert len(codes) == len(set(codes))
    pinned = {"DRIVER_ENTRY": 100, "FIND_ENTER": 101, "MODESET_DONE": 303,
              "IOCTL": 400, "DD_ENABLE_SURF": 502, "HWC_LINADDR": 602,
              "SAFE_MARK_OK": 801, "MARK": 900}
    ev = vcrlog.load_events()
    by_name = {name: code for code, (name, _) in ev.items()}
    for name, code in pinned.items():
        assert by_name[name] == code


def test_tsv_from_vcrctl_parses():
    text = ("#vcrlog boot_count=4 version=1 entries=1024 next_seq=3 created=0\n"
            "1\t10\t1\t2\t101\t4\t00000001\t00000003\t00000000\t00000000\tPHASE find adapter\n"
            "2\t12\t2\t0\t508\t88\t00000005\t00000000\t00000000\t00000000\tMAP_VIDEO_MEMORY failed\n")
    hdr, ents = vcrlog.parse_tsv(text)
    assert hdr["boot_count"] == "4"
    assert [e["seq"] for e in ents] == [1, 2]
    line = vcrlog.format_entry(ents[1], vcrlog.load_events())
    assert "DD_FAIL" in line and "ERR" in line and "MAP_VIDEO_MEMORY failed" in line


RING_WRITER = r"""
#include <stdio.h>
#include <string.h>
#include "%(kmd)s/common/vcr_log.c"
static unsigned char ring[VCR_LOG_RING_BYTES(8)];
int main(int argc, char **argv) {
    vcr_log_ring *r = (vcr_log_ring *)ring;
    unsigned i;
    char m[32];
    vcr_log_init(r, 8, 7, 0x100, 0, 0);
    for (i = 1; i <= 11; i++) {
        sprintf(m, "entry %%u", i);
        vcr_log_put(r, i * 10, 300, 1, 2, 4, i, 0, 0, 0, m);
    }
    r->e[(11 - 1) %% 8].seq = 0;      /* the crash tore the newest record */
    fwrite(ring, 1, sizeof ring, stdout);
    return 0;
}
"""


@pytest.fixture(scope="module")
def c_ring(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler")
    d = tmp_path_factory.mktemp("ring")
    src = d / "w.c"
    src.write_text(RING_WRITER % {"kmd": KMD})
    exe = d / "w"
    subprocess.run([cc, "-std=c11", "-o", str(exe), str(src)], check=True)
    return subprocess.run([str(exe)], check=True, capture_output=True).stdout


def test_a_ring_written_by_the_c_code_is_found_in_a_kernel_dump(c_ring, tmp_path, capsys):
    hdr = bytearray(b"PAGE" * 1024)                 # DUMP_HEADER32 is 0x1000
    hdr[0:8] = b"PAGEDUMP"
    struct.pack_into("<5I", hdr, 0x28, 0xEA, 0x81234567, 0x2, 0x3, 0x4)
    struct.pack_into("<I", hdr, 0xF88, 2)          # kernel dump
    junk = b"\x00" * 5000 + b"VCRKMD-FLIGHTREC" + b"\xff" * 64   # a bogus magic
    dump = bytes(hdr) + junk + c_ring + b"\x00" * 4096
    p = tmp_path / "MEMORY.DMP"
    p.write_bytes(dump)

    rings = vcrdump.find_rings(dump)
    assert len(rings) == 1                         # the bogus header is rejected
    ents = vcrdump.read_entries(dump, rings[0])
    # 11 written into 8 slots: 4..11 survive, 11 is torn -> 4..10
    assert [e["seq"] for e in ents] == [4, 5, 6, 7, 8, 9, 10]
    assert ents[0]["msg"] == "entry 4" and ents[0]["a"] == 4
    assert rings[0]["boot_count"] == 7

    assert vcrdump.main([str(p)]) == 0
    out = capsys.readouterr().out
    assert "bugcheck 0x000000EA" in out and "kernel" in out and "MODESET_BEGIN" in out


def test_a_minidump_says_the_ring_is_not_there(tmp_path, capsys):
    hdr = bytearray(b"PAGE" * 1024)
    hdr[0:8] = b"PAGEDUMP"
    struct.pack_into("<I", hdr, 0xF88, 4)
    p = tmp_path / "Mini.dmp"
    p.write_bytes(bytes(hdr))
    assert vcrdump.main([str(p)]) == 1
    out = capsys.readouterr().out
    assert "minidump" in out and "no vcr-kmd flight recorder" in out


def test_the_xp_export_tables_catch_the_memcmp_trap():
    """mingw's libntoskrnl.a offers memcmp; XP SP3's ntoskrnl does not export
    it, so a driver importing it links and then fails to load at boot."""
    import check_imports
    nt = check_imports.xp_exports("ntoskrnl.exe")
    assert nt and "memcpy" in nt and "memcmp" not in nt
    assert "ZwFlushKey" in nt and "PsGetProcessCreateTimeQuadPart" in nt
    w32k = check_imports.xp_exports("win32k.sys")
    assert "EngCreateBitmap" in w32k and "memcpy" not in w32k
    assert "VideoPortMapMemory" in check_imports.xp_exports("videoprt.sys")


@pytest.mark.parametrize("name", ["vcrmp.sys", "vcrdd.dll"])
def test_the_built_drivers_load_on_xp(name):
    import check_imports
    f = KMD / "out" / name
    if not f.exists():
        pytest.skip(f"{f} not built (make -C voodoo-cleanroom/vcr-kmd)")
    n, problems = check_imports.check(str(f))
    assert not problems, problems
    assert n > 5
