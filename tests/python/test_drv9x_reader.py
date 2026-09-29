"""agent/src/drv9x.c must hand drvsafe a double-NUL-terminated id list.

drvsafe_ids_3dfx() walks NUL-separated strings up to an EMPTY one (the NT
REG_MULTI_SZ shape, and also how it accepts a Win9x comma list). drv9x reads
Win9x's REG_SZ HardwareID/CompatibleIDs into buffers REUSED for every device;
resetting only out[0] left the previous device's longer tail after the new
NUL. On .243 (2026-09-28) DRIVERS STATUS reported the VIA USB controller -
enumerated right after the Voodoo 2 - as "excluded_3dfx" from that tail.
tests/native/test_drvsafe.c shows the hazard itself."""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "agent" / "src" / "drv9x.c"


def test_drv9x_str_clears_the_whole_buffer_before_and_after_a_failed_read():
    s = SRC.read_text()
    m = re.search(r"static void drv9x_str\([^)]*\)\s*\{(.*?)\n\}", s, re.S)
    assert m, "drv9x_str not found"
    body = m.group(1)
    q = body.index("RegQueryValueExA")
    assert "memset(out, 0, cch)" in body[:q], "the buffer must be cleared BEFORE the read, not just out[0]"
    assert "cch - 2" in body, "leave room for the second NUL"


def test_every_id_list_goes_through_drv9x_str():
    s = SRC.read_text()
    assert 'drv9x_str(he, "HardwareID", hwids' in s
    assert 'drv9x_str(he, "CompatibleIDs", compat' in s
