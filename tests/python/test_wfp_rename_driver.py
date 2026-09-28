"""The WFP driver-rename tool must not ship a file Windows will reject.

WHY THIS EXISTS. On XP an unsigned display driver gets its PROTECTED files
silently reverted to the in-box SP3 copies by Windows File Protection, leaving a
new miniport driving an old display DLL and the box in VGA fallback (800x600x4,
ONE enumerated mode) while Device Manager reports status OK. Measured on .110
with Catalyst 10.2 on 2026-09-28; fleetbook recipe
`xp-unsigned-display-driver-wfp-reverts-protected-dlls-vga-fa`.

`scripts/fleet/wfp-rename-driver.py` beats that by renaming the files to names
WFP does not protect and patching the inter-DLL references in place. Three of
its properties are load-bearing and all three fail SILENTLY - the box just stays
in VGA, or bluescreens, and nothing points at the tool:

  1. the patched image must be the same SIZE (an in-place patch has no
     relocation, so equal-length names are the whole premise);
  2. the PE checksum must be RECOMPUTED - these load kernel-side under win32k;
  3. no reference to a protected name may survive, or that reference quietly
     pulls the reverted in-box DLL back into the stack.

The checksum routine is itself checked against real PE files' stored values,
because a checksum function that has never reproduced a known value is a guess.
"""

import importlib.util
import os
import struct
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(REPO, "scripts", "fleet", "wfp-rename-driver.py")


def _load():
    spec = importlib.util.spec_from_file_location("wfp_rename_driver", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


wfp = _load()


def _synthetic_pe(body=b""):
    """A minimal but structurally real PE: MZ stub, e_lfanew, PE signature,
    COFF header, and an optional header long enough to hold CheckSum at +64."""
    lfanew = 0x80
    buf = bytearray(b"\x00" * lfanew)
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, lfanew)
    buf += b"PE\0\0"
    buf += b"\x00" * 20                     # COFF header
    buf += b"\x00" * 224                    # optional header (CheckSum at +64)
    buf += body
    if len(buf) % 2:
        buf += b"\x00"
    data = bytearray(buf)
    off = wfp.checksum_offset(data)
    struct.pack_into("<I", data, off, wfp.pe_checksum(bytes(data), off))
    return bytes(data)


def test_checksum_routine_reproduces_its_own_stamp():
    data = _synthetic_pe(b"payload")
    off = wfp.checksum_offset(data)
    assert struct.unpack_from("<I", data, off)[0] == wfp.pe_checksum(data, off)


def test_checksum_changes_when_a_byte_changes():
    """If this ever passes trivially the tool could ship a stale checksum."""
    data = bytearray(_synthetic_pe(b"aaaaaaaa"))
    off = wfp.checksum_offset(data)
    before = wfp.pe_checksum(bytes(data), off)
    data[-1] ^= 0xFF
    assert wfp.pe_checksum(bytes(data), off) != before


def test_patch_preserves_size_and_replaces_both_encodings():
    body = b"ati3duag.dll" + b"\x00" + wfp._encode("ati2cqag.dll", True)
    data = _synthetic_pe(body)
    name_map = {"ati3duag.dll": "ati3dr10.dll", "ati2cqag.dll": "ati2cr10.dll"}
    out, count = wfp.patch_one(data, name_map)
    assert len(out) == len(data), "an in-place patch must not change the size"
    assert count == 2
    assert b"ati3dr10.dll" in out
    assert wfp._encode("ati2cr10.dll", True) in out
    assert b"ati3duag.dll" not in out
    assert wfp._encode("ati2cqag.dll", True) not in out


def test_patch_preserves_letter_case():
    """A case-sensitive consumer must still match after the rename."""
    data = _synthetic_pe(b"ATI3DUAG.DLL")
    out, _ = wfp.patch_one(data, {"ati3duag.dll": "ati3dr10.dll"})
    assert b"ATI3DR10.DLL" in out


def test_unequal_length_rename_is_refused(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    (src / "ati2dvag.dll").write_bytes(_synthetic_pe())
    proc = subprocess.run(
        [sys.executable, TOOL, "--src", str(src), "--out", str(tmp_path / "out"),
         "--map", "ati2dvag.dll=atidvr1.dll"],
        capture_output=True, text=True)
    assert proc.returncode != 0
    assert "length" in (proc.stderr + proc.stdout).lower()


def test_end_to_end_rewrites_refs_and_restamps_checksum(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    # ati2dvag references ati3duag, exactly as Catalyst's real pair does.
    (src / "ati2dvag.dll").write_bytes(_synthetic_pe(b"ati3duag.dll"))
    (src / "ati3duag.dll").write_bytes(_synthetic_pe(b"ati3duag.dll"))
    out = tmp_path / "out"
    proc = subprocess.run(
        [sys.executable, TOOL, "--src", str(src), "--out", str(out),
         "--map", "ati2dvag.dll=atidvr10.dll",
         "--map", "ati3duag.dll=ati3dr10.dll"],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr

    for name in ("atidvr10.dll", "ati3dr10.dll"):
        data = (out / name).read_bytes()
        assert b"ati3duag.dll" not in data, "a protected name survived the patch"
        off = wfp.checksum_offset(data)
        stored = struct.unpack_from("<I", data, off)[0]
        assert stored == wfp.pe_checksum(data, off), "checksum was not restamped"


CATALYST = os.path.expanduser("~/.retro-fleet/drivers/catalyst-10.2-xp")


@pytest.mark.skipif(not os.path.isdir(CATALYST),
                    reason="Catalyst 10.2 files not stashed on this host")
def test_checksum_routine_against_real_shipped_binaries():
    """The routine must reproduce the vendor's own stored checksums exactly."""
    names = ["ati2dvag.dll", "ati3duag.dll", "ati2cqag.dll", "ativvaxx.dll",
             "ati2mtag.sys"]
    paths = [os.path.join(CATALYST, n) for n in names
             if os.path.isfile(os.path.join(CATALYST, n))]
    assert paths, "stash present but empty"
    assert wfp.verify_checksums(paths)
