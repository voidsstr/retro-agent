"""disk9x - the Win9x raw-disk reader - can only READ (2026-09-25).

Written to identify a second disk on .243 that the BIOS showed and Win98 gave
no drive letter ("ESDI BIOS read failure"). It talks to the BIOS through
VWIN32's INT 13h service on a box that needs a person to recover, so its
safety is pinned here rather than left to a code review:
  - the only INT 13h functions it can issue are 02h (read), 08h, 15h and 41h -
    no write/format/extended call, and NO AH=00h reset (on an AT-class BIOS a
    reset with DL>=80h can reset C:'s channel under Win98's own driver);
  - the DeviceIoControl code is VWIN32_DIOC_DOS_INT13 (4) - 3 would be
    DOS_INT26, an absolute disk WRITE;
  - output is a NEW file under C:\\RETRO_AGENT only (CREATE_NEW, bare name);
  - the read loop stops at the first failure and is time-bounded.
"""
import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "disk9x.c").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)      # comments may name forbidden functions


def test_only_read_and_query_functions_are_ever_loaded_into_eax():
    loads = re.findall(r"reg_EAX\s*=\s*(0x[0-9A-Fa-f]+)", CODE)
    assert loads, "no INT 13h calls found - has the file changed shape?"
    assert set(x.lower() for x in loads) <= {"0x0201", "0x0800", "0x1500", "0x4100"}, loads


def test_there_is_no_reset_and_no_write_function():
    assert "0x0000" not in CODE and not re.search(r"reg_EAX\s*=\s*0\b", CODE), "AH=00h reset is banned"
    for ah in ("0x03", "0x05", "0x0B", "0x0b", "0x43", "0x0C", "0x0c"):
        assert not re.search(r"reg_EAX\s*=\s*" + ah, CODE), ah


def test_the_vwin32_call_is_int13_not_int26():
    assert re.search(r"#define VWIN32_DIOC_DOS_INT13\s+4\b", SRC)
    calls = re.findall(r"DeviceIoControl\(vw,\s*(\w+)", CODE)
    assert calls and set(calls) == {"VWIN32_DIOC_DOS_INT13"}, calls


def test_only_bios_hard_disks_80_to_83():
    assert "drive < 0x80 || drive > 0x83" in CODE
    assert "for (d = 0x80; d <= 0x83; d++)" in CODE


def test_output_never_overwrites_and_stays_in_retro_agent():
    assert "CREATE_NEW" in CODE
    assert '#define OUT_DIR  "C:\\\\RETRO_AGENT\\\\"' in SRC
    body = CODE[CODE.index("static int bare_name"):CODE.index("static void do_read")]
    for ch in ("':'", "'\\\\'", "'/'"):
        assert ch in body


def test_reading_stops_at_the_first_failure_and_is_time_bounded():
    loop = CODE[CODE.index("for (i = 0; i < count; i++)"):]
    loop = loop[:loop.index("CloseHandle(of)")]
    assert "break;" in loop.split("FAILED")[1][:200], "must stop at the first failed sector"
    assert "too_slow()" in loop
    assert re.search(r"#define CALL_MAX_MS\s+3000", SRC) and re.search(r"#define RUN_MAX_MS\s+20000", SRC)


def test_a_sector_counts_only_if_data_actually_arrived():
    rs = CODE[CODE.index("static int read_sector"):CODE.index("static int bare_name")]
    assert "FillMemory(sector, sizeof(sector), SENTINEL)" in rs
    assert "no data transferred" in rs
