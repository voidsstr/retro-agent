"""ide9x / idewrite9x refuse to touch the secondary IDE channel while Windows
owns it (2026-09-27).

On .243 the 80 GB disk came online natively (ESDI_506 claimed it on
MF\\CHILD0001, Problem 0). A check script then ran `ide9x identify` on that
same channel: raw task-file commands with the drive's interrupt masked, under
a live Windows driver. The driver was left waiting, the agent's next query of
D: blocked, and the agent died with nobody at the box.

Both tools now read HKEY_DYN_DATA\\Config Manager\\Enum first and refuse when
the channel devnode has no problem code, when any devnode hangs off
&CHILD0001&, or when the live tree cannot be read (fail closed).
"""
import re
from pathlib import Path

W9X = Path(__file__).resolve().parents[2] / "scripts" / "fleet" / "win9x"


def body(code, name):
    start = code.index(name)
    depth, i = 0, code.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[start:i + 1]
        i += 1


def _code(name):
    return re.sub(r"/\*.*?\*/", "", (W9X / name).read_text(), flags=re.S)


def test_both_tools_check_ownership_before_any_port_work():
    for name in ("ide9x.c", "idewrite9x.c"):
        code = _code(name)
        main = body(code, "void __stdcall start(void)")
        guard = main.index("channel_owned_by_windows(why, sizeof(why))")
        assert main.index("chan_lock()") < guard < main.index("lstrcpynA(cmdbuf"), name
        assert "ExitProcess(7)" in main[guard:guard + 300], name


def test_the_guard_keys_on_the_live_devnode_tree_and_fails_closed():
    for name in ("ide9x.c", "idewrite9x.c"):
        g = body(_code(name), "static int channel_owned_by_windows")
        assert "0x80000006UL" in g and '"Config Manager\\\\Enum"' in g, name     # HKEY_DYN_DATA
        assert '"MF\\\\CHILD0001\\\\"' in g and "problem == 0" in g, name
        assert '"&CHILD0001&"' in g, name
        first_return = g.index("return 1;")
        assert g.index("RegOpenKeyExA") < first_return, "an unreadable tree must refuse"
