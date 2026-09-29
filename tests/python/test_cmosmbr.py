"""scripts/fleet/win9x/cmosmbr - the boot-disk stage that keeps .243 bootable with its 80 GB disk fitted.

WHY (2026-09-29): .243's CMOS battery is dead. Every power loss makes POST
reload Compaq defaults: 1Bh (secondary IDE master type) goes to auto (44h), POST
types the 80 GB disk with a translation that overflows to 256 heads, and IO.SYS
hangs at a blinking cursor. Measured: disk removed -> boots; disk fitted after a
power-off -> hangs. With 1Bh = 00 Windows drives all 80 GB natively, and a warm
POST does not auto-type the drive. The stage, in the MBR code area, puts 1Bh
back to 00 (and 2Dh bit 3, skip F1) and warm-resets once, before any OS runs.

These tests boot the REAL bytes in QEMU (the production build must do nothing on
a non-Compaq ROM; the TEST=2 build is the production logic with the ROM strings
read from RAM, so the Compaq branch really runs) and drive the REAL installer
through INT 13h in DOSBox. Each skips LOUDLY when its emulator is missing.
"""
import os
import shutil
import struct
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(REPO, "scripts", "fleet", "win9x", "cmosmbr")
sys.path.insert(0, os.path.join(D, "test"))
from mkdisk import merge  # noqa: E402

STAMP = bytes([0, 0, 0x80, 0x11, 0x22, 0x33])
SAVE = 32                        # LBA of the saved original MBR (Makefile SAVESEC = 33)
# .243's LBA 1 and 2 hold an OLD DOS MBR with a stale table (dumped 2026-09-29);
# nothing may write them.
STALE = b"\x33" * 510 + b"\x55\xaa"


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    if not shutil.which("as") or not shutil.which("ld"):
        pytest.skip("no binutils - cmosmbr was NOT built or tested")
    out = tmp_path_factory.mktemp("cmosmbr")
    for f in ("cmosmbr.S", "Makefile"):
        shutil.copy(os.path.join(D, f), out)
    shutil.copytree(os.path.join(D, "test"), out / "test")
    subprocess.run(["make", "-s", "-C", str(out)], check=True)
    for name in ("origmbr", "vbr", "prep"):
        base = 0x7C00 if name == "vbr" else 0x600
        subprocess.run(["as", "--32", "-o", str(out / "test" / (name + ".o")),
                        str(out / "test" / (name + ".S"))], check=True)
        subprocess.run(["ld", "-m", "elf_i386", "-Ttext", hex(base), "--oformat", "binary",
                        "-o", str(out / "test" / (name + ".bin")),
                        str(out / "test" / (name + ".o"))], check=True, capture_output=True)
    return out


def _orig(built):
    o = bytearray((built / "test" / "origmbr.bin").read_bytes())
    o[0xDA:0xE0] = STAMP
    o[0x1BE:0x1CE] = struct.pack("<BBBBBBBBII", 0x80, 1, 1, 0, 6, 15, 63, 19, 63, 20000)
    return bytes(o)


def _disk(built, lba0, lba2=None, cyl=21):
    img = bytearray(512 * 16 * 63 * cyl)
    orig = _orig(built)
    img[0:512] = lba0
    img[512:1024] = STALE
    img[1024:1536] = STALE
    img[SAVE * 512:(SAVE + 1) * 512] = orig
    if lba2:
        img[1024:1536] = lba2           # the QEMU prep sector loads LBA 2 as 'the MBR'
    img[63 * 512:64 * 512] = (built / "test" / "vbr.bin").read_bytes()
    return img


def _boot(tmp_path, img):
    if not shutil.which("qemu-system-i386"):
        pytest.skip("no qemu-system-i386 - the stage was NOT booted")
    p, dbg = tmp_path / "d.img", tmp_path / "dbg.txt"
    p.write_bytes(img)
    subprocess.run(["timeout", "25", "qemu-system-i386", "-drive", "file=%s,format=raw" % p,
                    "-debugcon", "file:%s" % dbg, "-device", "isa-debug-exit,iobase=0xf4,iosize=1",
                    "-display", "none", "-m", "16"], capture_output=True)
    return dbg.read_text() if dbg.exists() else ""


def test_the_stage_fits_and_leaves_the_stamp_hole(built):
    code = (built / "cmosmbr.bin").read_bytes()
    assert len(code) == 440
    assert code[0xDA:0xE0] == bytes(6), "0xDA..0xDF must be left for the disk's own Win9x stamp"
    src = open(os.path.join(D, "cmosmbr.S")).read()
    mk = open(os.path.join(D, "Makefile")).read()
    assert "--defsym TEST=0 --defsym WANT1B=0x00 --defsym ROMSEG=0xF000" in mk
    assert '.ascii "COMPAQ"' in src and '.ascii "04/25/97"' in src
    # the production and ROM-in-RAM builds differ ONLY in the segment constant
    diff = [i for i, (a, b) in enumerate(zip(code, (built / "cmosmbr_rom.bin").read_bytes())) if a != b]
    assert diff == [36, 37], diff
    assert "SAVESEC = 33" in mk and "mov cx, SAVESEC" in src


def test_merge_keeps_the_disks_stamp_signature_and_table(built):
    code, orig = (built / "cmosmbr.bin").read_bytes(), _orig(built)
    m = merge(code, orig)
    assert m[0xDA:0xE0] == STAMP and m[0x1B8:] == orig[0x1B8:] and m[:0xDA] == code[:0xDA]


def test_on_another_rom_it_changes_nothing_and_chains(built, tmp_path):
    out = _boot(tmp_path, _disk(built, merge((built / "cmosmbr.bin").read_bytes(), _orig(built))))
    # O = the original MBR ran, V = the VBR, then 1Bh, 2Dh, checksum mark, stamp
    assert out.startswith("OV") and out.strip().endswith("000080112233"), out
    assert out[2:4] != "00", "1Bh must not have been touched on a non-Compaq ROM: " + out


def test_on_the_compaq_rom_it_clears_1b_sets_skip_f1_and_resets_once(built, tmp_path):
    rom = merge((built / "cmosmbr_rom.bin").read_bytes(), _orig(built))
    out = _boot(tmp_path, _disk(built, (built / "test" / "prep.bin").read_bytes(), lba2=rom))
    # PQ = prep (boot 1: stage writes + resets), PQ (boot 2: marker -> chain), O, V
    assert out.strip() == "PQPQOV0008K000080112233", out


def test_a_bad_checksum_is_refused(built, tmp_path):
    # QEMU's own CMOS checksum is invalid: the ROM-in-RAM build without the prep
    # fixer must refuse to write anything, and still boot.
    rom = merge((built / "cmosmbr_rom.bin").read_bytes(), _orig(built))
    out = _boot(tmp_path, _disk(built, rom))
    assert out.startswith("OV") and "X" in out and out[2:4] != "00", out


def _dosbox(tmp_path, img, cmd):
    if not shutil.which("dosbox"):
        pytest.skip("no dosbox - CMOSINST was NOT exercised")
    c = tmp_path / "C" / "CMOSMBR"
    c.mkdir(parents=True, exist_ok=True)
    shutil.copy(os.path.join(D, "CMOSINST.EXE"), c)
    disk = tmp_path / "disk.img"
    if img is not None:
        disk.write_bytes(img)
    conf = tmp_path / "d.conf"
    conf.write_text("[sdl]\noutput=surface\n[dosbox]\nmemsize=16\n[cpu]\ncycles=max\n"
                    "[mixer]\nnosound=true\n[autoexec]\nMOUNT C \"%s\"\n"
                    "imgmount 2 \"%s\" -t hdd -fs none -size 512,63,16,21\nC:\ncd CMOSMBR\n"
                    "CMOSINST %s > C:\\OUT.TXT\nexit\n" % (tmp_path / "C", disk, cmd))
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", HOME=str(tmp_path))
    subprocess.run(["timeout", "-s", "KILL", "60", "dosbox", "-conf", str(conf), "-userconf-skip"],
                   env=env, capture_output=True)
    return (tmp_path / "C" / "OUT.TXT").read_text(errors="replace"), disk.read_bytes()


def test_installer_install_is_idempotent_and_remove_restores(built, tmp_path):
    orig = _orig(built)
    img = _disk(built, orig)
    img[SAVE * 512:(SAVE + 1) * 512] = bytes(512)   # the save sector empty, as on .243
    out, d1 = _dosbox(tmp_path, img, "INSTALL")
    assert "INSTALLED" in out, out
    assert d1[:512] == merge(open(os.path.join(D, "cmosmbr.bin"), "rb").read(), orig)
    assert d1[SAVE * 512:(SAVE + 1) * 512] == orig
    assert d1[512:1536] == STALE * 2, "LBA 1..2 must never be written"
    out, d2 = _dosbox(tmp_path, None, "INSTALL")
    assert "already installed - nothing written" in out and d2 == d1
    out, d3 = _dosbox(tmp_path, None, "REMOVE")
    assert "REMOVED" in out and d3[:512] == orig and d3[SAVE * 512:(SAVE + 1) * 512] == bytes(512)
    assert d3[512:1536] == STALE * 2


def test_installer_refuses_a_used_save_sector(built, tmp_path):
    orig = _orig(built)
    img = _disk(built, orig)
    img[SAVE * 512:(SAVE + 1) * 512] = b"\x01" * 512  # something lives in the save sector
    out, d = _dosbox(tmp_path, img, "INSTALL")
    assert "REFUSED: the save sector is not empty" in out and d == bytes(img)


def _real_disk(built, lba0, lba2=None):
    """.243's REAL boot-disk MBR (dumped by CMOSINST CHECK 2026-09-29: the Win98
    FDISK MBR, FAT32-LBA partition at 63) and its real stale LBA 1."""
    real = open(os.path.join(D, "test", "mbr243.bin"), "rb").read()
    img = bytearray(512 * 16 * 63 * 21)
    img[0:512] = lba0(real) if callable(lba0) else lba0
    img[512:1024] = open(os.path.join(D, "test", "lba1-243.bin"), "rb").read()
    img[1024:1536] = lba2(real) if callable(lba2) else (lba2 or STALE)
    img[SAVE * 512:(SAVE + 1) * 512] = real
    img[63 * 512:64 * 512] = (built / "test" / "vbr.bin").read_bytes()
    return img, real


def test_243s_real_mbr_chains_behind_the_stage(built, tmp_path):
    code = (built / "cmosmbr.bin").read_bytes()
    img, real = _real_disk(built, lambda r: merge(code, r))
    out = _boot(tmp_path, img)
    # no 'O' - the real MBR prints nothing; V = our VBR reached through it; the
    # stamp at 0x6DA is the real one (00 00 80 57 42 00) after ITS relocation
    assert out.startswith("V") and out.strip().endswith("000080574200"), out
    assert out[1:3] != "00", out


def test_243s_real_mbr_on_the_compaq_path(built, tmp_path):
    rom = (built / "cmosmbr_rom.bin").read_bytes()
    img, _ = _real_disk(built, (built / "test" / "prep.bin").read_bytes(), lba2=lambda r: merge(rom, r))
    out = _boot(tmp_path, img)
    assert out.strip() == "PQPQV0008K000080574200", out
