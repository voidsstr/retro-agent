"""The staged-library validator refuses an icon that XP cannot draw.

WHY. On 2026-09-28 .123 (XP) showed a blank generic page for Carmageddon,
Serious Sam TFE/TSE and Warcraft I/II although every launch.txt named an icon
that existed. The GOG / Serious Sam .ico files held only PNG-compressed images
(Vista+ only; XP has nothing it can decode) and Carmageddon's icon was a DOS4GW
binary with no resources. The validator only checked that the file existed.
`icon_xp_problem()` in scripts/validate-staged-library.py now checks the icon
is drawable; these tests pin both the fixed and the old-broken answers.
"""
import importlib.util
import os
import struct

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location(
    "validate_staged_library", os.path.join(REPO, "scripts", "validate-staged-library.py"))
vl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vl)


def _ico(entries):
    """entries: list of payload bytes; build an ICONDIR around them."""
    head = struct.pack("<HHH", 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    dirs, body = b"", b""
    for p in entries:
        dirs += struct.pack("<BBBBHHII", 32, 32, 0, 0, 1, 32, len(p), off + len(body))
        body += p
    return head + dirs + body


PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 32
DIB = struct.pack("<IiiHH", 40, 32, 64, 1, 32) + b"\0" * 64


def _write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return str(p)


def test_png_only_ico_fails(tmp_path):
    prob = vl.icon_xp_problem(_write(tmp_path, "gog.ico", _ico([PNG, PNG])))
    assert prob and prob[0] == "fail" and "PNG" in prob[1]


def test_ico_with_a_bitmap_entry_passes(tmp_path):
    # A Vista icon that ALSO carries BMP entries is fine: XP uses those.
    assert vl.icon_xp_problem(_write(tmp_path, "ok.ico", _ico([DIB, PNG]))) is None


def test_bmp_renamed_ico_is_a_warning(tmp_path):
    prob = vl.icon_xp_problem(_write(tmp_path, "d1x.ico", b"BM" + b"\0" * 60))
    assert prob and prob[0] == "warn"


def test_dos_exe_has_no_icon(tmp_path):
    # Carmageddon's MAINPROG.EXE: MZ, but an LE image - no PE header at all.
    dos = bytearray(b"MZ" + b"\0" * 0x80)
    struct.pack_into("<I", dos, 0x3C, 0x40)
    dos[0x40:0x42] = b"LE"
    prob = vl.icon_xp_problem(_write(tmp_path, "MAINPROG.EXE", bytes(dos)))
    assert prob and prob[0] == "fail"


def test_real_pe_with_icon_passes():
    # retro_chat.exe is tracked and carries the fleet icon.
    exe = os.path.join(REPO, "agent", "tools", "retro_chat.exe")
    assert vl.pe_has_icon(open(exe, "rb").read())
    assert vl.icon_xp_problem(exe) is None


def test_the_check_is_wired_into_check_title():
    src = open(os.path.join(REPO, "scripts", "validate-staged-library.py")).read()
    assert "prob = icon_xp_problem(ipath)" in src
