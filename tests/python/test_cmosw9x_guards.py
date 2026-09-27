"""cmosw9x - the CMOS writer for .243 (Compaq Deskpro 2000) - can only touch
the registers it exists for, and puts everything back if anything else moves.

.243 has no reachable Computer Setup, so a wrong CMOS write can only be undone
by a person with a Compaq diskette. Two settings are written from Windows 98:

  - 2Dh bit 3, "POST Error Handling: skip F1 message" (2026-09-27): every boot
    halted on 301-Keyboard Error waiting for F1. F000:95F8 clears the wait flag
    (BDA 40:12 bit 0) only when the checksum is valid and 2Dh bit 3 is set.
  - 1Bh, the secondary master drive type (2026-09-26, the 80 GB disk).

Pinned here:
  - only 1Bh/2Dh/2Eh/2Fh can be written by a change; the repair path writes
    only snapshot values, and never the clock or the RTC's C/D registers;
  - the checksum is the 16-bit sum of 10h..2Dh, high byte at 2Eh (the ROM's
    formula), computed from the LIVE bytes, never a constant;
  - every change requires a valid checksum first, and is followed by a
    full-bank re-read that includes 0Ah/0Bh; any difference restores the
    snapshot;
  - index writes are paced with the ROM's own delay (in al,84h), and the index
    is left at 0Dh.
"""
import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "cmosw9x.c").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)

# .243's CMOS bank 0 as read on 2026-09-26/27 (the POST had re-typed 1Bh to 44h).
CMOS_243 = bytes.fromhex(
    "540049002200002609262602508000004000f000038002003c41004400f00000"
    "000000007e2b00000000022300000434003c2080001101000000000000000000"
)


def body(name):
    start = CODE.index(name)
    depth, i = 0, CODE.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(CODE[i], 0)
        if depth == 0:
            return CODE[start:i + 1]
        i += 1


def regs():
    r = bytearray(128)
    r[:len(CMOS_243)] = CMOS_243
    return r


def checksum(r):
    return sum(r[0x10:0x2E]) & 0xFFFF


def test_only_two_functions_write_the_data_port():
    writers = [m.start() for m in re.finditer(r"outb\(0x71\s*,", CODE)]
    assert len(writers) == 2, writers
    wr = body("static int cmos_wr")
    assert wr.index("if (!writable(idx)) return -1;") < wr.index("outb(0x71, v)")
    pb = body("static int cmos_put_back")
    assert "outb(0x71, before[idx])" in pb, "the repair path may write only snapshot values"
    assert pb.index("return -1;") < pb.index("outb(0x71")


def test_the_writable_registers_are_the_right_ones():
    assert re.search(r"#define REG_TYPE\s+0x1B\b", CODE)
    assert re.search(r"#define REG_FLAGS\s+0x2D\b", CODE)
    assert re.search(r"#define REG_CSHI\s+0x2E\b", CODE)
    assert re.search(r"#define REG_CSLO\s+0x2F\b", CODE)
    assert re.search(r"#define FLAG_SKIP_F1\s+0x08\b", CODE)
    wb = body("static int writable")
    assert set(re.findall(r"idx == (\w+)", wb)) == {"REG_TYPE", "REG_FLAGS", "REG_CSHI", "REG_CSLO"}


def test_repair_never_writes_the_clock_or_the_read_only_rtc_registers():
    pb = body("static int cmos_put_back")
    assert "idx < 0x0A" in pb and "idx == 0x0C" in pb and "idx == 0x0D" in pb
    vr = body("static int volatile_reg")
    assert "i <= 0x09" in vr and "i == 0x0C" in vr and "i == 0x0D" in vr
    assert "0x0A" not in vr and "0x0B" not in vr, "0Ah/0Bh must be compared, not ignored"


def test_checksum_formula_matches_the_rom_and_the_box():
    s = body("static unsigned sum10_2d")
    assert "for (i = 0x10; i <= 0x2D; i++) s += b[i];" in s and "s & 0xFFFF" in s
    assert "((unsigned)b[REG_CSHI] << 8) | b[REG_CSLO]" in CODE
    r = regs()
    assert checksum(r) == 0x0434 == (r[0x2E] << 8 | r[0x2F]), "the box's CMOS must read as valid"


def test_postskip_on_gives_the_reviewed_bytes():
    """2Dh 00->08 with the checksum recomputed: 0434h -> 043Ch (2Eh=04, 2Fh=3C)."""
    r = regs()
    r[0x2D] |= 0x08
    cs = checksum(r)
    assert (cs >> 8, cs & 0xFF) == (0x04, 0x3C)
    ch = body("static int change")
    assert "cs = sum10_2d(want);" in ch, "the checksum must come from the live image"
    assert not re.search(r"want\[REG_CS(HI|LO)\]\s*=\s*0x", ch), "no hard-coded checksum"


def test_every_change_needs_a_valid_checksum_first():
    main = body("void __stdcall start")
    refuse = main.index('else if (!cs_valid(before) && lstrcmpiA(c, "restore"))')
    for op in ('"postskip on"', '"postskip off"', '"none"'):
        assert refuse < main.index(op), "%s must be behind the checksum gate" % op


def test_postskip_only_flips_bit_3():
    main = body("void __stdcall start")
    on = main[main.index('"postskip on"'):main.index('"postskip off"')]
    off = main[main.index('"postskip off"'):main.index('"none"')]
    assert "before[REG_FLAGS] | FLAG_SKIP_F1" in on
    assert "before[REG_FLAGS] & ~FLAG_SKIP_F1" in off


def test_whole_bank_is_verified_and_restored_on_any_difference():
    ch = body("static int change")
    assert ch.index("read_all(after)") > ch.index("cmos_wr(REG_CSLO")
    assert "diff_count(after, want, 1)" in ch and "cs_valid(after)" in ch
    repair = ch[ch.index("REPAIR: putting"):]
    assert "cmos_put_back" in repair and "diff_count(after, before, 1) == 0" in repair
    assert "DO NOT REBOOT" in repair


def test_model_of_the_repair_restores_a_stray_write():
    """A data byte that lands in register B (a stray index between our two
    port writes) must be found and put back."""
    before = regs()
    after = bytearray(before)
    after[0x2D] |= 0x08
    after[0x2F] = 0x3C
    after[0x0B] = 0x3C                          # the stray write
    want = bytearray(before)
    want[0x2D] |= 0x08
    want[0x2E], want[0x2F] = 0x04, 0x3C
    volatile = set(range(0, 0x0A)) | {0x0C, 0x0D}
    diffs = [i for i in range(128) if i not in volatile and after[i] != want[i]]
    assert diffs == [0x0B]


def test_io_is_paced_and_the_index_is_left_at_0d():
    assert "static void io_delay(void) { (void)inb(0x84); }" in CODE
    wr = body("static int cmos_wr")
    assert wr.index("io_delay()") < wr.index("outb(0x71")
    assert "static void cmos_done(void) { outb(0x70, 0x0D); }" in CODE
    assert "cmos_done();" in body("static int change") and "cmos_done();" in body("static void read_all")
