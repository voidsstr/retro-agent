"""cmosw9x - the CMOS writer for .243's secondary-IDE-master type - can only
touch the three registers it exists for (2026-09-26).

.243 (Compaq Deskpro 2000, BIOS 04/25/97) auto-typed an 80 GB disk as type 68,
whose logical-heads byte is 00 (256 heads overflowed): the BIOS rejects every
read above head 0, ESDI_506's verify read fails and the whole secondary IDE
channel is torn down. Setting CMOS 1Bh to 00 hides the disk from the BIOS so
ESDI_506 claims it from IDENTIFY. The box has no reachable BIOS setup, so a
wrong CMOS write could only be undone by a person with a Compaq diskette -
hence the guards pinned here:
  - the single write path writes 71h only for 1Bh, 2Eh or 2Fh;
  - the checksum is the 16-bit sum of 10h..2Dh, high byte at 2Eh (the formula
    the ROM uses, and the one the box's own CMOS satisfies);
  - `none` is a compare-and-swap: 1Bh must be 44h and the checksum valid;
  - every write is re-read and compared (clock registers excluded);
  - the index port is left at 0Dh.
"""
import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "cmosw9x.c").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)


def body(name):
    start = CODE.index(name)
    depth, i = 0, CODE.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(CODE[i], 0)
        if depth == 0:
            return CODE[start:i + 1]
        i += 1


def test_only_one_function_writes_the_data_port():
    writers = [m.start() for m in re.finditer(r"outb\(0x71\s*,", CODE)]
    assert len(writers) == 1, writers
    wr = body("static int cmos_wr")
    assert "outb(0x71, v)" in wr
    guard = wr.index("if (idx != REG_TYPE && idx != REG_CSHI && idx != REG_CSLO) return -1;")
    assert guard < wr.index("outb(0x71, v)")


def test_the_three_registers_are_the_right_ones():
    assert re.search(r"#define REG_TYPE\s+0x1B\b", CODE)
    assert re.search(r"#define REG_CSHI\s+0x2E\b", CODE)
    assert re.search(r"#define REG_CSLO\s+0x2F\b", CODE)
    calls = re.findall(r"cmos_wr\((\w+),", CODE)
    assert set(calls) <= {"idx", "REG_TYPE", "REG_CSHI", "REG_CSLO"}, calls


def test_checksum_formula_matches_the_rom_and_the_box():
    s = body("static unsigned sum10_2d")
    assert "for (i = 0x10; i <= 0x2D; i++) s += b[i];" in s
    assert "s & 0xFFFF" in s
    assert "((unsigned)b[REG_CSHI] << 8) | b[REG_CSLO]" in CODE
    # .243's CMOS as read on 2026-09-26: the formula must call it valid, and
    # the planned change must produce 03F0.
    cmos = bytes.fromhex(
        "230043000600002609262602508004004000f000038002003c41004400f00000"
        "000000007e2b00000000022300000434003c2080001101000000000000000000")
    regs = bytearray(128)
    regs[:len(cmos)] = cmos
    total = sum(regs[0x10:0x2E]) & 0xFFFF
    assert regs[0x1B] == 0x44 and total == 0x0434 == (regs[0x2E] << 8 | regs[0x2F])
    regs[0x1B] = 0
    assert sum(regs[0x10:0x2E]) & 0xFFFF == 0x03F0


def test_none_is_a_compare_and_swap():
    main = body("void __stdcall start")
    branch = main[main.index('"none"'):main.index('"restore"')]
    assert "before[REG_TYPE] != TYPE_WAS || stored_cs(before) != sum10_2d(before)" in branch
    assert branch.index("REFUSED") < branch.index("set_type(TYPE_NONE)")
    assert re.search(r"#define TYPE_WAS\s+0x44\b", CODE) and re.search(r"#define TYPE_NONE\s+0x00\b", CODE)


def test_every_write_is_read_back_and_compared():
    st = body("static int set_type")
    assert st.index("read_all(after)") > st.index("cmos_wr(REG_CSLO")
    assert "for (i = 0x0D; i < 128; i++)" in st and "after[i] != want[i]" in st
    assert "stored_cs(after) != sum10_2d(after)" in st


def test_index_is_left_at_0d():
    assert "static void cmos_done(void) { outb(0x70, 0x0D); }" in CODE
    assert "cmos_done();" in body("static int set_type") and "cmos_done();" in body("static void read_all")
