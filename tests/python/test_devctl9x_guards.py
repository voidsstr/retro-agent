"""devctl9x (scripts/fleet/win9x/devctl9x.c) - disable/enable a Win9x devnode.

Written 2026-09-27 for .243: once its NEC USB card had drivers the box froze
within about two minutes of every boot, and a registry change made through
REGEDIT did not survive the freeze (Win98 flushes its registry lazily). So:
  - `persistoff` / `persiston` write ONLY HKLM\\Enum\\<id> ConfigFlags and
    RegFlushKey it before doing anything else, touching no devnode;
  - the live verbs go through Config Manager, loaded dynamically (a static
    cfgmgr32 import kills a Win9x exe at load);
  - ids can come from a file, because COMMAND.COM caps a line at ~127 chars.
"""
import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x" / "devctl9x.c").read_text()
CODE = re.sub(r"/\*.*?\*/", "", SRC, flags=re.S)


def test_persist_modes_write_configflags_and_flush_before_any_devnode_call():
    persist = CODE[CODE.index("if (mode >= 4) {"):CODE.index("cr = loc(&dn, id, 0);")]
    assert 'RegSetValueExA(k, "ConfigFlags", 0, REG_BINARY, flags, 4)' in persist
    assert persist.index("RegSetValueExA") < persist.index("RegFlushKey(k)")
    assert "RegFlushKey(HKEY_LOCAL_MACHINE)" in persist
    assert "RegQueryValueExA" in persist, "the value is read back and reported"
    assert "continue;" in persist, "no CM_* call in a persist mode"
    for cm in ("dis(", "ena(", "loc("):
        assert cm not in persist


def test_cfgmgr32_is_dynamic_and_only_required_for_live_verbs():
    assert 'LoadLibraryA("cfgmgr32.dll")' in CODE
    assert "#pragma comment" not in CODE and "cfgmgr32.h" not in CODE
    assert "if (mode < 4 && (!loc || !dis || !ena || !sta))" in CODE


def test_ids_can_come_from_a_file():
    assert "if (*c == '@')" in CODE and "ReadFile(f, ids, sizeof(ids) - 1, &n, NULL);" in CODE
    assert re.search(r"static char ids\[4096\];", CODE), "static, so no __chkstk under -nostdlib"
