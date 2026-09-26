"""idewrite9x - the write-capable secondary-IDE tool - cannot reach any other
disk (2026-09-25).

It exists to wipe and format one disk on .243 (Seagate ST380013A, secondary
master) that no BIOS or Windows tool can reach. It writes raw sectors from
ring 3 next to Win98's own IDE driver, so the guards are pinned here:
  - ports: only the secondary channel (170h-177h, 376h); the primary channel
    (C: and the DVD) never appears;
  - device: the MASTER only - the device/head register is only ever given
    DEV_MASTER_LBA (E0h) plus LBA bits; no slave bit;
  - commands: only ECh IDENTIFY, 30h WRITE SECTORS, E7h FLUSH CACHE;
  - device control: only 02h / 08h, never SRST;
  - every write path runs verify_target() first, which refuses unless the
    drive's serial equals the one named on the command line;
  - writes are bounded by the IDENTIFY capacity.
"""
import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "idewrite9x.c").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)


def body(name):
    start = CODE.index(name)
    depth, i = 0, CODE.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(CODE[i], 0)
        if depth == 0:
            return CODE[start:i + 1]
        i += 1


def test_secondary_channel_only():
    assert re.search(r"#define IDE_BASE\s+0x170\b", CODE) and re.search(r"#define IDE_CTRL\s+0x376\b", CODE)
    for primary in ("0x1F0", "0x1f0", "0x3F6", "0x3f6", "0x1F7", "0x1f7"):
        assert primary not in CODE
    ports = set(re.findall(r"\bout[bw]\((\w+),", CODE))
    assert ports <= {"IDE_CTRL", "R_DEVHEAD", "R_COUNT", "R_LBA0", "R_LBA1", "R_LBA2", "R_CMDSTAT", "R_DATA",
                     "port"}, ports


def test_master_only():
    assert re.search(r"#define DEV_MASTER_LBA\s+0xE0\b", CODE)
    dev = re.findall(r"outb\(R_DEVHEAD,\s*([^;]+)\);", CODE)
    assert dev and all("DEV_MASTER_LBA" in d and "0x10" not in d for d in dev), dev


def test_only_identify_write_and_flush():
    cmds = set(re.findall(r"outb\(R_CMDSTAT,\s*(\w+)\)", CODE))
    assert cmds == {"ATA_IDENTIFY", "ATA_WRITE_SECTORS", "ATA_FLUSH_CACHE"}, cmds
    assert re.search(r"#define ATA_WRITE_SECTORS\s+0x30\b", CODE)
    assert re.search(r"#define ATA_FLUSH_CACHE\s+0xE7\b", CODE)
    assert set(re.findall(r"outb\(IDE_CTRL,\s*(\w+)\)", CODE)) <= {"CTL_NIEN", "CTL_IDLE"}


def test_every_write_path_checks_the_serial_first():
    for fn in ("static void do_zero", "static void do_put", "static void do_flush"):
        b = body(fn)
        assert "verify_target(serial)" in b, fn
        first_write = min(i for i in (b.find("write_run("), b.find("ATA_FLUSH_CACHE")) if i >= 0)
        assert b.index("verify_target(serial)") < first_write, fn
    v = body("static int verify_target")
    # an exact byte compare: lstrcmpA is a locale compare (word sort ignores hyphens)
    assert "!bytes_equal(serial, want)" in v and "return 0" in v
    assert "lstrcmpA" not in v


def test_writes_are_bounded_by_the_drive_capacity():
    wr = body("static int write_run")
    assert "lba >= capacity || count > capacity - lba" in wr
    assert "count == 0 || count > 256" in wr


def test_a_zero_run_can_be_stopped_and_reports_progress():
    """Killing a write mid-run leaves the drive in a data-out phase; the only
    sanctioned stop is the stop file, checked between 256-sector runs."""
    b = body("static void do_zero")
    assert "GetFileAttributesA(STOP_FILE)" in b
    assert b.index("GetFileAttributesA(STOP_FILE)") < b.index("write_run(")
    assert "done % 8192 == 0" in b


def test_both_ide_tools_share_one_channel_lock():
    """ide9x and idewrite9x must never program the secondary task file at once."""
    other = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "ide9x.c").read_text()
    for s in (SRC, other):
        assert 'CreateMutexA(NULL, TRUE, "retro_ide_secondary")' in s
        assert "ERROR_ALREADY_EXISTS" in s
        main = s[s.index("void __stdcall start(void)"):]
        assert main.index("chan_lock()") < main.index("lstrcpynA(cmdbuf")


def test_flush_reports_success():
    """wait_done() returns the STATUS byte (50h when idle), so a success test
    of '== 0' never fired: the first real flush on .243 logged nothing at all
    and the plan reported a failed flush that had in fact completed."""
    b = body("static void do_flush")
    assert 'wait_done("FLUSH CACHE") >= 0' in b
    assert "== 0" not in b
