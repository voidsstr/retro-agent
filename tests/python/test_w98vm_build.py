"""scripts/vm/win98 - the Windows 98 SE build VM (86Box), built on the host.

Pins what the 2026-09-30 build measured, so a later edit cannot quietly undo
it: the answer-file template carries a PLACEHOLDER, never a key (the key is
pulled from the vault at build time); the floppy-chaining MBR is exactly 440
bytes and boots drive 00h; the VM uses the Cirrus GD5436 (86Box's S3 Trio64
went black under Win98's own S3 driver); the SB16 is 220/5/1/5 and the agent
is forwarded to 19930; and the copy is checked by BYTE TOTAL, parsed from
mtools' space-grouped digits (the first parse read "181" from "181 541 424").
"""
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(REPO, "scripts", "vm", "win98")


def _r(name):
    return open(os.path.join(D, name)).read()


def test_the_answer_file_template_holds_no_key():
    t = _r("msbatch.inf.in")
    assert 'ProductKey="@PRODUCTKEY@"' in t
    assert not re.search(r"[A-Z0-9]{5}(-[A-Z0-9]{5}){4}", t), "a literal key in the repo"
    b = _r("build-w98vm.sh")
    assert "keyvault.py', 'get', 'fleet-win98se-product-key'" in b and 'shred -u "$WORK/msbatch.inf"' in b


def test_the_vm_is_shaped_like_243_with_a_working_display():
    c = _r("86box.cfg.in")
    assert "gfxcard = cl_gd5436_pci" in c and "s3_trio64" not in c
    assert re.search(r"base = 0220\nirq = 5\ndma = 1\ndma16 = 5", c)
    assert "type = 2" in c and "voodoo = 1" in c                  # Voodoo 2
    assert "cpu_family = pentium_p54c" in c and "0_external = 19930" in c


def test_the_byte_total_parse_handles_mtools_digit_groups():
    b = _r("build-w98vm.sh")
    m = re.search(r"sed -n '(s/.*)' \| tail -1 \| tr -d ' '", b)
    assert m, "the byte-total parse"
    line = "      864 files         181 541 424 bytes\n"
    out = subprocess.run(["sed", "-n", m.group(1)], input=line, capture_output=True, text=True).stdout
    assert out.replace(" ", "").strip() == "181541424"
    assert '[ "$SRC" = "$GOT" ] || { echo "COPY SHORT' in b


def test_the_floppy_mbr_is_440_bytes_and_boots_drive_zero(tmp_path):
    if not shutil.which("as") or not shutil.which("ld"):
        pytest.skip("no binutils - flopmbr was NOT built")
    o, b = tmp_path / "f.o", tmp_path / "f.bin"
    subprocess.run(["as", "--32", "-o", str(o), os.path.join(D, "flopmbr.S")], check=True)
    subprocess.run(["ld", "-m", "elf_i386", "-Ttext", "0x600", "--oformat", "binary", "-o", str(b), str(o)],
                   check=True, capture_output=True)
    code = b.read_bytes()
    assert len(code) == 440
    assert b"\x31\xd2\xcd\x13" in code, "xor dx,dx ; int 13h - reads drive 00h (the floppy)"


def test_inject_refuses_a_running_vm_and_the_setup_floppy_stops_at_stage_two():
    assert "REFUSED: stop the VM first" in _r("inject-agent.sh")
    a = _r("setup-autoexec.bat")
    assert "IF EXIST C:\\WINDOWS\\WIN.COM GOTO STAGE2" in a and "A:\\FDISK.EXE /MBR" in a
    assert "SETUP.EXE C:\\WIN98\\MSBATCH.INF" in a
