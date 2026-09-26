"""ide9x - direct-port IDE reader for the secondary channel - is read-only and
never touches the primary channel (2026-09-25).

Written to identify a disk on .243 that the BIOS showed and Win98 could not
read ("ESDI BIOS read failure"): VWIN32's INT 13h does not serve hard disks on
9x and FDISK stalled the box for five minutes probing it through the BIOS. It
does port I/O from ring 3, next to Win98's own IDE driver, on a box that needs
a person to recover, so its guarantees are pinned here:
  - ports: only the secondary channel (170h-177h, 376h) and PCI config READS;
    never 1F0h-1F7h/3F6h where C: and the DVD live;
  - ATA commands: only ECh IDENTIFY and 20h READ SECTORS;
  - device control: only 02h (nIEN) and 08h - never SRST (04h);
  - output: NEW files only, and never a DOS device name.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

SRC = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "ide9x.c").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)


def test_the_channel_is_the_secondary_and_the_primary_never_appears():
    assert re.search(r"#define IDE_BASE\s+0x170\b", CODE)
    assert re.search(r"#define IDE_CTRL\s+0x376\b", CODE)
    for primary in ("0x1F0", "0x1f0", "0x3F6", "0x3f6", "0x1F7", "0x1f7"):
        assert primary not in CODE, primary
    for reg, off in (("R_DATA", 0), ("R_ERROR", 1), ("R_COUNT", 2), ("R_LBA0", 3), ("R_LBA1", 4),
                     ("R_LBA2", 5), ("R_DEVHEAD", 6), ("R_CMDSTAT", 7)):
        assert re.search(r"#define %s\s+\(IDE_BASE \+ %d\)" % (reg, off), CODE), reg


def test_every_port_write_goes_to_a_named_secondary_register():
    ports = re.findall(r"\boutb\((\w+),", CODE)
    assert ports and set(ports) <= {"IDE_CTRL", "R_DEVHEAD", "R_COUNT", "R_LBA0", "R_LBA1", "R_LBA2",
                                    "R_CMDSTAT", "port"}, ports
    assert set(re.findall(r"\boutl\((\w+),", CODE)) <= {"PCI_ADDR", "port"}


def test_only_identify_and_read_sectors_can_be_commanded():
    assert re.search(r"#define ATA_IDENTIFY\s+0xEC\b", CODE)
    assert re.search(r"#define ATA_READ_SECTORS\s+0x20\b", CODE)
    cmds = re.findall(r"outb\(R_CMDSTAT,\s*(\w+)\)", CODE)
    assert cmds and set(cmds) == {"ATA_IDENTIFY", "ATA_READ_SECTORS"}, cmds


def test_device_control_is_never_given_srst():
    vals = re.findall(r"outb\(IDE_CTRL,\s*(\w+)\)", CODE)
    assert set(vals) <= {"CTL_NIEN", "CTL_IDLE"}, vals
    assert re.search(r"#define CTL_NIEN\s+0x02\b", CODE) and re.search(r"#define CTL_IDLE\s+0x08\b", CODE)


def test_output_is_create_new_only():
    assert "CREATE_ALWAYS" not in CODE and "OPEN_ALWAYS" in CODE   # the log only
    assert CODE.count("CREATE_NEW") >= 2


def test_a_command_is_only_issued_to_an_idle_device():
    sel = CODE[CODE.index("static int select_dev"):CODE.index("static int wait_data")]
    assert "ST_BSY | ST_DRQ" in sel and "drain(" in sel


@pytest.fixture(scope="module")
def safe_name(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - safe_name NOT exercised")
    start = SRC.index("static int safe_name")
    depth, i = 0, SRC.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(SRC[i], 0)
        if depth == 0:
            break
        i += 1
    d = tmp_path_factory.mktemp("safename")
    (d / "t.c").write_text("#include <stdio.h>\n#include <string.h>\n#define lstrcmpA strcmp\n"
                           + SRC[start:i + 1] +
                           "\nint main(int c,char**v){int i;for(i=1;i<c;i++)printf(\"%d\\n\",safe_name(v[i]));return 0;}\n")
    subprocess.run([cc, "-o", str(d / "t"), str(d / "t.c")], check=True)
    return lambda *names: [int(x) for x in subprocess.run([str(d / "t"), *names], capture_output=True,
                                                          text=True, check=True).stdout.split()]


def test_dos_device_names_and_paths_are_refused(safe_name):
    assert safe_name("LPT1", "com3.bin", "CON", "nul.txt", "aux", "PRN.BIN", "CLOCK$",
                     "a:b", "..\\x", "C:\\WINDOWS\\SYSTEM.INI", "") == [0] * 11
    assert safe_name("MBR.BIN", "P1BOOT.BIN", "root-dir_1.bin", "COM10", "LPT0") == [1] * 5
