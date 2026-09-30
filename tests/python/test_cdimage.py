"""scripts/fleet/cdimage/cdimage.c - an exact .iso of a data CD from the raw volume.

Written 2026-09-29 to image the Windows 98 SE CD in .110 onto the NAS for the
86Box Win98 build VM. What it must never do is hand back a PLAUSIBLE image that
is not the disc: a sector the drive cannot read ends the run and deletes the
partial file (a padded or truncated ISO installs 'fine' until setup hits the
hole). It copies what the ISO9660 volume descriptor says the volume spans,
which is what carries the El Torito boot record and boot image, and hashes
exactly the bytes it wrote so the copy on the share can be checked elsewhere.
"""
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "cdimage", "cdimage.c")


def _src():
    return open(SRC).read()


def test_an_unreadable_sector_fails_and_removes_the_partial_image():
    s = _src()
    i = s.index("if (rd(h, lba, n, buf))")
    block = s[i:s.index("CryptHashData(ch, buf, n * SEC, 0);", i)]
    assert "unreadable" in block and "DeleteFileA(part)" in block and "return 2" in block
    assert "t2 < 10" in block, "single-sector retries before giving up"
    assert re.search(r"for \(t = 0; t < 3; t\+\+\)", s), "three tries per read"


def test_length_comes_from_the_primary_volume_descriptor():
    s = _src()
    assert 'memcmp(buf + 1, "CD001", 5)' in s and "buf[0] != 1" in s
    assert "vol = buf[80] | (buf[81] << 8) | (buf[82] << 16)" in s   # volume space size, LE
    assert "CREATE_NEW" in s, "never overwrite an existing image"


def test_the_image_gets_its_final_name_only_when_complete():
    """2026-09-29: the network dropped mid-run and a truncated file sat on the
    share under the final name. Writes go to <out>.partial; the rename is the
    last thing, after every sector and the hash."""
    s = _src()
    assert '"%s.partial"' in s
    create = s.index("out = CreateFileA(part,")
    rename = s.index("MoveFileA(part, argv[2])")
    assert create < s.index("CryptGetHashParam") < rename
    assert "CreateFileA(argv[2]" not in s, "never write the final name directly"
    assert "CALG_MD5" in s


def test_it_imports_only_what_windows_xp_resolves(tmp_path):
    cc = shutil.which("i686-w64-mingw32-gcc")
    od = shutil.which("i686-w64-mingw32-objdump")
    if not cc or not od:
        pytest.skip("no mingw - cdimage was NOT built")
    out = tmp_path / "cdimage.exe"
    subprocess.run([cc, "-O2", "-o", str(out), SRC, "-ladvapi32"], check=True)
    dump = subprocess.run([od, "-p", str(out)], capture_output=True, text=True, check=True).stdout
    dlls = {d.lower() for d in re.findall(r"DLL Name: (\S+)", dump)}
    assert dlls <= {"kernel32.dll", "advapi32.dll", "msvcrt.dll"}, dlls
    assert re.search(r"MajorSubsystemVersion\s+4", dump), "Vista-only subsystem would not load on XP"
